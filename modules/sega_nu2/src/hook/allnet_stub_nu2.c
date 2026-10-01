/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  allnet_stub_nu2.c -- ALL.Net loopback + naomitest.hostname DNS override
 *                       (Sega Nu2 board module -- fresh, independent copy).
 * =============================================================================
 *
 *  ENGINE FACTS:
 *    Sega Nu2 titles reach the ALL.Net billing/authorization backend via TWO
 *    layers:
 *      (a) DNS lookup for `naomitest.hostname` (the well-known ALL.Net
 *          production endpoint on the arcade LAN).
 *      (b) HTTP GET `/sys/servlet/PowerOn` (also under /netfw/) on port 80 or 8080
 *          expecting a form-encoded stat=1 body.
 *    If (a) fails the game retries a few times then aborts to a "network
 *    error" screen; if (b) returns anything other than stat=1 the game aborts
 *    with a bill-server error.
 *
 *  WHAT THIS STUB DOES:
 *    - IAT-hook gethostbyname / getaddrinfo (ws2_32.dll): any name matching
 *      naomitest.hostname (or the substring "naomitest") resolves to
 *      127.0.0.1.
 *    - Bind a tiny HTTP responder on 127.0.0.1:80 (or the env-overridden port)
 *      that answers every GET with a stat=1 ALL.Net form body. Runs in a
 *      worker thread; no incoming connect = idle worker.
 *    - Byte-safe: if port 80 is already bound (a real webserver is running),
 *      we log and continue -- the DNS override alone is often enough because
 *      the game's retry loop is DNS-first.
 *
 *  Env gate: ULTRA_SEGA_ALLNET=1 (set by sega_nu2_main.c).
 *  Env knobs:
 *    ULTRA_SEGA_ALLNET_PORT   -- override the loopback port (default 80)
 *    ULTRA_SEGA_ALLNET_HOST   -- comma-separated hostnames to redirect
 *                                (default "naomitest.hostname,mucha-front.sega.co.jp")
 * =============================================================================
 */
#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "sega_nu2_hook.h"

typedef struct hostent* (WSAAPI *pGetHostByName)(const char*);
typedef int  (WSAAPI *pGetAddrInfo)(PCSTR,PCSTR,const ADDRINFOA*,PADDRINFOA*);
typedef void (WSAAPI *pFreeAddrInfo)(PADDRINFOA);
typedef int  (WSAAPI *pGetAddrInfoW)(PCWSTR,PCWSTR,const ADDRINFOW*,PADDRINFOW*);
typedef void (WSAAPI *pFreeAddrInfoW)(PADDRINFOW);

static pGetHostByName  Real_gethostbyname = NULL;
static pGetAddrInfo    Real_getaddrinfo   = NULL;
static pFreeAddrInfo   Real_freeaddrinfo  = NULL;
static pGetAddrInfoW   Real_GetAddrInfoW  = NULL;
static pFreeAddrInfoW  Real_FreeAddrInfoW = NULL;

static int g_enabled = 0;
static int g_port    = 80;
static char g_hosts[512] = "naomitest.hostname,mucha-front.sega.co.jp,ib.naominet.jp";

static int host_matches(const char* name) {
    if (!name || !*name) return 0;
    char lower[256] = {0};
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    /* Scan comma-separated list of allowed hosts. */
    const char* p = g_hosts;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        /* trim leading spaces */
        while (len && *p == ' ') { ++p; --len; }
        if (len > 0) {
            char buf[128] = {0};
            size_t c = len < sizeof(buf)-1 ? len : sizeof(buf)-1;
            for (size_t i = 0; i < c; ++i) {
                char q = p[i];
                buf[i] = (q >= 'A' && q <= 'Z') ? (char)(q + 32) : q;
            }
            /* substring match; the game sometimes prefixes/subdomains */
            if (strstr(lower, buf)) return 1;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Static hostent for the redirected lookup. Windows keeps this in TLS so
 * repeated calls are safe. */
static struct hostent g_hostent;
static struct in_addr g_localhost_addr;
static char*  g_addr_list[2];
static char   g_official_name[64];

static struct hostent* WSAAPI Hook_gethostbyname(const char* name) {
    if (g_enabled && host_matches(name)) {
        ul_sn2_log("allnet: gethostbyname(%s) -> 127.0.0.1\n", name);
        g_localhost_addr.s_addr = htonl(0x7F000001);
        g_addr_list[0] = (char*)&g_localhost_addr;
        g_addr_list[1] = NULL;
        strncpy(g_official_name, name, sizeof(g_official_name)-1);
        g_official_name[sizeof(g_official_name)-1] = 0;
        g_hostent.h_name       = g_official_name;
        g_hostent.h_aliases    = NULL;
        g_hostent.h_addrtype   = AF_INET;
        g_hostent.h_length     = sizeof(struct in_addr);
        g_hostent.h_addr_list  = g_addr_list;
        return &g_hostent;
    }
    return Real_gethostbyname ? Real_gethostbyname(name) : NULL;
}

static int WSAAPI Hook_getaddrinfo(PCSTR node, PCSTR svc,
        const ADDRINFOA* hints, PADDRINFOA* out) {
    if (g_enabled && host_matches(node)) {
        ul_sn2_log("allnet: getaddrinfo(%s) -> 127.0.0.1\n", node);
        /* Rewrite the node to "127.0.0.1" and delegate to the real API so we
         * get a properly ref-counted result the app will freeaddrinfo cleanly. */
        return Real_getaddrinfo
            ? Real_getaddrinfo("127.0.0.1", svc, hints, out)
            : getaddrinfo("127.0.0.1", svc, hints, out);
    }
    return Real_getaddrinfo ? Real_getaddrinfo(node, svc, hints, out)
                            : getaddrinfo(node, svc, hints, out);
}
static int WSAAPI Hook_GetAddrInfoW(PCWSTR node, PCWSTR svc,
        const ADDRINFOW* hints, PADDRINFOW* out) {
    if (g_enabled && node) {
        char narrow[256] = {0};
        int wl = WideCharToMultiByte(CP_UTF8, 0, node, -1, narrow, sizeof narrow - 1, NULL, NULL);
        (void)wl;
        if (host_matches(narrow)) {
            ul_sn2_log("allnet: GetAddrInfoW(%s) -> 127.0.0.1\n", narrow);
            return Real_GetAddrInfoW
                ? Real_GetAddrInfoW(L"127.0.0.1", svc, hints, out)
                : GetAddrInfoW(L"127.0.0.1", svc, hints, out);
        }
    }
    return Real_GetAddrInfoW ? Real_GetAddrInfoW(node, svc, hints, out)
                             : GetAddrInfoW(node, svc, hints, out);
}

/* Loopback HTTP server: answer every request with the ALL.Net PowerOn form
 * body. Runs until process exit. Byte-safe: bind failure = log + exit. */
static DWORD WINAPI allnet_http_server(LPVOID arg) {
    (void)arg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        ul_sn2_log("allnet: WSAStartup failed\n");
        return 1;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        ul_sn2_log("allnet: socket() failed\n");
        return 1;
    }
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    a.sin_port = htons((u_short)g_port);
    if (bind(srv, (struct sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
        ul_sn2_log("allnet: bind 127.0.0.1:%d failed (%d) -- DNS override still active\n",
                  g_port, WSAGetLastError());
        closesocket(srv);
        return 1;
    }
    if (listen(srv, 4) == SOCKET_ERROR) {
        ul_sn2_log("allnet: listen failed\n");
        closesocket(srv);
        return 1;
    }
    ul_sn2_log("allnet: HTTP responder listening on 127.0.0.1:%d\n", g_port);
    static const char body[] =
        "stat=1&uri=http://127.0.0.1/&host=naomitest.hostname"
        "&place_id=0123&name=Ultra&nickname=UltraLoader"
        "&region0=1&region_name0=W&region_name1=W&region_name2=W&region_name3=W"
        "&country=JPN&allnet_id=UltraLoader\r\n";
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n", (int)(sizeof(body) - 1));
    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(10); continue; }
        char req[1024];
        int r = recv(c, req, sizeof req - 1, 0);
        if (r > 0) req[r] = 0;
        send(c, hdr,  hl, 0);
        send(c, body, sizeof(body) - 1, 0);
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
}

void ul_sn2_install_allnet(void) {
    if (!ul_sn2_env_on("ULTRA_SEGA_ALLNET")) {
        ul_sn2_log("allnet: skipped (ULTRA_SEGA_ALLNET!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    const char* portstr = ul_sn2_env("ULTRA_SEGA_ALLNET_PORT");
    if (portstr) g_port = atoi(portstr);
    if (g_port <= 0 || g_port > 65535) g_port = 80;

    const char* hosts = ul_sn2_env("ULTRA_SEGA_ALLNET_HOST");
    if (hosts && *hosts) {
        strncpy(g_hosts, hosts, sizeof(g_hosts)-1);
        g_hosts[sizeof(g_hosts)-1] = 0;
    }

    HMODULE ws = LoadLibraryA("ws2_32.dll");
    if (ws) {
        Real_gethostbyname = (pGetHostByName)(void*)GetProcAddress(ws, "gethostbyname");
        Real_getaddrinfo   = (pGetAddrInfo)  (void*)GetProcAddress(ws, "getaddrinfo");
        Real_freeaddrinfo  = (pFreeAddrInfo) (void*)GetProcAddress(ws, "freeaddrinfo");
        Real_GetAddrInfoW  = (pGetAddrInfoW) (void*)GetProcAddress(ws, "GetAddrInfoW");
        Real_FreeAddrInfoW = (pFreeAddrInfoW)(void*)GetProcAddress(ws, "FreeAddrInfoW");
    }
    ul_sn2_iat_patch_all("ws2_32.dll", "gethostbyname", (void*)Hook_gethostbyname);
    ul_sn2_iat_patch_all("ws2_32.dll", "getaddrinfo",   (void*)Hook_getaddrinfo);
    ul_sn2_iat_patch_all("ws2_32.dll", "GetAddrInfoW",  (void*)Hook_GetAddrInfoW);
    /* Also patch wsock32.dll (some legacy Sega network shims use it). */
    ul_sn2_iat_patch_all("wsock32.dll", "gethostbyname", (void*)Hook_gethostbyname);

    HANDLE th = CreateThread(NULL, 0, allnet_http_server, NULL, 0, NULL);
    if (th) CloseHandle(th);
    ul_sn2_log("allnet: armed (port=%d, hosts=%s)\n", g_port, g_hosts);
}

#endif /* _WIN32 */
