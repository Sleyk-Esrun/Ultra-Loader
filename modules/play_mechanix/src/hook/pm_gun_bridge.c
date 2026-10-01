/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  pm_gun_bridge.c -- Play Mechanix lightgun aim/trigger + standalone input
 * =============================================================================
 *
 *  This board's whole reason for being is a lightgun cabinet, so this file
 *  owns the one input source that matters: the aim position fed to
 *  jvs_core_pm's screen_x/screen_y (answered on JVS_CMD_READ_SCRPOS), the
 *  trigger bit, and TEST/SERVICE/START/COIN so a standalone run (no loader
 *  attached) is still playable end-to-end.
 *
 *  Aim priority:
 *   1. A live Ultra Omni-Track pose from the loader's shared block
 *      (ultra_io_shared_pm::screen_x/screen_y + the trigger bit in player[0]),
 *      read with the same seqlock-snapshot pattern every other Ultra Loader
 *      lightgun bridge uses (modules/native_x86/src/hook/input_src_gun.c /
 *      typex2_gun.c, read only as reference -- this is this board's own,
 *      independent re-expression of that pattern against its own IPC struct).
 *   2. Falls back to the OS mouse cursor mapped into the D3D9 device window's
 *      client rect (d3d9_throttle_pm.c hands us that window the moment
 *      CreateDevice fires), with the left mouse button as trigger -- so a
 *      standalone test run (or a real cabinet with no loader driving it) is
 *      still aimable without a physical gun.
 *
 *  Digital controls (TEST/SERVICE/START/COIN) read directly via
 *  GetAsyncKeyState when no loader shared block is live -- simple edge-
 *  detected key taps, no gamepad/analog needed for a lightgun-only cabinet.
 *  Keys are overridable via env vars so a real cabinet's mapping can differ
 *  from the defaults without a rebuild.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API + this
 *  board's own IPC contract (ultra_io_shared_pm.h).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_pm.h"
#include "ultra_io_shared_pm.h"
#include "pm_hook.h"

static volatile LONG g_stop = 0;
/* Written once by the D3D9 CreateDevice hook (render thread), read on the gun
 * poll thread -- volatile so the reader sees the store without a barrier race. */
static HWND volatile g_devwnd = NULL;

void pm_gun_bridge_set_window(HWND hwnd) { g_devwnd = hwnd; }

/* Coherent seqlock snapshot of the loader-mapped gun pose (mirrors the
 * reference input_src_gun.c / typex2_gun.c pattern, re-expressed against this
 * board's own ultra_io_shared_pm struct). Returns 1 if a live, coherent pose
 * was captured (screen_x/y already 0..65535 calibrated by the loader). */
static int shared_gun_snapshot(int* out_x, int* out_y, int* out_trig) {
    ultra_io_shared_pm* s = (ultra_io_shared_pm*)pm_hook_shared();
    if (!s || s->magic != ULTRA_IO_MAGIC) return 0;
    if (!(s->flags & ULTRA_IO_FLAG_LOADER_ALIVE)) return 0;
    uint16_t sx = 0x8000, sy = 0x8000, p0 = 0; int got = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = s->seq; MemoryBarrier();
        sx = s->screen_x; sy = s->screen_y; p0 = s->player[0];
        MemoryBarrier();
        uint32_t s2 = s->seq;
        if (s1 == s2 && !(s1 & 1u)) { got = 1; break; }
    }
    if (!got) return 0;
    *out_x = sx; *out_y = sy; *out_trig = (p0 & 0x0200) != 0;   /* PUSH1/trigger bit */
    return 1;
}

/* Ultra Omni-Bridge pose adapter -- the lightgun pose source handed to
 * ultra_omni_bridge_install()'s 2nd argument (hook_dllmain_pm.c). Its
 * signature int(*)(int*,int*,int*) IS ultra_bridge_pose_fn, so this is a thin
 * non-static wrapper over the snapshot above; no second copy of the seqlock /
 * validation logic exists or should.
 *
 * WHY A DEAD LOADER MUST MEAN "NO POSE", NOT "LAST KNOWN POSE":
 * shared_gun_snapshot() returns 0 unless ULTRA_IO_FLAG_LOADER_ALIVE is set.
 * The shared block is a file mapping that outlives the loader process, so its
 * bytes keep reading back the final pose forever after the loader exits (or
 * after Omni-Track goes inactive). If we returned that stale pose, Omni-
 * Bridge's gun_thread would keep warping the OS cursor to a frozen aim point
 * ~120x/sec -- fighting a real mouse -- and, if the trigger bit happened to be
 * set in that last write, would hold the left button DOWN indefinitely
 * (auto-fire nobody asked for). Returning 0 instead makes the bridge fall
 * through to its gamepad path / go inert, and lets pm_gun_bridge.c's own
 * gun_thread fall back to the real OS cursor. "No live loader" is a fallback
 * condition, never a frozen aim.
 *
 * These two consumers cannot fight over the cursor: Omni-Bridge's gun_thread
 * is the only SetCursorPos writer and only acts when this returns 1;
 * mouse_gun_snapshot() below is the only GetCursorPos reader and runs only
 * when this returns 0 (see gun_thread's fallback chain). Mutually exclusive
 * by construction. */
int play_mechanix_gun_pose(int* out_x, int* out_y, int* out_trig) {
    if (!out_x || !out_y || !out_trig) return 0;
    return shared_gun_snapshot(out_x, out_y, out_trig);
}

/* Mouse-cursor fallback: map the OS cursor into the device window's client
 * rect and normalize to the same 0..65535 JVS convention. */
static int mouse_gun_snapshot(int* out_x, int* out_y, int* out_trig) {
    HWND w = g_devwnd;
    if (!w) return 0;
    RECT rc; if (!GetClientRect(w, &rc)) return 0;
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return 0;
    POINT pt; if (!GetCursorPos(&pt)) return 0;
    ScreenToClient(w, &pt);
    if (pt.x < 0) pt.x = 0;
    if (pt.x >= cw) pt.x = cw - 1;
    if (pt.y < 0) pt.y = 0;
    if (pt.y >= ch) pt.y = ch - 1;
    *out_x = (int)((long)pt.x * 65535 / (cw > 1 ? cw - 1 : 1));
    *out_y = (int)((long)pt.y * 65535 / (ch > 1 ? ch - 1 : 1));
    *out_trig = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    return 1;
}

/* ---- standalone digital controls (TEST/SERVICE/START/COIN) ---------------- *
 * Overridable via env vars; sane defaults for a single-gun cabinet. Edge-
 * detected so a held key doesn't spam coin inserts. */
static WORD env_vk(const char* name, WORD dflt) {
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    return (WORD)strtol(e, NULL, 0);
}

typedef struct { WORD vk; int prev; } edge_key;
static edge_key g_key_start   = { 0 };
static edge_key g_key_coin    = { 0 };
static edge_key g_key_test    = { 0 };
static edge_key g_key_service = { 0 };
static volatile LONG g_coin_total = 0;   /* cumulative coin-key taps (edge count) */

static int key_edge(edge_key* k) {
    int down = (GetAsyncKeyState(k->vk) & 0x8000) != 0;
    int edge = down && !k->prev;
    k->prev = down;
    return edge;
}

/* Current merged input snapshot, written by the poll thread and read by the
 * hook's pull-input path (single-writer, POD-copy read -- same pattern the
 * reference hook's standalone gamepad fallback uses for g_xinput_in). */
static ultra_pm_jvs_input g_input;

void pm_gun_bridge_current(ultra_pm_jvs_input* out) {
    if (out) *out = g_input;
}
/* Cumulative coin-key taps; the hook diffs this against its own "seen"
 * counter and calls jvs_pm_insert_coin for the delta (edge-triggered, never
 * lost or doubled even if pull_input is called at an irregular rate). */
long pm_gun_bridge_coin_total(void) { return g_coin_total; }

static DWORD WINAPI gun_thread(LPVOID arg) {
    (void)arg;
    g_key_start.vk   = env_vk("ULTRA_PM_START_VK",   0x31); /* '1' */
    g_key_coin.vk    = env_vk("ULTRA_PM_COIN_VK",    0x35); /* '5' */
    g_key_test.vk    = env_vk("ULTRA_PM_TEST_VK",    VK_F2);
    g_key_service.vk = env_vk("ULTRA_PM_SERVICE_VK", 0x39); /* '9' */

    for (int a = 0; a < JVS_MAX_ANALOG; ++a) g_input.analog[a] = 0x8000;
    g_input.screen_x = g_input.screen_y = 0x8000;

    pm_log("[play_mechanix] gun bridge: armed (shared pose preferred, mouse fallback; "
           "start=0x%X coin=0x%X test=0x%X service=0x%X)",
           g_key_start.vk, g_key_coin.vk, g_key_test.vk, g_key_service.vk);

    while (!g_stop) {
        int x = 0x8000, y = 0x8000, trig = 0;
        if (!shared_gun_snapshot(&x, &y, &trig))
            if (!mouse_gun_snapshot(&x, &y, &trig)) { x = 0x8000; y = 0x8000; trig = 0; }

        uint16_t p0 = 0;
        if (trig)                                          p0 |= 0x0200;   /* PUSH1 = trigger */
        if (GetAsyncKeyState(g_key_start.vk)   & 0x8000)   p0 |= 0x8000;   /* START (level, held is fine) */
        if (GetAsyncKeyState(g_key_service.vk) & 0x8000)   p0 |= 0x4000;   /* SERVICE (level) */
        if (key_edge(&g_key_coin)) InterlockedIncrement(&g_coin_total);   /* COIN: one pulse per press */

        uint8_t sys = 0;
        if (GetAsyncKeyState(g_key_test.vk) & 0x8000) sys |= 0x80;   /* TEST */

        g_input.system    = sys;
        g_input.player[0] = p0;
        g_input.screen_x  = (uint16_t)x;
        g_input.screen_y  = (uint16_t)y;

        Sleep(8);   /* ~120 Hz poll */
    }
    return 0;
}

static HANDLE g_thread = NULL;
void pm_gun_bridge_install(void) {
    if (g_thread) return;   /* idempotent */
    g_thread = CreateThread(NULL, 0, gun_thread, NULL, 0, NULL);
}
void pm_gun_bridge_stop(void) {
    InterlockedExchange(&g_stop, 1);
    if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
}

#endif /* _WIN32 */
