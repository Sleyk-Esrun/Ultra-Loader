/*
 * unix_socket.c
 * Unix Domain Socket Semantic Layer
 *
 * Lindbergh uses Unix sockets (AF_UNIX) for:
 *   - Inter-process communication between game and system daemon
 *   - Audio server (PulseAudio/ESD socket at /tmp/.esd-0 or similar)
 *   - Input event multiplexing
 *
 * AF_UNIX socket paths on Linux map to:
 *   Abstract namespace: \0name  (first byte is NUL)
 *   Filesystem:         /tmp/foo, /var/run/foo
 *
 * Win32 doesn't have AF_UNIX natively (it was added in Win10 1803 but
 * is limited). We implement it via named pipes for reliability:
 *
 *   connect("/tmp/foo")  → CreateFile(\\.\pipe\ultraloader_foo)
 *   bind("/tmp/foo")     → CreateNamedPipe(\\.\pipe\ultraloader_foo)
 *   accept()             → ConnectNamedPipe()
 *
 * Socket fd table is separate from the main fd table to avoid conflicts.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "unix_socket.h"

/* ------------------------------------------------------------------ */
/* Socket fd pool                                                      */
/* ------------------------------------------------------------------ */
#define SOCK_FD_BASE  0x3000
#define SOCK_FD_MAX   128

#define SOCK_DOMAIN_UNIX   1
#define SOCK_DOMAIN_INET   2
#define SOCK_DOMAIN_INET6  10

#define SOCK_TYPE_STREAM   1
#define SOCK_TYPE_DGRAM    2
#define SOCK_TYPE_SEQPACKET 5

#define SOCK_STATE_CREATED    0
#define SOCK_STATE_BOUND      1
#define SOCK_STATE_LISTENING  2
#define SOCK_STATE_CONNECTED  3

typedef struct {
    int    domain;
    int    type;
    int    state;
    bool   in_use;
    bool   nonblocking;

    /* For AF_UNIX (named pipe backend) */
    HANDLE pipe;
    char   path[256];       /* bound/connected path */
    char   pipe_name[300];  /* \\.\pipe\ultraloader_xxx */

    /* For AF_INET (Winsock backend) */
    SOCKET sock;

    /* Pending connection (server side) */
    HANDLE pending_pipe;
} SockEntry;

static SockEntry g_socks[SOCK_FD_MAX];
static bool      g_winsock_init = false;

static void ensure_winsock(void)
{
    if (!g_winsock_init) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2,2), &wd);
        g_winsock_init = true;
    }
}

static int sock_alloc(int domain, int type)
{
    for (int i = 0; i < SOCK_FD_MAX; i++) {
        if (!g_socks[i].in_use) {
            memset(&g_socks[i], 0, sizeof(SockEntry));
            g_socks[i].domain  = domain;
            g_socks[i].type    = type;
            g_socks[i].state   = SOCK_STATE_CREATED;
            g_socks[i].in_use  = true;
            g_socks[i].pipe    = INVALID_HANDLE_VALUE;
            g_socks[i].sock    = INVALID_SOCKET;
            return SOCK_FD_BASE + i;
        }
    }
    return -1;
}

static SockEntry *sock_get(int fd)
{
    int idx = fd - SOCK_FD_BASE;
    if (idx < 0 || idx >= SOCK_FD_MAX) return NULL;
    if (!g_socks[idx].in_use) return NULL;
    return &g_socks[idx];
}

bool unix_is_sockfd(int fd)
{
    return fd >= SOCK_FD_BASE && fd < SOCK_FD_BASE + SOCK_FD_MAX
           && g_socks[fd - SOCK_FD_BASE].in_use;
}

/* ------------------------------------------------------------------ */
/* Path -> pipe name conversion                                        */
/* ------------------------------------------------------------------ */
static void path_to_pipe(const char *path, char *pipe_name, size_t sz)
{
    /* Strip leading / and replace / with _ */
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') p = "unnamed";

    char safe[256] = {0};
    size_t si = 0;
    for (; *p && si < 255; p++) {
        safe[si++] = (*p == '/') ? '_' : *p;
    }
    snprintf(pipe_name, sz, "\\\\.\\pipe\\ultraloader_%s", safe);
}

/* ------------------------------------------------------------------ */
/* sockaddr_un structure                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t sun_family;  /* AF_UNIX = 1 */
    char     sun_path[108];
} Linux_SockaddrUn;

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} Linux_SockaddrIn;

/* ------------------------------------------------------------------ */
/* socket()                                                            */
/* ------------------------------------------------------------------ */
int unix_socket(int domain, int type, int protocol)
{
    int actual_type = type & 0xF; /* strip SOCK_NONBLOCK/SOCK_CLOEXEC flags */
    bool nonblock   = (type & 0x800) != 0;

    loader_log(LOG_DEBUG, "socket(domain=%d type=%d)", domain, actual_type);

    if (domain == SOCK_DOMAIN_UNIX) {
        int fd = sock_alloc(domain, actual_type);
        if (fd < 0) return -24; /* EMFILE */
        if (nonblock) g_socks[fd - SOCK_FD_BASE].nonblocking = true;
        return fd;
    }

    if (domain == SOCK_DOMAIN_INET || domain == SOCK_DOMAIN_INET6) {
        ensure_winsock();
        int af  = (domain == SOCK_DOMAIN_INET6) ? AF_INET6 : AF_INET;
        int wt  = (actual_type == SOCK_TYPE_STREAM) ? SOCK_STREAM : SOCK_DGRAM;
        SOCKET s = socket(af, wt, 0);
        if (s == INVALID_SOCKET) return -24;
        int fd = sock_alloc(domain, actual_type);
        if (fd < 0) { closesocket(s); return -24; }
        g_socks[fd - SOCK_FD_BASE].sock = s;
        if (nonblock) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            g_socks[fd - SOCK_FD_BASE].nonblocking = true;
        }
        return fd;
    }

    loader_log(LOG_WARN, "socket: unsupported domain %d", domain);
    return -22; /* EINVAL */
}

/* ------------------------------------------------------------------ */
/* bind()                                                              */
/* ------------------------------------------------------------------ */
int unix_bind(int fd, const void *addr, uint32_t addrlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        Linux_SockaddrUn *un = (Linux_SockaddrUn*)addr;
        const char *path = un->sun_path;
        if (path[0] == '\0') path++; /* abstract namespace */

        strncpy(s->path, path, sizeof(s->path)-1);
        path_to_pipe(s->path, s->pipe_name, sizeof(s->pipe_name));
        s->state = SOCK_STATE_BOUND;

        loader_log(LOG_DEBUG, "bind AF_UNIX \"%s\" -> %s", s->path, s->pipe_name);
        return 0;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        Linux_SockaddrIn *in = (Linux_SockaddrIn*)addr;
        struct sockaddr_in wa = {0};
        wa.sin_family = AF_INET;
        wa.sin_port   = in->sin_port;
        wa.sin_addr.s_addr = in->sin_addr;
        if (bind(s->sock, (struct sockaddr*)&wa, sizeof(wa)) == SOCKET_ERROR)
            return -98; /* EADDRINUSE */
        s->state = SOCK_STATE_BOUND;
        return 0;
    }

    return -22;
}

/* ------------------------------------------------------------------ */
/* listen()                                                            */
/* ------------------------------------------------------------------ */
int unix_listen(int fd, int backlog)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        /* Create named pipe server */
        s->pipe = CreateNamedPipeA(
            s->pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            backlog > 0 ? backlog : PIPE_UNLIMITED_INSTANCES,
            65536, 65536, 0, NULL);

        if (s->pipe == INVALID_HANDLE_VALUE) {
            loader_log(LOG_ERROR, "CreateNamedPipe failed: %lu", GetLastError());
            return -98;
        }
        s->state = SOCK_STATE_LISTENING;
        loader_log(LOG_DEBUG, "listen AF_UNIX %s", s->pipe_name);
        return 0;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        if (listen(s->sock, backlog) == SOCKET_ERROR) return -98;
        s->state = SOCK_STATE_LISTENING;
        return 0;
    }

    return -22;
}

/* ------------------------------------------------------------------ */
/* connect()                                                           */
/* ------------------------------------------------------------------ */
int unix_connect(int fd, const void *addr, uint32_t addrlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        Linux_SockaddrUn *un = (Linux_SockaddrUn*)addr;
        const char *path = un->sun_path;
        if (path[0] == '\0') path++;

        strncpy(s->path, path, sizeof(s->path)-1);
        path_to_pipe(s->path, s->pipe_name, sizeof(s->pipe_name));

        loader_log(LOG_DEBUG, "connect AF_UNIX \"%s\" -> %s", s->path, s->pipe_name);

        /* Try to connect to named pipe */
        HANDLE h = CreateFileA(s->pipe_name,
                               GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);

        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) return -11; /* EAGAIN */
            loader_log(LOG_DEBUG, "connect: no server on %s (%lu), returning ECONNREFUSED",
                       s->pipe_name, err);
            return -111; /* ECONNREFUSED */
        }

        s->pipe  = h;
        s->state = SOCK_STATE_CONNECTED;
        return 0;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        Linux_SockaddrIn *in = (Linux_SockaddrIn*)addr;
        struct sockaddr_in wa = {0};
        wa.sin_family = AF_INET;
        wa.sin_port   = in->sin_port;
        wa.sin_addr.s_addr = in->sin_addr;
        if (connect(s->sock, (struct sockaddr*)&wa, sizeof(wa)) == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return -115; /* EINPROGRESS */
            return -111;
        }
        s->state = SOCK_STATE_CONNECTED;
        return 0;
    }

    return -22;
}

/* ------------------------------------------------------------------ */
/* accept()                                                            */
/* ------------------------------------------------------------------ */
int unix_accept(int fd, void *addr, uint32_t *addrlen)
{
    SockEntry *s = sock_get(fd);
    if (!s || s->state != SOCK_STATE_LISTENING) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        /* Wait for connection on the named pipe */
        BOOL ok = ConnectNamedPipe(s->pipe, NULL);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            if (s->nonblocking) return -11; /* EAGAIN */
            return -5; /* EIO */
        }

        /* Create a new fd for the accepted connection */
        int newfd = sock_alloc(s->domain, s->type);
        if (newfd < 0) return -24;

        SockEntry *ns = sock_get(newfd);
        ns->pipe  = s->pipe;
        ns->state = SOCK_STATE_CONNECTED;
        strncpy(ns->pipe_name, s->pipe_name, sizeof(ns->pipe_name)-1);

        /* Create a new pipe instance for next accept */
        s->pipe = CreateNamedPipeA(s->pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536, 65536, 0, NULL);

        return newfd;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        SOCKET cs = accept(s->sock, NULL, NULL);
        if (cs == INVALID_SOCKET) {
            return s->nonblocking ? -11 : -5;
        }
        int newfd = sock_alloc(s->domain, s->type);
        if (newfd < 0) { closesocket(cs); return -24; }
        SockEntry *ns = sock_get(newfd);
        ns->sock  = cs;
        ns->state = SOCK_STATE_CONNECTED;
        return newfd;
    }

    return -22;
}

/* ------------------------------------------------------------------ */
/* send / recv                                                         */
/* ------------------------------------------------------------------ */
int64_t unix_send(int fd, const void *buf, size_t len, int flags)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        if (s->pipe == INVALID_HANDLE_VALUE) return -9;
        DWORD written;
        if (!WriteFile(s->pipe, buf, (DWORD)len, &written, NULL)) return -5;
        return (int64_t)written;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        int r = send(s->sock, (const char*)buf, (int)len, 0);
        return (r == SOCKET_ERROR) ? -5 : r;
    }

    return -22;
}

int64_t unix_recv(int fd, void *buf, size_t len, int flags)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;

    if (s->domain == SOCK_DOMAIN_UNIX) {
        if (s->pipe == INVALID_HANDLE_VALUE) return -9;
        DWORD got;
        if (!ReadFile(s->pipe, buf, (DWORD)len, &got, NULL)) {
            if (GetLastError() == ERROR_NO_DATA) return -11; /* EAGAIN */
            return -5;
        }
        return (int64_t)got;
    }

    if (s->domain == SOCK_DOMAIN_INET) {
        int r = recv(s->sock, (char*)buf, (int)len, 0);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            return (e == WSAEWOULDBLOCK) ? -11 : -5;
        }
        return r;
    }

    return -22;
}

/* ------------------------------------------------------------------ */
/* sendto / recvfrom (UDP)                                             */
/* ------------------------------------------------------------------ */
int64_t unix_sendto(int fd, const void *buf, size_t len, int flags,
                     const void *dest_addr, uint32_t addrlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (s->domain == SOCK_DOMAIN_INET) {
        Linux_SockaddrIn *in = (Linux_SockaddrIn*)dest_addr;
        struct sockaddr_in wa = {0};
        wa.sin_family = AF_INET;
        wa.sin_port   = in->sin_port;
        wa.sin_addr.s_addr = in->sin_addr;
        int r = sendto(s->sock, (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&wa, sizeof(wa));
        return (r == SOCKET_ERROR) ? -5 : r;
    }
    return unix_send(fd, buf, len, flags);
}

int64_t unix_recvfrom(int fd, void *buf, size_t len, int flags,
                       void *src_addr, uint32_t *addrlen)
{
    return unix_recv(fd, buf, len, flags);
}

/* ------------------------------------------------------------------ */
/* getsockopt / setsockopt                                             */
/* ------------------------------------------------------------------ */
int unix_setsockopt(int fd, int level, int optname,
                     const void *optval, uint32_t optlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (s->domain == SOCK_DOMAIN_INET && s->sock != INVALID_SOCKET) {
        /* Translate Linux socket option levels/names to Winsock */
        setsockopt(s->sock, level, optname, (const char*)optval, (int)optlen);
    }
    return 0; /* always succeed */
}

int unix_getsockopt(int fd, int level, int optname,
                     void *optval, uint32_t *optlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (s->domain == SOCK_DOMAIN_INET && s->sock != INVALID_SOCKET) {
        getsockopt(s->sock, level, optname, (char*)optval, (int*)optlen);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* getsockname                                                         */
/* ------------------------------------------------------------------ */
int unix_getsockname(int fd, void *addr, uint32_t *addrlen)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (addr && addrlen && *addrlen >= 3) {
        Linux_SockaddrUn *un = (Linux_SockaddrUn*)addr;
        un->sun_family = (uint16_t)s->domain;
        strncpy(un->sun_path, s->path, sizeof(un->sun_path)-1);
        *addrlen = 3 + strlen(s->path);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* shutdown / close                                                    */
/* ------------------------------------------------------------------ */
int unix_shutdown(int fd, int how)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (s->domain == SOCK_DOMAIN_INET && s->sock != INVALID_SOCKET)
        shutdown(s->sock, how);
    return 0;
}

int unix_close(int fd)
{
    SockEntry *s = sock_get(fd);
    if (!s) return -9;
    if (s->pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(s->pipe);
        CloseHandle(s->pipe);
    }
    if (s->sock != INVALID_SOCKET) closesocket(s->sock);
    memset(s, 0, sizeof(SockEntry));
    return 0;
}

/* ------------------------------------------------------------------ */
/* read / write passthrough for socket fds                            */
/* ------------------------------------------------------------------ */
int64_t unix_read(int fd, void *buf, size_t len)
{
    return unix_recv(fd, buf, len, 0);
}

int64_t unix_write(int fd, const void *buf, size_t len)
{
    return unix_send(fd, buf, len, 0);
}

/* ------------------------------------------------------------------ */
/* socketpair (used by some Lindbergh IPC)                            */
/* ------------------------------------------------------------------ */
int unix_socketpair(int domain, int type, int protocol, int sv[2])
{
    /* Create a connected pipe pair */
    char pipe_name[256];
    static int pair_id = 0;
    snprintf(pipe_name, sizeof(pipe_name),
             "\\\\.\\pipe\\ultraloader_pair_%d_%lu", pair_id++, GetCurrentThreadId());

    HANDLE server = CreateNamedPipeA(pipe_name,
        PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, NULL);
    if (server == INVALID_HANDLE_VALUE) return -5;

    HANDLE client = CreateFileA(pipe_name, GENERIC_READ|GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
    if (client == INVALID_HANDLE_VALUE) { CloseHandle(server); return -5; }

    ConnectNamedPipe(server, NULL);

    int fd0 = sock_alloc(domain, type & 0xF);
    int fd1 = sock_alloc(domain, type & 0xF);
    if (fd0 < 0 || fd1 < 0) {
        CloseHandle(server); CloseHandle(client);
        return -24;
    }

    SockEntry *s0 = sock_get(fd0);
    SockEntry *s1 = sock_get(fd1);
    s0->pipe = server; s0->state = SOCK_STATE_CONNECTED;
    s1->pipe = client; s1->state = SOCK_STATE_CONNECTED;

    sv[0] = fd0; sv[1] = fd1;
    return 0;
}
