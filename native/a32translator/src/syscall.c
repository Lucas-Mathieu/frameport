/* ARM EABI syscalls on an AArch64 host. Guest pointers are host pointers (identity mapping), so most calls pass
 * through; structures whose 32-bit layout differs (stat64, timespec, iovec, rusage, ...) are converted. 64-bit
 * arguments arrive in even/odd register pairs (EABI). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "arm_syscalls.h"
#include "interp.h"
#include "runtime.h"
#include "signals.h"
#include "threads.h"

static inline long host(long nr, long a, long b, long c, long d, long e, long f)
{
    long r = syscall(nr, a, b, c, d, e, f);
    return r == -1 ? -errno : r;
}
#define H0(nr) host(nr, 0, 0, 0, 0, 0, 0)
#define H1(nr, a) host(nr, (long)(a), 0, 0, 0, 0, 0)
#define H2(nr, a, b) host(nr, (long)(a), (long)(b), 0, 0, 0, 0)
#define H3(nr, a, b, c) host(nr, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define H4(nr, a, b, c, d) host(nr, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define H5(nr, a, b, c, d, e) host(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define H6(nr, a, b, c, d, e, f) host(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

#define P(x) ((void *)(uintptr_t)(uint32_t)(x))
#define SI(x) ((long)(int32_t)(x)) /* signed 32-bit argument */
#define U64(lo, hi) ((uint64_t)(uint32_t)(hi) << 32 | (uint32_t)(lo))

/* ---------------------------------------------------------------- guest structures */

struct g_timespec { int32_t sec, nsec; };
struct g_timeval { int32_t sec, usec; };
struct g_iovec { uint32_t base, len; };
struct g_itimerspec { struct g_timespec interval, value; };
struct g_itimerval { struct g_timeval interval, value; };
struct g_rlimit { uint32_t cur, max; };
struct g_tms { int32_t utime, stime, cutime, cstime; };

struct g_stat64 {
    uint64_t st_dev;
    uint8_t pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev;
    uint8_t pad3[4];
    int64_t st_size __attribute__((aligned(8)));
    uint32_t st_blksize;
    uint64_t st_blocks __attribute__((aligned(8)));
    uint32_t g_atime, g_atime_nsec, g_mtime, g_mtime_nsec, g_ctime, g_ctime_nsec;
    uint64_t st_ino;
};
_Static_assert(sizeof(struct g_stat64) == 104, "stat64");

struct g_statfs64 {
    uint32_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} __attribute__((packed, aligned(4)));
_Static_assert(sizeof(struct g_statfs64) == 84, "statfs64");

struct g_statfs {
    uint32_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
};

struct g_rusage {
    struct g_timeval utime, stime;
    int32_t maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv, nsignals, nvcsw,
        nivcsw;
};

struct g_sysinfo {
    int32_t uptime;
    uint32_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t totalhigh, freehigh, mem_unit;
    char f[8];
};

struct g_flock { int16_t type, whence; int32_t start, len, pid; };

struct g_msghdr { uint32_t name, namelen, iov, iovlen, control, controllen, flags; };
struct g_cmsghdr { uint32_t len; int32_t level, type; };

static void ts_in(struct timespec *t, uint32_t g)
{
    struct g_timespec *s = P(g);
    t->tv_sec = s->sec;
    t->tv_nsec = s->nsec;
}

static void ts_out(uint32_t g, const struct timespec *t)
{
    struct g_timespec *s = P(g);
    s->sec = (int32_t)t->tv_sec;
    s->nsec = (int32_t)t->tv_nsec;
}

static void tv_out(struct g_timeval *d, const struct timeval *t)
{
    d->sec = (int32_t)t->tv_sec;
    d->usec = (int32_t)t->tv_usec;
}

static void stat_out(uint32_t g, const struct stat *s)
{
    struct g_stat64 d;
    memset(&d, 0, sizeof d);
    d.st_dev = s->st_dev;
    d.__st_ino = (uint32_t)s->st_ino;
    d.st_mode = s->st_mode;
    d.st_nlink = (uint32_t)s->st_nlink;
    d.st_uid = s->st_uid;
    d.st_gid = s->st_gid;
    d.st_rdev = s->st_rdev;
    d.st_size = s->st_size;
    d.st_blksize = (uint32_t)s->st_blksize;
    d.st_blocks = s->st_blocks;
    d.g_atime = (uint32_t)s->st_atim.tv_sec;
    d.g_atime_nsec = (uint32_t)s->st_atim.tv_nsec;
    d.g_mtime = (uint32_t)s->st_mtim.tv_sec;
    d.g_mtime_nsec = (uint32_t)s->st_mtim.tv_nsec;
    d.g_ctime = (uint32_t)s->st_ctim.tv_sec;
    d.g_ctime_nsec = (uint32_t)s->st_ctim.tv_nsec;
    d.st_ino = s->st_ino;
    memcpy(P(g), &d, sizeof d);
}

static void rusage_out(uint32_t g, const struct rusage *r)
{
    struct g_rusage d;
    tv_out(&d.utime, &r->ru_utime);
    tv_out(&d.stime, &r->ru_stime);
    d.maxrss = (int32_t)r->ru_maxrss, d.ixrss = (int32_t)r->ru_ixrss, d.idrss = (int32_t)r->ru_idrss;
    d.isrss = (int32_t)r->ru_isrss, d.minflt = (int32_t)r->ru_minflt, d.majflt = (int32_t)r->ru_majflt;
    d.nswap = (int32_t)r->ru_nswap, d.inblock = (int32_t)r->ru_inblock, d.oublock = (int32_t)r->ru_oublock;
    d.msgsnd = (int32_t)r->ru_msgsnd, d.msgrcv = (int32_t)r->ru_msgrcv, d.nsignals = (int32_t)r->ru_nsignals;
    d.nvcsw = (int32_t)r->ru_nvcsw, d.nivcsw = (int32_t)r->ru_nivcsw;
    memcpy(P(g), &d, sizeof d);
}

static struct iovec *iov_in(uint32_t g, uint32_t n)
{
    if (n > 1024) return NULL;
    struct iovec *v = malloc(sizeof *v * (n ? n : 1));
    struct g_iovec *s = P(g);
    for (uint32_t k = 0; k < n; k++) {
        v[k].iov_base = P(s[k].base);
        v[k].iov_len = s[k].len;
    }
    return v;
}

/* ---------------------------------------------------------------- paths */

extern const char *g_sysroot;
const char *g_sysroot;

static const char *gpath(uint32_t g, char *buf, size_t n)
{
    const char *p = P(g);
    if (g_sysroot && p[0] == '/' && strncmp(p, "/proc", 5) && strncmp(p, "/dev", 4) && strncmp(p, "/sys", 4)) {
        snprintf(buf, n, "%s%s", g_sysroot, p);
        if (access(buf, F_OK) == 0) return buf;
    }
    return p;
}

static int is_self_exe(const char *p)
{
    char buf[64];
    if (!strcmp(p, "/proc/self/exe")) return 1;
    snprintf(buf, sizeof buf, "/proc/%d/exe", getpid());
    return !strcmp(p, buf);
}

static long do_readlinkat(int dirfd, uint32_t path, uint32_t buf, uint32_t size)
{
    char tmp[4096];
    const char *p = gpath(path, tmp, sizeof tmp);
    if (is_self_exe(p)) {
        size_t n = strlen(g_exe_path);
        if (n > size) n = size;
        memcpy(P(buf), g_exe_path, n);
        return (long)n;
    }
    return H4(__NR_readlinkat, dirfd, p, P(buf), size);
}

/* /proc/cpuinfo as a 32-bit process sees it on an arm64 kernel (AArch32 feature names) */
static long fake_cpuinfo(void)
{
    int fd = (int)syscall(__NR_memfd_create, "cpuinfo", 0);
    if (fd < 0) return -errno;
    unsigned long hw = getauxval(AT_HWCAP);
    char feat[256] = "half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm";
    if (hw & (1 << 3)) strcat(feat, " aes");
    if (hw & (1 << 4)) strcat(feat, " pmull");
    if (hw & (1 << 5)) strcat(feat, " sha1");
    if (hw & (1 << 6)) strcat(feat, " sha2");
    strcat(feat, " crc32");
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long k = 0; k < (n > 0 ? n : 1); k++) {
        dprintf(fd, "processor\t: %ld\nmodel name\t: ARMv8 Processor rev 0 (v8l)\nBogoMIPS\t: 38.40\n"
                    "Features\t: %s\nCPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x0\n"
                    "CPU part\t: 0xd08\nCPU revision\t: 0\n\n", k, feat);
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static long do_openat(int dirfd, uint32_t path, int flags, int mode)
{
    char tmp[4096];
    const char *p = gpath(path, tmp, sizeof tmp);
    if (is_self_exe(p)) p = g_exe_path;
    if (!strcmp(p, "/proc/cpuinfo") && !(flags & (O_WRONLY | O_RDWR))) return fake_cpuinfo();
    return H4(__NR_openat, dirfd, p, flags, mode);
}

/* ---------------------------------------------------------------- individual calls */

static long do_fstatat(int dirfd, uint32_t path, uint32_t buf, int flags)
{
    struct stat st;
    char tmp[4096];
    const char *p = path ? gpath(path, tmp, sizeof tmp) : "";
    if (path && is_self_exe(p)) p = g_exe_path;
    long r = H4(__NR_newfstatat, dirfd, p, &st, flags);
    if (r == 0) stat_out(buf, &st);
    return r;
}

static long do_statfs64(long r, struct statfs *s, uint32_t buf)
{
    if (r) return r;
    struct g_statfs64 d;
    memset(&d, 0, sizeof d);
    d.f_type = (uint32_t)s->f_type, d.f_bsize = (uint32_t)s->f_bsize, d.f_blocks = s->f_blocks;
    d.f_bfree = s->f_bfree, d.f_bavail = s->f_bavail, d.f_files = s->f_files, d.f_ffree = s->f_ffree;
    memcpy(d.f_fsid, &s->f_fsid, 8);
    d.f_namelen = (uint32_t)s->f_namelen, d.f_frsize = (uint32_t)s->f_frsize, d.f_flags = (uint32_t)s->f_flags;
    memcpy(P(buf), &d, sizeof d);
    return 0;
}

static long do_statfs32(long r, struct statfs *s, uint32_t buf)
{
    if (r) return r;
    struct g_statfs d;
    memset(&d, 0, sizeof d);
    if ((s->f_blocks | s->f_bfree | s->f_bavail | s->f_files | s->f_ffree) >> 32) return -EOVERFLOW;
    d.f_type = (uint32_t)s->f_type, d.f_bsize = (uint32_t)s->f_bsize, d.f_blocks = (uint32_t)s->f_blocks;
    d.f_bfree = (uint32_t)s->f_bfree, d.f_bavail = (uint32_t)s->f_bavail, d.f_files = (uint32_t)s->f_files;
    d.f_ffree = (uint32_t)s->f_ffree;
    memcpy(d.f_fsid, &s->f_fsid, 8);
    d.f_namelen = (uint32_t)s->f_namelen, d.f_frsize = (uint32_t)s->f_frsize, d.f_flags = (uint32_t)s->f_flags;
    memcpy(P(buf), &d, sizeof d);
    return 0;
}

static long do_getdents32(int fd, uint32_t dirp, uint32_t count)
{
    /* struct linux_dirent { u32 ino; u32 off; u16 reclen; char name[]; ... char type at reclen - 1 } */
    char *buf = malloc(count);
    long n = H3(__NR_getdents64, fd, buf, count / 2 + 64 < count ? count / 2 + 64 : count);
    if (n <= 0) {
        free(buf);
        return n;
    }
    uint32_t out = 0;
    for (long pos = 0; pos < n;) {
        struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[]; } *e = (void *)(buf + pos);
        size_t nl = strlen(e->name);
        uint32_t rl = (uint32_t)((10 + nl + 2 + 3) & ~3u);
        if (out + rl > count) {
            if (!out) {
                free(buf);
                return -EINVAL;
            }
            H3(__NR_lseek, fd, e->off, SEEK_SET); /* hmm: rewind to the first entry not returned */
            break;
        }
        uint8_t *d = P(dirp + out);
        uint32_t ino = (uint32_t)e->ino, off = (uint32_t)e->off;
        uint16_t r16 = (uint16_t)rl;
        memcpy(d, &ino, 4);
        memcpy(d + 4, &off, 4);
        memcpy(d + 8, &r16, 2);
        memcpy(d + 10, e->name, nl + 1);
        d[rl - 1] = e->type;
        out += rl;
        pos += e->reclen;
    }
    free(buf);
    return out;
}

static long do_futex(uint32_t uaddr, int op, uint32_t val, uint32_t timeout, uint32_t uaddr2, uint32_t val3, int t64)
{
    int cmd = op & FUTEX_CMD_MASK;
    struct timespec ts, *tp = NULL;
    long arg4 = timeout;
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_LOCK_PI || cmd == FUTEX_WAIT_REQUEUE_PI ||
        cmd == 13 /* FUTEX_LOCK_PI2 */) {
        if (timeout) {
            if (t64) memcpy(&ts, P(timeout), sizeof ts);
            else ts_in(&ts, timeout);
            tp = &ts;
        }
        arg4 = (long)tp;
    }
    return H6(__NR_futex, P(uaddr), op, val, arg4, P(uaddr2), val3);
}

static long do_msg(int fd, uint32_t gmsg, int flags, int send)
{
    struct g_msghdr *gm = P(gmsg);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_name = gm->name ? P(gm->name) : NULL;
    m.msg_namelen = gm->namelen;
    struct iovec *iov = iov_in(gm->iov, gm->iovlen);
    if (!iov) return -EINVAL;
    m.msg_iov = iov;
    m.msg_iovlen = gm->iovlen;
    /* control messages: 32-bit cmsghdr (12 bytes, 4-byte aligned) <-> 64-bit (16 bytes, 8-byte aligned) */
    char ctl[4096];
    uint32_t gclen = gm->controllen;
    if (gm->control && gclen) {
        m.msg_control = ctl;
        m.msg_controllen = send ? 0 : sizeof ctl;
        if (send) {
            uint32_t off = 0;
            size_t hoff = 0;
            while (off + sizeof(struct g_cmsghdr) <= gclen) {
                struct g_cmsghdr *gc = P(gm->control + off);
                if (gc->len < sizeof *gc || off + gc->len > gclen) break;
                size_t dl = gc->len - sizeof *gc;
                struct cmsghdr *hc = (struct cmsghdr *)(ctl + hoff);
                if (hoff + CMSG_SPACE(dl) > sizeof ctl) break;
                hc->cmsg_len = CMSG_LEN(dl);
                hc->cmsg_level = gc->level;
                hc->cmsg_type = gc->type;
                memcpy(CMSG_DATA(hc), (char *)gc + sizeof *gc, dl);
                hoff += CMSG_SPACE(dl);
                off += (gc->len + 3) & ~3u;
            }
            m.msg_controllen = hoff;
        }
    }
    long r = send ? H3(__NR_sendmsg, fd, &m, flags) : H3(__NR_recvmsg, fd, &m, flags);
    if (r >= 0 && !send) {
        gm->namelen = m.msg_namelen;
        gm->flags = (uint32_t)m.msg_flags;
        uint32_t off = 0;
        if (gm->control && gclen) {
            for (struct cmsghdr *hc = CMSG_FIRSTHDR(&m); hc; hc = CMSG_NXTHDR(&m, hc)) {
                size_t dl = hc->cmsg_len - CMSG_LEN(0);
                uint32_t need = (uint32_t)(sizeof(struct g_cmsghdr) + dl);
                if (off + need > gclen) {
                    gm->flags |= MSG_CTRUNC;
                    break;
                }
                struct g_cmsghdr *gc = P(gm->control + off);
                gc->len = need;
                gc->level = hc->cmsg_level;
                gc->type = hc->cmsg_type;
                memcpy((char *)gc + sizeof *gc, CMSG_DATA(hc), dl);
                off += (need + 3) & ~3u;
            }
        }
        gm->controllen = off;
    }
    free(iov);
    return r;
}

static long do_sockopt_timeval(int fd, int level, int name, uint32_t val, uint32_t len, int set)
{
    struct timeval tv;
    if (set) {
        if (len < sizeof(struct g_timeval)) return -EINVAL;
        struct g_timeval *g = P(val);
        tv.tv_sec = g->sec;
        tv.tv_usec = g->usec;
        return H5(__NR_setsockopt, fd, level, name, &tv, sizeof tv);
    }
    socklen_t hl = sizeof tv;
    long r = H5(__NR_getsockopt, fd, level, name, &tv, &hl);
    if (r == 0) {
        uint32_t *gl = P(len);
        if (*gl < sizeof(struct g_timeval)) return -EINVAL;
        tv_out(P(val), &tv);
        *gl = sizeof(struct g_timeval);
    }
    return r;
}

static long do_select(uint32_t n, uint32_t r, uint32_t w, uint32_t e, uint32_t tvp)
{
    struct timespec ts, *tp = NULL;
    struct timeval tv;
    if (tvp) {
        struct g_timeval *g = P(tvp);
        ts.tv_sec = g->sec;
        ts.tv_nsec = (long)g->usec * 1000;
        tp = &ts;
    }
    long res = H6(__NR_pselect6, n, r ? P(r) : NULL, w ? P(w) : NULL, e ? P(e) : NULL, tp, NULL);
    if (tvp) {
        tv.tv_sec = ts.tv_sec;
        tv.tv_usec = ts.tv_nsec / 1000;
        tv_out(P(tvp), &tv);
    }
    return res;
}

static long do_pselect(uint32_t n, uint32_t r, uint32_t w, uint32_t e, uint32_t tsp, uint32_t sigp, int t64)
{
    struct timespec ts, *tp = NULL;
    struct { const void *ss; size_t len; } sig, *sp = NULL;
    if (tsp) {
        if (t64) memcpy(&ts, P(tsp), sizeof ts);
        else ts_in(&ts, tsp);
        tp = &ts;
    }
    if (sigp) {
        uint32_t *g = P(sigp);
        sig.ss = g[0] ? P(g[0]) : NULL;
        sig.len = g[1];
        sp = &sig;
    }
    long res = H6(__NR_pselect6, n, r ? P(r) : NULL, w ? P(w) : NULL, e ? P(e) : NULL, tp, sp);
    if (tsp) {
        if (t64) memcpy(P(tsp), &ts, sizeof ts);
        else ts_out(tsp, &ts);
    }
    return res;
}

static long do_uname(uint32_t buf)
{
    struct utsname *u = P(buf);
    long r = H1(__NR_uname, u);
    if (r == 0) snprintf(u->machine, sizeof u->machine, "armv8l");
    return r;
}

static long do_fcntl(int fd, int cmd, uint32_t arg, int is64)
{
    switch (cmd) {
    case 5: case 6: case 7: { /* F_GETLK/F_SETLK/F_SETLKW with the 32-bit struct flock */
        struct g_flock *g = P(arg);
        struct flock f = {.l_type = g->type, .l_whence = g->whence, .l_start = g->start, .l_len = g->len,
                          .l_pid = g->pid};
        long r = H3(__NR_fcntl, fd, cmd, &f);
        if (r == 0 && cmd == 5) {
            if (f.l_start > INT32_MAX || f.l_len > INT32_MAX) return -EOVERFLOW;
            g->type = f.l_type, g->whence = f.l_whence, g->start = (int32_t)f.l_start, g->len = (int32_t)f.l_len;
            g->pid = f.l_pid;
        }
        return r;
    }
    case 12: case 13: case 14: /* F_GETLK64.. : same layout as the 64-bit struct flock */
        return H3(__NR_fcntl, fd, cmd - 7, P(arg));
    default:
        (void)is64;
        return H3(__NR_fcntl, fd, cmd, (long)SI(arg) == (long)(int32_t)arg ? (long)arg : (long)arg);
    }
}

static long do_rlimit_get(int res, uint32_t g)
{
    struct rlimit r;
    long ret = H4(__NR_prlimit64, 0, res, NULL, &r);
    if (ret == 0) {
        struct g_rlimit *d = P(g);
        d->cur = r.rlim_cur > 0xffffffffull ? 0xffffffffu : (uint32_t)r.rlim_cur;
        d->max = r.rlim_max > 0xffffffffull ? 0xffffffffu : (uint32_t)r.rlim_max;
    }
    return ret;
}

static long do_rlimit_set(int res, uint32_t g)
{
    struct g_rlimit *s = P(g);
    struct rlimit r = {s->cur == 0xffffffffu ? RLIM_INFINITY : s->cur, s->max == 0xffffffffu ? RLIM_INFINITY : s->max};
    return H4(__NR_prlimit64, 0, res, &r, NULL);
}

static long do_sysinfo(uint32_t g)
{
    struct sysinfo s;
    long r = H1(__NR_sysinfo, &s);
    if (r) return r;
    struct g_sysinfo d;
    memset(&d, 0, sizeof d);
    /* scale memory figures so they fit 32 bits */
    unsigned long unit = s.mem_unit ? s.mem_unit : 1;
    while ((s.totalram * unit) >> 32 && unit < (1ul << 30)) {
        s.totalram >>= 1, s.freeram >>= 1, s.sharedram >>= 1, s.bufferram >>= 1, s.totalswap >>= 1, s.freeswap >>= 1;
        unit <<= 1;
    }
    d.uptime = (int32_t)s.uptime;
    for (int k = 0; k < 3; k++) d.loads[k] = (uint32_t)s.loads[k];
    d.totalram = (uint32_t)s.totalram, d.freeram = (uint32_t)s.freeram, d.sharedram = (uint32_t)s.sharedram;
    d.bufferram = (uint32_t)s.bufferram, d.totalswap = (uint32_t)s.totalswap, d.freeswap = (uint32_t)s.freeswap;
    d.procs = s.procs;
    d.mem_unit = (uint32_t)unit;
    memcpy(P(g), &d, sizeof d);
    return 0;
}

static long do_execve(a32_cpu *c, uint32_t path, uint32_t argv, uint32_t envp);

/* ---------------------------------------------------------------- dispatcher */

static long sys(a32_cpu *c, uint32_t nr)
{
    uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3], a4 = c->r[4], a5 = c->r[5];
    char tmp[4096], tmp2[4096];
    switch (nr) {
    case G_NR_restart_syscall: return H0(__NR_restart_syscall);
    case G_NR_exit: thread_exit(c, (int)a0);
    case G_NR_exit_group: guest_exit((int)a0);
    case G_NR_read: return H3(__NR_read, SI(a0), P(a1), a2);
    case G_NR_write: return H3(__NR_write, SI(a0), P(a1), a2);
    case G_NR_open: return do_openat(AT_FDCWD, a0, SI(a1), a2);
    case G_NR_openat: return do_openat(SI(a0), a1, SI(a2), a3);
    case G_NR_creat: return do_openat(AT_FDCWD, a0, O_CREAT | O_WRONLY | O_TRUNC, a1);
    case G_NR_close: return H1(__NR_close, SI(a0));
    case G_NR_close_range: return H3(__NR_close_range, a0, a1, a2);
    case G_NR_lseek: {
        long r = H3(__NR_lseek, SI(a0), SI(a1), SI(a2));
        if (r > INT32_MAX) return -EOVERFLOW;
        return r;
    }
    case G_NR__llseek: {
        long r = H3(__NR_lseek, SI(a0), (long)U64(a2, a1), SI(a4));
        if (r >= 0) {
            int64_t v = r;
            memcpy(P(a3), &v, 8);
            r = 0;
        }
        return r;
    }
    case G_NR_readv: case G_NR_writev: case G_NR_preadv: case G_NR_pwritev:
    case G_NR_preadv2: case G_NR_pwritev2: {
        struct iovec *v = iov_in(a1, a2);
        if (!v) return -EINVAL;
        long r;
        switch (nr) {
        case G_NR_readv: r = H3(__NR_readv, SI(a0), v, a2); break;
        case G_NR_writev: r = H3(__NR_writev, SI(a0), v, a2); break;
        case G_NR_preadv: r = H4(__NR_preadv, SI(a0), v, a2, U64(a3, a4)); break;
        case G_NR_pwritev: r = H4(__NR_pwritev, SI(a0), v, a2, U64(a3, a4)); break;
        case G_NR_preadv2: r = H5(__NR_preadv2, SI(a0), v, a2, U64(a3, a4), SI(a5)); break;
        default: r = H5(__NR_pwritev2, SI(a0), v, a2, U64(a3, a4), SI(a5)); break;
        }
        free(v);
        return r;
    }
    case G_NR_pread64: return H4(__NR_pread64, SI(a0), P(a1), a2, U64(a4, a5));
    case G_NR_pwrite64: return H4(__NR_pwrite64, SI(a0), P(a1), a2, U64(a4, a5));
    case G_NR_sendfile: {
        off_t off;
        if (a2) off = *(int32_t *)P(a2);
        long r = H4(__NR_sendfile, SI(a0), SI(a1), a2 ? &off : NULL, a3);
        if (a2 && r >= 0) *(int32_t *)P(a2) = (int32_t)off;
        return r;
    }
    case G_NR_sendfile64: return H4(__NR_sendfile, SI(a0), SI(a1), a2 ? P(a2) : NULL, a3);
    case G_NR_stat64: return do_fstatat(AT_FDCWD, a0, a1, 0);
    case G_NR_lstat64: return do_fstatat(AT_FDCWD, a0, a1, AT_SYMLINK_NOFOLLOW);
    case G_NR_fstat64: return do_fstatat(SI(a0), 0, a1, AT_EMPTY_PATH);
    case G_NR_fstatat64: return do_fstatat(SI(a0), a1, a2, SI(a3));
    case G_NR_statx: return H5(__NR_statx, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), a3, P(a4));
    case G_NR_statfs64: case G_NR_statfs: {
        struct statfs s;
        long r = H2(__NR_statfs, gpath(a0, tmp, sizeof tmp), &s);
        return nr == G_NR_statfs ? do_statfs32(r, &s, a1) : do_statfs64(r, &s, a2);
    }
    case G_NR_fstatfs64: case G_NR_fstatfs: {
        struct statfs s;
        long r = H2(__NR_fstatfs, SI(a0), &s);
        return nr == G_NR_fstatfs ? do_statfs32(r, &s, a1) : do_statfs64(r, &s, a2);
    }
    case G_NR_getdents64: return H3(__NR_getdents64, SI(a0), P(a1), a2);
    case G_NR_getdents: return do_getdents32(SI(a0), a1, a2);
    case G_NR_access: return H4(__NR_faccessat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), SI(a1), 0);
    case G_NR_faccessat: return H4(__NR_faccessat, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), 0);
    case G_NR_faccessat2: return H4(__NR_faccessat2, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), SI(a3));
    case G_NR_readlink: return do_readlinkat(AT_FDCWD, a0, a1, a2);
    case G_NR_readlinkat: return do_readlinkat(SI(a0), a1, a2, a3);
    case G_NR_unlink: return H3(__NR_unlinkat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), 0);
    case G_NR_unlinkat: return H3(__NR_unlinkat, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2));
    case G_NR_rmdir: return H3(__NR_unlinkat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), AT_REMOVEDIR);
    case G_NR_mkdir: return H3(__NR_mkdirat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), a1);
    case G_NR_mkdirat: return H3(__NR_mkdirat, SI(a0), gpath(a1, tmp, sizeof tmp), a2);
    case G_NR_mknod: return H4(__NR_mknodat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), a1, a2);
    case G_NR_mknodat: return H4(__NR_mknodat, SI(a0), gpath(a1, tmp, sizeof tmp), a2, a3);
    case G_NR_rename: return H5(__NR_renameat2, AT_FDCWD, gpath(a0, tmp, sizeof tmp), AT_FDCWD, gpath(a1, tmp2, sizeof tmp2), 0);
    case G_NR_renameat: return H5(__NR_renameat2, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), gpath(a3, tmp2, sizeof tmp2), 0);
    case G_NR_renameat2: return H5(__NR_renameat2, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), gpath(a3, tmp2, sizeof tmp2), a4);
    case G_NR_link: return H5(__NR_linkat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), AT_FDCWD, gpath(a1, tmp2, sizeof tmp2), 0);
    case G_NR_linkat: return H5(__NR_linkat, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), gpath(a3, tmp2, sizeof tmp2), SI(a4));
    case G_NR_symlink: return H3(__NR_symlinkat, P(a0), AT_FDCWD, gpath(a1, tmp, sizeof tmp));
    case G_NR_symlinkat: return H3(__NR_symlinkat, P(a0), SI(a1), gpath(a2, tmp, sizeof tmp));
    case G_NR_chmod: return H3(__NR_fchmodat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), a1);
    case G_NR_fchmod: return H2(__NR_fchmod, SI(a0), a1);
    case G_NR_fchmodat: return H3(__NR_fchmodat, SI(a0), gpath(a1, tmp, sizeof tmp), a2);
    case G_NR_chown32: return H5(__NR_fchownat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), SI(a1), SI(a2), 0);
    case G_NR_lchown32: return H5(__NR_fchownat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), SI(a1), SI(a2), AT_SYMLINK_NOFOLLOW);
    case G_NR_fchown32: return H3(__NR_fchown, SI(a0), SI(a1), SI(a2));
    case G_NR_fchownat: return H5(__NR_fchownat, SI(a0), gpath(a1, tmp, sizeof tmp), SI(a2), SI(a3), SI(a4));
    case G_NR_chdir: return H1(__NR_chdir, gpath(a0, tmp, sizeof tmp));
    case G_NR_fchdir: return H1(__NR_fchdir, SI(a0));
    case G_NR_getcwd: return H2(__NR_getcwd, P(a0), a1);
    case G_NR_truncate64: return H2(__NR_truncate, gpath(a0, tmp, sizeof tmp), U64(a2, a3));
    case G_NR_ftruncate64: return H2(__NR_ftruncate, SI(a0), U64(a2, a3));
    case G_NR_truncate: return H2(__NR_truncate, gpath(a0, tmp, sizeof tmp), SI(a1));
    case G_NR_ftruncate: return H2(__NR_ftruncate, SI(a0), SI(a1));
    case G_NR_fallocate: return H4(__NR_fallocate, SI(a0), SI(a1), U64(a2, a3), U64(a4, a5));
    case G_NR_arm_fadvise64_64: return H4(__NR_fadvise64, SI(a0), U64(a2, a3), U64(a4, a5), SI(a1));
    case G_NR_readahead: return H3(__NR_readahead, SI(a0), U64(a2, a3), a4);
    case G_NR_arm_sync_file_range: return H4(__NR_sync_file_range, SI(a0), U64(a2, a3), U64(a4, a5), a1);
    case G_NR_fsync: return H1(__NR_fsync, SI(a0));
    case G_NR_fdatasync: return H1(__NR_fdatasync, SI(a0));
    case G_NR_sync: return H0(__NR_sync);
    case G_NR_syncfs: return H1(__NR_syncfs, SI(a0));
    case G_NR_flock: return H2(__NR_flock, SI(a0), SI(a1));
    case G_NR_utimensat: {
        struct timespec ts[2];
        if (a2) ts_in(&ts[0], a2), ts_in(&ts[1], a2 + 8);
        return H4(__NR_utimensat, SI(a0), a1 ? gpath(a1, tmp, sizeof tmp) : NULL, a2 ? ts : NULL, SI(a3));
    }
    case G_NR_utimensat_time64:
        return H4(__NR_utimensat, SI(a0), a1 ? gpath(a1, tmp, sizeof tmp) : NULL, a2 ? P(a2) : NULL, SI(a3));
    case G_NR_utimes: {
        struct timespec ts[2];
        if (a1) {
            struct g_timeval *g = P(a1);
            for (int k = 0; k < 2; k++) ts[k].tv_sec = g[k].sec, ts[k].tv_nsec = (long)g[k].usec * 1000;
        }
        return H4(__NR_utimensat, AT_FDCWD, gpath(a0, tmp, sizeof tmp), a1 ? ts : NULL, 0);
    }
    case G_NR_dup: return H1(__NR_dup, SI(a0));
    case G_NR_dup2:
        if (a0 == a1) return H2(__NR_fcntl, SI(a0), F_GETFD) < 0 ? -EBADF : (long)a1;
        return H3(__NR_dup3, SI(a0), SI(a1), 0);
    case G_NR_dup3: return H3(__NR_dup3, SI(a0), SI(a1), SI(a2));
    case G_NR_pipe: return H2(__NR_pipe2, P(a0), 0);
    case G_NR_pipe2: return H2(__NR_pipe2, P(a0), SI(a1));
    case G_NR_fcntl: return do_fcntl(SI(a0), SI(a1), a2, 0);
    case G_NR_fcntl64: return do_fcntl(SI(a0), SI(a1), a2, 1);
    case G_NR_ioctl: return H3(__NR_ioctl, SI(a0), a1, (long)a2);
    case G_NR_umask: return H1(__NR_umask, a0);
    case G_NR_chroot: return H1(__NR_chroot, gpath(a0, tmp, sizeof tmp));

    case G_NR_getpid: return H0(__NR_getpid);
    case G_NR_getppid: return H0(__NR_getppid);
    case G_NR_gettid: return H0(__NR_gettid);
    case G_NR_getuid32: case G_NR_getuid: return H0(__NR_getuid);
    case G_NR_geteuid32: case G_NR_geteuid: return H0(__NR_geteuid);
    case G_NR_getgid32: case G_NR_getgid: return H0(__NR_getgid);
    case G_NR_getegid32: case G_NR_getegid: return H0(__NR_getegid);
    case G_NR_setuid32: return H1(__NR_setuid, a0);
    case G_NR_setgid32: return H1(__NR_setgid, a0);
    case G_NR_setreuid32: return H2(__NR_setreuid, SI(a0), SI(a1));
    case G_NR_setregid32: return H2(__NR_setregid, SI(a0), SI(a1));
    case G_NR_setresuid32: return H3(__NR_setresuid, SI(a0), SI(a1), SI(a2));
    case G_NR_setresgid32: return H3(__NR_setresgid, SI(a0), SI(a1), SI(a2));
    case G_NR_getresuid32: return H3(__NR_getresuid, P(a0), P(a1), P(a2));
    case G_NR_getresgid32: return H3(__NR_getresgid, P(a0), P(a1), P(a2));
    case G_NR_getgroups32: return H2(__NR_getgroups, SI(a0), P(a1));
    case G_NR_setgroups32: return H2(__NR_setgroups, SI(a0), P(a1));
    case G_NR_setfsuid32: return H1(__NR_setfsuid, a0);
    case G_NR_setfsgid32: return H1(__NR_setfsgid, a0);
    case G_NR_getpgid: return H1(__NR_getpgid, SI(a0));
    case G_NR_setpgid: return H2(__NR_setpgid, SI(a0), SI(a1));
    case G_NR_getpgrp: return H1(__NR_getpgid, 0);
    case G_NR_getsid: return H1(__NR_getsid, SI(a0));
    case G_NR_setsid: return H0(__NR_setsid);
    case G_NR_getpriority: return H2(__NR_getpriority, SI(a0), SI(a1));
    case G_NR_setpriority: return H3(__NR_setpriority, SI(a0), SI(a1), SI(a2));
    case G_NR_personality: {
        /* the persona (low byte, e.g. PER_LINUX32) is the guest's: hosts without AArch32 refuse PER_LINUX32 */
        static uint32_t persona = 0x0008; /* PER_LINUX32: what an AArch32 process sees */
        long host_flags = H1(__NR_personality, 0xffffffffu);
        if (host_flags < 0) return host_flags;
        uint32_t old = persona | ((uint32_t)host_flags & ~0xffu);
        if (a0 != 0xffffffffu) {
            long r = H1(__NR_personality, (host_flags & 0xff) | (a0 & ~0xffu));
            if (r < 0) return r;
            persona = a0 & 0xff;
        }
        return old;
    }
    case G_NR_capget: return H2(__NR_capget, P(a0), P(a1));
    case G_NR_capset: return H2(__NR_capset, P(a0), P(a1));
    case G_NR_kill: return H2(__NR_kill, SI(a0), SI(a1));
    case G_NR_tkill: return H2(__NR_tkill, SI(a0), SI(a1));
    case G_NR_tgkill: return H3(__NR_tgkill, SI(a0), SI(a1), SI(a2));

    case G_NR_brk: return mem_brk(a0);
    case G_NR_mmap2: {
        int64_t r = mem_mmap(a0, a1, SI(a2), SI(a3), SI(a4), (uint64_t)a5 << 12);
        return (long)r;
    }
    case G_NR_munmap: return mem_munmap(a0, a1);
    case G_NR_mprotect: return mem_mprotect(a0, a1, SI(a2));
    case G_NR_mremap: return (long)mem_mremap(a0, a1, a2, SI(a3), a4);
    case G_NR_madvise: return H3(__NR_madvise, P(a0), a1, SI(a2));
    case G_NR_msync: return H3(__NR_msync, P(a0), a1, SI(a2));
    case G_NR_mlock: return H2(__NR_mlock, P(a0), a1);
    case G_NR_munlock: return H2(__NR_munlock, P(a0), a1);
    case G_NR_mlockall: return H1(__NR_mlockall, SI(a0));
    case G_NR_munlockall: return H0(__NR_munlockall);
    case G_NR_mincore: return H3(__NR_mincore, P(a0), a1, P(a2));
    case G_NR_membarrier: return H3(__NR_membarrier, SI(a0), SI(a1), SI(a2));
    case G_NR_memfd_create: return H2(__NR_memfd_create, P(a0), a1);

    case G_NR_uname: return do_uname(a0);
    case G_NR_getrandom: return H3(__NR_getrandom, P(a0), a1, a2);
    case G_NR_sysinfo: return do_sysinfo(a0);
    case G_NR_prctl: return H5(__NR_prctl, SI(a0), a1, a2, a3, a4);
    case G_NR_set_tid_address:
        g_self->clear_tid = a0;
        return H0(__NR_gettid);
    case G_NR_set_robust_list: return 0; /* the kernel would read 64-bit pointers; robust futexes unsupported */
    case G_NR_get_robust_list: return -ENOSYS;
    case G_NR_rseq: return -ENOSYS;
    case G_NR_futex: return do_futex(a0, SI(a1), a2, a3, a4, a5, 0);
    case G_NR_futex_time64: return do_futex(a0, SI(a1), a2, a3, a4, a5, 1);
    case G_NR_sched_yield: return H0(__NR_sched_yield);
    case G_NR_sched_getaffinity: return H3(__NR_sched_getaffinity, SI(a0), a1 & ~7u, P(a2));
    case G_NR_sched_setaffinity: return H3(__NR_sched_setaffinity, SI(a0), a1 & ~7u, P(a2));
    case G_NR_sched_getparam: return H2(__NR_sched_getparam, SI(a0), P(a1));
    case G_NR_sched_setparam: return H2(__NR_sched_setparam, SI(a0), P(a1));
    case G_NR_sched_getscheduler: return H1(__NR_sched_getscheduler, SI(a0));
    case G_NR_sched_setscheduler: return H3(__NR_sched_setscheduler, SI(a0), SI(a1), P(a2));
    case G_NR_sched_get_priority_max: return H1(__NR_sched_get_priority_max, SI(a0));
    case G_NR_sched_get_priority_min: return H1(__NR_sched_get_priority_min, SI(a0));
    case G_NR_getcpu: return H3(__NR_getcpu, a0 ? P(a0) : NULL, a1 ? P(a1) : NULL, NULL);

    case G_NR_clock_gettime: case G_NR_clock_getres: {
        struct timespec ts;
        long r = H2(nr == G_NR_clock_gettime ? __NR_clock_gettime : __NR_clock_getres, SI(a0), &ts);
        if (r == 0 && a1) ts_out(a1, &ts);
        return r;
    }
    case G_NR_clock_gettime64: return H2(__NR_clock_gettime, SI(a0), P(a1));
    case G_NR_clock_getres_time64: return H2(__NR_clock_getres, SI(a0), a1 ? P(a1) : NULL);
    case G_NR_gettimeofday: {
        struct timeval tv;
        long r = H2(__NR_gettimeofday, a0 ? &tv : NULL, a1 ? P(a1) : NULL);
        if (r == 0 && a0) tv_out(P(a0), &tv);
        return r;
    }
    case G_NR_nanosleep: {
        struct timespec in, rem;
        ts_in(&in, a0);
        long r = H2(__NR_nanosleep, &in, &rem);
        if (r == -EINTR && a1) ts_out(a1, &rem);
        return r;
    }
    case G_NR_clock_nanosleep: {
        struct timespec in, rem;
        ts_in(&in, a2);
        long r = H4(__NR_clock_nanosleep, SI(a0), SI(a1), &in, &rem);
        if (r == -EINTR && a3) ts_out(a3, &rem);
        return r;
    }
    case G_NR_clock_nanosleep_time64: return H4(__NR_clock_nanosleep, SI(a0), SI(a1), P(a2), a3 ? P(a3) : NULL);
    case G_NR_times: {
        struct tms t;
        long r = H1(__NR_times, &t);
        if (a0) {
            struct g_tms *g = P(a0);
            g->utime = (int32_t)t.tms_utime, g->stime = (int32_t)t.tms_stime;
            g->cutime = (int32_t)t.tms_cutime, g->cstime = (int32_t)t.tms_cstime;
        }
        return (int32_t)r;
    }
    case G_NR_getrusage: {
        struct rusage ru;
        long r = H2(__NR_getrusage, SI(a0), &ru);
        if (r == 0) rusage_out(a1, &ru);
        return r;
    }
    case G_NR_wait4: {
        struct rusage ru;
        long r = H4(__NR_wait4, SI(a0), a1 ? P(a1) : NULL, SI(a2), a3 ? &ru : NULL);
        if (r > 0 && a3) rusage_out(a3, &ru);
        return r;
    }
    case G_NR_ugetrlimit: return do_rlimit_get(SI(a0), a1);
    case G_NR_setrlimit: return do_rlimit_set(SI(a0), a1);
    case G_NR_prlimit64: return H4(__NR_prlimit64, SI(a0), SI(a1), a2 ? P(a2) : NULL, a3 ? P(a3) : NULL);
    case G_NR_getitimer: case G_NR_setitimer: {
        struct itimerval in, out;
        if (nr == G_NR_setitimer) {
            struct g_itimerval *g = P(a1);
            in.it_interval.tv_sec = g->interval.sec, in.it_interval.tv_usec = g->interval.usec;
            in.it_value.tv_sec = g->value.sec, in.it_value.tv_usec = g->value.usec;
        }
        long r = nr == G_NR_setitimer ? H3(__NR_setitimer, SI(a0), &in, &out) : H2(__NR_getitimer, SI(a0), &out);
        uint32_t o = nr == G_NR_setitimer ? a2 : a1;
        if (r == 0 && o) {
            struct g_itimerval *g = P(o);
            tv_out(&g->interval, &out.it_interval);
            tv_out(&g->value, &out.it_value);
        }
        return r;
    }

    case G_NR_poll: return H5(__NR_ppoll, P(a0), a1, NULL, NULL, 0);
    case G_NR_ppoll: case G_NR_ppoll_time64: {
        struct timespec ts, *tp = NULL;
        if (a2) {
            if (nr == G_NR_ppoll) ts_in(&ts, a2);
            else memcpy(&ts, P(a2), sizeof ts);
            tp = &ts;
        }
        long r = H5(__NR_ppoll, P(a0), a1, tp, a3 ? P(a3) : NULL, a4);
        if (a2 && nr == G_NR_ppoll) ts_out(a2, &ts);
        return r;
    }
    case G_NR__newselect: return do_select(a0, a1, a2, a3, a4);
    case G_NR_pselect6: return do_pselect(a0, a1, a2, a3, a4, a5, 0);
    case G_NR_pselect6_time64: return do_pselect(a0, a1, a2, a3, a4, a5, 1);
    case G_NR_epoll_create: return H1(__NR_epoll_create1, 0);
    case G_NR_epoll_create1: return H1(__NR_epoll_create1, SI(a0));
    case G_NR_epoll_ctl: return H4(__NR_epoll_ctl, SI(a0), SI(a1), SI(a2), a3 ? P(a3) : NULL);
    case G_NR_epoll_wait: return H6(__NR_epoll_pwait, SI(a0), P(a1), SI(a2), SI(a3), NULL, 8);
    case G_NR_epoll_pwait: return H6(__NR_epoll_pwait, SI(a0), P(a1), SI(a2), SI(a3), a4 ? P(a4) : NULL, a5);
    case G_NR_eventfd: return H2(__NR_eventfd2, a0, 0);
    case G_NR_eventfd2: return H2(__NR_eventfd2, a0, SI(a1));
    case G_NR_signalfd4: return H4(__NR_signalfd4, SI(a0), P(a1), a2, SI(a3));
    case G_NR_timerfd_create: return H2(__NR_timerfd_create, SI(a0), SI(a1));
    case G_NR_timerfd_settime: case G_NR_timerfd_gettime: {
        struct itimerspec in, out;
        long r;
        if (nr == G_NR_timerfd_settime) {
            struct g_itimerspec *g = P(a2);
            in.it_interval.tv_sec = g->interval.sec, in.it_interval.tv_nsec = g->interval.nsec;
            in.it_value.tv_sec = g->value.sec, in.it_value.tv_nsec = g->value.nsec;
            r = H4(__NR_timerfd_settime, SI(a0), SI(a1), &in, &out);
        } else {
            r = H2(__NR_timerfd_gettime, SI(a0), &out);
        }
        uint32_t o = nr == G_NR_timerfd_settime ? a3 : a1;
        if (r == 0 && o) {
            ts_out(o, &out.it_interval);
            ts_out(o + 8, &out.it_value);
        }
        return r;
    }
    case G_NR_timerfd_settime64: return H4(__NR_timerfd_settime, SI(a0), SI(a1), P(a2), a3 ? P(a3) : NULL);
    case G_NR_timerfd_gettime64: return H2(__NR_timerfd_gettime, SI(a0), P(a1));
    case G_NR_inotify_init: return H1(__NR_inotify_init1, 0);
    case G_NR_inotify_init1: return H1(__NR_inotify_init1, SI(a0));
    case G_NR_inotify_add_watch: return H3(__NR_inotify_add_watch, SI(a0), gpath(a1, tmp, sizeof tmp), a2);
    case G_NR_inotify_rm_watch: return H2(__NR_inotify_rm_watch, SI(a0), SI(a1));
    case G_NR_splice: return H6(__NR_splice, SI(a0), a1 ? P(a1) : NULL, SI(a2), a3 ? P(a3) : NULL, a4, a5);
    case G_NR_tee: return H4(__NR_tee, SI(a0), SI(a1), a2, a3);
    case G_NR_copy_file_range:
        return H6(__NR_copy_file_range, SI(a0), a1 ? P(a1) : NULL, SI(a2), a3 ? P(a3) : NULL, a4, a5);

    case G_NR_socket: return H3(__NR_socket, SI(a0), SI(a1), SI(a2));
    case G_NR_socketpair: return H4(__NR_socketpair, SI(a0), SI(a1), SI(a2), P(a3));
    case G_NR_bind: return H3(__NR_bind, SI(a0), P(a1), a2);
    case G_NR_connect: return H3(__NR_connect, SI(a0), P(a1), a2);
    case G_NR_listen: return H2(__NR_listen, SI(a0), SI(a1));
    case G_NR_accept: return H4(__NR_accept4, SI(a0), a1 ? P(a1) : NULL, a2 ? P(a2) : NULL, 0);
    case G_NR_accept4: return H4(__NR_accept4, SI(a0), a1 ? P(a1) : NULL, a2 ? P(a2) : NULL, SI(a3));
    case G_NR_getsockname: return H3(__NR_getsockname, SI(a0), P(a1), P(a2));
    case G_NR_getpeername: return H3(__NR_getpeername, SI(a0), P(a1), P(a2));
    case G_NR_send: return H6(__NR_sendto, SI(a0), P(a1), a2, SI(a3), NULL, 0);
    case G_NR_sendto: return H6(__NR_sendto, SI(a0), P(a1), a2, SI(a3), a4 ? P(a4) : NULL, a5);
    case G_NR_recv: return H6(__NR_recvfrom, SI(a0), P(a1), a2, SI(a3), NULL, NULL);
    case G_NR_recvfrom: return H6(__NR_recvfrom, SI(a0), P(a1), a2, SI(a3), a4 ? P(a4) : NULL, a5 ? P(a5) : NULL);
    case G_NR_shutdown: return H2(__NR_shutdown, SI(a0), SI(a1));
    case G_NR_setsockopt:
        if (a1 == SOL_SOCKET && (a2 == SO_RCVTIMEO || a2 == SO_SNDTIMEO)) return do_sockopt_timeval(SI(a0), SI(a1), SI(a2), a3, a4, 1);
        return H5(__NR_setsockopt, SI(a0), SI(a1), SI(a2), a3 ? P(a3) : NULL, a4);
    case G_NR_getsockopt:
        if (a1 == SOL_SOCKET && (a2 == SO_RCVTIMEO || a2 == SO_SNDTIMEO)) return do_sockopt_timeval(SI(a0), SI(a1), SI(a2), a3, a4, 0);
        return H5(__NR_getsockopt, SI(a0), SI(a1), SI(a2), a3 ? P(a3) : NULL, a4 ? P(a4) : NULL);
    case G_NR_sendmsg: return do_msg(SI(a0), a1, SI(a2), 1);
    case G_NR_recvmsg: return do_msg(SI(a0), a1, SI(a2), 0);

    case G_NR_rt_sigaction: return sys_rt_sigaction(SI(a0), a1, a2, a3);
    case G_NR_rt_sigprocmask: return sys_rt_sigprocmask(c, SI(a0), a1, a2, a3);
    case G_NR_rt_sigpending: return H2(__NR_rt_sigpending, P(a0), a1);
    case G_NR_rt_sigsuspend: return sys_rt_sigsuspend(c, a0, a1);
    case G_NR_sigaltstack: return sys_sigaltstack(c, a0, a1);
    case G_NR_rt_sigreturn: return sys_rt_sigreturn(c);
    case G_NR_sigreturn: return sys_sigreturn(c);
    case G_NR_rt_sigtimedwait: case G_NR_rt_sigtimedwait_time64: {
        struct timespec ts, *tp = NULL;
        if (a2) {
            if (nr == G_NR_rt_sigtimedwait) ts_in(&ts, a2);
            else memcpy(&ts, P(a2), sizeof ts);
            tp = &ts;
        }
        siginfo_t si;
        long r = H4(__NR_rt_sigtimedwait, P(a0), a1 ? &si : NULL, tp, a3);
        if (r > 0 && a1) siginfo_out(a1, &si);
        return r;
    }
    case G_NR_rt_sigqueueinfo: case G_NR_rt_tgsigqueueinfo: return -EPERM;

    case G_NR_clone: return sys_clone(c, a0, a1, a2, a3, a4);
    case G_NR_fork: case G_NR_vfork: return sys_clone(c, SIGCHLD, 0, 0, 0, 0);
    case G_NR_execve: return do_execve(c, a0, a1, a2);

    case G_NR_ARM_set_tls:
        c->tpidruro = a0;
        return 0;
    case G_NR_ARM_get_tls: return c->tpidruro;
    case G_NR_ARM_cacheflush:
        code_invalidate(a0, a1 - a0);
        c->exit_reason = EXIT_FLUSH;
        return 0;
    case G_NR_ARM_breakpoint: return H2(__NR_kill, getpid(), SIGTRAP);
    default:
        if (opt_verbose || opt_strace) fprintf(stderr, "a32: unimplemented syscall %u\n", nr);
        return -ENOSYS;
    }
}

/* ---------------------------------------------------------------- execve */

static int is_arm32_elf(const char *path)
{
    unsigned char h[20];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    return n == (ssize_t)sizeof h && !memcmp(h, "\177ELF", 4) && h[4] == 1 && h[5] == 1 && h[18] == 40;
}

static long do_execve(a32_cpu *c, uint32_t path, uint32_t argv, uint32_t envp)
{
    (void)c;
    char tmp[4096], self[4096];
    const char *p = gpath(path, tmp, sizeof tmp);
    uint32_t *ga = P(argv), *ge = P(envp);
    int na = 0, ne = 0;
    while (ga && ga[na]) na++;
    while (ge && ge[ne]) ne++;
    char **args = calloc(na + 8, sizeof *args), **env = calloc(ne + 1, sizeof *env);
    for (int k = 0; k < ne; k++) env[k] = P(ge[k]);
    long r;
    if (is_arm32_elf(p)) {
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n < 0) return -errno;
        self[n] = 0;
        int k = 0;
        args[k++] = self;
        if (g_sysroot) args[k++] = "--sysroot", args[k++] = (char *)g_sysroot;
        if (opt_interp) args[k++] = "--interp";
        args[k++] = "--argv0";
        args[k++] = na ? P(ga[0]) : (char *)p;
        args[k++] = "--";
        args[k++] = (char *)p;
        for (int j = 1; j < na; j++) args[k++] = P(ga[j]);
        r = H3(__NR_execve, self, args, env);
    } else {
        for (int k = 0; k < na; k++) args[k] = P(ga[k]);
        r = H3(__NR_execve, p, args, env);
    }
    free(args);
    free(env);
    return r;
}

static const char *syscall_name(uint32_t nr)
{
    for (size_t k = 0; k < sizeof g_syscall_names / sizeof g_syscall_names[0]; k++)
        if ((uint32_t)g_syscall_names[k].nr == nr) return g_syscall_names[k].name;
    return "?";
}

/* system calls that can block: a pending signal is delivered before they start, and they are restarted after the
 * guest's handler when a signal interrupted them (the kernel's -ERESTARTSYS) */
static int blocking_syscall(uint32_t nr)
{
    switch (nr) {
    case G_NR_futex: case G_NR_futex_time64: case G_NR_nanosleep: case G_NR_clock_nanosleep:
    case G_NR_clock_nanosleep_time64: case G_NR_poll: case G_NR_ppoll: case G_NR_ppoll_time64:
    case G_NR__newselect: case G_NR_pselect6: case G_NR_pselect6_time64: case G_NR_epoll_wait:
    case G_NR_epoll_pwait: case G_NR_epoll_pwait2: case G_NR_read: case G_NR_readv: case G_NR_wait4:
    case G_NR_waitid: case G_NR_accept: case G_NR_accept4: case G_NR_recvfrom: case G_NR_recvmsg:
        return 1;
    }
    return 0;
}

/* after -EINTR: Linux restarts these only with SA_RESTART; sleeps, polls and waits for signals never; timed futex
 * waits always (restart_block) */
static int restart_after_signal(a32_cpu *c, uint32_t nr)
{
    switch (nr) {
    case G_NR_nanosleep: case G_NR_clock_nanosleep: case G_NR_clock_nanosleep_time64: case G_NR_poll:
    case G_NR_ppoll: case G_NR_ppoll_time64: case G_NR__newselect: case G_NR_pselect6: case G_NR_pselect6_time64:
    case G_NR_epoll_wait: case G_NR_epoll_pwait: case G_NR_epoll_pwait2:
        return 0;
    case G_NR_futex: case G_NR_futex_time64: {
        uint32_t op = c->r[1] & 127;
        if ((op == 0 || op == 9) && c->r[3]) return 1; /* FUTEX_WAIT(_BITSET) with a timeout */
        break;
    }
    }
    return signal_restarts(g_self);
}

__attribute__((weak)) void a32_hypercall(a32_cpu *c, uint32_t n)
{
    (void)n;
    c->r[0] = (uint32_t)-ENOSYS;
}

void do_syscall(a32_cpu *c)
{
    uint32_t nr = c->r[7];
    if ((nr & 0xffff0000u) == A32_HYPERCALL && !c->svc_imm) {
        a32_hypercall(c, nr & 0xffff);
        return;
    }
    if (c->svc_imm != 0 && !c->thumb) {
        /* OABI: number in the SVC immediate (0x900000 + nr) */
        nr = c->svc_imm & 0xfffff;
    }
    if (opt_strace)
        fprintf(stderr, "[%d] %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", (int)syscall(__NR_gettid), syscall_name(nr),
                c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5]);
    int blocking = blocking_syscall(nr);
    if (blocking && signal_pending(g_self)) {
        /* deliver first, then make the call (as if the signal came just before it) */
        if (opt_strace) fprintf(stderr, " = (signal first)\n");
        c->r[15] -= c->thumb ? 2 : 4;
        return;
    }
    long r = sys(c, nr);
    if (r == -EINTR && blocking && signal_pending(g_self) && restart_after_signal(c, nr)) {
        if (opt_strace) fprintf(stderr, " = (interrupted, restarted after the handler)\n");
        c->r[15] -= c->thumb ? 2 : 4;
        return;
    }
    if (nr == G_NR_rt_sigreturn || nr == G_NR_sigreturn) {
        if (opt_strace) fprintf(stderr, " = (sigreturn to 0x%x)\n", c->r[15]);
        return;
    }
    if (opt_strace) fprintf(stderr, " = %ld\n", r);
    c->r[0] = (uint32_t)r;
}
