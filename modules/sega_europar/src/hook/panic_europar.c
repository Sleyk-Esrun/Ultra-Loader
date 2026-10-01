/* SPDX-License-Identifier: MIT
 *
 *  panic_europar.c -- an escape hatch that survives a frozen game
 *
 *  WHY THIS EXISTS. 2026-09-20: Ford Racing's operator shell (GameShell.exe) was
 *  launched for the first time and hung the instant it opened our synthetic HAPP
 *  device. It had already put a window up, so the owner's DESKTOP went with it --
 *  taskbar included -- and there was no way out of it from the keyboard. The
 *  processes had to be killed from outside and explorer restarted.
 *
 *  That must never happen again, and the ordinary routes are not enough:
 *
 *    - The Ultra overlay (Start+Select) is pumped from the GAME's message loop.
 *      A game frozen inside a blocking ReadFile never pumps anything, so the
 *      overlay cannot appear. It was registered in that very run and was useless.
 *    - RegisterHotKey delivers WM_HOTKEY to a message queue. Same problem.
 *
 *  So this runs on ITS OWN THREAD and polls GetAsyncKeyState, which reads the
 *  async key state directly and does not need a message pump anywhere. It keeps
 *  working while every other thread in the process is wedged.
 *
 *  TWO GUARDS:
 *
 *    1. PANIC KEY -- Ctrl+Alt+Q, held briefly. Calls TerminateProcess on our own
 *       process. Deliberately TerminateProcess and not ExitProcess: ExitProcess
 *       runs DLL_PROCESS_DETACH and waits on the loader lock, which a wedged
 *       thread may be holding -- exactly the case we are escaping from.
 *
 *    2. BOOT WATCHDOG -- optional. If the process has not reported progress
 *       within N seconds of start, kill it. A game that is running normally calls
 *       ultra_panic_alive() from its present/poll path, so a title that simply
 *       loads slowly is not punished; only one that never gets anywhere is.
 *
 *  Both are OFF unless a container asks for them, so nothing here reaches a
 *  working title unless it opts in.
 *
 *    ULTRA_PANIC_KEY      1 = arm Ctrl+Alt+Q   (default 0)
 *    ULTRA_PANIC_TIMEOUT  seconds with no progress before self-kill; 0 = never
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG  g_alive_tick = 0;
static volatile LONG  g_panic_stop = 0;
static          DWORD g_start_ms   = 0;
static          int   g_timeout_s  = 0;

/* Called from a normal per-frame path to say "still making progress". */
void ultra_panic_alive(void) { InterlockedIncrement(&g_alive_tick); }

static void panic_log(const char *msg) {
    FILE *f = fopen("ultra_panic.log", "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

static DWORD WINAPI panic_thread(LPVOID unused) {
    LONG  last_tick = -1;
    DWORD last_move = GetTickCount();
    int   held_ms   = 0;
    (void)unused;

    for (;;) {
        if (InterlockedCompareExchange(&g_panic_stop, 0, 0)) return 0;
        Sleep(50);

        /* ---- 1. the panic key ---- */
        {
            int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            int alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
            int q    = (GetAsyncKeyState('Q')        & 0x8000) != 0;
            if (ctrl && alt && q) {
                held_ms += 50;
                /* a short hold, so a stray chord cannot kill a working game */
                if (held_ms >= 300) {
                    panic_log("PANIC KEY (Ctrl+Alt+Q) -- terminating this process");
                    TerminateProcess(GetCurrentProcess(), 3);
                    return 0;
                }
            } else {
                held_ms = 0;
            }
        }

        /* ---- 2. the boot watchdog ---- */
        if (g_timeout_s > 0) {
            LONG  now_tick = InterlockedCompareExchange(&g_alive_tick, 0, 0);
            DWORD now_ms   = GetTickCount();
            if (now_tick != last_tick) {          /* progress -- reset the clock */
                last_tick = now_tick;
                last_move = now_ms;
            } else if ((int)((now_ms - last_move) / 1000) >= g_timeout_s &&
                       (int)((now_ms - g_start_ms) / 1000) >= g_timeout_s) {
                char m[160];
                snprintf(m, sizeof m,
                         "WATCHDOG: no progress for %d s -- terminating so the desktop is not held",
                         g_timeout_s);
                panic_log(m);
                TerminateProcess(GetCurrentProcess(), 4);
                return 0;
            }
        }
    }
}

void ultra_panic_install(void) {
    char k[16] = {0}, t[16] = {0};
    int  armed = 0;
    HANDLE th;

    GetEnvironmentVariableA("ULTRA_PANIC_KEY", k, sizeof k);
    GetEnvironmentVariableA("ULTRA_PANIC_TIMEOUT", t, sizeof t);
    if (k[0] && atoi(k) != 0) armed = 1;
    if (t[0]) {
        g_timeout_s = atoi(t);
        if (g_timeout_s < 0)    g_timeout_s = 0;
        if (g_timeout_s > 3600) g_timeout_s = 3600;
        if (g_timeout_s > 0) armed = 1;
    }
    if (!armed) return;

    g_start_ms = GetTickCount();
    th = CreateThread(NULL, 0, panic_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}
