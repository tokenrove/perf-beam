#define _FILE_OFFSET_BITS 64

#include "util.h"
#include <sys/types.h>
#include <byteswap.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <sys/utsname.h>

#include "evlist.h"
#include "evsel.h"
#include "header.h"
#include "../perf.h"
#include "trace-event.h"
#include "session.h"
#include "symbol.h"
#include "debug.h"
#include "cpumap.h"

static bool no_buildid_cache = false;

static int event_count;
static struct perf_trace_event_type *events;

static u32 header_argc;
static const char **header_argv;

int perf_header__push_event(u64 id, const char *name)
{
	if (strlen(name) > MAX_EVENT_NAME)
		pr_warning("Event %s will be truncated\n", name);

	if (!events) {
		events = malloc(sizeof(struct perf_trace_event_type));
		if (events == NULL)
			return -ENOMEM;
	} else {
		struct perf_trace_event_type *nevents;

		nevents = realloc(events, (event_count + 1) * sizeof(*events));
		if (nevents == NULL)
			return -ENOMEM;
		events = nevents;
	}
	memset(&events[event_count], 0, sizeof(struct perf_trace_event_type));
	events[event_count].event_id = id;
	strncpy(events[event_count].name, name, MAX_EVENT_NAME - 1);
	event_count++;
	return 0;
}

char *perf_header__find_event(u64 id)
{
	int i;
	for (i = 0 ; i < event_count; i++) {
		if (events[i].event_id == id)
			return events[i].name;
	}
	return NULL;
}

static const char *__perf_magic = "PERFFILE";

#define PERF_MAGIC	(*(u64 *)__perf_magic)

struct perf_file_attr {
	struct perf_event_attr	attr;
	struct perf_file_section	ids;
};

void perf_header__set_feat(struct perf_header *header, int feat)
{
	set_bit(feat, header->adds_features);
}

void perf_header__clear_feat(struct perf_header *header, int feat)
{
	clear_bit(feat, header->adds_features);
}

bool perf_header__has_feat(const struct perf_header *header, int feat)
{
	return test_bit(feat, header->adds_features);
}

static int do_write(int fd, const void *buf, size_t size)
{
	while (size) {
		int ret = write(fd, buf, size);

		if (ret < 0)
			return -errno;

		size -= ret;
		buf += ret;
	}

	return 0;
}

#define NAME_ALIGN 64

static int write_padded(int fd, const void *bf, size_t count,
			size_t count_aligned)
{
	static const char zero_buf[NAME_ALIGN];
	int err = do_write(fd, bf, count);

	if (!err)
		err = do_write(fd, zero_buf, count_aligned - count);

	return err;
}

static int do_write_string(int fd, const char *str)
{
	u32 len, olen;
	int ret;

	olen = strlen(str) + 1;
	len = ALIGN(olen, NAME_ALIGN);

	/* write len, incl. \0 */
	ret = do_write(fd, &len, sizeof(len));
	if (ret < 0)
		return ret;

	return write_padded(fd, str, olen, len);
}

static char *do_read_string(int fd, struct perf_header *ph)
{
	ssize_t sz, ret;
	u32 len;
	char *buf;

	sz = read(fd, &len, sizeof(len));
	if (sz < (ssize_t)sizeof(len))
		return NULL;

	if (ph->needs_swap)
		len = bswap_32(len);

	buf = malloc(len);
	if (!buf)
		return NULL;

	ret = read(fd, buf, len);
	if (ret == (ssize_t)len) {
		/*
		 * strings are padded by zeroes
		 * thus the actual strlen of buf
		 * may be less than len
		 */
		return buf;
	}

	free(buf);
	return NULL;
}

int
perf_header__set_cmdline(int argc, const char **argv)
{
	int i;

	header_argc = (u32)argc;

	/* do not include NULL termination */
	header_argv = calloc(argc, sizeof(char *));
	if (!header_argv)
		return -ENOMEM;

	/*
	 * must copy argv contents because it gets moved
	 * around during option parsing
	 */
	for (i = 0; i < argc ; i++)
		header_argv[i] = argv[i];

	return 0;
}

#define dsos__for_each_with_build_id(pos, head)	\
	list_for_each_entry(pos, head, node)	\
		if (!pos->has_build_id)		\
			continue;		\
		else

static int __dsos__write_buildid_table(struct list_head *head, pid_t pid,
				u16 misc, int fd)
{
	struct dso *pos;

	dsos__for_each_with_build_id(pos, head) {
		int err;
		struct build_id_event b;
		size_t len;

		if (!pos->hit)
			continue;
		len = pos->long_name_len + 1;
		len = ALIGN(len, NAME_ALIGN);
		memset(&b, 0, sizeof(b));
		memcpy(&b.build_id, pos->build_id, sizeof(pos->build_id));
		b.pid = pid;
		b.header.misc = misc;
		b.header.size = sizeof(b) + len;
		err = do_write(fd, &b, sizeof(b));
		if (err < 0)
			return err;
		err = write_padded(fd, pos->long_name,
				   pos->long_name_len + 1, len);
		if (err < 0)
			return err;
	}

	return 0;
}

static int machine__write_buildid_table(struct machine *machine, int fd)
{
	int err;
	u16 kmisc = PERF_RECORD_MISC_KERNEL,
	    umisc = PERF_RECORD_MISC_USER;

	if (!machine__is_host(machine)) {
		kmisc = PERF_RECORD_MISC_GUEST_KERNEL;
		umisc = PERF_RECORD_MISC_GUEST_USER;
	}

	err = __dsos__write_buildid_table(&machine->kernel_dsos, machine->pid,
					  kmisc, fd);
	if (err == 0)
		err = __dsos__write_buildid_table(&machine->user_dsos,
						  machine->pid, umisc, fd);
	return err;
}

static int dsos__write_buildid_table(struct perf_header *header, int fd)
{
	struct perf_session *session = container_of(header,
			struct perf_session, header);
	struct rb_node *nd;
	int err = machine__write_buildid_table(&session->host_machine, fd);

	if (err)
		return err;

	for (nd = rb_first(&session->machines); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		err = machine__write_buildid_table(pos, fd);
		if (err)
			break;
	}
	return err;
}

int build_id_cache__add_s(const char *sbuild_id, const char *debugdir,
			  const char *name, bool is_kallsyms)
{
	const size_t size = PATH_MAX;
	char *realname, *filename = zalloc(size),
	     *linkname = zalloc(size), *targetname;
	int len, err = -1;

	if (is_kallsyms) {
		if (symbol_conf.kptr_restrict) {
			pr_debug("Not caching a kptr_restrict'ed /proc/kallsyms\n");
			return 0;
		}
		realname = (char *)name;
	} else
		realname = realpath(name, NULL);

	if (realname == NULL || filename == NULL || linkname == NULL)
		goto out_free;

	len = snprintf(filename, size, "%s%s%s",
		       debugdir, is_kallsyms ? "/" : "", realname);
	if (mkdir_p(filename, 0755))
		goto out_free;

	snprintf(filename + len, sizeof(filename) - len, "/%s", sbuild_id);

	if (access(filename, F_OK)) {
		if (is_kallsyms) {
			 if (copyfile("/proc/kallsyms", filename))
				goto out_free;
		} else if (link(realname, filename) && copyfile(name, filename))
			goto out_free;
	}

	len = snprintf(linkname, size, "%s/.build-id/%.2s",
		       debugdir, sbuild_id);

	if (access(linkname, X_OK) && mkdir_p(linkname, 0755))
		goto out_free;

	snprintf(linkname + len, size - len, "/%s", sbuild_id + 2);
	targetname = filename + strlen(debugdir) - 5;
	memcpy(targetname, "../..", 5);

	if (symlink(targetname, linkname) == 0)
		err = 0;
out_free:
	if (!is_kallsyms)
		free(realname);
	free(filename);
	free(linkname);
	return err;
}

static int build_id_cache__add_b(const u8 *build_id, size_t build_id_size,
				 const char *name, const char *debugdir,
				 bool is_kallsyms)
{
	char sbuild_id[BUILD_ID_SIZE * 2 + 1];

	build_id__sprintf(build_id, build_id_size, sbuild_id);

	return build_id_cache__add_s(sbuild_id, debugdir, name, is_kallsyms);
}

int build_id_cache__remove_s(const char *sbuild_id, const char *debugdir)
{
	const size_t size = PATH_MAX;
	char *filename = zalloc(size),
	     *linkname = zalloc(size);
	int err = -1;

	if (filename == NULL || linkname == NULL)
		goto out_free;

	snprintf(linkname, size, "%s/.build-id/%.2s/%s",
		 debugdir, sbuild_id, sbuild_id + 2);

	if (access(linkname, F_OK))
		goto out_free;

	if (readlink(linkname, filename, size - 1) < 0)
		goto out_free;

	if (unlink(linkname))
		goto out_free;

	/*
	 * Since the link is relative, we must make it absolute:
	 */
	snprintf(linkname, size, "%s/.build-id/%.2s/%s",
		 debugdir, sbuild_id, filename);

	if (unlink(linkname))
		goto out_free;

	err = 0;
out_free:
	free(filename);
	free(linkname);
	return err;
}

static int dso__cache_build_id(struct dso *dso, const char *debugdir)
{
	bool is_kallsyms = dso->kernel && dso->long_name[0] != '/';

	return build_id_cache__add_b(dso->build_id, sizeof(dso->build_id),
				     dso->long_name, debugdir, is_kallsyms);
}

static int __dsos__cache_build_ids(struct list_head *head, const char *debugdir)
{
	struct dso *pos;
	int err = 0;

	dsos__for_each_with_build_id(pos, head)
		if (dso__cache_build_id(pos, debugdir))
			err = -1;

	return err;
}

static int machine__cache_build_ids(struct machine *machine, const char *debugdir)
{
	int ret = __dsos__cache_build_ids(&machine->kernel_dsos, debugdir);
	ret |= __dsos__cache_build_ids(&machine->user_dsos, debugdir);
	return ret;
}

static int perf_session__cache_build_ids(struct perf_session *session)
{
	struct rb_node *nd;
	int ret;
	char debugdir[PATH_MAX];

	snprintf(debugdir, sizeof(debugdir), "%s", buildid_dir);

	if (mkdir(debugdir, 0755) != 0 && errno != EEXIST)
		return -1;

	ret = machine__cache_build_ids(&session->host_machine, debugdir);

	for (nd = rb_first(&session->machines); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		ret |= machine__cache_build_ids(pos, debugdir);
	}
	return ret ? -1 : 0;
}

static bool machine__read_build_ids(struct machine *machine, bool with_hits)
{
	bool ret = __dsos__read_build_ids(&machine->kernel_dsos, with_hits);
	ret |= __dsos__read_build_ids(&machine->user_dsos, with_hits);
	return ret;
}

static bool perf_session__read_build_ids(struct perf_session *session, bool with_hits)
{
	struct rb_node *nd;
	bool ret = machine__read_build_ids(&session->host_machine, with_hits);

	for (nd = rb_first(&session->machines); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		ret |= machine__read_build_ids(pos, with_hits);
	}

	return ret;
}

static int write_trace_info(int fd, struct perf_header *h __used,
			    struct perf_evlist *evlist)
{
	return read_tracing_data(fd, &evlist->entries);
}


static int write_build_id(int fd, struct perf_header *h,
			  struct perf_evlist *evlist __used)
{
	struct perf_session *session;
	int err;

	session = container_of(h, struct perf_session, header);

	if (!perf_session__read_build_ids(session, true))
		return -1;

	err = dsos__write_buildid_table(h, fd);
	if (err < 0) {
		pr_debug("failed to write buildid table\n");
		return err;
	}
	if (!no_buildid_cache)
		perf_session__cache_build_ids(session);

	return 0;
}

static int write_hostname(int fd, struct perf_header *h __used,
			  struct perf_evlist *evlist __used)
{
	struct utsname uts;
	int ret;

	ret = uname(&uts);
	if (ret < 0)
		return -1;

	return do_write_string(fd, uts.nodename);
}

static int write_osrelease(int fd, struct perf_header *h __used,
			   struct perf_evlist *evlist __used)
{
	struct utsname uts;
	int ret;

	ret = uname(&uts);
	if (ret < 0)
		return -1;

	return do_write_string(fd, uts.release);
}

static int write_arch(int fd, struct perf_header *h __used,
		      struct perf_evlist *evlist __used)
{
	struct utsname uts;
	int ret;

	ret = uname(&uts);
	if (ret < 0)
		return -1;

	return do_write_string(fd, uts.machine);
}

static int write_version(int fd, struct perf_header *h __used,
			 struct perf_evlist *evlist __used)
{
	return do_write_string(fd, perf_version_string);
}

static int write_cpudesc(int fd, struct perf_header *h __used,
		       struct perf_evlist *evlist __used)
{
#ifndef CPUINFO_PROC
#define CPUINFO_PROC NULL
#endif
	FILE *file;
	char *buf = NULL;
	char *s, *p;
	const char *search = CPUINFO_PROC;
	size_t len = 0;
	int ret = -1;

	if (!search)
		return -1;

	file = fopen("/proc/cpuinfo", "r");
	if (!file)
		return -1;

	while (getline(&buf, &len, file) > 0) {
		ret = strncmp(buf, search, strlen(search));
		if (!ret)
			break;
	}

	if (ret)
		goto done;

	s = buf;

	p = strchr(buf, ':');
	if (p && *(p+1) == ' ' && *(p+2))
		s = p + 2;
	p = strchr(s, '\n');
	if (p)
		*p = '\0';

	/* squash extra space characters (branding string) */
	p = s;
	while (*p) {
		if (isspace(*p)) {
			char *r = p + 1;
			char *q = r;
			*p = ' ';
			while (*q && isspace(*q))
				q++;
			if (q != (p+1))
				while ((*r++ = *q++));
		}
		p++;
	}
	ret = do_write_string(fd, s);
done:
	free(buf);
	fclose(file);
	return ret;
}

static int write_nrcpus(int fd, struct perf_header *h __used,
			struct perf_evlist *evlist __used)
{
	long nr;
	u32 nrc, nra;
	int ret;

	nr = sysconf(_SC_NPROCESSORS_CONF);
	if (nr < 0)
		return -1;

	nrc = (u32)(nr & UINT_MAX);

	nr = sysconf(_SC_NPROCESSORS_ONLN);
	if (nr < 0)
		return -1;

	nra = (u32)(nr & UINT_MAX);

	ret = do_write(fd, &nrc, sizeof(nrc));
	if (ret < 0)
		return ret;

	return do_write(fd, &nra, sizeof(nra));
}

static int write_event_desc(int fd, struct perf_header *h __used,
			    struct perf_evlist *evlist)
{
	struct perf_evsel *attr;
	u32 nre = 0, nri, sz;
	int ret;

	list_for_each_entry(attr, &evlist->entries, node)
		nre++;

	/*
	 * write number of events
	 */
	ret = do_write(fd, &nre, sizeof(nre));
	if (ret < 0)
		return ret;

	/*
	 * size of perf_event_attr struct
	 */
	sz = (u32)sizeof(attr->attr);
	ret = do_write(fd, &sz, sizeof(sz));
	if (ret < 0)
		return ret;

	list_for_each_entry(attr, &evlist->entries, node) {

		ret = do_write(fd, &attr->attr, sz);
		if (ret < 0)
			return ret;
		/*
		 * write number of unique id per event
		 * there is one id per instance of an event
		 *
		 * copy into an nri to be independent of the
		 * type of ids,
		 */
		nri = attr->ids;
		ret = do_write(fd, &nri, sizeof(nri));
		if (ret < 0)
			return ret;

		/*
		 * write event string as passed on cmdline
		 */
		ret = do_write_string(fd, event_name(attr));
		if (ret < 0)
			return ret;
		/*
		 * write unique ids for this event
		 */
		ret = do_write(fd, attr->id, attr->ids * sizeof(u64));
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int write_cmdline(int fd, struct perf_header *h __used,
			 struct perf_evlist *evlist __used)
{
	char buf[MAXPATHLEN];
	char proc[32];
	u32 i, n;
	int ret;

	/*
	 * actual atual path to perf binary
	 */
	sprintf(proc, "/proc/%d/exe", getpid());
	ret = readlink(proc, buf, sizeof(buf));
	if (ret <= 0)
		return -1;

	/* readlink() does not add null termination */
	buf[ret] = '\0';

	/* account for binary path */
	n = header_argc + 1;

	ret = do_write(fd, &n, sizeof(n));
	if (ret < 0)
		return ret;

	ret = do_write_string(fd, buf);
	if (ret < 0)
		return ret;

	for (i = 0 ; i < header_argc; i++) {
		ret = do_write_string(fd, header_argv[i]);
		if (ret < 0)
			return ret;
	}
	return 0;
}

#define CORE_SIB_FMT \
	"/sys/devices/system/cpu/cpu%d/topology/core_siblings_list"
#define THRD_SIB_FMT \
	"/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list"

struct cpu_topo {
	u32 core_sib;
	u32 thread_sib;
	char **core_siblings;
	char **thread_siblings;
};

static int build_cpu_topo(struct cpu_topo *tp, int cpu)
{
	FILE *fp;
	char filename[MAXPATHLEN];
	char *buf = NULL, *p;
	size_t len = 0;
	u32 i = 0;
	int ret = -1;

	sprintf(filename, CORE_SIB_FMT, cpu);
	fp = fopen(filename, "r");
	if (!fp)
		return -1;

	if (getline(&buf, &len, fp) <= 0)
		goto done;

	fclose(fp);

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	for (i = 0; i < tp->core_sib; i++) {
		if (!strcmp(buf, tp->core_siblings[i]))
			break;
	}
	if (i == tp->core_sib) {
		tp->core_siblings[i] = buf;
		tp->core_sib++;
		buf = NULL;
		len = 0;
	}

	sprintf(filename, THRD_SIB_FMT, cpu);
	fp = fopen(filename, "r");
	if (!fp)
		goto done;

	if (getline(&buf, &len, fp) <= 0)
		goto done;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	for (i = 0; i < tp->thread_sib; i++) {
		if (!strcmp(buf, tp->thread_siblings[i]))
			break;
	}
	if (i == tp->thread_sib) {
		tp->thread_siblings[i] = buf;
		tp->thread_sib++;
		buf = NULL;
	}
	ret = 0;
done:
	if(fp)
		fclose(fp);
	free(buf);
	return ret;
}

static void free_cpu_topo(struct cpu_topo *tp)
{
	u32 i;

	if (!tp)
		return;

	for (i = 0 ; i < tp->core_sib; i++)
		free(tp->core_siblings[i]);

	for (i = 0 ; i < tp->thread_sib; i++)
		free(tp->thread_siblings[i]);

	free(tp);
}

static struct cpu_topo *build_cpu_topology(void)
{
	struct cpu_topo *tp;
	void *addr;
	u32 nr, i;
	size_t sz;
	long ncpus;
	int ret = -1;

	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus < 0)
		return NULL;

	nr = (u32)(ncpus & UINT_MAX);

	sz = nr * sizeof(char *);

	addr = calloc(1, sizeof(*tp) + 2 * sz);
	if (!addr)
		return NULL;

	tp = addr;

	addr += sizeof(*tp);
	tp->core_siblings = addr;
	addr += sz;
	tp->thread_siblings = addr;

	for (i = 0; i < nr; i++) {
		ret = build_cpu_topo(tp, i);
		if (ret < 0)
			break;
	}
	if (ret) {
		free_cpu_topo(tp);
		tp = NULL;
	}
	return tp;
}

static int write_cpu_topology(int fd, struct perf_header *h __used,
			  struct perf_evlist *evlist __used)
{
	struct cpu_topo *tp;
	u32 i;
	int ret;

	tp = build_cpu_topology();
	if (!tp)
		return -1;

	ret = do_write(fd, &tp->core_sib, sizeof(tp->core_sib));
	if (ret < 0)
		goto done;

	for (i = 0; i < tp->core_sib; i++) {
		ret = do_write_string(fd, tp->core_siblings[i]);
		if (ret < 0)
			goto done;
	}
	ret = do_write(fd, &tp->thread_sib, sizeof(tp->thread_sib));
	if (ret < 0)
		goto done;

	for (i = 0; i < tp->thread_sib; i++) {
		ret = do_write_string(fd, tp->thread_siblings[i]);
		if (ret < 0)
			break;
	}
done:
	free_cpu_topo(tp);
	return ret;
}



static int write_total_mem(int fd, struct perf_header *h __used,
			  struct perf_evlist *evlist __used)
{
	char *buf = NULL;
	FILE *fp;
	size_t len = 0;
	int ret = -1, n;
	uint64_t mem;

	fp = fopen("/proc/meminfo", "r");
	if (!fp)
		return -1;

	while (getline(&buf, &len, fp) > 0) {
		ret = strncmp(buf, "MemTotal:", 9);
		if (!ret)
			break;
	}
	if (!ret) {
		n = sscanf(buf, "%*s %"PRIu64, &mem);
		if (n == 1)
			ret = do_write(fd, &mem, sizeof(mem));
	}
	free(buf);
	fclose(fp);
	return ret;
}

static int write_topo_node(int fd, int node)
{
	char str[MAXPATHLEN];
	char field[32];
	char *buf = NULL, *p;
	size_t len = 0;
	FILE *fp;
	u64 mem_total, mem_free, mem;
	int ret = -1;

	sprintf(str, "/sys/devices/system/node/node%d/meminfo", node);
	fp = fopen(str, "r");
	if (!fp)
		return -1;

	while (getline(&buf, &len, fp) > 0) {
		/* skip over invalid lines */
		if (!strchr(buf, ':'))
			continue;
		if (sscanf(buf, "%*s %*d %s %"PRIu64, field, &mem) != 2)
			goto done;
		if (!strcmp(field, "MemTotal:"))
			mem_total = mem;
		if (!strcmp(field, "MemFree:"))
			mem_free = mem;
	}

	fclose(fp);

	ret = do_write(fd, &mem_total, sizeof(u64));
	if (ret)
		goto done;

	ret = do_write(fd, &mem_free, sizeof(u64));
	if (ret)
		goto done;

	ret = -1;
	sprintf(str, "/sys/devices/system/node/node%d/cpulist", node);

	fp = fopen(str, "r");
	if (!fp)
		goto done;

	if (getline(&buf, &len, fp) <= 0)
		goto done;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	ret = do_write_string(fd, buf);
done:
	free(buf);
	fclose(fp);
	return ret;
}

static int write_numa_topology(int fd, struct perf_header *h __used,
			  struct perf_evlist *evlist __used)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	struct cpu_map *node_map = NULL;
	char *c;
	u32 nr, i, j;
	int ret = -1;

	fp = fopen("/sys/devices/system/node/online", "r");
	if (!fp)
		return -1;

	if (getline(&buf, &len, fp) <= 0)
		goto done;

	c = strchr(buf, '\n');
	if (c)
		*c = '\0';

	node_map = cpu_map__new(buf);
	if (!node_map)
		goto done;

	nr = (u32)node_map->nr;

	ret = do_write(fd, &nr, sizeof(nr));
	if (ret < 0)
		goto done;

	for (i = 0; i < nr; i++) {
		j = (u32)node_map->map[i];
		ret = do_write(fd, &j, sizeof(j));
		if (ret < 0)
			break;

		ret = write_topo_node(fd, i);
		if (ret < 0)
			break;
	}
done:
	free(buf);
	fclose(fp);
	free(node_map);
	return ret;
}

/*
 * default get_cpuid(): nothing gets recorded
 * actual implementation must be in arch/$(ARCH)/util/header.c
 */
int __attribute__((weak)) get_cpuid(char *buffer __used, size_t sz __used)
{
	return -1;
}

static int write_cpuid(int fd, struct perf_header *h __used,
		       struct perf_evlist *evlist __used)
{
	char buffer[64];
	int ret;

	ret = get_cpuid(buffer, sizeof(buffer));
	if (!ret)
		goto write_it;

	return -1;
write_it:
	return do_write_string(fd, buffer);
}

static void print_hostname(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# hostname : %s\n", str);
	free(str);
}

static void print_osrelease(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# os release : %s\n", str);
	free(str);
}

static void print_arch(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# arch : %s\n", str);
	free(str);
}

static void print_cpudesc(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# cpudesc : %s\n", str);
	free(str);
}

static void print_nrcpus(struct perf_header *ph, int fd, FILE *fp)
{
	ssize_t ret;
	u32 nr;

	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		nr = -1; /* interpreted as error */

	if (ph->needs_swap)
		nr = bswap_32(nr);

	fprintf(fp, "# nrcpus online : %u\n", nr);

	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		nr = -1; /* interpreted as error */

	if (ph->needs_swap)
		nr = bswap_32(nr);

	fprintf(fp, "# nrcpus avail : %u\n", nr);
}

static void print_version(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# perf version : %s\n", str);
	free(str);
}

static void print_cmdline(struct perf_header *ph, int fd, FILE *fp)
{
	ssize_t ret;
	char *str;
	u32 nr, i;

	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		return;

	if (ph->needs_swap)
		nr = bswap_32(nr);

	fprintf(fp, "# cmdline : ");

	for (i = 0; i < nr; i++) {
		str = do_read_string(fd, ph);
		fprintf(fp, "%s ", str);
		free(str);
	}
	fputc('\n', fp);
}

static void print_cpu_topology(struct perf_header *ph, int fd, FILE *fp)
{
	ssize_t ret;
	u32 nr, i;
	char *str;

	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		return;

	if (ph->needs_swap)
		nr = bswap_32(nr);

	for (i = 0; i < nr; i++) {
		str = do_read_string(fd, ph);
		fprintf(fp, "# sibling cores   : %s\n", str);
		free(str);
	}

	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		return;

	if (ph->needs_swap)
		nr = bswap_32(nr);

	for (i = 0; i < nr; i++) {
		str = do_read_string(fd, ph);
		fprintf(fp, "# sibling threads : %s\n", str);
		free(str);
	}
}

static void print_event_desc(struct perf_header *ph, int fd, FILE *fp)
{
	struct perf_event_attr attr;
	uint64_t id;
	void *buf = NULL;
	char *str;
	u32 nre, sz, nr, i, j, msz;
	int ret;

	/* number of events */
	ret = read(fd, &nre, sizeof(nre));
	if (ret != (ssize_t)sizeof(nre))
		goto error;

	if (ph->needs_swap)
		nre = bswap_32(nre);

	ret = read(fd, &sz, sizeof(sz));
	if (ret != (ssize_t)sizeof(sz))
		goto error;

	if (ph->needs_swap)
		sz = bswap_32(sz);

	/*
	 * ensure it is at least to our ABI rev
	 */
	if (sz < (u32)sizeof(attr))
		goto error;

	memset(&attr, 0, sizeof(attr));

	/* read entire region to sync up to next field */
	buf = malloc(sz);
	if (!buf)
		goto error;

	msz = sizeof(attr);
	if (sz < msz)
		msz = sz;

	for (i = 0 ; i < nre; i++) {

		ret = read(fd, buf, sz);
		if (ret != (ssize_t)sz)
			goto error;

		if (ph->needs_swap)
			perf_event__attr_swap(buf);

		memcpy(&attr, buf, msz);

		ret = read(fd, &nr, sizeof(nr));
		if (ret != (ssize_t)sizeof(nr))
			goto error;

		if (ph->needs_swap)
			nr = bswap_32(nr);

		str = do_read_string(fd, ph);
		fprintf(fp, "# event : name = %s, ", str);
		free(str);

		fprintf(fp, "type = %d, config = 0x%"PRIx64
			    ", config1 = 0x%"PRIx64", config2 = 0x%"PRIx64,
				attr.type,
				(u64)attr.config,
				(u64)attr.config1,
				(u64)attr.config2);

		fprintf(fp, ", excl_usr = %d, excl_kern = %d",
				attr.exclude_user,
				attr.exclude_kernel);

		if (nr)
			fprintf(fp, ", id = {");

		for (j = 0 ; j < nr; j++) {
			ret = read(fd, &id, sizeof(id));
			if (ret != (ssize_t)sizeof(id))
				goto error;

			if (ph->needs_swap)
				id = bswap_64(id);

			if (j)
				fputc(',', fp);

			fprintf(fp, " %"PRIu64, id);
		}
		if (nr && j == nr)
			fprintf(fp, " }");
		fputc('\n', fp);
	}
	free(buf);
	return;
error:
	fprintf(fp, "# event desc: not available or unable to read\n");
}

static void print_total_mem(struct perf_header *h __used, int fd, FILE *fp)
{
	uint64_t mem;
	ssize_t ret;

	ret = read(fd, &mem, sizeof(mem));
	if (ret != sizeof(mem))
		goto error;

	if (h->needs_swap)
		mem = bswap_64(mem);

	fprintf(fp, "# total memory : %"PRIu64" kB\n", mem);
	return;
error:
	fprintf(fp, "# total memory : unknown\n");
}

static void print_numa_topology(struct perf_header *h __used, int fd, FILE *fp)
{
	ssize_t ret;
	u32 nr, c, i;
	char *str;
	uint64_t mem_total, mem_free;

	/* nr nodes */
	ret = read(fd, &nr, sizeof(nr));
	if (ret != (ssize_t)sizeof(nr))
		goto error;

	if (h->needs_swap)
		nr = bswap_32(nr);

	for (i = 0; i < nr; i++) {

		/* node number */
		ret = read(fd, &c, sizeof(c));
		if (ret != (ssize_t)sizeof(c))
			goto error;

		if (h->needs_swap)
			c = bswap_32(c);

		ret = read(fd, &mem_total, sizeof(u64));
		if (ret != sizeof(u64))
			goto error;

		ret = read(fd, &mem_free, sizeof(u64));
		if (ret != sizeof(u64))
			goto error;

		if (h->needs_swap) {
			mem_total = bswap_64(mem_total);
			mem_free = bswap_64(mem_free);
		}

		fprintf(fp, "# node%u meminfo  : total = %"PRIu64" kB,"
			    " free = %"PRIu64" kB\n",
			c,
			mem_total,
			mem_free);

		str = do_read_string(fd, h);
		fprintf(fp, "# node%u cpu list : %s\n", c, str);
		free(str);
	}
	return;
error:
	fprintf(fp, "# numa topology : not available\n");
}

static void print_cpuid(struct perf_header *ph, int fd, FILE *fp)
{
	char *str = do_read_string(fd, ph);
	fprintf(fp, "# cpuid : %s\n", str);
	free(str);
}

struct feature_ops {
	int (*write)(int fd, struct perf_header *h, struct perf_evlist *evlist);
	void (*print)(struct perf_header *h, int fd, FILE *fp);
	const char *name;
	bool full_only;
};

#define FEAT_OPA(n, func) \
	[n] = { .name = #n, .write = write_##func, .print = print_##func }
#define FEAT_OPF(n, func) \
	[n] = { .name = #n, .write = write_##func, .print = print_##func, .full_only = true }

/* feature_ops not implemented: */
#define print_trace_info		NULL
#define print_build_id			NULL

static const struct feature_ops feat_ops[HEADER_LAST_FEATURE] = {
	FEAT_OPA(HEADER_TRACE_INFO,	trace_info),
	FEAT_OPA(HEADER_BUILD_ID,	build_id),
	FEAT_OPA(HEADER_HOSTNAME,	hostname),
	FEAT_OPA(HEADER_OSRELEASE,	osrelease),
	FEAT_OPA(HEADER_VERSION,	version),
	FEAT_OPA(HEADER_ARCH,		arch),
	FEAT_OPA(HEADER_NRCPUS,		nrcpus),
	FEAT_OPA(HEADER_CPUDESC,	cpudesc),
	FEAT_OPA(HEADER_CPUID,		cpuid),
	FEAT_OPA(HEADER_TOTAL_MEM,	total_mem),
	FEAT_OPA(HEADER_EVENT_DESC,	event_desc),
	FEAT_OPA(HEADER_CMDLINE,	cmdline),
	FEAT_OPF(HEADER_CPU_TOPOLOGY,	cpu_topology),
	FEAT_OPF(HEADER_NUMA_TOPOLOGY,	numa_topology),
};

struct header_print_data {
	FILE *fp;
	bool full; /* extended list of headers */
};

static int perf_file_section__fprintf_info(struct perf_file_section *section,
					   struct perf_header *ph,
					   int feat, int fd, void *data)
{
	struct header_print_data *hd = data;

	if (lseek(fd, section->offset, SEEK_SET) == (off_t)-1) {
		pr_debug("Failed to lseek to %" PRIu64 " offset for feature "
				"%d, continuing...\n", section->offset, feat);
		return 0;
	}
	if (feat >= HEADER_LAST_FEATURE) {
		pr_warning("unknown feature %d\n", feat);
		return 0;
	}
	if (!feat_ops[feat].print)
		return 0;

	if (!feat_ops[feat].full_only || hd->full)
		feat_ops[feat].print(ph, fd, hd->fp);
	else
		fprintf(hd->fp, "# %s info available, use -I to display\n",
			feat_ops[feat].name);

	return 0;
}

int perf_header__fprintf_info(struct perf_session *session, FILE *fp, bool full)
{
	struct header_print_data hd;
	struct perf_header *header = &session->header;
	int fd = session->fd;
	hd.fp = fp;
	hd.full = full;

	perf_header__process_sections(header, fd, &hd,
				      perf_file_section__fprintf_info);
	return 0;
}

static int do_write_feat(int fd, struct perf_header *h, int type,
			 struct perf_file_section **p,
			 struct perf_evlist *evlist)
{
	int err;
	int ret = 0;

	if (perf_header__has_feat(h, type)) {
		if (!feat_ops[type].write)
			return -1;

		(*p)->offset = lseek(fd, 0, SEEK_CUR);

		err = feat_ops[type].write(fd, h, evlist);
		if (err < 0) {
			pr_debug("failed to write feature %d\n", type);

			/* undo anything written */
			lseek(fd, (*p)->offset, SEEK_SET);

			return -1;
		}
		(*p)->size = lseek(fd, 0, SEEK_CUR) - (*p)->offset;
		(*p)++;
	}
	return ret;
}

static int perf_header__adds_write(struct perf_header *header,
				   struct perf_evlist *evlist, int fd)
{
	int nr_sections;
	struct perf_file_section *feat_sec, *p;
	int sec_size;
	u64 sec_start;
	int feat;
	int err;

	nr_sections = bitmap_weight(header->adds_features, HEADER_FEAT_BITS);
	if (!nr_sections)
		return 0;

	feat_sec = p = calloc(sizeof(*feat_sec), nr_sections);
	if (feat_sec == NULL)
		return -ENOMEM;

	sec_size = sizeof(*feat_sec) * nr_sections;

	sec_start = header->data_offset + header->data_size;
	lseek(fd, sec_start + sec_size, SEEK_SET);

	for_each_set_bit(feat, header->adds_features, HEADER_FEAT_BITS) {
		if (do_write_feat(fd, header, feat, &p, evlist))
			perf_header__clear_feat(header, feat);
	}

	lseek(fd, sec_start, SEEK_SET);
	/*
	 * may write more than needed due to dropped feature, but
	 * this is okay, reader will skip the mising entries
	 */
	err = do_write(fd, feat_sec, sec_size);
	if (err < 0)
		pr_debug("failed to write feature section\n");
	free(feat_sec);
	return err;
}

int perf_header__write_pipe(int fd)
{
	struct perf_pipe_file_header f_header;
	int err;

	f_header = (struct perf_pipe_file_header){
		.magic	   = PERF_MAGIC,
		.size	   = sizeof(f_header),
	};

	err = do_write(fd, &f_header, sizeof(f_header));
	if (err < 0) {
		pr_debug("failed to write perf pipe header\n");
		return err;
	}

	return 0;
}

int perf_session__write_header(struct perf_session *session,
			       struct perf_evlist *evlist,
			       int fd, bool at_exit)
{
	struct perf_file_header f_header;
	struct perf_file_attr   f_attr;
	struct perf_header *header = &session->header;
	struct perf_evsel *attr, *pair = NULL;
	int err;

	lseek(fd, sizeof(f_header), SEEK_SET);

	if (session->evlist != evlist)
		pair = list_entry(session->evlist->entries.next, struct perf_evsel, node);

	list_for_each_entry(attr, &evlist->entries, node) {
		attr->id_offset = lseek(fd, 0, SEEK_CUR);
		err = do_write(fd, attr->id, attr->ids * sizeof(u64));
		if (err < 0) {
out_err_write:
			pr_debug("failed to write perf header\n");
			return err;
		}
		if (session->evlist != evlist) {
			err = do_write(fd, pair->id, pair->ids * sizeof(u64));
			if (err < 0)
				goto out_err_write;
			attr->ids += pair->ids;
			pair = list_entry(pair->node.next, struct perf_evsel, node);
		}
	}

	header->attr_offset = lseek(fd, 0, SEEK_CUR);

	list_for_each_entry(attr, &evlist->entries, node) {
		f_attr = (struct perf_file_attr){
			.attr = attr->attr,
			.ids  = {
				.offset = attr->id_offset,
				.size   = attr->ids * sizeof(u64),
			}
		};
		err = do_write(fd, &f_attr, sizeof(f_attr));
		if (err < 0) {
			pr_debug("failed to write perf header attribute\n");
			return err;
		}
	}

	header->event_offset = lseek(fd, 0, SEEK_CUR);
	header->event_size = event_count * sizeof(struct perf_trace_event_type);
	if (events) {
		err = do_write(fd, events, header->event_size);
		if (err < 0) {
			pr_debug("failed to write perf header events\n");
			return err;
		}
	}

	header->data_offset = lseek(fd, 0, SEEK_CUR);

	if (at_exit) {
		err = perf_header__adds_write(header, evlist, fd);
		if (err < 0)
			return err;
	}

	f_header = (struct perf_file_header){
		.magic	   = PERF_MAGIC,
		.size	   = sizeof(f_header),
		.attr_size = sizeof(f_attr),
		.attrs = {
			.offset = header->attr_offset,
			.size   = evlist->nr_entries * sizeof(f_attr),
		},
		.data = {
			.offset = header->data_offset,
			.size	= header->data_size,
		},
		.event_types = {
			.offset = header->event_offset,
			.size	= header->event_size,
		},
	};

	memcpy(&f_header.adds_features, &header->adds_features, sizeof(header->adds_features));

	lseek(fd, 0, SEEK_SET);
	err = do_write(fd, &f_header, sizeof(f_header));
	if (err < 0) {
		pr_debug("failed to write perf header\n");
		return err;
	}
	lseek(fd, header->data_offset + header->data_size, SEEK_SET);

	header->frozen = 1;
	return 0;
}

static int perf_header__getbuffer64(struct perf_header *header,
				    int fd, void *buf, size_t size)
{
	if (readn(fd, buf, size) <= 0)
		return -1;

	if (header->needs_swap)
		mem_bswap_64(buf, size);

	return 0;
}

int perf_header__process_sections(struct perf_header *header, int fd,
				  void *data,
				  int (*process)(struct perf_file_section *section,
						 struct perf_header *ph,
						 int feat, int fd, void *data))
{
	struct perf_file_section *feat_sec, *sec;
	int nr_sections;
	int sec_size;
	int feat;
	int err;

	nr_sections = bitmap_weight(header->adds_features, HEADER_FEAT_BITS);
	if (!nr_sections)
		return 0;

	feat_sec = sec = calloc(sizeof(*feat_sec), nr_sections);
	if (!feat_sec)
		return -1;

	sec_size = sizeof(*feat_sec) * nr_sections;

	lseek(fd, header->data_offset + header->data_size, SEEK_SET);

	err = perf_header__getbuffer64(header, fd, feat_sec, sec_size);
	if (err < 0)
		goto out_free;

	for_each_set_bit(feat, header->adds_features, HEADER_LAST_FEATURE) {
		err = process(sec++, header, feat, fd, data);
		if (err < 0)
			goto out_free;
	}
	err = 0;
out_free:
	free(feat_sec);
	return err;
}

int perf_file_header__read(struct perf_file_header *header,
			   struct perf_header *ph, int fd)
{
	lseek(fd, 0, SEEK_SET);

	if (readn(fd, header, sizeof(*header)) <= 0 ||
	    memcmp(&header->magic, __perf_magic, sizeof(header->magic)))
		return -1;

	if (header->attr_size != sizeof(struct perf_file_attr)) {
		u64 attr_size = bswap_64(header->attr_size);

		if (attr_size != sizeof(struct perf_file_attr))
			return -1;

		mem_bswap_64(header, offsetof(struct perf_file_header,
					    adds_features));
		ph->needs_swap = true;
	}

	if (header->size != sizeof(*header)) {
		/* Support the previous format */
		if (header->size == offsetof(typeof(*header), adds_features))
			bitmap_zero(header->adds_features, HEADER_FEAT_BITS);
		else
			return -1;
	} else if (ph->needs_swap) {
		unsigned int i;
		/*
		 * feature bitmap is declared as an array of unsigned longs --
		 * not good since its size can differ between the host that
		 * generated the data file and the host analyzing the file.
		 *
		 * We need to handle endianness, but we don't know the size of
		 * the unsigned long where the file was generated. Take a best
		 * guess at determining it: try 64-bit swap first (ie., file
		 * created on a 64-bit host), and check if the hostname feature
		 * bit is set (this feature bit is forced on as of fbe96f2).
		 * If the bit is not, undo the 64-bit swap and try a 32-bit
		 * swap. If the hostname bit is still not set (e.g., older data
		 * file), punt and fallback to the original behavior --
		 * clearing all feature bits and setting buildid.
		 */
		for (i = 0; i < BITS_TO_LONGS(HEADER_FEAT_BITS); ++i)
			header->adds_features[i] = bswap_64(header->adds_features[i]);

		if (!test_bit(HEADER_HOSTNAME, header->adds_features)) {
			for (i = 0; i < BITS_TO_LONGS(HEADER_FEAT_BITS); ++i) {
				header->adds_features[i] = bswap_64(header->adds_features[i]);
				header->adds_features[i] = bswap_32(header->adds_features[i]);
			}
		}

		if (!test_bit(HEADER_HOSTNAME, header->adds_features)) {
			bitmap_zero(header->adds_features, HEADER_FEAT_BITS);
			set_bit(HEADER_BUILD_ID, header->adds_features);
		}
	}

	memcpy(&ph->adds_features, &header->adds_features,
	       sizeof(ph->adds_features));

	ph->event_offset = header->event_types.offset;
	ph->event_size   = header->event_types.size;
	ph->data_offset  = header->data.offset;
	ph->data_size	 = header->data.size;
	return 0;
}

static int __event_process_build_id(struct build_id_event *bev,
				    char *filename,
				    struct perf_session *session)
{
	int err = -1;
	struct list_head *head;
	struct machine *machine;
	u16 misc;
	struct dso *dso;
	enum dso_kernel_type dso_type;

	machine = perf_session__findnew_machine(session, bev->pid);
	if (!machine)
		goto out;

	misc = bev->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;

	switch (misc) {
	case PERF_RECORD_MISC_KERNEL:
		dso_type = DSO_TYPE_KERNEL;
		head = &machine->kernel_dsos;
		break;
	case PERF_RECORD_MISC_GUEST_KERNEL:
		dso_type = DSO_TYPE_GUEST_KERNEL;
		head = &machine->kernel_dsos;
		break;
	case PERF_RECORD_MISC_USER:
	case PERF_RECORD_MISC_GUEST_USER:
		dso_type = DSO_TYPE_USER;
		head = &machine->user_dsos;
		break;
	default:
		goto out;
	}

	dso = __dsos__findnew(head, filename);
	if (dso != NULL) {
		char sbuild_id[BUILD_ID_SIZE * 2 + 1];

		dso__set_build_id(dso, &bev->build_id);

		if (filename[0] == '[')
			dso->kernel = dso_type;

		build_id__sprintf(dso->build_id, sizeof(dso->build_id),
				  sbuild_id);
		pr_debug("build id event received for %s: %s\n",
			 dso->long_name, sbuild_id);
	}

	err = 0;
out:
	return err;
}

static int perf_header__read_build_ids_abi_quirk(struct perf_header *header,
						 int input, u64 offset, u64 size)
{
	struct perf_session *session = container_of(header, struct perf_session, header);
	struct {
		struct perf_event_header   header;
		u8			   build_id[ALIGN(BUILD_ID_SIZE, sizeof(u64))];
		char			   filename[0];
	} old_bev;
	struct build_id_event bev;
	char filename[PATH_MAX];
	u64 limit = offset + size;

	while (offset < limit) {
		ssize_t len;

		if (read(input, &old_bev, sizeof(old_bev)) != sizeof(old_bev))
			return -1;

		if (header->needs_swap)
			perf_event_header__bswap(&old_bev.header);

		len = old_bev.header.size - sizeof(old_bev);
		if (read(input, filename, len) != len)
			return -1;

		bev.header = old_bev.header;

		/*
		 * As the pid is the missing value, we need to fill
		 * it properly. The header.misc value give us nice hint.
		 */
		bev.pid	= HOST_KERNEL_ID;
		if (bev.header.misc == PERF_RECORD_MISC_GUEST_USER ||
		    bev.header.misc == PERF_RECORD_MISC_GUEST_KERNEL)
			bev.pid	= DEFAULT_GUEST_KERNEL_ID;

		memcpy(bev.build_id, old_bev.build_id, sizeof(bev.build_id));
		__event_process_build_id(&bev, filename, session);

		offset += bev.header.size;
	}

	return 0;
}

static int perf_header__read_build_ids(struct perf_header *header,
				       int input, u64 offset, u64 size)
{
	struct perf_session *session = container_of(header, struct perf_session, header);
	struct build_id_event bev;
	char filename[PATH_MAX];
	u64 limit = offset + size, orig_offset = offset;
	int err = -1;

	while (offset < limit) {
		ssize_t len;

		if (read(input, &bev, sizeof(bev)) != sizeof(bev))
			goto out;

		if (header->needs_swap)
			perf_event_header__bswap(&bev.header);

		len = bev.header.size - sizeof(bev);
		if (read(input, filename, len) != len)
			goto out;
		/*
		 * The a1645ce1 changeset:
		 *
		 * "perf: 'perf kvm' tool for monitoring guest performance from host"
		 *
		 * Added a field to struct build_id_event that broke the file
		 * format.
		 *
		 * Since the kernel build-id is the first entry, process the
		 * table using the old format if the well known
		 * '[kernel.kallsyms]' string for the kernel build-id has the
		 * first 4 characters chopped off (where the pid_t sits).
		 */
		if (memcmp(filename, "nel.kallsyms]", 13) == 0) {
			if (lseek(input, orig_offset, SEEK_SET) == (off_t)-1)
				return -1;
			return perf_header__read_build_ids_abi_quirk(header, input, offset, size);
		}

		__event_process_build_id(&bev, filename, session);

		offset += bev.header.size;
	}
	err = 0;
out:
	return err;
}

static int perf_file_section__process(struct perf_file_section *section,
				      struct perf_header *ph,
				      int feat, int fd, void *data __used)
{
	if (lseek(fd, section->offset, SEEK_SET) == (off_t)-1) {
		pr_debug("Failed to lseek to %" PRIu64 " offset for feature "
			  "%d, continuing...\n", section->offset, feat);
		return 0;
	}

	if (feat >= HEADER_LAST_FEATURE) {
		pr_debug("unknown feature %d, continuing...\n", feat);
		return 0;
	}

	switch (feat) {
	case HEADER_TRACE_INFO:
		trace_report(fd, false);
		break;
	case HEADER_BUILD_ID:
		if (perf_header__read_build_ids(ph, fd, section->offset, section->size))
			pr_debug("Failed to read buildids, continuing...\n");
		break;
	default:
		break;
	}

	return 0;
}

static int perf_file_header__read_pipe(struct perf_pipe_file_header *header,
				       struct perf_header *ph, int fd,
				       bool repipe)
{
	if (readn(fd, header, sizeof(*header)) <= 0 ||
	    memcmp(&header->magic, __perf_magic, sizeof(header->magic)))
		return -1;

	if (repipe && do_write(STDOUT_FILENO, header, sizeof(*header)) < 0)
		return -1;

	if (header->size != sizeof(*header)) {
		u64 size = bswap_64(header->size);

		if (size != sizeof(*header))
			return -1;

		ph->needs_swap = true;
	}

	return 0;
}

static int perf_header__read_pipe(struct perf_session *session, int fd)
{
	struct perf_header *header = &session->header;
	struct perf_pipe_file_header f_header;

	if (perf_file_header__read_pipe(&f_header, header, fd,
					session->repipe) < 0) {
		pr_debug("incompatible file format\n");
		return -EINVAL;
	}

	session->fd = fd;

	return 0;
}

int perf_session__read_header(struct perf_session *session, int fd)
{
	struct perf_header *header = &session->header;
	struct perf_file_header	f_header;
	struct perf_file_attr	f_attr;
	u64			f_id;
	int nr_attrs, nr_ids, i, j;

	session->evlist = perf_evlist__new(NULL, NULL);
	if (session->evlist == NULL)
		return -ENOMEM;

	if (session->fd_pipe)
		return perf_header__read_pipe(session, fd);

	if (perf_file_header__read(&f_header, header, fd) < 0) {
		pr_debug("incompatible file format\n");
		return -EINVAL;
	}

	nr_attrs = f_header.attrs.size / sizeof(f_attr);
	lseek(fd, f_header.attrs.offset, SEEK_SET);

	for (i = 0; i < nr_attrs; i++) {
		struct perf_evsel *evsel;
		off_t tmp;

		if (readn(fd, &f_attr, sizeof(f_attr)) <= 0)
			goto out_errno;

		if (header->needs_swap)
			perf_event__attr_swap(&f_attr.attr);

		tmp = lseek(fd, 0, SEEK_CUR);
		evsel = perf_evsel__new(&f_attr.attr, i);

		if (evsel == NULL)
			goto out_delete_evlist;
		/*
		 * Do it before so that if perf_evsel__alloc_id fails, this
		 * entry gets purged too at perf_evlist__delete().
		 */
		perf_evlist__add(session->evlist, evsel);

		nr_ids = f_attr.ids.size / sizeof(u64);
		/*
		 * We don't have the cpu and thread maps on the header, so
		 * for allocating the perf_sample_id table we fake 1 cpu and
		 * hattr->ids threads.
		 */
		if (perf_evsel__alloc_id(evsel, 1, nr_ids))
			goto out_delete_evlist;

		lseek(fd, f_attr.ids.offset, SEEK_SET);

		for (j = 0; j < nr_ids; j++) {
			if (perf_header__getbuffer64(header, fd, &f_id, sizeof(f_id)))
				goto out_errno;

			perf_evlist__id_add(session->evlist, evsel, 0, j, f_id);
		}

		lseek(fd, tmp, SEEK_SET);
	}

	symbol_conf.nr_events = nr_attrs;

	if (f_header.event_types.size) {
		lseek(fd, f_header.event_types.offset, SEEK_SET);
		events = malloc(f_header.event_types.size);
		if (events == NULL)
			return -ENOMEM;
		if (perf_header__getbuffer64(header, fd, events,
					     f_header.event_types.size))
			goto out_errno;
		event_count =  f_header.event_types.size / sizeof(struct perf_trace_event_type);
	}

	perf_header__process_sections(header, fd, NULL,
				      perf_file_section__process);

	lseek(fd, header->data_offset, SEEK_SET);

	header->frozen = 1;
	return 0;
out_errno:
	return -errno;

out_delete_evlist:
	perf_evlist__delete(session->evlist);
	session->evlist = NULL;
	return -ENOMEM;
}

int perf_event__synthesize_attr(struct perf_tool *tool,
				struct perf_event_attr *attr, u16 ids, u64 *id,
				perf_event__handler_t process)
{
	union perf_event *ev;
	size_t size;
	int err;

	size = sizeof(struct perf_event_attr);
	size = ALIGN(size, sizeof(u64));
	size += sizeof(struct perf_event_header);
	size += ids * sizeof(u64);

	ev = malloc(size);

	if (ev == NULL)
		return -ENOMEM;

	ev->attr.attr = *attr;
	memcpy(ev->attr.id, id, ids * sizeof(u64));

	ev->attr.header.type = PERF_RECORD_HEADER_ATTR;
	ev->attr.header.size = size;

	err = process(tool, ev, NULL, NULL);

	free(ev);

	return err;
}

int perf_event__synthesize_attrs(struct perf_tool *tool,
				   struct perf_session *session,
				   perf_event__handler_t process)
{
	struct perf_evsel *attr;
	int err = 0;

	list_for_each_entry(attr, &session->evlist->entries, node) {
		err = perf_event__synthesize_attr(tool, &attr->attr, attr->ids,
						  attr->id, process);
		if (err) {
			pr_debug("failed to create perf header attribute\n");
			return err;
		}
	}

	return err;
}

int perf_event__process_attr(union perf_event *event,
			     struct perf_evlist **pevlist)
{
	unsigned int i, ids, n_ids;
	struct perf_evsel *evsel;
	struct perf_evlist *evlist = *pevlist;

	if (evlist == NULL) {
		*pevlist = evlist = perf_evlist__new(NULL, NULL);
		if (evlist == NULL)
			return -ENOMEM;
	}

	evsel = perf_evsel__new(&event->attr.attr, evlist->nr_entries);
	if (evsel == NULL)
		return -ENOMEM;

	perf_evlist__add(evlist, evsel);

	ids = event->header.size;
	ids -= (void *)&event->attr.id - (void *)event;
	n_ids = ids / sizeof(u64);
	/*
	 * We don't have the cpu and thread maps on the header, so
	 * for allocating the perf_sample_id table we fake 1 cpu and
	 * hattr->ids threads.
	 */
	if (perf_evsel__alloc_id(evsel, 1, n_ids))
		return -ENOMEM;

	for (i = 0; i < n_ids; i++) {
		perf_evlist__id_add(evlist, evsel, 0, i, event->attr.id[i]);
	}

	return 0;
}

int perf_event__synthesize_event_type(struct perf_tool *tool,
				      u64 event_id, char *name,
				      perf_event__handler_t process,
				      struct machine *machine)
{
	union perf_event ev;
	size_t size = 0;
	int err = 0;

	memset(&ev, 0, sizeof(ev));

	ev.event_type.event_type.event_id = event_id;
	memset(ev.event_type.event_type.name, 0, MAX_EVENT_NAME);
	strncpy(ev.event_type.event_type.name, name, MAX_EVENT_NAME - 1);

	ev.event_type.header.type = PERF_RECORD_HEADER_EVENT_TYPE;
	size = strlen(ev.event_type.event_type.name);
	size = ALIGN(size, sizeof(u64));
	ev.event_type.header.size = sizeof(ev.event_type) -
		(sizeof(ev.event_type.event_type.name) - size);

	err = process(tool, &ev, NULL, machine);

	return err;
}

int perf_event__synthesize_event_types(struct perf_tool *tool,
				       perf_event__handler_t process,
				       struct machine *machine)
{
	struct perf_trace_event_type *type;
	int i, err = 0;

	for (i = 0; i < event_count; i++) {
		type = &events[i];

		err = perf_event__synthesize_event_type(tool, type->event_id,
							type->name, process,
							machine);
		if (err) {
			pr_debug("failed to create perf header event type\n");
			return err;
		}
	}

	return err;
}

int perf_event__process_event_type(struct perf_tool *tool __unused,
				   union perf_event *event)
{
	if (perf_header__push_event(event->event_type.event_type.event_id,
				    event->event_type.event_type.name) < 0)
		return -ENOMEM;

	return 0;
}

int perf_event__synthesize_tracing_data(struct perf_tool *tool, int fd,
					struct perf_evlist *evlist,
					perf_event__handler_t process)
{
	union perf_event ev;
	struct tracing_data *tdata;
	ssize_t size = 0, aligned_size = 0, padding;
	int err __used = 0;

	/*
	 * We are going to store the size of the data followed
	 * by the data contents. Since the fd descriptor is a pipe,
	 * we cannot seek back to store the size of the data once
	 * we know it. Instead we:
	 *
	 * - write the tracing data to the temp file
	 * - get/write the data size to pipe
	 * - write the tracing data from the temp file
	 *   to the pipe
	 */
	tdata = tracing_data_get(&evlist->entries, fd, true);
	if (!tdata)
		return -1;

	memset(&ev, 0, sizeof(ev));

	ev.tracing_data.header.type = PERF_RECORD_HEADER_TRACING_DATA;
	size = tdata->size;
	aligned_size = ALIGN(size, sizeof(u64));
	padding = aligned_size - size;
	ev.tracing_data.header.size = sizeof(ev.tracing_data);
	ev.tracing_data.size = aligned_size;

	process(tool, &ev, NULL, NULL);

	/*
	 * The put function will copy all the tracing data
	 * stored in temp file to the pipe.
	 */
	tracing_data_put(tdata);

	write_padded(fd, NULL, 0, padding);

	return aligned_size;
}

int perf_event__process_tracing_data(union perf_event *event,
				     struct perf_session *session)
{
	ssize_t size_read, padding, size = event->tracing_data.size;
	off_t offset = lseek(session->fd, 0, SEEK_CUR);
	char buf[BUFSIZ];

	/* setup for reading amidst mmap */
	lseek(session->fd, offset + sizeof(struct tracing_data_event),
	      SEEK_SET);

	size_read = trace_report(session->fd, session->repipe);

	padding = ALIGN(size_read, sizeof(u64)) - size_read;

	if (read(session->fd, buf, padding) < 0)
		die("reading input file");
	if (session->repipe) {
		int retw = write(STDOUT_FILENO, buf, padding);
		if (retw <= 0 || retw != padding)
			die("repiping tracing data padding");
	}

	if (size_read + padding != size)
		die("tracing data size mismatch");

	return size_read + padding;
}

int perf_event__synthesize_build_id(struct perf_tool *tool,
				    struct dso *pos, u16 misc,
				    perf_event__handler_t process,
				    struct machine *machine)
{
	union perf_event ev;
	size_t len;
	int err = 0;

	if (!pos->hit)
		return err;

	memset(&ev, 0, sizeof(ev));

	len = pos->long_name_len + 1;
	len = ALIGN(len, NAME_ALIGN);
	memcpy(&ev.build_id.build_id, pos->build_id, sizeof(pos->build_id));
	ev.build_id.header.type = PERF_RECORD_HEADER_BUILD_ID;
	ev.build_id.header.misc = misc;
	ev.build_id.pid = machine->pid;
	ev.build_id.header.size = sizeof(ev.build_id) + len;
	memcpy(&ev.build_id.filename, pos->long_name, pos->long_name_len);

	err = process(tool, &ev, NULL, machine);

	return err;
}

int perf_event__process_build_id(struct perf_tool *tool __used,
				 union perf_event *event,
				 struct perf_session *session)
{
	__event_process_build_id(&event->build_id,
				 event->build_id.filename,
				 session);
	return 0;
}

void disable_buildid_cache(void)
{
	no_buildid_cache = true;
}
