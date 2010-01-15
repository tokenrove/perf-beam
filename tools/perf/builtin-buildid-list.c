/*
 * builtin-buildid-list.c
 *
 * Builtin buildid-list command: list buildids in perf.data
 *
 * Copyright (C) 2009, Red Hat Inc.
 * Copyright (C) 2009, Arnaldo Carvalho de Melo <acme@redhat.com>
 */
#include "builtin.h"
#include "perf.h"
#include "util/cache.h"
#include "util/debug.h"
#include "util/parse-options.h"
#include "util/session.h"
#include "util/symbol.h"

static char const *input_name = "perf.data";
static int force;
static bool with_hits;

static const char * const buildid_list_usage[] = {
	"perf buildid-list [<options>]",
	NULL
};

static const struct option options[] = {
	OPT_BOOLEAN('H', "with-hits", &with_hits, "Show only DSOs with hits"),
	OPT_STRING('i', "input", &input_name, "file",
		    "input file name"),
	OPT_BOOLEAN('f', "force", &force, "don't complain, do it"),
	OPT_BOOLEAN('v', "verbose", &verbose,
		    "be more verbose"),
	OPT_END()
};

static int build_id_list__process_event(event_t *event,
					struct perf_session *session)
{
	struct addr_location al;
	u8 cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread = perf_session__findnew(session, event->ip.pid);

	if (thread == NULL) {
		pr_err("problem processing %d event, skipping it.\n",
			event->header.type);
		return -1;
	}

	thread__find_addr_map(thread, session, cpumode, MAP__FUNCTION,
			      event->ip.ip, &al);

	if (al.map != NULL)
		al.map->dso->hit = 1;

	return 0;
}

static struct perf_event_ops build_id_list__event_ops = {
	.sample	= build_id_list__process_event,
	.mmap	= event__process_mmap,
	.fork	= event__process_task,
};

static int __cmd_buildid_list(void)
{
	int err = -1;
	struct perf_session *session;

	session = perf_session__new(input_name, O_RDONLY, force);
	if (session == NULL)
		return -1;

	if (with_hits)
		perf_session__process_events(session, &build_id_list__event_ops);

	dsos__fprintf_buildid(stdout, with_hits);

	perf_session__delete(session);
	return err;
}

int cmd_buildid_list(int argc, const char **argv, const char *prefix __used)
{
	argc = parse_options(argc, argv, options, buildid_list_usage, 0);
	setup_pager();
	return __cmd_buildid_list();
}
