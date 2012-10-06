#include "builtin.h"
#include "util/color.h"
#include "util/evlist.h"
#include "util/machine.h"
#include "util/thread.h"
#include "util/parse-options.h"
#include "util/thread_map.h"
#include "event-parse.h"

#include <libaudit.h>
#include <stdlib.h>

static struct syscall_fmt {
	const char *name;
	const char *alias;
	bool	   errmsg;
	bool	   timeout;
} syscall_fmts[] = {
	{ .name	    = "arch_prctl", .errmsg = true, .alias = "prctl", },
	{ .name	    = "fstat",	    .errmsg = true, .alias = "newfstat", },
	{ .name	    = "fstatat",    .errmsg = true, .alias = "newfstatat", },
	{ .name	    = "futex",	    .errmsg = true, },
	{ .name	    = "poll",	    .errmsg = true, .timeout = true, },
	{ .name	    = "ppoll",	    .errmsg = true, .timeout = true, },
	{ .name	    = "read",	    .errmsg = true, },
	{ .name	    = "recvfrom",   .errmsg = true, },
	{ .name	    = "select",	    .errmsg = true, .timeout = true, },
	{ .name	    = "stat",	    .errmsg = true, .alias = "newstat", },
};

static int syscall_fmt__cmp(const void *name, const void *fmtp)
{
	const struct syscall_fmt *fmt = fmtp;
	return strcmp(name, fmt->name);
}

static struct syscall_fmt *syscall_fmt__find(const char *name)
{
	const int nmemb = ARRAY_SIZE(syscall_fmts);
	return bsearch(name, syscall_fmts, nmemb, sizeof(struct syscall_fmt), syscall_fmt__cmp);
}

struct syscall {
	struct event_format *tp_format;
	const char	    *name;
	struct syscall_fmt  *fmt;
};

struct thread_trace {
	u64		  entry_time;
	u64		  exit_time;
	bool		  entry_pending;
	char		  *entry_str;
};

static struct thread_trace *thread_trace__new(void)
{
	return zalloc(sizeof(struct thread_trace));
}

static struct thread_trace *thread__trace(struct thread *thread)
{
	if (thread == NULL)
		goto fail;

	if (thread->priv == NULL)
		thread->priv = thread_trace__new();

	if (thread->priv == NULL)
		goto fail;

	return thread->priv;
fail:
	color_fprintf(stdout, PERF_COLOR_RED,
		      "WARNING: not enough memory, dropping samples!\n");
	return NULL;
}

struct trace {
	int			audit_machine;
	struct {
		int		max;
		struct syscall  *table;
	} syscalls;
	struct perf_record_opts opts;
	struct machine		host;
	u64			base_time;
	bool			multiple_threads;
};

static size_t trace__fprintf_tstamp(struct trace *trace, u64 tstamp, FILE *fp)
{
	double ts = (double)(tstamp - trace->base_time) / NSEC_PER_MSEC;

	return fprintf(fp, "%10.3f: ", ts);
}

static bool done = false;

static void sig_handler(int sig __maybe_unused)
{
	done = true;
}

static size_t trace__fprintf_entry_head(struct trace *trace, struct thread *thread,
					u64 tstamp, FILE *fp)
{
	size_t printed = trace__fprintf_tstamp(trace, tstamp, fp);

	if (trace->multiple_threads)
		printed += fprintf(fp, "%d ", thread->pid);

	return printed;
}

static int trace__process_event(struct machine *machine, union perf_event *event)
{
	int ret = 0;

	switch (event->header.type) {
	case PERF_RECORD_LOST:
		color_fprintf(stdout, PERF_COLOR_RED,
			      "LOST %" PRIu64 " events!\n", event->lost.lost);
		ret = machine__process_lost_event(machine, event);
	default:
		ret = machine__process_event(machine, event);
		break;
	}

	return ret;
}

static int trace__tool_process(struct perf_tool *tool __maybe_unused,
			       union perf_event *event,
			       struct perf_sample *sample __maybe_unused,
			       struct machine *machine)
{
	return trace__process_event(machine, event);
}

static int trace__symbols_init(struct trace *trace, struct perf_evlist *evlist)
{
	int err = symbol__init();

	if (err)
		return err;

	machine__init(&trace->host, "", HOST_KERNEL_ID);
	machine__create_kernel_maps(&trace->host);

	if (perf_target__has_task(&trace->opts.target)) {
		err = perf_event__synthesize_thread_map(NULL, evlist->threads,
							trace__tool_process,
							&trace->host);
	} else {
		err = perf_event__synthesize_threads(NULL, trace__tool_process,
						     &trace->host);
	}

	if (err)
		symbol__exit();

	return err;
}

static int trace__read_syscall_info(struct trace *trace, int id)
{
	char tp_name[128];
	struct syscall *sc;
	const char *name = audit_syscall_to_name(id, trace->audit_machine);

	if (name == NULL)
		return -1;

	if (id > trace->syscalls.max) {
		struct syscall *nsyscalls = realloc(trace->syscalls.table, (id + 1) * sizeof(*sc));

		if (nsyscalls == NULL)
			return -1;

		if (trace->syscalls.max != -1) {
			memset(nsyscalls + trace->syscalls.max + 1, 0,
			       (id - trace->syscalls.max) * sizeof(*sc));
		} else {
			memset(nsyscalls, 0, (id + 1) * sizeof(*sc));
		}

		trace->syscalls.table = nsyscalls;
		trace->syscalls.max   = id;
	}

	sc = trace->syscalls.table + id;
	sc->name = name;
	sc->fmt  = syscall_fmt__find(sc->name);

	snprintf(tp_name, sizeof(tp_name), "sys_enter_%s", sc->name);
	sc->tp_format = event_format__new("syscalls", tp_name);

	if (sc->tp_format == NULL && sc->fmt && sc->fmt->alias) {
		snprintf(tp_name, sizeof(tp_name), "sys_enter_%s", sc->fmt->alias);
		sc->tp_format = event_format__new("syscalls", tp_name);
	}

	return sc->tp_format != NULL ? 0 : -1;
}

static size_t syscall__scnprintf_args(struct syscall *sc, char *bf, size_t size,
				      unsigned long *args)
{
	int i = 0;
	size_t printed = 0;

	if (sc->tp_format != NULL) {
		struct format_field *field;

		for (field = sc->tp_format->format.fields->next; field; field = field->next) {
			printed += scnprintf(bf + printed, size - printed,
					     "%s%s: %ld", printed ? ", " : "",
					     field->name, args[i++]);
		}
	} else {
		while (i < 6) {
			printed += scnprintf(bf + printed, size - printed,
					     "%sarg%d: %ld",
					     printed ? ", " : "", i, args[i]);
			++i;
		}
	}

	return printed;
}

typedef int (*tracepoint_handler)(struct trace *trace, struct perf_evsel *evsel,
				  struct perf_sample *sample);

static struct syscall *trace__syscall_info(struct trace *trace,
					   struct perf_evsel *evsel,
					   struct perf_sample *sample)
{
	int id = perf_evsel__intval(evsel, sample, "id");

	if (id < 0) {
		printf("Invalid syscall %d id, skipping...\n", id);
		return NULL;
	}

	if ((id > trace->syscalls.max || trace->syscalls.table[id].name == NULL) &&
	    trace__read_syscall_info(trace, id))
		goto out_cant_read;

	if ((id > trace->syscalls.max || trace->syscalls.table[id].name == NULL))
		goto out_cant_read;

	return &trace->syscalls.table[id];

out_cant_read:
	printf("Problems reading syscall %d information\n", id);
	return NULL;
}

static int trace__sys_enter(struct trace *trace, struct perf_evsel *evsel,
			    struct perf_sample *sample)
{
	char *msg;
	void *args;
	size_t printed = 0;
	struct thread *thread = machine__findnew_thread(&trace->host, sample->tid);
	struct syscall *sc = trace__syscall_info(trace, evsel, sample);
	struct thread_trace *ttrace = thread__trace(thread);

	if (ttrace == NULL || sc == NULL)
		return -1;

	args = perf_evsel__rawptr(evsel, sample, "args");
	if (args == NULL) {
		printf("Problems reading syscall arguments\n");
		return -1;
	}

	ttrace = thread->priv;

	if (ttrace->entry_str == NULL) {
		ttrace->entry_str = malloc(1024);
		if (!ttrace->entry_str)
			return -1;
	}

	ttrace->entry_time = sample->time;
	msg = ttrace->entry_str;
	printed += scnprintf(msg + printed, 1024 - printed, "%s(", sc->name);

	printed += syscall__scnprintf_args(sc, msg + printed, 1024 - printed,  args);

	if (!strcmp(sc->name, "exit_group") || !strcmp(sc->name, "exit")) {
		trace__fprintf_entry_head(trace, thread, sample->time, stdout);
		printf("%-70s\n", ttrace->entry_str);
	} else
		ttrace->entry_pending = true;

	return 0;
}

static int trace__sys_exit(struct trace *trace, struct perf_evsel *evsel,
			   struct perf_sample *sample)
{
	int ret;
	struct thread *thread = machine__findnew_thread(&trace->host, sample->tid);
	struct thread_trace *ttrace = thread__trace(thread);
	struct syscall *sc = trace__syscall_info(trace, evsel, sample);

	if (ttrace == NULL || sc == NULL)
		return -1;

	ret = perf_evsel__intval(evsel, sample, "ret");

	ttrace = thread->priv;

	ttrace->exit_time = sample->time;

	trace__fprintf_entry_head(trace, thread, sample->time, stdout);

	if (ttrace->entry_pending) {
		printf("%-70s", ttrace->entry_str);
	} else {
		printf(" ... [");
		color_fprintf(stdout, PERF_COLOR_YELLOW, "continued");
		printf("]: %s()", sc->name);
	}

	if (ret < 0 && sc->fmt && sc->fmt->errmsg) {
		char bf[256];
		const char *emsg = strerror_r(-ret, bf, sizeof(bf)),
			   *e = audit_errno_to_name(-ret);

		printf(") = -1 %s %s", e, emsg);
	} else if (ret == 0 && sc->fmt && sc->fmt->timeout)
		printf(") = 0 Timeout");
	else
		printf(") = %d", ret);

	putchar('\n');

	ttrace->entry_pending = false;

	return 0;
}

static int trace__run(struct trace *trace, int argc, const char **argv)
{
	struct perf_evlist *evlist = perf_evlist__new(NULL, NULL);
	struct perf_evsel *evsel;
	int err = -1, i, nr_events = 0, before;
	const bool forks = argc > 0;

	if (evlist == NULL) {
		printf("Not enough memory to run!\n");
		goto out;
	}

	if (perf_evlist__add_newtp(evlist, "raw_syscalls", "sys_enter", trace__sys_enter) ||
	    perf_evlist__add_newtp(evlist, "raw_syscalls", "sys_exit", trace__sys_exit)) {
		printf("Couldn't read the raw_syscalls tracepoints information!\n");
		goto out_delete_evlist;
	}

	err = perf_evlist__create_maps(evlist, &trace->opts.target);
	if (err < 0) {
		printf("Problems parsing the target to trace, check your options!\n");
		goto out_delete_evlist;
	}

	err = trace__symbols_init(trace, evlist);
	if (err < 0) {
		printf("Problems initializing symbol libraries!\n");
		goto out_delete_evlist;
	}

	perf_evlist__config_attrs(evlist, &trace->opts);

	signal(SIGCHLD, sig_handler);
	signal(SIGINT, sig_handler);

	if (forks) {
		err = perf_evlist__prepare_workload(evlist, &trace->opts, argv);
		if (err < 0) {
			printf("Couldn't run the workload!\n");
			goto out_delete_evlist;
		}
	}

	err = perf_evlist__open(evlist);
	if (err < 0) {
		printf("Couldn't create the events: %s\n", strerror(errno));
		goto out_delete_evlist;
	}

	err = perf_evlist__mmap(evlist, UINT_MAX, false);
	if (err < 0) {
		printf("Couldn't mmap the events: %s\n", strerror(errno));
		goto out_delete_evlist;
	}

	perf_evlist__enable(evlist);

	if (forks)
		perf_evlist__start_workload(evlist);

	trace->multiple_threads = evlist->threads->map[0] == -1 || evlist->threads->nr > 1;
again:
	before = nr_events;

	for (i = 0; i < evlist->nr_mmaps; i++) {
		union perf_event *event;

		while ((event = perf_evlist__mmap_read(evlist, i)) != NULL) {
			const u32 type = event->header.type;
			tracepoint_handler handler;
			struct perf_sample sample;

			++nr_events;

			err = perf_evlist__parse_sample(evlist, event, &sample);
			if (err) {
				printf("Can't parse sample, err = %d, skipping...\n", err);
				continue;
			}

			if (trace->base_time == 0)
				trace->base_time = sample.time;

			if (type != PERF_RECORD_SAMPLE) {
				trace__process_event(&trace->host, event);
				continue;
			}

			evsel = perf_evlist__id2evsel(evlist, sample.id);
			if (evsel == NULL) {
				printf("Unknown tp ID %" PRIu64 ", skipping...\n", sample.id);
				continue;
			}

			if (sample.raw_data == NULL) {
				printf("%s sample with no payload for tid: %d, cpu %d, raw_size=%d, skipping...\n",
				       perf_evsel__name(evsel), sample.tid,
				       sample.cpu, sample.raw_size);
				continue;
			}

			if (sample.raw_data == NULL) {
				printf("%s sample with no payload for tid: %d, cpu %d, raw_size=%d, skipping...\n",
				       perf_evsel__name(evsel), sample.tid,
				       sample.cpu, sample.raw_size);
				continue;
			}

			handler = evsel->handler.func;
			handler(trace, evsel, &sample);
		}
	}

	if (nr_events == before) {
		if (done)
			goto out_delete_evlist;

		poll(evlist->pollfd, evlist->nr_fds, -1);
	}

	if (done)
		perf_evlist__disable(evlist);

	goto again;

out_delete_evlist:
	perf_evlist__delete(evlist);
out:
	return err;
}

int cmd_trace(int argc, const char **argv, const char *prefix __maybe_unused)
{
	const char * const trace_usage[] = {
		"perf trace [<options>] [<command>]",
		"perf trace [<options>] -- <command> [<options>]",
		NULL
	};
	struct trace trace = {
		.audit_machine = audit_detect_machine(),
		.syscalls = {
			. max = -1,
		},
		.opts = {
			.target = {
				.uid	   = UINT_MAX,
				.uses_mmap = true,
			},
			.user_freq     = UINT_MAX,
			.user_interval = ULLONG_MAX,
			.no_delay      = true,
			.mmap_pages    = 1024,
		},
	};
	const struct option trace_options[] = {
	OPT_STRING('p', "pid", &trace.opts.target.pid, "pid",
		    "trace events on existing process id"),
	OPT_STRING(0, "tid", &trace.opts.target.tid, "tid",
		    "trace events on existing thread id"),
	OPT_BOOLEAN(0, "all-cpus", &trace.opts.target.system_wide,
		    "system-wide collection from all CPUs"),
	OPT_STRING(0, "cpu", &trace.opts.target.cpu_list, "cpu",
		    "list of cpus to monitor"),
	OPT_BOOLEAN(0, "no-inherit", &trace.opts.no_inherit,
		    "child tasks do not inherit counters"),
	OPT_UINTEGER(0, "mmap-pages", &trace.opts.mmap_pages,
		     "number of mmap data pages"),
	OPT_STRING(0, "uid", &trace.opts.target.uid_str, "user",
		   "user to profile"),
	OPT_END()
	};
	int err;
	char bf[BUFSIZ];

	argc = parse_options(argc, argv, trace_options, trace_usage, 0);

	err = perf_target__validate(&trace.opts.target);
	if (err) {
		perf_target__strerror(&trace.opts.target, err, bf, sizeof(bf));
		printf("%s", bf);
		return err;
	}

	err = perf_target__parse_uid(&trace.opts.target);
	if (err) {
		perf_target__strerror(&trace.opts.target, err, bf, sizeof(bf));
		printf("%s", bf);
		return err;
	}

	if (!argc && perf_target__none(&trace.opts.target))
		trace.opts.target.system_wide = true;

	return trace__run(&trace, argc, argv);
}
