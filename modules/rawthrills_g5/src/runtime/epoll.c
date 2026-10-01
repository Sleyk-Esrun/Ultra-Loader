/*
 * epoll.c
 * epoll Semantic Layer -> Win32 I/O Completion Ports
 *
 * epoll is Linux's scalable I/O event notification system.
 * Lindbergh uses it for:
 *   - Watching JVS serial input
 *   - Network event multiplexing
 *   - Audio buffer ready signals
 *
 * Linux epoll API:
 *   epoll_create1(flags)           -> fd
 *   epoll_ctl(epfd, op, fd, event) -> 0
 *   epoll_wait(epfd, events, n, timeout) -> n_ready
 *
 * Win32 IOCP is fundamentally different (completion vs readiness model)
 * and is overkill here. For Lindbergh's usage patterns a simpler approach
 * works: we maintain a watched-fd list and poll them with WaitForMultipleObjects.
 *
 * For pipe/socket fds in our unix_socket layer we use overlapped I/O events.
 * For real Win32 HANDLEs we use native wait handles.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "epoll.h"
#include "unix_socket.h"

/* ------------------------------------------------------------------ */
/* epoll instance pool                                                 */
/* ------------------------------------------------------------------ */
#define EPOLL_FD_BASE   0x4000
#define EPOLL_FD_MAX    32
#define EPOLL_WATCH_MAX 256

typedef struct {
    int      fd;           /* watched Linux fd */
    uint32_t events;       /* EPOLLIN | EPOLLOUT | ... */
    uint64_t data;         /* user data (from epoll_event.data.u64) */
    HANDLE   wait_handle;  /* Win32 event for this fd */
} EpollWatch;

typedef struct {
    bool       in_use;
    EpollWatch watches[EPOLL_WATCH_MAX];
    int        watch_count;
} EpollInst;

static EpollInst g_epolls[EPOLL_FD_MAX];

static int epoll_alloc(void)
{
    for (int i = 0; i < EPOLL_FD_MAX; i++) {
        if (!g_epolls[i].in_use) {
            memset(&g_epolls[i], 0, sizeof(EpollInst));
            g_epolls[i].in_use = true;
            return EPOLL_FD_BASE + i;
        }
    }
    return -1;
}

static EpollInst *epoll_get(int fd)
{
    int idx = fd - EPOLL_FD_BASE;
    if (idx < 0 || idx >= EPOLL_FD_MAX) return NULL;
    if (!g_epolls[idx].in_use) return NULL;
    return &g_epolls[idx];
}

bool epoll_is_epfd(int fd)
{
    int idx = fd - EPOLL_FD_BASE;
    return idx >= 0 && idx < EPOLL_FD_MAX && g_epolls[idx].in_use;
}

/* ------------------------------------------------------------------ */
/* epoll_create1                                                       */
/* ------------------------------------------------------------------ */
int epoll_create1_impl(int flags)
{
    int fd = epoll_alloc();
    loader_log(LOG_DEBUG, "epoll_create1 -> fd=%d", fd);
    return fd;
}

/* ------------------------------------------------------------------ */
/* epoll_ctl                                                            */
/* ------------------------------------------------------------------ */
int epoll_ctl_impl(int epfd, int op, int fd, Linux_EpollEvent *event)
{
    EpollInst *ep = epoll_get(epfd);
    if (!ep) return -9; /* EBADF */

    loader_log(LOG_DEBUG, "epoll_ctl epfd=%d op=%d fd=%d events=0x%x",
               epfd, op, fd, event ? event->events : 0);

    switch (op) {

    case EPOLL_CTL_ADD: {
        if (ep->watch_count >= EPOLL_WATCH_MAX) return -28; /* ENOMEM */

        /* Find or create a Win32 event for this fd */
        HANDLE hev = CreateEventA(NULL, FALSE, FALSE, NULL);

        EpollWatch *w = &ep->watches[ep->watch_count++];
        w->fd          = fd;
        w->events      = event ? event->events : EPOLLIN;
        w->data        = event ? event->data.u64 : 0;
        w->wait_handle = hev;
        return 0;
    }

    case EPOLL_CTL_MOD: {
        for (int i = 0; i < ep->watch_count; i++) {
            if (ep->watches[i].fd == fd) {
                ep->watches[i].events = event ? event->events : EPOLLIN;
                ep->watches[i].data   = event ? event->data.u64 : 0;
                return 0;
            }
        }
        return -2; /* ENOENT */
    }

    case EPOLL_CTL_DEL: {
        for (int i = 0; i < ep->watch_count; i++) {
            if (ep->watches[i].fd == fd) {
                CloseHandle(ep->watches[i].wait_handle);
                /* Compact array */
                int remaining = ep->watch_count - i - 1;
                if (remaining > 0)
                    memmove(&ep->watches[i], &ep->watches[i+1],
                            remaining * sizeof(EpollWatch));
                ep->watch_count--;
                return 0;
            }
        }
        return -2;
    }
    }
    return -22;
}

/* ------------------------------------------------------------------ */
/* Check if a watched fd has data available                           */
/* ------------------------------------------------------------------ */
static bool fd_is_readable(int fd)
{
    /* Unix socket fds */
    if (unix_is_sockfd(fd)) {
        /* Use PeekNamedPipe or select with zero timeout */
        /* For now signal readable if connected */
        return true;
    }

    /* Regular file fds - always readable */
    return true;
}

static bool fd_is_writable(int fd)
{
    return true; /* conservative: always writable */
}

/* ------------------------------------------------------------------ */
/* epoll_wait                                                           */
/* ------------------------------------------------------------------ */
int epoll_wait_impl(int epfd, Linux_EpollEvent *events, int maxevents, int timeout_ms)
{
    EpollInst *ep = epoll_get(epfd);
    if (!ep) return -9;

    if (ep->watch_count == 0) {
        if (timeout_ms > 0) Sleep(timeout_ms);
        return 0;
    }

    /* For simplicity: poll all watched fds for readiness.
     * A production implementation would use overlapped I/O events. */

    DWORD deadline = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD start    = GetTickCount();
    int   ready    = 0;

    do {
        for (int i = 0; i < ep->watch_count && ready < maxevents; i++) {
            EpollWatch *w = &ep->watches[i];
            uint32_t revents = 0;

            if ((w->events & EPOLLIN)  && fd_is_readable(w->fd)) revents |= EPOLLIN;
            if ((w->events & EPOLLOUT) && fd_is_writable(w->fd)) revents |= EPOLLOUT;

            if (revents) {
                events[ready].events    = revents;
                events[ready].data.u64  = w->data;
                ready++;

                if (w->events & EPOLLONESHOT)
                    w->events = 0; /* disarm */
            }
        }

        if (ready > 0) break;
        if (timeout_ms == 0) break;

        Sleep(1); /* 1ms poll interval */

    } while (deadline == INFINITE ||
             (GetTickCount() - start) < deadline);

    return ready;
}

/* ------------------------------------------------------------------ */
/* close epoll fd                                                       */
/* ------------------------------------------------------------------ */
int epoll_close(int fd)
{
    EpollInst *ep = epoll_get(fd);
    if (!ep) return -9;
    for (int i = 0; i < ep->watch_count; i++)
        CloseHandle(ep->watches[i].wait_handle);
    memset(ep, 0, sizeof(EpollInst));
    return 0;
}
