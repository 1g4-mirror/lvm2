// SPDX-License-Identifier: GPL-2.0+
/*
 * devloc - locate the physical device and offset backing a file.
 *
 * Method: an empirical "oracle".  The tool triggers a single direct-I/O
 * read (or a nonce write, with --nonce) through the top-level block
 * device and watches block_rq_issue tracepoint events on the leaf
 * devices below it, reporting which device and sector actually served
 * the I/O.  It performs no dm table parsing and no mapping arithmetic;
 * it only reports observations, and flags ambiguity instead of
 * guessing.
 *
 * Stages:
 *   1. FIEMAP the file to translate file offset -> physical offset on
 *      the block device holding the filesystem (the "top" device).
 *   2. Discover leaf devices by walking the slaves tree in sysfs from
 *      the top device (topology knowledge only, no mapping knowledge).
 *   3. For each probe block: drain the tracepoint buffer, issue one
 *      O_DIRECT read through the top device, collect events, match
 *      them against the leaf set, optionally verify by comparing the
 *      bytes read from the top device with the bytes at the candidate
 *      leaf location.
 *
 * Exit status:
 *   0  every probe resolved to exactly one device+offset
 *   1  usage or I/O error
 *   2  one or more probes ambiguous or unobserved ("couldn't tell")
 *   3  one or more probes unmappable (hole, delalloc, unwritten)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LEAVES      64
#define MAX_CANDS       32
#define MAX_EVENTS      16384
#define DEFAULT_BS      4096
#define DEFAULT_SAMPLE  1024
#define MAX_DEPTH       16

typedef uint64_t u64;
typedef uint32_t u32;

/* ------------------------------------------------------------------ */
/* options                                                            */
/* ------------------------------------------------------------------ */

struct opts {
	const char *file;
	u64 offset;		/* bytes */
	u64 length;		/* bytes, 0 = one block */
	int dev_mode;
	const char *device;
	u64 dev_sector;		/* 512B sectors, dev_mode only */
	unsigned bs;
	int sample;		/* -1 auto, 0 all, >0 N evenly spaced */
	int probe_all;
	int repeat;
	int nonce;
	int no_restore;
	int force;
	int force_writeback;
	int verify_content;	/* default 1 */
	int quiet_check;
	int strict;
	int passthrough;
	int show_events;
	int json;
	int summary;
	char units;
	int timeout_ms;
	const char *leaves_arg;
	int verbose;
};

static struct opts opts = {
	.bs = DEFAULT_BS,
	.sample = -1,
	.repeat = 1,
	.verify_content = 1,
	.timeout_ms = 2000,
	.units = 'b',
};

static void usage(FILE *out)
{
	fprintf(out,
"Usage: devloc [options] FILE [OFFSET [LENGTH]]\n"
"       devloc [options] --device DEV --sector N\n"
"\n"
"Locate the physical device and offset backing a file (or a raw device\n"
"sector) by triggering I/O and observing which leaf device serves it.\n"
"\n"
"Offsets and lengths accept K/M/G/T suffixes (binary) and 's' for\n"
"512-byte sectors.  LENGTH defaults to one block.\n"
"\n"
"Input scope:\n"
"  --device DEV --sector N   probe a raw device/sector directly\n"
"  --block-size SIZE         probe granularity (default 4KiB)\n"
"  --sample N                probe N evenly spaced blocks in the range\n"
"  --probe-all               probe every block (no sampling)\n"
"  --repeat N                repeat each probe N times (path/mirror spread)\n"
"\n"
"Triggering:\n"
"  (default)                 read-only: one O_DIRECT read per probe\n"
"  --nonce                   write a unique nonce, observe, restore\n"
"                            (needs --force if the device holds a live fs)\n"
"  --no-restore              leave the nonce in place (test/scratch only)\n"
"  --passthrough             observe events only, trigger nothing\n"
"\n"
"Observation:\n"
"  --leaves LIST             comma list of maj:min or device paths to\n"
"                            match events against (default: auto-detect\n"
"                            via sysfs slaves tree)\n"
"  --timeout MSECS           per-probe event wait (default 2000)\n"
"  --events                  dump raw observed events\n"
"  --quiet-check             abort if leaf devices show background I/O\n"
"\n"
"Output:\n"
"  --json                    machine-readable output (offsets in bytes)\n"
"  --summary                 per-device histogram of probed blocks\n"
"  --units b|s|k|m|g         display units (default bytes)\n"
"  --force-writeback         pass FIEMAP_FLAG_SYNC (flush delalloc)\n"
"  --no-verify-content       skip content comparison\n"
"  --strict                  abort at the first ambiguous probe\n"
"  -v                        be verbose\n");
}

/* ------------------------------------------------------------------ */
/* small utilities                                                    */
/* ------------------------------------------------------------------ */

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "devloc: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void *xmalloc(size_t sz)
{
	void *p = malloc(sz);
	if (!p)
		die("out of memory");
	return p;
}

static u64 parse_size(const char *s)
{
	char *end;
	unsigned long long v = strtoull(s, &end, 0);
	if (end == s || v == ULLONG_MAX)
		die("bad size: %s", s);
	if (*end) {
		char u = tolower((unsigned char)*end);
		const char *rest = end + 1;
		if (u == 's') {
			v *= 512;
		} else if (u == 'k') {
			v *= 1024;
		} else if (u == 'm') {
			v *= 1024 * 1024;
		} else if (u == 'g') {
			v *= 1024ULL * 1024 * 1024;
		} else if (u == 't') {
			v *= 1024ULL * 1024 * 1024 * 1024;
		} else {
			die("bad size suffix in %s", s);
		}
		if (*rest && strcmp(rest, "b") && strcmp(rest, "ib"))
			die("bad size suffix in %s", s);
	}
	return (u64)v;
}

static void fmt_units(u64 bytes, char units, char *buf, size_t sz)
{
	switch (units) {
	case 's':
		snprintf(buf, sz, "%llu", (unsigned long long)(bytes / 512));
		break;
	case 'k':
	case 'm':
	case 'g': {
		unsigned div = units == 'k' ? 1 : units == 'm' ? 2 : 3;
		u64 scale = 1ULL << (10 * div);
		double v = (double)bytes / scale;
		if (v == (u64)v)
			snprintf(buf, sz, "%llu", (unsigned long long)v);
		else
			snprintf(buf, sz, "%.2f", v);
		break;
	}
	default:
		snprintf(buf, sz, "%llu", (unsigned long long)bytes);
		break;
	}
}

/* kernel-encoded dev_t as it appears in block tracepoints */
static u32 kdev_encode(dev_t d)
{
	return ((u32)major(d) << 20) | (u32)minor(d);
}

static dev_t kdev_decode(u32 v)
{
	return makedev(v >> 20, v & ((1u << 20) - 1));
}

static void unescape_mount(char *s)
{
	char *o = s;
	for (char *p = s; *p;) {
		if (p[0] == '\\' && isdigit((unsigned char)p[1]) &&
		    isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3])) {
			*o++ = (p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0');
			p += 4;
		} else {
			*o++ = *p++;
		}
	}
	*o = 0;
}

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                      */
/* ------------------------------------------------------------------ */

static int read_first_line(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sz - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return 0;
}

static int sysfs_path(dev_t dev, char *buf, size_t sz)
{
	return snprintf(buf, sz, "/sys/dev/block/%u:%u", major(dev), minor(dev)) >= (int)sz ? -1 : 0;
}

static int sysfs_attr_u64(dev_t dev, const char *attr, u64 *out)
{
	char path[PATH_MAX], buf[64];
	if (sysfs_path(dev, path, sizeof(path)) < 0)
		return -1;
	strncat(path, attr, sizeof(path) - strlen(path) - 1);
	if (read_first_line(path, buf, sizeof(buf)) < 0)
		return -1;
	*out = strtoull(buf, NULL, 10);
	return 0;
}

static int parse_devt(const char *s, dev_t *out)
{
	unsigned int ma, mi;
	if (sscanf(s, "%u:%u", &ma, &mi) == 2) {
		*out = makedev(ma, mi);
		return 0;
	}
	return -1;
}

static int dev_from_sysfs_dir(const char *dir, dev_t *out)
{
	char path[PATH_MAX], buf[64];
	snprintf(path, sizeof(path), "%s/dev", dir);
	if (read_first_line(path, buf, sizeof(buf)) < 0)
		return -1;
	return parse_devt(buf, out);
}

/* human-readable device name: dm name if it has one, else sysfs basename */
static int dev_display_name(dev_t dev, char *buf, size_t sz)
{
	char path[PATH_MAX], tmp[PATH_MAX];

	if (sysfs_path(dev, path, sizeof(path)) < 0)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s/dm/name", path);
	if (read_first_line(tmp, buf, sz) == 0 && buf[0])
		return 0;

	if (!realpath(path, tmp))
		return -1;
	const char *b = strrchr(tmp, '/');
	snprintf(buf, sz, "%s", b ? b + 1 : tmp);
	return 0;
}

/* if dev is a partition, return the whole-disk devt it belongs to */
static int parent_disk(dev_t dev, dev_t *out)
{
	u64 start;
	char path[PATH_MAX], real[PATH_MAX];

	if (sysfs_attr_u64(dev, "/start", &start) < 0)
		return -1;		/* not a partition */
	if (sysfs_path(dev, path, sizeof(path)) < 0)
		return -1;
	strncat(path, "/..", sizeof(path) - strlen(path) - 1);
	if (!realpath(path, real))
		return -1;
	return dev_from_sysfs_dir(real, out);
}

/* open a device node for devt, trying the usual places */
static int open_dev_node(dev_t dev, int flags)
{
	char path[PATH_MAX], name[128];

	snprintf(path, sizeof(path), "/dev/block/%u:%u", major(dev), minor(dev));
	int fd = open(path, flags | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	if (dev_display_name(dev, name, sizeof(name)) == 0) {
		snprintf(path, sizeof(path), "/dev/mapper/%s", name);
		fd = open(path, flags | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		snprintf(path, sizeof(path), "/dev/%s", name);
		fd = open(path, flags | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}
	errno = ENOENT;
	return -1;
}

/* ------------------------------------------------------------------ */
/* top device                                                         */
/* ------------------------------------------------------------------ */

struct top_info {
	dev_t dev;
	char name[128];
	char display[256];
	char mountpoint[PATH_MAX];
	char fstype[32];
	u64 part_start;		/* sectors, if top is a partition */
};

static int find_mount(dev_t dev, char *mp, size_t mpsz, char *fstype, size_t fssz)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[8192];

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned int id, parent;
		int ma, mi;
		char root[4096], mnt[4096];

		if (sscanf(line, "%u %u %d:%d %4095s %4095s",
			   &id, &parent, &ma, &mi, root, mnt) != 6)
			continue;
		if (makedev(ma, mi) != dev)
			continue;
		unescape_mount(mnt);
		snprintf(mp, mpsz, "%s", mnt);
		/* " - fstype source superopts" follows the optional fields */
		char *dash = strstr(line, " - ");
		if (dash && sscanf(dash + 3, "%31s", fstype) == 1) {
			fclose(f);
			return 0;
		}
		(void)fssz;
	}
	fclose(f);
	return -1;
}

static void resolve_top_file(const char *file, struct top_info *t)
{
	struct stat st;

	if (stat(file, &st) < 0)
		die("stat %s: %s", file, strerror(errno));
	t->dev = st.st_dev;
	if (dev_display_name(t->dev, t->name, sizeof(t->name)) < 0)
		die("cannot resolve device %u:%u for %s (is /sys mounted?)",
		    major(t->dev), minor(t->dev), file);
	if (find_mount(t->dev, t->mountpoint, sizeof(t->mountpoint),
		       t->fstype, sizeof(t->fstype)) < 0) {
		snprintf(t->mountpoint, sizeof(t->mountpoint), "(not mounted)");
		snprintf(t->fstype, sizeof(t->fstype), "?");
	}
	sysfs_attr_u64(t->dev, "/start", &t->part_start);
	snprintf(t->display, sizeof(t->display), "%s on %s (%s) dev %u:%u %s",
		 file, t->mountpoint, t->fstype, major(t->dev), minor(t->dev),
		 t->name);
}

static void resolve_top_device(const char *device, struct top_info *t)
{
	struct stat st;

	if (stat(device, &st) < 0)
		die("stat %s: %s", device, strerror(errno));
	if (!S_ISBLK(st.st_mode))
		die("%s is not a block device", device);
	t->dev = st.st_rdev;
	if (dev_display_name(t->dev, t->name, sizeof(t->name)) < 0)
		die("cannot resolve device %u:%u", major(t->dev), minor(t->dev));
	sysfs_attr_u64(t->dev, "/start", &t->part_start);
	snprintf(t->display, sizeof(t->display), "device %s (%s) dev %u:%u",
		 device, t->name, major(t->dev), minor(t->dev));
}

/* ------------------------------------------------------------------ */
/* leaves                                                             */
/* ------------------------------------------------------------------ */

/*
 * A leaf is the bottom of the top device's slaves tree: the device we
 * expect to see block_rq_issue events on.
 *
 * Sector convention: event records carry the devt of the whole disk
 * (rq->q->disk) and a whole-disk-relative sector — bios targeting a
 * partition are remapped by blk_partition_remap() before reaching the
 * disk queue.  So candidates are stored with disk-relative sectors and
 * content checks read the whole disk; partition-relative offsets are
 * derived for display.
 */
struct leaf {
	dev_t node;		/* leaf node (partition or disk) from slaves tree */
	dev_t disk;		/* whole disk owning the queue (= event dev) */
	u64 part_start;		/* sectors of node within disk (0 if node is disk) */
	u64 part_size;		/* sectors of node (for containment) */
	char name[128];
	char diskname[128];
};

static struct leaf leaves[MAX_LEAVES];
static int nleaves;

static void add_leaf(dev_t node)
{
	for (int i = 0; i < nleaves; i++)
		if (leaves[i].node == node)
			return;
	if (nleaves >= MAX_LEAVES)
		die("too many leaves (> %d)", MAX_LEAVES);

	struct leaf *l = &leaves[nleaves++];
	memset(l, 0, sizeof(*l));
	l->node = node;
	l->disk = node;

	dev_t pd;
	if (parent_disk(node, &pd) == 0) {
		l->disk = pd;
		sysfs_attr_u64(node, "/start", &l->part_start);
		dev_display_name(pd, l->diskname, sizeof(l->diskname));
	}
	sysfs_attr_u64(node, "/size", &l->part_size);
	dev_display_name(node, l->name, sizeof(l->name));
}

static void discover_leaves_r(dev_t dev, int depth)
{
	char path[PATH_MAX], slave_path[PATH_MAX];

	if (depth > MAX_DEPTH)
		return;
	if (sysfs_path(dev, path, sizeof(path)) < 0)
		return;
	snprintf(slave_path, sizeof(slave_path), "%s/slaves", path);

	DIR *d = opendir(slave_path);
	if (!d) {
		add_leaf(dev);
		return;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		dev_t sdev;
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", slave_path, de->d_name);
		if (dev_from_sysfs_dir(p, &sdev) < 0)
			continue;
		n++;
		discover_leaves_r(sdev, depth + 1);
	}
	closedir(d);
	if (!n)
		add_leaf(dev);
}

static void parse_leaves_arg(void)
{
	char *copy = strdup(opts.leaves_arg);
	char *tok, *save = NULL;

	if (!copy)
		die("out of memory");
	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		struct stat st;
		if (stat(tok, &st) == 0 && S_ISBLK(st.st_mode)) {
			add_leaf(st.st_rdev);
		} else {
			dev_t d;
			if (parse_devt(tok, &d) < 0)
				die("bad --leaves entry: %s", tok);
			add_leaf(d);
		}
	}
	free(copy);
}

/* ------------------------------------------------------------------ */
/* tracepoint monitor                                                 */
/* ------------------------------------------------------------------ */

struct obs {
	u32 dev;		/* kernel-encoded devt */
	u64 sector;
	u32 nr_sect;
	u32 bytes;
	char rwbs[12];
	char comm[17];
	u64 time_ns;
};

struct tpmon {
	int ncpu;
	struct {
		int fd;
		void *base;			/* mmap'd ring */
		struct perf_event_mmap_page *pc;
		int pages;
		size_t pagesz;
	} cpu[1024];
	int off_dev, off_sector, off_nr, off_bytes, off_rwbs, off_comm;
	int rwbs_len, comm_len;
};

static struct tpmon tpmon;
static struct obs events[MAX_EVENTS];
static int nevents;

static long perf_open(struct perf_event_attr *attr, pid_t pid, int cpu,
		      int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static const char *tracefs_base(void)
{
	static char base[PATH_MAX];
	static int checked;
	const char *cands[] = { "/sys/kernel/tracing", "/sys/kernel/debug/tracing" };

	if (checked)
		return base[0] ? base : NULL;
	checked = 1;
	for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/events/block/block_rq_issue/id", cands[i]);
		if (access(p, R_OK) == 0) {
			snprintf(base, sizeof(base), "%s", cands[i]);
			return base;
		}
	}
	/* last resort: find a tracefs mount in /proc/mounts */
	FILE *f = fopen("/proc/mounts", "r");
	char line[1024];
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char devn[256], mp[512], type[64];
			if (sscanf(line, "%255s %511s %63s", devn, mp, type) != 3)
				continue;
			if (strcmp(type, "tracefs"))
				continue;
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/events/block/block_rq_issue/id", mp);
			if (access(p, R_OK) == 0) {
				snprintf(base, sizeof(base), "%s", mp);
				fclose(f);
				return base;
			}
		}
		fclose(f);
	}
	return NULL;
}

/* parse the tracepoint "format" file for field offsets */
static int parse_tp_format(const char *dir)
{
	char path[PATH_MAX], buf[65536];
	FILE *f;

	snprintf(path, sizeof(path), "%s/events/block/block_rq_issue/format", dir);
	f = fopen(path, "r");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;

	tpmon.off_dev = tpmon.off_sector = tpmon.off_nr = -1;
	tpmon.off_bytes = tpmon.off_rwbs = tpmon.off_comm = -1;

	char *line, *next;
	for (line = buf; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		if (strncmp(line, "field:", 6))
			continue;
		char work[512];
		snprintf(work, sizeof(work), "%s", line);
		char *save = NULL;
		char *t1 = strtok_r(work, ";", &save);
		char *t2 = strtok_r(NULL, ";", &save);
		char *t3 = strtok_r(NULL, ";", &save);
		if (!t1 || !t2 || !t3)
			continue;
		/* "field:<type words...> <name>[N]" — name is the last token */
		char *nm = t1 + strlen(t1);
		while (nm > t1 && nm[-1] != ' ' && nm[-1] != '\t')
			nm--;
		if (nm == t1)
			continue;
		char *br = strchr(nm, '[');
		if (br)
			*br = 0;
		const char *name = nm;
		int off = -1, size = -1;
		if (sscanf(t2, " offset:%d", &off) != 1)
			continue;
		if (sscanf(t3, " size:%d", &size) != 1)
			continue;
		if (!strcmp(name, "dev"))
			tpmon.off_dev = off;
		else if (!strcmp(name, "sector"))
			tpmon.off_sector = off;
		else if (!strcmp(name, "nr_sector"))
			tpmon.off_nr = off;
		else if (!strcmp(name, "bytes"))
			tpmon.off_bytes = off;
		else if (!strcmp(name, "rwbs")) {
			tpmon.off_rwbs = off;
			tpmon.rwbs_len = size;
		} else if (!strcmp(name, "comm")) {
			tpmon.off_comm = off;
			tpmon.comm_len = size;
		}
	}
	return (tpmon.off_dev >= 0 && tpmon.off_sector >= 0) ? 0 : -1;
}

static void tpmon_open_(void)
{
	const char *base = tracefs_base();
	if (!base)
		die("tracefs not mounted with tracepoints; try: "
		    "mount -t tracefs tracefs /sys/kernel/tracing");
	if (parse_tp_format(base) < 0)
		die("cannot parse block_rq_issue format from %s", base);

	char path[PATH_MAX], buf[64];
	snprintf(path, sizeof(path), "%s/events/block/block_rq_issue/id", base);
	if (read_first_line(path, buf, sizeof(buf)) < 0)
		die("cannot read tracepoint id");
	int id = atoi(buf);

	struct perf_event_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_TRACEPOINT;
	attr.config = id;
	attr.sample_period = 1;
	attr.sample_type = PERF_SAMPLE_RAW | PERF_SAMPLE_TIME;

	int ncpu = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpu > 1024)
		ncpu = 1024;
	int opened = 0;
	for (int cpu = 0; cpu < ncpu; cpu++) {
		int fd = perf_open(&attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
		if (fd < 0) {
			if (cpu == 0 && (errno == EACCES || errno == EPERM))
				die("cannot open tracepoint (run as root, or check "
				    "/proc/sys/kernel/perf_event_paranoid): %s",
				    strerror(errno));
			continue;	/* offline cpu */
		}
		int pages = 64;
		size_t pagesz = sysconf(_SC_PAGESIZE);
		size_t len = (size_t)(1 + pages) * pagesz;
		void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (base == MAP_FAILED) {
			close(fd);
			continue;
		}
		tpmon.cpu[opened].fd = fd;
		tpmon.cpu[opened].base = base;
		tpmon.cpu[opened].pc = base;
		tpmon.cpu[opened].pages = pages;
		tpmon.cpu[opened].pagesz = pagesz;
		opened++;
	}
	if (!opened)
		die("no usable perf event fds");
	tpmon.ncpu = opened;
}

static u64 rec_u64(const void *rec, int off, int size)
{
	u64 v = 0;
	if (off < 0 || size <= 0 || size > 8)
		return 0;
	memcpy(&v, (const char *)rec + off, size);
	return v;
}

/* parse one record (perf_event_header at start) into events[] */
static void parse_record(char *rec)
{
	struct perf_event_header *h = (void *)rec;

	if (h->type != PERF_RECORD_SAMPLE)
		return;
	char *s = rec + sizeof(*h);
	u64 time = *(u64 *)s;
	s += sizeof(u64);
	u32 raw_size = *(u32 *)s;
	s += sizeof(u32);
	void *raw = s;
	if (sizeof(*h) + sizeof(u64) + sizeof(u32) + raw_size > h->size)
		return;
	if (nevents >= MAX_EVENTS)
		return;

	struct obs *o = &events[nevents++];
	o->dev = (u32)rec_u64(raw, tpmon.off_dev, 4);
	o->sector = rec_u64(raw, tpmon.off_sector, 8);
	o->nr_sect = (u32)rec_u64(raw, tpmon.off_nr, 4);
	o->bytes = (u32)rec_u64(raw, tpmon.off_bytes, 4);
	memset(o->rwbs, 0, sizeof(o->rwbs));
	if (tpmon.off_rwbs >= 0) {
		int l = tpmon.rwbs_len;
		if (l > (int)sizeof(o->rwbs) - 1)
			l = sizeof(o->rwbs) - 1;
		memcpy(o->rwbs, (char *)raw + tpmon.off_rwbs, l);
	}
	memset(o->comm, 0, sizeof(o->comm));
	if (tpmon.off_comm >= 0) {
		int l = tpmon.comm_len;
		if (l > (int)sizeof(o->comm) - 1)
			l = sizeof(o->comm) - 1;
		memcpy(o->comm, (char *)raw + tpmon.off_comm, l);
	}
	o->time_ns = time;
}

/* consume all records from one cpu's ring buffer */
static void tpmon_read_cpu(int c)
{
	struct perf_event_mmap_page *pc = tpmon.cpu[c].pc;
	size_t datasz = (size_t)tpmon.cpu[c].pages * tpmon.cpu[c].pagesz;
	size_t pagesz = tpmon.cpu[c].pagesz;
	char *data = (char *)tpmon.cpu[c].base + pagesz;

	u64 head = __atomic_load_n(&pc->data_head, __ATOMIC_ACQUIRE);
	u64 tail = pc->data_tail;

	while (tail != head) {
		size_t off = tail % datasz;
		char *rec = data + off;
		struct perf_event_header h;

		memcpy(&h, rec, sizeof(h));	/* may straddle the wrap point */
		if (h.size < 8)
			break;			/* corrupt; stop */
		if (off + h.size > datasz) {
			/* record wraps around the end: copy it out first */
			char tmp[4096];
			if (h.size > (int)sizeof(tmp))
				break;
			size_t first = datasz - off;
			memcpy(tmp, rec, first);
			memcpy(tmp + first, data, h.size - first);
			parse_record(tmp);
		} else {
			parse_record(rec);
		}
		tail += h.size;
		if (nevents >= MAX_EVENTS)
			break;
	}
	__atomic_store_n(&pc->data_tail, tail, __ATOMIC_RELEASE);
}

static void tpmon_read_all(void)
{
	for (int i = 0; i < tpmon.ncpu; i++)
		tpmon_read_cpu(i);
}

static void tpmon_drain(void)
{
	/* consume everything currently pending */
	for (int i = 0; i < tpmon.ncpu; i++) {
		struct perf_event_mmap_page *pc = tpmon.cpu[i].pc;
		u64 head = __atomic_load_n(&pc->data_head, __ATOMIC_ACQUIRE);
		__atomic_store_n(&pc->data_tail, head, __ATOMIC_RELEASE);
	}
}

static void collect_for(int ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	u64 deadline = ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL + ms;

	for (;;) {
		tpmon_read_all();
		clock_gettime(CLOCK_MONOTONIC, &ts);
		u64 now = ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
		if (now >= deadline)
			break;
		usleep((deadline - now) > 10 ? 10000 : 1000);
	}
}

/* ------------------------------------------------------------------ */
/* FIEMAP                                                             */
/* ------------------------------------------------------------------ */

struct fext {
	u64 logical;
	u64 physical;
	u64 length;
	u32 flags;
};

static int fext_cmp(const void *a, const void *b)
{
	const struct fext *x = a, *y = b;
	return x->logical < y->logical ? -1 : x->logical > y->logical ? 1 : 0;
}

static int fiemap_collect(int fd, u64 start, u64 end, struct fext **out, int *nout)
{
	int cap = 256, n = 0;
	struct fext *exts = xmalloc(cap * sizeof(*exts));
	u32 sync_flag = opts.force_writeback ? FIEMAP_FLAG_SYNC : 0;
	u64 cur = start;

	while (cur < end) {
		size_t bufsz = sizeof(struct fiemap) + 255 * sizeof(struct fiemap_extent);
		struct fiemap *fm = xmalloc(bufsz);
		memset(fm, 0, sizeof(*fm));
		fm->fm_flags = sync_flag;
		fm->fm_start = cur;
		fm->fm_length = end - cur;
		fm->fm_extent_count = 255;

		if (ioctl(fd, FS_IOC_FIEMAP, (unsigned long)fm) < 0) {
			int e = errno;
			free(fm);
			free(exts);
			return -e;
		}
		u32 mapped = fm->fm_mapped_extents;
		int last = fm->fm_flags & FIEMAP_EXTENT_LAST;
		for (u32 i = 0; i < mapped; i++) {
			struct fiemap_extent *fe = &fm->fm_extents[i];
			if (n >= cap) {
				cap *= 2;
				exts = realloc(exts, cap * sizeof(*exts));
				if (!exts)
					die("out of memory");
			}
			exts[n].logical = fe->fe_logical;
			exts[n].physical = fe->fe_physical;
			exts[n].length = fe->fe_length;
			exts[n].flags = fe->fe_flags;
			n++;
			cur = fe->fe_logical + fe->fe_length;
		}
		free(fm);
		if (!mapped || last || cur >= end)
			break;
	}
	qsort(exts, n, sizeof(*exts), fext_cmp);
	*out = exts;
	*nout = n;
	return 0;
}

static struct fext *find_fext(struct fext *exts, int n, u64 off)
{
	for (int i = 0; i < n; i++)
		if (off >= exts[i].logical && off < exts[i].logical + exts[i].length)
			return &exts[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* probes                                                             */
/* ------------------------------------------------------------------ */

struct probe {
	u64 file_off;		/* bytes (device mode: raw offset) */
	u64 fs_off;		/* bytes on the top device */
	u64 len;
	u32 eflags;
	int unmappable;
	const char *reason;
};

static struct probe *probes;
static int nprobes;

static void build_probes_file(int fd, u64 fsize)
{
	u64 start = (opts.offset / opts.bs) * opts.bs;
	u64 end = opts.offset + (opts.length ? opts.length : opts.bs);
	if (end > fsize) {
		fprintf(stderr, "devloc: warning: range clamped to file size %llu\n",
			(unsigned long long)fsize);
		end = fsize;
	}
	if (start >= end)
		die("offset beyond end of file");

	u64 total = (end - start + opts.bs - 1) / opts.bs;
	u64 step = opts.bs;
	u64 nprobe = total;

	if (opts.probe_all || opts.sample == 0) {
		nprobe = total;
	} else if (opts.sample > 0) {
		nprobe = (u64)opts.sample < total ? (u64)opts.sample : total;
	} else if (total > DEFAULT_SAMPLE) {
		nprobe = DEFAULT_SAMPLE;
	}
	if (nprobe < 1)
		nprobe = 1;
	if (total > nprobe)
		step = ((total - 1) / (nprobe - 1 ? nprobe - 1 : 1)) * opts.bs;
	if (step < opts.bs)
		step = opts.bs;
	if (total > nprobe && !opts.json)
		fprintf(stderr, "devloc: note: sampling %llu of %llu blocks "
			"(use --probe-all or --sample N to change)\n",
			(unsigned long long)nprobe, (unsigned long long)total);

	struct fext *exts;
	int nexts;
	int rc = fiemap_collect(fd, start, end, &exts, &nexts);
	if (rc == -ENOTTY || rc == -EOPNOTSUPP)
		die("filesystem does not support FIEMAP");
	if (rc < 0)
		die("FIEMAP: %s", strerror(-rc));

	probes = xmalloc((nprobe + 1) * sizeof(*probes));
	for (u64 i = 0, off = start; i < nprobe && off < end; i++, off += step) {
		struct probe *p = &probes[nprobes++];
		struct fext *fe = find_fext(exts, nexts, off);
		memset(p, 0, sizeof(*p));
		p->file_off = off;
		p->len = opts.bs;
		if (!fe) {
			p->unmappable = 1;
			p->reason = "hole (or delalloc; try --force-writeback)";
			continue;
		}
		p->eflags = fe->flags;
		if (fe->flags & FIEMAP_EXTENT_DELALLOC) {
			p->unmappable = 1;
			p->reason = "delalloc (data not yet on disk; try --force-writeback)";
			continue;
		}
		p->fs_off = fe->physical + (off - fe->logical);
		if (fe->flags & FIEMAP_EXTENT_UNWRITTEN)
			p->reason = "unwritten";
	}
	free(exts);
}

static void build_probe_device(void)
{
	probes = xmalloc(sizeof(*probes));
	memset(probes, 0, sizeof(*probes));
	probes[0].file_off = opts.dev_sector * 512;
	probes[0].fs_off = opts.dev_sector * 512;
	probes[0].len = opts.bs;
	nprobes = 1;
}

/* ------------------------------------------------------------------ */
/* probing                                                            */
/* ------------------------------------------------------------------ */

struct cand {
	dev_t node;		/* leaf node (0 if unknown device) */
	dev_t unknown;		/* decoded devt when not in leaf set */
	u64 sector;		/* relative to node */
	u32 nr;
	int count;
	int verified;		/* 1 match, 0 unchecked, -1 differs */
	int serves_reads;	/* observed serving a read of this block */
	struct leaf *leaf;
};

struct result {
	struct probe *probe;
	int ncand;
	struct cand cands[MAX_CANDS];
	int ambiguous;
	int observed;
	int bg_excluded;
	int copies;		/* content-verified locations (mirror legs) */
	int read_probed;	/* nonce mode: read probe identified servers */
};

static struct result *results;
static int top_odirect = 1;

static void add_cand(struct result *r, dev_t node, struct leaf *lf, dev_t unknown, u64 sector, u32 nr)
{
	for (int i = 0; i < r->ncand; i++) {
		struct cand *c = &r->cands[i];
		if (c->node == node && c->unknown == unknown && c->sector == sector) {
			c->count++;
			return;
		}
	}
	if (r->ncand >= MAX_CANDS)
		return;
	struct cand *c = &r->cands[r->ncand++];
	memset(c, 0, sizeof(*c));
	c->node = node;
	c->leaf = lf;
	c->unknown = unknown;
	c->sector = sector;
	c->nr = nr;
	c->count = 1;
}

/* match collected events against the leaf set */
static void match_events(struct result *r, char dir)
{
	for (int i = 0; i < nevents; i++) {
		struct obs *ev = &events[i];
		if (ev->rwbs[0] != dir)
			continue;
		u32 want_nr = opts.bs / 512;
		if (tpmon.off_nr >= 0 && ev->nr_sect && ev->nr_sect != want_nr)
			continue;
		if (tpmon.off_nr < 0 && tpmon.off_bytes >= 0 &&
		    ev->bytes && ev->bytes != opts.bs)
			continue;
		if (opts.show_events) {
			char nbuf[128];
			dev_t d = kdev_decode(ev->dev);
			dev_display_name(d, nbuf, sizeof(nbuf));
			fprintf(stderr, "  event: %u:%u %-16s sector %-10llu + %-4u %s [%s]\n",
				major(d), minor(d), nbuf,
				(unsigned long long)ev->sector,
				ev->nr_sect, ev->rwbs, ev->comm);
		}
		int matched = 0;
		for (int l = 0; l < nleaves; l++) {
			if (kdev_encode(leaves[l].disk) != ev->dev)
				continue;
			/* an event carries a disk-relative sector; only
			 * attribute it to a partition leaf whose range
			 * actually contains that sector */
			if (leaves[l].part_size &&
			    (ev->sector < leaves[l].part_start ||
			     ev->sector >= leaves[l].part_start + leaves[l].part_size))
				continue;
			add_cand(r, leaves[l].node, &leaves[l], 0, ev->sector, ev->nr_sect);
			matched++;
		}
		if (!matched)
			add_cand(r, 0, NULL, kdev_decode(ev->dev), ev->sector, ev->nr_sect);
	}
}

static int content_check(struct cand *c, const void *expect, unsigned len)
{
	char *tmp;
	int fd, rc = 0;
	/* the whole disk owns the queue; cand->sector is disk-relative */
	dev_t d = c->leaf ? c->leaf->disk : c->unknown;

	if (!d)
		return 0;
	if (posix_memalign((void **)&tmp, 4096, len) < 0)
		return 0;
	fd = open_dev_node(d, O_RDONLY | O_DIRECT);
	if (fd < 0)
		fd = open_dev_node(d, O_RDONLY);
	if (fd < 0) {
		free(tmp);
		return 0;
	}
	if (pread(fd, tmp, len, c->sector * 512) == (ssize_t)len)
		rc = memcmp(tmp, expect, len) == 0 ? 1 : -1;
	close(fd);
	free(tmp);
	return rc;
}

static void run_probe(int top_fd, struct result *r, char dir, char *iobuf)
{
	for (int rep = 0; rep < opts.repeat; rep++) {
		tpmon_drain();

		if (!top_odirect)
			posix_fadvise(top_fd, r->probe->fs_off, opts.bs, POSIX_FADV_DONTNEED);

		ssize_t n;
		if (dir == 'R')
			n = pread(top_fd, iobuf, opts.bs, r->probe->fs_off);
		else
			n = pwrite(top_fd, iobuf, opts.bs, r->probe->fs_off);
		if (n != (ssize_t)opts.bs)
			die("I/O at offset %llu failed: %s",
			    (unsigned long long)r->probe->fs_off, strerror(errno));

		int before = r->ncand;
		nevents = 0;
		tpmon_read_all();
		match_events(r, dir);
		if (r->ncand == before) {
			/* give async completions a moment, then retry */
			nevents = 0;
			collect_for(opts.timeout_ms > 200 ? 200 : opts.timeout_ms);
			match_events(r, dir);
		}
	}
	r->observed = r->ncand > 0;

	if (opts.verify_content && dir == 'R') {
		int nver = 0;
		for (int i = 0; i < r->ncand; i++)
			r->cands[i].verified = content_check(&r->cands[i], iobuf, opts.bs);
		for (int i = 0; i < r->ncand; i++)
			if (r->cands[i].verified == 1)
				nver++;
		if (nver >= 1) {
			/* every content-verified candidate is a real copy of the
			 * block (mirror legs / all paths of this read); anything
			 * else is background I/O */
			r->copies = nver;
			r->bg_excluded = r->ncand - nver;
			r->ambiguous = 0;
		} else {
			/* nothing content-verified (crypt, parity, or unreadable
			 * leaves): cannot discriminate */
			r->ambiguous = (r->ncand > 1);
		}
	} else {
		r->ambiguous = (r->ncand > 1);
	}
}

/* ------------------------------------------------------------------ */
/* nonce mode                                                         */
/* ------------------------------------------------------------------ */

struct nonce_state {
	int active;
	int fd;
	u64 off;
	unsigned len;
	char *orig;
};

static struct nonce_state nonce_state;

static void nonce_restore(void)
{
	if (!nonce_state.active)
		return;
	nonce_state.active = 0;
	if (pwrite(nonce_state.fd, nonce_state.orig, nonce_state.len,
		   nonce_state.off) == (ssize_t)nonce_state.len)
		fsync(nonce_state.fd);
}

static void run_probe_nonce(int top_fd, struct result *r, char *iobuf)
{
	char *nonce, *orig, *check;
	int restored = -1;
	struct result rr;			/* read-probe observations */

	memset(&rr, 0, sizeof(rr));
	rr.probe = r->probe;

	if (posix_memalign((void *)&orig, 4096, opts.bs) < 0 ||
	    posix_memalign((void *)&nonce, 4096, opts.bs) < 0 ||
	    posix_memalign((void *)&check, 4096, opts.bs) < 0)
		die("out of memory");

	if (pread(top_fd, orig, opts.bs, r->probe->fs_off) != (ssize_t)opts.bs)
		die("nonce: cannot read original block: %s", strerror(errno));

	memcpy(nonce, "DEVLOC-NONCE-v1\n", 16);
	if (getrandom(nonce + 16, opts.bs - 16, 0) < 0)
		die("getrandom: %s", strerror(errno));

	/* read probe: on a healthy array only a data location serves reads
	 * of the block, which identifies it even when a parity location
	 * holds identical content (all-zero stripes make parity == data).
	 * Done BEFORE the nonce write: a just-written stripe stays in md's
	 * stripe cache and would serve this read without disk I/O.  A
	 * distant read first evicts the cache so recently-written blocks
	 * are still probed on disk. */
	{
		struct stat st;
		if (fstat(top_fd, &st) == 0 && (u64)st.st_size > (128ULL << 20)) {
			u64 len = 64ULL << 20;
			u64 ev;
			if (r->probe->fs_off + (128ULL << 20) <= (u64)st.st_size)
				ev = r->probe->fs_off + (64ULL << 20);
			else if (r->probe->fs_off >= (128ULL << 20))
				ev = r->probe->fs_off - (128ULL << 20);
			else
				ev = (u64)st.st_size - len;
			char *ebuf;
			if (posix_memalign((void **)&ebuf, 4096, 1 << 20) == 0) {
				for (u64 o = 0; o < len; o += (1 << 20))
					if (pread(top_fd, ebuf, 1 << 20, ev + o) < 0)
						break;
				free(ebuf);
			}
		}

		int nrep = opts.repeat > 4 ? 4 : opts.repeat;
		if (nrep < 1)
			nrep = 1;
		for (int rep = 0; rep < nrep; rep++) {
			tpmon_drain();
			if (pread(top_fd, iobuf, opts.bs, r->probe->fs_off) != (ssize_t)opts.bs)
				break;
			int before = rr.ncand;
			nevents = 0;
			tpmon_read_all();
			match_events(&rr, 'R');
			if (rr.ncand == before) {
				nevents = 0;
				collect_for(opts.timeout_ms > 200 ? 200 : opts.timeout_ms);
				match_events(&rr, 'R');
			}
		}
	}

	nonce_state.active = 1;
	nonce_state.fd = top_fd;
	nonce_state.off = r->probe->fs_off;
	nonce_state.len = opts.bs;
	nonce_state.orig = orig;

	tpmon_drain();
	if (pwrite(top_fd, nonce, opts.bs, r->probe->fs_off) != (ssize_t)opts.bs)
		die("nonce: write failed: %s", strerror(errno));

	nevents = 0;
	{
		/* writes can sit in the raid stripe cache, and the data and
		 * parity writes may land at different times; collect for the
		 * full window rather than stopping at the first event */
		collect_for(opts.timeout_ms);
		match_events(r, 'W');
	}

	/* verify the nonce at each candidate location (disk-relative) */
	for (int i = 0; i < r->ncand; i++) {
		dev_t d = r->cands[i].leaf ? r->cands[i].leaf->disk : r->cands[i].unknown;
		int fd = open_dev_node(d, O_RDONLY | O_DIRECT);
		if (fd < 0)
			fd = open_dev_node(d, O_RDONLY);
		if (fd >= 0) {
			if (pread(fd, check, opts.bs, r->cands[i].sector * 512) == (ssize_t)opts.bs)
				r->cands[i].verified =
					memcmp(check, nonce, opts.bs) == 0 ? 1 : -1;
			close(fd);
		}
	}
	r->observed = r->ncand > 0;
	{
		int ncopy = 0;
		for (int i = 0; i < r->ncand; i++)
			if (r->cands[i].verified == 1)
				ncopy++;
		r->copies = ncopy;
		/* multiple nonce-verified locations are mirror copies, not
		 * ambiguity; unverified companions are parity legs */
		r->ambiguous = (r->ncand > 1 && ncopy == 0);
	}

	/* mark which write locations serve reads of this block (data) */
	if (rr.ncand > 0) {
		r->read_probed = 1;
		for (int i = 0; i < r->ncand; i++)
			for (int j = 0; j < rr.ncand; j++)
				if (r->cands[i].node == rr.cands[j].node &&
				    r->cands[i].unknown == rr.cands[j].unknown &&
				    r->cands[i].sector == rr.cands[j].sector)
					r->cands[i].serves_reads = 1;
	}

	if (!opts.no_restore) {
		nonce_restore();
		if (pread(top_fd, check, opts.bs, r->probe->fs_off) == (ssize_t)opts.bs)
			restored = memcmp(check, orig, opts.bs) == 0;
		free(orig);
	} else {
		nonce_state.active = 0;
	}
	r->probe->reason = restored == 1 ? "nonce (restored)" :
			   restored == 0 ? "nonce (RESTORE FAILED)" : "nonce";
	free(nonce);
	free(check);
}

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

static const char *flag_names(u32 flags)
{
	static char buf[256];
	int off = 0;
	if (flags & FIEMAP_EXTENT_UNWRITTEN)
		off += snprintf(buf + off, sizeof(buf) - off, "%sunwritten", off ? "," : "");
	if (flags & FIEMAP_EXTENT_SHARED)
		off += snprintf(buf + off, sizeof(buf) - off, "%sshared(reflink)", off ? "," : "");
	if (flags & FIEMAP_EXTENT_DELALLOC)
		off += snprintf(buf + off, sizeof(buf) - off, "%sdelalloc", off ? "," : "");
	return buf;
}

static void json_escape(const char *s, char *out, size_t sz)
{
	char *o = out;
	for (const char *p = s; *p && (size_t)(o - out) < sz - 8; p++) {
		if (*p == '"' || *p == '\\') {
			*o++ = '\\';
			*o++ = *p;
		} else if ((unsigned char)*p < 0x20) {
			o += snprintf(o, sz - (o - out), "\\u%04x", *p);
		} else {
			*o++ = *p;
		}
	}
	*o = 0;
}

static void print_result_human(struct top_info *t)
{
	char b1[32], b2[32], b3[32], b4[32];

	printf("top:      %s\n", t->display);
	if (!opts.dev_mode) {
		fmt_units(opts.offset, opts.units, b1, sizeof(b1));
		fmt_units(opts.length ? opts.length : opts.bs, opts.units, b2, sizeof(b2));
		printf("range:    file offset %s + %s (%d block%s%s)\n",
		       b1, b2, nprobes, nprobes == 1 ? "" : "s",
		       nprobes > 1 ? " probed" : "");
	}
	{
		char ln[1024] = "";
		for (int i = 0; i < nleaves; i++) {
			char one[160];
			snprintf(one, sizeof(one), "%s%s (%u:%u)", i ? ", " : "",
				 leaves[i].name, major(leaves[i].node),
				 minor(leaves[i].node));
			if (strlen(ln) + strlen(one) + 1 < sizeof(ln))
				strcat(ln, one);
		}
		printf("leaves:   %s\n", nleaves ? ln : "(none)");
	}
	printf("\n");
	printf("%-14s %-11s %-18s %-14s %-8s %s\n",
	       "OFFSET", "FS SECTOR", "DEVICE", "DEV OFFSET", "LEN", "STATUS");

	for (int i = 0; i < nprobes; i++) {
		struct result *r = &results[i];
		struct probe *p = r->probe;

		if (p->unmappable) {
			fmt_units(p->file_off, opts.units, b1, sizeof(b1));
			printf("%-14s %-11s %-18s %-14s %-8s %s (%s)\n",
			       b1, "-", "-", "-", "-", "UNMAPPABLE", p->reason);
			continue;
		}

		fmt_units(p->file_off, opts.units, b1, sizeof(b1));
		fmt_units(p->len, opts.units, b4, sizeof(b4));

		if (!r->observed) {
			printf("%-14s %-11llu %-18s %-14s %-8s %s\n", b1,
			       (unsigned long long)(p->fs_off / 512), "-", "-", b4,
			       "NO-OBSERVATION");
			printf("    (thin-unmapped block, all-cached, or no I/O "
			       "reached a leaf)\n");
			continue;
		}

		int printed = 0;
		for (int j = 0; j < r->ncand; j++) {
			struct cand *c = &r->cands[j];
			if (r->bg_excluded && c->verified != 1)
				continue;	/* background, excluded by content check */
			char devname[128];
			const char *status;
			if (!c->leaf) {
				dev_display_name(c->unknown, devname, sizeof(devname));
				status = "not-in-leaf-set";
			} else if (opts.nonce) {
				if (c->serves_reads)
					status = c->verified == 1 ?
						"data (serves reads, nonce verified)" :
						"serves reads";
				else if (c->verified == 1)
					status = r->read_probed ?
						"copy (nonce verified, not serving reads)" :
						"copy (nonce verified)";
				else
					status = "parity (no nonce)";
			} else if (c->verified == 1) {
				status = r->copies > 1 ? "copy" : "verified";
			} else if (c->verified == -1) {
				status = "differs";
			} else {
				status = "unverified";
			}
			if (r->ambiguous)
				status = "AMBIGUOUS";
			fmt_units((c->sector - (c->leaf ? c->leaf->part_start : 0)) * 512,
				  opts.units, b3, sizeof(b3));
			char fscol[32];
			if (printed)
				fscol[0] = 0;
			else
				snprintf(fscol, sizeof(fscol), "%llu",
					 (unsigned long long)(p->fs_off / 512));
			printf("%-14s %-11s %-18s %-14s %-8s %s\n",
			       printed ? "" : b1, fscol,
			       c->leaf ? c->leaf->name : devname,
			       b3, b4, status);
			printed++;
		}

		if (r->bg_excluded)
			printf("    (%d background candidate%s excluded by content check)\n",
			       r->bg_excluded, r->bg_excluded == 1 ? "" : "s");
		if (opts.nonce && r->read_probed) {
			int nonsrv = 0;
			for (int j = 0; j < r->ncand; j++)
				if (r->cands[j].verified == 1 && !r->cands[j].serves_reads)
					nonsrv++;
			if (nonsrv)
				printf("    (read probe: locations serving reads are data copies; "
				       "others holding the nonce\n     are mirror copies or parity "
				       "equal to the data by coincidence)\n");
		}
		if (p->eflags & (FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_SHARED |
				 FIEMAP_EXTENT_DELALLOC))
			printf("    (extent flags: %s)\n", flag_names(p->eflags));
		if (opts.nonce && p->reason)
			printf("    (%s)\n", p->reason);
		for (int j = 0; j < r->ncand; j++) {
			struct cand *c = &r->cands[j];
			if (c->leaf && c->leaf->part_start) {
				printf("    (%s is a partition of %s, start %llu sectors; "
				       "disk-relative offset %llu on %s)\n",
				       c->leaf->name, c->leaf->diskname,
				       (unsigned long long)c->leaf->part_start,
				       (unsigned long long)(c->sector * 512),
				       c->leaf->diskname);
				break;
			}
		}
		if (opts.repeat > 1) {
			for (int j = 0; j < r->ncand; j++)
				if (r->cands[j].verified >= 0 || !opts.verify_content)
					printf("    (served %d/%d)\n",
					       r->cands[j].count, opts.repeat);
		}
	}
}

static void print_summary(void)
{
	struct { char name[128]; u64 count, min, max; } agg[MAX_LEAVES + 8];
	int nagg = 0;
	u64 total = 0;

	for (int i = 0; i < nprobes; i++) {
		struct result *r = &results[i];
		if (!r->observed || r->ambiguous)
			continue;
		struct cand *c = NULL;
		for (int j = 0; j < r->ncand; j++)
			if (r->cands[j].verified == 1) {
				c = &r->cands[j];
				break;
			}
		if (!c)
			for (int j = 0; j < r->ncand; j++)
				if (r->cands[j].verified != -1) {
					c = &r->cands[j];
					break;
				}
		if (!c)
			continue;
		char name[128];
		if (c->leaf)
			snprintf(name, sizeof(name), "%s", c->leaf->name);
		else
			dev_display_name(c->unknown, name, sizeof(name));
		int k;
		for (k = 0; k < nagg; k++)
			if (!strcmp(agg[k].name, name))
				break;
		if (k == nagg) {
			snprintf(agg[k].name, sizeof(agg[k].name), "%s", name);
			agg[k].count = 0;
			agg[k].min = ULLONG_MAX;
			agg[k].max = 0;
			nagg++;
		}
		agg[k].count++;
		agg[k].min = agg[k].min > r->probe->file_off ? r->probe->file_off : agg[k].min;
		agg[k].max = agg[k].max < r->probe->file_off ? r->probe->file_off : agg[k].max;
		total++;
	}
	char b1[32], b2[32];
	printf("DEVICE          BLOCKS   %%     FILE RANGE\n");
	for (int k = 0; k < nagg; k++) {
		fmt_units(agg[k].min, opts.units, b1, sizeof(b1));
		fmt_units(agg[k].max, opts.units, b2, sizeof(b2));
		printf("%-15s %-6llu %5.1f  %s .. %s\n", agg[k].name,
		       (unsigned long long)agg[k].count,
		       total ? 100.0 * agg[k].count / total : 0.0, b1, b2);
	}
}

static void print_json(struct top_info *t)
{
	char esc[PATH_MAX * 2];

	json_escape(t->display, esc, sizeof(esc));
	printf("{\n  \"top\": { \"display\": \"%s\", \"dev\": { \"major\": %u, \"minor\": %u, "
	       "\"name\": \"%s\" } },\n",
	       esc, major(t->dev), minor(t->dev), t->name);
	printf("  \"block_size\": %u,\n", opts.bs);
	printf("  \"probes\": [\n");
	int first = 1;
	for (int i = 0; i < nprobes; i++) {
		struct result *r = &results[i];
		struct probe *p = r->probe;
		if (p->unmappable)
			continue;
		if (!first)
			printf(",\n");
		first = 0;
		printf("    { \"file_offset\": %llu, \"fs_offset\": %llu, \"len\": %llu, "
		       "\"extent_flags\": \"%s\", \"observed\": %s, \"ambiguous\": %s, "
		       "\"read_probed\": %s, "
		       "\"observations\": [ ",
		       (unsigned long long)p->file_off, (unsigned long long)p->fs_off,
		       (unsigned long long)p->len, flag_names(p->eflags),
		       r->observed ? "true" : "false", r->ambiguous ? "true" : "false",
		       r->read_probed ? "true" : "false");
		for (int j = 0; j < r->ncand; j++) {
			struct cand *c = &r->cands[j];
			dev_t d = c->leaf ? c->leaf->disk : c->unknown;
			dev_display_name(d, esc, sizeof(esc));
			printf("%s{ \"device\": \"%s\", \"major\": %u, \"minor\": %u, "
			       "\"offset\": %llu, \"disk\": \"%s\", \"disk_offset\": %llu, "
			       "\"verified\": %s, \"served\": %d, "
			       "\"in_leaf_set\": %s, \"serves_reads\": %s }",
			       j ? ", " : "",
			       c->leaf ? c->leaf->name : esc,
			       c->leaf ? major(c->leaf->node) : major(d),
			       c->leaf ? minor(c->leaf->node) : minor(d),
			       (unsigned long long)((c->sector - (c->leaf ? c->leaf->part_start : 0)) * 512),
			       esc, (unsigned long long)(c->sector * 512),
			       c->verified == 1 ? "true" : "false",
			       c->count, c->leaf ? "true" : "false",
			       c->serves_reads ? "true" : "false");
		}
		printf(" ] }");
	}
	printf("\n  ],\n");
	printf("  \"unmappable\": [\n");
	first = 1;
	for (int i = 0; i < nprobes; i++) {
		if (!probes[i].unmappable)
			continue;
		if (!first)
			printf(",\n");
		first = 0;
		printf("    { \"file_offset\": %llu, \"len\": %llu, \"reason\": \"%s\" }",
		       (unsigned long long)probes[i].file_off,
		       (unsigned long long)probes[i].len, probes[i].reason);
	}
	printf("\n  ]\n}\n");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	struct top_info t;
	int top_fd = -1;
	char *iobuf;
	int exit_code = 0;
	const char *pos[3] = { 0 };
	int npos = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-v")) {
			opts.verbose++;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "--device") && i + 1 < argc) {
			opts.dev_mode = 1;
			opts.device = argv[++i];
		} else if (!strcmp(a, "--sector") && i + 1 < argc) {
			opts.dev_sector = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--block-size") && i + 1 < argc) {
			opts.bs = parse_size(argv[++i]);
		} else if (!strcmp(a, "--sample") && i + 1 < argc) {
			opts.sample = atoi(argv[++i]);
		} else if (!strcmp(a, "--probe-all")) {
			opts.probe_all = 1;
		} else if (!strcmp(a, "--repeat") && i + 1 < argc) {
			opts.repeat = atoi(argv[++i]);
			if (opts.repeat < 1)
				die("--repeat must be >= 1");
		} else if (!strcmp(a, "--nonce")) {
			opts.nonce = 1;
		} else if (!strcmp(a, "--no-restore")) {
			opts.no_restore = 1;
		} else if (!strcmp(a, "--force")) {
			opts.force = 1;
		} else if (!strcmp(a, "--force-writeback")) {
			opts.force_writeback = 1;
		} else if (!strcmp(a, "--no-verify-content")) {
			opts.verify_content = 0;
		} else if (!strcmp(a, "--quiet-check")) {
			opts.quiet_check = 1;
		} else if (!strcmp(a, "--strict")) {
			opts.strict = 1;
		} else if (!strcmp(a, "--passthrough")) {
			opts.passthrough = 1;
		} else if (!strcmp(a, "--events")) {
			opts.show_events = 1;
		} else if (!strcmp(a, "--json")) {
			opts.json = 1;
		} else if (!strcmp(a, "--summary")) {
			opts.summary = 1;
		} else if (!strcmp(a, "--units") && i + 1 < argc) {
			opts.units = argv[++i][0];
		} else if (!strcmp(a, "--timeout") && i + 1 < argc) {
			opts.timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(a, "--leaves") && i + 1 < argc) {
			opts.leaves_arg = argv[++i];
		} else if (!strncmp(a, "--", 2)) {
			fprintf(stderr, "devloc: unknown option %s\n", a);
			usage(stderr);
			return 1;
		} else if (a[0] == '-' && a[1]) {
			fprintf(stderr, "devloc: unknown option %s\n", a);
			usage(stderr);
			return 1;
		} else {
			if (npos >= 3)
				die("too many arguments (FILE [OFFSET [LENGTH]])");
			pos[npos++] = a;
		}
	}

	if (opts.bs < 512 || (opts.bs & (opts.bs - 1)))
		die("--block-size must be a power of two >= 512");
	if (!strchr("bskmg", opts.units))
		die("--units must be one of b, s, k, m, g");

	tpmon_open_();

	/* passthrough: just watch, no file/device needed */
	if (opts.passthrough) {
		nevents = 0;
		collect_for(opts.timeout_ms);
		printf("%-10s %-16s %-12s %-6s %s\n", "DEV", "NAME", "SECTOR", "NR", "RWBS COMM");
		for (int i = 0; i < nevents; i++) {
			struct obs *ev = &events[i];
			char name[128];
			dev_display_name(kdev_decode(ev->dev), name, sizeof(name));
			printf("%-10u %-16s %-12llu %-6u %s %s\n", ev->dev, name,
			       (unsigned long long)ev->sector, ev->nr_sect,
			       ev->rwbs, ev->comm);
		}
		return 0;
	}

	if (opts.dev_mode) {
		if (!opts.device)
			die("--device requires a device path");
		resolve_top_device(opts.device, &t);
	} else {
		if (!npos)
			die("no file given (see --help)");
		opts.file = pos[0];
		if (npos > 1)
			opts.offset = parse_size(pos[1]);
		if (npos > 2)
			opts.length = parse_size(pos[2]);
		resolve_top_file(opts.file, &t);
	}

	if (opts.leaves_arg)
		parse_leaves_arg();
	else
		discover_leaves_r(t.dev, 0);

	atexit(nonce_restore);

	/* background-I/O check */
	nevents = 0;
	collect_for(300);
	int bg = 0;
	for (int i = 0; i < nevents; i++)
		for (int l = 0; l < nleaves; l++)
			if (kdev_encode(leaves[l].disk) == events[i].dev)
				bg++;
	if (bg) {
		if (opts.quiet_check) {
			fprintf(stderr, "devloc: aborting: %d background event(s) on leaf "
				"devices in 300ms (--quiet-check)\n", bg);
			return 1;
		}
		fprintf(stderr, "devloc: warning: %d background event(s) on leaf devices "
			"in 300ms; results may be ambiguous (consider --nonce or "
			"--quiet-check)\n", bg);
	}

	/* build probe list */
	if (opts.dev_mode) {
		build_probe_device();
	} else {
		int fd = open(opts.file, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die("open %s: %s", opts.file, strerror(errno));
		struct stat st;
		if (fstat(fd, &st) < 0)
			die("fstat %s: %s", opts.file, strerror(errno));
		build_probes_file(fd, (u64)st.st_size);
		close(fd);
	}

	/* open the top device */
	int oflags = (opts.nonce ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT;
	if (opts.dev_mode)
		top_fd = open(opts.device, oflags);
	if (top_fd < 0)
		top_fd = open_dev_node(t.dev, oflags);
	if (top_fd < 0) {
		oflags &= ~O_DIRECT;
		if (opts.dev_mode)
			top_fd = open(opts.device, oflags);
		if (top_fd < 0)
			top_fd = open_dev_node(t.dev, oflags);
		if (top_fd >= 0) {
			top_odirect = 0;
			fprintf(stderr, "devloc: warning: O_DIRECT unavailable on top "
				"device; using buffered I/O\n");
		}
	}
	if (top_fd < 0)
		die("cannot open top device %u:%u: %s (needs root?)",
		    major(t.dev), minor(t.dev), strerror(errno));

	if (opts.nonce && !opts.force) {
		char mp[PATH_MAX], ft[32];
		if (find_mount(t.dev, mp, sizeof(mp), ft, sizeof(ft)) == 0)
			die("refusing --nonce: %u:%u holds a live filesystem (%s on %s); "
			    "use --force to override",
			    major(t.dev), minor(t.dev), ft, mp);
	}

	results = calloc(nprobes, sizeof(*results));
	if (!results)
		die("out of memory");
	if (posix_memalign((void **)&iobuf, 4096, opts.bs) < 0)
		die("out of memory");

	int tty = isatty(2);
	for (int i = 0; i < nprobes; i++) {
		results[i].probe = &probes[i];
		if (probes[i].unmappable)
			continue;
		if (opts.nonce)
			run_probe_nonce(top_fd, &results[i], iobuf);
		else
			run_probe(top_fd, &results[i], 'R', iobuf);
		if (tty && nprobes > 8 && (i % 16) == 15)
			fprintf(stderr, ".");
		if (opts.strict && (results[i].ambiguous || !results[i].observed)) {
			fprintf(stderr, "devloc: --strict: probe at offset %llu %s\n",
				(unsigned long long)probes[i].file_off,
				results[i].ambiguous ? "ambiguous" : "unobserved");
			return 2;
		}
	}
	if (tty && nprobes > 8)
		fprintf(stderr, "\n");

	/* output */
	if (opts.json)
		print_json(&t);
	else if (opts.summary)
		print_summary();
	else
		print_result_human(&t);

	/* exit code */
	for (int i = 0; i < nprobes; i++) {
		if (probes[i].unmappable)
			exit_code = 3;
		else if (results[i].ambiguous || !results[i].observed)
			exit_code = exit_code ? exit_code : 2;
	}

	close(top_fd);
	return exit_code;
}
