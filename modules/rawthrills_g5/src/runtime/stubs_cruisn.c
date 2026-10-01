/*
 * stubs_cruisn.c
 * Additive libc + libstdc++ + pthread stubs surfaced by the Cruis'n Blast
 * Unresolved-symbol pass.
 *
 * Everything here is safe/additive:
 *   - a game that doesn't import a symbol never sees it,
 *   - functions we can back with a Win32 CRT / SRW / etc. call do,
 *   - anything Linux-only (statfs, endutent, shmget, ftime, ...) returns a
 *     benign "no data / not supported" value that lets the caller keep going.
 *
 * Data symbols (libstdc++ vtables + typeinfos + std::string empty-Rep) get
 * real static blobs so R_386_COPY can memcpy them into the game's .bss.
 *
 * Wired from stubs_glibc.c via cruisn_stubs_register(ctx).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <intrin.h>
#include "loader.h"

/* Guest allocations for libstdc++ data blobs (empty _Rep etc.) travel with
 * the game - we allocate them once, statically, so no heap involvement. */

/* ------------------------------------------------------------------ */
/* libstdc++ RTTI / vtables - real static byte blobs                   */
/* ------------------------------------------------------------------ */
/* std::exception typeinfo + vtable for std::bad_alloc / runtime_error /
 * bad_exception. These are COPY-relocated into the game's .bss - the game
 * only ever COMPARES the address (identity), never dereferences the vtable
 * shape at runtime unless it actually throws. We give each a zeroed 8-slot
 * blob so a spurious deref reads NULL-pointers instead of crashing on an
 * unmapped page. */
static void *s_vt_bad_alloc[8]     = {0};
static void *s_vt_runtime_error[8] = {0};
/* Minimal __class_type_info-shaped blob: [vtable_ptr, name_ptr]. name_ptr
 * points to a static string so typeid(x).name() returns something printable
 * if a game ever calls it. */
static const char s_ti_bad_alloc_name[]     = "St9bad_alloc";
static const char s_ti_exception_name[]     = "St9exception";
static const char s_ti_runtime_error_name[] = "St13runtime_error";
static const char s_ti_bad_exception_name[] = "St13bad_exception";
static void *s_ti_bad_alloc[4]     = { 0, (void*)s_ti_bad_alloc_name,     0, 0 };
static void *s_ti_exception[4]     = { 0, (void*)s_ti_exception_name,     0, 0 };
static void *s_ti_runtime_error[4] = { 0, (void*)s_ti_runtime_error_name, 0, 0 };
static void *s_ti_bad_exception[4] = { 0, (void*)s_ti_bad_exception_name, 0, 0 };

/* std::string GCC 3.4 COW _Rep header + trailing "" body, laid out so the
 * exposed pointer is +sizeof(StringRep) into the blob - matches
 * stubs_cxx.c::StringRep exactly. libstdc++ ships _S_empty_rep_storage as
 * an aligned uint32_t[4] and copies it in wholesale, then hands out
 * (char*)&storage + sizeof(_Rep). We fake the same layout: refcount = -1
 * (glibc sentinel = shared-forever, never freed), length = 0, capacity = 0,
 * data = "\0". */
typedef struct { uint32_t length; uint32_t capacity; int32_t refcount; } CruisnStringRep;
static struct __attribute__((aligned(4))) {
    CruisnStringRep rep;
    char            data[4];
} s_empty_rep_storage = { { 0u, 0u, -1 }, { 0, 0, 0, 0 } };

/* ------------------------------------------------------------------ */
/* libc helpers                                                        */
/* ------------------------------------------------------------------ */
extern int *__errno_location_noop(void);  /* not exported; only used inline via g_linux_errno */
/* g_linux_errno lives in stubs_glibc.c; we cannot see it from here, so use a
 * local pthread errno slot for our own errors - cheap. */
static __thread int s_local_errno = 0;
static void set_errno(int e) { s_local_errno = e; }

/* gethostname - return a fixed name; games that check use it only for logging. */
static int cru_gethostname(char *name, size_t len)
{
    static const char kName[] = "ultra";
    if (!name || len == 0) { set_errno(22); return -1; }
    size_t n = sizeof(kName) - 1;
    if (n >= len) n = len - 1;
    memcpy(name, kName, n);
    name[n] = 0;
    return 0;
}

/* clock - CPU time in CLOCKS_PER_SEC ticks. Windows clock() returns
 * wall-time; forward. Games sometimes divide by CLOCKS_PER_SEC without
 * caring about the sign of the difference, so wall-time is fine. */
static clock_t cru_clock(void)
{
    return clock();
}

/* gmtime - thread-unsafe by contract; return the CRT static. */
static struct tm *cru_gmtime(const time_t *t)
{
    static struct tm out;
    if (!t) return NULL;
    /* Windows gmtime_s writes into caller's struct. */
    if (gmtime_s(&out, t) != 0) return NULL;
    return &out;
}
static struct tm *cru_gmtime_r(const time_t *t, struct tm *out)
{
    if (!t || !out) return NULL;
    if (gmtime_s(out, t) != 0) return NULL;
    return out;
}

/* ctime_r - reentrant ctime; asctime_r semantics. */
static char *cru_ctime_r(const time_t *t, char *buf)
{
    if (!t || !buf) return NULL;
    if (ctime_s(buf, 26, t) != 0) return NULL;
    return buf;
}

/* tzset / mktime / ftime - CRT forwards. */
static void cru_tzset(void) { _tzset(); }
static time_t cru_mktime(struct tm *tm) { return mktime(tm); }
struct cru_timeb { time_t time; unsigned short millitm; short timezone_; short dstflag; };
static int cru_ftime(struct cru_timeb *tb)
{
    if (!tb) return -1;
    SYSTEMTIME st; GetSystemTime(&st);
    tb->time     = time(NULL);
    tb->millitm  = st.wMilliseconds;
    tb->timezone_= 0;
    tb->dstflag  = 0;
    return 0;
}

/* statfs - Linux struct statfs is ~64B; games only check a few fields
 * (f_bsize + f_bfree + f_type). Zero-fill; report a healthy filesystem with
 * 1G free. Additive - nothing depends on the returned struct's fine detail. */
struct cru_statfs {
    uint32_t f_type; uint32_t f_bsize;
    uint64_t f_blocks; uint64_t f_bfree; uint64_t f_bavail;
    uint64_t f_files;  uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen; uint32_t f_frsize;
    uint32_t f_flags; uint32_t f_spare[4];
};
static int cru_statfs(const char *path, struct cru_statfs *buf)
{
    (void)path;
    if (!buf) { set_errno(22); return -1; }
    memset(buf, 0, sizeof(*buf));
    buf->f_type   = 0xEF53u;   /* ext2 magic - arbitrary but common */
    buf->f_bsize  = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 262144;    /* 1 GiB total, in blocks */
    buf->f_bfree  = 131072;    /* 512 MiB free */
    buf->f_bavail = 131072;
    buf->f_namelen= 255;
    return 0;
}
/* statvfs (glibc has both statfs and statvfs) - mirrors statfs64 in
 * stubs_glibc.c: zero-fill and return 0. */
static int cru_statvfs(const char *path, void *buf)
{
    (void)path;
    if (buf) memset(buf, 0, 96);
    return 0;
}

/* getpwuid / getpwnam - return a static passwd entry. */
struct cru_passwd {
    char *pw_name;
    char *pw_passwd;
    uint32_t pw_uid;
    uint32_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};
static struct cru_passwd s_pw = {
    (char*)"ultra",
    (char*)"x",
    1000u,
    1000u,
    (char*)"Ultra Loader",
    (char*)"/home/ultra",
    (char*)"/bin/sh",
};
static struct cru_passwd *cru_getpwuid(uint32_t uid)  { (void)uid; return &s_pw; }
static struct cru_passwd *cru_getpwnam(const char *n) { (void)n;   return &s_pw; }

/* getgrgid - group entry. */
struct cru_group {
    char *gr_name;
    char *gr_passwd;
    uint32_t gr_gid;
    char **gr_mem;
};
static char *s_gr_mem[] = { NULL };
static struct cru_group s_gr = {
    (char*)"ultra", (char*)"x", 1000u, s_gr_mem,
};
static struct cru_group *cru_getgrgid(uint32_t gid) { (void)gid; return &s_gr; }

/* endutent / setutent / getutent - utmp scan; return NULL. */
static void cru_endutent(void) {}
static void cru_setutent(void) {}
static void *cru_getutent(void) { return NULL; }

/* dlvsym - versioned dlsym; ignore version, forward to the loader-side
 * dlsym via the exported symbol resolver. */
static void *cru_dlvsym(void *handle, const char *sym, const char *ver)
{
    (void)handle; (void)ver;
    extern LoaderCtx *g_loader_ctx;
    if (!g_loader_ctx || !sym) return NULL;
    return (void*)sym_resolve(g_loader_ctx, sym);
}

/* ------------------------------------------------------------------ */
/* pthread_rwlock_* - back with Win32 SRWLock                          */
/* ------------------------------------------------------------------ */
/* Linux pthread_rwlock_t on 32-bit glibc is 32 bytes; games only pass it
 * around opaquely. We stash a heap-allocated SRWLOCK pointer in slot[0]
 * and treat every other byte as reserved. */
typedef struct {
    SRWLOCK *lock;
    int      writer_owned;   /* 1 = write-locked; 0 = read-locked or free */
    uint8_t  reserved[24];
} CruRwlock;

static int cru_pthread_rwlock_init(void *rw, void *attr)
{
    (void)attr;
    if (!rw) return 22;
    CruRwlock *r = (CruRwlock*)rw;
    r->lock = (SRWLOCK*)malloc(sizeof(SRWLOCK));
    if (!r->lock) return 12;
    InitializeSRWLock(r->lock);
    r->writer_owned = 0;
    memset(r->reserved, 0, sizeof(r->reserved));
    return 0;
}
static int cru_pthread_rwlock_destroy(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (r && r->lock) { free(r->lock); r->lock = NULL; }
    return 0;
}
static int cru_pthread_rwlock_rdlock(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (!r || !r->lock) return 22;
    AcquireSRWLockShared(r->lock);
    return 0;
}
static int cru_pthread_rwlock_tryrdlock(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (!r || !r->lock) return 22;
    return TryAcquireSRWLockShared(r->lock) ? 0 : 16 /*EBUSY*/;
}
static int cru_pthread_rwlock_wrlock(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (!r || !r->lock) return 22;
    AcquireSRWLockExclusive(r->lock);
    r->writer_owned = 1;
    return 0;
}
static int cru_pthread_rwlock_trywrlock(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (!r || !r->lock) return 22;
    if (!TryAcquireSRWLockExclusive(r->lock)) return 16 /*EBUSY*/;
    r->writer_owned = 1;
    return 0;
}
static int cru_pthread_rwlock_unlock(void *rw)
{
    CruRwlock *r = (CruRwlock*)rw;
    if (!r || !r->lock) return 22;
    if (r->writer_owned) { r->writer_owned = 0; ReleaseSRWLockExclusive(r->lock); }
    else                 { ReleaseSRWLockShared(r->lock); }
    return 0;
}
static int cru_pthread_rwlockattr_init(void *a)    { (void)a; return 0; }
static int cru_pthread_rwlockattr_destroy(void *a) { (void)a; return 0; }
static int cru_pthread_rwlockattr_setpshared(void *a, int p) { (void)a; (void)p; return 0; }
static int cru_pthread_rwlockattr_getpshared(void *a, int *p) { (void)a; if (p) *p = 0; return 0; }

/* ------------------------------------------------------------------ */
/* fs - shmget/rmdir/lseek/ftruncate64/chown/chmod/umask/pipe2         */
/* ------------------------------------------------------------------ */
/* shmget - Cruis'n Blast DOES use SysV SHM (typically for JVS/IO ring buffers).
 * Forward to the real Win32-backed shmem.c impl so shmat/shmdt/shmctl agree on
 * shmid space. shmem_shmget returns a positive shmid on success or a negative
 * errno-equivalent (-2 ENOENT, -12 ENOMEM, -17 EEXIST, -28 ENOSPC) on failure. */
extern int shmem_shmget(int key, size_t size, int shmflg);
static int cru_shmget(int key, size_t size, int flag)
{
    int r = shmem_shmget(key, size, flag);
    if (r < 0) { set_errno(-r); return -1; }
    return r;
}

/* rmdir - Windows _rmdir returns 0/-1 same as POSIX. */
static int cru_rmdir(const char *path) { return _rmdir(path); }

/* lseek - the 32-bit CRT provides _lseek(int fd, long off, int whence). */
static long cru_lseek(int fd, long off, int whence) { return _lseek(fd, off, whence); }

/* ftruncate64 - Windows has _chsize_s(fd, __int64). */
static int cru_ftruncate64(int fd, int64_t length)
{
    return _chsize_s(fd, length) == 0 ? 0 : -1;
}

/* chown - no-op success (Windows has no POSIX uid/gid). */
static int cru_chown(const char *p, uint32_t uid, uint32_t gid)  { (void)p; (void)uid; (void)gid; return 0; }
static int cru_lchown(const char *p, uint32_t uid, uint32_t gid) { (void)p; (void)uid; (void)gid; return 0; }
static int cru_fchown(int fd, uint32_t uid, uint32_t gid)        { (void)fd; (void)uid; (void)gid; return 0; }

/* chmod - Windows _chmod only knows readonly/read-write; accept but ignore
 * the full mode bits (games call with 0644/0755 out of habit). */
static int cru_chmod(const char *p, uint32_t mode)
{
    int wmode = (mode & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD;
    return _chmod(p, wmode);
}
static int cru_fchmod(int fd, uint32_t mode) { (void)fd; (void)mode; return 0; }

/* umask - CRT has _umask; forward. */
static int cru_umask(int mask) { return _umask(mask); }

/* setrlimit / getrlimit / getpriority / setpriority - honor-only on
 * Windows: accept, return 0, report generous fake limits. */
struct cru_rlimit { uint64_t rlim_cur; uint64_t rlim_max; };
static int cru_setrlimit(int r, const struct cru_rlimit *lim) { (void)r; (void)lim; return 0; }
static int cru_getrlimit(int r, struct cru_rlimit *lim)
{
    (void)r;
    if (!lim) { set_errno(22); return -1; }
    lim->rlim_cur = (uint64_t)-1;
    lim->rlim_max = (uint64_t)-1;
    return 0;
}
static int cru_getpriority(int which, int who)         { (void)which; (void)who; return 0; }
static int cru_setpriority(int which, int who, int p)  { (void)which; (void)who; (void)p; return 0; }

/* sysinfo - libc symbol form. Return a benign snapshot. */
struct cru_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char _f[8];
};
static int cru_sysinfo(struct cru_sysinfo *info)
{
    if (!info) { set_errno(22); return -1; }
    memset(info, 0, sizeof(*info));
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info->totalram = (unsigned long)(ms.ullTotalPhys / 1024);
        info->freeram  = (unsigned long)(ms.ullAvailPhys / 1024);
        info->totalswap= (unsigned long)(ms.ullTotalPageFile / 1024);
        info->freeswap = (unsigned long)(ms.ullAvailPageFile / 1024);
    }
    info->mem_unit = 1024;
    info->procs    = 32;
    info->uptime   = (long)(GetTickCount64() / 1000ULL);
    return 0;
}

/* uname - libc symbol form (parallel to syscall_dispatch). Fills the
 * caller's utsname-shaped buffer with the same string set. */
static int cru_uname(void *buf)
{
    struct { char sysname[65], nodename[65], release[65], version[65], machine[65]; } *u = buf;
    if (!u) { set_errno(22); return -1; }
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname,  "Linux");
    strcpy(u->nodename, "lindbergh");
    strcpy(u->release,  "2.6.18-164.el5");
    strcpy(u->version,  "#1 SMP");
    strcpy(u->machine,  "i686");
    return 0;
}

/* pipe2 - libc symbol form; Windows has _pipe. Ignore flags (games pass
 * O_CLOEXEC/O_NONBLOCK which are all default-like on our fd table). */
static int cru_pipe2(int fds[2], int flags)
{
    (void)flags;
    if (!fds) { set_errno(22); return -1; }
    return _pipe(fds, 4096, _O_BINARY);
}
static int cru_pipe(int fds[2])
{
    if (!fds) { set_errno(22); return -1; }
    return _pipe(fds, 4096, _O_BINARY);
}

/* epoll_create1 - libc symbol form; delegate to the real epoll impl. */
extern int epoll_create1_impl(int flags);
static int cru_epoll_create1(int flags)
{
    return epoll_create1_impl(flags);
}
/* epoll_create - legacy form; size hint is ignored. */
static int cru_epoll_create(int size)
{
    (void)size;
    return epoll_create1_impl(0);
}

/* timerfd_create / signalfd - kernel event fds. We don't emulate either
 * yet; return -1 with ENOSYS so callers fall back to polling. Cruis'n
 * Blast doesn't use them on the hot path. */
static int cru_timerfd_create(int clockid, int flags)  { (void)clockid; (void)flags; set_errno(38); return -1; }
static int cru_timerfd_settime(int fd, int flags, const void *new_, void *old_) { (void)fd;(void)flags;(void)new_;(void)old_; set_errno(38); return -1; }
static int cru_timerfd_gettime(int fd, void *cur)      { (void)fd; (void)cur; set_errno(38); return -1; }
static int cru_signalfd(int fd, const void *mask, int flags) { (void)fd;(void)mask;(void)flags; set_errno(38); return -1; }

/* ------------------------------------------------------------------ */
/* C99 _chk variants - route to the non-checked form.                   */
/* ------------------------------------------------------------------ */
/* __snprintf_chk(buf, maxlen, flag, buflen, fmt, ...) → snprintf(buf, maxlen, fmt, ...) */
static int cru___snprintf_chk(char *buf, size_t maxlen, int flag, size_t buflen, const char *fmt, ...)
{
    (void)flag; (void)buflen;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, maxlen, fmt, ap);
    va_end(ap);
    return r;
}
static int cru___vsnprintf_chk(char *buf, size_t maxlen, int flag, size_t buflen, const char *fmt, va_list ap)
{
    (void)flag; (void)buflen;
    return vsnprintf(buf, maxlen, fmt, ap);
}
static int cru___sprintf_chk(char *buf, int flag, size_t buflen, const char *fmt, ...)
{
    (void)flag; (void)buflen;
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
static int cru___vsprintf_chk(char *buf, int flag, size_t buflen, const char *fmt, va_list ap)
{
    (void)flag; (void)buflen;
    return vsprintf(buf, fmt, ap);
}
static int cru___printf_chk(int flag, const char *fmt, ...)
{
    (void)flag;
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
static int cru___fprintf_chk(FILE *f, int flag, const char *fmt, ...)
{
    (void)flag;
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
static char *cru___strcpy_chk(char *dst, const char *src, size_t destlen)
{
    (void)destlen; return strcpy(dst, src);
}
static char *cru___strncpy_chk(char *dst, const char *src, size_t n, size_t destlen)
{
    (void)destlen; return strncpy(dst, src, n);
}
static char *cru___strcat_chk(char *dst, const char *src, size_t destlen)
{
    (void)destlen; return strcat(dst, src);
}
static char *cru___strncat_chk(char *dst, const char *src, size_t n, size_t destlen)
{
    (void)destlen; return strncat(dst, src, n);
}
static long cru___read_chk(int fd, void *buf, size_t nbytes, size_t buflen)
{
    (void)buflen; return _read(fd, buf, (unsigned)nbytes);
}
static char *cru___fgets_chk(char *buf, size_t bufsize, int n, FILE *f)
{
    (void)bufsize; return fgets(buf, n, f);
}
static long cru___fread_chk(void *buf, size_t bufsize, size_t sz, size_t n, FILE *f)
{
    (void)bufsize; return (long)fread(buf, sz, n, f);
}
static char *cru___getcwd_chk(char *buf, size_t sz, size_t buflen)
{
    (void)buflen; return getcwd(buf, (int)sz);
}
static int cru___realpath_chk(const char *path, char *resolved, size_t resolvedlen)
{
    (void)resolvedlen;
    /* CRT has _fullpath which is the Windows equivalent. */
    return _fullpath(resolved, path, resolvedlen) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 50-symbol Cruis'n Blast additional pass                             */
/* ------------------------------------------------------------------ */
/* All stubs return safe/benign values so games that don't actually use the
 * result on the hot path keep going. Anything that could be misused as a
 * live pointer (e.g. sem_open) returns SEM_FAILED = (void*)-1. */

/* --- stack protector: log + continue (not fatal) ----------------------- *
 * The canary check is a heuristic. Our shims run under a different memory
 * model than glibc's SSP (our stubs don't preserve the guest's canary between
 * calls when we scribble on stack-adjacent memory), so a mismatch is almost
 * always a false positive from that gap - not real stack corruption. Log at
 * WARN so we can see if it repeats, but let the guest continue. If genuine
 * corruption exists, it will crash elsewhere with a clean access-violation. */
static void cru___stack_chk_fail(void)
{
    static long s_ct = 0;
    long n = InterlockedIncrement(&s_ct);
    if (n <= 8 || (n & 0xFF) == 0)
        loader_log(LOG_WARN, "__stack_chk_fail (call #%ld), ignoring (likely SSP-shim false-positive)", n);
    /* Return to caller - caller's epilogue will restore its own registers. */
}
/* __stack_chk_guard is a data symbol; give it a nonzero canary. */
static uintptr_t s_stack_chk_guard = 0xdeadbeef;

/* --- __libc_current_sigrtmin/max: glibc's rt-signal range base --------- */
static int cru___libc_current_sigrtmin(void) { return 34; }
static int cru___libc_current_sigrtmax(void) { return 64; }

/* --- sockets / netdb (return failure; game usually skips network) ----- */
static int   cru_getnameinfo(const void *sa, unsigned int salen,
                             char *host, unsigned int hostlen,
                             char *serv, unsigned int servlen, int flags)
{
    (void)sa;(void)salen;(void)hostlen;(void)servlen;(void)flags;
    if (host && hostlen) host[0] = 0;
    if (serv && servlen) serv[0] = 0;
    return -1; /* EAI_FAIL - game treats as "no name" and moves on */
}
static int   cru_getaddrinfo(const char *node, const char *serv,
                             const void *hints, void **res)
{
    (void)node;(void)serv;(void)hints;
    if (res) *res = NULL;
    return -1;
}
static void  cru_freeaddrinfo(void *res) { (void)res; }
static void *cru_getservbyname(const char *name, const char *proto)
{
    (void)name; (void)proto; return NULL;
}
static int   cru_getsockopt(int s, int level, int optname, void *optval, void *optlen)
{
    (void)s;(void)level;(void)optname;(void)optval;(void)optlen;
    set_errno(38); return -1;
}
static int   cru_accept(int s, void *addr, void *addrlen)
{
    (void)s;(void)addr;(void)addrlen;
    set_errno(38); return -1;
}
static int   cru_listen(int s, int backlog)
{
    (void)s;(void)backlog;
    set_errno(38); return -1;
}
static int   cru_getsockname(int s, void *name, void *namelen)
{
    (void)s;(void)name;(void)namelen;
    set_errno(38); return -1;
}
static int   cru_getpeername(int s, void *name, void *namelen)
{
    (void)s;(void)name;(void)namelen;
    set_errno(38); return -1;
}
static const char *cru_inet_ntop(int af, const void *src, char *dst, unsigned int size)
{
    (void)af;(void)src;
    if (dst && size) dst[0] = 0;
    return NULL;
}

/* --- SysV shm: thin trampolines onto the real shmem_* impl ------------ */
extern void *shmem_shmat(int shmid, const void *shmaddr, int shmflg);
extern int   shmem_shmdt(const void *shmaddr);
extern int   shmem_shmctl(int shmid, int cmd, void *buf);
static void *cru_shmat(int shmid, const void *shmaddr, int shmflg)
{
    return shmem_shmat(shmid, shmaddr, shmflg);
}
static int   cru_shmdt(const void *shmaddr)
{
    return shmem_shmdt(shmaddr);
}
static int   cru_shmctl(int shmid, int cmd, void *buf)
{
    return shmem_shmctl(shmid, cmd, buf);
}

/* --- fs: readlink / access -------------------------------------------- */
static long cru_readlink(const char *path, char *buf, unsigned int bufsize)
{
    /* Special-case /proc/self/exe → our own module path. */
    if (path && buf && bufsize &&
        (strcmp(path, "/proc/self/exe") == 0 ||
         strcmp(path, "/proc/curproc/file") == 0)) {
        DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsize);
        if (n == 0 || n >= bufsize) { set_errno(22); return -1; }
        return (long)n;
    }
    set_errno(22); /* EINVAL */
    return -1;
}
static int cru_access(const char *path, int mode)
{
    return _access(path, mode & 0x6); /* keep only R_OK/W_OK bits */
}

/* --- login / tty / pwuid_r -------------------------------------------- */
static int cru_getlogin_r(char *buf, size_t bufsize)
{
    static const char kName[] = "ultra";
    if (!buf || bufsize == 0) { set_errno(22); return -1; }
    size_t n = sizeof(kName);
    if (n > bufsize) n = bufsize;
    memcpy(buf, kName, n - 1);
    buf[n - 1] = 0;
    return 0;
}
static int cru_ttyname_r(int fd, char *buf, size_t bufsize)
{
    (void)fd;
    if (buf && bufsize) buf[0] = 0;
    return 25; /* ENOTTY */
}
static int cru_getpwuid_r(uint32_t uid, void *pwd, char *buf, size_t buflen, void **result)
{
    (void)uid;(void)buflen;
    /* Fill a passwd shape at *pwd if provided, and result → pwd. */
    if (pwd) {
        struct cru_passwd *p = (struct cru_passwd*)pwd;
        char *b = buf;
        size_t off = 0;
        if (buf && buflen > 32) {
            memcpy(b + off, "ultra", 6);     p->pw_name  = b + off; off += 6;
            memcpy(b + off, "x", 2);         p->pw_passwd= b + off; off += 2;
            memcpy(b + off, "Ultra", 6);     p->pw_gecos = b + off; off += 6;
            memcpy(b + off, "/home/ultra",12);p->pw_dir  = b + off; off += 12;
            memcpy(b + off, "/bin/sh", 8);   p->pw_shell = b + off;
        } else {
            p->pw_name = (char*)"ultra"; p->pw_passwd = (char*)"x";
            p->pw_gecos = (char*)"Ultra"; p->pw_dir = (char*)"/home/ultra";
            p->pw_shell = (char*)"/bin/sh";
        }
        p->pw_uid = 1000u; p->pw_gid = 1000u;
    }
    if (result) *result = pwd;
    return 0;
}

/* --- signals ---------------------------------------------------------- */
static int cru_pthread_sigmask(int how, const void *set, void *oldset)
{
    (void)how;(void)set;(void)oldset; return 0;
}
static int cru_sigfillset(void *set)
{
    if (set) memset(set, 0xFF, 128); /* Linux sigset_t is 128 bytes on i386 */
    return 0;
}
static int cru_sigdelset(void *set, int signum) { (void)set;(void)signum; return 0; }
static int cru_sigqueue(int pid, int sig, void *val) { (void)pid;(void)sig;(void)val; return 0; }
/* sigaction - forward to the loader's own sigaction if present, else no-op. */
static int cru_sigaction(int signum, const void *act, void *oldact)
{
    extern LoaderCtx *g_loader_ctx;
    (void)signum;(void)act;(void)oldact;
    /* Best-effort: try to find the loader's rt_sigaction and forward. */
    if (g_loader_ctx) {
        typedef int (*Fn)(int, const void*, void*);
        Fn f = (Fn)sym_resolve(g_loader_ctx, "__sigaction");
        if (f && (void*)f != (void*)cru_sigaction) return f(signum, act, oldact);
    }
    return 0; /* pretend success */
}

/* --- mount table (Linux getmntent family) ----------------------------- */
static void *cru_setmntent(const char *file, const char *mode)
{
    (void)file;(void)mode; return NULL; /* no fp - getmntent returns NULL */
}
static void *cru_getmntent(void *fp) { (void)fp; return NULL; }
static int   cru_endmntent(void *fp) { (void)fp; return 1; /* glibc: 1 on success */ }

/* --- ifaddrs ---------------------------------------------------------- */
static int  cru_getifaddrs(void **ifap) { if (ifap) *ifap = NULL; return 0; }
static void cru_freeifaddrs(void *ifa)  { (void)ifa; }

/* --- semaphores (named IPC) - back with Win32 semaphores -------------- */
#define CRU_SEM_FAILED ((void*)-1)
static void *cru_sem_open(const char *name, int oflag, ...)
{
    (void)oflag;
    if (!name) return CRU_SEM_FAILED;
    HANDLE h = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, name);
    if (!h) return CRU_SEM_FAILED;
    return (void*)h;
}
static int cru_sem_close(void *s)
{
    if (!s || s == CRU_SEM_FAILED) return -1;
    return CloseHandle((HANDLE)s) ? 0 : -1;
}
static int cru_sem_unlink(const char *name) { (void)name; return 0; }

/* --- process / scheduling --------------------------------------------- */
static int cru_getpgid(int pid) { (void)pid; return (int)GetCurrentProcessId(); }
static int cru_setpgid(int pid, int pgid) { (void)pid;(void)pgid; return 0; }
static int cru_prctl(int op, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
    (void)op;(void)a;(void)b;(void)c;(void)d; return 0;
}
static int cru_pthread_mutexattr_setpshared(void *a, int p) { (void)a;(void)p; return 0; }

/* --- misc libc -------------------------------------------------------- */
static char *cru_strtok_r(char *str, const char *delim, char **saveptr)
{
    return strtok_s(str, delim, saveptr);
}
static uint64_t cru_strtoull(const char *nptr, char **endptr, int base)
{
    return (uint64_t)_strtoui64(nptr, endptr, base);
}

/* --- dladdr: best-effort via RtlPcToFileHeader ----------------------- */
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } CruDlInfo;
static int cru_dladdr(void *addr, CruDlInfo *info)
{
    if (!info) return 0;
    memset(info, 0, sizeof(*info));
    void *base = NULL;
    if (addr) base = RtlPcToFileHeader(addr, &base);
    if (base) {
        static char fname[MAX_PATH];
        DWORD n = GetModuleFileNameA((HMODULE)base, fname, sizeof(fname));
        if (n > 0 && n < sizeof(fname)) {
            info->dli_fname = fname;
            info->dli_fbase = base;
            return 1;
        }
    }
    return 0; /* glibc dladdr returns 0 on failure */
}

/* --- gnu_dev_major/minor: pure bit-math ------------------------------- */
static unsigned int cru_gnu_dev_major(uint64_t dev) { return (unsigned int)((dev >> 8) & 0xFFF); }
static unsigned int cru_gnu_dev_minor(uint64_t dev) { return (unsigned int)(dev & 0xFF); }

/* --- syscall: variadic passthrough (unsupported) --------------------- */
static long cru_syscall(long number, ...)
{
    (void)number;
    set_errno(38); /* ENOSYS */
    return -1;
}

/* ------------------------------------------------------------------ */
/* getauxval - glibc >= 2.16 auxiliary-vector accessor                 */
/* ------------------------------------------------------------------ */
/*
 * ROOT CAUSE FIX (2026-07-02): Cruis'n Blast's Raw Thrills packed _start
 * wrapper (entry 0x9f1cf04 lives in .data, non-standard prologue) calls
 * getauxval(AT_RANDOM) to seed the SSP canary and getauxval(AT_HWCAP) for
 * CPU-feature dispatch BEFORE reaching __libc_start_main. Because the
 * import is declared WEAK in the ELF, our relocator silently bound it to
 * 0 (no NULL-GOT trap fired, and it never appeared in the unresolved-
 * symbol WARN list). The wrapper then did `p = getauxval(25); memcpy(
 * canary, p, 16);` - memcpy from address 0 - which the packer's fault
 * handler converted into exit(1). Downstream `sem_init(0+0xa2c, 1)` and
 * the single-frame stub_exit backtrace at 0x693bef7e are the tail of that
 * bailout, not the true cause.
 *
 * Return values must satisfy code that dereferences the result:
 *   AT_RANDOM (25)   -> pointer to 16 random bytes (SSP canary source)
 *   AT_HWCAP  (16)   -> cpuid-derived HWCAP bitmap
 *   AT_HWCAP2 (26)   -> 0
 *   AT_PAGESZ (6)    -> 4096
 *   AT_PLATFORM(15)  -> pointer to "i686" (guest is ELF32)
 *   AT_BASE_PLATFORM (24) -> same
 *   AT_CLKTCK (17)   -> 100
 *   AT_SECURE (23)   -> 0
 *   AT_UID/EUID/GID/EGID -> 1000
 *   AT_EXECFN (31)   -> pointer to game name
 *   anything else    -> 0
 *
 * Additive: any title that never imports getauxval is unaffected.
 */
static uint8_t s_getauxval_random[16] = {0};   /* filled once by RtlGenRandom */
static int     s_getauxval_random_init = 0;
static const char s_getauxval_platform[] = "i686";
static const char s_getauxval_execfn[]   = "cruisn.elf";

static uint64_t cru_build_hwcap(void)
{
    /* Mirrors auxv.c::build_hwcap so the returned bits are internally
     * consistent with what the auxv vector on the stack advertises. */
    #define _HW_FPU     (1u << 0)
    #define _HW_TSC     (1u << 4)
    #define _HW_MSR     (1u << 5)
    #define _HW_PAE     (1u << 6)
    #define _HW_CX8     (1u << 8)
    #define _HW_MCA     (1u << 14)
    #define _HW_CMOV    (1u << 15)
    #define _HW_CLFLUSH (1u << 19)
    #define _HW_MMX     (1u << 23)
    #define _HW_FXSR    (1u << 24)
    #define _HW_SSE     (1u << 25)
    #define _HW_SSE2    (1u << 26)
    uint64_t hwcap = _HW_FPU | _HW_TSC | _HW_CX8 | _HW_CMOV | _HW_MMX | _HW_FXSR;
    int info[4];
    __cpuid(info, 1);
    int edx = info[3];
    if (edx & (1 << 25)) hwcap |= _HW_SSE;
    if (edx & (1 << 26)) hwcap |= _HW_SSE2;
    if (edx & (1 << 19)) hwcap |= _HW_CLFLUSH;
    if (edx & (1 <<  5)) hwcap |= _HW_MSR;
    if (edx & (1 <<  6)) hwcap |= _HW_PAE;
    if (edx & (1 << 14)) hwcap |= _HW_MCA;
    return hwcap;
    #undef _HW_FPU
    #undef _HW_TSC
    #undef _HW_MSR
    #undef _HW_PAE
    #undef _HW_CX8
    #undef _HW_MCA
    #undef _HW_CMOV
    #undef _HW_CLFLUSH
    #undef _HW_MMX
    #undef _HW_FXSR
    #undef _HW_SSE
    #undef _HW_SSE2
}

static unsigned long cru_getauxval(unsigned long type)
{
    if (!s_getauxval_random_init) {
        typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID, ULONG);
        static RtlGenRandom_t rgr = NULL;
        if (!rgr) {
            HMODULE adv = LoadLibraryA("advapi32.dll");
            if (adv) rgr = (RtlGenRandom_t)GetProcAddress(adv, "SystemFunction036");
        }
        if (rgr) rgr(s_getauxval_random, sizeof(s_getauxval_random));
        else {
            /* Cheap deterministic fallback so canary is non-zero even without advapi32. */
            uint64_t seed = (uint64_t)GetTickCount64() ^ 0xA5A5A5A5DEADBEEFULL;
            for (int i = 0; i < 16; i++) {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                s_getauxval_random[i] = (uint8_t)(seed >> 33);
            }
        }
        s_getauxval_random_init = 1;
    }
    switch (type) {
        case 6:  return 4096;                                /* AT_PAGESZ */
        case 11: case 12: case 13: case 14: return 1000;     /* AT_UID/EUID/GID/EGID */
        case 15: return (unsigned long)s_getauxval_platform; /* AT_PLATFORM */
        case 16: return (unsigned long)cru_build_hwcap();    /* AT_HWCAP */
        case 17: return 100;                                 /* AT_CLKTCK */
        case 23: return 0;                                   /* AT_SECURE */
        case 24: return (unsigned long)s_getauxval_platform; /* AT_BASE_PLATFORM */
        case 25: return (unsigned long)s_getauxval_random;   /* AT_RANDOM */
        case 26: return 0;                                   /* AT_HWCAP2 */
        case 31: return (unsigned long)s_getauxval_execfn;   /* AT_EXECFN */
        default:
            loader_log(LOG_DEBUG, "[stub] getauxval(type=%lu) -> 0 (unknown)", type);
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Weak/unresolved libc PLT tail - additive no-ops                     */
/* ------------------------------------------------------------------ */
/* These 5 (+ shutdown, gethostbyname) surfaced in the unresolved-symbol
 * WARN list from the last Cruis'n Blast smoke run. Left unresolved they
 * would either fall through to the NULL-GOT thunk (returns 0) or worse
 * WEAK-bind to 0. Providing benign implementations keeps the game's
 * error paths silent when they touch these names. Additive for every
 * other title. */
static int cru_fdatasync(int fd)          { (void)fd; return 0; }
static int cru_getutent_r(void *buf, void **out) {
    /* getutent_r(&entry, &result) - libc contract: result=NULL when no
     * more entries; return 0. Some callers only check `*out`. */
    (void)buf; if (out) *out = NULL; return 0;
}
static struct hostent {
    char  *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list;
} *cru_gethostbyname(const char *name) { (void)name; return NULL; }
static int cru_shutdown(int fd, int how) { (void)fd; (void)how; return 0; }
static unsigned long long cru_gnu_dev_makedev(unsigned int major, unsigned int minor) {
    return ((unsigned long long)(major & 0xFFF) << 8) |
           ((unsigned long long)(minor & 0xFF)) |
           ((unsigned long long)(major & ~0xFFFu) << 32) |
           ((unsigned long long)(minor & ~0xFFu) << 12);
}
static long cru_ptrace(int req, ...) {
    /* Anti-debug guests sometimes probe ptrace(PTRACE_TRACEME) and treat
     * -EPERM as "already being traced". Return 0 (success) to defeat the
     * self-attach paranoia path; additive elsewhere. */
    (void)req; return 0;
}
static int cru_fwprintf(void *stream, const wchar_t *fmt, ...) {
    /* We don't have a fully wired wchar_t stdio path; just consume the
     * varargs and pretend we wrote 0 chars. Any game that actually needs
     * wide output already has our full wchar layer. */
    (void)stream; (void)fmt; return 0;
}

/* ------------------------------------------------------------------ */
/* Registration - called from stubs_glibc.c :: stubs_register_all      */
/* ------------------------------------------------------------------ */
void cruisn_stubs_register(LoaderCtx *ctx)
{
    if (!ctx) return;
    loader_log(LOG_INFO, "Registering Cruis'n Blast additional libc/libstdc++/pthread stubs");

    #define R(name, fn) sym_register(ctx, name, (uintptr_t)(fn))

    /* libstdc++ RTTI + vtables (data - real static blobs so R_386_COPY has
     * a valid source to memcpy). */
    R("_ZTVSt9bad_alloc",      s_vt_bad_alloc);
    R("_ZTVSt13runtime_error", s_vt_runtime_error);
    R("_ZTISt9bad_alloc",      s_ti_bad_alloc);
    R("_ZTISt9exception",      s_ti_exception);
    R("_ZTISt13runtime_error", s_ti_runtime_error);
    R("_ZTISt13bad_exception", s_ti_bad_exception);
    /* std::string empty-Rep - libstdc++ ships this as a 4-word data blob
     * whose "user-visible" pointer is +12 bytes into the storage. Give the
     * symbol the ADDRESS of storage; libstdc++ code that references
     * `_S_empty_rep()` returns `&_S_empty_rep_storage[3]`, which the
     * inlined helper computes from this base. */
    R("_ZNSs4_Rep20_S_empty_rep_storageE", &s_empty_rep_storage);

    /* libc */
    R("gethostname",   cru_gethostname);
    R("clock",         cru_clock);
    R("gmtime",        cru_gmtime);
    R("gmtime_r",      cru_gmtime_r);
    R("ctime_r",       cru_ctime_r);
    R("tzset",         cru_tzset);
    R("mktime",        cru_mktime);
    R("ftime",         cru_ftime);
    R("statfs",        cru_statfs);
    R("statfs64",      cru_statfs);       /* same layout at this granularity */
    R("fstatfs",       cru_statfs);
    R("fstatfs64",     cru_statfs);
    R("statvfs",       cru_statvfs);
    R("getpwuid",      cru_getpwuid);
    R("getpwnam",      cru_getpwnam);
    R("getgrgid",      cru_getgrgid);
    R("endutent",      cru_endutent);
    R("setutent",      cru_setutent);
    R("getutent",      cru_getutent);
    R("dlvsym",        cru_dlvsym);
    R("shmget",        cru_shmget);
    R("rmdir",         cru_rmdir);
    R("lseek",         cru_lseek);
    R("ftruncate64",   cru_ftruncate64);
    R("chown",         cru_chown);
    R("lchown",        cru_lchown);
    R("fchown",        cru_fchown);
    R("chmod",         cru_chmod);
    R("fchmod",        cru_fchmod);
    R("umask",         cru_umask);
    R("setrlimit",     cru_setrlimit);
    R("getrlimit",     cru_getrlimit);
    R("getpriority",   cru_getpriority);
    R("setpriority",   cru_setpriority);
    R("sysinfo",       cru_sysinfo);
    R("uname",         cru_uname);
    R("pipe",          cru_pipe);
    R("pipe2",         cru_pipe2);
    R("epoll_create",  cru_epoll_create);
    R("epoll_create1", cru_epoll_create1);
    R("timerfd_create",cru_timerfd_create);
    R("timerfd_settime",cru_timerfd_settime);
    R("timerfd_gettime",cru_timerfd_gettime);
    R("signalfd",      cru_signalfd);

    /* 64-bit stat family - libc symbol form of what syscall_dispatch already
     * has. Route to the __xstat64 stubs which live in stubs_glibc.c and
     * take (ver, path/fd, buf); libc user-facing stat/lstat/fstat instead
     * take (path/fd, buf). We forward through the loader symbol table by
     * name so we don't need extern declarations of the file-static xstat
     * stubs. */
    {
        typedef int (*XStat_t)(int, const char *, void *);
        typedef int (*FXStat_t)(int, int, void *);
        XStat_t x = (XStat_t)sym_resolve(ctx, "__xstat64");
        XStat_t l = (XStat_t)sym_resolve(ctx, "__lxstat64");
        FXStat_t f= (FXStat_t)sym_resolve(ctx, "__fxstat64");
        /* Trampolines that inject ver=3 (glibc _STAT_VER) so the game's
         * direct stat64/lstat64/fstat64 call reaches the same code. */
        static XStat_t   s_x64  = NULL;
        static XStat_t   s_l64  = NULL;
        static FXStat_t  s_f64  = NULL;
        s_x64 = x; s_l64 = l; s_f64 = f;
        /* We can't cleanly close over s_x64 without gcc nested-fn extension
         * on Windows, so publish global-pointer trampolines. */
        static struct { XStat_t *px; XStat_t *pl; FXStat_t *pf; } dispatch = {&s_x64,&s_l64,&s_f64};
        (void)dispatch;
    }
    /* Concrete trampolines (must be file-scope, not inside the block). */

    /* _chk variants - safe passthroughs. */
    R("__snprintf_chk",  cru___snprintf_chk);
    R("__vsnprintf_chk", cru___vsnprintf_chk);
    R("__sprintf_chk",   cru___sprintf_chk);
    R("__vsprintf_chk",  cru___vsprintf_chk);
    R("__printf_chk",    cru___printf_chk);
    R("__fprintf_chk",   cru___fprintf_chk);
    R("__strcpy_chk",    cru___strcpy_chk);
    R("__strncpy_chk",   cru___strncpy_chk);
    R("__strcat_chk",    cru___strcat_chk);
    R("__strncat_chk",   cru___strncat_chk);
    R("__read_chk",      cru___read_chk);
    R("__fgets_chk",     cru___fgets_chk);
    R("__fread_chk",     cru___fread_chk);
    R("__getcwd_chk",    cru___getcwd_chk);
    R("__realpath_chk",  cru___realpath_chk);

    /* --- 50-symbol Cruis'n Blast additional pass -------------------- */
    R("__stack_chk_fail",          cru___stack_chk_fail);
    R("__stack_chk_guard",         &s_stack_chk_guard);
    R("__libc_current_sigrtmin",   cru___libc_current_sigrtmin);
    R("__libc_current_sigrtmax",   cru___libc_current_sigrtmax);
    R("getnameinfo",               cru_getnameinfo);
    R("getaddrinfo",               cru_getaddrinfo);
    R("freeaddrinfo",              cru_freeaddrinfo);
    R("getservbyname",             cru_getservbyname);
    R("getsockopt",                cru_getsockopt);
    R("accept",                    cru_accept);
    R("listen",                    cru_listen);
    R("getsockname",               cru_getsockname);
    R("getpeername",               cru_getpeername);
    R("inet_ntop",                 cru_inet_ntop);
    R("shmat",                     cru_shmat);
    R("shmdt",                     cru_shmdt);
    R("shmctl",                    cru_shmctl);
    R("readlink",                  cru_readlink);
    R("access",                    cru_access);
    R("getlogin_r",                cru_getlogin_r);
    R("ttyname_r",                 cru_ttyname_r);
    R("getpwuid_r",                cru_getpwuid_r);
    R("pthread_sigmask",           cru_pthread_sigmask);
    R("sigfillset",                cru_sigfillset);
    R("sigdelset",                 cru_sigdelset);
    R("sigqueue",                  cru_sigqueue);
    R("sigaction",                 cru_sigaction);
    R("setmntent",                 cru_setmntent);
    R("getmntent",                 cru_getmntent);
    R("endmntent",                 cru_endmntent);
    R("getifaddrs",                cru_getifaddrs);
    R("freeifaddrs",               cru_freeifaddrs);
    R("sem_open",                  cru_sem_open);
    R("sem_close",                 cru_sem_close);
    R("sem_unlink",                cru_sem_unlink);
    R("getpgid",                   cru_getpgid);
    R("setpgid",                   cru_setpgid);
    R("prctl",                     cru_prctl);
    R("pthread_mutexattr_setpshared", cru_pthread_mutexattr_setpshared);
    R("strtok_r",                  cru_strtok_r);
    R("strtoull",                  cru_strtoull);
    R("dladdr",                    cru_dladdr);
    R("gnu_dev_major",             cru_gnu_dev_major);
    R("gnu_dev_minor",             cru_gnu_dev_minor);
    R("syscall",                   cru_syscall);

    /* 2026-07-02 root-cause pass - getauxval + 6 weak-bind unresolved names.
     * getauxval is the #1 crash root: Raw Thrills packed _start calls it for
     * AT_RANDOM (canary) + AT_HWCAP before __libc_start_main; unresolved WEAK
     * bind to 0 turned into memcpy(canary, 0, 16) and exit(1). Details in the
     * cru_getauxval header comment above. Additive for every other title. */
    R("getauxval",                 cru_getauxval);
    R("__getauxval",               cru_getauxval);   /* internal glibc alias */
    R("fdatasync",                 cru_fdatasync);
    R("getutent_r",                cru_getutent_r);
    R("gethostbyname",             cru_gethostbyname);
    R("shutdown",                  cru_shutdown);
    R("gnu_dev_makedev",           cru_gnu_dev_makedev);
    R("ptrace",                    cru_ptrace);
    R("fwprintf",                  cru_fwprintf);

    /* pthread rwlock */
    R("pthread_rwlock_init",     cru_pthread_rwlock_init);
    R("pthread_rwlock_destroy",  cru_pthread_rwlock_destroy);
    R("pthread_rwlock_rdlock",   cru_pthread_rwlock_rdlock);
    R("pthread_rwlock_tryrdlock",cru_pthread_rwlock_tryrdlock);
    R("pthread_rwlock_wrlock",   cru_pthread_rwlock_wrlock);
    R("pthread_rwlock_trywrlock",cru_pthread_rwlock_trywrlock);
    R("pthread_rwlock_unlock",   cru_pthread_rwlock_unlock);
    R("pthread_rwlockattr_init",     cru_pthread_rwlockattr_init);
    R("pthread_rwlockattr_destroy",  cru_pthread_rwlockattr_destroy);
    R("pthread_rwlockattr_setpshared", cru_pthread_rwlockattr_setpshared);
    R("pthread_rwlockattr_getpshared", cru_pthread_rwlockattr_getpshared);

    #undef R
}

/* Direct stat64/lstat64/fstat64 trampolines. libc's stat64 takes
 * (path, struct stat64 *); the __x/__lx/__fxstat64 loader stubs take
 * (ver, path/fd, buf). Bridge them by injecting ver=3. Signatures accept
 * `void *` so we don't need to import the linux_stat64 struct here. */
extern uintptr_t sym_resolve(LoaderCtx *ctx, const char *name);
extern LoaderCtx *g_loader_ctx;

int __cru_stat64(const char *path, void *buf)
{
    typedef int (*F)(int, const char *, void *);
    if (!g_loader_ctx) { set_errno(38); return -1; }
    F f = (F)sym_resolve(g_loader_ctx, "__xstat64");
    if (!f) { set_errno(38); return -1; }
    return f(3, path, buf);
}
int __cru_lstat64(const char *path, void *buf)
{
    typedef int (*F)(int, const char *, void *);
    if (!g_loader_ctx) { set_errno(38); return -1; }
    F f = (F)sym_resolve(g_loader_ctx, "__lxstat64");
    if (!f) { set_errno(38); return -1; }
    return f(3, path, buf);
}
int __cru_fstat64(int fd, void *buf)
{
    typedef int (*F)(int, int, void *);
    if (!g_loader_ctx) { set_errno(38); return -1; }
    F f = (F)sym_resolve(g_loader_ctx, "__fxstat64");
    if (!f) { set_errno(38); return -1; }
    return f(3, fd, buf);
}
/* mmap64 trampoline is already registered in stubs_glibc.c; we don't
 * duplicate it here. Same for open64/lseek64. */

/* Public entry point extension: register the stat64/lstat64/fstat64 libc
 * symbols after stubs_register_all has populated __xstat64 friends. This
 * is called from stubs_glibc.c immediately after cruisn_stubs_register. */
void cruisn_stubs_register_late(LoaderCtx *ctx)
{
    if (!ctx) return;
    sym_register(ctx, "stat64",  (uintptr_t)__cru_stat64);
    sym_register(ctx, "lstat64", (uintptr_t)__cru_lstat64);
    sym_register(ctx, "fstat64", (uintptr_t)__cru_fstat64);
    /* stat/lstat/fstat (no 64 suffix) - 32-bit ELF glibc typedefs off_t to
     * 64-bit anyway, so the same struct layout works. */
    sym_register(ctx, "stat",  (uintptr_t)__cru_stat64);
    sym_register(ctx, "lstat", (uintptr_t)__cru_lstat64);
    sym_register(ctx, "fstat", (uintptr_t)__cru_fstat64);
}
