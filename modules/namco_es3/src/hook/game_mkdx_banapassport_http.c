/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_mkdx_banapassport_http.c -- Mario Kart Arcade GP DX: in-process
 *                                    no-op responder for the Bananapass
 *                                    registration-query network check
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS
 *  ---------------------------------------------------------------------------
 *  Decompiling MK_AGP3_FINAL.exe (Ghidra, 2026-09-29) found the real cause of
 *  "cannot reach Bananapass": the exe makes a real WinHTTP POST over HTTPS to
 *      https://user.banapassport.net/regq.html?id=013&ac=<code>
 *  (confirmed the ONLY https:// URL anywhere in the binary -- exhaustive
 *  string scan). That domain is real, live, AWS-hosted, and answers -- but it
 *  is Bandai Namco's own production BanaPassport authentication service and
 *  will never accept a request from a cabinet with no real Bandai Namco
 *  network credentials. Decompiling FUN_00478190 (the WinHTTP status
 *  callback's response handler) shows any HTTP status other than exactly 200
 *  is treated as a hard failure with no retry/offline fallback -- consistent
 *  with what an unauthenticated cabinet would see hitting a real production
 *  auth server.
 *
 *  The request/response body is JSON (JsonCpp, confirmed via RTTI: Json::
 *  Writer / Json::FastWriter vtables referenced directly from this code
 *  path). Decompiling the response-handler virtual method for two separate
 *  data categories (clHttpReq_BookkeepingInfo -- the save/load data this
 *  project's Bananapass feature actually needs -- and clHttpReq_BoardInfo)
 *  found the IDENTICAL pattern in both:
 *      value = response["result"]; ok = (value == 0);   // else: NG
 *  logged literally as "true"/"false". This is a shared base-class contract
 *  (clHttpRequestBase), not per-category, so it almost certainly holds for
 *  all nine clHttpReq_* categories (AccountingInfo, AMIDInfo, BoardInfo,
 *  BookkeepingInfo, CloneBattleInfo, Develop, EventInfo, GamePlayInfo,
 *  CommonInfo) confirmed to exist via RTTI type descriptors.
 *
 *  APPROACH: don't reverse-engineer Bandai Namco's real server or stand up a
 *  TLS endpoint (the game validates the cert with no bypass flag set, and a
 *  self-signed local cert would just trade one hard-fail for another). Since
 *  this exe has exactly one HTTPS call site in its entire body, it is safe to
 *  intercept at the WinHTTP API boundary itself, in-process, the same way
 *  this project already replaces IAuth/AMAuthd (game_mkdx_iauth.c) and the
 *  COM2 card vendor (game_mkdx_bananapass.c):
 *    - WinHttpConnect: when the target host is user.banapassport.net,
 *      redirect to a loopback responder this file itself runs -- no real
 *      network, no DNS, no Bandai Namco server involved at all.
 *    - WinHttpOpenRequest: drop the WINHTTP_FLAG_SECURE bit so the
 *      redirected connection is plain HTTP to our own loopback responder,
 *      sidestepping TLS/cert validation entirely (safe: the one call site
 *      this affects is the one we just redirected to ourselves).
 *  The rest of the game's real WinHTTP async state machine (WinHttpSend
 *  Request, WinHttpSetStatusCallback, WinHttpReceiveResponse, WinHttpQuery
 *  Headers, WinHttpReadData) runs completely untouched, for real, against
 *  our loopback responder -- only the destination changed.
 *
 *  RESPONSE CONTRACT
 *  ---------------------------------------------------------------------------
 *  {"result":0}  -- confirmed success value for every category checked so
 *  far. Category-specific fields beyond this bare minimum (e.g. Bookkeeping
 *  Info's save-data array) are NOT yet reverse-engineered; ULTRA_JVS_LOG will
 *  show exactly which category/step fails next if the bare {"result":0}
 *  answer is not enough for some categories, so the next capture is targeted
 *  instead of another guess.
 *
 *  GATING
 *  ---------------------------------------------------------------------------
 *  Same exe-identity check as game_mkdx_bananapass.c / game_mkdx_iauth.c
 *  (6,819,840 bytes / MK_AGP3_FINAL.exe / ULTRA_MKDX_ID=1 escape hatch).
 *  Unconditional once identity matches -- this is the same "no card, no
 *  hardware, none of that -- just get me speaking the game's own network
 *  check" role as the IAuth stub, not an optional feature.
 * =============================================================================
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* Loopback responder port. Arbitrary, private, chosen to avoid clashing with
 * any other service this box might run. */
#define BP_LOCAL_PORT 18099

static void bp_http_log(const char* fmt, ...) {
    char lp[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", lp, sizeof lp);
    if (!(n > 0 && n < sizeof lp && lp[0])) return;
    FILE* f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "[BP_HTTP] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 *  Loopback plain-HTTP responder. Answers EVERY request the same way: 200 OK,
 *  {"result":0}. No routing, no parsing of the request beyond waiting for it
 *  to finish arriving -- this process only ever redirects one URL here.
 * -------------------------------------------------------------------------- */
static const char BP_BODY[] = "{\"result\":0}";

/* Case-insensitive substring search -- strstr() alone would miss "content-
 * length:" vs "Content-Length:", and we don't control the game's casing. */
static const char* bp_stristr(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return p;
    return NULL;
}

static void bp_serve_one(SOCKET c) {
    /* Sized for the biggest Bananapass JSON payload we've seen reason to
     * expect (BookkeepingInfo's save-data array); grow if a real capture
     * ever gets truncated (this function logs whatever it captured either
     * way, truncated or not, so a real cap-hit is visible, not silent). */
    static char buf[65536];
    int total = 0;
    int headers_end = -1;
    long content_length = -1;

    for (;;) {
        int n = recv(c, buf + total, (int)sizeof(buf) - total - 1, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (headers_end < 0) {
            const char* he = strstr(buf, "\r\n\r\n");
            if (he) {
                headers_end = (int)(he - buf) + 4;
                const char* cl = bp_stristr(buf, "content-length:");
                if (cl && cl < he) content_length = atol(cl + 16);
            }
        }
        if (headers_end >= 0) {
            if (content_length < 0) break;                         /* no body expected */
            if (total - headers_end >= content_length) break;       /* full body in hand */
        }
        if (total >= (int)sizeof(buf) - 1) break;                   /* safety cap hit */
    }

    /* Log the ENTIRE raw request (request line + headers + body) exactly as
     * received -- this is the real, captured wire format the very first time
     * the game actually reaches this endpoint, instead of another decompiled
     * guess. Multi-line bodies (JSON) print fine through vfprintf("%s"). */
    bp_http_log("REQUEST (%d bytes, content_length=%ld):\n%s\n[/REQUEST]",
                total, content_length, buf);

    char resp[512];
    int blen = (int)sizeof(BP_BODY) - 1;
    int rlen = snprintf(resp, sizeof resp,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", blen, BP_BODY);
    send(c, resp, rlen, 0);
    shutdown(c, SD_SEND);
    closesocket(c);
}

static DWORD WINAPI bp_server_thread(LPVOID arg) {
    (void)arg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        bp_http_log("WSAStartup failed, loopback responder cannot start");
        return 0;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        bp_http_log("socket() failed: %d", WSAGetLastError());
        return 0;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(BP_LOCAL_PORT);

    if (bind(s, (struct sockaddr*)&addr, sizeof addr) == SOCKET_ERROR) {
        bp_http_log("bind() on 127.0.0.1:%d failed: %d", BP_LOCAL_PORT, WSAGetLastError());
        closesocket(s);
        return 0;
    }
    if (listen(s, 8) == SOCKET_ERROR) {
        bp_http_log("listen() failed: %d", WSAGetLastError());
        closesocket(s);
        return 0;
    }
    bp_http_log("loopback Bananapass responder listening on 127.0.0.1:%d", BP_LOCAL_PORT);

    for (;;) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        bp_serve_one(c);
    }
    /* unreachable */
}

static void bp_start_server_once(void) {
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    CreateThread(NULL, 0, bp_server_thread, NULL, 0, NULL);
}

/* ---------------------------------------------------------------------------
 *  WinHttpConnect / WinHttpOpenRequest interception
 * -------------------------------------------------------------------------- */
typedef HINTERNET (WINAPI *PFN_WinHttpConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *PFN_WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR,
                                                    LPCWSTR, LPCWSTR*, DWORD);

static PFN_WinHttpConnect     bp_real_WinHttpConnect = NULL;
static PFN_WinHttpOpenRequest bp_real_WinHttpOpenRequest = NULL;

static int bp_is_target_host(LPCWSTR host) {
    if (!host) return 0;
    return _wcsicmp(host, L"user.banapassport.net") == 0;
}

static HINTERNET WINAPI bp_Hook_WinHttpConnect(HINTERNET hSession, LPCWSTR pswzServerName,
                                                INTERNET_PORT nServerPort, DWORD dwReserved) {
    if (bp_is_target_host(pswzServerName)) {
        bp_start_server_once();
        bp_http_log("WinHttpConnect(%ls) -> redirected to loopback 127.0.0.1:%d",
                    pswzServerName, BP_LOCAL_PORT);
        return bp_real_WinHttpConnect ?
            bp_real_WinHttpConnect(hSession, L"127.0.0.1", BP_LOCAL_PORT, dwReserved) :
            NULL;
    }
    return bp_real_WinHttpConnect ?
        bp_real_WinHttpConnect(hSession, pswzServerName, nServerPort, dwReserved) : NULL;
}

static HINTERNET WINAPI bp_Hook_WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pwszVerb,
        LPCWSTR pwszObjectName, LPCWSTR pwszVersion, LPCWSTR pwszReferrer,
        LPCWSTR *ppwszAcceptTypes, DWORD dwFlags) {
    /* This exe has exactly one HTTPS call site anywhere in it (confirmed by
     * exhaustive string scan), and it is the one WinHttpConnect above just
     * redirected to our own loopback responder -- dropping the secure flag
     * unconditionally is safe and avoids needing a trusted TLS certificate
     * for a domain we don't own. */
    DWORD flags = dwFlags & ~((DWORD)0x00800000 /* WINHTTP_FLAG_SECURE */);
    if (flags != dwFlags) {
        bp_http_log("WinHttpOpenRequest(%ls %ls) -> dropped WINHTTP_FLAG_SECURE (talking to our own loopback responder)",
                    pwszVerb ? pwszVerb : L"", pwszObjectName ? pwszObjectName : L"");
    }
    return bp_real_WinHttpOpenRequest ?
        bp_real_WinHttpOpenRequest(hConnect, pwszVerb, pwszObjectName, pwszVersion,
                                    pwszReferrer, ppwszAcceptTypes, flags) : NULL;
}

/* ---------------------------------------------------------------------------
 *  Auto-registration -- same identity check as game_mkdx_bananapass.c /
 *  game_mkdx_iauth.c.
 * -------------------------------------------------------------------------- */
/* TEMPORARY unconditional bisection tracing (bring-up only, same pattern as
 * jvs_core_es3.c's cre_trace): zero [BP_HTTP] output ever appeared in a real
 * test despite the module being confirmed staged/signature-verified/injected
 * (ultra.log) and this exact log string being confirmed present in the built
 * module -- so either the identity gate isn't matching, or something in here
 * is failing/crashing silently before the old single log-line at the end.
 * This traces every step so the next run says exactly where it stops.
 * Remove once BP_HTTP is confirmed to actually run end to end. */
static void bpt(const char* fmt, ...) {
    char lp[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", lp, sizeof lp);
    if (!(n > 0 && n < sizeof lp && lp[0])) return;
    FILE* f = fopen(lp, "a");
    if (!f) return;
    fprintf(f, "[BPT] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void __attribute__((constructor)) game_mkdx_banapassport_http_autoregister(void) {
    bpt("autoregister: enter");
    int is_mkdx = 0;
    const char* e = getenv("ULTRA_MKDX_ID");
    if (e && e[0] == '1') is_mkdx = 1;
    if (!is_mkdx) {
        char p[MAX_PATH];
        DWORD pn = GetModuleFileNameA(NULL, p, MAX_PATH);
        bpt("autoregister: GetModuleFileNameA -> n=%lu p=%s", (unsigned long)pn, pn ? p : "?");
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) {
            bpt("autoregister: file size = %lu (want 6819840)", (unsigned long)fa.nFileSizeLow);
            if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 6819840)
                is_mkdx = 1;
        } else {
            bpt("autoregister: GetFileAttributesExA FAILED, gle=%lu", (unsigned long)GetLastError());
        }
        if (!is_mkdx) {
            const char* base = strrchr(p, '\\');
            base = base ? base + 1 : p;
            bpt("autoregister: basename=%s", base);
            if (_stricmp(base, "MK_AGP3_FINAL.exe") == 0) is_mkdx = 1;
        }
    }
    bpt("autoregister: is_mkdx=%d", is_mkdx);
    if (!is_mkdx) { bpt("autoregister: STOP -- identity did not match"); return; }

    HMODULE winhttp = GetModuleHandleA("winhttp.dll");
    bpt("autoregister: GetModuleHandleA(winhttp.dll) -> %p", (void*)winhttp);
    if (winhttp) {
        bp_real_WinHttpConnect = (PFN_WinHttpConnect)GetProcAddress(winhttp, "WinHttpConnect");
        bp_real_WinHttpOpenRequest = (PFN_WinHttpOpenRequest)GetProcAddress(winhttp, "WinHttpOpenRequest");
        bpt("autoregister: real fn ptrs Connect=%p OpenRequest=%p",
            (void*)bp_real_WinHttpConnect, (void*)bp_real_WinHttpOpenRequest);
    }
    bpt("autoregister: about to IAT-patch WinHttpConnect");
    ultra_iat_patch_all("winhttp.dll", "WinHttpConnect", (void*)bp_Hook_WinHttpConnect);
    bpt("autoregister: IAT-patched WinHttpConnect OK, about to patch WinHttpOpenRequest");
    ultra_iat_patch_all("winhttp.dll", "WinHttpOpenRequest", (void*)bp_Hook_WinHttpOpenRequest);
    bpt("autoregister: IAT-patched WinHttpOpenRequest OK");
    bp_http_log("Bananapass/BanaPassport HTTP no-op responder armed for MK_AGP3_FINAL.exe");
    bpt("autoregister: DONE");
}

#endif /* _WIN32 */
