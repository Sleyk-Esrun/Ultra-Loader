/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pcp_daemon_ringedge2.c -- the local Sega daemon this board cannot boot without
 * =============================================================================
 *
 *  WHY THIS IS IN THE HOOK AND NOT A SCRIPT YOU REMEMBER TO RUN.
 *
 *  Caladrius does not reach a single frame without a local daemon answering on
 *  loopback. On a real RingEdge cabinet that is Sega's own amDaemon. Without it
 *  the game loads every ROM, brings up DirectSound, and then parks its MAIN
 *  THREAD on a 60-second receive timeout, forever:
 *
 *      [TRACE/NET]  connect -> 127.0.0.1:40100
 *      [TRACE/WAIT] blocked 2047 ms -> signaled
 *      [TRACE/WAIT] blocked 9969 ms -> TIMEOUT      (repeating)
 *
 *  A Python stand-in proved the protocol, but it only works on the runs someone
 *  remembers to start it on -- and a launch without it looks EXACTLY like a
 *  broken game: no window, "not responding", nothing in the log to suggest a
 *  missing helper. That has now cost one debugging round already. A dependency
 *  the user has to remember is not a dependency that is met, so it lives here:
 *  it starts with the game, dies with the game, and cannot be forgotten.
 *
 *  THE PROTOCOL, and how much of it is actually known.
 *
 *  Sega's PCP. The SERVER SPEAKS FIRST -- measured, not assumed: the game
 *  connects, sends zero bytes, and waits out the full 60 s. The thing it is
 *  waiting for is one byte:
 *
 *      pcppSendPrompt 0x0056EC70:  c6 00 3e   mov byte ptr [eax], 0x3E   ('>')
 *                                  length set to 1 at 0x0056ED03
 *      pcppCheckPrompt 0x0056D9E0: scans the receive buffer for 0x3E, deletes
 *                                  it, and returns true if it deleted any
 *
 *  So: no CR, no LF, no NUL, no banner. One '>' on accept, and another after
 *  each reply so a persistent connection keeps working. Requests and replies
 *  are then CRLF-terminated "command=value&key=value" lines.
 *
 *  The 60 s is not a guess either: amMasterOpen 0x0054A440 calls
 *  pcpaOpenClient("127.0.0.1", port, 0xEA60 = 60000, 0).
 *
 *  TWO PORTS, because both the tracer and the probe were right about different
 *  things and it caused real confusion:
 *      40100  amMaster  -- amMasterInit 0x0054A250 writes 0x9CA4
 *      40190  amAuth    -- accessServer 0x0054D770 pushes 0x9CFE
 *      40192             -- an amAuth debug variant, bound as cheap insurance
 *
 *  WHAT IS GUESSED. The reply BODIES. The prompt and the framing are verified
 *  byte-exact; the field values below are the smallest set that got the game
 *  through auth on 2026-09-17 at 21:25, confirmed on the wire. "count=0" is
 *  free -- the parsed value's low byte is stored to 0x006E400E and a whole-image
 *  scan finds exactly one reference, the write itself. Nothing reads it.
 *
 *  THE COMPLETE COMMAND SURFACE, taken from the game's own .rdata rather than
 *  from a protocol description, so nothing here is inferred. Offsets are file
 *  offsets in SILVER_AMP_R_ROM.exe (ImageBase 0x400000, DYNAMICBASE=0):
 *
 *    amMaster, port 40100 (0x1BD524-0x1BD630)
 *      mxmaster.reconnect.usb.device   mxmaster.erase_log
 *      mxmaster.output_log             mxmaster.logging_available
 *      mxmaster.develop                mxmaster.foreground.getcount  [modeled]
 *      mxmaster.foreground.fault       mxmaster.foreground.next      [refused]
 *      mxmaster.foreground.active      mxmaster.foreground.current
 *
 *    amAuth, port 40190 (0x1BE024-0x1BEBE0)
 *      run.mode          app.property      keychip / serial.id
 *      current.status    [modeled]        accounting   [modeled]
 *      nearfull.setting  testmode.info     system.time
 *      place.info        network.macaddr   network.eth
 *      network.test      close.time        title.server.address
 *      inst.file.list    title.server.host lan.install.status
 *      keychip.info
 *
 *  Everything not marked [modeled] falls through to the generic answer BELOW
 *  AND LOGS A LINE SAYING SO. That matters more than the answer does: it means
 *  "a command the daemon ignores" is not a silent failure mode here. In the
 *  2026-09-18 09:55 run the game sent exactly two commands in its whole life --
 *  mxmaster.foreground.getcount and current.status -- and there is not one
 *  UNMODELLED line in the log. So the daemon is not what the game is waiting
 *  for: it never asked us for anything else.
 *
 *  mxmaster.foreground.next is REFUSED on purpose. Its handler at 0x0054B750
 *  escalates the session into PCP binary mode on a port we do not serve; a
 *  non-zero code short-circuits at 0x0054AF6F before the handler table is
 *  reached, so the refusal is clean.
 *
 *  Set ULTRA_RE2_PCP=0 to disable this and run an external daemon instead.
 * =============================================================================
 */
#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ringedge2_hook.h"

#define RE2_PCP_PROMPT      '>'
#define RE2_PCP_MAXPORTS    3
#define RE2_PCP_BUF         2048

static const unsigned short k_re2_pcp_ports[RE2_PCP_MAXPORTS] = { 40100, 40190, 40192 };
static void re2_pcp_serve(SOCKET c, unsigned short port);
static volatile LONG g_re2_pcp_started = 0;

/* Reply bodies. Kept as one table so they can be changed without reading the
 * connection code. Every value here is a decision, and the ones that are
 * guesses are marked in the comments above. */
static void re2_pcp_reply(unsigned short port, const char *cmd, const char *val,
                          char *out, size_t outsz)
{
    if (port == 40100) {
        /* amMaster. The echo check at 0x0054B357 compares atoi(our value) to
         * the kind the game sent, so the value is echoed back verbatim. */
        if (!strcmp(cmd, "mxmaster.foreground.getcount")) {
            _snprintf(out, outsz, "%s=%s&code=0&count=0", cmd,
                      (val && *val) ? val : "2");
            return;
        }
        if (!strcmp(cmd, "mxmaster.foreground.next")) {
            /* deliberate refusal -- see the header */
            _snprintf(out, outsz, "%s=%s&code=1", cmd, val ? val : "");
            ul_re2_log("[pcp] REFUSING %s on purpose (binary-mode escalation)", cmd);
            return;
        }
        ul_re2_log("[pcp] *** UNMODELLED amMaster command '%s' -- answering code=0", cmd);
        _snprintf(out, outsz, "%s=%s&code=0", cmd, val ? val : "");
        return;
    }

    /* amAuth. checkHeader compares the command word; ok/busy/ng live at
     * 0x005BF66C / 0x005BF664 / 0x005BF660. */
    if (!strcmp(cmd, "current.status")) {
        _snprintf(out, outsz, "current.status=ok&error=0&icon=normal&exit=false");
        return;
    }
    if (!strcmp(cmd, "accounting")) {
        _snprintf(out, outsz, "accounting=ok");
        return;
    }
    ul_re2_log("[pcp] *** UNMODELLED amAuth command '%s' -- answering ok", cmd);
    _snprintf(out, outsz, "%s=ok", cmd);
}

/* ONE THREAD PER CONNECTION, AND THIS IS NOT OPTIONAL.
 *
 * The first version served connections synchronously inside the accept loop.
 * That works only if a client connects, asks, and disconnects -- and this game
 * does not: it holds each connection OPEN and polls it every frame. So the
 * first connection (amMaster on 40100) parked the accept loop inside recv()
 * forever, 40190 was never accepted, and the game blocked its main thread on
 * auth with its window already created. The result was a white window that
 * Windows killed on the first click, and a log showing the daemon "listening"
 * on all three ports having never served a single request.
 *
 * The Python stand-in threaded each connection and worked; this is the port of
 * that, and the reason it has to be. */
typedef struct { SOCKET c; unsigned short port; } re2_pcp_conn;

static DWORD WINAPI re2_pcp_conn_thread(LPVOID arg)
{
    re2_pcp_conn *k = (re2_pcp_conn *)arg;
    SOCKET c = k->c;
    unsigned short port = k->port;
    free(k);
    re2_pcp_serve(c, port);
    return 0;
}

/* One connection, start to finish. Never lets an error escape: the game
 * reconnects every frame regardless, and a daemon that dies on a malformed
 * line would be worse than no daemon at all. */
static void re2_pcp_serve(SOCKET c, unsigned short port)
{
    char in[RE2_PCP_BUF], line[RE2_PCP_BUF], raw[RE2_PCP_BUF], out[RE2_PCP_BUF];
    int used = 0;
    char p = RE2_PCP_PROMPT;

    ul_re2_log("[pcp] :%u  connection accepted, prompt sent", (unsigned)port);
    if (send(c, &p, 1, 0) != 1) { closesocket(c); return; }

    for (;;) {
        int n = recv(c, in + used, (int)(sizeof in - 1 - used), 0);
        char *nl;
        /* WHO HUNG UP, AND WHEN. This was the one thing neither this daemon nor
         * the Python stand-in ever recorded, and it is the difference between
         * "the game finished the transaction and called pcpaClose" and "the
         * game is still sitting on an open socket waiting for something".
         * Both used to look identical: silence. */
        if (n == 0) {
            ul_re2_log("[pcp] :%u  the GAME closed this connection cleanly "
                       "(pcpaClose) -- its transaction is over", (unsigned)port);
            break;
        }
        if (n < 0) {
            ul_re2_log("[pcp] :%u  recv failed WSA=%d -- connection lost",
                       (unsigned)port, WSAGetLastError());
            break;
        }
        used += n;
        in[used] = 0;

        while ((nl = strchr(in, '\n')) != NULL) {
            char *eq, *amp, *cmd, *val;
            size_t len = (size_t)(nl - in);
            if (len >= sizeof line) len = sizeof line - 1;
            memcpy(line, in, len);
            line[len] = 0;
            while (len && (line[len-1] == '\r' || line[len-1] == ' ')) line[--len] = 0;

            /* consume the line from the buffer */
            memmove(in, nl + 1, (size_t)(used - (nl - in) - 1) + 1);
            used -= (int)(nl - in) + 1;
            if (used < 0) used = 0;

            if (!line[0]) continue;

            /* KEEP THE WHOLE LINE BEFORE IT IS CHOPPED UP. The parse below
             * NUL-terminates at the first '&', which throws away every
             * trailing key=value pair -- and those pairs are the actual
             * request for several commands (run.mode carries process=,
             * accounting carries player=, app.property carries version=). The
             * log used to print only the first pair, so what the game really
             * asked for was never recorded anywhere. */
            strncpy(raw, line, sizeof raw - 1);
            raw[sizeof raw - 1] = 0;

            /* "command=value&key=value..." -- we need the command and its
             * immediate value; the trailing pairs are not read by anything we
             * answer today. */
            cmd = line;
            val = NULL;
            amp = strchr(line, '&');
            if (amp) *amp = 0;
            eq = strchr(cmd, '=');
            if (eq) { *eq = 0; val = eq + 1; }

            out[0] = 0;
            re2_pcp_reply(port, cmd, val, out, sizeof out - 4);
            if (!out[0]) continue;
            strncat(out, "\r\n", sizeof out - strlen(out) - 1);

            ul_re2_log("[pcp] :%u  <- '%s'   -> '%.*s'",
                       (unsigned)port, raw, (int)(strlen(out) - 2), out);

            if (send(c, out, (int)strlen(out), 0) <= 0) goto done;
            /* a fresh prompt so a persistent connection keeps working */
            if (send(c, &p, 1, 0) != 1) goto done;
            ul_re2_log("[pcp] :%u  reply + prompt flushed, waiting for the next "
                       "request on this connection", (unsigned)port);
        }
        if (used >= (int)sizeof in - 1) used = 0;   /* overlong line: resync */
    }
done:
    closesocket(c);
}

static DWORD WINAPI re2_pcp_thread(LPVOID unused)
{
    WSADATA wsa;
    SOCKET ls[RE2_PCP_MAXPORTS];
    int i, nls = 0;

    (void)unused;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ul_re2_log("[pcp] WSAStartup failed -- no local daemon, the game will "
                   "stall on its 60s auth timeout");
        return 0;
    }

    for (i = 0; i < RE2_PCP_MAXPORTS; ++i) {
        struct sockaddr_in a;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL yes = TRUE;
        if (s == INVALID_SOCKET) continue;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(k_re2_pcp_ports[i]);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback ONLY */
        if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
            ul_re2_log("[pcp] port %u already in use -- assuming an external "
                       "daemon owns it and leaving it alone",
                       (unsigned)k_re2_pcp_ports[i]);
            closesocket(s);
            continue;
        }
        ls[nls++] = s;
        ul_re2_log("[pcp] listening on 127.0.0.1:%u", (unsigned)k_re2_pcp_ports[i]);
    }
    if (!nls) {
        ul_re2_log("[pcp] bound nothing -- the game will stall on auth");
        return 0;
    }

    for (;;) {
        fd_set rd;
        struct timeval tv;
        int r;
        FD_ZERO(&rd);
        for (i = 0; i < nls; ++i) FD_SET(ls[i], &rd);
        tv.tv_sec = 1; tv.tv_usec = 0;
        r = select(0, &rd, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        for (i = 0; i < nls; ++i) {
            if (FD_ISSET(ls[i], &rd)) {
                SOCKET c = accept(ls[i], NULL, NULL);
                if (c != INVALID_SOCKET) {
                    re2_pcp_conn *k = (re2_pcp_conn *)malloc(sizeof *k);
                    HANDLE th = NULL;
                    if (k) {
                        k->c = c;
                        k->port = k_re2_pcp_ports[i];
                        th = CreateThread(NULL, 0, re2_pcp_conn_thread, k, 0, NULL);
                    }
                    if (th) {
                        CloseHandle(th);
                    } else {
                        /* Never serve inline as a fallback -- that is exactly
                         * the deadlock this thread exists to avoid. Drop it;
                         * the game reconnects on its next frame. */
                        free(k);
                        closesocket(c);
                        ul_re2_log("[pcp] CreateThread failed err=%lu -- dropped a "
                                   "connection on :%u rather than block the accept loop",
                                   (unsigned long)GetLastError(),
                                   (unsigned)k_re2_pcp_ports[i]);
                    }
                }
            }
        }
    }
    for (i = 0; i < nls; ++i) closesocket(ls[i]);
    return 0;
}

void ul_re2_pcp_install(void)
{
    const char *e = getenv("ULTRA_RE2_PCP");
    HANDLE h;

    if (e && e[0] == '0') {
        ul_re2_log("[pcp] disabled (ULTRA_RE2_PCP=0) -- run an external daemon "
                   "or the game will stall on its 60s auth timeout");
        return;
    }
    if (InterlockedCompareExchange(&g_re2_pcp_started, 1, 0) != 0) return;

    /* Its own thread: the listener must not be on the path that installs the
     * rest of the hooks, and the game's main thread is the one that will be
     * blocking on us. */
    h = CreateThread(NULL, 0, re2_pcp_thread, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
        ul_re2_log("[pcp] local Sega daemon stand-in armed (prompt '>' on accept; "
                   "40100 amMaster, 40190/40192 amAuth)");
    } else {
        ul_re2_log("[pcp] CreateThread failed err=%lu", (unsigned long)GetLastError());
    }
}

#endif /* _WIN32 */
