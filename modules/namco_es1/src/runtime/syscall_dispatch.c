#include "compat32.h"
/*
 * syscall_dispatch.c
 *
 * Handles raw syscall instructions that Option B rewrites to call here.
 * Maps Linux x86-64 syscall numbers to Win32 equivalents.
 *
 * Linux syscall ABI (x86-64):
 *   rax = number, rdi=a0, rsi=a1, rdx=a2, r10=a3, r8=a4, r9=a5
 *   Return value in rax (-errno on error)
 *
 * We receive: nr, a0..a5 already shuffled by the trampoline.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "loader.h"
#include "vfs.h"
#include "signals.h"
#include "shmem.h"
#include "threads.h"
#include "libgl_stub.h"
#include "unix_socket.h"
#include "epoll.h"
#include "nvidia_stub.h"
#include "iso_loader.h"

/* Linux x86-64 syscall numbers (kernel 2.6 / 3.x, relevant subset) */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_poll            7
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_ioctl           16
#define SYS_pread64         17
#define SYS_pwrite64        18
#define SYS_access          21
#define SYS_pipe            22
#define SYS_select          23
#define SYS_sched_yield     24
#define SYS_mremap          25
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_nanosleep       35
#define SYS_getpid          39
#define SYS_socket          41
#define SYS_connect         42
#define SYS_accept          43
#define SYS_sendto          44
#define SYS_recvfrom        45
#define SYS_bind            49
#define SYS_listen          50
#define SYS_getsockname     51
#define SYS_fork            57
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_fcntl           72
#define SYS_ftruncate       77
#define SYS_getcwd          79
#define SYS_mkdir           83
#define SYS_unlink          87
#define SYS_readlink        89
#define SYS_chmod           90
#define SYS_getuid          102
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_getppid         110
#define SYS_setsid          112
#define SYS_setuid          105
#define SYS_setgid          106
#define SYS_rt_sigreturn    15
#define SYS_clone           56
#define SYS_futex           202
#define SYS_sched_getaffinity 204
#define SYS_exit_group      231
#define SYS_clock_gettime   228
#define SYS_clock_getres    229
#define SYS_tgkill          234
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_set_robust_list 273
#define SYS_eventfd2        290
#define SYS_epoll_create1   291
#define SYS_epoll_ctl       233
#define SYS_epoll_wait      232
#define SYS_pipe2           293
#define SYS_getrandom       318

/* Signal syscalls */
#define SYS_alarm           37
#define SYS_setitimer       38
#define SYS_getitimer       36
#define SYS_sigaltstack     131
#define SYS_kill            62
#define SYS_tgkill_nr       234

/* SysV IPC */
#define SYS_shmget          29
#define SYS_shmat           30
#define SYS_shmctl          31
#define SYS_shmdt           67

/* Process identity */
#define SYS_gettid          186
#define SYS_set_tid_address 218
#define SYS_getpgrp         111
#define SYS_setsid_nr       112

/* File / stat */
#define SYS_readlink_nr     89
#define SYS_stat_nr         4
#define SYS_lstat_nr        6
#define SYS_chmod_nr        90
#define SYS_mkdir_nr        83
#define SYS_unlink_nr       87
#define SYS_rename          82
#define SYS_symlink         88
#define SYS_link            86
#define SYS_dup3            292
#define SYS_pread64_nr      17
#define SYS_pwrite64_nr     18
#define SYS_sendfile        40

/* Misc */
#define SYS_prctl           157
#define SYS_arch_prctl      158
#define SYS_uname           63
#define SYS_sysinfo         99
#define SYS_times           100
#define SYS_getrusage       98
#define SYS_umask           95
#define SYS_chdir           80
#define SYS_fchdir          81
#define SYS_getdents64      217
#define SYS_inotify_init1   294
#define SYS_eventfd         284
#define SYS_timerfd_create  283
#define SYS_madvise         28
#define SYS_mincore         27
#define SYS_msync           26
#define SYS_sendmsg         46
#define SYS_recvmsg         47
#define SYS_socketpair      53
#define SYS_getsockopt      55
#define SYS_setsockopt      54
#define SYS_shutdown        48

/* Linux errno values */
#define LINUX_EPERM   1
#define LINUX_ENOENT  2
#define LINUX_EIO     5
#define LINUX_EBADF   9
#define LINUX_ENOMEM  12
#define LINUX_EACCES  13
#define LINUX_EFAULT  14
#define LINUX_EINVAL  22
#define LINUX_ENOSYS  38
#define LINUX_ENOTSUP 95

static int64_t linux_errno(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:    return -LINUX_ENOENT;
    case ERROR_ACCESS_DENIED:     return -LINUX_EACCES;
    case ERROR_INVALID_HANDLE:    return -LINUX_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: return -LINUX_ENOMEM;
    case ERROR_INVALID_PARAMETER: return -LINUX_EINVAL;
    default:                      return -LINUX_EIO;
    }
}

/* ------------------------------------------------------------------ */
/* fd translation table (Linux fd <-> Win32 HANDLE)                   */
/* ------------------------------------------------------------------ */
#define MAX_FDS 1024
static HANDLE g_fds[MAX_FDS];

static void fds_init(void)
{
    g_fds[0] = GetStdHandle(STD_INPUT_HANDLE);
    g_fds[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    g_fds[2] = GetStdHandle(STD_ERROR_HANDLE);
    for (int i = 3; i < MAX_FDS; i++) g_fds[i] = INVALID_HANDLE_VALUE;
}

static int fd_alloc(HANDLE h)
{
    for (int i = 3; i < MAX_FDS; i++) {
        if (g_fds[i] == INVALID_HANDLE_VALUE) {
            g_fds[i] = h;
            return i;
        }
    }
    return -1;
}

static HANDLE fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return INVALID_HANDLE_VALUE;
    return g_fds[fd];
}

/* ------------------------------------------------------------------ */
/* Linux open flags -> Win32                                           */
/* ------------------------------------------------------------------ */
#define LINUX_O_RDONLY   0
#define LINUX_O_WRONLY   1
#define LINUX_O_RDWR     2
#define LINUX_O_CREAT    0x40
#define LINUX_O_TRUNC    0x200
#define LINUX_O_APPEND   0x400
#define LINUX_O_NONBLOCK 0x800
#define LINUX_O_CLOEXEC  0x80000

static DWORD open_access(int flags)
{
    int mode = flags & 3;
    if (mode == LINUX_O_RDONLY) return GENERIC_READ;
    if (mode == LINUX_O_WRONLY) return GENERIC_WRITE;
    return GENERIC_READ | GENERIC_WRITE;
}

static DWORD open_create(int flags)
{
    if (flags & LINUX_O_CREAT) {
        return (flags & LINUX_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    }
    return (flags & LINUX_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
}

/* ------------------------------------------------------------------ */
/* Linux struct stat (simplified, 64-bit)                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
} Linux_Stat64;

static uint64_t filetime_to_nsec(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* FILETIME epoch: Jan 1 1601; Unix epoch: Jan 1 1970 */
    t -= 116444736000000000ULL;
    return t * 100; /* 100ns -> ns */
}

/* ------------------------------------------------------------------ */
/* timespec struct (Linux)                                             */
/* ------------------------------------------------------------------ */
typedef struct { int64_t tv_sec; int64_t tv_nsec; } Linux_Timespec;

/* ------------------------------------------------------------------ */
/* MMAP bookkeeping                                                    */
/* ------------------------------------------------------------------ */
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_PROT_NONE     0
#define LINUX_PROT_READ     1
#define LINUX_PROT_WRITE    2
#define LINUX_PROT_EXEC     4

static DWORD prot_to_win32(int prot)
{
    bool r = (prot & LINUX_PROT_READ)  != 0;
    bool w = (prot & LINUX_PROT_WRITE) != 0;
    bool x = (prot & LINUX_PROT_EXEC)  != 0;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* ------------------------------------------------------------------ */
/* futex (minimal: WAIT and WAKE only)                                 */
/* ------------------------------------------------------------------ */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* ------------------------------------------------------------------ */
/* Main dispatch function                                              */
/* ------------------------------------------------------------------ */
/* ---- Guest memory registry (for Snapshots) -------------------------------
 * Record the guest's OWN anonymous mmap arenas and brk heap so the snapshot
 * module can scope capture to exactly the game's memory - NOT the AMD driver's
 * GPU buffers, which are raw VirtualAllocs the guest never requested through us. */
#define GUEST_MMAP_MAX 512
static struct { uintptr_t base; size_t size; } g_guest_mmaps[GUEST_MMAP_MAX];
static int       g_guest_mmap_n = 0;
static uintptr_t g_guest_brk_base = 0, g_guest_brk_cur = 0;

static void guest_mmap_add(uintptr_t base, size_t size){
    if (!base || !size) return;
    for (int i=0;i<g_guest_mmap_n;i++) if (g_guest_mmaps[i].base==base){ g_guest_mmaps[i].size=size; return; }
    if (g_guest_mmap_n < GUEST_MMAP_MAX){ g_guest_mmaps[g_guest_mmap_n].base=base; g_guest_mmaps[g_guest_mmap_n].size=size; g_guest_mmap_n++; }
}
static void guest_mmap_del(uintptr_t base){
    for (int i=0;i<g_guest_mmap_n;i++) if (g_guest_mmaps[i].base==base){ g_guest_mmaps[i]=g_guest_mmaps[--g_guest_mmap_n]; return; }
}
/* snapshot.c accessors */
int  snap_get_guest_mmaps(uintptr_t *bases, uint32_t *sizes, int max){
    int n = g_guest_mmap_n < max ? g_guest_mmap_n : max;
    for (int i=0;i<n;i++){ bases[i]=g_guest_mmaps[i].base; sizes[i]=(uint32_t)g_guest_mmaps[i].size; }
    return n;
}
void snap_get_guest_brk(uintptr_t *base, uintptr_t *cur){ if(base)*base=g_guest_brk_base; if(cur)*cur=g_guest_brk_cur; }

uintptr_t syscall_dispatch(uint64_t nr,
                            uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;

    switch (nr) {

    /* --- read --- */
    case SYS_read: {
        HANDLE h   = fd_get((int)a0);
        void  *buf = (void*)a1;
        DWORD  n   = (DWORD)a2, got = 0;
        if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;
        if (!ReadFile(h, buf, n, &got, NULL)) return linux_errno();
        return got;
    }

    /* --- write --- */
    case SYS_write: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return vfs_write(fd, (const void*)a1, (size_t)a2);
        if (unix_is_sockfd(fd)) return unix_write(fd, (const void*)a1, (size_t)a2);
        HANDLE h   = fd_get(fd);
        void  *buf = (void*)a1;
        DWORD  n   = (DWORD)a2, wrote = 0;
        if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;
        if (!WriteFile(h, buf, n, &wrote, NULL)) return linux_errno();
        return wrote;
    }

    /* --- open --- */
    case SYS_open: {
        const char *path  = (const char*)a0;
        int         flags = (int)a1;
        /* Check VFS first */
        if (vfs_is_virtual_path(path)) return vfs_open(path, flags);
        HANDLE h = CreateFileA(path,
                               open_access(flags),
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, open_create(flags),
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return linux_errno();
        int fd = fd_alloc(h);
        if (fd < 0) { CloseHandle(h); return -LINUX_ENOMEM; }
        return fd;
    }

    /* --- openat --- */
    case SYS_openat: {
        const char *path  = (const char*)a1;
        int         flags = (int)a2;
        if (vfs_is_virtual_path(path)) return vfs_open(path, flags);
        HANDLE h = CreateFileA(path,
                               open_access(flags),
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, open_create(flags),
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return linux_errno();
        int fd = fd_alloc(h);
        if (fd < 0) { CloseHandle(h); return -LINUX_ENOMEM; }
        return fd;
    }

    /* --- close --- */
    case SYS_close: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return vfs_close(fd);
        if (unix_is_sockfd(fd)) return unix_close(fd);
        if (iso_is_isofd(fd)) return iso_vfs_close(fd);
        if (epoll_is_epfd(fd)) return epoll_close(fd);
        if (fd < 3) return 0;
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;
        CloseHandle(h);
        g_fds[fd] = INVALID_HANDLE_VALUE;
        return 0;
    }

    /* --- lseek --- */
    case SYS_lseek: {
        int fd = (int)a0;
        if (iso_is_isofd(fd)) return iso_vfs_lseek(fd, (int64_t)a1, (int)a2);
        if (vfs_is_vfd(fd)) return vfs_lseek(fd, (int64_t)a1, (int)a2);
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;
        LARGE_INTEGER li, out;
        li.QuadPart = (int64_t)a1;
        DWORD method = (a2 == 0) ? FILE_BEGIN : (a2 == 1) ? FILE_CURRENT : FILE_END;
        if (!SetFilePointerEx(h, li, &out, method)) return linux_errno();
        return (uintptr_t)out.QuadPart;
    }

    /* --- fstat --- */
    case SYS_fstat: {
        int fd = (int)a0;
        if (iso_is_isofd(fd)) return iso_vfs_fstat(fd, (Linux_Stat*)a1);
        if (vfs_is_vfd(fd)) return vfs_fstat(fd, (Linux_Stat*)a1);
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;
        Linux_Stat64 *st = (Linux_Stat64*)a1;
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h, &info)) return linux_errno();
        memset(st, 0, sizeof(*st));
        LARGE_INTEGER sz;
        sz.HighPart = info.nFileSizeHigh;
        sz.LowPart  = info.nFileSizeLow;
        st->st_size    = sz.QuadPart;
        st->st_blksize = 4096;
        st->st_blocks  = (sz.QuadPart + 511) / 512;
        st->st_atime_sec = filetime_to_nsec(info.ftLastAccessTime)  / 1000000000ULL;
        st->st_mtime_sec = filetime_to_nsec(info.ftLastWriteTime)   / 1000000000ULL;
        st->st_ctime_sec = filetime_to_nsec(info.ftCreationTime)    / 1000000000ULL;
        st->st_mode      = 0100644;
        return 0;
    }

    /* --- mmap --- */
    case SYS_mmap: {
        void   *addr  = (void*)a0;
        size_t  len   = (size_t)a1;
        int     prot  = (int)a2;
        int     flags = (int)a3;
        int     fd    = (int)a4;
        /* off_t offset = (int64_t)a5; */

        DWORD protect = prot_to_win32(prot);

        if (flags & LINUX_MAP_ANONYMOUS) {
            void *hint = (flags & LINUX_MAP_FIXED) ? addr : NULL;
            void *p = VirtualAlloc(hint, len, MEM_COMMIT | MEM_RESERVE, protect);
            if (!p) {
                loader_log(LOG_ERROR, "[mmap] anon FAILED len=0x%zx hint=%p flags=0x%x err=%lu", len, hint, flags, GetLastError());
                return -LINUX_ENOMEM;
            }
            loader_log(LOG_INFO, "[mmap] anon len=0x%zx -> %p", len, p);
            guest_mmap_add((uintptr_t)p, len);   /* track for Snapshots */
            return (uintptr_t)p;
        } else {
            /* File-backed mmap via Win32 CreateFileMapping */
            HANDLE h = fd_get(fd);
            if (h == INVALID_HANDLE_VALUE) return -LINUX_EBADF;

            DWORD page_prot = (prot & LINUX_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
            HANDLE mapping = CreateFileMappingA(h, NULL, page_prot, 0, 0, NULL);
            if (!mapping) return linux_errno();

            DWORD view_access = (prot & LINUX_PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
            void *p = MapViewOfFileEx(mapping, view_access, 0, (DWORD)a5, len,
                                      (flags & LINUX_MAP_FIXED) ? addr : NULL);
            CloseHandle(mapping);
            if (!p) return -LINUX_ENOMEM;
            return (uintptr_t)p;
        }
    }

    /* --- munmap --- */
    case SYS_munmap: {
        guest_mmap_del((uintptr_t)a0);
        VirtualFree((void*)a0, 0, MEM_RELEASE);
        return 0;
    }

    /* --- mprotect --- */
    case SYS_mprotect: {
        DWORD old, protect = prot_to_win32((int)a2);
        if (!VirtualProtect((void*)a0, (SIZE_T)a1, protect, &old))
            return linux_errno();
        return 0;
    }

    /* --- brk --- */
    case SYS_brk: {
        /* Heap tracked via g_guest_brk_* for Snapshots.
         * Reserve 256 MB upfront (matching real Lindbergh board RAM). */
        if (g_guest_brk_cur == 0) {
            void *heap = VirtualAlloc(NULL, 256*1024*1024, MEM_RESERVE, PAGE_READWRITE);
            g_guest_brk_base = g_guest_brk_cur = heap ? (uintptr_t)heap : 0x40000000;
        }
        if (a0 > g_guest_brk_cur) {
            /* Refuse to grow past the reserved region - caller sees unchanged
             * brk and treats it as OOM, rather than touching uncommitted pages. */
            if (a0 > g_guest_brk_base + 256*1024*1024)
                return g_guest_brk_cur;
            if (!VirtualAlloc((void*)g_guest_brk_cur, a0 - g_guest_brk_cur,
                              MEM_COMMIT, PAGE_READWRITE)) {
                loader_log(LOG_ERROR, "[brk] commit FAILED cur=0x%x req=0x%x delta=0x%x err=%lu",
                           (unsigned)g_guest_brk_cur, (unsigned)a0, (unsigned)(a0-g_guest_brk_cur), GetLastError());
                return g_guest_brk_cur;
            }
            loader_log(LOG_INFO, "[brk] grow 0x%x -> 0x%x (+0x%x)", (unsigned)g_guest_brk_cur, (unsigned)a0, (unsigned)(a0-g_guest_brk_cur));
            g_guest_brk_cur = a0;
        }
        return g_guest_brk_cur;
    }

    /* --- getpid / getuid etc --- */
    case SYS_getpid:    return (uintptr_t)GetCurrentProcessId();
    case SYS_getuid:
    case SYS_geteuid:   return 1000; /* fake non-root UID */
    case SYS_getgid:
    case SYS_getegid:   return 1000;
    case SYS_getppid:   return 1;

    /* --- nanosleep --- */
    case SYS_nanosleep: {
        /* ELF is 32-bit; struct timespec uses 32-bit long fields.
         * The local Linux_Timespec typedef has int64_t (wrong layout here),
         * so read as int32_t pair to match the guest struct exactly. */
        int32_t *raw = (int32_t*)(uintptr_t)a0;
        int32_t s  = raw ? raw[0] : 0;
        int32_t ns = raw ? raw[1] : 0;
        if (s >= 0 && ns >= 0) {
            DWORD ms = (DWORD)(s * 1000 + ns / 1000000);
            if (ms > 33) ms = 0;   /* cap: budget underflow → catch up */
            if (ms > 0) Sleep(ms);
        }
        int32_t *rem = (int32_t*)(uintptr_t)a1;
        if (rem) { rem[0] = 0; rem[1] = 0; }
        return 0;
    }

    /* --- clock_gettime --- */
    case SYS_clock_gettime: {
        Linux_Timespec *tp = (Linux_Timespec*)a1;
        extern uint64_t vtime_ns(void);          /* virtual clock (rewindable by Snapshots) */
        uint64_t ns = vtime_ns();
        tp->tv_sec  = (long)(ns / 1000000000ULL);
        tp->tv_nsec = (long)(ns % 1000000000ULL);
        return 0;
    }

    /* --- futex (WAIT / WAKE only) --- */
    case SYS_futex: {
        int *uaddr   = (int*)a0;
        int  op      = (int)a1 & 0xF;
        int  val     = (int)a2;
        if (op == FUTEX_WAIT) {
            if (*uaddr != val) return -LINUX_EINVAL;
            Linux_Timespec *ts = (Linux_Timespec*)a3;
            DWORD timeout_ms = ts ? (DWORD)(ts->tv_sec*1000 + ts->tv_nsec/1000000)
                                  : INFINITE;
            Sleep(timeout_ms);
            return 0;
        }
        if (op == FUTEX_WAKE) {
            /* wake-up: just return number of waiters woken (0 for now) */
            return 0;
        }
        return -LINUX_ENOSYS;
    }

    /* --- sched_yield --- */
    case SYS_sched_yield:
        SwitchToThread();
        return 0;

    /* --- exit / exit_group --- */
    case SYS_exit:
    case SYS_exit_group:
        loader_log(LOG_INFO, "ELF process exit(%llu)", (unsigned long long)a0);
        ExitProcess((UINT)a0);
        return 0;

    /* --- rt_sigaction -> signal semantic layer --- */
    case SYS_rt_sigaction:
        return signal_rt_sigaction((int)a0,
                                    (const Linux_Sigaction*)a1,
                                    (Linux_Sigaction*)a2,
                                    (size_t)a3);

    /* --- rt_sigprocmask -> signal semantic layer --- */
    case SYS_rt_sigprocmask:
        return signal_rt_sigprocmask((int)a0,
                                      (const uint64_t*)a1,
                                      (uint64_t*)a2,
                                      (size_t)a3);

    case SYS_rt_sigreturn:
        return 0;

    /* --- sigaltstack --- */
    case SYS_sigaltstack:
        return signal_sigaltstack((const Linux_Stack*)a0, (Linux_Stack*)a1);

    /* --- kill / tgkill -> signal delivery --- */
    case SYS_kill:
        return signal_kill((int)a0, (int)a1);
    case SYS_tgkill:
        return signal_tgkill((int)a0, (int)a1, (int)a2);

    /* --- setitimer / alarm -> SIGALRM timer --- */
    case SYS_setitimer:
        return signal_setitimer((int)a0,
                                 (const Linux_Itimerval*)a1,
                                 (Linux_Itimerval*)a2);
    case SYS_alarm:
        return signal_alarm((unsigned)a0);

    /* --- ioctl -> VFS ioctl layer --- */
    case SYS_ioctl: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return vfs_ioctl(fd, a1, (void*)a2);
        loader_log(LOG_DEBUG, "ioctl fd=%d req=0x%llx (real fd, stubbed)",
                   fd, (unsigned long long)a1);
        return -LINUX_EINVAL;
    }

    /* --- SysV shared memory -> shmem semantic layer --- */
    case SYS_shmget:
        return shmem_shmget((key_t)a0, (size_t)a1, (int)a2);
    case SYS_shmat:
        return (uintptr_t)shmem_shmat((int)a0, (const void*)a1, (int)a2);
    case SYS_shmdt:
        return shmem_shmdt((const void*)a0);
    case SYS_shmctl:
        return shmem_shmctl((int)a0, (int)a1, (void*)a2);

    /* --- Process / thread identity --- */
    case SYS_gettid:
        return (uintptr_t)GetCurrentThreadId();
    case SYS_set_tid_address:
        return (uintptr_t)GetCurrentThreadId();
    case SYS_getpgrp:
        return (uintptr_t)GetCurrentProcessId();

    /* --- uname --- */
    case SYS_uname: {
        struct { char sysname[65],nodename[65],release[65],version[65],machine[65]; } *u
            = (void*)a0;
        memset(u, 0, sizeof(*u));
        strcpy(u->sysname,  "Linux");
        strcpy(u->nodename, "lindbergh");
        strcpy(u->release,  "2.6.18-164.el5");
        strcpy(u->version,  "#1 SMP Mon Sep 14 00:00:00 EDT 2009");
        strcpy(u->machine,  "x86_64");
        return 0;
    }

    /* --- prctl (common uses: set thread name, PR_SET_DUMPABLE) --- */
    case SYS_prctl:
        return 0; /* acknowledge but ignore */

    /* --- arch_prctl (FS base for glibc TLS) -> thread semantic layer --- */
    case SYS_arch_prctl: {
        if (a0 == 0x1002) { /* ARCH_SET_FS */
            threads_set_fs_base((uintptr_t)a1);
            return 0;
        }
        if (a0 == 0x1003) { /* ARCH_GET_FS */
            *(uintptr_t*)a1 = threads_get_fs_base();
            return 0;
        }
        return -LINUX_EINVAL;
    }

    /* --- madvise (safe to ignore) --- */
    case SYS_madvise:
        return 0;

    /* --- msync --- */
    case SYS_msync:
        FlushViewOfFile((void*)a0, (SIZE_T)a1);
        return 0;

    /* --- chdir / fchdir --- */
    case SYS_chdir:
        return SetCurrentDirectoryA((const char*)a0) ? 0 : -LINUX_ENOENT;
    case SYS_fchdir:
        return 0; /* stub */

    /* --- umask --- */
    case SYS_umask:
        return 0022; /* return previous mask */

    /* --- set_robust_list (stub) --- */
    case SYS_set_robust_list:
        return 0;

    /* --- getrandom --- */
    case SYS_getrandom: {
        uint8_t *buf = (uint8_t*)a0;
        size_t   n   = (size_t)a1;
        /* Use Win32 BCryptGenRandom via RtlGenRandom */
        typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID, ULONG);
        static RtlGenRandom_t RtlGenRandom = NULL;
        if (!RtlGenRandom) {
            HMODULE advapi = LoadLibraryA("advapi32.dll");
            RtlGenRandom = (RtlGenRandom_t)GetProcAddress(advapi, "SystemFunction036");
        }
        if (RtlGenRandom && RtlGenRandom(buf, (ULONG)n)) return n;
        return -LINUX_EIO;
    }


    /* --- access --- */
    case SYS_access: {
        const char *path = (const char*)a0;
        if (vfs_is_virtual_path(path)) return 0; /* virtual paths always accessible */
        return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? 0 : -LINUX_ENOENT;
    }

    /* --- getcwd --- */
    case SYS_getcwd: {
        char  *buf = (char*)a0;
        size_t n   = (size_t)a1;
        if (!GetCurrentDirectoryA((DWORD)n, buf)) return linux_errno();
        return (uintptr_t)strlen(buf) + 1;
    }

    /* --- clone (thread creation - map to CreateThread) --- */
    /* --- clone (thread creation) -> thread semantic layer --- */
    case SYS_clone:
        return threads_clone(a0, a1, a2, a3, a4);

    /* --- fork --- */
    case SYS_fork:
        loader_log(LOG_WARN, "fork() called, not supported, returning ENOSYS");
        return -LINUX_ENOSYS;

    default:
        loader_log(LOG_WARN, "Unhandled syscall %llu", (unsigned long long)nr);
        return -LINUX_ENOSYS;
    }
}

/* ------------------------------------------------------------------ */
/* VEH-based trap (fallback for any syscalls Option B missed)          */
/* ------------------------------------------------------------------ */
static LONG WINAPI syscall_veh(EXCEPTION_POINTERS *ep)
{
    /* EXCEPTION_ILLEGAL_INSTRUCTION can fire on syscall in some contexts */
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uint8_t *ip = (uint8_t*)(uintptr_t)ep->ContextRecord->Rip;
    if (!ip || (uintptr_t)ip < 0x1000) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *ctx = ep->ContextRecord;

    /* INT 0x80 (CD 80) - Linux x86 32-bit syscall ABI (eax=nr, ebx/ecx/edx/esi/
     * edi=args). We deliberately do NOT statically rewrite int 0x80 sites: a blind
     * CD-80 byte-scan corrupts the many incidental CD 80 operand bytes inside real
     * instructions (a LEA disp, a jmp rel8, a cmp imm16, …), which crashes the game
     * the moment one of those instructions executes. Trapping at execution time is
     * exact - only an actually-executed int 0x80 ever faults here. */
    if (ip[0] == 0xCD && ip[1] == 0x80) {
        extern uintptr_t int80_dispatch(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
        ctx->Rax = (uint32_t)int80_dispatch((uint32_t)ctx->Rax,
                       (uint32_t)ctx->Rbx, (uint32_t)ctx->Rcx, (uint32_t)ctx->Rdx,
                       (uint32_t)ctx->Rsi, (uint32_t)ctx->Rdi);
        ctx->Rip += 2; /* past the 2-byte CD 80 */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* 0F 05 (syscall) - x86-64 ABI; rare in this 32-bit target, kept for safety. */
    if (ip[0] != 0x0F || ip[1] != 0x05)
        return EXCEPTION_CONTINUE_SEARCH;

    ctx->Rax = syscall_dispatch(ctx->Rax,
                                 ctx->Rdi, ctx->Rsi, ctx->Rdx,
                                 ctx->R10, ctx->R8,  ctx->R9);
    ctx->Rip += 2; /* skip past syscall instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool syscall_trap_install(LoaderCtx *ctx)
{
    fds_init();
    signals_init();
    threads_init();   /* thread semantic layer */
    ctx->veh_handle = AddVectoredExceptionHandler(1, syscall_veh);
    loader_log(LOG_INFO, "Syscall VEH trap + semantic layers installed");
    return ctx->veh_handle != NULL;
}

void syscall_trap_remove(LoaderCtx *ctx)
{
    if (ctx->veh_handle) {
        RemoveVectoredExceptionHandler(ctx->veh_handle);
        ctx->veh_handle = NULL;
    }
    signals_shutdown();
}

/* ------------------------------------------------------------------ */
/* INT 0x80 dispatch (Linux x86 32-bit syscall ABI)                   */
/* Trampoline passes: nr=eax, a0=ebx, a1=ecx, a2=edx, a3=esi, a4=edi */
/* Returns result in eax (sign-extended to 64-bit for our dispatcher) */
/* ------------------------------------------------------------------ */

/* Linux x86 32-bit syscall numbers (different from x86-64!) */
#define SYS32_exit        1
#define SYS32_read        3
#define SYS32_write       4
#define SYS32_open        5
#define SYS32_close       6
#define SYS32_lseek      19
#define SYS32_getpid     20
#define SYS32_access     33
#define SYS32_brk        45
#define SYS32_mmap       90
#define SYS32_munmap     91
#define SYS32_mprotect  125
#define SYS32_nanosleep 162
#define SYS32_mmap2     192  /* mmap with offset in pages */
#define SYS32_exit_group 252
#define SYS32_clock_gettime 265
#define SYS32_socket    281
#define SYS32_bind      282
#define SYS32_connect   283
#define SYS32_listen    284
#define SYS32_accept    285
#define SYS32_getsockname 286
#define SYS32_getpeername 287
#define SYS32_socketpair 288
#define SYS32_send      289
#define SYS32_recv      290
#define SYS32_sendto    291
#define SYS32_recvfrom  292
#define SYS32_shutdown  293
#define SYS32_setsockopt 294
#define SYS32_getsockopt 295
#define SYS32_set_thread_area 243
#define SYS32_futex     240
#define SYS32_set_tid_address 258
#define SYS32_rt_sigaction  174
#define SYS32_rt_sigprocmask 175
#define SYS32_getuid    199
#define SYS32_getgid    200
#define SYS32_geteuid   201
#define SYS32_getegid   202
#define SYS32_getppid   64
#define SYS32_sched_yield 158
#define SYS32_getdents64 220
#define SYS32_fcntl64   221
#define SYS32_stat64    195
#define SYS32_fstat64   197
#define SYS32_lstat64   196
#define SYS32_getcwd    183
#define SYS32_mkdir      39
#define SYS32_unlink     10
#define SYS32_ioctl      54
#define SYS32_shmget     29
#define SYS32_shmat      30
#define SYS32_shmdt      67
#define SYS32_shmctl     31
#define SYS32_prctl     172
#define SYS32_gettid    224
#define SYS32_clone     120
#define SYS32_uname      26
#define SYS32_times      43
#define SYS32_epoll_create  254
#define SYS32_epoll_ctl     255
#define SYS32_epoll_wait    256
#define SYS32_madvise   219
#define SYS32_getrandom 355

/* Linux x86 32-bit stat64 structure */
typedef struct {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad1[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
} Linux_Stat64_32;

/* set_thread_area for ELF32 TLS (GDT entry simulation) */
typedef struct {
    int  entry_number;   /* -1 = allocate */
    uint32_t base_addr;
    uint32_t limit;
    unsigned int seg_32bit:1, contents:2, read_exec_only:1,
                 limit_in_pages:1, seg_not_present:1, useable:1;
} Linux_UserDesc;

uintptr_t int80_dispatch(uint32_t nr,
                          uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3, uint32_t a4)
{
    switch (nr) {

    case SYS32_read: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return (uintptr_t)vfs_read(fd,(void*)(uintptr_t)a1,a2);
        if (unix_is_sockfd(fd)) return (uintptr_t)unix_read(fd,(void*)(uintptr_t)a1,a2);
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)-LINUX_EBADF;
        DWORD got = 0;
        ReadFile(h, (void*)(uintptr_t)a1, a2, &got, NULL);
        return got;
    }
    case SYS32_write: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return (uintptr_t)vfs_write(fd,(void*)(uintptr_t)a1,a2);
        if (unix_is_sockfd(fd)) return (uintptr_t)unix_write(fd,(void*)(uintptr_t)a1,a2);
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)-LINUX_EBADF;
        DWORD wrote = 0;
        WriteFile(h, (void*)(uintptr_t)a1, a2, &wrote, NULL);
        return wrote;
    }
    case SYS32_open: {
        const char *path = (const char*)(uintptr_t)a0;
        if (vfs_is_virtual_path(path)) return (uint32_t)vfs_open(path,(int)a1);
        HANDLE h = CreateFileA(path, open_access(a1),
                               FILE_SHARE_READ|FILE_SHARE_WRITE,
                               NULL, open_create(a1), FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)linux_errno();
        int fd = fd_alloc(h);
        if (fd < 0) { CloseHandle(h); return (uint32_t)-LINUX_ENOMEM; }
        return fd;
    }
    case SYS32_close: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return vfs_close(fd);
        if (unix_is_sockfd(fd)) return unix_close(fd);
        if (fd < 3) return 0;
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)-LINUX_EBADF;
        CloseHandle(h); g_fds[fd] = INVALID_HANDLE_VALUE;
        return 0;
    }
    case SYS32_brk:
        return (uint32_t)syscall_dispatch(SYS_brk, a0, 0,0,0,0,0);
    case SYS32_mmap2: {
        /* mmap2: offset is in 4096-byte pages */
        uint64_t byte_offset = (uint64_t)a4 * 4096;
        return (uint32_t)syscall_dispatch(SYS_mmap, a0, a1, a2, a3, (int)a4, byte_offset);
    }
    case SYS32_mmap:
        return (uint32_t)syscall_dispatch(SYS_mmap, a0,a1,a2,a3,a4,0);
    case SYS32_munmap:
        return (uint32_t)syscall_dispatch(SYS_munmap, a0,a1,0,0,0,0);
    case SYS32_mprotect:
        return (uint32_t)syscall_dispatch(SYS_mprotect, a0,a1,a2,0,0,0);
    case SYS32_madvise: return 0;
    case SYS32_fstat64: {
        int fd = (int)a0;
        Linux_Stat64_32 *st = (Linux_Stat64_32*)(uintptr_t)a1;
        if (!st) return (uint32_t)-LINUX_EFAULT;
        memset(st, 0, sizeof(*st));
        if (vfs_is_vfd(fd)) { st->st_mode = 0100444; return 0; }
        HANDLE h = fd_get(fd);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)-LINUX_EBADF;
        BY_HANDLE_FILE_INFORMATION info;
        GetFileInformationByHandle(h, &info);
        LARGE_INTEGER sz;
        sz.HighPart = info.nFileSizeHigh; sz.LowPart = info.nFileSizeLow;
        st->st_size = sz.QuadPart;
        st->st_mode = 0100644;
        st->st_blksize = 4096;
        return 0;
    }
    case SYS32_ioctl: {
        int fd = (int)a0;
        if (vfs_is_vfd(fd)) return (uint32_t)vfs_ioctl(fd, a1, (void*)(uintptr_t)a2);
        return (uint32_t)-LINUX_EINVAL;
    }
    case SYS32_nanosleep: return (uint32_t)syscall_dispatch(SYS_nanosleep,a0,a1,0,0,0,0);
    case SYS32_clock_gettime: return (uint32_t)syscall_dispatch(SYS_clock_gettime,a0,a1,0,0,0,0);
    case SYS32_getpid:  return (uint32_t)GetCurrentProcessId();
    case SYS32_getuid:
    case SYS32_geteuid: return 1000;
    case SYS32_getgid:
    case SYS32_getegid: return 1000;
    case SYS32_gettid:  return (uint32_t)GetCurrentThreadId();
    case SYS32_sched_yield: SwitchToThread(); return 0;
    case SYS32_exit:
    case SYS32_exit_group:
        loader_log(LOG_INFO, "ELF32 exit(%u)", a0);
        ExitProcess(a0); return 0;
    case SYS32_rt_sigaction:  return 0;
    case SYS32_rt_sigprocmask: return 0;
    case SYS32_set_tid_address: return (uint32_t)GetCurrentThreadId();
    case SYS32_prctl: return 0;
    case SYS32_uname: {
        /* Same as 64-bit uname but 32-bit pointer */
        struct { char a[65],b[65],c[65],d[65],e[65]; } *u =
            (void*)(uintptr_t)a0;
        if (u) {
            memset(u,0,sizeof(*u));
            strcpy(u->a,"Linux"); strcpy(u->b,"lindbergh");
            strcpy(u->c,"2.6.18-164.el5"); strcpy(u->e,"i686");
        }
        return 0;
    }
    case SYS32_getcwd: {
        char *buf = (char*)(uintptr_t)a0;
        if (!GetCurrentDirectoryA(a1, buf)) return (uint32_t)linux_errno();
        return (uint32_t)strlen(buf)+1;
    }
    case SYS32_set_thread_area: {
        /* GDT entry for TLS - store the base address in our FS emulation */
        Linux_UserDesc *ud = (Linux_UserDesc*)(uintptr_t)a0;
        if (ud) {
            loader_log(LOG_DEBUG, "set_thread_area base=0x%x", ud->base_addr);
            threads_set_fs_base(ud->base_addr);
            ud->entry_number = 6; /* fake GDT slot */
        }
        return 0;
    }
    case SYS32_futex:
        return (uint32_t)syscall_dispatch(SYS_futex, a0,a1,a2,a3,0,0);
    case SYS32_clone:
        return (uint32_t)threads_clone(a0, a1, a2, a3, a4);
    case SYS32_socket:
        return (uint32_t)unix_socket(a0,a1,a2);
    case SYS32_connect:
        return (uint32_t)unix_connect(a0,(void*)(uintptr_t)a1,a2);
    case SYS32_bind:
        return (uint32_t)unix_bind(a0,(void*)(uintptr_t)a1,a2);
    case SYS32_listen:
        return (uint32_t)unix_listen(a0,a1);
    case SYS32_accept:
        return (uint32_t)unix_accept(a0,(void*)(uintptr_t)a1,(uint32_t*)(uintptr_t)a2);
    case SYS32_send:
        return (uint32_t)unix_send(a0,(void*)(uintptr_t)a1,a2,a3);
    case SYS32_recv:
        return (uint32_t)unix_recv(a0,(void*)(uintptr_t)a1,a2,a3);
    case SYS32_shmget:
        return (uint32_t)shmem_shmget((key_t)a0,a1,a2);
    case SYS32_shmat:
        return (uint32_t)(uintptr_t)shmem_shmat(a0,(void*)(uintptr_t)a1,a2);
    case SYS32_shmdt:
        return (uint32_t)shmem_shmdt((void*)(uintptr_t)a0);
    case SYS32_shmctl:
        return (uint32_t)shmem_shmctl(a0,a1,(void*)(uintptr_t)a2);
    case SYS32_epoll_create:
        return (uint32_t)epoll_create1_impl(0);
    case SYS32_epoll_ctl:
        return (uint32_t)epoll_ctl_impl(a0,a1,a2,(Linux_EpollEvent*)(uintptr_t)a3);
    case SYS32_epoll_wait:
        return (uint32_t)epoll_wait_impl(a0,(Linux_EpollEvent*)(uintptr_t)a1,a2,a3);
    case SYS32_getrandom: {
        typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID,ULONG);
        static RtlGenRandom_t RtlGenRandom = NULL;
        if (!RtlGenRandom) {
            HMODULE adv = LoadLibraryA("advapi32.dll");
            RtlGenRandom = (RtlGenRandom_t)GetProcAddress(adv,"SystemFunction036");
        }
        if (RtlGenRandom) RtlGenRandom((void*)(uintptr_t)a0, a1);
        return a1;
    }
    case SYS32_lseek: {
        HANDLE h = fd_get((int)a0);
        if (h == INVALID_HANDLE_VALUE) return (uint32_t)-LINUX_EBADF;
        LARGE_INTEGER li, out; li.QuadPart = (int32_t)a1;
        DWORD m = (a2==0)?FILE_BEGIN:(a2==1)?FILE_CURRENT:FILE_END;
        SetFilePointerEx(h,li,&out,m);
        return (uint32_t)out.QuadPart;
    }
    case SYS32_access: {
        const char *p = (const char*)(uintptr_t)a0;
        if (vfs_is_virtual_path(p)) return 0;
        return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 0 : (uint32_t)-LINUX_ENOENT;
    }
    default:
        loader_log(LOG_WARN, "INT 0x80: unhandled syscall %u", nr);
        return (uint32_t)-LINUX_ENOSYS;
    }
}
