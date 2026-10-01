/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  nesicareader_stub.c -- stand-in for NESiCAReader.dll
 * =============================================================================
 *
 *  THE ERROR THIS FIXES
 *
 *      ERROR / 係員を呼んでください
 *      NESiCAリーダーエラー(13003)
 *
 *  13003 is not a guess. It is in the game's own error table, read out of
 *  Assembly-CSharp's metadata:
 *
 *      Siva.ErrorChecker:
 *          COIN_ERROR                    = 11001
 *          FAST_IO_ERROR                 = 12001
 *          NESICA_READER_ERROR           = 13001
 *          NESICA_READER_ERROR_NODE_OVER = 13002
 *          NESICA_READER_ERROR_CONNECT   = 13003   <-- this one
 *          NESICA_READER_ERROR_SEND_OVER = 13004
 *          TOUCH_MONITOR_ERROR           = 15001
 *          NESYS_ERROR                   = 21001
 *
 *  and those four reader codes line up one-for-one with the XIO_STATUS enum in
 *  the same assembly:
 *
 *          XioStatErrorSendOver = -4      -> 13004
 *          XioStatErrorConnect  = -3      -> 13003
 *          XioStatErrorNodeOver = -2      -> 13002
 *          XioStatError         = -1      -> 13001
 *          XioStatInit          =  0
 *          XioStatIdle          =  1
 *          XioStatSend          =  2
 *          XioStatRecv          =  3
 *
 *  So the game raises 13003 precisely when NESiCAReaderGetXioStatus() returns
 *  -3, CONNECT. The real NESiCAReader.dll talks to a card reader on COM2 (the
 *  string is in the DLL), there is no reader on this machine, so it reports a
 *  connect failure and the cabinet stops with an operator error.
 *
 *  WHY A REPLACEMENT DLL RATHER THAN EMULATING THE READER
 *
 *  Emulating the NESiCA reader means implementing its serial protocol on a
 *  virtual COM2. That is a large amount of work to arrive at "no card is
 *  inserted", which is what a cabinet with an idle reader reports anyway. The
 *  honest shape of the answer is: the reader hardware is healthy, and nobody
 *  has touched a card to it. That is what this returns.
 *
 *  It is the same approach as this board's iDmacDrv64 stand-in, and it is why
 *  ALL values here are env-overridable: the next title on this board may want
 *  different ones, and finding that out should cost a relaunch, not a rebuild.
 *
 *      ULTRA_NESICA_XIO=<n>     value GetXioStatus returns   (default 1, IDLE)
 *      ULTRA_NESICA_STATUS=<n>  value GetStatus returns      (default 0)
 *      ULTRA_NESICA_OPEN=<n>    value Open returns           (default 0, OK)
 *      ULTRA_NESICA_LOG=<path>  log every call
 *      ULTRA_NESICA=0           behave like the real DLL is absent (all errors),
 *                               to confirm this stub is really the one in use
 *
 *  CARD READING. Read/GetResult/GetID report "no card" forever. Card support
 *  would need NESYS, which is a dead online service - the cabinet was taken
 *  offline on 2021-04-01 - so there is nothing to authenticate against. FREE
 *  PLAY with no card is the working configuration.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* XIO_STATUS, from the game's own metadata. Negative is an error. */
#define XIO_ERR_SEND_OVER  (-4)
#define XIO_ERR_CONNECT    (-3)
#define XIO_ERR_NODE_OVER  (-2)
#define XIO_ERR            (-1)
#define XIO_INIT             0
#define XIO_IDLE             1
#define XIO_SEND             2
#define XIO_RECV             3

static char g_log[MAX_PATH];
static int  g_ready;
static CRITICAL_SECTION g_cs;

static void lg(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    if (!g_log[0]) return;
    if (g_ready) EnterCriticalSection(&g_cs);
    f = fopen(g_log, "a");
    if (f) {
        fputs("[NESiCAReader] ", f);
        va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc(10, f);
        fclose(f);
    }
    if (g_ready) LeaveCriticalSection(&g_cs);
}

static int envint(const char *name, int dflt)
{
    char v[32];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    if (!n || n >= sizeof v || !v[0]) return dflt;
    return (int)strtol(v, NULL, 0);
}

static int disabled(void)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA("ULTRA_NESICA", v, sizeof v);
    return (n && n < sizeof v && v[0] == '0');
}

/* Log the first call of each kind only. The game polls these every frame, and
 * an unbounded log turns the interesting first line into a needle. */
#define ONCE(tag) do { static int _s; if (!_s) { _s = 1; lg tag; } } while (0)

__declspec(dllexport) int NESiCAReaderOpen(void)
{
    int v = disabled() ? -1 : envint("ULTRA_NESICA_OPEN", 0);
    ONCE(("Open -> %d%s", v, v == 0 ? " (reader present)" : " (FORCED FAILURE)"));
    return v;
}

__declspec(dllexport) int NESiCAReaderClose(void)
{
    ONCE(("Close"));
    return 0;
}

__declspec(dllexport) int NESiCAReaderUpdate(void)
{
    ONCE(("Update (polled every frame; logged once)"));
    return 0;
}

/* THE ONE THAT MATTERS.
 *
 * The real DLL returns XioStatErrorConnect (-3) with no reader on COM2, and
 * Siva.ErrorChecker turns that into operator error 13003. Reporting an idle,
 * healthy reader is what a cabinet with nobody at it reports. */
__declspec(dllexport) int NESiCAReaderGetXioStatus(void)
{
    int v = disabled() ? XIO_ERR_CONNECT : envint("ULTRA_NESICA_XIO", XIO_IDLE);
    ONCE(("GetXioStatus -> %d%s", v,
          v < 0 ? "  <- NEGATIVE = the game will raise an operator error"
                : "  (healthy/idle)"));
    return v;
}

__declspec(dllexport) int NESiCAReaderGetStatus(void)
{
    int v = envint("ULTRA_NESICA_STATUS", 0);
    ONCE(("GetStatus -> %d", v));
    return v;
}

/* ---- card traffic: there is never a card ---------------------------------
 * NESYS, the service a card would authenticate against, was shut down on
 * 2021-04-01. "No card present" is both the honest answer and the working
 * one: the game falls through to FREE PLAY. */
__declspec(dllexport) int NESiCAReaderRead(void)
{
    ONCE(("Read -> 0 (no card)"));
    return 0;
}

__declspec(dllexport) int NESiCAReaderCancelRead(void)
{
    ONCE(("CancelRead"));
    return 0;
}

__declspec(dllexport) int NESiCAReaderGetResult(void)
{
    ONCE(("GetResult -> 0 (no card)"));
    return 0;
}

/* Takes an OUT buffer in the real ABI. We do not know its size, so we write
 * NOTHING and report failure instead of guessing a length - writing a
 * guessed number of bytes through a caller's pointer is how the Silent Hill
 * stub smashed a return address on another board in this project. Returning
 * non-zero means "no id available", which is true. */
__declspec(dllexport) int NESiCAReaderGetID(void *out, int len)
{
    (void)out; (void)len;
    ONCE(("GetID -> 1 (no card; buffer deliberately not written)"));
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        g_ready = 1;
        if (!GetEnvironmentVariableA("ULTRA_NESICA_LOG", g_log, sizeof g_log) || !g_log[0])
            if (!GetEnvironmentVariableA("ULTRA_JVS_LOG", g_log, sizeof g_log) || !g_log[0]) {
                DWORD n = GetTempPathA(sizeof g_log - 24, g_log);
                if (n && n < sizeof g_log - 24) strcat(g_log, "ultra_nesica.log");
                else g_log[0] = 0;
            }
        lg("==== loaded (Ultra Loader stand-in; reports an idle reader, no card) ====");
    }
    return TRUE;
}

#endif /* _WIN32 */
