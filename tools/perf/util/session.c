#define _FILE_OFFSET_BITS 64

#include <linux/kernel.h>

#include <byteswap.h>
#include <unistd.h>
#include <sys/types.h>

#include "session.h"
#include "sort.h"
#include "util.h"

static int perf_session__open(struct perf_session *self, bool force)
{
	struct stat input_stat;

	if (!strcmp(self->filename, "-")) {
		self->fd_pipe = true;
		self->fd = STDIN_FILENO;

		if (perf_header__read(self, self->fd) < 0)
			pr_err("incompatible file format");

		return 0;
	}

	self->fd = open(self->filename, O_RDONLY);
	if (self->fd < 0) {
		pr_err("failed to open file: %s", self->filename);
		if (!strcmp(self->filename, "perf.data"))
			pr_err("  (try 'perf record' first)");
		pr_err("\n");
		return -errno;
	}

	if (fstat(self->fd, &input_stat) < 0)
		goto out_close;

	if (!force && input_stat.st_uid && (input_stat.st_uid != geteuid())) {
		pr_err("file %s not owned by current user or root\n",
		       self->filename);
		goto out_close;
	}

	if (!input_stat.st_size) {
		pr_info("zero-sized file (%s), nothing to do!\n",
			self->filename);
		goto out_close;
	}

	if (perf_header__read(self, self->fd) < 0) {
		pr_err("incompatible file format");
		goto out_close;
	}

	self->size = input_stat.st_size;
	return 0;

out_close:
	close(self->fd);
	self->fd = -1;
	return -1;
}

void perf_session__update_sample_type(struct perf_session *self)
{
	self->sample_type = perf_header__sample_type(&self->header);
}

int perf_session__create_kernel_maps(struct perf_session *self)
{
	struct rb_root *machines = &self->machines;
	int ret = machines__create_kernel_maps(machines, HOST_KERNEL_ID);

	if (ret >= 0)
		ret = machines__create_guest_kernel_maps(machines);
	return ret;
}

struct perf_session *perf_session__new(const char *filename, int mode, bool force)
{
	size_t len = filename ? strlen(filename) + 1 : 0;
	struct perf_session *self = zalloc(sizeof(*self) + len);

	if (self == NULL)
		goto out;

	if (perf_header__init(&self->header) < 0)
		goto out_free;

	memcpy(self->filename, filename, len);
	self->threads = RB_ROOT;
	self->stats_by_id = RB_ROOT;
	self->last_match = NULL;
	self->mmap_window = 32;
	self->cwd = NULL;
	self->cwdlen = 0;
	self->unknown_events = 0;
	self->machines = RB_ROOT;
	self->ordered_samples.flush_limit = ULLONG_MAX;
	INIT_LIST_HEAD(&self->ordered_samples.samples_head);

	if (mode == O_RDONLY) {
		if (perf_session__open(self, force) < 0)
			goto out_delete;
	} else if (mode == O_WRONLY) {
		/*
		 * In O_RDONLY mode this will be performed when reading the
		 * kernel MMAP event, in event__process_mmap().
		 */
		if (perf_session__create_kernel_maps(self) < 0)
			goto out_delete;
	}

	perf_session__update_sample_type(self);
out:
	return self;
out_free:
	free(self);
	return NULL;
out_delete:
	perf_session__delete(self);
	return NULL;
}

void perf_session__delete(struct perf_session *self)
{
	perf_header__exit(&self->header);
	close(self->fd);
	free(self->cwd);
	free(self);
}

static bool symbol__match_parent_regex(struct symbol *sym)
{
	if (sym->name && !regexec(&parent_regex, sym->name, 0, NULL, 0))
		return 1;

	return 0;
}

struct map_symbol *perf_session__resolve_callchain(struct perf_session *self,
						   struct thread *thread,
						   struct ip_callchain *chain,
						   struct symbol **parent)
{
	u8 cpumode = PERF_RECORD_MISC_USER;
	unsigned int i;
	struct map_symbol *syms = calloc(chain->nr, sizeof(*syms));

	if (!syms)
		return NULL;

	for (i = 0; i < chain->nr; i++) {
		u64 ip = chain->ips[i];
		struct addr_location al;

		if (ip >= PERF_CONTEXT_MAX) {
			switch (ip) {
			case PERF_CONTEXT_HV:
				cpumode = PERF_RECORD_MISC_HYPERVISOR;	break;
			case PERF_CONTEXT_KERNEL:
				cpumode = PERF_RECORD_MISC_KERNEL;	break;
			case PERF_CONTEXT_USER:
				cpumode = PERF_RECORD_MISC_USER;	break;
			default:
				break;
			}
			continue;
		}

		al.filtered = false;
		thread__find_addr_location(thread, self, cpumode,
				MAP__FUNCTION, thread->pid, ip, &al, NULL);
		if (al.sym != NULL) {
			if (sort__has_parent && !*parent &&
			    symbol__match_parent_regex(al.sym))
				*parent = al.sym;
			if (!symbol_conf.use_callchain)
				break;
			syms[i].map = al.map;
			syms[i].sym = al.sym;
		}
	}

	return syms;
}

static int process_event_stub(event_t *event __used,
			      struct perf_session *session __used)
{
	dump_printf(": unhandled!\n");
	return 0;
}

static void perf_event_ops__fill_defaults(struct perf_event_ops *handler)
{
	if (handler->sample == NULL)
		handler->sample = process_event_stub;
	if (handler->mmap == NULL)
		handler->mmap = process_event_stub;
	if (handler->comm == NULL)
		handler->comm = process_event_stub;
	if (handler->fork == NULL)
		handler->fork = process_event_stub;
	if (handler->exit == NULL)
		handler->exit = process_event_stub;
	if (handler->lost == NULL)
		handler->lost = process_event_stub;
	if (handler->read == NULL)
		handler->read = process_event_stub;
	if (handler->throttle == NULL)
		handler->throttle = process_event_stub;
	if (handler->unthrottle == NULL)
		handler->unthrottle = process_event_stub;
	if (handler->attr == NULL)
		handler->attr = process_event_stub;
	if (handler->event_type == NULL)
		handler->event_type = process_event_stub;
	if (handler->tracing_data == NULL)
		handler->tracing_data = process_event_stub;
	if (handler->build_id == NULL)
		handler->build_id = process_event_stub;
}

static const char *event__name[] = {
	[0]			 = "TOTAL",
	[PERF_RECORD_MMAP]	 = "MMAP",
	[PERF_RECORD_LOST]	 = "LOST",
	[PERF_RECORD_COMM]	 = "COMM",
	[PERF_RECORD_EXIT]	 = "EXIT",
	[PERF_RECORD_THROTTLE]	 = "THROTTLE",
	[PERF_RECORD_UNTHROTTLE] = "UNTHROTTLE",
	[PERF_RECORD_FORK]	 = "FORK",
	[PERF_RECORD_READ]	 = "READ",
	[PERF_RECORD_SAMPLE]	 = "SAMPLE",
	[PERF_RECORD_HEADER_ATTR]	 = "ATTR",
	[PERF_RECORD_HEADER_EVENT_TYPE]	 = "EVENT_TYPE",
	[PERF_RECORD_HEADER_TRACING_DATA]	 = "TRACING_DATA",
	[PERF_RECORD_HEADER_BUILD_ID]	 = "BUILD_ID",
};

unsigned long event__total[PERF_RECORD_HEADER_MAX];

void event__print_totals(void)
{
	int i;
	for (i = 0; i < PERF_RECORD_HEADER_MAX; ++i) {
		if (!event__name[i])
			continue;
		pr_info("%10s events: %10ld\n",
			event__name[i], event__total[i]);
	}
}

void mem_bswap_64(void *src, int byte_size)
{
	u64 *m = src;

	while (byte_size > 0) {
		*m = bswap_64(*m);
		byte_size -= sizeof(u64);
		++m;
	}
}

static void event__all64_swap(event_t *self)
{
	struct perf_event_header *hdr = &self->header;
	mem_bswap_64(hdr + 1, self->header.size - sizeof(*hdr));
}

static void event__comm_swap(event_t *self)
{
	self->comm.pid = bswap_32(self->comm.pid);
	self->comm.tid = bswap_32(self->comm.tid);
}

static void event__mmap_swap(event_t *self)
{
	self->mmap.pid	 = bswap_32(self->mmap.pid);
	self->mmap.tid	 = bswap_32(self->mmap.tid);
	self->mmap.start = bswap_64(self->mmap.start);
	self->mmap.len	 = bswap_64(self->mmap.len);
	self->mmap.pgoff = bswap_64(self->mmap.pgoff);
}

static void event__task_swap(event_t *self)
{
	self->fork.pid	= bswap_32(self->fork.pid);
	self->fork.tid	= bswap_32(self->fork.tid);
	self->fork.ppid	= bswap_32(self->fork.ppid);
	self->fork.ptid	= bswap_32(self->fork.ptid);
	self->fork.time	= bswap_64(self->fork.time);
}

static void event__read_swap(event_t *self)
{
	self->read.pid		= bswap_32(self->read.pid);
	self->read.tid		= bswap_32(self->read.tid);
	self->read.value	= bswap_64(self->read.value);
	self->read.time_enabled	= bswap_64(self->read.time_enabled);
	self->read.time_running	= bswap_64(self->read.time_running);
	self->read.id		= bswap_64(self->read.id);
}

static void event__attr_swap(event_t *self)
{
	size_t size;

	self->attr.attr.type		= bswap_32(self->attr.attr.type);
	self->attr.attr.size		= bswap_32(self->attr.attr.size);
	self->attr.attr.config		= bswap_64(self->attr.attr.config);
	self->attr.attr.sample_period	= bswap_64(self->attr.attr.sample_period);
	self->attr.attr.sample_type	= bswap_64(self->attr.attr.sample_type);
	self->attr.attr.read_format	= bswap_64(self->attr.attr.read_format);
	self->attr.attr.wakeup_events	= bswap_32(self->attr.attr.wakeup_events);
	self->attr.attr.bp_type		= bswap_32(self->attr.attr.bp_type);
	self->attr.attr.bp_addr		= bswap_64(self->attr.attr.bp_addr);
	self->attr.attr.bp_len		= bswap_64(self->attr.attr.bp_len);

	size = self->header.size;
	size -= (void *)&self->attr.id - (void *)self;
	mem_bswap_64(self->attr.id, size);
}

static void event__event_type_swap(event_t *self)
{
	self->event_type.event_type.event_id =
		bswap_64(self->event_type.event_type.event_id);
}

static void event__tracing_data_swap(event_t *self)
{
	self->tracing_data.size = bswap_32(self->tracing_data.size);
}

typedef void (*event__swap_op)(event_t *self);

static event__swap_op event__swap_ops[] = {
	[PERF_RECORD_MMAP]   = event__mmap_swap,
	[PERF_RECORD_COMM]   = event__comm_swap,
	[PERF_RECORD_FORK]   = event__task_swap,
	[PERF_RECORD_EXIT]   = event__task_swap,
	[PERF_RECORD_LOST]   = event__all64_swap,
	[PERF_RECORD_READ]   = event__read_swap,
	[PERF_RECORD_SAMPLE] = event__all64_swap,
	[PERF_RECORD_HEADER_ATTR]   = event__attr_swap,
	[PERF_RECORD_HEADER_EVENT_TYPE]   = event__event_type_swap,
	[PERF_RECORD_HEADER_TRACING_DATA]   = event__tracing_data_swap,
	[PERF_RECORD_HEADER_BUILD_ID]   = NULL,
	[PERF_RECORD_HEADER_MAX]    = NULL,
};

struct sample_queue {
	u64			timestamp;
	struct sample_event	*event;
	struct list_head	list;
};

#define FLUSH_PERIOD	(2 * NSEC_PER_SEC)

static void flush_sample_queue(struct perf_session *s,
			       struct perf_event_ops *ops)
{
	struct list_head *head = &s->ordered_samples.samples_head;
	u64 limit = s->ordered_samples.flush_limit;
	struct sample_queue *tmp, *iter;

	if (!ops->ordered_samples)
		return;

	list_for_each_entry_safe(iter, tmp, head, list) {
		if (iter->timestamp > limit)
			return;

		if (iter == s->ordered_samples.last_inserted)
			s->ordered_samples.last_inserted = NULL;

		ops->sample((event_t *)iter->event, s);

		s->ordered_samples.last_flush = iter->timestamp;
		list_del(&iter->list);
		free(iter->event);
		free(iter);
	}
}

static void __queue_sample_end(struct sample_queue *new, struct list_head *head)
{
	struct sample_queue *iter;

	list_for_each_entry_reverse(iter, head, list) {
		if (iter->timestamp < new->timestamp) {
			list_add(&new->list, &iter->list);
			return;
		}
	}

	list_add(&new->list, head);
}

static void __queue_sample_before(struct sample_queue *new,
				  struct sample_queue *iter,
				  struct list_head *head)
{
	list_for_each_entry_continue_reverse(iter, head, list) {
		if (iter->timestamp < new->timestamp) {
			list_add(&new->list, &iter->list);
			return;
		}
	}

	list_add(&new->list, head);
}

static void __queue_sample_after(struct sample_queue *new,
				 struct sample_queue *iter,
				 struct list_head *head)
{
	list_for_each_entry_continue(iter, head, list) {
		if (iter->timestamp > new->timestamp) {
			list_add_tail(&new->list, &iter->list);
			return;
		}
	}
	list_add_tail(&new->list, head);
}

/* The queue is ordered by time */
static void __queue_sample_event(struct sample_queue *new,
				 struct perf_session *s)
{
	struct sample_queue *last_inserted = s->ordered_samples.last_inserted;
	struct list_head *head = &s->ordered_samples.samples_head;


	if (!last_inserted) {
		__queue_sample_end(new, head);
		return;
	}

	/*
	 * Most of the time the current event has a timestamp
	 * very close to the last event inserted, unless we just switched
	 * to another event buffer. Having a sorting based on a list and
	 * on the last inserted event that is close to the current one is
	 * probably more efficient than an rbtree based sorting.
	 */
	if (last_inserted->timestamp >= new->timestamp)
		__queue_sample_before(new, last_inserted, head);
	else
		__queue_sample_after(new, last_inserted, head);
}

static int queue_sample_event(event_t *event, struct sample_data *data,
			      struct perf_session *s,
			      struct perf_event_ops *ops)
{
	u64 timestamp = data->time;
	struct sample_queue *new;
	u64 flush_limit;


	if (s->ordered_samples.flush_limit == ULLONG_MAX)
		s->ordered_samples.flush_limit = timestamp + FLUSH_PERIOD;

	if (timestamp < s->ordered_samples.last_flush) {
		printf("Warning: Timestamp below last timeslice flush\n");
		return -EINVAL;
	}

	new = malloc(sizeof(*new));
	if (!new)
		return -ENOMEM;

	new->timestamp = timestamp;

	new->event = malloc(event->header.size);
	if (!new->event) {
		free(new);
		return -ENOMEM;
	}

	memcpy(new->event, event, event->header.size);

	__queue_sample_event(new, s);
	s->ordered_samples.last_inserted = new;

	/*
	 * We want to have a slice of events covering 2 * FLUSH_PERIOD
	 * If FLUSH_PERIOD is big enough, it ensures every events that occured
	 * in the first half of the timeslice have all been buffered and there
	 * are none remaining (we need that because of the weakly ordered
	 * event recording we have). Then once we reach the 2 * FLUSH_PERIOD
	 * timeslice, we flush the first half to be gentle with the memory
	 * (the second half can still get new events in the middle, so wait
	 * another period to flush it)
	 */
	flush_limit = s->ordered_samples.flush_limit;

	if (new->timestamp > flush_limit &&
		new->timestamp - flush_limit > FLUSH_PERIOD) {
		s->ordered_samples.flush_limit += FLUSH_PERIOD;
		flush_sample_queue(s, ops);
	}

	return 0;
}

static int perf_session__process_sample(event_t *event, struct perf_session *s,
					struct perf_event_ops *ops)
{
	struct sample_data data;

	if (!ops->ordered_samples)
		return ops->sample(event, s);

	bzero(&data, sizeof(struct sample_data));
	event__parse_sample(event, s->sample_type, &data);

	queue_sample_event(event, &data, s, ops);

	return 0;
}

static int perf_session__process_event(struct perf_session *self,
				       event_t *event,
				       struct perf_event_ops *ops,
				       u64 offset, u64 head)
{
	trace_event(event);

	if (event->header.type < PERF_RECORD_HEADER_MAX) {
		dump_printf("%#Lx [%#x]: PERF_RECORD_%s",
			    offset + head, event->header.size,
			    event__name[event->header.type]);
		++event__total[0];
		++event__total[event->header.type];
	}

	if (self->header.needs_swap && event__swap_ops[event->header.type])
		event__swap_ops[event->header.type](event);

	switch (event->header.type) {
	case PERF_RECORD_SAMPLE:
		return perf_session__process_sample(event, self, ops);
	case PERF_RECORD_MMAP:
		return ops->mmap(event, self);
	case PERF_RECORD_COMM:
		return ops->comm(event, self);
	case PERF_RECORD_FORK:
		return ops->fork(event, self);
	case PERF_RECORD_EXIT:
		return ops->exit(event, self);
	case PERF_RECORD_LOST:
		return ops->lost(event, self);
	case PERF_RECORD_READ:
		return ops->read(event, self);
	case PERF_RECORD_THROTTLE:
		return ops->throttle(event, self);
	case PERF_RECORD_UNTHROTTLE:
		return ops->unthrottle(event, self);
	case PERF_RECORD_HEADER_ATTR:
		return ops->attr(event, self);
	case PERF_RECORD_HEADER_EVENT_TYPE:
		return ops->event_type(event, self);
	case PERF_RECORD_HEADER_TRACING_DATA:
		/* setup for reading amidst mmap */
		lseek(self->fd, offset + head, SEEK_SET);
		return ops->tracing_data(event, self);
	case PERF_RECORD_HEADER_BUILD_ID:
		return ops->build_id(event, self);
	default:
		self->unknown_events++;
		return -1;
	}
}

void perf_event_header__bswap(struct perf_event_header *self)
{
	self->type = bswap_32(self->type);
	self->misc = bswap_16(self->misc);
	self->size = bswap_16(self->size);
}

static struct thread *perf_session__register_idle_thread(struct perf_session *self)
{
	struct thread *thread = perf_session__findnew(self, 0);

	if (thread == NULL || thread__set_comm(thread, "swapper")) {
		pr_err("problem inserting idle task.\n");
		thread = NULL;
	}

	return thread;
}

int do_read(int fd, void *buf, size_t size)
{
	void *buf_start = buf;

	while (size) {
		int ret = read(fd, buf, size);

		if (ret <= 0)
			return ret;

		size -= ret;
		buf += ret;
	}

	return buf - buf_start;
}

#define session_done()	(*(volatile int *)(&session_done))
volatile int session_done;

static int __perf_session__process_pipe_events(struct perf_session *self,
					       struct perf_event_ops *ops)
{
	event_t event;
	uint32_t size;
	int skip = 0;
	u64 head;
	int err;
	void *p;

	perf_event_ops__fill_defaults(ops);

	head = 0;
more:
	err = do_read(self->fd, &event, sizeof(struct perf_event_header));
	if (err <= 0) {
		if (err == 0)
			goto done;

		pr_err("failed to read event header\n");
		goto out_err;
	}

	if (self->header.needs_swap)
		perf_event_header__bswap(&event.header);

	size = event.header.size;
	if (size == 0)
		size = 8;

	p = &event;
	p += sizeof(struct perf_event_header);

	err = do_read(self->fd, p, size - sizeof(struct perf_event_header));
	if (err <= 0) {
		if (err == 0) {
			pr_err("unexpected end of event stream\n");
			goto done;
		}

		pr_err("failed to read event data\n");
		goto out_err;
	}

	if (size == 0 ||
	    (skip = perf_session__process_event(self, &event, ops,
						0, head)) < 0) {
		dump_printf("%#Lx [%#x]: skipping unknown header type: %d\n",
			    head, event.header.size, event.header.type);
		/*
		 * assume we lost track of the stream, check alignment, and
		 * increment a single u64 in the hope to catch on again 'soon'.
		 */
		if (unlikely(head & 7))
			head &= ~7ULL;

		size = 8;
	}

	head += size;

	dump_printf("\n%#Lx [%#x]: event: %d\n",
		    head, event.header.size, event.header.type);

	if (skip > 0)
		head += skip;

	if (!session_done())
		goto more;
done:
	err = 0;
out_err:
	return err;
}

int __perf_session__process_events(struct perf_session *self,
				   u64 data_offset, u64 data_size,
				   u64 file_size, struct perf_event_ops *ops)
{
	int err, mmap_prot, mmap_flags;
	u64 head, shift;
	u64 offset = 0;
	size_t	page_size;
	event_t *event;
	uint32_t size;
	char *buf;
	struct ui_progress *progress = ui_progress__new("Processing events...",
							self->size);
	if (progress == NULL)
		return -1;

	perf_event_ops__fill_defaults(ops);

	page_size = sysconf(_SC_PAGESIZE);

	head = data_offset;
	shift = page_size * (head / page_size);
	offset += shift;
	head -= shift;

	mmap_prot  = PROT_READ;
	mmap_flags = MAP_SHARED;

	if (self->header.needs_swap) {
		mmap_prot  |= PROT_WRITE;
		mmap_flags = MAP_PRIVATE;
	}
remap:
	buf = mmap(NULL, page_size * self->mmap_window, mmap_prot,
		   mmap_flags, self->fd, offset);
	if (buf == MAP_FAILED) {
		pr_err("failed to mmap file\n");
		err = -errno;
		goto out_err;
	}

more:
	event = (event_t *)(buf + head);
	ui_progress__update(progress, offset);

	if (self->header.needs_swap)
		perf_event_header__bswap(&event->header);
	size = event->header.size;
	if (size == 0)
		size = 8;

	if (head + event->header.size >= page_size * self->mmap_window) {
		int munmap_ret;

		shift = page_size * (head / page_size);

		munmap_ret = munmap(buf, page_size * self->mmap_window);
		assert(munmap_ret == 0);

		offset += shift;
		head -= shift;
		goto remap;
	}

	size = event->header.size;

	dump_printf("\n%#Lx [%#x]: event: %d\n",
		    offset + head, event->header.size, event->header.type);

	if (size == 0 ||
	    perf_session__process_event(self, event, ops, offset, head) < 0) {
		dump_printf("%#Lx [%#x]: skipping unknown header type: %d\n",
			    offset + head, event->header.size,
			    event->header.type);
		/*
		 * assume we lost track of the stream, check alignment, and
		 * increment a single u64 in the hope to catch on again 'soon'.
		 */
		if (unlikely(head & 7))
			head &= ~7ULL;

		size = 8;
	}

	head += size;

	if (offset + head >= data_offset + data_size)
		goto done;

	if (offset + head < file_size)
		goto more;
done:
	err = 0;
	/* do the final flush for ordered samples */
	self->ordered_samples.flush_limit = ULLONG_MAX;
	flush_sample_queue(self, ops);
out_err:
	ui_progress__delete(progress);
	return err;
}

int perf_session__process_events(struct perf_session *self,
				 struct perf_event_ops *ops)
{
	int err;

	if (perf_session__register_idle_thread(self) == NULL)
		return -ENOMEM;

	if (!symbol_conf.full_paths) {
		char bf[PATH_MAX];

		if (getcwd(bf, sizeof(bf)) == NULL) {
			err = -errno;
out_getcwd_err:
			pr_err("failed to get the current directory\n");
			goto out_err;
		}
		self->cwd = strdup(bf);
		if (self->cwd == NULL) {
			err = -ENOMEM;
			goto out_getcwd_err;
		}
		self->cwdlen = strlen(self->cwd);
	}

	if (!self->fd_pipe)
		err = __perf_session__process_events(self,
						     self->header.data_offset,
						     self->header.data_size,
						     self->size, ops);
	else
		err = __perf_session__process_pipe_events(self, ops);
out_err:
	return err;
}

bool perf_session__has_traces(struct perf_session *self, const char *msg)
{
	if (!(self->sample_type & PERF_SAMPLE_RAW)) {
		pr_err("No trace sample to read. Did you call 'perf %s'?\n", msg);
		return false;
	}

	return true;
}

int perf_session__set_kallsyms_ref_reloc_sym(struct map **maps,
					     const char *symbol_name,
					     u64 addr)
{
	char *bracket;
	enum map_type i;
	struct ref_reloc_sym *ref;

	ref = zalloc(sizeof(struct ref_reloc_sym));
	if (ref == NULL)
		return -ENOMEM;

	ref->name = strdup(symbol_name);
	if (ref->name == NULL) {
		free(ref);
		return -ENOMEM;
	}

	bracket = strchr(ref->name, ']');
	if (bracket)
		*bracket = '\0';

	ref->addr = addr;

	for (i = 0; i < MAP__NR_TYPES; ++i) {
		struct kmap *kmap = map__kmap(maps[i]);
		kmap->ref_reloc_sym = ref;
	}

	return 0;
}
