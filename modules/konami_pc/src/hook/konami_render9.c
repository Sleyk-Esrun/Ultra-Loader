/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  konami_render9.c -- render-output window control for the Konami-PC-only hook DLL
 * =============================================================================
 *
 *  SCOPE, AND WHY IT IS NARROWER THAN OTHER BOARDS' D3D9 PRESENT HOOKS:
 *
 *  Every other board's own hook DLL this project has built (cave_pc/src/hook/
 *  cave_render9.c, sega_europar's equivalent, etc.) hooks IDirect3DDevice9's
 *  own vtable (Present/Reset) directly, which requires obtaining the live
 *  device pointer -- normally done by that board's OWN hook DLL intercepting
 *  Direct3DCreate9/CreateDevice itself, since nothing else in the process
 *  already owns that job.
 *
 *  Konami PC (this board) is different: shaiolib_crk.c -- this board's own
 *  protected, hard-won, already-shipping GHL-board reimplementation -- ALREADY
 *  owns the ENTIRE D3D9 device lifecycle out of sheer necessity (it bootstraps
 *  Direct3DCreate9/CreateDevice itself when the game's own gated state machine
 *  never calls it, and hooks BeginScene/EndScene/Present on the device it
 *  creates). Getting this board's own independent hook DLL a safe way to reach
 *  that SAME device was investigated in depth and rejected on real risk
 *  grounds, per this board's own explicit standing instruction to leave
 *  shaiolib_crk.c / libbms_crk.c / libutil_crk.c / shaerror_crk.c /
 *  dinput8_crk.c completely unchanged:
 *
 *    - Exporting a tiny read-only accessor from shaiolib_crk.c so this DLL
 *      could ask it for the device pointer would have been the cleanest fix,
 *      but that IS a change to a protected file, however small -- ruled out.
 *    - Independently re-hooking Direct3DCreate9 (KSHG.exe's own IAT slot for
 *      it) from this DLL was the other option, but shaiolib_crk.c's own
 *      Direct3DCreate9 hook (shg_iat_hook("d3d9.dll","Direct3DCreate9",...))
 *      installs by scanning KSHG.exe's IAT for a slot whose CURRENT value
 *      equals the TRUE native GetProcAddress() result -- if this DLL's own
 *      hook happened to install FIRST (a real possibility: this DLL is
 *      injected via QueueUserAPC, which on a CREATE_SUSPENDED process can fire
 *      before or interleaved with the game's own dynamic-load sequence that
 *      pulls in shaiolib.dll -- the exact ordering is not something this pass
 *      could pin down without a live debugger), that scan would find NO match
 *      (the slot no longer holds the native address) and shaiolib_crk.c's own
 *      hook would silently fail to install. s_real_D3DC9 would stay NULL,
 *      which disables shg_bootstrap_d3d9() -- the 2026-07-31 fix that is this
 *      game's ONLY currently-known path to ever reach Direct3DCreate9 at all
 *      (see docs/STOCK_TAKE.md, konami_pc row). Silently regressing the one
 *      thing standing between this game and a black screen, in exchange for a
 *      cosmetic upscale/rotate feature, is not an acceptable trade -- ruled
 *      out.
 *
 *  So this file does NOT touch Direct3DCreate9, does NOT hold a device
 *  pointer, and does NOT hook any D3D9 vtable at all. What it DOES safely,
 *  entirely independent of shaiolib_crk.c and of the device: manage the
 *  game's OS-level window (found via the same generic EnumWindows technique
 *  used all over this project, not by calling into shaiolib_crk.c), and rely
 *  on a standard, well-documented Direct3D9 behavior instead of a hook:
 *  Present(dev, NULL, NULL, NULL, NULL) with a NULL source/dest rect performs
 *  an automatic stretch-blit from the backbuffer to the CURRENT client area of
 *  the device window. shaiolib_crk.c's own render thread already calls
 *  Present exactly this way (`s_prs_fwd(s_d3d9_dev_fwd, NULL, NULL, NULL,
 *  NULL)`), so resizing the OS window this file finds is enough to make an
 *  opt-in upscale happen for free, with zero device access and zero hook
 *  install-order risk.
 *
 *  Consequently there is no rotate, no sharpen, and no true per-frame
 *  QueryPerformanceCounter-paced frame limiter in this file -- all three would
 *  need a real Present/EndScene hook, which needs the device, which needs one
 *  of the two ruled-out options above. Frame pacing for this game is already
 *  handled entirely inside shaiolib_crk.c's own render thread (a hardcoded
 *  ~62.5fps Sleep(16) after each render call); that is unaffected either way.
 *  This is a real, deliberate scope reduction versus this project's other
 *  boards, specific to this one board's unusually fragile, already-owned D3D9
 *  lifecycle -- not an oversight.
 *
 *  Per this project's 2026-08-01 taito_typex/typex2 lesson: the window resize
 *  (upscale) is OFF unless the user explicitly sets ULTRA_RES / ULTRA_RES_W+H
 *  / ULTRA_BORDERLESS. Nothing in konami_pc_main.c sets any of these, so by
 *  default this file only does the conservative, always-safe part: keep the
 *  game window un-minimized, foreground, and sized to what shaiolib_crk.c
 *  itself already requested (defensive; shaiolib_crk.c's own rgate_keeper
 *  thread already does most of this -- this is a second, independent,
 *  best-effort layer, not a replacement for it).
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API. No access
 *  to any Direct3D interface pointer, and no dependency, compiled or source,
 *  on shaiolib_crk.c or any other file in modules/konami_pc/src/.
 *
 *  2026-08-02 UPDATE: shg_bootstrap_d3d9() and its forced-render machinery
 *  are now opt-in in shaiolib_crk.c (ULTRA_KONAMI_FORCE_RENDER=1 -- see that
 *  file's header near shg_force_render_enabled()), default OFF, after fresh
 *  headless testing reproduced a near-instant crash in that layer. shaiolib_
 *  crk.c's PASSIVE Direct3DCreate9 IAT hook (shg_Direct3DCreate9 -- forces
 *  windowed mode, logs, then calls through) stays installed UNCONDITIONALLY
 *  either way, so the install-order race reasoning above still applies exactly
 *  as written: this file must still never install a second, competing
 *  Direct3DCreate9 hook of its own. Nothing below changes.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "konami_hook.h"

#define tlog konami_log

/* ---- independent game-window discovery --------------------------------------
 * Deliberately reimplemented here rather than shared with shaiolib_crk.c (no
 * source or compiled dependency on that file -- see file header). Same
 * generic technique: prefer an exact title match ("KSHG"), fall back to any
 * visible top-level window owned by our own process that isn't the loader's
 * splash screen. */
static HWND g_wnd = NULL;

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    (void)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    char cls[64] = {0};
    GetClassNameA(h, cls, sizeof cls);
    if (!strcmp(cls, "UltraLoaderSplash")) return TRUE;   /* never grab the loader's own splash */
    RECT r = {0};
    GetWindowRect(h, &r);
    if (IsIconic(h) || (r.right - r.left) > 64) { g_wnd = h; return FALSE; }
    return TRUE;
}

static HWND find_game_window(void) {
    HWND kshg = FindWindowA(NULL, "KSHG");
    if (kshg && IsWindow(kshg)) return kshg;
    if (g_wnd && IsWindow(g_wnd)) return g_wnd;
    g_wnd = NULL;
    EnumWindows(enum_cb, 0);
    return g_wnd;
}

/* ---- opt-in resize target ------------------------------------------------- */
static int g_up_w = 0, g_up_h = 0;
static int g_res_read = 0;
static int g_resized  = 0;

static void read_target_res(void) {
    if (g_res_read) return;
    g_res_read = 1;
    const char* r = getenv("ULTRA_RES");
    if (r && *r) {
        if      (!strcmp(r, "1080p")) { g_up_w = 1920; g_up_h = 1080; }
        else if (!strcmp(r, "1440p")) { g_up_w = 2560; g_up_h = 1440; }
        else if (!strcmp(r, "4k") || !strcmp(r, "2160p")) { g_up_w = 3840; g_up_h = 2160; }
        else { int w=0,h=0; if (sscanf(r, "%dx%d", &w, &h) == 2 && w>0 && h>0) { g_up_w=w; g_up_h=h; } }
    }
    const char* w = getenv("ULTRA_RES_W"); const char* h = getenv("ULTRA_RES_H");
    if (w && *w && h && *h) { int wi=atoi(w), hi=atoi(h); if (wi>0&&hi>0){ g_up_w=wi; g_up_h=hi; } }
    if (g_up_w == 0) {
        const char* bl = getenv("ULTRA_BORDERLESS");
        if (bl && *bl == '1') {
            g_up_w = GetSystemMetrics(SM_CXSCREEN);
            g_up_h = GetSystemMetrics(SM_CYSCREEN);
        }
    }
    if (g_up_w > 0)
        tlog("render9: opt-in window-resize upscale armed -> %dx%d (relies on D3D9's own "
             "NULL-rect Present auto-stretch; no device hook involved)", g_up_w, g_up_h);
}

static int windowed_requested(void) {
    /* konami_pc_main.c already defaults ULTRA_SEGA_WINDOWED=1 if the user
     * hasn't set it; also honor the universal ULTRA_WINDOWED/ULTRA_BORDERLESS
     * knobs other boards use. Never forces anything if explicitly set to 0. */
    const char* v = getenv("ULTRA_SEGA_WINDOWED");
    if (v && *v) return v[0] == '1';
    v = getenv("ULTRA_WINDOWED");
    if (v && *v) return v[0] == '1';
    v = getenv("ULTRA_BORDERLESS");
    if (v && *v) return v[0] == '1';
    return 1;   /* shaiolib_crk.c's own device is always windowed anyway */
}

/* Defensive re-assert only: shaiolib_crk.c's own rgate_keeper thread already
 * un-minimizes + repositions the window. This is a second, independent,
 * best-effort pass -- harmless if it races (same idempotent SetWindowPos
 * calls), valuable if rgate_keeper hasn't found the window yet or the window
 * reverted after this pass ran. Only actually moves anything when a resize is
 * armed (g_up_w>0) or the window is currently iconic -- otherwise a no-op. */
static void apply_window_state(HWND w) {
    if (!w) return;
    if (IsIconic(w)) {
        ShowWindow(w, SW_RESTORE);
        tlog("render9: window was iconic, restored");
    }
    read_target_res();
    if (g_up_w > 0 && !g_resized && windowed_requested()) {
        RECT wr = { 0, 0, g_up_w, g_up_h };
        SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        AdjustWindowRect(&wr, WS_POPUP, FALSE);
        SetWindowPos(w, HWND_TOPMOST, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(w);
        InvalidateRect(w, NULL, TRUE);
        g_resized = 1;
        tlog("render9: window resized to %dx%d -- shaiolib_crk.c's own Present(NULL,NULL,...) "
             "call will auto-stretch the game's backbuffer to fill it", g_up_w, g_up_h);
    }
}

/* ---- install / poll ------------------------------------------------------- */
static int g_installed = 0;

void konami_render9_install(void) {
    /* konami_pc_main.c sets ULTRA_HOOK_D3D9=1 by default; a user/session can
     * set it to 0 to fully disable this file's window management as a safety
     * valve without rebuilding anything. */
    const char* g = getenv("ULTRA_HOOK_D3D9");
    if (g && g[0] == '0') return;

    /* Idempotent by design (matches every other *_install() in this DLL) --
     * cheap enough to call from a periodic worker thread that also retries
     * until the game has actually created its window. */
    HWND w = find_game_window();
    if (w) {
        g_wnd = w;
        if (!g_installed) {
            g_installed = 1;
            tlog("render9: found game window %p (device-independent; see file header for why "
                 "this hook does not touch D3D9 directly)", (void*)w);
        }
        apply_window_state(w);
    }
}

void konami_render9_teardown(void) {
    /* Nothing to release -- no D3D9 resources, no timer period armed (this
     * file installs no per-frame timing). */
}

HWND konami_render9_get_window(void) {
    if (g_wnd && IsWindow(g_wnd)) return g_wnd;
    return find_game_window();
}

#endif /* _WIN32 */
