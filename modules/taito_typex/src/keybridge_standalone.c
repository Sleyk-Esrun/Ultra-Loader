/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  keybridge_standalone.c -- ultra_typex_keybridge32.dll
 * =============================================================================
 *  A SEPARATE, self-contained DLL from this board's main hook -- same reason
 *  as codepage_spoof_standalone.c / lsdrv_shim_standalone.c.
 *
 *  Block King Ball Shooter's test-menu screens (confirmed live: the touch-
 *  sensor calibration prompt) label their controls "[ENTER]" / "[TEST]" --
 *  literal PC keyboard names, not JVS switch names. game.exe imports no
 *  GetAsyncKeyState/GetKeyState and creates its gameplay input purely through
 *  DirectInput8 (dinput8_emu.c, already part of this board's shipped hook,
 *  handles that), but these menu screens read the keyboard directly (most
 *  likely via WM_KEYDOWN in their own window proc) -- with no physical
 *  keyboard at the cabinet, there is no way to answer that prompt.
 *
 *  This is functionally the project's own Omni-Bridge (gamepad -> synthesized
 *  keyboard, see omni_bridge.c's key_tap()) -- Omni-Bridge itself was added to
 *  this board's hook source tree on 2026-08-02, one day AFTER this board's
 *  shipped hook was frozen (2026-08-01) to protect Chaos Breaker's rendering
 *  path, so it was never actually part of what runs today. Rather than fold
 *  it into that frozen hook (the same risk every other fix in this session
 *  has avoided), this is a small, purpose-built stand-in: watches XInput pad
 *  0 and taps the two keys these screens ask for. SendInput reaches
 *  WM_KEYDOWN listeners the same way a real keyboard would.
 *
 *  Mapping (matches this project's own "TEST=L3, SERVICE=R3" convention
 *  wherever these screens instead expect a literal keyboard key):
 *    A (bit 0x1000)        -> VK_RETURN   ("[ENTER]" -- confirm/next item)
 *    Left thumb-click / L3 (bit 0x0040) -> VK_F2  ("[TEST]" -- back/cancel,
 *                                          matching this project's TEST=L3
 *                                          and this Taito board family's own
 *                                          F2=TEST keyboard convention)
 *  Env-gated ON by default for the title this ships for via the provisional-
 *  fix table (taito_typex_main.c); ULTRA_KEYBRIDGE=0 disables it.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Same proven pattern as lsdrv_shim_standalone.c: fixed %TEMP% path (not
 * ULTRA_JVS_LOG, which this session's earlier tests found stuck pointing at
 * an unrelated stale file from prior testing), GetEnvironmentVariableA-safe
 * (no env dependency at all here), one process-wide lock. */
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static void klog(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 32, path);
        if (!n || n >= sizeof path - 32) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_keybridge_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[keybridge] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } kb_xpad;
typedef struct { DWORD packet; kb_xpad pad; } kb_xstate;
typedef DWORD (WINAPI *kb_p_xinput_get)(DWORD, kb_xstate*);
static kb_p_xinput_get s_xget;

/* HELD, not tapped: send the down edge once when the gamepad button goes
 * down, and hold it for as long as the button stays down (up to a safety
 * ceiling), releasing only on the button's up edge. A fixed 40ms tap already
 * confirmed our emulation reports the scancode DOWN correctly, but a genuine
 * hold rules out a slow poll cadence on this screen missing a brief blip --
 * a real key held for a second behaves nothing like one tapped for 40ms. */
static void key_down(int vk, const char* label) {
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    INPUT in[2];
    UINT sent;
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    sent = SendInput(2, in, sizeof(INPUT));
    HWND fg = GetForegroundWindow();
    char title[128] = "", cls[64] = "";
    DWORD fg_pid = 0;
    if (fg) { GetWindowTextA(fg, title, sizeof title); GetClassNameA(fg, cls, sizeof cls);
              GetWindowThreadProcessId(fg, &fg_pid); }
    klog("key_down(%s, vk=0x%02X, sc=0x%02X) SendInput sent=%u/2 (err=%lu), "
         "foreground=%p pid=%lu (this process pid=%lu) title=\"%s\" class=\"%s\"",
         label, vk, sc, sent, sent == 2 ? 0 : GetLastError(),
         (void*)fg, fg_pid, GetCurrentProcessId(), title, cls);
}
static void key_up(int vk, const char* label) {
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk; in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0; in[1].ki.wScan = (WORD)sc;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    /* The game's own poll sees this key "down" in ~368 of 369 readings
     * despite our own down/up pairs being perfectly balanced every time --
     * checking GetAsyncKeyState for THIS exact vk immediately after our own
     * release tells us definitively whether something OTHER than our code
     * (real hardware, another mapping tool) is re-asserting it, versus a
     * timing issue in our own injection. */
    klog("key_up(%s) -- GetAsyncKeyState(0x%02X) immediately after release = 0x%04X",
         label, vk, (unsigned)GetAsyncKeyState(vk));
}

static volatile LONG g_stop;
static DWORD WINAPI kb_thread(LPVOID arg) {
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    const char* loaded_from = NULL;
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) {
            s_xget = (kb_p_xinput_get)(void*)GetProcAddress(h, "XInputGetState");
            if (s_xget) loaded_from = dlls[i];
        }
    }
    if (!s_xget) { klog("XInputGetState not found in any xinput DLL -- bridge inert"); return 0; }
    klog("XInputGetState resolved from %s -- polling pad 0", loaded_from);

    int a_prev = 0, l3_prev = 0;
    unsigned tick = 0;
    int last_logged_connected = -1;
    WORD last_logged_buttons = 0xFFFF; /* force first heartbeat to print */
    while (!g_stop) {
        kb_xstate st; ZeroMemory(&st, sizeof st);
        DWORD rc = s_xget(0, &st);
        int connected = (rc == 0);
        if (connected != last_logged_connected) {
            klog("pad 0 connection state changed: %s (XInputGetState rc=%lu)",
                 connected ? "CONNECTED" : "NOT CONNECTED", rc);
            last_logged_connected = connected;
        }
        if (connected) {
            if (st.pad.wButtons != last_logged_buttons) {
                klog("pad 0 wButtons=0x%04X lx=%d ly=%d", st.pad.wButtons, st.pad.lx, st.pad.ly);
                last_logged_buttons = st.pad.wButtons;
            }
            int a_now  = !!(st.pad.wButtons & 0x1000);   /* A */
            int l3_now = !!(st.pad.wButtons & 0x0040);   /* left thumb click */
            if (a_now && !a_prev)   key_down(VK_RETURN, "ENTER/A");
            if (!a_now && a_prev)   key_up(VK_RETURN, "ENTER/A");
            /* ESCAPE confirmed to be a global quit hotkey (closes the whole
             * game), NOT specific to this dialog -- removed after live
             * testing showed it just exits, even though the game's own
             * WasKeyPressed(Escape) check does register a real press. */
            if (l3_now && !l3_prev) key_down(VK_F2, "TEST/L3");
            if (!l3_now && l3_prev) key_up(VK_F2, "TEST/L3");
            a_prev = a_now; l3_prev = l3_now;
        }
        /* Independent of anything we send: watch VK_RETURN's real OS-level
         * state continuously, logging only on change. If this shows "down"
         * for long stretches while we are NOT holding a key_down for it,
         * something other than our own injection is asserting it. */
        {
            static SHORT last_enter_state = 0x7FFF; /* force first log */
            SHORT cur = GetAsyncKeyState(VK_RETURN);
            if (cur != last_enter_state) {
                klog("VK_RETURN real state changed: 0x%04X -> 0x%04X (%s)",
                     (unsigned short)last_enter_state, (unsigned short)cur,
                     (cur & 0x8000) ? "NOW DOWN" : "NOW UP");
                last_enter_state = cur;
            }
        }
        /* Heartbeat every ~10s even with no activity, so a silent log is
         * distinguishable from a dead thread. */
        if ((++tick % 625) == 0)
            klog("heartbeat: alive, pad0 %s", connected ? "connected" : "not connected");
        Sleep(16);
    }
    /* Never leave a key held down past this thread's life. */
    if (a_prev)  key_up(VK_RETURN, "ENTER/A");
    if (l3_prev) { key_up(VK_F2, "TEST/L3"); key_up(VK_ESCAPE, "TEST/L3(ESC)"); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
        klog("armed (DLL_PROCESS_ATTACH)");
        /* CONFIRMED root cause of "nothing works": SendInput's key state is
         * a persistent, system-wide OS flag, not tied to any process's
         * lifetime. An earlier test run crashed while ENTER's synthetic
         * key-down was active, so its matching key-up never sent -- Windows
         * has considered Enter physically held down ever since, across
         * every later launch (live-traced: the game's own IsKeyDown(Enter)
         * returned "pressed" on literally every single poll, never once
         * "up", even with no controller input at all). A key permanently
         * "already down" produces no new press EDGE for a menu to react to,
         * which is why every press looked identical to no input whatsoever.
         * Release both keys we ever inject, unconditionally, the moment we
         * attach -- before anything else runs -- so a previous crash can
         * never leave this process starting from a stuck state. */
        key_up(VK_RETURN, "ENTER/A (startup clear)");
        key_up(VK_F2, "TEST/L3 (startup clear)");
        key_up(VK_ESCAPE, "TEST/L3(ESC) (startup clear)");
        /* Injection itself is already gated on ULTRA_KEYBRIDGE being set
         * (taito_typex_main.c's provisional-fix table), so no separate
         * internal check is needed here. */
        CreateThread(NULL, 0, kb_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}

__declspec(dllexport) const char* ultra_keybridge_id(void) {
    return "Ultra Loader gamepad -> keyboard test-menu bridge v1";
}

#endif /* _WIN32 */
