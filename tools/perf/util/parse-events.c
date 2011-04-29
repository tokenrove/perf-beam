#include "../../../include/linux/hw_breakpoint.h"
#include "util.h"
#include "../perf.h"
#include "evlist.h"
#include "evsel.h"
#include "parse-options.h"
#include "parse-events.h"
#include "exec_cmd.h"
#include "string.h"
#include "symbol.h"
#include "cache.h"
#include "header.h"
#include "debugfs.h"

struct event_symbol {
	u8		type;
	u64		config;
	const char	*symbol;
	const char	*alias;
};

enum event_result {
	EVT_FAILED,
	EVT_HANDLED,
	EVT_HANDLED_ALL
};

char debugfs_path[MAXPATHLEN];

#define CHW(x) .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_##x
#define CSW(x) .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_##x

static struct event_symbol event_symbols[] = {
  { CHW(CPU_CYCLES),			"cpu-cycles",			"cycles"		},
  { CHW(STALLED_CYCLES_FRONTEND),	"stalled-cycles-frontend",	"idle-cycles-frontend"	},
  { CHW(STALLED_CYCLES_BACKEND),	"stalled-cycles-backend",	"idle-cycles-backend"	},
  { CHW(INSTRUCTIONS),			"instructions",			""			},
  { CHW(CACHE_REFERENCES),		"cache-references",		""			},
  { CHW(CACHE_MISSES),			"cache-misses",			""			},
  { CHW(BRANCH_INSTRUCTIONS),		"branch-instructions",		"branches"		},
  { CHW(BRANCH_MISSES),			"branch-misses",		""			},
  { CHW(BUS_CYCLES),			"bus-cycles",			""			},

  { CSW(CPU_CLOCK),			"cpu-clock",			""			},
  { CSW(TASK_CLOCK),			"task-clock",			""			},
  { CSW(PAGE_FAULTS),			"page-faults",			"faults"		},
  { CSW(PAGE_FAULTS_MIN),		"minor-faults",			""			},
  { CSW(PAGE_FAULTS_MAJ),		"major-faults",			""			},
  { CSW(CONTEXT_SWITCHES),		"context-switches",		"cs"			},
  { CSW(CPU_MIGRATIONS),		"cpu-migrations",		"migrations"		},
  { CSW(ALIGNMENT_FAULTS),		"alignment-faults",		""			},
  { CSW(EMULATION_FAULTS),		"emulation-faults",		""			},
};

#define __PERF_EVENT_FIELD(config, name) \
	((config & PERF_EVENT_##name##_MASK) >> PERF_EVENT_##name##_SHIFT)

#define PERF_EVENT_RAW(config)		__PERF_EVENT_FIELD(config, RAW)
#define PERF_EVENT_CONFIG(config)	__PERF_EVENT_FIELD(config, CONFIG)
#define PERF_EVENT_TYPE(config)		__PERF_EVENT_FIELD(config, TYPE)
#define PERF_EVENT_ID(config)		__PERF_EVENT_FIELD(config, EVENT)

static const char *hw_event_names[PERF_COUNT_HW_MAX] = {
	"cycles",
	"instructions",
	"cache-references",
	"cache-misses",
	"branches",
	"branch-misses",
	"bus-cycles",
	"stalled-cycles-frontend",
	"stalled-cycles-backend",
};

static const char *sw_event_names[PERF_COUNT_SW_MAX] = {
	"cpu-clock",
	"task-clock",
	"page-faults",
	"context-switches",
	"CPU-migrations",
	"minor-faults",
	"major-faults",
	"alignment-faults",
	"emulation-faults",
};

#define MAX_ALIASES 8

static const char *hw_cache[][MAX_ALIASES] = {
 { "L1-dcache",	"l1-d",		"l1d",		"L1-data",		},
 { "L1-icache",	"l1-i",		"l1i",		"L1-instruction",	},
 { "LLC",	"L2"							},
 { "dTLB",	"d-tlb",	"Data-TLB",				},
 { "iTLB",	"i-tlb",	"Instruction-TLB",			},
 { "branch",	"branches",	"bpu",		"btb",		"bpc",	},
};

static const char *hw_cache_op[][MAX_ALIASES] = {
 { "load",	"loads",	"read",					},
 { "store",	"stores",	"write",				},
 { "prefetch",	"prefetches",	"speculative-read", "speculative-load",	},
};

static const char *hw_cache_result[][MAX_ALIASES] = {
 { "refs",	"Reference",	"ops",		"access",		},
 { "misses",	"miss",							},
};

#define C(x)		PERF_COUNT_HW_CACHE_##x
#define CACHE_READ	(1 << C(OP_READ))
#define CACHE_WRITE	(1 << C(OP_WRITE))
#define CACHE_PREFETCH	(1 << C(OP_PREFETCH))
#define COP(x)		(1 << x)

/*
 * cache operartion stat
 * L1I : Read and prefetch only
 * ITLB and BPU : Read-only
 */
static unsigned long hw_cache_stat[C(MAX)] = {
 [C(L1D)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(L1I)]	= (CACHE_READ | CACHE_PREFETCH),
 [C(LL)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(DTLB)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(ITLB)]	= (CACHE_READ),
 [C(BPU)]	= (CACHE_READ),
};

#define for_each_subsystem(sys_dir, sys_dirent, sys_next)	       \
	while (!readdir_r(sys_dir, &sys_dirent, &sys_next) && sys_next)	       \
	if (sys_dirent.d_type == DT_DIR &&				       \
	   (strcmp(sys_dirent.d_name, ".")) &&				       \
	   (strcmp(sys_dirent.d_name, "..")))

static int tp_event_has_id(struct dirent *sys_dir, struct dirent *evt_dir)
{
	char evt_path[MAXPATHLEN];
	int fd;

	snprintf(evt_path, MAXPATHLEN, "%s/%s/%s/id", debugfs_path,
			sys_dir->d_name, evt_dir->d_name);
	fd = open(evt_path, O_RDONLY);
	if (fd < 0)
		return -EINVAL;
	close(fd);

	return 0;
}

#define for_each_event(sys_dirent, evt_dir, evt_dirent, evt_next)	       \
	while (!readdir_r(evt_dir, &evt_dirent, &evt_next) && evt_next)        \
	if (evt_dirent.d_type == DT_DIR &&				       \
	   (strcmp(evt_dirent.d_name, ".")) &&				       \
	   (strcmp(evt_dirent.d_name, "..")) &&				       \
	   (!tp_event_has_id(&sys_dirent, &evt_dirent)))

#define MAX_EVENT_LENGTH 512


struct tracepoint_path *tracepoint_id_to_path(u64 config)
{
	struct tracepoint_path *path = NULL;
	DIR *sys_dir, *evt_dir;
	struct dirent *sys_next, *evt_next, sys_dirent, evt_dirent;
	char id_buf[4];
	int fd;
	u64 id;
	char evt_path[MAXPATHLEN];
	char dir_path[MAXPATHLEN];

	if (debugfs_valid_mountpoint(debugfs_path))
		return NULL;

	sys_dir = opendir(debugfs_path);
	if (!sys_dir)
		return NULL;

	for_each_subsystem(sys_dir, sys_dirent, sys_next) {

		snprintf(dir_path, MAXPATHLEN, "%s/%s", debugfs_path,
			 sys_dirent.d_name);
		evt_dir = opendir(dir_path);
		if (!evt_dir)
			continue;

		for_each_event(sys_dirent, evt_dir, evt_dirent, evt_next) {

			snprintf(evt_path, MAXPATHLEN, "%s/%s/id", dir_path,
				 evt_dirent.d_name);
			fd = open(evt_path, O_RDONLY);
			if (fd < 0)
				continue;
			if (read(fd, id_buf, sizeof(id_buf)) < 0) {
				close(fd);
				continue;
			}
			close(fd);
			id = atoll(id_buf);
			if (id == config) {
				closedir(evt_dir);
				closedir(sys_dir);
				path = zalloc(sizeof(*path));
				path->system = malloc(MAX_EVENT_LENGTH);
				if (!path->system) {
					free(path);
					return NULL;
				}
				path->name = malloc(MAX_EVENT_LENGTH);
				if (!path->name) {
					free(path->system);
					free(path);
					return NULL;
				}
				strncpy(path->system, sys_dirent.d_name,
					MAX_EVENT_LENGTH);
				strncpy(path->name, evt_dirent.d_name,
					MAX_EVENT_LENGTH);
				return path;
			}
		}
		closedir(evt_dir);
	}

	closedir(sys_dir);
	return NULL;
}

#define TP_PATH_LEN (MAX_EVENT_LENGTH * 2 + 1)
static const char *tracepoint_id_to_name(u64 config)
{
	static char buf[TP_PATH_LEN];
	struct tracepoint_path *path;

	path = tracepoint_id_to_path(config);
	if (path) {
		snprintf(buf, TP_PATH_LEN, "%s:%s", path->system, path->name);
		free(path->name);
		free(path->system);
		free(path);
	} else
		snprintf(buf, TP_PATH_LEN, "%s:%s", "unknown", "unknown");

	return buf;
}

static int is_cache_op_valid(u8 cache_type, u8 cache_op)
{
	if (hw_cache_stat[cache_type] & COP(cache_op))
		return 1;	/* valid */
	else
		return 0;	/* invalid */
}

static char *event_cache_name(u8 cache_type, u8 cache_op, u8 cache_result)
{
	static char name[50];

	if (cache_result) {
		sprintf(name, "%s-%s-%s", hw_cache[cache_type][0],
			hw_cache_op[cache_op][0],
			hw_cache_result[cache_result][0]);
	} else {
		sprintf(name, "%s-%s", hw_cache[cache_type][0],
			hw_cache_op[cache_op][1]);
	}

	return name;
}

const char *event_type(int type)
{
	switch (type) {
	case PERF_TYPE_HARDWARE:
		return "hardware";

	case PERF_TYPE_SOFTWARE:
		return "software";

	case PERF_TYPE_TRACEPOINT:
		return "tracepoint";

	case PERF_TYPE_HW_CACHE:
		return "hardware-cache";

	default:
		break;
	}

	return "unknown";
}

const char *event_name(struct perf_evsel *evsel)
{
	u64 config = evsel->attr.config;
	int type = evsel->attr.type;

	if (evsel->name)
		return evsel->name;

	return __event_name(type, config);
}

const char *__event_name(int type, u64 config)
{
	static char buf[32];

	if (type == PERF_TYPE_RAW) {
		sprintf(buf, "raw 0x%" PRIx64, config);
		return buf;
	}

	switch (type) {
	case PERF_TYPE_HARDWARE:
		if (config < PERF_COUNT_HW_MAX && hw_event_names[config])
			return hw_event_names[config];
		return "unknown-hardware";

	case PERF_TYPE_HW_CACHE: {
		u8 cache_type, cache_op, cache_result;

		cache_type   = (config >>  0) & 0xff;
		if (cache_type > PERF_COUNT_HW_CACHE_MAX)
			return "unknown-ext-hardware-cache-type";

		cache_op     = (config >>  8) & 0xff;
		if (cache_op > PERF_COUNT_HW_CACHE_OP_MAX)
			return "unknown-ext-hardware-cache-op";

		cache_result = (config >> 16) & 0xff;
		if (cache_result > PERF_COUNT_HW_CACHE_RESULT_MAX)
			return "unknown-ext-hardware-cache-result";

		if (!is_cache_op_valid(cache_type, cache_op))
			return "invalid-cache";

		return event_cache_name(cache_type, cache_op, cache_result);
	}

	case PERF_TYPE_SOFTWARE:
		if (config < PERF_COUNT_SW_MAX && sw_event_names[config])
			return sw_event_names[config];
		return "unknown-software";

	case PERF_TYPE_TRACEPOINT:
		return tracepoint_id_to_name(config);

	default:
		break;
	}

	return "unknown";
}

static int parse_aliases(const char **str, const char *names[][MAX_ALIASES], int size)
{
	int i, j;
	int n, longest = -1;

	for (i = 0; i < size; i++) {
		for (j = 0; j < MAX_ALIASES && names[i][j]; j++) {
			n = strlen(names[i][j]);
			if (n > longest && !strncasecmp(*str, names[i][j], n))
				longest = n;
		}
		if (longest > 0) {
			*str += longest;
			return i;
		}
	}

	return -1;
}

static enum event_result
parse_generic_hw_event(const char **str, struct perf_event_attr *attr)
{
	const char *s = *str;
	int cache_type = -1, cache_op = -1, cache_result = -1;

	cache_type = parse_aliases(&s, hw_cache, PERF_COUNT_HW_CACHE_MAX);
	/*
	 * No fallback - if we cannot get a clear cache type
	 * then bail out:
	 */
	if (cache_type == -1)
		return EVT_FAILED;

	while ((cache_op == -1 || cache_result == -1) && *s == '-') {
		++s;

		if (cache_op == -1) {
			cache_op = parse_aliases(&s, hw_cache_op,
						PERF_COUNT_HW_CACHE_OP_MAX);
			if (cache_op >= 0) {
				if (!is_cache_op_valid(cache_type, cache_op))
					return 0;
				continue;
			}
		}

		if (cache_result == -1) {
			cache_result = parse_aliases(&s, hw_cache_result,
						PERF_COUNT_HW_CACHE_RESULT_MAX);
			if (cache_result >= 0)
				continue;
		}

		/*
		 * Can't parse this as a cache op or result, so back up
		 * to the '-'.
		 */
		--s;
		break;
	}

	/*
	 * Fall back to reads:
	 */
	if (cache_op == -1)
		cache_op = PERF_COUNT_HW_CACHE_OP_READ;

	/*
	 * Fall back to accesses:
	 */
	if (cache_result == -1)
		cache_result = PERF_COUNT_HW_CACHE_RESULT_ACCESS;

	attr->config = cache_type | (cache_op << 8) | (cache_result << 16);
	attr->type = PERF_TYPE_HW_CACHE;

	*str = s;
	return EVT_HANDLED;
}

static enum event_result
parse_single_tracepoint_event(char *sys_name,
			      const char *evt_name,
			      unsigned int evt_length,
			      struct perf_event_attr *attr,
			      const char **strp)
{
	char evt_path[MAXPATHLEN];
	char id_buf[4];
	u64 id;
	int fd;

	snprintf(evt_path, MAXPATHLEN, "%s/%s/%s/id", debugfs_path,
		 sys_name, evt_name);

	fd = open(evt_path, O_RDONLY);
	if (fd < 0)
		return EVT_FAILED;

	if (read(fd, id_buf, sizeof(id_buf)) < 0) {
		close(fd);
		return EVT_FAILED;
	}

	close(fd);
	id = atoll(id_buf);
	attr->config = id;
	attr->type = PERF_TYPE_TRACEPOINT;
	*strp += strlen(sys_name) + evt_length + 1; /* + 1 for the ':' */

	attr->sample_type |= PERF_SAMPLE_RAW;
	attr->sample_type |= PERF_SAMPLE_TIME;
	attr->sample_type |= PERF_SAMPLE_CPU;

	attr->sample_period = 1;


	return EVT_HANDLED;
}

/* sys + ':' + event + ':' + flags*/
#define MAX_EVOPT_LEN	(MAX_EVENT_LENGTH * 2 + 2 + 128)
static enum event_result
parse_multiple_tracepoint_event(const struct option *opt, char *sys_name,
				const char *evt_exp, char *flags)
{
	char evt_path[MAXPATHLEN];
	struct dirent *evt_ent;
	DIR *evt_dir;

	snprintf(evt_path, MAXPATHLEN, "%s/%s", debugfs_path, sys_name);
	evt_dir = opendir(evt_path);

	if (!evt_dir) {
		perror("Can't open event dir");
		return EVT_FAILED;
	}

	while ((evt_ent = readdir(evt_dir))) {
		char event_opt[MAX_EVOPT_LEN + 1];
		int len;

		if (!strcmp(evt_ent->d_name, ".")
		    || !strcmp(evt_ent->d_name, "..")
		    || !strcmp(evt_ent->d_name, "enable")
		    || !strcmp(evt_ent->d_name, "filter"))
			continue;

		if (!strglobmatch(evt_ent->d_name, evt_exp))
			continue;

		len = snprintf(event_opt, MAX_EVOPT_LEN, "%s:%s%s%s", sys_name,
			       evt_ent->d_name, flags ? ":" : "",
			       flags ?: "");
		if (len < 0)
			return EVT_FAILED;

		if (parse_events(opt, event_opt, 0))
			return EVT_FAILED;
	}

	return EVT_HANDLED_ALL;
}

static enum event_result
parse_tracepoint_event(const struct option *opt, const char **strp,
		       struct perf_event_attr *attr)
{
	const char *evt_name;
	char *flags = NULL, *comma_loc;
	char sys_name[MAX_EVENT_LENGTH];
	unsigned int sys_length, evt_length;

	if (debugfs_valid_mountpoint(debugfs_path))
		return 0;

	evt_name = strchr(*strp, ':');
	if (!evt_name)
		return EVT_FAILED;

	sys_length = evt_name - *strp;
	if (sys_length >= MAX_EVENT_LENGTH)
		return 0;

	strncpy(sys_name, *strp, sys_length);
	sys_name[sys_length] = '\0';
	evt_name = evt_name + 1;

	comma_loc = strchr(evt_name, ',');
	if (comma_loc) {
		/* take the event name up to the comma */
		evt_name = strndup(evt_name, comma_loc - evt_name);
	}
	flags = strchr(evt_name, ':');
	if (flags) {
		/* split it out: */
		evt_name = strndup(evt_name, flags - evt_name);
		flags++;
	}

	evt_length = strlen(evt_name);
	if (evt_length >= MAX_EVENT_LENGTH)
		return EVT_FAILED;
	if (strpbrk(evt_name, "*?")) {
		*strp += strlen(sys_name) + evt_length + 1; /* 1 == the ':' */
		return parse_multiple_tracepoint_event(opt, sys_name, evt_name,
						       flags);
	} else {
		return parse_single_tracepoint_event(sys_name, evt_name,
						     evt_length, attr, strp);
	}
}

static enum event_result
parse_breakpoint_type(const char *type, const char **strp,
		      struct perf_event_attr *attr)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (!type[i])
			break;

		switch (type[i]) {
		case 'r':
			attr->bp_type |= HW_BREAKPOINT_R;
			break;
		case 'w':
			attr->bp_type |= HW_BREAKPOINT_W;
			break;
		case 'x':
			attr->bp_type |= HW_BREAKPOINT_X;
			break;
		default:
			return EVT_FAILED;
		}
	}
	if (!attr->bp_type) /* Default */
		attr->bp_type = HW_BREAKPOINT_R | HW_BREAKPOINT_W;

	*strp = type + i;

	return EVT_HANDLED;
}

static enum event_result
parse_breakpoint_event(const char **strp, struct perf_event_attr *attr)
{
	const char *target;
	const char *type;
	char *endaddr;
	u64 addr;
	enum event_result err;

	target = strchr(*strp, ':');
	if (!target)
		return EVT_FAILED;

	if (strncmp(*strp, "mem", target - *strp) != 0)
		return EVT_FAILED;

	target++;

	addr = strtoull(target, &endaddr, 0);
	if (target == endaddr)
		return EVT_FAILED;

	attr->bp_addr = addr;
	*strp = endaddr;

	type = strchr(target, ':');

	/* If no type is defined, just rw as default */
	if (!type) {
		attr->bp_type = HW_BREAKPOINT_R | HW_BREAKPOINT_W;
	} else {
		err = parse_breakpoint_type(++type, strp, attr);
		if (err == EVT_FAILED)
			return EVT_FAILED;
	}

	/*
	 * We should find a nice way to override the access length
	 * Provide some defaults for now
	 */
	if (attr->bp_type == HW_BREAKPOINT_X)
		attr->bp_len = sizeof(long);
	else
		attr->bp_len = HW_BREAKPOINT_LEN_4;

	attr->type = PERF_TYPE_BREAKPOINT;

	return EVT_HANDLED;
}

static int check_events(const char *str, unsigned int i)
{
	int n;

	n = strlen(event_symbols[i].symbol);
	if (!strncasecmp(str, event_symbols[i].symbol, n))
		return n;

	n = strlen(event_symbols[i].alias);
	if (n) {
		if (!strncasecmp(str, event_symbols[i].alias, n))
			return n;
	}

	return 0;
}

static enum event_result
parse_symbolic_event(const char **strp, struct perf_event_attr *attr)
{
	const char *str = *strp;
	unsigned int i;
	int n;

	for (i = 0; i < ARRAY_SIZE(event_symbols); i++) {
		n = check_events(str, i);
		if (n > 0) {
			attr->type = event_symbols[i].type;
			attr->config = event_symbols[i].config;
			*strp = str + n;
			return EVT_HANDLED;
		}
	}
	return EVT_FAILED;
}

static enum event_result
parse_raw_event(const char **strp, struct perf_event_attr *attr)
{
	const char *str = *strp;
	u64 config;
	int n;

	if (*str != 'r')
		return EVT_FAILED;
	n = hex2u64(str + 1, &config);
	if (n > 0) {
		*strp = str + n + 1;
		attr->type = PERF_TYPE_RAW;
		attr->config = config;
		return EVT_HANDLED;
	}
	return EVT_FAILED;
}

static enum event_result
parse_numeric_event(const char **strp, struct perf_event_attr *attr)
{
	const char *str = *strp;
	char *endp;
	unsigned long type;
	u64 config;

	type = strtoul(str, &endp, 0);
	if (endp > str && type < PERF_TYPE_MAX && *endp == ':') {
		str = endp + 1;
		config = strtoul(str, &endp, 0);
		if (endp > str) {
			attr->type = type;
			attr->config = config;
			*strp = endp;
			return EVT_HANDLED;
		}
	}
	return EVT_FAILED;
}

static int
parse_event_modifier(const char **strp, struct perf_event_attr *attr)
{
	const char *str = *strp;
	int exclude = 0;
	int eu = 0, ek = 0, eh = 0, precise = 0;

	if (!*str)
		return 0;

	if (*str++ != ':')
		return -1;

	while (*str) {
		if (*str == 'u') {
			if (!exclude)
				exclude = eu = ek = eh = 1;
			eu = 0;
		} else if (*str == 'k') {
			if (!exclude)
				exclude = eu = ek = eh = 1;
			ek = 0;
		} else if (*str == 'h') {
			if (!exclude)
				exclude = eu = ek = eh = 1;
			eh = 0;
		} else if (*str == 'p') {
			precise++;
		} else
			break;

		++str;
	}
	if (str < *strp + 2)
		return -1;

	*strp = str;

	attr->exclude_user   = eu;
	attr->exclude_kernel = ek;
	attr->exclude_hv     = eh;
	attr->precise_ip     = precise;

	return 0;
}

/*
 * Each event can have multiple symbolic names.
 * Symbolic names are (almost) exactly matched.
 */
static enum event_result
parse_event_symbols(const struct option *opt, const char **str,
		    struct perf_event_attr *attr)
{
	enum event_result ret;

	ret = parse_tracepoint_event(opt, str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	ret = parse_raw_event(str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	ret = parse_numeric_event(str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	ret = parse_symbolic_event(str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	ret = parse_generic_hw_event(str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	ret = parse_breakpoint_event(str, attr);
	if (ret != EVT_FAILED)
		goto modifier;

	fprintf(stderr, "invalid or unsupported event: '%s'\n", *str);
	fprintf(stderr, "Run 'perf list' for a list of valid events\n");
	return EVT_FAILED;

modifier:
	if (parse_event_modifier(str, attr) < 0) {
		fprintf(stderr, "invalid event modifier: '%s'\n", *str);
		fprintf(stderr, "Run 'perf list' for a list of valid events and modifiers\n");

		return EVT_FAILED;
	}

	return ret;
}

int parse_events(const struct option *opt, const char *str, int unset __used)
{
	struct perf_evlist *evlist = *(struct perf_evlist **)opt->value;
	struct perf_event_attr attr;
	enum event_result ret;
	const char *ostr;

	for (;;) {
		ostr = str;
		memset(&attr, 0, sizeof(attr));
		ret = parse_event_symbols(opt, &str, &attr);
		if (ret == EVT_FAILED)
			return -1;

		if (!(*str == 0 || *str == ',' || isspace(*str)))
			return -1;

		if (ret != EVT_HANDLED_ALL) {
			struct perf_evsel *evsel;
			evsel = perf_evsel__new(&attr, evlist->nr_entries);
			if (evsel == NULL)
				return -1;
			perf_evlist__add(evlist, evsel);

			evsel->name = calloc(str - ostr + 1, 1);
			if (!evsel->name)
				return -1;
			strncpy(evsel->name, ostr, str - ostr);
		}

		if (*str == 0)
			break;
		if (*str == ',')
			++str;
		while (isspace(*str))
			++str;
	}

	return 0;
}

int parse_filter(const struct option *opt, const char *str,
		 int unset __used)
{
	struct perf_evlist *evlist = *(struct perf_evlist **)opt->value;
	struct perf_evsel *last = NULL;

	if (evlist->nr_entries > 0)
		last = list_entry(evlist->entries.prev, struct perf_evsel, node);

	if (last == NULL || last->attr.type != PERF_TYPE_TRACEPOINT) {
		fprintf(stderr,
			"-F option should follow a -e tracepoint option\n");
		return -1;
	}

	last->filter = strdup(str);
	if (last->filter == NULL) {
		fprintf(stderr, "not enough memory to hold filter string\n");
		return -1;
	}

	return 0;
}

static const char * const event_type_descriptors[] = {
	"Hardware event",
	"Software event",
	"Tracepoint event",
	"Hardware cache event",
	"Raw hardware event descriptor",
	"Hardware breakpoint",
};

/*
 * Print the events from <debugfs_mount_point>/tracing/events
 */

void print_tracepoint_events(const char *subsys_glob, const char *event_glob)
{
	DIR *sys_dir, *evt_dir;
	struct dirent *sys_next, *evt_next, sys_dirent, evt_dirent;
	char evt_path[MAXPATHLEN];
	char dir_path[MAXPATHLEN];

	if (debugfs_valid_mountpoint(debugfs_path))
		return;

	sys_dir = opendir(debugfs_path);
	if (!sys_dir)
		return;

	for_each_subsystem(sys_dir, sys_dirent, sys_next) {
		if (subsys_glob != NULL && 
		    !strglobmatch(sys_dirent.d_name, subsys_glob))
			continue;

		snprintf(dir_path, MAXPATHLEN, "%s/%s", debugfs_path,
			 sys_dirent.d_name);
		evt_dir = opendir(dir_path);
		if (!evt_dir)
			continue;

		for_each_event(sys_dirent, evt_dir, evt_dirent, evt_next) {
			if (event_glob != NULL && 
			    !strglobmatch(evt_dirent.d_name, event_glob))
				continue;

			snprintf(evt_path, MAXPATHLEN, "%s:%s",
				 sys_dirent.d_name, evt_dirent.d_name);
			printf("  %-42s [%s]\n", evt_path,
				event_type_descriptors[PERF_TYPE_TRACEPOINT]);
		}
		closedir(evt_dir);
	}
	closedir(sys_dir);
}

/*
 * Check whether event is in <debugfs_mount_point>/tracing/events
 */

int is_valid_tracepoint(const char *event_string)
{
	DIR *sys_dir, *evt_dir;
	struct dirent *sys_next, *evt_next, sys_dirent, evt_dirent;
	char evt_path[MAXPATHLEN];
	char dir_path[MAXPATHLEN];

	if (debugfs_valid_mountpoint(debugfs_path))
		return 0;

	sys_dir = opendir(debugfs_path);
	if (!sys_dir)
		return 0;

	for_each_subsystem(sys_dir, sys_dirent, sys_next) {

		snprintf(dir_path, MAXPATHLEN, "%s/%s", debugfs_path,
			 sys_dirent.d_name);
		evt_dir = opendir(dir_path);
		if (!evt_dir)
			continue;

		for_each_event(sys_dirent, evt_dir, evt_dirent, evt_next) {
			snprintf(evt_path, MAXPATHLEN, "%s:%s",
				 sys_dirent.d_name, evt_dirent.d_name);
			if (!strcmp(evt_path, event_string)) {
				closedir(evt_dir);
				closedir(sys_dir);
				return 1;
			}
		}
		closedir(evt_dir);
	}
	closedir(sys_dir);
	return 0;
}

void print_events_type(u8 type)
{
	struct event_symbol *syms = event_symbols;
	unsigned int i;
	char name[64];

	for (i = 0; i < ARRAY_SIZE(event_symbols); i++, syms++) {
		if (type != syms->type)
			continue;

		if (strlen(syms->alias))
			snprintf(name, sizeof(name),  "%s OR %s",
				 syms->symbol, syms->alias);
		else
			snprintf(name, sizeof(name), "%s", syms->symbol);

		printf("  %-42s [%s]\n", name,
			event_type_descriptors[type]);
	}
}

int print_hwcache_events(const char *event_glob)
{
	unsigned int type, op, i, printed = 0;

	for (type = 0; type < PERF_COUNT_HW_CACHE_MAX; type++) {
		for (op = 0; op < PERF_COUNT_HW_CACHE_OP_MAX; op++) {
			/* skip invalid cache type */
			if (!is_cache_op_valid(type, op))
				continue;

			for (i = 0; i < PERF_COUNT_HW_CACHE_RESULT_MAX; i++) {
				char *name = event_cache_name(type, op, i);

				if (event_glob != NULL && 
				    !strglobmatch(name, event_glob))
					continue;

				printf("  %-42s [%s]\n", name,
					event_type_descriptors[PERF_TYPE_HW_CACHE]);
				++printed;
			}
		}
	}

	return printed;
}

/*
 * Print the help text for the event symbols:
 */
void print_events(const char *event_glob)
{
	struct event_symbol *syms = event_symbols;
	unsigned int i, type, prev_type = -1, printed = 0, ntypes_printed = 0;
	char name[40];

	printf("\n");
	printf("List of pre-defined events (to be used in -e):\n");

	for (i = 0; i < ARRAY_SIZE(event_symbols); i++, syms++) {
		type = syms->type;

		if (type != prev_type && printed) {
			printf("\n");
			printed = 0;
			ntypes_printed++;
		}

		if (event_glob != NULL && 
		    !(strglobmatch(syms->symbol, event_glob) ||
		      (syms->alias && strglobmatch(syms->alias, event_glob))))
			continue;

		if (strlen(syms->alias))
			sprintf(name, "%s OR %s", syms->symbol, syms->alias);
		else
			strcpy(name, syms->symbol);
		printf("  %-42s [%s]\n", name,
			event_type_descriptors[type]);

		prev_type = type;
		++printed;
	}

	if (ntypes_printed) {
		printed = 0;
		printf("\n");
	}
	print_hwcache_events(event_glob);

	if (event_glob != NULL)
		return;

	printf("\n");
	printf("  %-42s [%s]\n",
		"rNNN (see 'perf list --help' on how to encode it)",
	       event_type_descriptors[PERF_TYPE_RAW]);
	printf("\n");

	printf("  %-42s [%s]\n",
			"mem:<addr>[:access]",
			event_type_descriptors[PERF_TYPE_BREAKPOINT]);
	printf("\n");

	print_tracepoint_events(NULL, NULL);

	exit(129);
}
