/*
 * x11_server.c - Minimal fake X11 server for Ultra Loader
 *
 * Ported from Lindbergh Launcher (SDL3 loader) to Ultra Loader.
 * Adapted: loader_ctx_t removed, log_warn -> loader_log.
 *
 * Runs in a background thread, listens on TCP 127.0.0.1:6000 (display :0).
 *
 * Implements just enough of the X11 wire protocol to:
 *   1. Complete the connection handshake (setup response)
 *   2. Return sane replies to QueryExtension, InternAtom, GetGeometry
 *   3. Accept CreateWindow / MapWindow silently
 *   4. Keep the connection alive so libX11 doesn't abort
 *
 * All real rendering goes through opengl32.dll via our GOT thunks.
 * This server exists only to satisfy libX11's XOpenDisplay() requirement.
 *
 * Note: Unix abstract socket (@/tmp/.X11-unix/X0) is not available on
 * Windows. TCP only. libX11 will fall back to TCP when given "127.0.0.1:0".
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "loader.h"

#define X11_PORT    6000    /* DISPLAY=:0 -> TCP port 6000 */
#define X11_BACKLOG 4

/* ── Server state ────────────────────────────────────────────────────────*/
static SOCKET            g_listen_sock   = INVALID_SOCKET;
static HANDLE            g_server_thread = NULL;
static volatile bool     g_running       = false;

/* ── Build minimal X11 setup response ───────────────────────────────────*/
static int build_setup_response(uint8_t *buf, int bufsz)
{
    memset(buf, 0, bufsz);
    int pos = 0;

    buf[pos++] = 1;   /* success */
    buf[pos++] = 0;   /* pad */

    *(uint16_t*)(buf+pos) = 11; pos+=2;  /* proto major */
    *(uint16_t*)(buf+pos) = 0;  pos+=2;  /* proto minor */

    uint8_t *addl_len_ptr = buf + pos;
    *(uint16_t*)(buf+pos) = 0; pos+=2;   /* additional data length (filled later) */

    int data_start = pos;

    *(uint32_t*)(buf+pos) = 60000000;   pos+=4;  /* release number */
    *(uint32_t*)(buf+pos) = 0x00200000; pos+=4;  /* resource_id_base */
    *(uint32_t*)(buf+pos) = 0x001FFFFF; pos+=4;  /* resource_id_mask */
    *(uint32_t*)(buf+pos) = 256;        pos+=4;  /* motion_buffer_size */

    const char *vendor = "Ultra Loader X11";
    uint16_t vlen = (uint16_t)strlen(vendor);
    *(uint16_t*)(buf+pos) = vlen;  pos+=2;  /* vendor string length */
    *(uint16_t*)(buf+pos) = 65535; pos+=2;  /* max request length */
    buf[pos++] = 1;   /* number of screens */
    buf[pos++] = 2;   /* number of pixmap formats */
    buf[pos++] = 0;   /* image byte order: LSBFirst */
    buf[pos++] = 0;   /* bitmap bit order: LeastSignificant */
    buf[pos++] = 32;  /* bitmap scan unit */
    buf[pos++] = 32;  /* bitmap scan pad */
    buf[pos++] = 32;  /* bitmap stride */
    buf[pos++] = 0;   /* pad */
    buf[pos++] = 8;   /* min keycode */
    buf[pos++] = 255; /* max keycode */
    pos += 4;         /* pad */

    /* vendor string padded to 4 bytes */
    memcpy(buf+pos, vendor, vlen); pos += vlen;
    while (pos & 3) buf[pos++] = 0;

    /* pixmap format 1: depth=1, bpp=1, scanline-pad=32 */
    buf[pos++]=1; buf[pos++]=1; buf[pos++]=32;
    pos+=5;  /* pad to 8 bytes */

    /* pixmap format 2: depth=24, bpp=32, scanline-pad=32 */
    buf[pos++]=24; buf[pos++]=32; buf[pos++]=32;
    pos+=5;

    /* Screen */
    *(uint32_t*)(buf+pos) = 0x00200001; pos+=4;  /* root window */
    *(uint32_t*)(buf+pos) = 0x00200020; pos+=4;  /* default colormap */
    *(uint32_t*)(buf+pos) = 0x00FFFFFF; pos+=4;  /* white pixel */
    *(uint32_t*)(buf+pos) = 0x00000000; pos+=4;  /* black pixel */
    *(uint32_t*)(buf+pos) = 0x00FFFFFF; pos+=4;  /* current input masks */
    *(uint16_t*)(buf+pos) = 1280; pos+=2;
    *(uint16_t*)(buf+pos) = 720;  pos+=2;
    *(uint16_t*)(buf+pos) = 338;  pos+=2;        /* width mm */
    *(uint16_t*)(buf+pos) = 190;  pos+=2;        /* height mm */
    *(uint16_t*)(buf+pos) = 1; pos+=2;           /* min maps */
    *(uint16_t*)(buf+pos) = 1; pos+=2;           /* max maps */
    *(uint32_t*)(buf+pos) = 0x21; pos+=4;        /* root visual id (TrueColor) */
    buf[pos++] = 4;  /* backing store: Always */
    buf[pos++] = 1;  /* save unders */
    buf[pos++] = 24; /* root depth */
    buf[pos++] = 1;  /* number of depths */

    /* Depth */
    buf[pos++] = 24; /* depth */
    buf[pos++] = 0;  /* pad */
    *(uint16_t*)(buf+pos) = 1; pos+=2;  /* number of visuals */
    pos += 4;  /* pad */

    /* Visual */
    *(uint32_t*)(buf+pos) = 0x21; pos+=4;        /* visual id */
    buf[pos++] = 4;   /* class: TrueColor */
    buf[pos++] = 8;   /* bits per rgb */
    *(uint16_t*)(buf+pos) = 256; pos+=2;         /* colormap entries */
    *(uint32_t*)(buf+pos) = 0x00FF0000; pos+=4;  /* red mask */
    *(uint32_t*)(buf+pos) = 0x0000FF00; pos+=4;  /* green mask */
    *(uint32_t*)(buf+pos) = 0x000000FF; pos+=4;  /* blue mask */
    pos += 4;  /* pad */

    /* Fill in additional data length (in 4-byte units) */
    int data_bytes = pos - data_start;
    *(uint16_t*)addl_len_ptr = (uint16_t)(data_bytes / 4);

    return pos;
}

/* ── Handle one client connection ────────────────────────────────────────*/
static void handle_client(SOCKET client)
{
    loader_log(LOG_INFO, "[X11] Client connected");

    /* Read connection setup request */
    typedef struct {
        uint8_t  byte_order;
        uint8_t  pad0;
        uint16_t proto_major;
        uint16_t proto_minor;
        uint16_t auth_proto_len;
        uint16_t auth_data_len;
        uint16_t pad1;
    } x11_setup_req_t;

    x11_setup_req_t req;
    int n = recv(client, (char*)&req, sizeof(req), MSG_WAITALL);
    if (n < (int)sizeof(req)) {
        loader_log(LOG_WARN, "[X11] Short setup request (%d bytes)", n);
        closesocket(client);
        return;
    }

    /* Drain auth data */
    int skip = 0;
    if (req.auth_proto_len) skip += (req.auth_proto_len + 3) & ~3;
    if (req.auth_data_len)  skip += (req.auth_data_len  + 3) & ~3;
    if (skip > 0) {
        char tmp[256];
        while (skip > 0) {
            int r = recv(client, tmp, skip > 256 ? 256 : skip, 0);
            if (r <= 0) break;
            skip -= r;
        }
    }

    loader_log(LOG_INFO, "[X11] Setup: byte_order=0x%02x proto=%d.%d",
               req.byte_order, req.proto_major, req.proto_minor);

    /* Send setup response */
    uint8_t resp[1024];
    int resp_len = build_setup_response(resp, sizeof(resp));
    send(client, (char*)resp, resp_len, 0);
    loader_log(LOG_INFO, "[X11] Setup response sent (%d bytes)", resp_len);

    /* Request loop */
    uint32_t next_id = 0x00200100;
    uint32_t seq = 1;

    while (g_running) {
        uint8_t hdr[4];
        n = recv(client, (char*)hdr, 4, MSG_WAITALL);
        if (n <= 0) break;

        uint8_t  opcode  = hdr[0];
        uint16_t req_len = *(uint16_t*)(hdr+2);
        int      data_len = (req_len > 1) ? (req_len-1)*4 : 0;

        uint8_t data[4096];
        if (data_len > 0 && data_len <= (int)sizeof(data))
            recv(client, (char*)data, data_len, MSG_WAITALL);

        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1;  /* reply */
        *(uint16_t*)(reply+2) = (uint16_t)seq++;

        switch (opcode) {
            case 98:  /* QueryExtension - report not present */
                send(client, (char*)reply, 32, 0);
                break;

            case 16:  /* InternAtom */
                *(uint32_t*)(reply+8) = next_id++;
                send(client, (char*)reply, 32, 0);
                break;

            case 3:   /* GetWindowAttributes */
            case 14:  /* GetGeometry */
                reply[1] = 1;
                *(uint32_t*)(reply+8)  = 0x00200001;  /* root */
                *(uint16_t*)(reply+12) = 1280;
                *(uint16_t*)(reply+14) = 720;
                send(client, (char*)reply, 32, 0);
                break;

            case 38:  /* QueryPointer */
                reply[1] = 1;
                *(uint32_t*)(reply+8) = 0x00200001;
                send(client, (char*)reply, 32, 0);
                break;

            /* Requests that need no reply */
            case 1:   /* CreateWindow */
            case 2:   /* ChangeWindowAttributes */
            case 4:   /* DestroyWindow */
            case 5:   /* DestroySubwindows */
            case 6:   /* ChangeSaveSet */
            case 7:   /* ReparentWindow */
            case 8:   /* MapWindow */
            case 9:   /* MapSubwindows */
            case 10:  /* UnmapWindow */
            case 11:  /* UnmapSubwindows */
            case 12:  /* ConfigureWindow */
            case 15:  /* QueryTree */
            case 20:  /* GetProperty */
            case 23:  /* GetSelectionOwner */
                break;

            default:
                break;
        }
    }

    loader_log(LOG_INFO, "[X11] Client disconnected");
    closesocket(client);
}

/* ── Server thread ────────────────────────────────────────────────────────*/
static DWORD WINAPI x11_server_thread(void *arg)
{
    (void)arg;
    loader_log(LOG_INFO, "[X11] Server thread started on 127.0.0.1:%d", X11_PORT);

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen_sock, &fds);
        struct timeval tv = {0, 100000};  /* 100ms poll */

        int r = select((int)g_listen_sock+1, &fds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct sockaddr_in addr;
        int addrlen = sizeof(addr);
        SOCKET client = accept(g_listen_sock, (struct sockaddr*)&addr, &addrlen);
        if (client == INVALID_SOCKET) continue;

        handle_client(client);
    }

    loader_log(LOG_INFO, "[X11] Server thread exiting");
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────*/
bool x11_server_start(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_sock == INVALID_SOCKET) {
        loader_log(LOG_WARN, "[X11] socket() failed: %d", WSAGetLastError());
        return false;
    }

    int opt = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(X11_PORT);

    if (bind(g_listen_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        loader_log(LOG_WARN, "[X11] bind() failed on port %d: %d",
                   X11_PORT, WSAGetLastError());
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        return false;
    }

    listen(g_listen_sock, X11_BACKLOG);

    g_running = true;
    g_server_thread = CreateThread(NULL, 0, x11_server_thread, NULL, 0, NULL);
    if (!g_server_thread) {
        loader_log(LOG_WARN, "[X11] CreateThread failed: %lu", GetLastError());
        g_running = false;
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        return false;
    }

    loader_log(LOG_INFO, "[X11] Fake X server listening on 127.0.0.1:%d", X11_PORT);
    return true;
}

void x11_server_stop(void)
{
    g_running = false;
    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    if (g_server_thread) {
        WaitForSingleObject(g_server_thread, 2000);
        CloseHandle(g_server_thread);
        g_server_thread = NULL;
    }
}
