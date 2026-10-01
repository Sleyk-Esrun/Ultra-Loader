/*
 * shaiolib_crk.c -- stub shaiolib.dll for Konami PC arcade (Silent Hill, etc.)
 *
 * Replaces the real hardware shaiolib.dll with one that reads from
 * keyboard + mouse.  konami_io_swap_in() drops this file as shaiolib.dll
 * in the game directory before launch.
 *
 * KEY MAP (standard arcade layout):
 *   5       = Coin slot 1 (P1 coin)
 *   6       = Coin slot 2 (P2 coin)
 *   1       = P1 Start
 *   2       = P2 Start
 *   9       = Service
 *   F2      = Test
 *   LMB     = Trigger / fire
 *   RMB     = Reload / off-screen shot
 *   Mouse   = Gun aim
 *
 * SECURITY: all SECURE_* calls return 0 (OK / no error).
 * SYSINFO:  returns GHL identifiers matching what the game expects.
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <setjmp.h>
#include <malloc.h>  /* _resetstkoflw() -- re-arm the stack guard page after a
                      * caught STATUS_STACK_OVERFLOW (see shg_veh). */

/* ── forward declarations (resolve ordering between rgate_keeper and render thread) ── */
static volatile DWORD  s_render_tid         = 0;
static void           *s_d3d9_dev_fwd       = NULL;
static volatile LONG   s_render_thread_started = 0;
static DWORD WINAPI    shg_direct_render_thread(LPVOID p);
static volatile LONG   s_render_jmp_armed   = 0;
static jmp_buf         s_render_jmp;
static void            shg_bootstrap_d3d9(void);  /* 2026-07-31: D3D9 bootstrap */
static void            shg_xhair_start(void);    /* crosshair overlay thread */
static void            shg_apply_internal_res(void); /* KSHG internal-res patches */
static void            shg_snd_probe_tick(void);     /* DirectSound Play/Stop instrument */

/* ── env-gated debug log (ULTRA_SHAIOLIB_LOG=1 or =<path>) ──────────────── */
static FILE*  s_logf     = NULL;
static int    s_log_init = 0;
static DWORD  s_log_t0   = 0;
static DWORD  s_btn_last  = 0;
static DWORD  s_10key_last= 0;
static DWORD  s_gun_last  = 0;
#define SLOG_RATE_MS 333

static FILE* shg_logf(void) {
    if (s_log_init) return s_logf;
    s_log_init = 1;
    const char* lp = getenv("ULTRA_SHAIOLIB_LOG");
    if (!lp || !lp[0]) return NULL;
    char path[MAX_PATH];
    if (lp[0] == '1' && lp[1] == '\0') {
        GetTempPathA(MAX_PATH, path);
        strncat(path, "shaiolib_debug.log", MAX_PATH - strlen(path) - 1);
    } else {
        strncpy(path, lp, MAX_PATH - 1); path[MAX_PATH - 1] = '\0';
    }
    s_logf = fopen(path, "w");
    s_log_t0 = GetTickCount();
    if (s_logf) { fprintf(s_logf, "=== shaiolib_crk debug log ===\n"); fflush(s_logf); }
    return s_logf;
}
#define SLOG(fmt, ...) do { FILE* _f = shg_logf(); if (_f) { \
    fprintf(_f, "[%5lu] " fmt "\n", (unsigned long)(GetTickCount()-s_log_t0), ##__VA_ARGS__); \
    fflush(_f); } } while(0)
#define SLOG_RATE(ts, fmt, ...) do { FILE* _f = shg_logf(); if (_f) { \
    DWORD _now = GetTickCount(); if (_now - (ts) >= SLOG_RATE_MS) { (ts) = _now; \
    fprintf(_f, "[%5lu] " fmt "\n", (unsigned long)(_now-s_log_t0), ##__VA_ARGS__); \
    fflush(_f); } } } while(0)

/* ── 2026-08-02 architecture rebuild: two DEBUG opt-OUT switches ────────────
 * Fresh headless testing against the real dump this pass reproduced the exact
 * near-instant (sub-1s) STATUS_ACCESS_VIOLATION crash documented in
 * docs/STOCK_TAKE.md's konami_pc row: a genuine ntdll-internal wait-block-list
 * race (RtlWakeAddressAll, the WAKE side of NtWaitForAlertByThreadId) that
 * fires under repeated multi-thread contention on the same kernel wait
 * address, reproducing at DLL_PROCESS_ATTACH time. A first attempt at fixing
 * this defaulted BOTH layers below to OFF (safer: no crash). LIVE RE-TEST
 * PROVED THAT WRONG: with both off the game reliably reaches a STABLE BLACK
 * SCREEN -- no crash, but also no video and no audio, confirmed by the
 * project owner testing the installed build directly and independently
 * confirmed via %TEMP%\shg_watchdog.log (the main thread parks forever, EIP
 * frozen at the exact same ntdll wait for 60+ consecutive 300ms samples,
 * because NOTHING ever times that wait out without the capping below). A
 * silent, stable black screen is a WORSE observable outcome than the old
 * behavior (which at least sometimes renders a real window before crashing --
 * the only runs this game has EVER produced a frame on). So both are back to
 * DEFAULT ON (the original, always-on behavior) -- these are now debug OPT-
 * OUT switches (set the var to "0" to disable), not opt-in switches, kept as
 * real, working kill switches for future debugging rather than deleted:
 *
 *   ULTRA_KONAMI_DEEP_HOOKS=0   -- disables the deep Nt*-wait-primitive inline
 *     hooks (NtWaitForAlertByThreadId-cap, NtSignalAndWaitForSingleObject-cap,
 *     NtWFSO/NtWFMO/NtWFKE-cap, msgloop-inline, hidden-wait-inline) -- the
 *     layer implicated in the ntdll race above, but ALSO the only thing that
 *     ever stops the main thread's kernel wait from blocking forever. The
 *     ALWAYS-necessary hooks (HID/board-presence faking, shaerror.dll fix,
 *     dialog suppression, D3D9 passthrough, the 9 targeted binary patches,
 *     GAKS/GKS auto-calibration) are unaffected either way -- those are plain
 *     IAT/GPA patches, not implicated in the ntdll-internal race, and stay on
 *     unconditionally regardless of this switch.
 *   ULTRA_KONAMI_FORCE_RENDER=0 -- disables the auto-click / rgate_keeper /
 *     direct-render-thread machinery that pokes KSHG.exe's own internals at
 *     hardcoded addresses and (after a 5s grace period) force-bootstraps a
 *     D3D9 device itself when the game's own state machine never calls
 *     Direct3DCreate9. This is the ONLY layer that has ever produced this
 *     game's real frames (a 150+s survival with an active window) -- so it
 *     stays on by default; set to 0 only to isolate whether a future crash
 *     comes from this layer specifically.
 *
 * Both are read once, at first use (module-level "static int" + one-shot
 * getenv, matching this file's existing ULTRA_SHAIOLIB_LOG pattern). */
static int s_deep_hooks_init = 0, s_deep_hooks = 0;
static int shg_deep_hooks_enabled(void) {
    if (!s_deep_hooks_init) {
        s_deep_hooks_init = 1;
        const char* e = getenv("ULTRA_KONAMI_DEEP_HOOKS");
        /* DEFAULT OFF. The build script has claimed for a month that this
         * defaults off; the code said otherwise, and that disagreement is
         * why the game died in ntdll. This layer manufactures STATUS_TIMEOUT
         * for waits ntdll issued with a NULL timeout, which drives
         * RtlpWaitOnCriticalSection into its deadlock-diagnostic branch and
         * dereferences that NULL. Opt IN with =1 if it is ever needed. */
        s_deep_hooks = (e && e[0] == '1');
    }
    return s_deep_hooks;
}
static int s_force_render_init = 0, s_force_render = 0;
static int shg_force_render_enabled(void) {
    if (!s_force_render_init) {
        s_force_render_init = 1;
        const char* e = getenv("ULTRA_KONAMI_FORCE_RENDER");
        /* Left ON: the auto-click / render-gate layer is what got this game
         * to a window before, and it is not implicated in the crash. */
        s_force_render = !(e && e[0] == '0');
    }
    return s_force_render;
}

/* ── internal state ─────────────────────────────────────────────────────── */
static int      g_coins[2]    = {0, 0};  /* coin counters per slot        */
static int      g_coin_held[2]= {0, 0};  /* edge-detect: was key held     */
static volatile int g_coin_accept = 1;  /* COIN_countup(1/0): the game's own
                                        * coin-acceptance switch. Starts enabled;
                                        * the service menu turns it off and on. */
static int      g_gun_en[2]   = {1, 1};  /* gun enabled per player        */
static int      g_last_err    = 0;
static HWND     g_hwnd        = NULL;

/* Gun activation tracking: don't report the gun as available until the user
 * has deliberately moved the mouse.  At game launch the cursor sits wherever
 * it was on the desktop (often the bottom-right corner of the 640×480 window).
 * If the service menu treats gun-Y as a scroll speed (Y > center → scroll DOWN),
 * the default corner position drives constant downward cycling.  We suppress the
 * gun until the cursor has moved ≥4 pixels from its initial position. */
static LONG     g_gun_init_x  = LONG_MIN;  /* screen coords at first GUN call */
static LONG     g_gun_init_y  = LONG_MIN;
static int      g_gun_active  = 0;         /* 1 once mouse has been aimed */
static DWORD    g_gun_arm_deadline = 0;    /* backstop: arm the gun even if the cursor never moves */
static int      g_trig_prev   = 0;         /* edge-detect: last LMB state */
static volatile int g_io_ready   = 0;      /* set in SECURE_check; gates security confirmation */
static volatile int g_io_enabled = 0;      /* set 5 s after security; gates gun + trigger output */
static int      g_post_sec_frames = 0;     /* update() calls since g_io_ready fired */
/* Survives terminate+initialize: the game calls SECURE_* only in the FIRST boot cycle, then
 * does terminate+initialize and runs without calling SECURE_* again.  g_io_ready/enabled are
 * reset in initialize, so without this flag they would stay 0 forever in the second cycle. */
static int      g_security_cleared = 0;
/* Calibration phase gate: set to 1 in is_boot_board_gun, cleared when SECURE_check fires.
 * Used to limit the WFSO cap to only the calibration window (not normal gameplay). */
static volatile int g_cal_phase = 0;
/* Set when VEH fires (audio callback crash handled): enables verbose Sleep/WFSO logging
 * so we can see what the game is waiting on after the GHL unwind. */
static volatile int g_veh_fired = 0;
static HANDLE       g_main_thread_h = NULL; /* set in VEH[256] for watchdog */
/* Set at the very top of DllMain (which runs on the main thread - the loader
 * injects via QueueUserAPC on the primary thread).  Lets the VEH tell "this
 * is the game's one true main thread" apart from any auxiliary detection
 * thread, BEFORE shg_start_watchdog() (which runs much later in DllMain) has
 * had a chance to open its own handle.  See the "non-render high-fault"
 * branch in shg_veh for why this distinction turned out to matter. */
static DWORD         g_main_thread_id = 0;
/* After security clears, the game's GUN CALIBRATION loop calls GetAsyncKeyState(VK_LBUTTON)
 * directly (not through shaiolib) waiting for a trigger press.  Without the real GHL board
 * this spins forever ("Not Responding").  We hook GAKS in KSHG.exe's IAT and return
 * "LMB pressed" for 500 ms after security so calibration sees one click and advances. */
static DWORD    g_auto_lmb_until = 0;

/* (button bitmask defines removed - BUTTON_status uses per-byte encoding) */

/* ── helpers ────────────────────────────────────────────────────────────── */
static HWND get_game_hwnd(void) {
    /* Re-scan for the game window by title on every call so we don't cache a
     * stale handle from before the game window was created (shaiolib_initialize
     * is called early, when the foreground window is still the loader).  If we
     * cache the loader window, ScreenToClient converts screen coords into loader-
     * client space, giving Y values > 1000 on a 1080p screen.  The game then
     * sees the gun pointing far below all menu items → wraps → cursor cycles. */
    HWND kshg = FindWindowA(NULL, "KSHG");
    if (kshg && IsWindow(kshg)) { g_hwnd = kshg; return g_hwnd; }
    if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
    g_hwnd = GetForegroundWindow();
    return g_hwnd;
}


/* ── GetAsyncKeyState / GetKeyState hooks (calibration auto-trigger) ──────
 * KSHG.exe's GUN CALIBRATION loop polls one of these to detect LMB.
 * GetAsyncKeyState reflects hardware state - updated immediately by SendInput.
 * GetKeyState reflects per-thread message state - only updated when the message
 * pump processes WM_LBUTTONDOWN.  When the pump is frozen (tight calibration
 * loop), GetKeyState never sees our SendInput events.
 * We hook BOTH so whichever path the game uses we return "LMB pressed" for
 * 500 ms after security clears. */
typedef SHORT (WINAPI *pfnGAKS)(int);
static pfnGAKS s_real_GAKS = NULL;
/* Rate-limited per-VK tracking: log each unique VK once, plus track LMB separately. */
static DWORD s_gaks_vk_seen[256/32];  /* bitmask: vk 0-255 seen */
static DWORD s_gaks_lmb_last = 0;     /* last time VK_LBUTTON was logged */
static SHORT WINAPI shg_GetAsyncKeyState(int vk) {
    if (vk >= 0 && vk < 256) {
        int idx = vk >> 5, bit = 1u << (vk & 31);
        if (!(s_gaks_vk_seen[idx] & bit)) {
            s_gaks_vk_seen[idx] |= bit;
            SLOG("GAKS: first vk=%d (0x%02X) auto_until=%lu",
                 vk, (unsigned)vk, (unsigned long)g_auto_lmb_until);
        }
    }
    if (vk == VK_LBUTTON) {
        DWORD _now = GetTickCount();
        SHORT real_val = s_real_GAKS ? s_real_GAKS(vk) : 0;
        int synth = (g_auto_lmb_until && _now < g_auto_lmb_until) ? 1 : 0;
        SHORT ret = synth ? (SHORT)0x8001 : real_val;
        if (_now - s_gaks_lmb_last >= 500) {
            s_gaks_lmb_last = _now;
            SLOG("GAKS LMB: real=%04X synth=%d ret=%04X auto_until=%lu",
                 (unsigned short)real_val, synth, (unsigned short)ret,
                 (unsigned long)g_auto_lmb_until);
        }
        if (synth) {
            if (g_auto_lmb_until && _now >= g_auto_lmb_until) {
                g_auto_lmb_until = 0;
                SLOG("auto-LMB GAKS window expired");
            }
            return (SHORT)0x8001;
        }
        return real_val;
    }
    return s_real_GAKS ? s_real_GAKS(vk) : 0;
}

typedef SHORT (WINAPI *pfnGKS)(int);
static pfnGKS s_real_GKS = NULL;
static SHORT WINAPI shg_GetKeyState(int vk) {
    static int s_gks_once = 0;
    if (!s_gks_once) { s_gks_once = 1; SLOG("GKS hook: first call vk=%d auto_until=%lu", vk, (unsigned long)g_auto_lmb_until); }
    if (vk == VK_LBUTTON && g_auto_lmb_until) {
        DWORD now = GetTickCount();
        if (now < g_auto_lmb_until)
            return (SHORT)0x8001;
        /* don't clear g_auto_lmb_until here - GAKS hook owns expiry */
    }
    return s_real_GKS ? s_real_GKS(vk) : 0;
}

/* ── auto-calibration SendInput thread ─────────────────────────────────── */
/* After security clears, KSHG.exe enters a GUN CALIBRATION loop that polls
 * GetAsyncKeyState(VK_LBUTTON) directly.  If our IAT hook for GAKS does not
 * fire (e.g. the game resolves it via GetProcAddress rather than the static IAT),
 * the loop spins forever → "Not Responding".
 * Belt-and-suspenders: spawn a thread that injects a real system-level LMB
 * click via SendInput.  This sets the kernel VK_LBUTTON state regardless of
 * how the game reads it (confirmed to work: identical to the manual click test).
 * Three clicks spaced 700ms apart covers 1-to-3-point calibration sequences
 * without risk of starting a credit (requires coin insert + start button). */
static volatile LONG s_auto_click_fired = 0;
static volatile LONG s_rgate_started = 0;
static volatile DWORD s_game_thread_id = 0; /* the game's REAL main thread - recorded from inside initialize() */
static volatile HANDLE s_game_thread_h  = NULL; /* SYNCHRONIZE handle to it, for the life watchdog */
static volatile LONG   g_shutting_down  = 0;    /* set in DLL_PROCESS_DETACH so the watchdog stands down */

/* THE RENDER PROBE IS GONE, AND SO IS THE PREMISE IT WAS BUILT ON.
 *
 * It patched 9 bytes at 0x4DA890 believing that to be a render-update entry
 * gated on a "render-system activation byte" at 0x6FB6E8, and forced that byte
 * to 1 so the gate always fell through.
 *
 * Every part of that is wrong. 0x6FB6E8 is SHOP CLOSING MODE -- the operator
 * setting. The menu label table at 0x6CEF00 reads
 *   { DIFFICULTY LEVEL, NUMBER OF LIVES, VIOLENCE LEVEL, LANGUAGE,
 *     SHOP CLOSING MODE, SHOP CLOSING TIME, PAYMENT MODE, ... }
 * and the setter at 0x4DA3B0 writes them in that order, SHOP CLOSING MODE
 * landing as a BYTE at 0x6FB6E8 = settings+0x28 = setting.1 file+0x30.
 * 0x4DA890 has exactly ONE caller, 0x4DA965, in the test-menu teardown.
 *
 * So this code was reaching into the operator settings and switching the
 * cabinet's shop-closing mode ON, several times a second, and then patching
 * over the check that reads it. That is the whole reason CLOSE SHOP would not
 * stay OFF: the owner set it off, the game loaded 0 from setting.1, and we set
 * it back to 1 at t=2968ms.
 */

/* ── Window un-minimize subsystem ─────────────────────────────────────── *
 * KSHG starts with its window iconic at (-32000,-32000) - the D3D9          *
 * exclusive-fullscreen minimize position.  We subclass the WndProc to block *
 * SC_MINIMIZE / SIZE_MINIMIZED messages and restore the window on a timer.  */
static HWND    s_kshg_wnd     = NULL;
static WNDPROC s_orig_wndproc = NULL;

static LRESULT CALLBACK shg_prevent_minimize(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE) {
        SLOG("wndproc: blocked SC_MINIMIZE");
        return 0;
    }
    if (msg == WM_SIZE && wp == SIZE_MINIMIZED) {
        SLOG("wndproc: blocked SIZE_MINIMIZED, posting SC_RESTORE");
        PostMessageA(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        return 0;
    }
    return CallWindowProcA(s_orig_wndproc, hwnd, msg, wp, lp);
}

static BOOL CALLBACK shg_enum_windows_cb(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    /* Accept any top-level window owned by our process that is iconic or large */
    RECT r = {0};
    GetWindowRect(hwnd, &r);
    if (IsIconic(hwnd) || (r.right - r.left) > 100) {
        *(HWND*)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI shg_rgate_keeper(LPVOID p) {
    (void)p;
    DWORD start_tick = GetTickCount();
    int logged_edi = 0;
    for (;;) {
        /* Log the EDI function pointer [0x680280] once to confirm it's valid. */
        if (!logged_edi && !IsBadReadPtr((void*)0x680280, 4)) {
            DWORD edi_val = *(volatile DWORD *)0x680280;
            SLOG("rgate_keeper: [0x680280]=0x%08lX (EDI fast-path fn ptr)", (unsigned long)edi_val);
            logged_edi = 1;
        }
        /* [0x6E3A10] master render gate - must stay 1 */
        volatile BYTE *rg10 = (volatile BYTE *)0x6E3A10;
        if (!IsBadWritePtr((void*)rg10, 1) && *rg10 == 0) {
            *rg10 = 1;
            SLOG("rgate_keeper: [0x6E3A10] restored to 1");
        }
        /* [0x6E3A11] second render enable - only set by game when 4th init param != 0;
         * without real GHL hardware that param is always 0, so we force it. */
        volatile BYTE *rg11 = (volatile BYTE *)0x6E3A11;
        if (!IsBadWritePtr((void*)rg11, 1) && *rg11 == 0) {
            *rg11 = 1;
            SLOG("rgate_keeper: [0x6E3A11] set to 1");
        }
        /* [0x6E3A12] IS NOT A "READY GATE". DO NOT WRITE IT.
         *
         * It was forced to 1 here, described as a "hardware-I/O ready gate set by
         * kacbookkeeping on real hardware". KSHG.exe disagrees. There are exactly
         * THREE references to this byte in the whole image, and the game NEVER
         * sets it:
         *   0x4D0574  mov BYTE PTR ds:0x6E3A12,0   the only write -- a CLEAR, and
         *                                          it sits at the tail of a
         *                                          teardown that closes the handle
         *                                          in [0x6CE748] via [0x68022C]
         *   0x4D05A1  cmp BYTE PTR ds:0x6E3A12,0   read: bail if zero
         *   0x4D0645  cmp BYTE PTR ds:0x6E3A12,bl  read: bail if zero
         *
         * Both readers guard a TRANSMIT path. 0x4D0580 checks [0x6E3A10], then the
         * handle at [0x6CE748], then this byte, and only then reads the button
         * words at 0x6E3988 / 0x6E398C / 0x6E3990 and pushes 0x28 bytes through
         * [0x680290]. So the byte means "that link is open", and the game clears it
         * precisely when it closes the link.
         *
         * Forcing it told the game a link was up that never was. The 2026-09-17 log
         * caught the argument: 63 "rgate_keeper: [0x6E3A12] set to 1" lines in a
         * 14-second run, about one every 203 ms -- the game tearing the link down
         * and us re-asserting it, five times a second, forever. The owner heard it
         * as a continuous beep at roughly that rate and described it as "some button
         * held down permanently".
         *
         * This is the same mistake as [0x6E3A18], found the same way: read the
         * game's own writers and what gates them. A byte the game only ever CLEARS
         * is not a flag waiting for us to set it. */
        /* [0x6E3A18] IS THE I/O-ERROR LATCH. DO NOT WRITE IT. ────────────────
         *
         * This used to be forced to 1, described as a "post-API-call enable".
         * That was wrong, and it is what parked this game on a blank GHL page
         * for an entire session while it ignored every button pressed at it.
         *
         * KSHG has exactly four references to this byte:
         *   0x4D0FD0  mov al,ds:0x6E3A18 ; ret        the getter, 9 callers
         *   0x4D0FE0  ... mov ds:0x6E3A18,cl  (cl=0)  cleared after reset_error
         *   0x4D1DE9  mov ds:0x6E3A18,bl   (bl=0)     zeroed by the game's init
         *   0x4D22A6  mov BYTE PTR ds:0x6E3A18,1      THE ONLY SETTER
         *
         * and that setter is gated on the call right before it:
         *   0x4D229C  call ds:0x6804A4   ; shaiolib_is_error
         *   0x4D22A1  test eax,eax
         *   0x4D22A3  je 0x4D22AD        ; no error -> skip the set
         * Our is_error stub returns 0 on every call, so the game itself would
         * NEVER set this byte. Forcing it manufactured an I/O error that does
         * not exist.
         *
         * It cost us twice over, which is why both halves of the symptom lined
         * up so neatly:
         *
         *   1. THE PICTURE. The boot path reads the getter and, if set, builds
         *      an ErrorView:
         *        0x4D7DC8 call 0x4D0FD0 / test al,al / je 0x4D7E1B
         *        0x4D7DD1 push 0x4CC / operator new / push 0 (NULL key)
         *        0x4D7DF3 call 0x4DB980          ; ErrorView ctor
         *      That is the blank framed box, the "GHL:A:A:A:2007122000" banner
         *      (built at 0x4DA4B0 from Data/TestMode/release_code.num) and the
         *      "CREDIT(S)=%d COIN(S)=%d" footer at 0x4DF3C5. The box is EMPTY
         *      and the credit number is garbage because the NULL-key path fills
         *      its record from shaerror_get_errdata, and our shaerror_crk.c
         *      zeroes that record.
         *
         *   2. THE WEDGE. Seven scene entry points share a three-way bail --
         *      TEST held? / this latch? / error state == 4? -- e.g. 0x4E663E,
         *      0x4F2772, 0x5026CB. While this byte is 1, no scene can start.
         *
         * And it explains why the owner's F2 / 9 / 5 / 1 / 2 / Z / X all reached
         * shaiolib_BUTTON_status (the log shows every one of them reported) and
         * still did nothing: ErrorView polls input only when its record's hint
         * begins "TEST BUTTON" or its mode is 2 or 3 (0x4DF3AB-0x4DF5B6), and a
         * zeroed record makes both false. The page cannot be dismissed. The
         * exit is to never build it.
         *
         * The game's own init already zeroes this byte at 0x4D1DE9. The log
         * caught the argument in progress: we set it at t=109ms, the game
         * cleared it, and we set it again at t=312ms. Leave it alone and the
         * game's zero stands.
         *
         * The neighbors are NOT implicated and stay exactly as they are:
         * 0x6E3A10, 0x6E3A11, 0x6E3A12 and the 0x6FB6E8 activation byte have no
         * reader in that scene-bail pattern. */
        /* 0x6FB6E8 IS DELIBERATELY NOT TOUCHED. It is SHOP CLOSING MODE, and
         * this loop used to force it to 1 every 200 ms. 0x4DA2E8 is the operand
         * of "mov byte [0x6FB6E8],al" inside the FACTORY-DEFAULTS copy at
         * 0x4DA290 -- not a hardware I/O copy, and nothing to do with render.
         * Leave the game's own value alone; it comes from setting.1 and it is
         * the operator's to choose. */

        /* ---- WHICH BIT IS STUCK --------------------------------------------
         *
         * The sound trace proved the beep is NOT a repeated Play call -- the
         * game never calls IDirectSoundBuffer::Play at all, so the sound comes
         * out of an already-looping stream, i.e. the game is genuinely mixing a
         * UI sound over and over. It is silent on the boot screen and beeps on
         * the title screen, so a menu is reacting to a HELD input.
         *
         * Rather than keep guessing which input, read the game's OWN assembled
         * button state. KSHG rebuilds these three words every frame at 0x4D2210
         * (it zeroes them at 0x4D223D/47/51, then ORs in each source), and every
         * consumer reads the gun bit through the is_pressed accessor:
         *     [0x6E3988] held      [0x6E398C] released     [0x6E3990] pressed
         * A bit that stays set in HELD is the stuck button, and its mask names
         * it -- 0x20 is P1's gun bit, per the mask table at 0x6CE7E4.
         *
         * Log-only, and only on CHANGE, so a steady state costs one line. */
        {
            static DWORD s_last_h = 0xFFFFFFFFu, s_last_r, s_last_p;
            volatile DWORD *held = (volatile DWORD *)0x6E3988;
            volatile DWORD *rel  = (volatile DWORD *)0x6E398C;
            volatile DWORD *prs  = (volatile DWORD *)0x6E3990;
            if (!IsBadReadPtr((void *)held, 4) &&
                !IsBadReadPtr((void *)rel, 4) &&
                !IsBadReadPtr((void *)prs, 4)) {
                DWORD h = *held, r = *rel, p = *prs;
                if (h != s_last_h || r != s_last_r || p != s_last_p) {
                    s_last_h = h; s_last_r = r; s_last_p = p;
                    SLOG("BTNWORD: held=%08lX released=%08lX pressed=%08lX",
                         (unsigned long)h, (unsigned long)r, (unsigned long)p);
                }
            }
        }
        /* Board-ID string "GHL-A", game may reset to "empty" or zero. */
        volatile char *rgb = (volatile char *)0x6CE7A4;
        if (!IsBadWritePtr((void*)rgb, 6) && memcmp((void*)rgb, "GHL-A", 5) != 0) {
            char old[9]; memcpy(old, (void*)rgb, 8); old[8] = '\0';
            memcpy((void*)rgb, "GHL-A\0", 6);
            SLOG("rgate_keeper: [0x6CE7A4] was '%.8s', restored to 'GHL-A'", old);
        }
        /* *** THIS IS WHERE PLAYER 1 START WAS BEING REFUSED. ***
         *
         * A block here forced [0x6FB614] to 2 after 10 seconds, believing states
         * 0 and 1 to be hardware-detect phases that "loop forever calling
         * ExitProcess at 78 s" and state 2 to enable the D3D render path.
         *
         * 0x6FB614 is the shop-closing STATE, and P1 START is refused whenever
         * it is >= 2 -- checked at 0x4F2795 (cmp eax,2 / jl), 0x4EF772 and
         * 0x4FB3A8. We were putting the cabinet into "shop closed" ten seconds
         * into every boot and then wondering why START did nothing.
         *
         * It also explains the workaround exactly: leaving the TEST menu
         * recomputes the state below 2, and the old force was a one-shot, so it
         * was never re-applied. Hence "START works, but only after a trip to the
         * service menu and back".
         *
         * The ExitProcess claim had no support either -- ExitProcess (IAT
         * 0x6800A4) has exactly one reference in the whole image, at 0x6553BA,
         * in CRT teardown. The render thread is started unconditionally and
         * never reads 0x6FB614.
         */

        /* Find game window and subclass it to prevent re-minimization */
        if (!s_kshg_wnd) {
            HWND found = NULL;
            EnumWindows(shg_enum_windows_cb, (LPARAM)&found);
            if (found) {
                s_kshg_wnd = found;
                SLOG("rgate_keeper: found HWND=%p, subclassing WndProc", (void*)found);
                s_orig_wndproc = (WNDPROC)(LONG_PTR)SetWindowLongA(
                    found, GWL_WNDPROC, (LONG)(LONG_PTR)shg_prevent_minimize);
            }
        }
        /* Keep restoring the window if it reverts to iconic */
        if (s_kshg_wnd && IsIconic(s_kshg_wnd)) {
            SLOG("rgate_keeper: window iconic, forcing SW_RESTORE + SetWindowPos");
            ShowWindow(s_kshg_wnd, SW_RESTORE);
            SetWindowPos(s_kshg_wnd, HWND_TOP, 0, 0, 1920, 1080,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER);
        }

        /* 2026-07-31: restart the render thread if it exited due to an
         * ntdll-internal crash (Fix 3 resets s_render_thread_started to 0
         * after ExitThread(0) via s_render_tid=0 signal).  Only restart
         * after D3D device is ready (s_d3d9_dev_fwd non-null) and we've
         * been running > 5s (enough time for an initial launch). */
        if (!s_render_tid && !s_render_thread_started &&
            s_d3d9_dev_fwd && (GetTickCount() - start_tick) >= 5000) {
            SLOG("rgate_keeper: render thread not running -- restarting");
            if (!InterlockedExchange(&s_render_thread_started, 1)) {
                HANDLE hrt = CreateThread(NULL, 0, shg_direct_render_thread, NULL, 0, NULL);
                if (hrt) { CloseHandle(hrt); SLOG("rgate_keeper: render thread restarted"); }
                else { InterlockedExchange(&s_render_thread_started, 0); }
            }
        }

        Sleep(200);
    }
    return 0;
}
/* ── Direct render thread ────────────────────────────────────────────────── *
 * The game only registers its render callback (0x425856) after GHL hardware *
 * init completes.  Without real hardware that never happens.  We call       *
 * 0x425856 directly from our own thread so the D3D render path fires.       *
 *                                                                            *
 * 0x425856 is a __thiscall method; the 'this' pointer must be in ECX when  *
 * it is called.  The game's render object lives at [0x6DE498].              */
typedef void (__thiscall *shg_render_thiscall_t)(void *self);

/* Forward declarations for D3D9 globals used by the render thread.
 * The actual definitions live in the D3D9 hook section below. */
typedef HRESULT (__stdcall *pfnD3DDevPresent_fwd)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT (__stdcall *pfnD3DEndScene_fwd)(void *);
/* s_d3d9_dev_fwd declared at top of file */
static pfnD3DDevPresent_fwd s_prs_fwd      = NULL;
static pfnD3DEndScene_fwd   s_end_fwd      = NULL;

/* Render thread TID - lets the VEH identify crashes on our render thread */
/* s_render_tid declared at top of file */
/* Set to 1 by the VEH when it catches and unwinds a crash on our render thread */
static volatile LONG  s_render_crashed = 0;

/* setjmp/longjmp recovery for render-thread crashes.
 * VEH detects crash on render thread, calls longjmp back to the saved point
 * in shg_direct_render_thread so the render loop can retry next frame. */
/* s_render_jmp, s_render_jmp_armed declared at top of file */
static volatile DWORD  s_render_exc_fault  = 0;
static volatile DWORD  s_render_exc_eip    = 0;
static volatile DWORD  s_render_exc_edx    = 0;  /* EDX saved from crash context */

/* No-op target for the vtable[1] patch at 0x681114.
 * The slot contains the IEEE float 0.0625 (0x3D800000) in the binary - not a
 * code pointer.  We overwrite it at runtime so the render function can call it
 * without faulting.  Unknown calling convention: use naked ret; any arguments
 * are harmless since we discard them and the caller cleans the stack. */
/* noop stub for missing hardware vtable methods.
 * Returns EAX=1 to match the prior VEH crash-recovery behavior:
 * before the 128-slot vtbl fix, calls to BSS-garbage slots crashed and
 * the VEH returned EAX=1 (success) for all fault<0x10000 recoveries.
 * The board detection loop at 0x4CFFDB uses the return value to decide
 * whether the board callback succeeded; EAX=0 causes an infinite retry. */
static void __attribute__((naked)) shg_vtable1_noop(void) {
    __asm__ volatile ("movl $1, %%eax\n\t" "ret\n\t" : : : );
}

/* Stub interface object written to KSHG's [0x00680D88] in .rdata.
 * The original shaiolib.dll populated this with a real C++ object at DLL init.
 * 128 slots covers all accesses: game calls slot 57 (offset 0xE4) and
 * slot 69 (offset 0x114) - beyond the old 32-slot array, those hit BSS
 * zeros and crash as near-null calls on the timer thread (tid=23128). */
#define SHG_IFACE_SLOTS 128
static DWORD shg_iface_vtbl[SHG_IFACE_SLOTS];  /* array of noop ptrs */
static DWORD shg_iface_obj[1];                  /* "object": first DWORD = vtbl ptr */

static void shg_install_interface_680D88(void) {
    DWORD noop_addr = (DWORD)(ULONG_PTR)shg_vtable1_noop;
    DWORD old_prot = 0;
    DWORD *target = (DWORD *)0x680D88u;
    int i;
    for (i = 0; i < SHG_IFACE_SLOTS; i++)
        shg_iface_vtbl[i] = noop_addr;
    shg_iface_obj[0] = (DWORD)(ULONG_PTR)shg_iface_vtbl;
    /* Patch .rdata pointer to our stub object */
    if (!IsBadReadPtr(target, 4) && VirtualProtect(target, 4, PAGE_READWRITE, &old_prot)) {
        *target = (DWORD)(ULONG_PTR)shg_iface_obj;
        VirtualProtect(target, 4, old_prot, &old_prot);
        SLOG("iface680D88: [0x680D88]=obj=0x%08lX vtbl=0x%08lX noop=0x%08lX",
             (unsigned long)shg_iface_obj[0] ? (unsigned long)*target : 0UL,
             (unsigned long)(ULONG_PTR)shg_iface_vtbl, (unsigned long)noop_addr);
    } else {
        SLOG("iface680D88: VirtualProtect failed err=%lu", (unsigned long)GetLastError());
    }
}

/* Patch all non-code entries in the 8-slot table at 0x681110.
 * Valid code ptrs are in .text (0x401000..0x67FFFF).  Other slots hold float
 * constants or .rdata pointers that crash when called.  The render function
 * at 0x425856 calls these slots sequentially; we noop all that aren't code. */
static void shg_patch_vtable1(void) {
    DWORD *tbl = (DWORD *)0x681110u;
    DWORD old_prot = 0;
    int i;
    SLOG("vtable-patch: table at 0x681110:");
    for (i = 0; i < 8; i++)
        SLOG("  [%d] @ 0x%08lX = 0x%08lX", i,
             (unsigned long)(0x681110u + (DWORD)(i * 4)), (unsigned long)tbl[i]);
    if (!VirtualProtect(tbl, 32, PAGE_EXECUTE_READWRITE, &old_prot)) {
        SLOG("vtable-patch: VirtualProtect err=%lu", (unsigned long)GetLastError());
        return;
    }
    for (i = 0; i < 8; i++) {
        DWORD v = tbl[i];
        if (v < 0x401000u || v >= 0x680000u) {  /* not in .text → not a code ptr */
            tbl[i] = (DWORD)(ULONG_PTR)shg_vtable1_noop;
            SLOG("vtable-patch: slot[%d] 0x%08lX → noop", i, (unsigned long)v);
        }
    }
    VirtualProtect(tbl, 32, old_prot, &old_prot);
}

/* Check if a DWORD value is the address of committed, executable memory.
 * Used to distinguish valid function pointers from stale DLL addresses /
 * float constants / data pointers inside vtable-like arrays. */
static int shg_is_exec_addr(DWORD v) {
    MEMORY_BASIC_INFORMATION mbi;
    DWORD prot;
    if (v < 0x1000u) return 0;
    if (!VirtualQuery((LPCVOID)(ULONG_PTR)v, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    prot = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return (prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
}

/* Patch the function-pointer table at [EDX+0..] where EDX = [0x00680D88].
 * Code at 0x0041C58F: CALL [EDX+0x14].  EDX is loaded from [0x00680D88].
 * On the original Konami arcade HW, the table entries are DLL function ptrs
 * that happen to be at the addresses hardcoded in the binary.  On our PC the
 * DLLs load at ASLR addresses, so any entry not in currently-committed
 * executable memory is a stale address → noop it.  Leave valid entries alone
 * so DLL dispatch that IS mapped (e.g. OpenGL, D3D) still works. */
static void shg_patch_vtable2(void) {
    DWORD vtbl_ptr;
    DWORD *vtbl;
    DWORD old_prot = 0;
    int i;
    if (IsBadReadPtr((void*)0x680D88u, 4)) {
        SLOG("vtable2-patch: 0x680D88 not readable");
        return;
    }
    vtbl_ptr = *(DWORD*)0x680D88u;
    SLOG("vtable2-patch: [0x680D88]=0x%08lX", (unsigned long)vtbl_ptr);
    if (!vtbl_ptr || IsBadReadPtr((void*)(ULONG_PTR)vtbl_ptr, 64)) {
        SLOG("vtable2-patch: vtbl_ptr not readable");
        return;
    }
    vtbl = (DWORD*)(ULONG_PTR)vtbl_ptr;
    /* Log first 16 entries with executability status */
    for (i = 0; i < 16; i++) {
        if (IsBadReadPtr(vtbl + i, 4)) break;
        SLOG("  vtbl2[%2d] @ 0x%08lX = 0x%08lX exec=%d", i,
             (unsigned long)(vtbl_ptr + (DWORD)(i * 4)),
             (unsigned long)vtbl[i], shg_is_exec_addr(vtbl[i]));
    }
    if (!VirtualProtect(vtbl, 64, PAGE_EXECUTE_READWRITE, &old_prot)) {
        SLOG("vtable2-patch: VirtualProtect err=%lu", (unsigned long)GetLastError());
        return;
    }
    for (i = 0; i < 16; i++) {
        if (IsBadReadPtr(vtbl + i, 4)) break;
        DWORD v = vtbl[i];
        if (!shg_is_exec_addr(v)) {
            vtbl[i] = (DWORD)(ULONG_PTR)shg_vtable1_noop;
            SLOG("  vtbl2-patch: slot[%d] 0x%08lX (not exec) → noop", i, (unsigned long)v);
        }
    }
    VirtualProtect(vtbl, 64, old_prot, &old_prot);
}

/* Call render_fn(render_obj) with ECX = ESI = render_obj.
 * 0x425856 reads [ESI] at offset +0x50 for a secondary vtable dispatch;
 * supplying ESI = render_obj gives it a non-null, valid vtable to read.
 *
 * IMPORTANT: use explicit "d"(fn)/"a"(obj) register constraints so GCC cannot
 * alias fn into ESI.  With generic "r" constraints GCC chose ESI for fn, which
 * the movl %1,%%esi then overwrote with obj - causing a call to the heap object
 * instead of the render function.  EDX and EAX are caller-saved; callee trashes
 * them freely, which is fine since we never read them after the call. */
static void shg_render_call(void *fn, void *obj) {
    __asm__ volatile (
        "pushl %%esi\n\t"        /* save caller's ESI */
        "movl  %%eax, %%esi\n\t" /* ESI = obj  (EAX holds obj via "a" constraint) */
        "movl  %%eax, %%ecx\n\t" /* ECX = obj  (thiscall this) */
        "call  *%%edx\n\t"        /* call fn    (EDX holds fn via "d" constraint) */
        "popl  %%esi\n\t"         /* restore caller's ESI */
        :
        : "d"(fn), "a"(obj)       /* fn→EDX, obj→EAX - no aliasing possible */
        : "ecx", "memory"
    );
}
/* s_render_thread_started declared at top of file */
/* 2026-07-31: restart counter - rgate_keeper polls this to respawn the render
 * thread if it exits due to an ntdll-internal crash (see Fix 3 above). */
static volatile LONG s_render_thread_restart = 0;
/* shg_direct_render_thread declared at top of file */

static DWORD WINAPI shg_direct_render_thread(LPVOID p) {
    (void)p;
    s_render_tid = GetCurrentThreadId();
    static int s_bootstrap_tries = 0;
    static int s_bootstrap_gaveup = 0;
    SLOG("render-thread: started tid=%lu, waiting for D3D device", (unsigned long)s_render_tid);
    {
        DWORD t0 = GetTickCount();
        while (!s_d3d9_dev_fwd) {
            Sleep(100);
            DWORD waited = GetTickCount() - t0;
            /* At 5s: if the game still hasn't called Direct3DCreate9,
             * bootstrap D3D9 ourselves. The game's state machine gates
             * this call behind real GHL hardware; we bypass it entirely.
             * 2026-07-31: this is the fix for the "Direct3DCreate9 never
             * called" gap identified in STOCK_TAKE.md. */
            /* ONE BOOTSTRAP ATTEMPT, NOT A SPIN.
             *
             * This retried every 100 ms forever. The 2026-09-17 log shows 97
             * full attempts in a 21-second run -- roughly six a second, each
             * one creating an IDirect3D9, PATCHING ITS VTABLE, trying
             * CreateDevice twice (hardware then software VP), failing both
             * with hr=0x88760868, and going round again. 97 vtable patches and
             * ~200 failed device creations, continuously, underneath a game
             * that was rendering perfectly well on its own device.
             *
             * If two CreateDevice calls fail, a third identical one will not
             * succeed. The bootstrap exists for the case where the game NEVER
             * creates a device; when it cannot create one either, the honest
             * thing is to stop and say so, not to churn D3D at 6 Hz forever.
             *
             * Capped at 3 attempts. The loop still waits on s_d3d9_dev_fwd
             * afterwards, so if the game does eventually produce a device the
             * render thread picks it up exactly as before. */
            if (waited >= 5000 && !s_d3d9_dev_fwd && s_bootstrap_tries >= 3
                && !s_bootstrap_gaveup) {
                s_bootstrap_gaveup = 1;
                SLOG("render-thread: D3D9 bootstrap FAILED 3 times -- giving up and "
                     "leaving D3D alone. If the game has its own device this is "
                     "harmless; if the screen is black, THIS is why.");
            }
            if (waited >= 5000 && !s_d3d9_dev_fwd && s_bootstrap_tries < 3) {
                ++s_bootstrap_tries;
                SLOG("render-thread: game has not called Direct3DCreate9 after 5s -- "
                     "bootstrapping D3D9 (attempt %d of 3)", s_bootstrap_tries);
                shg_bootstrap_d3d9();
            }
            if (waited > 120000 && !s_d3d9_dev_fwd) {
                SLOG("render-thread: D3D device not ready after 120s, aborting");
                InterlockedExchange(&s_render_thread_started, 0);
                return 0;
            }
        }
        SLOG("render-thread: D3D device ready at %lums, sleeping 15s for kacbookkeeping",
             (unsigned long)(GetTickCount() - t0));
        Sleep(15000);
    }

    void *render_fn = (void *)(ULONG_PTR)0x425856u;
    /* volatile so longjmp does not restore them to stale register values */
    volatile DWORD frame = 0;
    volatile DWORD bad   = 0;
    volatile DWORD ok    = 0;

    /* Patch vtable[1] at 0x681114 from float-constant 0x3D800000 to a real noop
     * so the game's render function can call it without faulting. */
    shg_patch_vtable1();

    /* Log key data-section state once at render start */
    {
        DWORD v614 = !IsBadReadPtr((void*)0x6DF614u,4) ? *(DWORD*)0x6DF614u : 0xDEAD;
        DWORD v610 = !IsBadReadPtr((void*)0x6DF610u,4) ? *(DWORD*)0x6DF610u : 0xDEAD;
        DWORD v498 = !IsBadReadPtr((void*)0x6DE498u,4) ? *(DWORD*)0x6DE498u : 0xDEAD;
        SLOG("render-thread: pre-render state [0x6DF614]=%08lX [0x6DF610]=%08lX [0x6DE498]=%08lX",
             (unsigned long)v614, (unsigned long)v610, (unsigned long)v498);
    }

    for (;;) {
        DWORD *obj_ptr = (DWORD *)0x6DE498u;
        BOOL valid = !IsBadReadPtr((void*)obj_ptr, 4) && *obj_ptr != 0;
        if (!valid) {
            if (frame == 0 || frame % 30 == 0)
                SLOG("render-thread: [0x6DE498]=0, waiting...");
            Sleep(500);
            frame++;
            continue;
        }
        void *render_obj = (void *)(ULONG_PTR)(*obj_ptr);

        if (frame < 5 || frame % 60 == 0)
            SLOG("render-thread: frame %lu obj=%p ok=%lu bad=%lu",
                 (unsigned long)frame, render_obj,
                 (unsigned long)ok, (unsigned long)bad);

        /* longjmp/setjmp recovery: VEH fires on render-thread crash, saves fault
         * info, then calls longjmp back here.
         *
         * IMPORTANT: use canonical if(setjmp()!=0) form with volatile int crashed.
         * A plain `int crashed = setjmp(...)` lets GCC keep crashed in EAX; then
         * InterlockedExchange/Present (which clobber EAX) corrupt it before the
         * `if (crashed)` check at the bottom.  volatile forces a memory store
         * immediately after setjmp "returns" (both normal and longjmp paths), so
         * subsequent calls cannot alias the value away. */
        volatile int crashed = 0;
        InterlockedExchange(&s_render_jmp_armed, 1);
        if (setjmp(s_render_jmp) != 0) {
            crashed = 1;  /* longjmp path: render call faulted */
        } else {
            shg_render_call(render_fn, render_obj);
            crashed = 0;  /* normal return: no crash */
        }
        InterlockedExchange(&s_render_jmp_armed, 0);

        if (frame < 5)
            SLOG("render-thread: post-render frame=%lu crashed=%d bad=%lu ok=%lu",
                 (unsigned long)frame, (int)crashed,
                 (unsigned long)bad, (unsigned long)ok);

        /* Flip back buffer to screen after the render chain runs (or crashes).
         * Skip EndScene here - calling it after VEH recovery crashes d3d9.dll. */
        if (s_d3d9_dev_fwd && s_prs_fwd) {
            HRESULT phr = s_prs_fwd(s_d3d9_dev_fwd, NULL, NULL, NULL, NULL);
            if (frame < 5 || frame % 60 == 0)
                SLOG("render-thread: Present frame=%lu hr=0x%08lX",
                     (unsigned long)frame, (unsigned long)phr);
        }

        if (crashed) {
            bad++;
            /* One-shot patch: on first crash at 0x24748B56, fix the vtable at [0x680D88] */
            {
                static volatile LONG s_vt2_done = 0;
                if (s_render_exc_eip == 0x24748B56u &&
                    !InterlockedCompareExchange(&s_vt2_done, 1, 0))
                    shg_patch_vtable2();
            }
            if (bad <= 3 || bad % 60 == 0)
                SLOG("render-thread: CRASH frame=%lu #%lu fault=0x%08lX eip=0x%08lX (longjmp-recovered)",
                     (unsigned long)frame, (unsigned long)bad,
                     (unsigned long)s_render_exc_fault, (unsigned long)s_render_exc_eip);
            if (bad == 120) {
                SLOG("render-thread: 120 consecutive crashes, pausing 60s");
                Sleep(60000);
                bad = 0;
            }
            Sleep(1000);
        } else {
            if (bad) {
                SLOG("render-thread: RECOVERED frame=%lu after %lu crashes",
                     (unsigned long)frame, (unsigned long)bad);
                bad = 0;
            }
            ok++;
            if (ok <= 5 || ok % 300 == 0)
                SLOG("render-thread: ok frame=%lu total_ok=%lu",
                     (unsigned long)frame, (unsigned long)ok);
        }

        frame++;
        if (!crashed) Sleep(16);
    }
    return 0;
}

static DWORD WINAPI shg_auto_click_thread(LPVOID arg) {
    (void)arg;
    INPUT dn, up;
    memset(&dn, 0, sizeof dn); dn.type=INPUT_MOUSE; dn.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    memset(&up, 0, sizeof up); up.type=INPUT_MOUSE; up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
    /* Cover ALL input paths the calibration loop might use:
     *   SendInput  → sets hardware GAKS state (GAKS-based loops)
     *   PostMessage → puts WM_LBUTTONDOWN in the message queue (pump-based loops)
     * Three clicks with 100ms hold, spaced 500ms apart. */
    int delays[3] = { 300, 500, 500 };
    int i;
    for (i = 0; i < 3; i++) {
        Sleep(delays[i]);
        HWND hw = FindWindowA(NULL, "KSHG");
        if (!hw) { SLOG("auto-click[%d]: no KSHG hwnd yet", i); }
        if (hw) SetForegroundWindow(hw);
        /* Arm auto-LMB so shg_ReadFile delivers trigger=1 in HID reports */
        g_auto_lmb_until = GetTickCount() + 200;
        SendInput(1, &dn, sizeof(INPUT));
        if (hw) PostMessage(hw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(320,240));
        SLOG("auto-click[%d]: LMB DOWN (hwnd=%p)", i, (void*)hw);
        Sleep(100);
        SendInput(1, &up, sizeof(INPUT));
        if (hw) PostMessage(hw, WM_LBUTTONUP, 0, MAKELPARAM(320,240));
        g_auto_lmb_until = 0;
        SLOG("auto-click[%d]: LMB UP", i);
    }
    SLOG("auto-click: done");
    return 0;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */
static void shg_write_gun_cal(void);  /* defined in SECURE_check section below */

/* ── LIFE WATCHDOG ───────────────────────────────────────────────────────────
 *
 * THE OWNER MUST NEVER NEED TASK MANAGER. That is the whole job of this block.
 *
 * On 2026-09-17 a corrupted return address made our VEH run ExitThread(0) on
 * the game's main thread. ExitThread on a main thread does NOT end a Win32
 * process: the game's window died, and the render thread, the Sleep-capper,
 * the heartbeat and -- worst of all -- the hook's WS_EX_TOPMOST pause overlay
 * kept running for another 74 seconds. The owner was left with a click-eating
 * window floating over their desktop, above a process only Task Manager could
 * kill.
 *
 * The root cause is fixed, but a game can lose its main thread for reasons
 * that are not our fault, so this is a floor under the failure rather than a
 * bet that it never recurs.
 *
 * WHY WAIT ON THE THREAD HANDLE, and not on a window. Both obvious liveness
 * checks read TRUE in this exact bug: d3d9_throttle.c re-points the device
 * window on every CreateDevice (the log shows a live 1920x1080 in-process
 * window at t=5.7s, well after the game window was gone), and the EnumWindows
 * probe also found a window 4.5s after death. A thread handle cannot lie.
 *
 * THREE THINGS THIS MUST NOT DO:
 *   1. Fire on a NORMAL exit, where the main thread also ends. Hence the grace
 *      period and g_shutting_down, which DLL_PROCESS_DETACH sets.
 *   2. Leave a mouse button latched. TerminateProcess skips DLL_PROCESS_DETACH,
 *      where the auto-click release normally happens -- and a pinned mouse has
 *      already cost this owner twice. So release LMB and RMB first, the same
 *      way shg_auto_click_thread does.
 *   3. Fire before the game ever ran. s_game_thread_h stays NULL until
 *      initialize(), so it fails closed.
 */
static DWORD WINAPI shg_life_watchdog_proc(LPVOID a) {
    HANDLE h = (HANDLE)s_game_thread_h;
    INPUT up;
    (void)a;
    if (!h) return 0;

    WaitForSingleObject(h, INFINITE);

    SLOG("life-watchdog: GAME MAIN THREAD tid=%lu HAS EXITED -- the process cannot run without "
         "it and will not end by itself; releasing the mouse and terminating",
         (unsigned long)s_game_thread_id);

    /* Give a clean shutdown time to announce itself before we force one. */
    Sleep(1500);
    if (g_shutting_down) {
        SLOG("life-watchdog: DLL_PROCESS_DETACH is running -- normal shutdown, standing down");
        return 0;
    }

    /* Never hand back a latched mouse button. */
    memset(&up, 0, sizeof up);
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;  SendInput(1, &up, sizeof(INPUT));
    up.mi.dwFlags = MOUSEEVENTF_RIGHTUP; SendInput(1, &up, sizeof(INPUT));

    SLOG("life-watchdog: TerminateProcess(self, 3)");
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

static void shg_arm_life_watchdog(void) {
    HANDLE t;
    if (s_game_thread_h || !s_game_thread_id) return;
    s_game_thread_h = OpenThread(SYNCHRONIZE, FALSE, s_game_thread_id);
    if (!s_game_thread_h) {
        SLOG("life-watchdog: OpenThread(tid=%lu) FAILED err=%lu -- NO self-terminate protection "
             "this run; if the game loses its main thread the process will have to be killed by hand",
             (unsigned long)s_game_thread_id, (unsigned long)GetLastError());
        return;
    }
    t = CreateThread(NULL, 0, shg_life_watchdog_proc, NULL, 0, NULL);
    if (t) { CloseHandle(t); SLOG("life-watchdog: armed on game main thread tid=%lu",
                                  (unsigned long)s_game_thread_id); }
    else     SLOG("life-watchdog: CreateThread FAILED err=%lu", (unsigned long)GetLastError());
}

__declspec(dllexport) int shaiolib_initialize(void) {
    /* RECORD THE GAME'S REAL MAIN THREAD, AS EARLY AS WE EVER SEE IT.
     *
     * The game calls this from its own main thread, at ~125 ms. Waiting for
     * the first update() call instead left the VEH's identity test unarmed
     * until ~1046 ms, and g_main_thread_id is the injection thread, not this
     * one -- so a fault in that window had nothing to compare against and the
     * main thread got killed. Arm it here and the window closes. */
    if (!s_game_thread_id) {
        s_game_thread_id = GetCurrentThreadId();
        SLOG("initialize: GAME MAIN THREAD tid=%lu (DllMain ran on tid=%lu -- that is the "
             "INJECTION thread and is NEVER the game's main thread)",
             (unsigned long)s_game_thread_id, (unsigned long)g_main_thread_id);
        shg_arm_life_watchdog();
    }
    g_coins[0] = g_coins[1] = 0;
    g_last_err = 0;
    /* Reset gun activation state on every init so the movement gate re-arms
     * after the game's own terminate+reinitialize cycle at ~1s post-boot. */
    g_gun_arm_deadline = 0;
    g_gun_init_x = LONG_MIN;
    g_gun_init_y = LONG_MIN;
    g_gun_active = 0;
    g_trig_prev       = 0;
    g_post_sec_frames = 0;
    /* Force security cleared: on real GHL hardware SECURE_check sets this, but
     * without real hardware SECURE_check is never called.  Set it here so the
     * game treats initialization as successful and enters the render loop.
     * Previous observation "returning 1 → short-boot → no render" was taken
     * before [0x6E3A10] was identified and patched; now that the render gate is
     * also forced open, returning 1 should be safe. */
    g_security_cleared = 1;
    g_io_ready   = 1;
    g_io_enabled = 1;
    g_auto_lmb_until = GetTickCount() + 80;
    /* Write board-ID string: is_boot_board_gun (which writes "GHL-A") may
     * never be called if the game takes the short-boot path.  Write it here
     * unconditionally so the BeginScene render gate is open from the start. */
    {
        volatile char *rgb = (volatile char *)0x6CE7A4;
        if (!IsBadWritePtr((void*)rgb, 6)) {
            memcpy((void*)rgb, "GHL-A\0", 6);
            SLOG("initialize: wrote 'GHL-A' to board-id [0x6CE7A4]");
        }
    }
    SLOG("initialize: security forced → io_ready=1 io_enabled=1 auto-LMB 80ms");
    if (!shg_force_render_enabled()) {
        SLOG("initialize: ULTRA_KONAMI_FORCE_RENDER=0 -- skipping auto-click/"
             "rgate_keeper/direct-render-thread (debug opt-out; the game will NOT "
             "reach Direct3DCreate9/render on its own -- expect a black screen).");
    } else {
    if (!InterlockedExchange(&s_auto_click_fired, 1)) {
        HANDLE ht = CreateThread(NULL, 0, shg_auto_click_thread, NULL, 0, NULL);
        if (ht) { CloseHandle(ht); SLOG("initialize: auto-click thread spawned"); }
        else SLOG("initialize: WARNING auto-click CreateThread failed");
    }
    /* Start render-gate keeper thread (idempotent) */
    if (!InterlockedExchange(&s_rgate_started, 1)) {
        HANDLE ht2 = CreateThread(NULL, 0, shg_rgate_keeper, NULL, 0, NULL);
        if (ht2) CloseHandle(ht2);
    }
    /* Start direct render thread - calls 0x425856 at 60fps bypassing the
     * callback registration that requires real GHL hardware (idempotent) */
    if (!InterlockedExchange(&s_render_thread_started, 1)) {
        HANDLE ht3 = CreateThread(NULL, 0, shg_direct_render_thread, NULL, 0, NULL);
        if (ht3) { CloseHandle(ht3); SLOG("initialize: direct render thread spawned"); }
        else SLOG("initialize: WARNING direct render thread CreateThread failed");
    }
    }
    SLOG("initialize -> 1 (security forced, render gate armed)");
    return 1;
}
__declspec(dllexport) void shaiolib_terminate(void) { SLOG("terminate"); }
__declspec(dllexport) int  shaiolib_start_boot(void)          { SLOG("start_boot -> 1"); return 1; }
__declspec(dllexport) int  shaiolib_is_boot_all(void)         { SLOG("is_boot_all -> 1"); return 1; }
__declspec(dllexport) int  shaiolib_is_boot_board_button(void){ SLOG("is_boot_board_button -> 1"); return 1; }
__declspec(dllexport) int  shaiolib_is_boot_board_gun(void)   {
    SLOG("is_boot_board_gun -> 1");
    /* Boot board checks complete - safe to enable I/O for gameplay.
     * In short-boot (gun.1 present) SECURE_* is skipped, so this is the only
     * event that signals "all boot checks passed; game entering attract/play mode".
     * In full-boot, SECURE_check fires after this, and the second initialize
     * re-confirms g_io_enabled via g_security_cleared; this call is a no-op there. */
    if (!g_io_enabled) {
        g_io_ready   = 1;
        g_io_enabled = 1;
        g_cal_phase  = 1;  /* mark calibration window so WFSO cap activates */
        SLOG("I/O enabled: boot board checks passed");
    }
    /* Render gate backup: [0x6CE7A4] must NOT be "empty" for BeginScene to fire.
     * The SYSINFO get_mother_board wrapper should copy our return value there;
     * this direct write is a safety net in case that path runs before ours. */
    {
        volatile char *rg = (volatile char *)0x6CE7A4;
        if (!IsBadWritePtr((void*)rg, 6) && memcmp((void*)rg, "empty\0", 6) == 0) {
            memcpy((void*)rg, "GHL-A\0", 6);
            SLOG("is_boot_board_gun: wrote 'GHL-A' to render-gate string [0x6CE7A4]");
        } else if (!IsBadReadPtr((void*)rg, 6)) {
            SLOG("is_boot_board_gun: [0x6CE7A4] already='%.8s'", (char*)rg);
        }
    }
    /* Render-enable byte: [0x6E3A10] must be non-zero for the attract/game render
     * loop to call BeginScene/Present.  On real hardware SECURE_check sets this;
     * we set it here since SECURE_check is never reached without real GHL hardware. */
    {
        volatile BYTE *rgate = (volatile BYTE *)0x6E3A10;
        if (!IsBadWritePtr((void*)rgate, 1)) {
            BYTE prev = *rgate;
            *rgate = 1;
            SLOG("is_boot_board_gun: [0x6E3A10] render-enable %u->1", (unsigned)prev);
        } else {
            SLOG("is_boot_board_gun: [0x6E3A10] not writable (IsBadWritePtr)");
        }
    }
    return 1;
}
__declspec(dllexport) int  shaiolib_is_error(void)            { SLOG("is_error -> 0"); return 0; }
__declspec(dllexport) void shaiolib_reset_error(void)         { SLOG("reset_error"); g_last_err = 0; }
__declspec(dllexport) int  shaiolib_get_lasterror(void)       { SLOG("get_lasterror -> 0"); return g_last_err; }

/* ── GAMEPAD SOURCE: XINPUT, READ HERE, NOT THROUGH ULTRA_IO_SHM ──────────
 *
 * KSHG is a cabinet. Every input the game has reaches it through this DLL, so
 * this is where a pad has to arrive.
 *
 * The obvious route -- read the loader's already-resolved pad out of the
 * ULTRA_IO_SHM block -- does not work, for two independent reasons, both read
 * out of the source rather than assumed:
 *
 *   1. konami_pc_module.cpp:563-583 creates its OWN block, named
 *      "Local\UltraLoaderIO_<pid>_<n>", and calls SetEnvironmentVariableA on
 *      ULTRA_IO_SHM in the loader process before spawning us. That clobbers
 *      the name ultra_io_publish.c:135/162 published ("Local\UltraIO_<pid>"),
 *      so the child looks up the wrong block.
 *   2. Nothing ever writes that second block. Its only writer is set_input()
 *      (konami_pc_module.cpp:866-923), whose single call site in the entire
 *      tree is work/module_host/host.cpp:1391 -- a path a native_x86 dispatch
 *      never takes, because the module spawns KSHG.exe itself.
 *
 * A shaiolib that opened ULTRA_IO_SHM would therefore map a block with correct
 * magic and LOADER_ALIVE set whose sequence counter never moved: alive-looking
 * and delivering nothing, which is the worst kind of wrong. SHM-PROBE below
 * keeps that a measurement instead of a belief.
 *
 * XInput here is not a new dependency. shg_pad_aim() further down this same
 * file already drives player 2's gun from it; this reads it ONCE per frame and
 * shares the snapshot.
 */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } shg_xpad;
typedef struct { DWORD packet; shg_xpad pad; }                         shg_xstate;
typedef DWORD (WINAPI *shg_pXInputGetState)(DWORD, shg_xstate *);

#define SHG_XI_START 0x0010
#define SHG_XI_BACK  0x0020
#define SHG_XI_L3    0x0040
#define SHG_XI_R3    0x0080
#define SHG_XI_LB    0x0100
#define SHG_XI_RB    0x0200
#define SHG_XI_A     0x1000
#define SHG_XI_B     0x2000
#define SHG_XI_X     0x4000
#define SHG_XI_Y     0x8000

static int shg_env_int(const char *name, int dflt);   /* defined below, beside shg_pad_aim */

typedef struct {
    int   present;
    WORD  buttons;
    BYTE  lt, rt;
    SHORT lx, ly, rx, ry;
} shg_pad_snapshot;

static shg_pad_snapshot g_padst[2];

static int shg_pad_enabled(void) {
    static int s_on = -1;
    if (s_on < 0) s_on = shg_env_int("ULTRA_SHG_PAD", 1);   /* ULTRA_SHG_PAD=0 = keyboard only */
    return s_on;
}

/* One XInput binding for the whole DLL. shg_pad_aim() resolves through here
 * too, so there is exactly one place that can fail to find the export. */
static shg_pXInputGetState shg_xinput(void) {
    static shg_pXInputGetState s_get = NULL;
    static int s_tried = 0;
    if (!s_tried) {
        static const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
        int i;
        s_tried = 1;
        for (i = 0; dlls[i] && !s_get; ++i) {
            HMODULE h = LoadLibraryA(dlls[i]);
            if (h) s_get = (shg_pXInputGetState)(void *)GetProcAddress(h, "XInputGetState");
        }
        SLOG("PAD: XInput %s", s_get ? "loaded" : "NOT AVAILABLE - keyboard path unchanged");
    }
    return s_get;
}

/* ONE XInput read per frame, on the game's own thread.
 *
 * An EMPTY slot costs roughly 0.5-1 ms inside XInputGetState -- at 60 Hz that
 * is a measurable frame cost for a pad that is not even plugged in -- so an
 * absent slot is re-probed at 1 Hz. A connected pad costs microseconds and is
 * read every frame.
 *
 * LoadLibraryA is safe from here: update() runs on the game's main thread long
 * after DllMain, never under the loader lock. */
static void shg_pad_tick(void) {
    static DWORD s_next_probe[2];
    shg_pXInputGetState get;
    int slot, want;

    if (!shg_pad_enabled()) return;
    get = shg_xinput();
    if (!get) return;

    want = shg_env_int("ULTRA_SHG_PAD_P2", 0) ? 2 : 1;
    for (slot = 0; slot < want; ++slot) {
        shg_xstate st;
        DWORD now = GetTickCount();
        if (!g_padst[slot].present && now < s_next_probe[slot]) continue;
        memset(&st, 0, sizeof st);
        if (get((DWORD)slot, &st) == 0) {
            int was = g_padst[slot].present;
            g_padst[slot].present = 1;
            g_padst[slot].buttons = st.pad.wButtons;
            g_padst[slot].lt = st.pad.bLT; g_padst[slot].rt = st.pad.bRT;
            g_padst[slot].lx = st.pad.lx;  g_padst[slot].ly = st.pad.ly;
            g_padst[slot].rx = st.pad.rx;  g_padst[slot].ry = st.pad.ry;
            if (!was) SLOG("PAD: slot %d CONNECTED", slot);
        } else {
            if (g_padst[slot].present) SLOG("PAD: slot %d disconnected", slot);
            memset(&g_padst[slot], 0, sizeof g_padst[slot]);
            s_next_probe[slot] = now + 1000;
        }
    }
}

static int shg_pad_btn(int slot, WORD mask) {
    if (slot < 0 || slot > 1 || !g_padst[slot].present) return 0;
    return (g_padst[slot].buttons & mask) ? 1 : 0;
}

/* PAD AIM IS A VIRTUAL CURSOR, NEVER THE OS CURSOR.
 *
 * SetCursorPos is forbidden in this file: moving or parking the real pointer
 * has pinned the owner's mouse twice. This integrates the stick into a
 * position inside the game's own 640x480 aim box and never touches the system
 * cursor at all.
 *
 * Ownership is last-mover-wins. The pad claims aim only on a deliberate
 * deflection past the dead zone, and gives it straight back the moment the
 * mouse moves -- so with no pad attached, or a pad sitting still, GUN_status
 * executes exactly the instructions it executes today. */
/* Published by shaiolib_GUN_status every frame: exactly what the game was told,
 * so the crosshair cannot drift away from where the shot will actually land. */
static volatile LONG g_xh_x[2]    = { -1, -1 };
static volatile LONG g_xh_y[2]    = { -1, -1 };
static volatile LONG g_xh_live[2] = {  0,  0 };

static float g_paim_x = -1.0f, g_paim_y = -1.0f;   /* 0..1 within the aim box */
static int   g_pad_owns_aim = 0;
static DWORD g_paim_t = 0;

/* STICK AIM IS OPT-IN, AND OFF BY DEFAULT. THIS IS A CORRECTION.
 *
 * It shipped ON, and that was wrong. This is a LIGHTGUN title, and the aim path
 * that actually works is the mouse -- driven either by a real mouse or by the
 * Omni-Track lightgun service (ULTRA_LIGHTGUN_ENABLE=1, which is what this
 * game's container sets). A virtual stick aim is a SECOND aim source competing
 * for the same output, and last-mover-wins does not arbitrate them safely: the
 * pad claims ownership on any deflection past the dead zone and only releases
 * it when the cursor physically moves, so a resting-but-drifting stick can hold
 * the claim and freeze the gun where it stands.
 *
 * That is exactly what the 16:47 log shows: owns=pad on all 195 samples, X
 * frozen at 286 for the entire session while the owner was aiming with the gun
 * service and getting hits. The stick was not broken; it was winning an
 * argument it should never have been in.
 *
 * So: buttons come from the pad (measured working -- TEST, SERVICE, COIN,
 * START and OPTION R all produced clean edges), aim comes from the mouse, and
 * the two no longer overlap. Set ULTRA_SHG_PAD_AIM=1 only for a setup with no
 * gun and no mouse, where a stick is the only pointing device available. */
static int shg_pad_aim_enabled(void) {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = shg_env_int("ULTRA_SHG_PAD_AIM", 0);
        if (s_on && shg_env_int("ULTRA_LIGHTGUN_ENABLE", 0))
            SLOG("PAD: stick aim ON while the lightgun service is ALSO on -- two "
                 "sources are driving one gun, expect them to fight");
        else
            SLOG("PAD: stick aim %s (buttons are unaffected either way)",
                 s_on ? "ON (ULTRA_SHG_PAD_AIM=1)" : "off - aim comes from the mouse");
    }
    return s_on;
}

static void shg_pad_aim_tick(void) {
    DWORD now = GetTickCount(), dt;
    float sx, sy, speed;

    if (!shg_pad_aim_enabled()) return;
    if (!shg_pad_enabled() || !g_padst[0].present) return;
    if (!g_paim_t) g_paim_t = now;
    dt = now - g_paim_t;
    g_paim_t = now;
    if (dt > 100) dt = 100;                      /* a stall must not teleport the aim */

    sx = (float)g_padst[0].lx / 32767.0f;
    sy = (float)g_padst[0].ly / 32767.0f;
    if (sx > -0.24f && sx < 0.24f) sx = 0.0f;    /* a resting stick must never aim */
    if (sy > -0.24f && sy < 0.24f) sy = 0.0f;
    if (sx == 0.0f && sy == 0.0f) return;        /* stick at rest: the mouse keeps aim */

    if (g_paim_x < 0.0f) { g_paim_x = 0.5f; g_paim_y = 0.5f; }   /* first claim: center */

    speed = (float)shg_env_int("ULTRA_SHG_PAD_AIM_SPEED", 110) / 100.0f;   /* screens/sec */
    g_paim_x += sx * speed * (float)dt / 1000.0f;
    g_paim_y -= sy * speed * (float)dt / 1000.0f;   /* stick up = screen up */

    if (g_paim_x < 0.0f) g_paim_x = 0.0f;
    if (g_paim_x > 1.0f) g_paim_x = 1.0f;
    if (g_paim_y < 0.0f) g_paim_y = 0.0f;
    if (g_paim_y > 1.0f) g_paim_y = 1.0f;
    g_pad_owns_aim = 1;
}

/* PADRAW: what the stick ACTUALLY reports, versus what we did with it.
 *
 * The 16:47 run logged 195 GUN samples in which Y moved across eleven values
 * and X never left 286 -- not by one pixel, in nearly two minutes of play. The
 * buttons in that same run were provably fine (TEST, SERVICE, COIN, START and
 * OPTION R all showed clean 0x81/0x80/0x01 edges), which means the XInput read
 * itself is correct and lx specifically is what needs looking at.
 *
 * Rather than guess between "the stick's X really does rest inside the 24%
 * dead zone" and "we are mishandling it", print both the raw axis and the
 * integrated result on the same line. Rate-limited, and it is logged whether
 * or not the stick is deflected -- a probe that only fires when something
 * moves cannot tell you that nothing is moving. */
static void shg_pad_log(void) {
    static DWORD s_last = 0;
    if (!g_padst[0].present) return;
    SLOG_RATE(s_last,
              "PADRAW: lx=%6d ly=%6d (norm %.3f/%.3f, dz=0.24 -> %s/%s) "
              "lt=%3u rt=%3u btn=%04X  aim=(%.3f,%.3f) owns=%d",
              (int)g_padst[0].lx, (int)g_padst[0].ly,
              (double)g_padst[0].lx / 32767.0, (double)g_padst[0].ly / 32767.0,
              (g_padst[0].lx > 7864 || g_padst[0].lx < -7864) ? "LIVE" : "dead",
              (g_padst[0].ly > 7864 || g_padst[0].ly < -7864) ? "LIVE" : "dead",
              (unsigned)g_padst[0].lt, (unsigned)g_padst[0].rt,
              (unsigned)g_padst[0].buttons,
              (double)g_paim_x, (double)g_paim_y, g_pad_owns_aim);
}

/* ── update (called every game frame) ───────────────────────────────────── */
__declspec(dllexport) int shaiolib_update(void) {
    static int s_upd_once = 0;
    static DWORD s_upd_n = 0;
    if (!s_upd_once) {
        s_upd_once = 1;
        /* Backstop only. initialize() normally records this ~900 ms earlier;
         * do NOT overwrite it here, or a game that drives update() from a
         * worker would relabel its own main thread and re-arm the bug this
         * pair of variables exists to prevent. */
        if (!s_game_thread_id) s_game_thread_id = GetCurrentThreadId();
        SLOG("update: FIRST CALL tid=%lu (game main thread on record: %lu)",
             (unsigned long)GetCurrentThreadId(), (unsigned long)s_game_thread_id);
        shg_arm_life_watchdog();
        /* Started here, not in DllMain: the game window exists by now, and
         * CreateWindowEx/RegisterClass under the loader lock would be a
         * deadlock waiting to happen. */
        shg_xhair_start();
    }
    s_upd_n++;
    /* Pulse every 60 frames (~1s at 60fps) so we can confirm update() runs after VEH. */
    if (s_upd_n % 60 == 0) SLOG("update: pulse n=%lu tid=%lu", (unsigned long)s_upd_n, (unsigned long)GetCurrentThreadId());
    /* ONE pad read per frame, shared by every consumer below. This must run
     * before anything that reads g_padst, and the coin edge detector directly
     * following is the first of them. */
    shg_pad_tick();
    shg_pad_aim_tick();
    shg_pad_log();
    shg_snd_probe_tick();   /* one-shot once the game's SoundManager exists */

    /* Edge-detect coin keys so one press = one coin */
    /* Honor the mech: while the game has coin acceptance switched off (in the
     * service menu, for instance) a key press must NOT create a credit. */
    /* COIN IS SELECT/BACK AND NOTHING ELSE. No face button, trigger or stick
     * can insert a credit -- a pad that drifts, or a stray press during play,
     * must never cost the player money. The OR sits at the boolean, so the
     * existing one-press-one-coin edge detection below covers the pad free. */
    /* LATCH THE BUTTON, GATE THE CREDIT -- IN THAT ORDER.
     *
     * This used to fold g_coin_accept into the value that gets latched, which
     * manufactures a coin out of nothing: hold the coin button while acceptance
     * is OFF and the latch stays 0; when acceptance comes back ON the very next
     * poll sees 1-with-latch-0 and calls that a fresh press. Nobody touched the
     * button.
     *
     * That window is live on exactly the route the owner uses -- acceptance
     * toggles on entering and leaving the service menu, and the 17:44 log shows
     * it happening twice ([67843] DISABLED, [89671] ENABLED) either side of a
     * TEST-menu visit. It is the best explanation on the table for credits that
     * appear when SERVICE is pressed.
     *
     * Now the latch tracks the physical button whatever the mech is doing, and
     * acceptance only decides whether a real edge becomes a credit. */
    int r0 = ((GetAsyncKeyState('5') & 0x8000) != 0) || shg_pad_btn(0, SHG_XI_BACK);
    int r1 = ((GetAsyncKeyState('6') & 0x8000) != 0) || shg_pad_btn(1, SHG_XI_BACK);
    if (r0 && !g_coin_held[0] && g_coin_accept) g_coins[0]++;
    if (r1 && !g_coin_held[1] && g_coin_accept) g_coins[1]++;
    g_coin_held[0] = r0;
    g_coin_held[1] = r1;
    /* Count frames after security clears.  After 300 frames (~5 s at 60 fps)
     * enable gun + trigger so the game is past all boot/test screens before
     * any cursor position or click can drive phantom trigger=ON or DOWN. */
    if (g_io_ready && !g_io_enabled) {
        if (++g_post_sec_frames >= 300) {
            g_io_enabled = 1;
            SLOG("I/O enabled (300 frames post-security)");
        }
    }

    /* SHM-PROBE: READ-ONLY, ONE SHOT, WIRED TO NOTHING.
     *
     * This settles by measurement the claim the comment above g_padst makes
     * from source alone: that the loader's published input block is not
     * reachable from inside this process. At ~10 s the loader's pump has had
     * ample time to be writing.
     *
     * Read the name it prints, not the theory. If it comes back as
     * UltraLoaderIO_<pid>_<n> with a frozen sequence counter, the module
     * overwrote the environment variable and nothing writes that block, and
     * XInput above is the only honest source. If it comes back as
     * UltraIO_<pid> with a MOVING counter, the reasoning was wrong and reading
     * the loader's block becomes the better answer.
     *
     * It deliberately feeds nothing this run. A probe that also changed
     * behavior would not be a probe. */
    if (s_upd_n == 600) {
        char nm[128];
        DWORD n = GetEnvironmentVariableA("ULTRA_IO_SHM", nm, (DWORD)sizeof nm);
        if (!n || n >= sizeof nm) {
            SLOG("SHM-PROBE: ULTRA_IO_SHM is unset inside the game process");
        } else {
            HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, nm);
            if (!m) {
                SLOG("SHM-PROBE: name=%s OpenFileMapping failed err=%lu",
                     nm, (unsigned long)GetLastError());
            } else {
                const volatile unsigned *q = (const volatile unsigned *)
                    MapViewOfFile(m, FILE_MAP_READ, 0, 0, 64);
                if (!q) {
                    SLOG("SHM-PROBE: name=%s MapViewOfFile failed err=%lu",
                         nm, (unsigned long)GetLastError());
                } else {
                    unsigned magic = q[0], seq1 = q[2];
                    Sleep(200);
                    SLOG("SHM-PROBE: name=%s magic=0x%08X seq %u -> %u  => %s",
                         nm, magic, seq1, q[2],
                         (q[2] != seq1) ? "LIVE WRITER" : "NO WRITER (block is frozen)");
                    UnmapViewOfFile((LPCVOID)q);
                }
                CloseHandle(m);
            }
        }
    }
    return 0;
}

/* ── coin ───────────────────────────────────────────────────────────────── */
/* COIN_count TAKES AN OUT-POINTER, NOT A SLOT INDEX. THIS IS THE BEEP.
 *
 * KSHG.exe calls it like this, read from the image:
 *     0x004D1213  lea eax,[esp+0xc]      ; address of a local
 *                 push eax
 *                 call DWORD PTR [0x6804DC]   ; shaiolib_COIN_count
 *     0x004D1224  test that local             ; -> plays
 *                                             ;    ../Sound/SE/SE_SYS_COIN.pcm
 *
 * This function used to be declared (int slot) and opened with
 *     if (slot < 0 || slot > 1) return 0;
 * A stack address such as 0x0019FD08 is comfortably greater than 1, so EVERY
 * call was rejected at the first line and the out-param was never written. The
 * game then tested whatever happened to be on its stack and decided, on that
 * garbage, to play the coin sound -- over and over. That is the continuous
 * beeping, and it is why it starts at the title screen (where the credit
 * display lives) and is silent on the boot screen.
 *
 * The guard is also why no probe ever saw this: the SLOG_RATE below is
 * unconditional, yet "COIN" appears zero times in the log. Every call was
 * discarded before it could announce itself.
 *
 * THIRD INSTANCE OF THE STUB-ABI TRAP TODAY, after shg_EaxManagerCreate's
 * ret-immediate and boot_trace.c's WINAPI-vs-__cdecl shims. Recover a stub's
 * true signature from the CALLER's pushes, never from a plausible-looking name.
 * A parameter named "slot" that the caller fills with a pointer is not a slot. */
__declspec(dllexport) int shaiolib_COIN_count(int *out) {
    static DWORD s_coin_last = 0;
    int slot = 0;                      /* the game asks about P1 at this site */

    /* NO AUTO-INSERT. Removed 2026-09-17.
     *
     * A single P1 coin used to be granted ~18 s in, so attract mode could reach
     * gameplay back when none of the coin path worked. It has outlived its
     * purpose: the poll pair below is correct now, key 5 credits properly, and
     * the game's own free-play setting decides whether a coin is needed at all.
     * Granting a phantom credit on top of that is the loader inventing money the
     * operator did not put in.
     *
     * It also had a side effect worth not reintroducing: it set g_coin_held[0]=1
     * to stop itself repeating, which is the same edge-detect latch the real key
     * 5 insert uses. */

    /* REPORT NEW COINS SINCE THE LAST POLL -- A DELTA, NOT THE RUNNING TOTAL.
     * The game keeps the total itself. Its poll is a tight pair:
     *     4d1218  call COIN_count(&local)   ; how many new coins
     *     4d121f  call COIN_consume(local)  ; consume exactly those
     *     4d1224  cmp [esp+0xc],0 / je skip ; zero -> nothing happened
     *     4d123f  add edx,[esp+0xc]         ; TOTAL += count
     *     4d1246  mov [0x6E3A00],edx
     * so whatever we report here is ADDED to the credit total every poll.
     * Reporting the running total made the game re-add it on each poll and the
     * counter ran away to 99. g_coins[] is the PENDING count; COIN_consume
     * drains it, so a single insert is reported once and then reads zero. */
    if (out && (ULONG_PTR)out > 0x10000u && !IsBadWritePtr(out, sizeof(int)))
        *out = g_coins[slot];

    if (g_coins[slot])
        SLOG("COIN_count: reporting %d new coin(s) to the game", g_coins[slot]);
    return g_coins[slot];
}
/* COIN_countup IS AN ENABLE FLAG, NOT A COIN EVENT AND NOT A SLOT INDEX.
 *
 * This used to do `if (slot >= 0 && slot <= 1) g_coins[slot]++;` on the
 * assumption that the game calls it when a coin passes the sensor. It does not.
 * KSHG 0x004D0D70 is a coin-acceptance toggle and this is its only caller:
 *
 *     4d0d71  mov bl,[esp+8]        ; the argument is a FLAG
 *     4d0d75  test bl,bl / je OFF
 *   ON:  push 1 / call [0x680530]   ; COIN_countup(1)
 *             call [0x6804F8]       ; COIN_blocker_open
 *   OFF:      call [0x6804FC]       ; COIN_blocker_close
 *        push 0 / call [0x680530]   ; COIN_countup(0)
 *
 * (IAT names verified: 0x680530 COIN_countup, 0x6804F8 blocker_open,
 *  0x6804FC blocker_close.)
 *
 * So 1 means "start accepting coins" and 0 means "stop" -- and the old code
 * credited a coin for BOTH, because 0 and 1 are both valid slot indices. Every
 * entry to and exit from the service menu toggles acceptance, so each visit
 * added credits, and the game played its coin sound for each one. That is the
 * beeping the owner heard return on leaving the service menu, and the reason
 * the counter had climbed to 37 credits nobody inserted.
 *
 * Fourth instance of the stub-ABI trap on this board. */
__declspec(dllexport) int shaiolib_COIN_countup(int enable) {
    int on = enable ? 1 : 0;
    if (on != g_coin_accept) {
        g_coin_accept = on;
        SLOG("COIN_countup: coin acceptance %s", on ? "ENABLED" : "DISABLED");
    }
    return 0;
}
/* ONE ARGUMENT: the number of coins to consume. Not a slot index.
 *
 * The call site at 0x004D121E pushes exactly one value -- the count COIN_count
 * just reported -- and the shared `add esp,8` at 0x004D1221 cleans that push
 * and COIN_count's together. Declaring (int slot, int n) made `n` read the
 * caller's stack, so `g_coins[slot] -= n` subtracted garbage; a negative one
 * ADDS credits, which is the other half of the runaway.
 *
 * This is the pair that drains the pending count, so a coin is reported to the
 * game exactly once. */
__declspec(dllexport) int shaiolib_COIN_consume(int n) {
    if (n > 0) {
        g_coins[0] -= n;
        if (g_coins[0] < 0) g_coins[0] = 0;
        SLOG("COIN_consume: game took %d coin(s), %d pending", n, g_coins[0]);
    }
    return 0;
}
__declspec(dllexport) void shaiolib_COIN_blocker_open (int slot) { (void)slot; }
__declspec(dllexport) void shaiolib_COIN_blocker_close(int slot) { (void)slot; }

/* ── buttons ────────────────────────────────────────────────────────────── */
/*
 * Real signature (confirmed from KSHG.exe disassembly):
 *   int shaiolib_BUTTON_status(void *buf)  - buf = 12 signed bytes
 *
 * Each byte encodes one button:
 *   0x00 = idle
 *   0x01 = RELEASE edge  (positive, bit 0)  -> sets [0x6E398C] released
 *   0x80 = HELD, no new press (negative, bit 0 CLEAR) -> [0x6E3988] only
 *   0x81 = PRESS edge (negative + bit 0) -> [0x6E3990] pressed AND held
 *
 * Read from the consumer at 0x4D0700, not guessed: for each byte it does
 *   test al,al / jge positive
 *     test dl,al / je skip ; or [0x6E3990],edx   (PRESSED, only if bit0)
 *   skip: or [0x6E3988],edx                      (HELD, always if al<0)
 *   positive: test dl,al / or [0x6E398C],edx     (RELEASED)
 * and buf[N] carries bit (1<<N). An earlier version of this comment said
 * 0x01 meant "held"; it is a RELEASE, and believing it made every held key
 * fire a release every frame.
 *
 * Byte → game output bit mapping (observed from KSHG.exe):
 *   buf[0] → bit 0  (Start P1 / some action)
 *   buf[1] → bit 1  (Start P2 / fire)
 *   buf[2] → bit 2
 *   buf[3] → bit 3
 *   buf[4] → bit 4
 *   buf[5] → bit 6  (note: bit 5 skipped)
 *   buf[6] → bit 7
 */
__declspec(dllexport) int shaiolib_BUTTON_status(void *out) {
    if ((size_t)out < 0x10000u) return 0;  /* guard against NULL / near-null */
    int8_t *buf = (int8_t *)out;
    memset(buf, 0, 12);
    /* EDGE-DETECTED, AND THIS IS THE ROOT CAUSE OF THE "TURBO" AND THE BEEPING.
     *
     * The encoding is documented at the top of this function: 0x00 released,
     * 0x01 HELD, 0x81 NEWLY PRESSED. The old macro was
     *     #define BTN(vk) ((GetAsyncKeyState(vk) & 0x8000) ? 0x81 : 0x00)
     * so a key that was merely held reported NEWLY PRESSED on every single
     * frame. The game therefore saw a brand-new press ~60 times a second:
     *   - holding a direction flipped through menu items far too fast
     *   - the COIN slot generated a fresh credit event every frame, which is
     *     the continuous beeping. Silencing the coin SOUND only hid that;
     *     switching the cabinet off free play brought it straight back.
     * Now: 0x81 on the leading edge only, 0x01 while the key stays down.
     *
     * THE SLOT ORDER IS ALSO CORRECTED. The previous mapping was inferred and
     * wrong; this one is measured, from the game's own I/O CHECK page with one
     * key pressed at a time:
     *     old buf[0] (key 1)  showed up as TEST          -> slot 0 is TEST
     *     old buf[1] (key 2)  showed up as SERVICE       -> slot 1 is SERVICE
     *     old buf[2] (key F2) showed up as COIN MECH     -> slot 2 is COIN
     *     old buf[3] (key 9)  showed up as P1 START      -> slot 3 is P1 START
     *     old buf[4] (key 5)  showed up as P2 START      -> slot 4 is P2 START
     *     old buf[5] (key 6)  showed up as P1 OPTION L   -> slot 5 is P1 OPT L
     * Keys are now assigned so the arcade convention holds: 1/2 start, 5 coin,
     * F2 test, 9 service.
     *
     * SLOTS 6, 7 AND 8 ARE NOW NAMED, AND 9..11 NEVER EXISTED. An earlier note
     * here said slots 6..11 were "not yet located". Three of them are located,
     * and the other three have no consumer at all: the routine at 0x004D0700
     * RETURNS at 0x4D087D/0x4D088C having read only nine bytes. They were never
     * going to be found by pressing keys, because nothing reads them.
     *
     * The three new names come from the I/O CHECK page's own mask tables, which
     * are indexed by player:
     *     0x6CF338 START    = {0x008, 0x010}
     *     0x6CF330 OPTION L = {0x040, 0x200}
     *     0x6CF328 OPTION R = {0x080, 0x400}
     * The START and OPTION L rows reproduce the owner's one-key-at-a-time
     * measurement exactly, and that agreement is what licenses reading the
     * remaining three the same way. Final layout:
     *     buf[0] TEST         buf[1] SERVICE      buf[2] COIN MECH
     *     buf[3] P1 START     buf[4] P2 START     buf[5] P1 OPTION L
     *     buf[6] P1 OPTION R  buf[7] P2 OPTION L  buf[8] P2 OPTION R
     *
     * Bits 0x020 and 0x100 are skipped on purpose: those are the two GUN
     * triggers, set by the gun consumer at 0x004D1440 from mask table
     * 0x6CE7E4, not by this function. */
    {
        static unsigned char s_prev[12];
        int i;
        static const int keymap[9] = {
            VK_F2,   /* slot 0: TEST BUTTON      */
            '9',     /* slot 1: SERVICE BUTTON   */
            '5',     /* slot 2: COIN MECH        */
            '1',     /* slot 3: PLAYER1 START    */
            '2',     /* slot 4: PLAYER2 START    */
            '6',     /* slot 5: PLAYER1 OPTION L */
            '7',     /* slot 6: PLAYER1 OPTION R */
            '8',     /* slot 7: PLAYER2 OPTION L */
            '0',     /* slot 8: PLAYER2 OPTION R */
        };
        /* UNIVERSAL ACROSS EVERY BOARD IN THIS PROJECT: TEST = L3, SERVICE =
         * R3. The generic input profile puts COIN on 0x0040, which is L3 and
         * would collide head-on with TEST, so coin lives on BACK here -- and
         * only on BACK. */
        static const struct { int pad; WORD mask; } padmap[9] = {
            { 0, SHG_XI_L3    },   /* slot 0: TEST BUTTON      */
            { 0, SHG_XI_R3    },   /* slot 1: SERVICE BUTTON   */
            { 0, SHG_XI_BACK  },   /* slot 2: COIN MECH        */
            { 0, SHG_XI_START },   /* slot 3: PLAYER1 START    */
            { 1, SHG_XI_START },   /* slot 4: PLAYER2 START    */
            { 0, SHG_XI_LB    },   /* slot 5: PLAYER1 OPTION L */
            { 0, SHG_XI_RB    },   /* slot 6: PLAYER1 OPTION R */
            { 1, SHG_XI_LB    },   /* slot 7: PLAYER2 OPTION L */
            { 1, SHG_XI_RB    },   /* slot 8: PLAYER2 OPTION R */
        };
        for (i = 0; i < 9; ++i) {
            /* OR AT THE BOOLEAN, NOT AT THE BYTE. Keyboard and pad merge into
             * one "is this button down" answer, and the edge detector below
             * then produces the 0x81/0x80/0x01/0x00 protocol for the pad for
             * free -- exactly as it already does for the keyboard. Merging the
             * encoded bytes instead would double-report presses. The keyboard
             * is never replaced, only joined. */
            int down = (GetAsyncKeyState(keymap[i]) & 0x8000) ? 1 : 0;
            if (!down) down = shg_pad_btn(padmap[i].pad, padmap[i].mask);
            /* 0x81 press edge | 0x80 held | 0x01 release edge | 0x00 idle */
            buf[i] = down ? (s_prev[i] ? (int8_t)0x80 : (int8_t)0x81)
                          : (s_prev[i] ? (int8_t)0x01 : (int8_t)0x00);
            s_prev[i] = (unsigned char)down;
        }
    }
    SLOG_RATE(s_btn_last,
              "BUTTON: [0]=%02X [1]=%02X [2]=%02X [3]=%02X [4]=%02X [5]=%02X "
              "[6]=%02X [7]=%02X [8]=%02X",
              (uint8_t)buf[0],(uint8_t)buf[1],(uint8_t)buf[2],
              (uint8_t)buf[3],(uint8_t)buf[4],(uint8_t)buf[5],
              (uint8_t)buf[6],(uint8_t)buf[7],(uint8_t)buf[8]);
    return 1;  /* 1 = buttons available */
}

/* ── lightgun ───────────────────────────────────────────────────────────── */
/*
 * Real signature (confirmed from KSHG.exe disassembly):
 *   int shaiolib_GUN_status(void *buf)  - buf = 32 bytes (2 players × 16 bytes)
 *
 * Per-player entry (16 bytes) - layout confirmed from KSHG.exe disassembly:
 *   +0x00  int32_t x        - horizontal screen pixel coordinate
 *   +0x04  int32_t y        - vertical screen pixel coordinate
 *   +0x08  int32_t trigger  - non-zero = gun FIRED/trigger pressed  ← game checks HERE first
 *   +0x0C  int32_t status   - 0 = normal; non-zero = gun error/unavailable
 *
 * CRITICAL: offset +0x08 is the PRIMARY fire signal.  The game checks it before
 * anything else.  Setting it to 1 (even to mean "gun connected") causes the
 * game to treat every frame as a trigger press, driving TRIGGER permanently ON
 * and scrolling the service menu non-stop.  Keep it 0 when not actually firing.
 *
 * Returns: non-zero = success (gun subsystem OK), 0 = no guns.
 */
typedef struct { int32_t x, y, trigger, status; } SHGA_GUN_ENTRY;
typedef struct { SHGA_GUN_ENTRY player[2]; }          SHGA_GUN_BUF;

/* Small env reader: this DLL is injected into a game, so every knob has to be
 * settable from the game's container without a rebuild. */
static int shg_env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return atoi(v);
}

/* PLAYER 2'S GUN, FROM A PAD.
 *
 * The game reads both players out of one buffer with one code path, so P2 needs
 * nothing new on the game's side - only a source. A second physical lightgun is
 * the real answer; an XInput pad's left stick is a complete stand-in that makes
 * two-player play testable today, and it is how the Taito Type X+ board's
 * two-gun mode is exercised as well.
 *
 * The stick is ABSOLUTE here, not a velocity: center of the stick is the center
 * of the screen. That is what a gun does, and it means no cursor and no warping
 * for player 2 at all. Returns 0 when no pad is connected, leaving P2 zeroed
 * exactly as before. */
/* typedefs for these now live beside g_padst, above update(). */

static int shg_pad_aim(int slot, int gw, int gh, int *ox, int *oy, int *otrig) {
    shg_pXInputGetState s_get = shg_xinput();
    shg_xstate st;
    if (!s_get) return 0;
    memset(&st, 0, sizeof st);
    if (s_get((DWORD)slot, &st) != 0) return 0;      /* not connected */
    {
        int x = (int)(((int64_t)st.pad.lx + 32768) * gw / 65536);
        int y = (int)(((int64_t)(-st.pad.ly) + 32767) * gh / 65536);  /* screen Y is down */
        if (x < 0) x = 0;
        if (x > gw - 1) x = gw - 1;
        if (y < 0) y = 0;
        if (y > gh - 1) y = gh - 1;
        *ox = x;
        *oy = y;
        *otrig = (st.pad.bRT > 30 || (st.pad.wButtons & 0x1000)) ? 1 : 0;   /* RT or A */
    }
    return 1;
}

/* ── CROSSHAIR: A LAYERED WINDOW, NOT A DRAW CALL ─────────────────────────
 *
 * THE THIRD APPROACH, AND THE REASON THE FIRST TWO DIED.
 *
 * Attempt 1 drew it in this file's own Present hook. It never executed once.
 * Attempt 2 moved it to the board hook's D3D9 Present. That file is not even
 * compiled into this board's hook -- konami_pc builds from HOOK_SRC, which does
 * not list d3d9_throttle.c.
 *
 * The 17:10 run settles why, and it is worth stating plainly because it is not
 * what anyone assumed: THERE IS NO D3D9 HOOK IN THIS PROCESS AT ALL. Isolating
 * the last run in the (appending) board log shows only [konami] render9 lines --
 * no "D3D9: Direct3DCreate9", no "CreateDevice", no "device created ->
 * Present+Reset hooked". Earlier runs in the same file DO show them, which is
 * exactly how two hours went into a device that this build never had. And this
 * file's own bootstrap says so directly:
 *     render-thread: D3D9 bootstrap FAILED 3 times -- giving up
 *     render-thread: D3D device not ready after 120s, aborting
 * KSHG renders entirely by itself, through a device nobody holds a pointer to.
 *
 * So: draw OUTSIDE the game. A layered, click-through, topmost window per
 * player, moved to follow the aim. It needs no device, wins no races, and
 * cannot corrupt a render path -- it is not in one.
 *
 * WHY IT IS CHEAP. The window is only as big as the crosshair (tens of pixels),
 * not the screen, so UpdateLayeredWindow copies a tiny bitmap. Moving a small
 * window is far cheaper than recompositing a 1920x1080 surface every frame.
 *
 * WHY IT CANNOT STEAL INPUT. WS_EX_TRANSPARENT makes it click-through,
 * WS_EX_NOACTIVATE stops it ever taking focus, and WS_EX_TOOLWINDOW keeps it out
 * of the task bar and Alt-Tab. It is never a target for anything.
 */
#define SHG_XH_MAXPX 192           /* window edge; bounds the bitmap we build */

typedef struct {
    HWND   wnd;
    HDC    mem;
    HBITMAP bmp, oldbmp;
    void  *bits;
    int    size;                   /* current window edge, px */
    int    shown;
} shg_xh_win;

static shg_xh_win g_xhw[2];
static volatile LONG g_xh_thread_started = 0;

static int shg_xhair_enabled(void) {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = shg_env_int("ULTRA_SHG_CROSSHAIR", 1);
        SLOG("XHAIR: %s", s_on ? "on (ULTRA_SHG_CROSSHAIR=0 to disable)" : "off");
    }
    return s_on;
}

static LRESULT CALLBACK shg_xh_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   /* belt and braces with WS_EX_TRANSPARENT */
    return DefWindowProcA(h, m, w, l);
}

/* Build (or rebuild, on a size change) one player's layered window. */
static int shg_xh_make(int p, int size) {
    static int s_cls = 0;
    shg_xh_win *w = &g_xhw[p];
    BITMAPINFO bi;

    if (size < 8) size = 8;
    if (size > SHG_XH_MAXPX) size = SHG_XH_MAXPX;
    if (w->wnd && w->size == size) return 1;

    if (!s_cls) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = shg_xh_proc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = NULL;
        wc.lpszClassName = "UltraShgCrosshair";
        RegisterClassA(&wc);
        s_cls = 1;
    }
    if (!w->wnd) {
        w->wnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "UltraShgCrosshair", "", WS_POPUP,
            0, 0, size, size, NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!w->wnd) {
            SLOG("XHAIR: CreateWindowEx failed err=%lu -- no crosshair for player %d",
                 (unsigned long)GetLastError(), p);
            return 0;
        }
        SLOG("XHAIR: player %d overlay created (%dpx, click-through, no-activate)", p, size);
    }
    if (w->mem) {
        SelectObject(w->mem, w->oldbmp);
        DeleteObject(w->bmp);
        DeleteDC(w->mem);
        w->mem = NULL; w->bmp = NULL; w->bits = NULL;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;            /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    {
        HDC scr = GetDC(NULL);
        w->mem = CreateCompatibleDC(scr);
        ReleaseDC(NULL, scr);
    }
    if (!w->mem) return 0;
    w->bmp = CreateDIBSection(w->mem, &bi, DIB_RGB_COLORS, &w->bits, NULL, 0);
    if (!w->bmp) { DeleteDC(w->mem); w->mem = NULL; return 0; }
    w->oldbmp = (HBITMAP)SelectObject(w->mem, w->bmp);
    w->size = size;
    return 1;
}

/* Premultiplied BGRA, which is what UpdateLayeredWindow expects. Full alpha on
 * the strokes, zero everywhere else -- so the window is genuinely invisible
 * except for the crosshair itself, not a faint square. */
static void shg_xh_paint(int p, int size, int arm, int th, DWORD argb) {
    shg_xh_win *w = &g_xhw[p];
    unsigned char *px = (unsigned char *)w->bits;
    int c = size / 2, gap = arm / 3, x, y;
    unsigned char A = (unsigned char)((argb >> 24) & 0xFF);
    unsigned char R = (unsigned char)((argb >> 16) & 0xFF);
    unsigned char G = (unsigned char)((argb >>  8) & 0xFF);
    unsigned char B = (unsigned char)( argb        & 0xFF);
    if (!px) return;
    if (!A) A = 255;
    /* premultiply */
    R = (unsigned char)((R * A) / 255);
    G = (unsigned char)((G * A) / 255);
    B = (unsigned char)((B * A) / 255);

    memset(px, 0, (size_t)size * size * 4);
    for (y = 0; y < size; ++y) {
        int dy = y - c;
        for (x = 0; x < size; ++x) {
            int dx = x - c, on = 0;
            if (dy > -th && dy < th) {           /* horizontal arms */
                int ax = dx < 0 ? -dx : dx;
                if (ax >= gap && ax <= arm) on = 1;
            }
            if (dx > -th && dx < th) {           /* vertical arms */
                int ay = dy < 0 ? -dy : dy;
                if (ay >= gap && ay <= arm) on = 1;
            }
            if (on) {
                unsigned char *q = px + ((size_t)y * size + x) * 4;
                q[0] = B; q[1] = G; q[2] = R; q[3] = A;
            }
        }
    }
}

static void shg_xh_hide(int p) {
    if (g_xhw[p].wnd && g_xhw[p].shown) {
        ShowWindow(g_xhw[p].wnd, SW_HIDE);
        g_xhw[p].shown = 0;
    }
}

/* ── THE CROSSHAIR MUST SIT WHERE THE GAME AIMS, NOT WHERE WE AIMED ───────
 *
 * The overlay's placement arithmetic was already centered and is NOT the bug.
 * Measured offline against the real constants: arm=12 th=2 gives a 32px window
 * (the log agrees -- "player 0 overlay created (32px)"), the painted glyph's
 * bounding box is x=[4..28] y=[4..28] so its center is bitmap pixel (16,16) =
 * size/2, and `- g_xhw[p].size / 2` puts that pixel exactly on the aim point.
 * Subtracting another half-size would have MOVED A CORRECT CROSSHAIR.
 *
 * THE REAL OFFSET IS THE GAME'S OWN CALIBRATION. shg_write_gun_cal() below
 * ships the real 2012 cabinet blob, and AIM_store (0x4D12E0) runs every reading
 * we hand it through a per-player piecewise-linear transform built from that
 * blob, against the screen anchors in .data (0x6CE7DC=320, 0x6CE7E0=240, half
 * spans 272 and 192):
 *
 *     x <  cx :  out = 320 - (cx-x)*272/leftSpan
 *     x >= cx :  out = 320 + (x-cx)*272/rightSpan
 *     y <  cy :  out = 240 - (cy-y)*192/upSpan
 *     y >= cy :  out = 240 + (y-cy)*192/downSpan
 *
 * So the game's own reticle lands at out(), while this overlay was drawn at the
 * raw value we posted -- the two are never on top of each other. P1's blob is
 * cx=356 cy=237 left=276 up=190 right=270 down=180, which gives, measured:
 *
 *     x: 0->-30   106->74   212->179   318->283   424->388   530->495   636->602
 *     y: 0->1     95->97    190->193   285->291   380->392   475->493
 *
 * i.e. about 35 px LEFT everywhere in x, and +1 at the top growing to +18 at the
 * bottom in y. That is the "slightly off" -- and because it VARIES with position
 * (-30..-36 in x, +1..+18 in y) a constant nudge cannot fix it; the whole
 * transform has to be applied. The file's own recorded observation confirms the
 * decode: it records "our x=202 came out at 168", and this formula gives 169.
 *
 * WE DO NOT TOUCH THE COORDINATES THE GAME IS GIVEN. Aim itself is correct and
 * this game is locked; only the drawing of the marker moves, onto the place the
 * shot actually goes.
 *
 * CONSTANTS COME FROM THE FILE ON DISK, not from the blob literal, because
 * shg_write_gun_cal() deliberately leaves a genuine hardware calibration alone
 * -- if one is ever present the crosshair must follow it too. Any failure falls
 * back to the identity, which is exactly today's behavior.
 *
 * ULTRA_SHG_XHAIR_CAL=0 reverts to drawing the raw value, from the container,
 * with no rebuild. */
static volatile LONG g_xhcal_state = 0;      /* 0 = unread, 1 = loaded, 2 = identity */
static int           g_xhcal[2][6];          /* cx, cy, left, up, right, down */

static void shg_xh_cal_load(void) {
    unsigned char buf[54];
    char exepath[MAX_PATH], path[MAX_PATH], *last;
    FILE *f;
    int p, i;

    if (g_xhcal_state) return;
    /* DEFAULT 0, DELIBERATELY. Moving the crosshair onto the game's calibrated
     * aim point is the WRONG half of this problem, and shipping it on would
     * have made the reported defect worse rather than better.
     *
     * The owner's report was "the cursor is slightly off, the cursor needs to
     * be centered in the crosshairs" -- i.e. the OS pointer and our crosshair
     * disagree. The gun.1 on disk (cx=356, spans 276/272) bends the game's aim
     * about 36 px LEFT of the raw pointer at 640x480. Drawing the crosshair at
     * the bent position therefore does not unite them: it moves our marker 36 px
     * AWAY from the pointer the owner is looking at, turning a slight offset
     * into an obvious one.
     *
     * The distortion itself is what needs removing, not tracking. KSHG_no_cursor.exe,
     * which ships in this same dump, does exactly that: it NOPs the `jne` at
     * 0x4D1303 that selects the calibrated store, forcing the verbatim one at
     * 0x4D1305, so the game records the coordinates we post and aim, pointer and
     * crosshair all land on one pixel. (That build also blanks three HASP
     * strings and alters a SetCursor argument, so it is a crack, not a clean
     * reference -- only the one branch is worth copying, and only after the
     * owner agrees to touch a locked game's aim path.)
     *
     * So: the machinery below stays, fully working, and stays OFF. Set
     * ULTRA_SHG_XHAIR_CAL=1 in the container to try it with no rebuild. */
    if (!shg_env_int("ULTRA_SHG_XHAIR_CAL", 0)) {
        g_xhcal_state = 2;
        SLOG("XHAIR: calibration mapping OFF (ULTRA_SHG_XHAIR_CAL=0) -- drawing raw aim");
        return;
    }
    GetModuleFileNameA(NULL, exepath, MAX_PATH);
    last = strrchr(exepath, '\\');
    if (last) *(last + 1) = '\0'; else { exepath[0] = '.'; exepath[1] = '\\'; exepath[2] = '\0'; }
    _snprintf(path, MAX_PATH, "%sData\\TestDat\\gun.1", exepath);
    f = fopen(path, "rb");
    if (!f) {
        g_xhcal_state = 2;
        SLOG("XHAIR: no %s -- crosshair drawn at raw aim (identity)", path);
        return;
    }
    if (fread(buf, 1, sizeof buf, f) != sizeof buf) {
        fclose(f);
        g_xhcal_state = 2;
        SLOG("XHAIR: gun.1 short read -- crosshair drawn at raw aim (identity)");
        return;
    }
    fclose(f);
    /* 4 header bytes, then 6 little-endian dwords per player, then the CRC-16
     * trailer: cx, cy, LEFT, UP, RIGHT, DOWN -- left/up BEFORE right/down. */
    for (p = 0; p < 2; ++p) {
        for (i = 0; i < 6; ++i) {
            int off = 4 + p * 24 + i * 4;
            g_xhcal[p][i] = (int)((unsigned)buf[off]
                                | ((unsigned)buf[off+1] <<  8)
                                | ((unsigned)buf[off+2] << 16)
                                | ((unsigned)buf[off+3] << 24));
        }
    }
    /* A zero span would divide by zero; anything that odd is not a calibration
     * we understand, so fall back rather than guess. */
    for (p = 0; p < 2; ++p)
        for (i = 2; i < 6; ++i)
            if (g_xhcal[p][i] <= 0) {
                g_xhcal_state = 2;
                SLOG("XHAIR: gun.1 span[%d][%d]=%d -- unusable, drawing raw aim",
                     p, i, g_xhcal[p][i]);
                return;
            }
    g_xhcal_state = 1;
    SLOG("XHAIR: gun.1 calibration loaded -- "
         "P1 cx=%d cy=%d L=%d U=%d R=%d D=%d / P2 cx=%d cy=%d L=%d U=%d R=%d D=%d",
         g_xhcal[0][0], g_xhcal[0][1], g_xhcal[0][2],
         g_xhcal[0][3], g_xhcal[0][4], g_xhcal[0][5],
         g_xhcal[1][0], g_xhcal[1][1], g_xhcal[1][2],
         g_xhcal[1][3], g_xhcal[1][4], g_xhcal[1][5]);
}

/* Map one player's posted aim into the screen position the game will use.
 * Both players go through this -- the blob carries a separate six-value set for
 * each, and P2's differs from P1's (cx=339 cy=225 vs 356/237). */
static void shg_xh_cal_apply(int p, int *x, int *y, int gw, int gh) {
    int cx, cy, ox, oy;
    shg_xh_cal_load();
    if (g_xhcal_state != 1) return;              /* identity = previous behavior */
    cx = g_xhcal[p][0]; cy = g_xhcal[p][1];
    ox = (*x <  cx) ? 320 - (cx - *x) * 272 / g_xhcal[p][2]
                    : 320 + (*x - cx) * 272 / g_xhcal[p][4];
    oy = (*y <  cy) ? 240 - (cy - *y) * 192 / g_xhcal[p][3]
                    : 240 + (*y - cy) * 192 / g_xhcal[p][5];
    /* Keep the overlay inside the game window, as the raw path already was --
     * the transform can run a little past both edges (P1 x=0 maps to -30). */
    if (ox < 0) ox = 0;
    if (ox > gw - 1) ox = gw - 1;
    if (oy < 0) oy = 0;
    if (oy > gh - 1) oy = gh - 1;
    *x = ox; *y = oy;
}

static DWORD WINAPI shg_xhair_thread(LPVOID unused) {
    (void)unused;
    SLOG("XHAIR: overlay thread started tid=%lu", (unsigned long)GetCurrentThreadId());
    for (;;) {
        HWND game = get_game_hwnd();
        RECT cr;
        POINT org;
        int p, cw, ch, gw, gh, arm, th, size;
        const char *e;
        DWORD col[2];

        Sleep(16);
        if (!shg_xhair_enabled()) { shg_xh_hide(0); shg_xh_hide(1); continue; }
        if (!game || !IsWindow(game) || IsIconic(game)) { shg_xh_hide(0); shg_xh_hide(1); continue; }

        GetClientRect(game, &cr);
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        if (cw < 16 || ch < 16) { shg_xh_hide(0); shg_xh_hide(1); continue; }
        org.x = 0; org.y = 0;
        ClientToScreen(game, &org);

        gw = shg_env_int("ULTRA_SHG_AIM_W", 640);
        gh = shg_env_int("ULTRA_SHG_AIM_H", 480);
        if (gw <= 0) gw = 640;
        if (gh <= 0) gh = 480;

        /* Sizes are given in the game's own 640x480 space and scaled up with the
         * window, so the crosshair keeps its apparent size at any output res. */
        arm = shg_env_int("ULTRA_SHG_CROSSHAIR_SIZE", 12);
        th  = shg_env_int("ULTRA_SHG_CROSSHAIR_THICK", 2);
        if (arm < 4) arm = 4;
        if (th  < 1) th  = 1;
        arm = arm * ch / gh; if (arm < 4) arm = 4;
        th  = th  * ch / gh; if (th  < 1) th  = 1;
        size = (arm + th + 2) * 2;
        if (size > SHG_XH_MAXPX) { size = SHG_XH_MAXPX; arm = size/2 - th - 2; }

        col[0] = (DWORD)shg_env_int("ULTRA_SHG_CROSSHAIR_COL1", 0);
        col[1] = (DWORD)shg_env_int("ULTRA_SHG_CROSSHAIR_COL2", 0);
        if (!col[0]) col[0] = 0xFFFF2828u;   /* P1 red  */
        if (!col[1]) col[1] = 0xFF28D8FFu;   /* P2 cyan */

        for (p = 0; p < 2; ++p) {
            int ax, ay, sx, sy;
            POINT ptSrc = {0, 0}, ptPos;
            SIZE  sz;
            BLENDFUNCTION bf;

            if (!g_xh_live[p]) { shg_xh_hide(p); continue; }
            if (!shg_xh_make(p, size)) continue;

            ax = (int)g_xh_x[p];
            ay = (int)g_xh_y[p];
            /* Draw where the shot goes, not where we posted it -- the game
             * re-maps every reading through its gun.1 calibration. Loop body is
             * shared, so BOTH players get this, each with its own constants. */
            shg_xh_cal_apply(p, &ax, &ay, gw, gh);
            sx = org.x + (int)((long long)ax * cw / gw) - g_xhw[p].size / 2;
            sy = org.y + (int)((long long)ay * ch / gh) - g_xhw[p].size / 2;

            shg_xh_paint(p, g_xhw[p].size, arm, th, col[p]);

            ptPos.x = sx; ptPos.y = sy;
            sz.cx = g_xhw[p].size; sz.cy = g_xhw[p].size;
            memset(&bf, 0, sizeof bf);
            bf.BlendOp = AC_SRC_OVER;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(g_xhw[p].wnd, NULL, &ptPos, &sz,
                                g_xhw[p].mem, &ptSrc, 0, &bf, ULW_ALPHA);
            if (!g_xhw[p].shown) {
                ShowWindow(g_xhw[p].wnd, SW_SHOWNOACTIVATE);
                g_xhw[p].shown = 1;
            }
            /* Stay above the game without ever taking focus from it. */
            SetWindowPos(g_xhw[p].wnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

static void shg_xhair_start(void) {
    if (InterlockedCompareExchange(&g_xh_thread_started, 1, 0) != 0) return;
    if (!shg_xhair_enabled()) return;
    {
        HANDLE h = CreateThread(NULL, 0, shg_xhair_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else SLOG("XHAIR: CreateThread failed err=%lu", (unsigned long)GetLastError());
    }
}

__declspec(dllexport) int shaiolib_GUN_status(void *out) {
    if ((size_t)out < 0x10000u) return 0;  /* guard against NULL / near-null */
    SHGA_GUN_BUF *buf = (SHGA_GUN_BUF *)out;
    memset(buf, 0, sizeof *buf);

    HWND hw = get_game_hwnd();

    /* Read cursor in screen space first (before any ScreenToClient conversion)
     * so we can do the movement-activation check against raw screen coords. */
    POINT screen_pt = {0};
    GetCursorPos(&screen_pt);

    /* Gun activation gate: suppress gun until the user deliberately moves the
     * mouse.  At game launch the cursor sits at whatever screen position it was
     * before (often the bottom-right corner of a 640×480 window).  Some Konami
     * service menus treat gun-Y as a relative scroll speed: Y > center → scroll
     * DOWN continuously → the initial corner position causes permanent cycling.
     * We hold available=0 until the cursor moves ≥4 pixels from where it was on
     * the very first GUN_status call. */
    if (g_gun_init_x == LONG_MIN) {
        g_gun_init_x = screen_pt.x;
        g_gun_init_y = screen_pt.y;
    }
    if (!g_gun_active) {
        LONG dx = screen_pt.x - g_gun_init_x;
        LONG dy = screen_pt.y - g_gun_init_y;
        if (dx*dx + dy*dy >= 16) {  /* 4 pixels radius */
            g_gun_active = 1;
            SLOG("GUN: activated (mouse moved %ld,%ld from initial position)",
                 (long)dx, (long)dy);
        }
    }

    /* LAST MOVER WINS. The mouse takes aim back the instant it moves, so a
     * player who reaches for the mouse mid-game is not fighting a stick that
     * still thinks it is in charge. */
    {
        /* A MOVE, NOT A DIFFERENCE. The first version compared the cursor against
         * the previous reading and handed aim back on any inequality at all.
         * That is not "the mouse moved", it is "the mouse is somewhere", and a
         * cursor sitting in the window's top-left corner kept reclaiming aim
         * from a stick that was actively being used: the 17:44 run logged 159
         * samples at x=0 y=0 owns=mouse interleaved with live pad aim, so the
         * crosshair was snapping to the corner and back several times a second.
         *
         * 3 pixels, squared, so a jitter or a one-pixel rounding wobble is not
         * mistaken for intent. A genuine reach for the mouse clears it instantly;
         * a resting cursor never does. */
        static POINT s_last_mouse = { LONG_MIN, LONG_MIN };
        if (s_last_mouse.x == LONG_MIN) {
            s_last_mouse = screen_pt;          /* first reading is a baseline, not a move */
        } else {
            LONG mdx = screen_pt.x - s_last_mouse.x;
            LONG mdy = screen_pt.y - s_last_mouse.y;
            if (mdx * mdx + mdy * mdy >= 9) {
                s_last_mouse = screen_pt;
                g_pad_owns_aim = 0;
            }
        }
    }

    /* THE STICK IS A SECOND ACTIVATOR, on the same principle as the 4-pixel
     * mouse gate above: only a deliberate movement counts. g_pad_owns_aim is
     * set in shg_pad_aim_tick() only past a 24% deflection, so a resting or
     * drifting stick can no more wake the gun than a resting cursor can. */
    if (!g_gun_active && g_pad_owns_aim) {
        g_gun_active = 1;
        SLOG("GUN: activated by pad stick");
    }

    if (!g_gun_active || !g_gun_en[0] || !g_io_enabled) {
        /* Keep edge-detect state current so we don't get a spurious trigger=1
         * on the very first frame the gun goes active.
         * IMPORTANT: when auto-LMB is active (calibration window) do NOT reset
         * g_trig_prev to raw GAKS here - that would cause the 10Key edge detector
         * to fire an extra 0x81 on every GUN_status call (GAKS=0 during auto-LMB
         * resets g_trig_prev→0, next 10Key call sees trig_cur=1 → infinite edges).
         * Instead, mirror the same auto-LMB adjusted state that 10Key uses. */
        {
            int _tc = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
            if (!_tc && g_auto_lmb_until && GetTickCount() < g_auto_lmb_until)
                _tc = 1;
            g_trig_prev = _tc;
        }
        /* SAY "UNAVAILABLE", DO NOT SAY "A GUN AT (0,0)".
         *
         * 2026-09-17: this branch used to return 1 without touching buf at
         * all. Per this file's own header, +0x0C status means "0 = normal;
         * non-zero = gun error/unavailable" -- so leaving the buffer alone
         * told the game it had a perfectly good gun pinned at the top-left
         * corner of the screen.
         *
         * The header also records what that costs, and it is exactly what the
         * owner reported as "it keeps pressing super fast like its held down":
         * Konami service menus read gun-Y as a RELATIVE scroll speed, so a
         * y pinned far above center scrolls continuously. The gun gate above
         * exists to prevent that from a corner cursor -- and then this branch
         * handed the game the worst possible corner anyway.
         *
         * Report the gun as unavailable instead. The game then ignores the
         * coordinates entirely rather than acting on a fake one. */
        g_xh_live[0] = g_xh_live[1] = 0;   /* no gun, no crosshair */
        SLOG_RATE(s_gun_last, "GUN: INACTIVE (waiting for mouse move) screen=(%d,%d) "
                  "start=(%ld,%ld) delta=(%ld,%ld) active=%d -- delta tells you whether the "
                  "cursor is moving at all",
                  (int)screen_pt.x, (int)screen_pt.y,
                  (long)g_gun_init_x, (long)g_gun_init_y,
                  (long)(screen_pt.x - g_gun_init_x),
                  (long)(screen_pt.y - g_gun_init_y), g_gun_active);
        return 1;
    }

    POINT pt = screen_pt;
    /* 640x480, not 800x480. This game's native aim space is a hardcoded
     * 640x480 (KSHG.exe creates its client area with `push 0x1E0; push 0x280`
     * at 0x4D048E and clamps aim at 0x280/0x1E0 at 0x4D139A/0x4D1425). There
     * is no 800x600 anywhere in the image; the old 800 was a guess. */
    RECT cr = {0, 0, 640, 480};   /* safe fallback = the game's native space */
    if (hw) {
        GetClientRect(hw, &cr);
        ScreenToClient(hw, &pt);
    }
    int32_t cw = cr.right  > 0 ? cr.right  : 800;
    int32_t ch = cr.bottom > 0 ? cr.bottom : 480;

    /* If the mouse cursor is OUTSIDE the game window, report gun as off-screen. */
    /* ...but only when the MOUSE is the thing aiming. The pad's aim is a
     * virtual position inside the game's own box and has nothing to do with
     * where the OS pointer happens to be sitting; without this guard a cursor
     * parked off the window would veto every pad frame. */
    int inside = (pt.x >= 0 && pt.x < cw && pt.y >= 0 && pt.y < ch);
    if (!inside && !g_pad_owns_aim) {
        /* trigger=0 from memset - gun off screen = no fire. */
        g_trig_prev = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        SLOG_RATE(s_gun_last, "GUN: OFF-SCREEN client=(%d,%d) cw=%d ch=%d",
                  (int)pt.x, (int)pt.y, (int)cw, (int)ch);
        return 1;
    }

    /* P1 - mouse cursor as gun.
     * Edge-detect the trigger: report 1 only on the FIRST frame LMB goes down.
     * Without this, holding LMB fires trigger=1 at 60fps - the service menu
     * interprets each poll as a separate "select" and cycles through all items
     * nonstop, making an unbearable selection noise. */
    int trig_cur = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    /* RT fires, on exactly the rule LMB already uses -- so whatever
     * ULTRA_SHG_AIM_ALWAYS does today, it keeps doing. 60/255 is past the
     * Xbox trigger's own resting noise. */
    if (!trig_cur && g_padst[0].present && g_padst[0].rt > 60) trig_cur = 1;

    /* SCALE INTO THE GAME'S OWN AIM SPACE. THIS IS THE "NO CONTROLS" BUG.
     *
     * The game's aim space is a hardcoded 640x480 and it takes whatever we put
     * here VERBATIM - it does not scale, and with no calibration file loaded it
     * does not even clamp. We were handing it client pixels of a window the
     * loader had resized to 1920x1080, so anything past x=640 or y=480 was
     * meaningless and aim only worked in the top-left corner of the screen.
     *
     * We own this writer, so the fix belongs here rather than confining the
     * cursor: scale the full client area onto 640x480 and the whole window
     * stays aimable. Origin is TOP-LEFT and Y increases DOWNWARD - the
     * opposite anchor to Gundam, which measures from the bottom. */
    {
        int32_t gw = shg_env_int("ULTRA_SHG_AIM_W", 640);
        int32_t gh = shg_env_int("ULTRA_SHG_AIM_H", 480);
        int32_t ax = (int32_t)(((int64_t)pt.x * gw) / (cw > 0 ? cw : gw));
        int32_t ay = (int32_t)(((int64_t)pt.y * gh) / (ch > 0 ? ch : gh));
        if (ax < 0) ax = 0;
        if (ax > gw - 1) ax = gw - 1;
        if (ay < 0) ay = 0;
        if (ay > gh - 1) ay = gh - 1;
        /* PAD OVERRIDE. g_paim_* is already normalized 0..1 of the aim box, so
         * it needs no client-rect scaling and is immune to the window size --
         * it cannot be stranded in a strip the way a cursor can. */
        if (g_pad_owns_aim && g_paim_x >= 0.0f) {
            ax = (int32_t)(g_paim_x * (float)(gw - 1));
            ay = (int32_t)(g_paim_y * (float)(gh - 1));
        }
        buf->player[0].x = ax;
        buf->player[0].y = ay;
    }

    /* THE AIM-REFRESH GATE.
     *
     * The game only stores aim on frames where this field is non-zero
     * (0x4D1464: `cmp dword ptr [edi-4],0 / je` skips AIM_store). We set it
     * only on the leading edge of LMB, so the player's aim was frozen between
     * clicks - it moved one frame per shot and sat still the rest of the time.
     *
     * Whether the field is "trigger" or "this reading is valid" is not settled
     * by the disassembly: the older edge-detect was added to stop a service
     * menu auto-repeating, but that may have been driven by a different,
     * latched word. Rather than guess, this is switchable and one playtest
     * settles it:
     *
     *   ULTRA_SHG_AIM_ALWAYS=1 (default) - held high while the gun is live, so
     *       aim tracks every frame. If the service menu starts auto-repeating,
     *       the old reading was right.
     *   ULTRA_SHG_AIM_ALWAYS=0           - the previous leading-edge behavior. */
    buf->player[0].trigger = shg_env_int("ULTRA_SHG_AIM_ALWAYS", 1)
                                 ? 1
                                 : ((trig_cur && !g_trig_prev) ? 1 : 0);
    buf->player[0].status  = 0;   /* 0 = normal; game processes coords + fires */
    g_trig_prev = trig_cur;
    /* Publish for the crosshair. Taken from buf, not from the source values, so
     * what is drawn is exactly what the game was told -- if they ever disagree,
     * the crosshair is the one telling the truth. */
    g_xh_x[0] = (LONG)buf->player[0].x;
    g_xh_y[0] = (LONG)buf->player[0].y;
    g_xh_live[0] = 1;

    /* ---- PLAYER 2 ---------------------------------------------------------
     * P2 is not a separate code path in the game: it is index 1 of this very
     * buffer, read by the same loop with the same per-player calibration. The
     * asymmetry was entirely ours - this slot was left zeroed, so P2 could
     * never aim no matter what hardware was attached.
     *
     * Sourced from XInput pad 1 so two-player play is testable without a
     * second gun; a real second lightgun replaces it at the same fields.
     * Only filled in gun-cabinet mode, so a single-player session is
     * unchanged. */
    if (shg_env_int("ULTRA_GUN_CABINET", 0) || shg_env_int("ULTRA_SHG_TWO_GUNS", 0)) {
        int32_t gw = shg_env_int("ULTRA_SHG_AIM_W", 640);
        int32_t gh = shg_env_int("ULTRA_SHG_AIM_H", 480);
        int p2x = 0, p2y = 0, p2t = 0;
        if (shg_pad_aim(1, gw, gh, &p2x, &p2y, &p2t)) {
            buf->player[1].x       = p2x;
            buf->player[1].y       = p2y;
            buf->player[1].trigger = shg_env_int("ULTRA_SHG_AIM_ALWAYS", 1) ? 1 : p2t;
            buf->player[1].status  = 0;
            g_xh_x[1] = (LONG)buf->player[1].x;
            g_xh_y[1] = (LONG)buf->player[1].y;
            g_xh_live[1] = 1;
        }
    }

    SLOG_RATE(s_gun_last, "GUN: x=%d y=%d trig=%d owns=%s hwnd=%p cw=%d ch=%d",
              (int)buf->player[0].x, (int)buf->player[0].y,
              (int)buf->player[0].trigger,
              g_pad_owns_aim ? "pad" : "mouse",
              (void*)hw, (int)cw, (int)ch);
    return 1;
}

__declspec(dllexport) void shaiolib_GUN_enable (int p) { if (p>=0&&p<2) g_gun_en[p]=1; }
__declspec(dllexport) void shaiolib_GUN_disable(int p) { if (p>=0&&p<2) g_gun_en[p]=0; }
__declspec(dllexport) void shaiolib_GUN_flash_on (int p) { (void)p; }
__declspec(dllexport) void shaiolib_GUN_flash_off(int p) { (void)p; }
__declspec(dllexport) int  shaiolib_GUN_reaction(int p, int n)         { (void)p;(void)n; return 0; }
__declspec(dllexport) int  shaiolib_GUN_set_reaction_pattern(int p, int a, int b)
                                                                        { (void)p;(void)a;(void)b; return 0; }

/* ── security (all pass) ────────────────────────────────────────────────── */
/* return 1 = "OK / check passed"; the game polls these and treats 0 as
 * "not ready / failed", SECURITY shows ".." forever if we return 0. */

/* Synthetic gun.1: 54-byte calibration blob from the real cabinet (2012).
 * Written to Data\TestDat\gun.1 on first SECURE_check so the second boot
 * cycle finds calibration data and skips the GHL-hardware calibration loop.
 * This avoids the calibration hang (tight GAKS loop on main thread) without
 * requiring a physical GHL board or a hooked GetAsyncKeyState path. */
static void shg_write_gun_cal(void) {
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;
    /* IDENTITY CALIBRATION. Superseded the real 2012 cabinet blob 2026-09-17.
     *
     * The game does not use our coordinates directly. AIM_store (0x4D12E0) runs
     * every reading through a per-player piecewise-linear calibration whose
     * constants come from this file, using the screen anchors in .data
     * (0x6CE7DC=320, 0x6CE7E0=240, half-spans 272 and 192):
     *
     *   x <  cx :  out = 320 - (cx-x)*272/leftSpan
     *   x >= cx :  out = 320 + (x-cx)*272/rightSpan
     *   y <  cy :  out = 240 - (cy-y)*192/upSpan
     *   y >= cy :  out = 240 + (y-cy)*192/downSpan
     *
     * The old blob held RAW GUN COUNTS from a physical 2012 cabinet -- center
     * (356,237), spans 276/190/270/180 -- because that is what a real GHL gun
     * reports. We do not have a GHL gun. We hand the game screen pixels that are
     * ALREADY correct in its native 640x480 space, so any calibration at all is
     * a distortion. Measured: our x=202 came out at 168, a 34-pixel left shift,
     * about 77 physical pixels at 1440 wide.
     *
     * Setting center to (320,240) and every span to exactly the screen half-span
     * makes each branch out = in, so the transform becomes the identity and our
     * coordinates arrive intact:
     *
     *   320 - (320-x)*272/272 == x        240 - (240-y)*192/192 == y
     *
     * Note we do NOT clear the per-player "use calibration" flags in bytes 0-1,
     * even though a zero there would take AIM_store's verbatim path. Those same
     * bytes are what tell the game a calibration EXISTS -- this whole file is
     * written so the second boot cycle skips the GHL hardware calibration loop,
     * which hangs without a board. Neutralize the maths, keep the flag.
     *
     * Field order per the loader at 0x4D0B30: centreX, centreY, LEFT, UP,
     * RIGHT, DOWN -- left/up before right/down, which is easy to get backwards.
     * Bytes 52-53 are a trailer, verified NOT a checksum over the payload
     * (0xD9C8 matches no byte, word or dword sum), so editing the body is safe. */
    /* THE REAL 2012 CABINET BLOB. DO NOT "CORRECT" THESE NUMBERS.
     *
     * 2026-09-17: these were replaced with an identity calibration -- center
     * (320,240), spans equal to the screen half-spans -- on the reasoning that we
     * hand the game already-correct screen pixels so any transform is a
     * distortion. The arithmetic was right and the change was still wrong.
     *
     * The game does not merely APPLY this data, it VALIDATES it. With the identity
     * values KSHG reads ../TestDat/gun.1 and then stops dead -- the log shows the
     * read followed by nothing but heartbeats, while a good run continues straight
     * on to ../TestDat/shoprank.lra and reaches the title screen. It falls into the
     * GHL gun-calibration loop this file exists to avoid: a tight GetAsyncKeyState
     * spin on the main thread that never exits without real hardware. That is the
     * blank gray page with the framed box -- it is the calibration target screen
     * (data/TestMode/TargetCircle.kit), not an error page.
     *
     * WHY THE EDIT FAILED, precisely: BYTES 52-53 ARE A CRC, not padding. An
     * earlier comment here asserted they were "NOT a checksum over the payload"
     * on the strength of byte, word and dword SUMS not matching -- a CRC was
     * never tried. They are CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over
     * bytes 0..51, stored BIG-ENDIAN:
     *     crc_ccitt_false(bytes[0..51]) == 0xC8D9  == the trailer
     *     crc_ccitt_false(all 54 bytes) == 0x0000  -- self-validating
     * So any change to the body leaves a file whose CRC no longer closes, the
     * game rejects it, and it drops into the calibration loop.
     *
     * THAT ALSO MEANS THE OFFSET IS FIXABLE PROPERLY, if it is ever worth it:
     * change the six values AND recompute the CRC over bytes 0..51, writing it
     * big-endian at 52-53. Verify by checking the CRC of all 54 bytes is zero
     * before shipping. Do NOT hand-edit values and leave the trailer alone.
     *
     * The safer route remains transforming OUR coordinates before they reach
     * the game, which needs no valid CRC and cannot brick the boot. */
    static const unsigned char s_gun1[54] = {
        0x01, 0x01, 0x20, 0x44, 0x64, 0x01, 0x00, 0x00,
        0xED, 0x00, 0x00, 0x00, 0x14, 0x01, 0x00, 0x00,
        0xBE, 0x00, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00,
        0xB4, 0x00, 0x00, 0x00, 0x53, 0x01, 0x00, 0x00,
        0xE1, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
        0xB3, 0x00, 0x00, 0x00, 0x21, 0x01, 0x00, 0x00,
        0xBF, 0x00, 0x00, 0x00, 0xC8, 0xD9
    };
    /* Build path from EXE location, not CWD: the game changes its working
     * directory to Data\eng during startup, so GetCurrentDirectory returns
     * a subdirectory rather than the game root. */
    char exepath[MAX_PATH];
    GetModuleFileNameA(NULL, exepath, MAX_PATH);
    char *last = strrchr(exepath, '\\');
    if (last) *(last + 1) = '\0'; else { exepath[0] = '.'; exepath[1] = '\\'; exepath[2] = '\0'; }
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%sData\\TestDat\\gun.1", exepath);
    /* Skip if gun.1 already exists - don't overwrite a real calibration file.
     *
     * ONE EXCEPTION: upgrade OUR OWN previous output. A gun.1 that is byte-for-
     * byte the legacy blob above can only have come from an older build of this
     * file, and leaving it in place would silently keep the 34-pixel aim offset
     * forever -- the new blob would never take effect on any machine that had
     * already run the old one. Anything else, including a genuine calibration
     * made on real hardware, is left strictly alone. */
    /* NEVER OVERWRITE AN EXISTING gun.1.
     *
     * An "upgrade our own previous blob in place" path lived here for a few hours
     * and did real damage: it rewrote this file on EVERY boot, so restoring the
     * working calibration by hand was silently undone before the game read it,
     * and several reverts that should have fixed the game appeared to do nothing.
     * This file is written once, when it is absent, and never touched again. */
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        SLOG("gun_cal: gun.1 already present -- leaving it alone");
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(s_gun1, 1, sizeof s_gun1, f);
        fclose(f);
        SLOG("gun_cal: wrote synthetic gun.1 -> %s", path);
    } else {
        SLOG("gun_cal: WARNING fopen failed: %s", path);
    }
}

__declspec(dllexport) int shaiolib_SECURE_check(void) {
    g_io_ready = 1; g_security_cleared = 1; g_cal_phase = 0;
    shg_write_gun_cal();
    SLOG("SECURE_check -> 1 (security_cleared set, cal_phase cleared)");
    return 1;
}
__declspec(dllexport) int shaiolib_SECURE_check_no_error(void) { SLOG("SECURE_check_no_error -> 1"); return 1; }
__declspec(dllexport) int shaiolib_SECURE_is_renewal(void)     { SLOG("SECURE_is_renewal -> 1"); return 1; }
__declspec(dllexport) int shaiolib_SECURE_renew(void)          { SLOG("SECURE_renew -> 1"); return 1; }

/* ── sysinfo (GHL = Silent Hill game code) ──────────────────────────────── */
/* KSHG.exe calls these with 0 args pushed (observed). The wrapper inside
 * KSHG.exe uses EAX (our return value) as the source pointer for a strcpy
 * into the game's board-name / system-id buffers.  Returning int 0 → NULL
 * → copy helper no-ops → [0x6CE7A4] stays "empty" → render gate closed.
 * Fix: return const char* to the string so EAX is a valid src pointer.
 * Also write to buf if the caller supplied one (belt-and-suspenders). */
static const char s_hw_id[]    = "KONAMI-PC-001";
static const char s_mb_id[]    = "GHL-A";
static const char s_sw_id[]    = "GHL";
/* ---- SYSINFO: ZERO-ARGUMENT, returns char* in EAX ------------------------
 *
 * These four took (char* buf, int len) and wrote through buf. KSHG.exe calls
 * all four with NO ARGUMENTS AT ALL and uses only the return value. Verified
 * at every call site in the image, not inferred:
 *
 *     4d7758  mov edi, ds:0x6804c0     ; get_hardware_id
 *     4d7760  call edi                 ; <- nothing pushed
 *     4d7762  mov ebx, ds:0x6804c4     ; get_system_id
 *     4d7768  push eax                 ; <- the RETURN VALUE is the string
 *     4d7769  call ebx                 ; <- nothing pushed
 *     4d7780  call ebx                 ; again, no args
 *     4d7782  call edi                 ; again, no args
 *
 * and the three one-instruction wrappers at 4d1b80 / 4d1ba0 / 4d1bc0 each
 * BEGIN with the call, so nothing is pushed there either. Unanimous.
 *
 * THIS WAS THE 0xC0000005. With nothing pushed, the old signature read the
 * caller's saved registers off the stack as (buf, len) -- at 4d7760 that is
 * saved edi and saved esi. Both routinely pass the > 0x10000 and > 0 guards,
 * so the stub then ran strncpy(<a register value>, id, <a register value>):
 * an arbitrary-address, arbitrary-length write. The guards made the code look
 * defensive while it did exactly the dangerous thing.
 *
 * __cdecl means the CALLER cleans the stack, so taking (void) is safe even if
 * some path did pass arguments -- they are simply ignored. Reading arguments
 * that may not be there is the unsafe direction, never the other way. */
static const char s_sys_id[]   = "GHL:A:A:2007122000";

__declspec(dllexport) const char* shaiolib_SYSINFO_get_hardware_id(void) {
    SLOG("SYSINFO_get_hardware_id -> %s", s_hw_id);
    return s_hw_id;
}
__declspec(dllexport) const char* shaiolib_SYSINFO_get_mother_board(void) {
    SLOG("SYSINFO_get_mother_board -> %s", s_mb_id);
    return s_mb_id;
}
__declspec(dllexport) const char* shaiolib_SYSINFO_get_software_id(void) {
    SLOG("SYSINFO_get_software_id -> %s", s_sw_id);
    return s_sw_id;
}
__declspec(dllexport) const char* shaiolib_SYSINFO_get_system_id(void) {
    SLOG("SYSINFO_get_system_id -> %s", s_sys_id);
    return s_sys_id;
}

/* ── 10-key - gun buttons in KSHG: trigger, option L/R ─────────────────────
 * KSHG calls shaiolib_10Key_status(void *buf) to read trigger + option buttons.
 * Original stub had wrong signature (void) so the game's buffer was never
 * written - it read uninitialized stack memory, trigger byte was garbage=1
 * → P1 TRIGGER permanently ON from the very first hardware check at boot.
 * Real layout (inferred from I/O CHECK: TRIGGER, OPTION L, OPTION R per player):
 *   buf[0] = P1 Trigger   buf[1] = P1 Option L   buf[2] = P1 Option R
 *   buf[3] = P2 Trigger   buf[4] = P2 Option L   buf[5] = P2 Option R
 * Each byte: 0x81 = pressed, 0x00 = not pressed (same as BUTTON_status). */
__declspec(dllexport) void shaiolib_10Key_disable(void) {}
__declspec(dllexport) void shaiolib_10Key_enable(void)  {}
__declspec(dllexport) int  shaiolib_10Key_status(void *out) {
    if ((size_t)out <= 0x10000u) return 0;
    /* Before security: the game passes a boot-phase context pointer here.
     * Writing to it corrupts game state and prevents SECURE_* from being reached. */
    if (!g_io_ready) return 0;
    /* After security: ALWAYS zero the entire buffer so the game never reads garbage
     * trigger/button state from uninitialized stack memory.  The game may call us
     * with a freshly stack-allocated buffer that has random non-zero bytes; without
     * this zero, trigger=ON and spurious directional buttons appear in I/O CHECK. */
    int8_t *buf = (int8_t *)out;
    /* TWELVE bytes, not twenty. This memset was the 0xC0000005.
     *
     * Caller at 0x004D14C0, read from the image:
     *     8d 44 24 f4   lea eax,[esp-0xC]     ; buffer = E-0xC  (ESP still E)
     *     83 ec 14      sub esp,0x14          ; frame  = E-0x14 .. E-1
     *     50            push eax
     *     ff 15 d4..    call shaiolib_10Key_status
     *
     * so the buffer runs E-0xC .. E-1: exactly 12 bytes, and [E] immediately
     * above it is THE RETURN ADDRESS. Zeroing 20 wrote E-0xC..E+7, clearing
     * [E], and the epilogue
     *     4d1738  add eax,4
     *     4d173b  cmp eax,0xc      <- the read loop's real bound
     *     4d173e  jl  0x4d1520
     *     4d1748  add esp,0x14
     *     4d174b  ret              <- returns through the dword we zeroed
     * then jumped to 0x00000000.
     *
     * The previous comment here claimed the game 'reads buf[8..19]' and that
     * 20 was needed to stop phantom buttons 9-12. The loop bound above says
     * otherwise: eax takes 0, 4, 8 and stops at 0xC, so nothing past buf[11]
     * is ever read and there were never buttons 9-12 to silence. The 16->20
     * change introduced the crash.
     *
     * A fault like this names no module, because address 0 belongs to none -
     * which is exactly what the loader saw: exit code 3221225477 and nothing
     * else. ultra_tracer's exception handler now reports that case explicitly. */
    memset(buf, 0, 12);
    if (!g_io_enabled) {
        SLOG_RATE(s_10key_last, "10KEY: zeroed (io not enabled yet)");
        return 1;  /* return 1 = board present; 0 would make game default trigger=ON */
    }
    /* P1 Trigger → edge-detected LMB (real or auto-calibration) */
    int trig_cur = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (!trig_cur && g_auto_lmb_until && GetTickCount() < g_auto_lmb_until)
        trig_cur = 1;  /* mirror auto-LMB so 10Key stays in sync with hooked GAKS */
    buf[0] = (trig_cur && !g_trig_prev) ? (int8_t)0x81 : 0;
    g_trig_prev = trig_cur;
    /* P1 Option L -> Z key;  P1 Option R -> X key.
     * EDGE-DETECTED, same fix as BUTTON_status: 0x81 means NEWLY PRESSED and
     * reporting it every frame a key is held makes the game see a new press
     * ~60 times a second. The trigger just above was already edge-detected;
     * these two were not, so the option buttons turbo'd. */
    {
        static unsigned char s_opt_prev[2];
        static const int optkey[2] = { 'Z', 'X' };
        int k;
        for (k = 0; k < 2; ++k) {
            int down = (GetAsyncKeyState(optkey[k]) & 0x8000) ? 1 : 0;
            buf[1 + k] = down ? (s_opt_prev[k] ? (int8_t)0x80 : (int8_t)0x81)
                              : (s_opt_prev[k] ? (int8_t)0x01 : (int8_t)0x00);
            s_opt_prev[k] = (unsigned char)down;
        }
    }
    SLOG_RATE(s_10key_last, "10KEY: trig=%02X optL=%02X optR=%02X",
              (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2]);
    return 1;
}

/* ── IAT hook utilities ─────────────────────────────────────────────────────
 * shg_iat_hook_mod: patch one IAT slot in any loaded PE (given its HMODULE).
 * shg_iat_hook: convenience wrapper that targets the main EXE (KSHG.exe).    */
static void shg_iat_hook_mod(HMODULE hmod, const char *dllname, const char *fnname,
                              void *hook) {
    HMODULE hd = GetModuleHandleA(dllname);
    if (!hd) return;
    void *real = (void *)(ULONG_PTR)GetProcAddress(hd, fnname);
    if (!real) return;
    BYTE *base = (BYTE *)(ULONG_PTR)hmod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || !dd->Size) return;
    IMAGE_IMPORT_DESCRIPTOR *imp =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((char *)(base + imp->Name), dllname)) continue;
        ULONG_PTR *fn = (ULONG_PTR *)(base + imp->FirstThunk);
        for (; *fn; ++fn) {
            if ((void *)(ULONG_PTR)*fn != real) continue;
            DWORD op;
            VirtualProtect(fn, sizeof *fn, PAGE_EXECUTE_READWRITE, &op);
            *fn = (ULONG_PTR)hook;
            VirtualProtect(fn, sizeof *fn, op, &op);
            return;
        }
    }
}

static void shg_iat_hook(const char *dllname, const char *fnname,
                          void *hook, void **orig) {
    HMODULE hd = GetModuleHandleA(dllname);
    if (!hd) return;
    void *real = (void *)(ULONG_PTR)GetProcAddress(hd, fnname);
    if (!real) return;
    if (orig) *orig = real;
    shg_iat_hook_mod(GetModuleHandleA(NULL), dllname, fnname, hook);
}

/* ── HID device shim (CreateFile / ReadFile / DeviceIoControl) ─────────────
 * KSHG opens the Konami GHL I/O board as a direct HID device:
 *   CreateFile("\\?\HID#VID_...") then reads raw reports via ReadFile.
 * Without the real board CreateFile fails → the game never gets a "trigger=0"
 * report → trigger stays at 1 (ON) permanently from process start.
 *
 * The standard fix is to present a virtual USB HID device the game can open
 * and get clean data from.  We do this in user mode: IAT-hook CreateFile
 * to return a sentinel handle for any HID path, ReadFile on that handle returns
 * all-zero reports (trigger=0, axes neutral), and DeviceIoControl answers the
 * standard HID IOCTLs so the game's HID parser initializes without errors.   */

#define SHG_HID_SENTINEL ((HANDLE)(ULONG_PTR)0x48494400u)

typedef HANDLE  (WINAPI *pfnCFA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE  (WINAPI *pfnCFW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL    (WINAPI *pfnRF) (HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL    (WINAPI *pfnDIC)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL    (WINAPI *pfnCH) (HANDLE);
typedef int     (WINAPI *pfnMBA)(HWND,LPCSTR,LPCSTR,UINT);
typedef DWORD   (WINAPI *pfnMWFMO)(DWORD,const HANDLE*,BOOL,DWORD,DWORD);
typedef int     (WINAPI *pfnMBW)(HWND,LPCWSTR,LPCWSTR,UINT);
typedef HMODULE (WINAPI *pfnLLA)(LPCSTR);
typedef HMODULE (WINAPI *pfnLLW)(LPCWSTR);
typedef HMODULE (WINAPI *pfnLLEA)(LPCSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *pfnLLEW)(LPCWSTR, HANDLE, DWORD);
typedef void    (WINAPI *pfnSlp)(DWORD);
typedef DWORD   (WINAPI *pfnWFSO)(HANDLE,DWORD);
typedef DWORD   (WINAPI *pfnWFMO)(DWORD,const HANDLE*,BOOL,DWORD);
typedef int     (WINAPI *pfnConnect)(UINT,const void*,int);   /* ws2_32 connect */
typedef DWORD   (WINAPI *pfnIcmpSE)(HANDLE,DWORD,LPVOID,WORD,LPVOID,LPVOID,DWORD,DWORD);

static pfnCFA  s_real_CFA  = NULL;  static pfnCFA s_cfa_tramp = NULL;
static pfnCFW  s_real_CFW  = NULL;  static pfnCFW s_cfw_tramp = NULL;
static pfnRF   s_real_RF   = NULL;  static pfnRF  s_rf_tramp  = NULL;
static pfnLLA  s_real_LLA  = NULL;
static pfnLLW  s_real_LLW  = NULL;
static pfnLLEA s_real_LLEA = NULL;
static pfnLLEW s_real_LLEW = NULL;
static pfnDIC  s_real_DIC  = NULL;
static pfnCH   s_real_CH   = NULL;
static pfnMBA  s_real_MBA  = NULL;
static pfnMBW  s_real_MBW  = NULL;
static pfnSlp  s_real_Slp  = NULL;
static pfnWFSO    s_real_WFSO    = NULL;
static pfnWFMO    s_real_WFMO    = NULL;
static pfnConnect s_real_Connect = NULL;
static pfnIcmpSE  s_real_IcmpSE  = NULL;
static pfnMWFMO   s_real_MWFMO2  = NULL;  /* MsgWaitForMultipleObjects (user32) */

static int shg_is_hid_pathA(LPCSTR p) {
    if (!p) return 0;
    for (const char *q = p; *q; ++q) {
        char c = *q; if (c>='A'&&c<='Z') c+=(char)32;
        if (c=='h'&&(q[1]=='i'||q[1]=='I')&&(q[2]=='d'||q[2]=='D')) return 1;
    }
    return 0;
}
static int shg_is_hid_pathW(LPCWSTR p) {
    if (!p) return 0;
    for (const WCHAR *q = p; *q; ++q) {
        WCHAR c=*q; if(c>='A'&&c<='Z') c+=(WCHAR)32;
        if(c=='h'&&(q[1]=='i'||q[1]=='I')&&(q[2]=='d'||q[2]=='D')) return 1;
    }
    return 0;
}

static HANDLE WINAPI shg_CreateFileA(LPCSTR n,DWORD a,DWORD s,
        LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD f,HANDLE t) {
    if (shg_is_hid_pathA(n)) { SLOG("CFA HID: \"%s\"->sentinel",n?n:""); return SHG_HID_SENTINEL; }
    { static DWORD s_cfa_log = 0;
      SLOG_RATE(s_cfa_log, "CFA non-HID: \"%s\"", n?n:"(null)");
      pfnCFA c = s_cfa_tramp ? s_cfa_tramp : s_real_CFA;
      HANDLE hr = c ? c(n,a,s,sa,d,f,t) : INVALID_HANDLE_VALUE;
      SLOG_RATE(s_cfa_log, "CFA non-HID ret: h=%p err=%lu", (void*)hr, (unsigned long)(hr==INVALID_HANDLE_VALUE?GetLastError():0));
      return hr; }
}
static HANDLE WINAPI shg_CreateFileW(LPCWSTR n,DWORD a,DWORD s,
        LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD f,HANDLE t) {
    if (shg_is_hid_pathW(n)) { SLOG("HID shim: CFW(HID)->sentinel"); return SHG_HID_SENTINEL; }
    { pfnCFW c = s_cfw_tramp ? s_cfw_tramp : s_real_CFW;
      return c ? c(n,a,s,sa,d,f,t) : INVALID_HANDLE_VALUE; }
}

static BOOL WINAPI shg_ReadFile(HANDLE h,LPVOID buf,DWORD n,LPDWORD got,LPOVERLAPPED ov) {
    if (h == SHG_HID_SENTINEL) {
        static DWORD s_rf_log_last = 0;
        if (buf && n>0) {
            memset(buf,0,n);
            if (n >= 1) {
                BYTE *rpt = (BYTE*)buf;
                /* Fill calibration position with screen-center coords.
                 * Calibration record: byte 0 = trigger+buttons, byte 1 = X axis,
                 * byte 2 = Y axis.  X/Y are signed-8 (-128..127); 0 = center. */
                if (n >= 3) { rpt[1] = 0; rpt[2] = 0; }  /* center crosshair */
                if (g_io_ready) {
                    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8001) ||
                        (g_auto_lmb_until && GetTickCount() < g_auto_lmb_until)) {
                        rpt[0] |= 0x01;  /* trigger bit */
                    }
                }
            }
        }
        SLOG_RATE(s_rf_log_last, "ReadFile sentinel: n=%lu io_ready=%d b0=0x%02X",
                  (unsigned long)n, g_io_ready, buf&&n>0 ? *(BYTE*)buf : 0);
        if (got) *got = n;
        return TRUE;
    }
    {
        static DWORD s_rf_real_last = 0;
        SLOG_RATE(s_rf_real_last, "ReadFile REAL: h=%p n=%lu", (void*)h, (unsigned long)n);
        pfnRF c = s_rf_tramp ? s_rf_tramp : s_real_RF;
        return c ? c(h,buf,n,got,ov) : FALSE;
    }
}

#ifndef IOCTL_HID_GET_COLLECTION_DESCRIPTOR
#define IOCTL_HID_GET_COLLECTION_DESCRIPTOR 0x000B0091
#endif
#ifndef IOCTL_HID_GET_ATTRIBUTES
#define IOCTL_HID_GET_ATTRIBUTES            0x000B0103
#endif
typedef struct { USHORT VendorID,ProductID,VersionNumber; } SHG_HID_ATTR;

/* Minimal valid HID report descriptor: 4 buttons + X/Y axes (3-byte report).
 * Button 0 = trigger.  The game's HID parser reads this at init time. */
static const BYTE s_hid_rdesc[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01,
    0x05,0x09, 0x19,0x01, 0x29,0x04,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x04, 0x81,0x02,
    0x95,0x04, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31,
    0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0xC0
};

static BOOL WINAPI shg_DeviceIoControl(HANDLE h,DWORD code,LPVOID ib,DWORD isz,
        LPVOID ob,DWORD osz,LPDWORD ret,LPOVERLAPPED ov) {
    if (h != SHG_HID_SENTINEL)
        return s_real_DIC ? s_real_DIC(h,code,ib,isz,ob,osz,ret,ov) : FALSE;
    if (ret) *ret = 0;
    switch (code) {
        case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
            if (ob && osz>=(DWORD)sizeof s_hid_rdesc) {
                memcpy(ob,s_hid_rdesc,sizeof s_hid_rdesc);
                if (ret) *ret=(DWORD)sizeof s_hid_rdesc; return TRUE;
            }
            return FALSE;
        case IOCTL_HID_GET_ATTRIBUTES:
            if (ob && osz>=sizeof(SHG_HID_ATTR)) {
                SHG_HID_ATTR *a=(SHG_HID_ATTR*)ob;
                a->VendorID=0x06D9; a->ProductID=0x0220; a->VersionNumber=0x0100;
                if (ret) *ret=sizeof(SHG_HID_ATTR); return TRUE;
            }
            return FALSE;
        default:
            if (ob && osz>0) memset(ob,0,osz);
            return TRUE;
    }
}

static BOOL WINAPI shg_CloseHandle(HANDLE h) {
    if (h == SHG_HID_SENTINEL) return TRUE;
    return s_real_CH ? s_real_CH(h) : FALSE;
}

/* Sleep hook via IAT only (no function-byte detour - trampoline crashes on
 * Windows 10 where kernel32!Sleep starts with a split multi-byte instruction).
 * During calibration: cap long sleeps.  Outside calibration: log > 3s sleeps
 * for diagnostics, then call through. */
static void WINAPI shg_Sleep(DWORD ms) {
    pfnSlp real = s_real_Slp;
    /* Pre-IO boot: cap long sleeps so libbms/kac network init doesn't stall. */
    if (ms >= 1000 && !g_io_enabled) {
        SLOG("Sleep pre-IO: %lu ms → 50ms", (unsigned long)ms);
        if (real) real(50);
        return;
    }
    if (g_cal_phase && ms >= 1000) {
        SLOG("Sleep cal_phase: %lu ms → 50ms", (unsigned long)ms);
        if (real) real(50);
        return;
    }
    /* After VEH fire OR after io_enabled (short-boot path): cap sleeps > 50ms.
     * The huge 1984204624ms sleep that normally drives the GHL hardware detection
     * loop must be capped whether we got here via VEH (full-boot) or via the
     * initialize()→1 short-boot path where g_veh_fired never becomes true. */
    if ((g_veh_fired || g_io_enabled) && ms > 50) {
        SLOG("Sleep cap: %lu ms → 50ms tid=%lu (veh=%d io=%d)",
             (unsigned long)ms, (unsigned long)GetCurrentThreadId(), g_veh_fired, g_io_enabled);
        if (real) real(50);
        return;
    }
    if (g_veh_fired || g_io_enabled) {
        SLOG("Sleep pass: %lu ms tid=%lu", (unsigned long)ms, (unsigned long)GetCurrentThreadId());
    } else if (ms >= 1000) {
        SLOG("Sleep LONG (io=0/cal=0): %lu ms", (unsigned long)ms);
    }
    if (real) real(ms);
}

/* IcmpSendEcho hook - KSHG.exe imports icmp.dll and pings the e-amusement
 * server at startup.  The call blocks for up to the 'Timeout' milliseconds
 * (typically 30 000-120 000 ms) waiting for an ICMP echo reply that never
 * comes.  Return 0 (failure) immediately so the game treats the server as
 * unreachable and moves on without waiting. */
static DWORD WINAPI shg_IcmpSendEcho(HANDLE h, DWORD dest, LPVOID req, WORD reqsz,
                                      LPVOID opts, LPVOID reply, DWORD replysz, DWORD timeout) {
    (void)h; (void)dest; (void)req; (void)reqsz; (void)opts;
    (void)reply; (void)replysz; (void)timeout;
    SLOG("IcmpSendEcho: blocked timeout=%lu → fail-fast (IP_REQ_TIMED_OUT)", (unsigned long)timeout);
    SetLastError(11010); /* IP_REQ_TIMED_OUT */
    return 0;
}

/* connect() hook - inline-hooked at ws2_32 function bytes so ALL callers are
 * intercepted regardless of which DLL calls it or how it obtained the pointer.
 * The e-amusement server is unreachable on a PC setup, so connect() would block
 * for ~120s (TCP default timeout) on every call.  Return WSAEHOSTUNREACH (10065)
 * immediately so the game sees "server not reachable" and moves on. */
static int WINAPI shg_connect(UINT s, const void *name, int namelen) {
    (void)s; (void)name; (void)namelen;
    SLOG("connect: intercepted -> WSAEHOSTUNREACH");
    SetLastError(10065); /* WSAEHOSTUNREACH */
    return -1;           /* SOCKET_ERROR */
}

/* WaitForSingleObject hook via IAT only (no function-byte detour).
 * During calibration: cap INFINITE/long waits to 200ms.
 * Outside calibration: rate-log long/INFINITE waits so we can see what blocks. */
static DWORD WINAPI shg_WFSO(HANDLE h, DWORD ms) {
    static DWORD s_wfso_long_last = 0;
    pfnWFSO real = s_real_WFSO;
    /* Pre-IO boot: cap long waits so libbms network init doesn't stall. */
    if ((ms == INFINITE || ms >= 2000) && !g_io_enabled) {
        SLOG("WFSO pre-IO: h=%p ms=%lu → 200ms", (void*)h, (unsigned long)ms);
        DWORD r = real ? real(h, 200) : WAIT_TIMEOUT;
        SLOG("WFSO pre-IO ret=%lu", (unsigned long)r);
        return r;
    }
    if ((ms == INFINITE || ms >= 5000) && g_cal_phase) {
        SLOG("WFSO BLOCKING cal_phase: h=%p ms=%lu → 200ms",
             (void*)h, (unsigned long)ms);
        DWORD r = real ? real(h, 200) : WAIT_TIMEOUT;
        SLOG("WFSO BLOCKING ret: %lu", (unsigned long)r);
        return r;
    }
    /* After VEH: log ALL waits so we can identify what the game blocks on. */
    if (g_veh_fired) {
        SLOG("WFSO post-VEH: h=%p ms=%lu", (void*)h, (unsigned long)ms);
        DWORD r = real ? real(h, ms == INFINITE ? 500 : ms) : WAIT_FAILED;
        SLOG("WFSO post-VEH ret: %lu", (unsigned long)r);
        return r;
    }
    if (ms == INFINITE || ms >= 5000) {
        SLOG_RATE(s_wfso_long_last, "WFSO LONG (cal=0): h=%p ms=%lu",
                  (void*)h, (unsigned long)ms);
    }
    return real ? real(h, ms) : WAIT_FAILED;
}

/* WaitForMultipleObjects hook - log and cap long waits during pre-IO boot. */
static DWORD WINAPI shg_WFMO(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    pfnWFMO real = s_real_WFMO;
    if ((ms == INFINITE || ms >= 2000) && !g_io_enabled) {
        SLOG("WFMO pre-IO: n=%lu all=%d ms=%lu → 200ms", (unsigned long)n, all, (unsigned long)ms);
        DWORD r = real ? real(n, hs, all, 200) : WAIT_TIMEOUT;
        SLOG("WFMO pre-IO ret=%lu", (unsigned long)r);
        return r;
    }
    if ((ms == INFINITE || ms >= 5000) && g_cal_phase) {
        SLOG("WFMO cal_phase: n=%lu all=%d ms=%lu → 200ms", (unsigned long)n, all, (unsigned long)ms);
        DWORD r = real ? real(n, hs, all, 200) : WAIT_TIMEOUT;
        SLOG("WFMO cal_phase ret=%lu", (unsigned long)r);
        return r;
    }
    return real ? real(n, hs, all, ms) : WAIT_FAILED;
}

/* MsgWaitForMultipleObjects hook - game main loop uses this for frame pacing.
 * Cap infinite/huge waits pre-IO.  Post-VEH: log with rate-limit to trace state. */
static DWORD WINAPI shg_MWFMO(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD mask) {
    pfnMWFMO real = s_real_MWFMO2;
    /* Pre-IO: hard cap so early bootstrap doesn't block for a network wait. */
    if ((ms == INFINITE || ms >= 2000) && !g_io_enabled) {
        SLOG("MWFMO pre-IO: n=%lu ms=%lu mask=0x%lx → 200ms",
             (unsigned long)n, (unsigned long)ms, (unsigned long)mask);
        DWORD r = real ? real(n, hs, all, 200, mask) : WAIT_TIMEOUT;
        SLOG("MWFMO pre-IO ret=%lu", (unsigned long)r);
        return r;
    }
    /* After VEH: log the first call so we know what timeout the game loop uses. */
    if (g_veh_fired) {
        static DWORD s_mwfmo_last = 0;
        SLOG_RATE(s_mwfmo_last, "MWFMO post-VEH: n=%lu ms=%lu mask=0x%lx",
                  (unsigned long)n, (unsigned long)ms, (unsigned long)mask);
    }
    return real ? real(n, hs, all, ms, mask) : WAIT_FAILED;
}

/* Belt-and-suspenders: also zero mouse button flags on any raw-input read. */
typedef UINT (WINAPI *pfnGetRID)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
static pfnGetRID s_real_GetRID = NULL;
static DWORD s_rid_mouse_last = 0, s_rid_hid_last = 0;
static UINT WINAPI shg_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data,
                                        PUINT sz, UINT hdr) {
    if (!s_real_GetRID) return (UINT)-1;
    UINT r = s_real_GetRID(h, cmd, data, sz, hdr);
    if (cmd==RID_INPUT && data && r!=(UINT)-1) {
        RAWINPUT *ri=(RAWINPUT*)data;
        if (ri->header.dwType==RIM_TYPEMOUSE) {
            DWORD fl = ri->data.mouse.usButtonFlags;
            SLOG_RATE(s_rid_mouse_last, "GetRID: MOUSE flags=0x%04X dx=%d dy=%d",
                      (unsigned)fl, (int)ri->data.mouse.lLastX, (int)ri->data.mouse.lLastY);
        } else if (ri->header.dwType==RIM_TYPEHID) {
            DWORD hsz=ri->data.hid.dwSizeHid, cnt=ri->data.hid.dwCount;
            SLOG_RATE(s_rid_hid_last, "GetRID: HID sz=%lu cnt=%lu b0=0x%02X",
                      (unsigned long)hsz, (unsigned long)cnt,
                      (hsz>0&&cnt>0) ? ri->data.hid.bRawData[0] : 0);
            if (hsz>0&&cnt>0&&hsz<=256&&cnt<=16) memset(ri->data.hid.bRawData,0,hsz*cnt);
        }
    }
    return r;
}

/* ── SetupAPI + HidD shim (GetProcAddress intercept) ────────────────────────
 * KSHG.exe never calls CreateFile for HID directly from its IAT.  Instead it
 * dynamically loads setupapi.dll via LoadLibraryA + GetProcAddress and calls:
 *   SetupDiGetClassDevs → enumerate HID class
 *   SetupDiEnumDeviceInterfaces → iterate devices
 *   SetupDiGetDeviceInterfaceDetail → get the device path string
 * When no GHL board (VID 06D9 / PID 0220) is found the game enters an error
 * mode: I/O CHECK shows ALL inputs as ON (error = stuck-pressed) and service
 * menu navigation falls through to a DirectInput joystick path that makes DOWN
 * permanently pressed.
 *
 * Fix: IAT-hook GetProcAddress so any SetupDi* or HidD_* lookup from KSHG.exe
 * gets our stub instead.  Our stubs report ONE fake GHL device.  When the game
 * then calls CreateFileA on the returned path (which contains "HID"), our
 * existing HID sentinel intercepts it → ReadFile returns zeros → trigger=0,
 * all directions=0.                                                           */

typedef PVOID HDEVINFO;  /* mirrors setupapi.h; avoid pulling in the whole header */
#define SHG_DEVINFO_SENTINEL ((HDEVINFO)(ULONG_PTR)0x44455649u)  /* "DEVI" */

/* Minimal struct mirrors (avoid pulling in setupapi.h) */
typedef struct { DWORD cbSize; GUID IfGuid; DWORD Flags; ULONG_PTR Reserved; } SHG_IF_DATA;
typedef struct { DWORD cbSize; CHAR  DevicePath[1]; } SHG_IF_DETAIL_A;

static HDEVINFO WINAPI shg_SetupDiGetClassDevsA(const GUID *g,PCSTR en,HWND hw,DWORD fl)
    {(void)g;(void)en;(void)hw;(void)fl;SLOG("SetupDi:GetClassDevsA->sentinel");return SHG_DEVINFO_SENTINEL;}
static HDEVINFO WINAPI shg_SetupDiGetClassDevsW(const GUID *g,PCWSTR en,HWND hw,DWORD fl)
    {(void)g;(void)en;(void)hw;(void)fl;SLOG("SetupDi:GetClassDevsW->sentinel");return SHG_DEVINFO_SENTINEL;}

static BOOL WINAPI shg_SetupDiEnumDeviceInterfaces(HDEVINFO di,void *did,
        const GUID *icg,DWORD idx,SHG_IF_DATA *ifd) {
    (void)did;(void)icg;
    if (di!=SHG_DEVINFO_SENTINEL){SetLastError(259/*ERROR_NO_MORE_ITEMS*/);return FALSE;}
    if (idx==0) {
        if(ifd){memset(ifd,0,sizeof*ifd);ifd->cbSize=sizeof*ifd;ifd->Flags=1/*SPINT_ACTIVE*/;}
        SLOG("SetupDi:EnumDeviceInterfaces(0)->TRUE");
        return TRUE;
    }
    SetLastError(259); return FALSE;
}

static BOOL WINAPI shg_SetupDiGetDeviceInterfaceDetailA(HDEVINFO di,SHG_IF_DATA *ifd,
        SHG_IF_DETAIL_A *detail,DWORD detail_sz,DWORD *req,void *devdata) {
    (void)ifd;(void)devdata;
    if (di!=SHG_DEVINFO_SENTINEL) return FALSE;
    /* Path contains "HID" so our CreateFileA hook intercepts the subsequent open. */
    static const char fake[]="\\\\?\\HID#VID_06D9&PID_0220#1&deadbeef&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    DWORD need=(DWORD)(sizeof(DWORD)+strlen(fake)+1);
    if(req)*req=need;
    if(!detail||detail_sz<need){SetLastError(122/*ERROR_INSUFFICIENT_BUFFER*/);return FALSE;}
    detail->cbSize=sizeof(DWORD)+1; /* sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) */
    strcpy(detail->DevicePath,fake);
    SLOG("SetupDi:GetDeviceInterfaceDetailA->\"%s\"",fake);
    return TRUE;
}
static BOOL WINAPI shg_SetupDiGetDeviceInterfaceDetailW(HDEVINFO di,SHG_IF_DATA *ifd,
        void *detail,DWORD detail_sz,DWORD *req,void *devdata)
    {(void)di;(void)ifd;(void)detail;(void)detail_sz;(void)devdata;if(req)*req=0;return FALSE;}

static BOOL WINAPI shg_SetupDiDestroyDeviceInfoList(HDEVINFO di){(void)di;return TRUE;}

/* HidD stubs: game may verify the device attributes after opening */
typedef struct{ULONG Size;USHORT VendorID,ProductID,VersionNumber;}SHG_HIDD_ATTR;
static BOOLEAN WINAPI shg_HidD_GetAttributes(HANDLE h,SHG_HIDD_ATTR *a){
    if(h!=SHG_HID_SENTINEL) return FALSE;
    if(a){a->Size=sizeof*a;a->VendorID=0x06D9;a->ProductID=0x0220;a->VersionNumber=0x0100;}
    SLOG("HidD_GetAttributes->06D9:0220"); return TRUE; }
#define SHG_PREPARSED ((void*)(ULONG_PTR)0x50524550u)
static BOOLEAN WINAPI shg_HidD_GetPreparsedData(HANDLE h,void**pp)
    {(void)h;if(pp)*pp=SHG_PREPARSED;return TRUE;}
static BOOLEAN WINAPI shg_HidD_FreePreparsedData(void*pp){(void)pp;return TRUE;}
static NTSTATUS WINAPI shg_HidP_GetCaps(void*pp,void*caps)
    {(void)pp;if(caps)memset(caps,0,sizeof(DWORD)*8);return 0x00110001/*HIDP_STATUS_SUCCESS*/;}

/* forward declarations - defined below, referenced in shg_GetProcAddress */
static int WINAPI shg_MessageBoxA(HWND,LPCSTR,LPCSTR,UINT);
static int WINAPI shg_MessageBoxW(HWND,LPCWSTR,LPCWSTR,UINT);

/* ── winmm callback protection ─────────────────────────────────────────────
 * kacbookkeeping.dll calls waveOutOpen / timeSetEvent with a callback pointer
 * that was never populated by the absent GHL hardware interrupt handler -
 * the field in the GHL board struct stays at a garbage small integer (0x0F,
 * 0x12, …).  Intercept all multimedia callback registrations and replace any
 * pointer < 0x10000 with a valid no-op to prevent the crash. */
static void CALLBACK shg_waveout_noop(HWAVEOUT hwo, UINT uMsg,
                                       DWORD_PTR dwInst, DWORD_PTR dw1, DWORD_PTR dw2)
    { (void)hwo; (void)uMsg; (void)dwInst; (void)dw1; (void)dw2; }
static void CALLBACK shg_timer_noop(UINT uID, UINT uMsg,
                                     DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
    { (void)uID; (void)uMsg; (void)dwUser; (void)dw1; (void)dw2; }
/* CreateTimerQueueTimer callback noop: WAITORTIMERCALLBACK signature */
static void CALLBACK shg_wortimer_noop(PVOID lpParam, BOOLEAN TimerOrWaitFired)
    { (void)lpParam; (void)TimerOrWaitFired; }

/* MMIO: THE ONLY INSTRUMENT THAT CAN SEE THIS GAME'S SOUND PATH.
 *
 * Log-only. It changes nothing and decides nothing -- it exists because every
 * sound question asked about this title so far has been asked of the wrong
 * layer, and the answers were silence in both directions.
 *
 * KSHG.exe statically imports mmioOpenA from WINMM.dll (IAT slot 0x680328,
 * read out of the PE import table). It imports NO waveOut* at all, no XAudio2,
 * and DSOUND only by ordinal 11. The loader's CreateFileA record cannot see an
 * mmio open, which is exactly why it has never had anything to say about audio
 * -- the file record was watching a door this game does not use.
 *
 * A failed open matters as much as a successful one: on failure the routine at
 * 0x433590 falls through to a FindResourceA/LoadResource path and plays audio
 * embedded in the executable instead, which would look like "sound works" while
 * every external sound file is missing. */
typedef HMMIO (WINAPI *pfnMmioOpenA)(LPSTR, LPMMIOINFO, DWORD);
static pfnMmioOpenA s_real_MMIOA = NULL;
static HMMIO WINAPI shg_mmioOpenA(LPSTR name, LPMMIOINFO info, DWORD flags) {
    static unsigned n = 0;
    HMMIO h = s_real_MMIOA ? s_real_MMIOA(name, info, flags) : (HMMIO)0;
    ++n;
    /* First 64 unconditionally. The boot opens are the ones that matter and
     * there are only about fourteen of them; a rate limiter that hid those
     * would waste the whole run. */
    if (n <= 64 || (n % 20) == 0)
        SLOG("MMIO: #%u open(\"%s\") flags=0x%08lX -> %p%s", n,
             (name && (size_t)name > 0x10000u) ? name : "(null)",
             (unsigned long)flags, (void *)h,
             h ? "" : "  <- FAILED (0x433590 falls through to its"
                      " FindResourceA/LoadResource embedded-audio path)");
    return h;
}

typedef MMRESULT (WINAPI *pfnWaveOutOpen)(LPHWAVEOUT,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD);
static pfnWaveOutOpen s_real_WOO = NULL;
static MMRESULT WINAPI shg_waveOutOpen(LPHWAVEOUT phwo, UINT uDev, LPCWAVEFORMATEX pwfx,
                                        DWORD_PTR dwCB, DWORD_PTR dwInst, DWORD fdwOpen) {
    if ((fdwOpen & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION && dwCB < 0x10000) {
        SLOG("waveOutOpen: bad callback=0x%08lX → noop (uDev=%u)", (unsigned long)dwCB, uDev);
        dwCB = (DWORD_PTR)(void*)shg_waveout_noop;
    } else {
        SLOG("waveOutOpen: callback=0x%08lX uDev=%u fdwOpen=0x%lX",
             (unsigned long)dwCB, uDev, (unsigned long)fdwOpen);
    }
    return s_real_WOO ? s_real_WOO(phwo, uDev, pwfx, dwCB, dwInst, fdwOpen) : MMSYSERR_NODRIVER;
}

typedef MMRESULT (WINAPI *pfnTimeSetEvent)(UINT,UINT,LPTIMECALLBACK,DWORD_PTR,UINT);
static pfnTimeSetEvent s_real_TSE = NULL;
static MMRESULT WINAPI shg_timeSetEvent(UINT uDelay, UINT uRes, LPTIMECALLBACK lpProc,
                                         DWORD_PTR dwUser, UINT fuEvent) {
    if ((ULONG_PTR)lpProc < 0x10000) {
        SLOG("timeSetEvent: bad callback=0x%p → noop", (void*)(ULONG_PTR)lpProc);
        lpProc = (LPTIMECALLBACK)shg_timer_noop;
    } else {
        SLOG("timeSetEvent: callback=%p delay=%u user=0x%08lX",
             (void*)lpProc, uDelay, (unsigned long)dwUser);
    }
    return s_real_TSE ? s_real_TSE(uDelay, uRes, lpProc, dwUser, fuEvent) : 0;
}

/* QueueUserAPC hook - catches APC-based callbacks delivered on main thread.
 * kacbookkeeping.dll may queue APCs via GetProcAddress("QueueUserAPC"); the
 * callback pointer (from an uninitialized GHL board struct) is a small integer.
 * Replace bad callbacks before the APC is queued. */
typedef DWORD (WINAPI *pfnQUAPC)(PAPCFUNC,HANDLE,ULONG_PTR);
static pfnQUAPC s_real_QUAPC = NULL;
static DWORD WINAPI shg_QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData) {
    if ((ULONG_PTR)pfnAPC < 0x10000) {
        SLOG("QueueUserAPC: bad callback=0x%p → noop", (void*)(ULONG_PTR)pfnAPC);
        pfnAPC = (PAPCFUNC)shg_wortimer_noop;
    } else {
        SLOG("QueueUserAPC: callback=%p tid-ish=%p", (void*)pfnAPC, (void*)hThread);
    }
    return s_real_QUAPC ? s_real_QUAPC(pfnAPC, hThread, dwData) : 0;
}

static void shg_install_quapc_inline(void) {
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                   "QueueUserAPC");
    if (!fn) { SLOG("inline-QUAPC: not found"); return; }
    SLOG("inline-QUAPC: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    s_real_QUAPC = (pfnQUAPC)fn;
    if (fn[0] == 0xFF || fn[0] == 0xE9 || fn[0] == 0xEB) {
        SLOG("inline-QUAPC: JMP stub fn[0]=0x%02X, GPA-hook only", fn[0]);
        return;
    }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("inline-QUAPC: VirtualAlloc failed"); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p)) {
        SLOG("inline-QUAPC: VirtualProtect failed %lu", (unsigned long)GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE); return;
    }
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_QueueUserAPC - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    s_real_QUAPC = (pfnQUAPC)tramp;
    SLOG("inline-QUAPC: installed tramp=%p", (void*)tramp);
}

/* CreateTimerQueueTimer hook - catches thread-pool timer callbacks.
 * kacbookkeeping.dll registers GHL board callbacks via CreateTimerQueueTimer
 * (obtained with GetProcAddress before our hooks are active).  On the real
 * cabinet the callback pointer is a valid GHL handler; without hardware it is
 * a small garbage integer.  Replace any bad callback with shg_wortimer_noop. */
typedef BOOL (WINAPI *pfnCTQT)(PHANDLE,HANDLE,WAITORTIMERCALLBACK,PVOID,ULONG,ULONG,ULONG);
static pfnCTQT s_real_CTQT = NULL;
static BOOL WINAPI shg_CreateTimerQueueTimer(PHANDLE phNew, HANDLE hQueue,
                                              WAITORTIMERCALLBACK cb, PVOID param,
                                              ULONG due, ULONG period, ULONG flags) {
    if ((ULONG_PTR)cb < 0x10000) {
        SLOG("CTQT: bad callback=0x%p → noop (due=%lu period=%lu)",
             (void*)(ULONG_PTR)cb, (unsigned long)due, (unsigned long)period);
        cb = shg_wortimer_noop;
    } else {
        SLOG("CTQT: callback=%p due=%lu period=%lu", (void*)cb,
             (unsigned long)due, (unsigned long)period);
    }
    return s_real_CTQT ? s_real_CTQT(phNew, hQueue, cb, param, due, period, flags) : FALSE;
}

static void shg_install_ctqt_inline(void) {
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                   "CreateTimerQueueTimer");
    if (!fn) fn = (BYTE *)(ULONG_PTR)GetProcAddress(GetModuleHandleA("KernelBase.dll"),
                                                      "CreateTimerQueueTimer");
    if (!fn) { SLOG("inline-CTQT: not found"); return; }
    SLOG("inline-CTQT: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    s_real_CTQT = (pfnCTQT)fn;  /* IAT / GPA path: before inline patch */
    /* Skip if starts with indirect-JMP (FF 25) or short JMP - can't 5-byte copy. */
    if (fn[0] == 0xFF || fn[0] == 0xE9 || fn[0] == 0xEB) {
        SLOG("inline-CTQT: JMP stub at fn[0]=0x%02X, GPA-hook only", fn[0]);
        return;
    }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("inline-CTQT: VirtualAlloc failed"); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p)) {
        SLOG("inline-CTQT: VirtualProtect failed %lu", (unsigned long)GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE); return;
    }
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_CreateTimerQueueTimer - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    s_real_CTQT = (pfnCTQT)tramp;
    SLOG("inline-CTQT: installed → shg_CTQT=%p tramp=%p",
         (void*)shg_CreateTimerQueueTimer, (void*)tramp);
}

/* ── D3D9 windowed-mode intercept ─────────────────────────────────────────────
 * KSHG.exe uses fullscreen exclusive D3D9 (640×480).  On Windows 10 DWM this
 * causes D3DERR_DEVICELOST → black screen.  We intercept Direct3DCreate9 via
 * the GPA hook, patch IDirect3D9::CreateDevice (vtable[16]) to force
 * Windowed=TRUE, then hook IDirect3DDevice9::Present (vtable[17]) to log frames.
 *
 * Minimal D3DPRESENT_PARAMETERS layout (matches d3d9.h exactly). */
typedef struct _SHG_D3DPP {
    UINT  BackBufferWidth;            /*  0 */
    UINT  BackBufferHeight;           /*  4 */
    DWORD BackBufferFormat;           /*  8 */
    UINT  BackBufferCount;            /* 12 */
    DWORD MultiSampleType;            /* 16 */
    DWORD MultiSampleQuality;         /* 20 */
    DWORD SwapEffect;                 /* 24 */
    HWND  hDeviceWindow;              /* 28 */
    BOOL  Windowed;                   /* 32 ← */
    BOOL  EnableAutoDepthStencil;     /* 36 */
    DWORD AutoDepthStencilFormat;     /* 40 */
    DWORD Flags;                      /* 44 */
    UINT  FullScreen_RefreshRateInHz; /* 48 ← */
    UINT  PresentationInterval;       /* 52 */
} SHG_D3DPP;

typedef HRESULT (__stdcall *pfnD3D9CD_real)(
    void*, UINT, DWORD, HWND, DWORD, SHG_D3DPP*, void**);
typedef HRESULT (__stdcall *pfnD3DDevPresent)(
    void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT (__stdcall *pfnD3DBeginScene)(void *);
typedef HRESULT (__stdcall *pfnD3DEndScene)(void *);
typedef HRESULT (__stdcall *pfnD3DGetSC)(void *, UINT, void **);
typedef HRESULT (__stdcall *pfnSCPresent)(void*, const RECT*, const RECT*, HWND, const void*, DWORD);
/* ONE argument, not two. See shg_EaxManagerCreate below -- getting this wrong
 * is what killed this game for five runs. */
typedef HRESULT (__stdcall *pfnEaxCreate)(void **);
typedef void*   (WINAPI  *pfnD3DC9)(UINT);

static pfnD3DC9         s_real_D3DC9       = NULL;
static pfnD3D9CD_real   s_real_D3D9CD      = NULL;
static pfnD3DDevPresent s_real_D3DDevPrst  = NULL;
static pfnD3DBeginScene s_real_D3DBegin    = NULL;
static pfnD3DEndScene   s_real_D3DEnd      = NULL;
static pfnD3DGetSC      s_real_D3DGetSC    = NULL;
static pfnSCPresent     s_real_SCPresent   = NULL;
static pfnEaxCreate     s_real_EaxCreate   = NULL;
static void           **s_d3d9_vtable      = NULL;
static void           **s_d3ddev_vtable    = NULL;
static volatile LONG    s_present_count    = 0;
static volatile LONG    s_begin_count      = 0;
static volatile LONG    s_sc_present_count = 0;
static void            *s_d3d9_dev         = NULL;  /* stored in CreateDevice hook */

/* ── SOUND INSTRUMENT: WHY BGM IS SILENT WHILE EVERYTHING ELSE PLAYS ──────
 *
 * LOG ONLY. It changes no behavior, allocates nothing the game can see, and
 * every hook forwards straight to the real method.
 *
 * WHAT THE OWNER'S TEST ALREADY SETTLED. The TEST menu's own sound check plays.
 * That tone is built as a RAW stream object in the same mode as BGM, bypassing
 * the wrapper that gives SE and VOICE their different flags -- so the streaming
 * path works, and "DirectSound is broken" is dead. Two candidates survive:
 *
 *   (1) the BGM buffer is created and Play is REJECTED -- we would see a failing
 *       HRESULT;
 *   (2) Play SUCCEEDS and something Stops it immediately. The refill thread's
 *       exit path at 0x00432574 calls Stop (call [ecx+0x48] at 0x0043257A) as
 *       soon as its event is signaled, and a successful Play followed by a
 *       prompt Stop produces EXACTLY the same symptom as a rejected Play:
 *       silence, no error, nothing in any log.
 *
 * Nothing currently distinguishes those two, which is the entire reason this
 * exists. That is also why Stop is instrumented and not just Play -- an
 * instrument that watched only Play would call case (2) "working".
 *
 * WHY IT IS NOT IN audio_hook.c, WHICH ALREADY TRIES THIS. That file guards its
 * vtable patch with a single boolean (!s_ah_real_play), so it patches the FIRST
 * buffer vtable it sees -- almost certainly the primary buffer -- and is blind
 * to every other one thereafter. Its 45,886-line log contains zero Play and
 * zero Lock lines as a result, and its comment concluding "the game never calls
 * Play" is an inference drawn from its own blind spot. This keeps a TABLE of
 * patched vtables instead. (It is also in the hook DLL, so fixing it there
 * would cost a module rebuild and a re-sign that Contra would inherit.)
 *
 * THE SINGLETON. 0x006DE5E0 holds the SoundManager pointer -- 6 references in
 * the image, read at 0x0042559F and re-read at 0x004255BF, and corroborated by
 * 0x00432EC0 which does `call 0x00425590; mov eax,[eax+4]; call [ecx+0x10]`
 * (IDirectSound8::GetCaps). So [singleton+4] is the IDirectSound8*.
 * It lives in .data's BSS tail (vsize 0x42A64 ends at 0x0070BA64), so it is
 * zero until the game creates it -- hence the poll rather than a one-shot read.
 */
#define SHG_SND_MAX_VTBL 16

typedef struct {
    DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
    void *lpwfxFormat;
    GUID  guid3DAlgorithm;
} SHG_DSBUFFERDESC;

typedef struct {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} SHG_WAVEFORMATEX;

typedef HRESULT (__stdcall *pfnDS_CreateSoundBuffer)(void *, const SHG_DSBUFFERDESC *, void **, void *);
typedef HRESULT (__stdcall *pfnDSB_Play)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (__stdcall *pfnDSB_Stop)(void *);

static pfnDS_CreateSoundBuffer s_snd_real_create = NULL;

/* one entry per distinct buffer vtable -- NOT a single boolean, see above */
static void  *s_snd_vtbl[SHG_SND_MAX_VTBL];
static pfnDSB_Play s_snd_real_play[SHG_SND_MAX_VTBL];
static pfnDSB_Stop s_snd_real_stop[SHG_SND_MAX_VTBL];
static int    s_snd_vtbl_n = 0;

static int shg_snd_slot_for(void **vtbl) {
    int i;
    for (i = 0; i < s_snd_vtbl_n; ++i) if (s_snd_vtbl[i] == (void *)vtbl) return i;
    return -1;
}

static HRESULT __stdcall shg_DSB_Play(void *self, DWORD r1, DWORD prio, DWORD flags) {
    void **vt = *(void ***)self;
    int slot = shg_snd_slot_for(vt);
    HRESULT hr = (slot >= 0 && s_snd_real_play[slot])
               ? s_snd_real_play[slot](self, r1, prio, flags) : 0;
    SLOG("SND: Play  buf=%p flags=0x%08lX%s -> hr=0x%08lX%s",
         self, (unsigned long)flags,
         (flags & 1) ? " LOOPING" : "",
         (unsigned long)hr, FAILED(hr) ? "   *** FAILED" : "");
    return hr;
}

static HRESULT __stdcall shg_DSB_Stop(void *self) {
    void **vt = *(void ***)self;
    int slot = shg_snd_slot_for(vt);
    HRESULT hr = (slot >= 0 && s_snd_real_stop[slot]) ? s_snd_real_stop[slot](self) : 0;
    /* The interesting line. A Stop arriving within a few hundred ms of a
     * successful Play IS the answer -- see candidate (2) above. */
    SLOG("SND: Stop  buf=%p -> hr=0x%08lX", self, (unsigned long)hr);
    return hr;
}

static void shg_snd_hook_buffer(void *buf) {
    void **vt;
    DWORD old;
    int slot;
    if (!buf) return;
    vt = *(void ***)buf;
    if (!vt || IsBadReadPtr(vt, sizeof(void *) * 20)) return;
    if (shg_snd_slot_for(vt) >= 0) return;                 /* already patched */
    if (s_snd_vtbl_n >= SHG_SND_MAX_VTBL) {
        SLOG("SND: vtable table full (%d) -- a buffer class is going unwatched",
             SHG_SND_MAX_VTBL);
        return;
    }
    slot = s_snd_vtbl_n++;
    s_snd_vtbl[slot]      = (void *)vt;
    s_snd_real_play[slot] = (pfnDSB_Play)vt[12];           /* IDirectSoundBuffer::Play */
    s_snd_real_stop[slot] = (pfnDSB_Stop)vt[18];           /* IDirectSoundBuffer::Stop */
    if (VirtualProtect(&vt[12], sizeof(void *) * 7, PAGE_EXECUTE_READWRITE, &old)) {
        vt[12] = (void *)shg_DSB_Play;
        vt[18] = (void *)shg_DSB_Stop;
        VirtualProtect(&vt[12], sizeof(void *) * 7, old, &old);
        SLOG("SND: buffer vtable #%d hooked (vtbl=%p play=%p stop=%p)",
             slot, (void *)vt, (void *)s_snd_real_play[slot], (void *)s_snd_real_stop[slot]);
    } else {
        s_snd_vtbl_n--;
        SLOG("SND: VirtualProtect failed on buffer vtable %p err=%lu",
             (void *)vt, (unsigned long)GetLastError());
    }
}

static HRESULT __stdcall shg_DS_CreateSoundBuffer(void *self, const SHG_DSBUFFERDESC *desc,
                                                  void **ppbuf, void *unk)
{
    HRESULT hr = s_snd_real_create ? s_snd_real_create(self, desc, ppbuf, unk) : 0;
    if (desc && !IsBadReadPtr(desc, sizeof *desc)) {
        const SHG_WAVEFORMATEX *w = (const SHG_WAVEFORMATEX *)desc->lpwfxFormat;
        if (w && !IsBadReadPtr(w, sizeof *w))
            SLOG("SND: CreateSoundBuffer bytes=%lu flags=0x%08lX fmt=%u ch=%u %luHz %ubit -> hr=0x%08lX%s",
                 (unsigned long)desc->dwBufferBytes, (unsigned long)desc->dwFlags,
                 (unsigned)w->wFormatTag, (unsigned)w->nChannels,
                 (unsigned long)w->nSamplesPerSec, (unsigned)w->wBitsPerSample,
                 (unsigned long)hr, FAILED(hr) ? "   *** FAILED" : "");
        else
            SLOG("SND: CreateSoundBuffer bytes=%lu flags=0x%08lX (no format) -> hr=0x%08lX%s",
                 (unsigned long)desc->dwBufferBytes, (unsigned long)desc->dwFlags,
                 (unsigned long)hr, FAILED(hr) ? "   *** FAILED" : "");
    }
    if (SUCCEEDED(hr) && ppbuf && *ppbuf) shg_snd_hook_buffer(*ppbuf);
    return hr;
}

/* Polls the singleton until the game has built its SoundManager, then hooks
 * IDirectSound8::CreateSoundBuffer (vtable slot 3) once. Gives up after ~60 s
 * so it cannot spin for the life of the process. */
static void shg_snd_probe_tick(void) {
    static int s_done = 0, s_gaveup = 0;
    static DWORD s_t0 = 0;
    void **mgr, *ds, **vt;
    DWORD old;

    if (s_done || s_gaveup) return;
    if (!shg_env_int("ULTRA_SHG_SOUND_PROBE", 0)) { s_gaveup = 1; return; }
    if (!s_t0) s_t0 = GetTickCount();
    if (GetTickCount() - s_t0 > 60000) {
        s_gaveup = 1;
        SLOG("SND: SoundManager never appeared at 0x006DE5E0 after 60s -- giving up");
        return;
    }
    if (IsBadReadPtr((void *)0x006DE5E0u, sizeof(void *))) return;
    mgr = *(void ***)0x006DE5E0u;
    if (!mgr || IsBadReadPtr(mgr, sizeof(void *) * 2)) return;
    ds = mgr[1];                                   /* [SoundManager+4] = IDirectSound8* */
    if (!ds || IsBadReadPtr(ds, sizeof(void *))) return;
    vt = *(void ***)ds;
    if (!vt || IsBadReadPtr(vt, sizeof(void *) * 8)) return;

    s_snd_real_create = (pfnDS_CreateSoundBuffer)vt[3];
    if (VirtualProtect(&vt[3], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        vt[3] = (void *)shg_DS_CreateSoundBuffer;
        VirtualProtect(&vt[3], sizeof(void *), old, &old);
        s_done = 1;
        SLOG("SND: IDirectSound8::CreateSoundBuffer hooked (ds=%p real=%p) -- "
             "every buffer from here on is watched, Play AND Stop",
             ds, (void *)s_snd_real_create);
    } else {
        s_gaveup = 1;
        SLOG("SND: VirtualProtect failed on the IDirectSound8 vtable err=%lu",
             (unsigned long)GetLastError());
    }
}

/* ── KSHG.exe INTERNAL RESOLUTION ─────────────────────────────────────────
 *
 * WHY THIS LIVES HERE AND NOT IN ULTRA_PATCH_FILE.
 *
 * shaiolib.dll is a STATIC import of KSHG.exe, so this DllMain runs during
 * loader init -- before the game's entry point, therefore before the boot
 * display-mode choice at 0x0042476C and before the render-target table is
 * built at 0x00416130. That is the same timing the container's patch file
 * gets, without its one drawback: apply_patch_file() in dongle_bypass.c does
 * fopen("ultra_patch.log", "w") on a RELATIVE path, and this game's working
 * directory is the read-only dump folder. Doing it here writes nothing there.
 *
 * Nothing is written to KSHG.exe on disk either -- these are runtime memory
 * writes, so the exe_sha256 pinned in silenthill.ufw stays valid.
 *
 * WHY THE GAME WAS STUCK AT 640x480, which is not what anyone assumed.
 *
 * It boots EXCLUSIVE FULLSCREEN. 0x0042476C calls the fullscreen combo-finder
 * at 0x004071C0 unconditionally, which sets m_bWindowed = 0 (0x00407441). The
 * 640x480 client rect everyone kept measuring was D3D9 owning the window in
 * exclusive mode -- not the game choosing a small window. That also explains
 * two things that had never added up:
 *   - our own CreateDevice failing with D3DERR_DEVICELOST, which is exactly
 *     what you get when another device already holds the adapter exclusively;
 *   - konami_render9.c's SetWindowPos appearing to "work" while changing
 *     nothing visible, because in exclusive mode it is cosmetic.
 *
 * There is NO backbuffer constant to patch: 0x00406C50 builds the present
 * parameters and, in WINDOWED mode, takes BackBufferWidth/Height straight from
 * GetClientRect. So the backbuffer follows the window for free -- once the game
 * is windowed.
 *
 * WHY (a) AND (b) ARE ONE STEP AND MUST NOT BE SPLIT.
 *
 * The game captures its window rect ONCE, with GetWindowRect at 0x00407753,
 * at device-creation time -- and its WM_SIZE handler restores exactly that rect
 * forever afterwards (SetWindowPos at 0x00407FC4). So resizing the window from
 * OUTSIDE, which is what render9 does today, self-cancels the moment the game
 * resets. A window BORN at the target size does not. Ship them together or the
 * launch is wasted.
 *
 * LEVELS
 *   0 (default)  nothing is patched; identical to every build before today
 *   1            boot windowed + window born at the target size
 *   2            level 1 plus the internal render targets
 * Level 2 is deliberately separate: level 1 is the part that cannot fail
 * silently, and the HUD verdict from level 1 decides whether 2 is worth it.
 */
static void shg_patch_bytes(ULONG_PTR va, const unsigned char *b, size_t n,
                            const char *what)
{
    DWORD old;
    if ((ULONG_PTR)GetModuleHandleA(NULL) != 0x00400000u) {
        SLOG("res-patch: host is not KSHG at 0x400000 -- refusing to write %s", what);
        return;
    }
    if (IsBadReadPtr((void *)va, n)) {
        SLOG("res-patch: %08X unreadable -- skipping %s", (unsigned)va, what);
        return;
    }
    if (!VirtualProtect((void *)va, n, PAGE_EXECUTE_READWRITE, &old)) {
        SLOG("res-patch: VirtualProtect FAILED at %08X (%s) err=%lu",
             (unsigned)va, what, (unsigned long)GetLastError());
        return;
    }
    memcpy((void *)va, b, n);
    VirtualProtect((void *)va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)va, n);
    SLOG("res-patch: %-24s applied at %08X (%u bytes)",
         what, (unsigned)va, (unsigned)n);
}

static void shg_apply_internal_res(void)
{
    const char *v = getenv("ULTRA_SHG_INTERNAL_RES");
    int lvl = (v && *v) ? atoi(v) : 0;

    if (lvl < 1) { SLOG("res-patch: not armed (ULTRA_SHG_INTERNAL_RES unset or 0)"); return; }
    SLOG("res-patch: arming level %d -- target 1440x1080", lvl);

    /* (a) BOOT WINDOWED.
     *   0042476C  E8 4F 2A FE FF   call 0x004071C0   ; fullscreen combo-finder
     * retargeted to 0x00406FA0, the WINDOWED finder (m_bWindowed = 1 at
     * 0x00407143). Same __thiscall, same three stack arguments, both ret 0xC,
     * so the call site needs no other change.
     *   rel32 = 0x00406FA0 - 0x00424771 = 0xFFFE282F */
    {
        static const unsigned char b[5] = { 0xE8, 0x2F, 0x28, 0xFE, 0xFF };
        shg_patch_bytes(0x0042476Cu, b, 5, "boot-windowed");
    }

    /* (b) WINDOW BORN AT 1440x1080 CLIENT.
     *   004D048E  68 E0 01 00 00  push 0x1E0   ; client HEIGHT 480 -> 1080
     *   004D0493  68 80 02 00 00  push 0x280   ; client WIDTH  640 -> 1440
     * These are CLIENT dimensions -- stored to this+0x2c / this+0x28 at
     * 0x004D03D1 / 0x004D03CC, with frame metrics added later at 0x004D02D0.
     * 1440x1080 is exact 4:3 and 1:1 vertically on a 1080p panel: full height,
     * pillarboxed, no resampling. NOT 1920x1080 -- the projection derives
     * horizontal FOV from the live viewport (0x0042AD50), so 16:9 would widen
     * the field of view rather than letterbox it. */
    {
        static const unsigned char h[4] = { 0x38, 0x04, 0x00, 0x00 };  /* 1080 */
        static const unsigned char w[4] = { 0xA0, 0x05, 0x00, 0x00 };  /* 1440 */
        shg_patch_bytes(0x004D048Fu, h, 4, "window-client-height");
        shg_patch_bytes(0x004D0494u, w, 4, "window-client-width");
    }

    if (lvl < 2) {
        SLOG("res-patch: level 1 only -- the scene still renders into 640x480 "
             "offscreen targets. Set ULTRA_SHG_INTERNAL_RES=2 for the real upscale.");
        return;
    }

    /* (c) THE ACTUAL INTERNAL UPSCALE -- the render-target table built at
     * 0x00416130 (7 slots, stride 0x18, base this+0x104):
     *     slot0 +0x104  A16B16G16R16F   640x480    HDR scene buffer
     *     slot1 +0x11C  R16F           1024x1024   shadow map   <- LEAVE ALONE
     *     slot2-4                       128x128    bloom chain  <- LEAVE ALONE
     *     slot5 +0x17C  X8R8G8B8        640x480    full-frame
     *     slot6 +0x194  X8R8G8B8        640x480    full-frame
     *
     * edi is loaded ONCE at 0x004161C9 and stored as the height of slots 0, 5
     * and 6, so a single patch moves all three. The two depth-stencils are
     * sized from slot0 at 0x00412CAA and follow automatically.
     *
     * The shadow map and bloom chain are deliberately untouched: bloom is
     * low-frequency by design and gains nothing from resolution.
     *
     * HONESTLY LABELED: that the 3D is fully resolution-independent is STRONG,
     * not verified. Projection-from-live-viewport at 0x0042AD50 is confirmed,
     * but 0x00406D80 is a purpose-built SetViewport(dev, w, h) helper whose
     * address sits in two .rdata pointer tables and which nobody has traced.
     * The conclusion probably survives -- D3D9 resets the viewport to the full
     * target on SetRenderTarget -- but it is an inference, and the 2D/HUD layer
     * is the part most likely to misbehave. */
    {
        static const unsigned char w[4]  = { 0xA0, 0x05, 0x00, 0x00 };  /* 1440 */
        static const unsigned char hh[4] = { 0x38, 0x04, 0x00, 0x00 };  /* 1080 */
        shg_patch_bytes(0x004161A5u, w,  4, "rt-slot0-width");
        shg_patch_bytes(0x004161CAu, hh, 4, "rt-shared-height");
        shg_patch_bytes(0x00416226u, w,  4, "rt-slot5-width");
        shg_patch_bytes(0x00416236u, w,  4, "rt-slot6-width");
    }
}

/* THE CROSSHAIR IS NOT DRAWN HERE, AND CANNOT BE.
 *
 * A first attempt drew it in shg_D3DDevPresent below. It never ran once: this
 * DLL never obtains a D3D9 device at all. The 17:10 log is unambiguous --
 *   render-thread: game has not called Direct3DCreate9 after 5s -- bootstrapping (1..3 of 3)
 *   render-thread: D3D9 bootstrap FAILED 3 times -- giving up
 *   render-thread: D3D device not ready after 120s, aborting
 * and "D3DPresent: frame#" appeared ZERO times while the game rendered happily
 * throughout.
 *
 * The reason is an IAT race this project has hit before: the board hook DLL
 * patches Direct3DCreate9 first, so IT receives the game's IDirect3D9 and IT
 * ends up owning the device. Its log proves it holds the real one:
 *   D3D9: Direct3DCreate9(0x20) -> 00aaeb00 OK
 *   D3D9: CreateDevice bb=640x480 ... windowed=0
 *   D3D9: device created -> Present+Reset hooked
 *
 * So the drawing lives in the hook (d3d9_throttle.c, Hook_Present9) and this
 * file's job is only to say WHERE the gun is pointing. That is published
 * through the export below, which the hook resolves by name at runtime -- no
 * shared memory, no link-time dependency between the two DLLs, and no
 * initialization order to get wrong.
 */

/* Published by shaiolib_GUN_status every frame: exactly what the game was told,
 * so the crosshair cannot drift away from where the shot will actually land.
 * Returns 1 when that player's gun is live, 0 when it is not (and the hook then
 * draws nothing, which is what an inactive gun should look like). */
__declspec(dllexport) int ultra_shg_get_crosshair(int player, int *x, int *y,
                                                  int *aim_w, int *aim_h)
{
    if (player < 0 || player > 1) return 0;
    if (aim_w) *aim_w = shg_env_int("ULTRA_SHG_AIM_W", 640);
    if (aim_h) *aim_h = shg_env_int("ULTRA_SHG_AIM_H", 480);
    if (!g_xh_live[player]) return 0;
    if (x) *x = (int)g_xh_x[player];
    if (y) *y = (int)g_xh_y[player];
    return 1;
}

static HRESULT __stdcall shg_D3DDevPresent(
    void *pDev, const RECT *pSrc, const RECT *pDst,
    HWND hWnd, const void *pDirty)
{
    LONG n = InterlockedIncrement(&s_present_count);
    if (n == 1 || !(n % 120))
        SLOG("D3DPresent: frame#%ld", (long)n);
    return s_real_D3DDevPrst ? s_real_D3DDevPrst(pDev, pSrc, pDst, hWnd, pDirty) : 0;
}

static HRESULT __stdcall shg_D3DBeginScene(void *pDev) {
    LONG n = InterlockedIncrement(&s_begin_count);
    if (n == 1 || !(n % 300))
        SLOG("D3DBeginScene: frame#%ld", (long)n);
    return s_real_D3DBegin ? s_real_D3DBegin(pDev) : 0;
}

static HRESULT __stdcall shg_D3DEndScene(void *pDev) {
    static volatile LONG s_end_count = 0;
    LONG n = InterlockedIncrement(&s_end_count);
    if (n == 1 || !(n % 60))
        SLOG("D3DEndScene: frame#%ld", (long)n);
    return s_real_D3DEnd ? s_real_D3DEnd(pDev) : 0;
}

static HRESULT __stdcall shg_SCPresent(void *pSC, const RECT *pSrc, const RECT *pDst,
                                        HWND hWnd, const void *pDirty, DWORD dwFlags) {
    LONG n = InterlockedIncrement(&s_sc_present_count);
    if (n == 1 || !(n % 120))
        SLOG("SwapChainPresent: frame#%ld flags=0x%lX", (long)n, (unsigned long)dwFlags);
    return s_real_SCPresent ? s_real_SCPresent(pSC, pSrc, pDst, hWnd, pDirty, dwFlags) : 0;
}

static HRESULT __stdcall shg_D3DGetSwapChain(void *pDev, UINT idx, void **ppSC) {
    HRESULT hr = s_real_D3DGetSC ? s_real_D3DGetSC(pDev, idx, ppSC) : 0x80004005L;
    SLOG("GetSwapChain: idx=%u ret=0x%08lX sc=%p", idx, (unsigned long)(DWORD)hr, ppSC ? *ppSC : NULL);
    if (!hr && ppSC && *ppSC && !s_real_SCPresent) {
        void **svtbl = *(void ***)(*ppSC);
        DWORD old;
        if (VirtualProtect(&svtbl[4], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            s_real_SCPresent = (pfnSCPresent)svtbl[4];
            svtbl[4] = (void*)shg_SCPresent;
            VirtualProtect(&svtbl[4], sizeof(void*), old, &old);
            SLOG("SwapChain: vtbl[4]=Present hooked (real=%p)", (void*)s_real_SCPresent);
        }
    }
    return hr;
}

/* ARITY. ONE stdcall argument. Do not add a second one.
 *
 * 2026-09-17: this function used to take (const void *riid, void **ppEax).
 * The real EaxMan.dll export at 0x100012B0 takes ONE argument and ends
 * `ret $0x4` on both paths; KSHG.exe's single call site at 0x0043111A pushes
 * exactly one (`lea 0x8(%edi),%eax; push %eax`). Declaring two __stdcall
 * parameters made GCC emit `ret $8`, so this function returned with ESP four
 * bytes HIGH and took its return address from the enclosing frame's arg1 --
 * which is the IDirectSound8 object pointer. The game then "executed" the
 * DirectSound object.
 *
 * That is the fault this project chased for five runs and twice dismissed as a
 * benign first-chance exception:
 *
 *   FAULT: PRIVILEGED_INSTRUCTION (0xC0000096) at <dsound obj>+3
 *   FAULT: bytes at EIP: 6C 88 24 7C 6C 78 24 7C ...    ("...IUNKNOWN")
 *
 * The +3 is the high byte of the object's vtable pointer, so the exception
 * TYPE followed wherever dsound.dll happened to be based: 0x6C = INSB
 * (privileged) in most runs, 0x60 = PUSHAD (legal!) in the one run that then
 * limped on for 131 seconds -- and that run's ESP at fault is exactly 0x20
 * lower, which is precisely what PUSHAD pushes. Same bug, different luck.
 *
 * shg_veh (see the excode filter further down) admits only ACCESS_VIOLATION
 * and STACK_OVERFLOW into recovery, so the privileged-instruction variant was
 * never recoverable and took the process out with code 0xC0000096.
 *
 * This is the STUB-DLL ABI TRAP recorded in project memory, recurring: recover
 * a stub's true argument count from the caller's own pushes and the real
 * export's `ret N`, never from a plausible-looking signature. */
static HRESULT __stdcall shg_EaxManagerCreate(void **ppEax) {
    DWORD tid = GetCurrentThreadId();
    /* 2026-07-23: DO NOT call through to the real EaxMan.dll anymore.
     *
     * Evidence: two independent runs, each instrumented with a passive
     * kernel-level Win32 debugger (kshg_wdbg.exe -- DebugActiveProcess +
     * WaitForDebugEvent, purely observational, always continues with
     * DBG_EXCEPTION_NOT_HANDLED so game/VEH behavior is unaffected), both
     * show a severe self-sustaining ntdll-internal crash cascade starting in
     * the SAME tick this function returns, every time: a repeating
     * null-pointer access violation inside ntdll (same EIP, EAX=0, "read
     * [EAX+4]" pattern, 1612 occurrences in one 10s run) that this file's VEH
     * resume heuristics can only partially paper over, eventually ending in
     * a fatal crash AT KiUserExceptionDispatcher's own entry point (confirmed
     * via RVA lookup against the local ntdll.dll export table) -- i.e. the
     * corruption becomes severe enough that exception dispatch itself fails.
     * The prior code called through to the REAL EaxMan.dll (Creative Labs'
     * closed-source EAX manager), which expects real EAX-capable sound
     * hardware (Sound Blaster Live!/Audigy-class) to enumerate/initialize --
     * hardware this environment does not have. EAX has always been an
     * optional DirectSound *enhancement* for games of this era (not a hard
     * requirement), so failing this call should make KSHG.exe fall back to
     * plain DirectSound exactly as it would on any real PC without an
     * EAX-capable card -- the same "fail fast, never touch the real
     * implementation" pattern this file already uses for other
     * absent-hardware/absent-service calls (shg_IcmpSendEcho, shg_connect),
     * rather than trying to neutralize a crash inside closed-source code
     * after the fact. s_real_EaxCreate is still resolved in DllMain (harmless
     * GetProcAddress) so this is a one-line-of-logic, fully reversible change
     * if evidence later shows it doesn't help. */
    SLOG("EaxManagerCreate: SKIPPED (no real EAX hardware, avoids ntdll crash cascade -- "
         "see 2026-07-23 comment) tid=%lu -> E_FAIL", (unsigned long)tid);
    if (ppEax) *ppEax = NULL;
    return 0x80004005L; /* E_FAIL */
}

static void shg_hook_d3ddevice(void *pDev) {
    if (!pDev || s_d3ddev_vtable) return;
    void **vtbl = *(void ***)pDev;
    s_d3ddev_vtable = vtbl;
    DWORD old;
    /* vtable[14]=GetSwapChain - log real addr for diagnostics, DO NOT hook yet */
    SLOG("D3D9Dev: vtbl[14] (GetSwapChain) real=%p [not hooked]", vtbl[14]);
    /* vtable[17] = Present */
    if (VirtualProtect(&vtbl[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        s_real_D3DDevPrst = (pfnD3DDevPresent)vtbl[17];
        vtbl[17] = (void*)shg_D3DDevPresent;
        VirtualProtect(&vtbl[17], sizeof(void*), old, &old);
        SLOG("D3D9Dev: vtbl[17]=Present hooked (real=%p)", (void*)s_real_D3DDevPrst);
    }
    /* vtable[39] = BeginScene */
    if (VirtualProtect(&vtbl[39], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        s_real_D3DBegin = (pfnD3DBeginScene)vtbl[39];
        vtbl[39] = (void*)shg_D3DBeginScene;
        VirtualProtect(&vtbl[39], sizeof(void*), old, &old);
        SLOG("D3D9Dev: vtbl[39]=BeginScene hooked (real=%p)", (void*)s_real_D3DBegin);
    }
    /* vtable[40] = EndScene */
    if (VirtualProtect(&vtbl[40], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        s_real_D3DEnd = (pfnD3DEndScene)vtbl[40];
        vtbl[40] = (void*)shg_D3DEndScene;
        VirtualProtect(&vtbl[40], sizeof(void*), old, &old);
        SLOG("D3D9Dev: vtbl[40]=EndScene hooked (real=%p)", (void*)s_real_D3DEnd);
    }
}

/* ── D3D9 bootstrap - called by render thread when game never calls Direct3DCreate9 ──
 * 2026-07-31 fix for "Direct3DCreate9 never called" gap identified in STOCK_TAKE.md
 * The game's state machine gates Direct3DCreate9 behind real GHL hardware completing
 * its init chain. Since that never happens, we create a D3D9 device ourselves on the
 * game's own window so the render thread can proceed to call 0x425856. */
#ifndef D3D_SDK_VERSION
#define D3D_SDK_VERSION 32  /* d3d9.h value; game uses 32 (confirmed from vtable hook) */
#endif

/* Forward declarations for D3D functions defined further below */
static HRESULT __stdcall shg_D3D9CreateDevice(void*, UINT, DWORD, HWND, DWORD, SHG_D3DPP*, void**);
static void shg_hook_d3ddevice(void *pDev);  /* wrapper defined below */
static volatile LONG s_d3d9_bootstrap_done = 0;
static void shg_bootstrap_d3d9(void) {
    if (InterlockedExchange(&s_d3d9_bootstrap_done, 1)) return;
    if (!s_real_D3DC9) {
        SLOG("d3d9-bootstrap: s_real_D3DC9 not yet resolved -- skipping");
        InterlockedExchange(&s_d3d9_bootstrap_done, 0);
        return;
    }
    /* Get the game's window -- rgate_keeper found it */
    HWND hw = s_kshg_wnd;
    if (!hw || !IsWindow(hw)) {
        /* Fallback: scan for any window belonging to our process */
        hw = NULL;
        EnumWindows(shg_enum_windows_cb, (LPARAM)&hw);
    }
    if (!hw) {
        SLOG("d3d9-bootstrap: no game window yet, will retry");
        InterlockedExchange(&s_d3d9_bootstrap_done, 0);
        return;
    }
    SLOG("d3d9-bootstrap: calling Direct3DCreate9 ourselves (hwnd=%p)", (void*)hw);
    void *pD3D = s_real_D3DC9(D3D_SDK_VERSION);
    if (!pD3D) {
        SLOG("d3d9-bootstrap: Direct3DCreate9 returned NULL");
        InterlockedExchange(&s_d3d9_bootstrap_done, 0);
        return;
    }
    SLOG("d3d9-bootstrap: pD3D=%p, hooking vtable", pD3D);
    /* Hook CreateDevice on this IDirect3D9 instance */
    if (!s_d3d9_vtable) {
        void **vtbl = *(void ***)pD3D;
        s_d3d9_vtable = vtbl;
        DWORD old;
        if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            s_real_D3D9CD = (pfnD3D9CD_real)vtbl[16];
            vtbl[16] = (void*)shg_D3D9CreateDevice;
            VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
            SLOG("d3d9-bootstrap: vtbl[16]=CreateDevice hooked");
        }
    }
    /* Create the device - windowed, D3DFMT_X8R8G8B8, swap-discard */
    SHG_D3DPP pp;
    memset(&pp, 0, sizeof pp);
    RECT cr = {0}; GetClientRect(hw, &cr);
    pp.BackBufferWidth  = cr.right  > 0 ? (UINT)cr.right  : 640;
    pp.BackBufferHeight = cr.bottom > 0 ? (UINT)cr.bottom : 480;
    pp.BackBufferFormat = 22;  /* D3DFMT_X8R8G8B8 */
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = 1;   /* D3DSWAPEFFECT_DISCARD */
    pp.hDeviceWindow    = hw;
    pp.Windowed         = TRUE;
    pp.PresentationInterval = 0; /* D3DPRESENT_INTERVAL_DEFAULT */
    void *pDev = NULL;
    SLOG("d3d9-bootstrap: CreateDevice %dx%d hwnd=%p",
         pp.BackBufferWidth, pp.BackBufferHeight, (void*)hw);
    HRESULT hr = s_real_D3D9CD
        ? s_real_D3D9CD(pD3D, 0 /*D3DADAPTER_DEFAULT*/,
                         1 /*D3DDEVTYPE_HAL*/, hw,
                         0x40 /*D3DCREATE_HARDWARE_VERTEXPROCESSING*/,
                         &pp, &pDev)
        : (HRESULT)0x80004005L;
    if (SUCCEEDED(hr) && pDev) {
        SLOG("d3d9-bootstrap: device created ok dev=%p", pDev);
        s_d3d9_dev     = pDev;
        s_d3d9_dev_fwd = pDev;
        shg_hook_d3ddevice(pDev);
        s_end_fwd = (pfnD3DEndScene_fwd)   s_real_D3DEnd;
        s_prs_fwd = (pfnD3DDevPresent_fwd) s_real_D3DDevPrst;
        SLOG("d3d9-bootstrap: s_d3d9_dev_fwd set, render thread will proceed");
    } else {
        SLOG("d3d9-bootstrap: CreateDevice FAILED hr=0x%08lX -- trying software VP",
             (unsigned long)(DWORD)hr);
        /* Retry with software vertex processing (works on more configs) */
        hr = s_real_D3D9CD
            ? s_real_D3D9CD(pD3D, 0, 1, hw,
                             0x20 /*D3DCREATE_SOFTWARE_VERTEXPROCESSING*/,
                             &pp, &pDev)
            : (HRESULT)0x80004005L;
        if (SUCCEEDED(hr) && pDev) {
            SLOG("d3d9-bootstrap: software VP device ok dev=%p", pDev);
            s_d3d9_dev     = pDev;
            s_d3d9_dev_fwd = pDev;
            shg_hook_d3ddevice(pDev);
            s_end_fwd = (pfnD3DEndScene_fwd)   s_real_D3DEnd;
            s_prs_fwd = (pfnD3DDevPresent_fwd) s_real_D3DDevPrst;
        } else {
            SLOG("d3d9-bootstrap: BOTH CreateDevice attempts failed hr=0x%08lX",
                 (unsigned long)(DWORD)hr);
            InterlockedExchange(&s_d3d9_bootstrap_done, 0); /* allow retry */
        }
    }
    /* Release our IDirect3D9 ref - device holds its own ref */
    typedef ULONG (__stdcall *pfnRelease)(void*);
    void **d3d_vtbl = *(void***)pD3D;
    if (d3d_vtbl && d3d_vtbl[2]) ((pfnRelease)d3d_vtbl[2])(pD3D);
}



static HRESULT __stdcall shg_D3D9CreateDevice(
    void *pD3D, UINT Adapter, DWORD DevType, HWND hWnd,
    DWORD BehaviorFlags, SHG_D3DPP *pPP, void **ppDev)
{
    if (pPP) {
        SLOG("D3D9CreateDevice: %dx%d windowed=%d fmt=%lu devtype=%lu",
             pPP->BackBufferWidth, pPP->BackBufferHeight, pPP->Windowed,
             (unsigned long)pPP->BackBufferFormat, (unsigned long)DevType);
        if (!pPP->Windowed) {
            pPP->Windowed = TRUE;
            pPP->FullScreen_RefreshRateInHz = 0;
            SLOG("D3D9CreateDevice: forcing windowed mode");
        }
    }
    HRESULT hr = s_real_D3D9CD
        ? s_real_D3D9CD(pD3D, Adapter, DevType, hWnd, BehaviorFlags, pPP, ppDev)
        : (HRESULT)0x80004005L /*E_FAIL*/;
    SLOG("D3D9CreateDevice: ret=0x%08lX dev=%p",
         (unsigned long)(DWORD)hr, ppDev ? *ppDev : NULL);
    if (!hr && ppDev && *ppDev) {
        s_d3d9_dev = *ppDev;
        s_d3d9_dev_fwd = *ppDev;
        shg_hook_d3ddevice(*ppDev);
        /* Populate forward-declared ptrs so the render thread can call them */
        s_end_fwd = (pfnD3DEndScene_fwd)   s_real_D3DEnd;
        s_prs_fwd = (pfnD3DDevPresent_fwd) s_real_D3DDevPrst;
    }
    return hr;
}

static void* WINAPI shg_Direct3DCreate9(UINT sdkver) {
    SLOG("Direct3DCreate9: SDKVer=%u", sdkver);
    void *pD3D = s_real_D3DC9 ? s_real_D3DC9(sdkver) : NULL;
    SLOG("Direct3DCreate9: pD3D=%p", pD3D);
    if (pD3D && !s_d3d9_vtable) {
        void **vtbl = *(void ***)pD3D;
        s_d3d9_vtable = vtbl;
        DWORD old;
        /* vtable[16] = CreateDevice */
        if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            s_real_D3D9CD = (pfnD3D9CD_real)vtbl[16];
            vtbl[16] = (void*)shg_D3D9CreateDevice;
            VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
            SLOG("D3D9: vtbl[16]=CreateDevice hooked (real=%p)", (void*)s_real_D3D9CD);
        }
    }
    return pD3D;
}

/* GetProcAddress hook: intercept setupapi + hid lookups from KSHG.exe */
typedef FARPROC (WINAPI *pfnGPA)(HMODULE,LPCSTR);
static pfnGPA s_real_GPA = NULL;
static FARPROC WINAPI shg_GetProcAddress(HMODULE hm, LPCSTR name) {
    if (name && (ULONG_PTR)name > 0xFFFF) {
        if (!strcmp(name,"SetupDiGetClassDevsA"))             return (FARPROC)shg_SetupDiGetClassDevsA;
        if (!strcmp(name,"SetupDiGetClassDevsW"))             return (FARPROC)shg_SetupDiGetClassDevsW;
        if (!strcmp(name,"SetupDiGetClassDevs"))              return (FARPROC)shg_SetupDiGetClassDevsA;
        if (!strcmp(name,"SetupDiEnumDeviceInterfaces"))      return (FARPROC)shg_SetupDiEnumDeviceInterfaces;
        if (!strcmp(name,"SetupDiGetDeviceInterfaceDetailA")) return (FARPROC)shg_SetupDiGetDeviceInterfaceDetailA;
        if (!strcmp(name,"SetupDiGetDeviceInterfaceDetailW")) return (FARPROC)shg_SetupDiGetDeviceInterfaceDetailW;
        if (!strcmp(name,"SetupDiDestroyDeviceInfoList"))     return (FARPROC)shg_SetupDiDestroyDeviceInfoList;
        if (!strcmp(name,"HidD_GetAttributes"))               return (FARPROC)shg_HidD_GetAttributes;
        if (!strcmp(name,"HidD_GetPreparsedData"))            return (FARPROC)shg_HidD_GetPreparsedData;
        if (!strcmp(name,"HidD_FreePreparsedData"))           return (FARPROC)shg_HidD_FreePreparsedData;
        if (!strcmp(name,"HidP_GetCaps"))                    return (FARPROC)shg_HidP_GetCaps;
        /* Catch runtime GetProcAddress lookups for input functions too. */
        if (!strcmp(name,"GetAsyncKeyState"))                return (FARPROC)shg_GetAsyncKeyState;
        if (!strcmp(name,"GetKeyState"))                     return (FARPROC)shg_GetKeyState;
        /* Suppress shaiolib error dialogs from any dynamic caller. */
        if (!strcmp(name,"MessageBoxA"))                     return (FARPROC)shg_MessageBoxA;
        if (!strcmp(name,"MessageBoxW"))                     return (FARPROC)shg_MessageBoxW;
        /* Multimedia / timer callback intercepts (dynamic-load path). */
        if (!strcmp(name,"waveOutOpen"))                     return (FARPROC)shg_waveOutOpen;
        if (!strcmp(name,"timeSetEvent"))                    return (FARPROC)shg_timeSetEvent;
        if (!strcmp(name,"CreateTimerQueueTimer"))           return (FARPROC)shg_CreateTimerQueueTimer;
        if (!strcmp(name,"ChangeTimerQueueTimer"))           return (FARPROC)shg_CreateTimerQueueTimer;
        if (!strcmp(name,"QueueUserAPC"))                    return (FARPROC)shg_QueueUserAPC;
        /* D3D9 windowed-mode intercept: capture the real pointer on first call */
        if (!strcmp(name,"Direct3DCreate9")) {
            if (!s_real_D3DC9 && s_real_GPA)
                s_real_D3DC9 = (pfnD3DC9)(ULONG_PTR)s_real_GPA(hm, name);
            SLOG("GPA: Direct3DCreate9 → shg hook");
            return (FARPROC)shg_Direct3DCreate9;
        }
    }
    return s_real_GPA ? s_real_GPA(hm, name) : NULL;
}

/* ── MessageBox suppression ─────────────────────────────────────────────────
 * KSHG.exe shows "*** ERROR *** ERR shaiolib_initialize" via MessageBoxA even
 * when shaiolib_initialize returns 0 (success).  The dialog is non-fatal but
 * blocks execution until OK is clicked - making headless/unattended runs hang.
 * Intercept any MessageBox containing "shaiolib" or "ERR" and return IDOK. */
static DWORD WINAPI shg_msgbox_click_thread(LPVOID arg) {
    (void)arg;
    INPUT dn = {0}; dn.type=INPUT_MOUSE; dn.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    INPUT up = {0}; up.type=INPUT_MOUSE; up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
    int i;
    for (i = 0; i < 3; i++) {
        if (i > 0) Sleep(500);
        /* Arm auto-LMB window so shg_ReadFile trigger bit is set */
        g_auto_lmb_until = GetTickCount() + 200;
        SendInput(1, &dn, sizeof(INPUT));
        Sleep(150);
        SendInput(1, &up, sizeof(INPUT));
        g_auto_lmb_until = 0;
        SLOG("msgbox-click[%d]: LMB sent + auto_lmb armed", i);
    }
    return 0;
}

static int WINAPI shg_MessageBoxA(HWND hw, LPCSTR text, LPCSTR cap, UINT typ) {
    if ((text && (strstr(text,"shaiolib") || strstr(text,"ERR"))) ||
        (cap  && (strstr(cap, "shaiolib") || strstr(cap, "ERR")))) {
        SLOG("MessageBoxA suppressed: cap=\"%s\" text=\"%s\"",
             cap ? cap : "", text ? text : "");
        /* Spawn click thread and wait 5 seconds before returning IDOK.
         * The real user took several seconds to click OK; during that time
         * any background calibration thread can time out and mark hardware
         * as unavailable.  Returning IDOK instantly bypasses that timeout
         * and leaves the calibration thread still waiting.
         * 5 s covers known Konami hardware-detect timeouts (~3 s). */
        HANDLE ht = CreateThread(NULL, 0, shg_msgbox_click_thread, NULL, 0, NULL);
        if (ht) CloseHandle(ht);
        SLOG("MessageBoxA: returning IDOK immediately (no delay)");
        return IDOK;
    }
    return s_real_MBA ? s_real_MBA(hw, text, cap, typ) : IDOK;
}
static int WINAPI shg_MessageBoxW(HWND hw, LPCWSTR text, LPCWSTR cap, UINT typ) {
    char nt[256]={0}, nc[256]={0};
    if (text) WideCharToMultiByte(CP_ACP,0,text,-1,nt,255,NULL,NULL);
    if (cap)  WideCharToMultiByte(CP_ACP,0,cap, -1,nc,255,NULL,NULL);
    if ((nt[0] && (strstr(nt,"shaiolib") || strstr(nt,"ERR"))) ||
        (nc[0] && (strstr(nc,"shaiolib") || strstr(nc,"ERR")))) {
        SLOG("MessageBoxW suppressed: cap=\"%s\" text=\"%s\"", nc, nt);
        return IDOK;
    }
    return s_real_MBW ? s_real_MBW(hw, text, cap, typ) : IDOK;
}

/* ── CreateMutexA diagnostic hook ───────────────────────────────────────────
 * KSHG.exe directly imports CreateMutexA.  CreateMutex itself never blocks
 * the calling thread - it either creates a new mutex object or opens a handle
 * to an existing one (ERROR_ALREADY_EXISTS in GetLastError just means someone
 * else already owns it) - so this can't itself be the ~10s hang.  But the
 * mutex NAME is a useful clue about what single-instance/handshake mechanism
 * the game uses, and a subsequent WaitForSingleObject/NtWaitForSingleObject
 * on the returned handle (both already hooked elsewhere) would show up too.
 * Pure diagnostic passthrough: does not alter behavior. */
typedef HANDLE (WINAPI *pfnCreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static pfnCreateMutexA s_real_CMA = NULL;
static HANDLE WINAPI shg_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initial, LPCSTR name) {
    SLOG("CreateMutexA: name=\"%s\" initialOwner=%d tid=%lu",
         name ? name : "(unnamed)", (int)initial, (unsigned long)GetCurrentThreadId());
    HANDLE h = s_real_CMA ? s_real_CMA(sa, initial, name) : NULL;
    DWORD err = GetLastError();
    SLOG("CreateMutexA: name=\"%s\" -> h=%p err=%lu (%s)",
         name ? name : "(unnamed)", (void*)h, (unsigned long)err,
         err == ERROR_ALREADY_EXISTS ? "ALREADY_EXISTS" : "new/ok");
    return h;
}

/* ── LED / card (unused in GHL) ─────────────────────────────────────────── */
__declspec(dllexport) void shaiolib_LED_change(int n) { (void)n; }
__declspec(dllexport) int  shaiolib_CARD_start(void)  { SLOG("CARD_start -> 0"); return 0; }
/* CARD_status(int, void* out) -- TWO arguments, the second an OUT buffer.
 *
 *     4d1c14  lea ecx,[esp+0x14]
 *     4d1c18  push ecx                 ; arg1 = the buffer
 *     4d1c19  push edi                 ; arg0
 *     4d1c1a  call ds:0x6804b0
 *     4d1c20  add esp,8                ; cdecl, 2 args
 *     4d1c23  cmp [esp+0x14],ebx       ; buffer[0] == 0 ? -> no card
 *     4d1c29  lea eax,[esp+0x20]       ; else copy the id string at +0x0C
 *     4d1c30  mov cl,[eax] ... until NUL
 *
 * Declared (void) before, so the buffer was never written and the game tested
 * uninitialised stack. When that stale dword happened to be non-zero it took
 * the second branch and ran an UNBOUNDED byte copy from uninitialised memory
 * until it happened to find a NUL -- corruption whose blast radius depends on
 * stack leftovers, which is the worst kind to reproduce.
 *
 * Zeroing the buffer makes 'no card present' deterministic, which is also the
 * honest answer: there is no card reader attached. */
__declspec(dllexport) int  shaiolib_CARD_status(int which, void *out) {
    if ((size_t)out > 0x10000u) memset(out, 0, 0x20);   /* [0] = 0 -> no card */
    SLOG("CARD_status(%d) -> 0 (no card; buffer zeroed)", which);
    return 0;
}
__declspec(dllexport) void shaiolib_CARD_stop(void)   { SLOG("CARD_stop"); }

/* forward decl - defined in IAT hook section below */
static void shg_iat_hook_mod(HMODULE, const char *, const char *, void *);
static void shg_install_connect_inline(void);
static void shg_install_icmp_inline(void);
static DWORD WINAPI shg_MWFMO(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
static HMODULE WINAPI shg_LoadLibraryA(LPCSTR);
static HMODULE WINAPI shg_LoadLibraryW(LPCWSTR);
static HMODULE WINAPI shg_LoadLibraryExA(LPCSTR, HANDLE, DWORD);
static HMODULE WINAPI shg_LoadLibraryExW(LPCWSTR, HANDLE, DWORD);

/* ── LoadLibraryA hook - patch dynamically-loaded support DLLs ──────────────
 * libutil.dll is NOT statically imported by KSHG.exe; it's loaded at runtime
 * via LoadLibraryA.  Our DllMain-time attempt to patch its IAT fails ("not in
 * process").  Hook LoadLibraryA on KSHG.exe's IAT so that when libutil.dll
 * loads, we immediately patch its CFA / ReadFile / GPA IAT before any
 * calibration code runs.  DllMain finishes BEFORE LoadLibraryA returns, but
 * most SDK DLLs open HID devices lazily (not in DllMain) - so the IAT patch
 * still takes effect before the first HID CreateFileA call. */
static void shg_patch_dll_iat(HMODULE h, const char *base) {
    shg_iat_hook_mod(h, "kernel32.dll", "GetProcAddress",       shg_GetProcAddress);
    shg_iat_hook_mod(h, "kernel32.dll", "CreateFileA",          shg_CreateFileA);
    shg_iat_hook_mod(h, "kernel32.dll", "CreateFileW",          shg_CreateFileW);
    shg_iat_hook_mod(h, "kernel32.dll", "ReadFile",             shg_ReadFile);
    shg_iat_hook_mod(h, "kernel32.dll", "CloseHandle",          shg_CloseHandle);
    shg_iat_hook_mod(h, "kernel32.dll", "LoadLibraryA",         shg_LoadLibraryA);
    shg_iat_hook_mod(h, "kernel32.dll", "LoadLibraryW",         shg_LoadLibraryW);
    shg_iat_hook_mod(h, "kernel32.dll", "LoadLibraryExA",       shg_LoadLibraryExA);
    shg_iat_hook_mod(h, "kernel32.dll", "LoadLibraryExW",       shg_LoadLibraryExW);
    /* Cap Sleep/WFSO/WFMO so libbms.dll's e-amusement network wait doesn't stall. */
    shg_iat_hook_mod(h, "kernel32.dll", "Sleep",                shg_Sleep);
    shg_iat_hook_mod(h, "kernel32.dll", "WaitForSingleObject",  shg_WFSO);
    shg_iat_hook_mod(h, "kernel32.dll", "WaitForMultipleObjects", shg_WFMO);
    /* icmp.dll: patch direct static import if present. */
    shg_iat_hook_mod(h, "icmp.dll",     "IcmpSendEcho",         shg_IcmpSendEcho);
    shg_iat_hook_mod(h, "user32.dll",   "GetAsyncKeyState",     shg_GetAsyncKeyState);
    shg_iat_hook_mod(h, "user32.dll",   "GetKeyState",          shg_GetKeyState);
    SLOG("IAT patched: %s (GPA+CFA+CFW+RF+CH+LLA+LLW+LLExA+LLExW+Slp+WFSO+WFMO+Icmp+GAKS+GKS)", base);
}

static void shg_check_and_patch(HMODULE h, const char *base) {
    if (!h || !base) return;
    SLOG("LoadLibrary: \"%s\" -> %p", base, (void*)h);
    static const char *const s_targets[] = { "libutil.dll", "libbms.dll", NULL };
    const char *const *t;
    for (t = s_targets; *t; ++t)
        if (_stricmp(base, *t) == 0) { shg_patch_dll_iat(h, base); break; }
    /* ws2_32.dll loads lazily (not at process startup).  Install the connect()
     * inline hook as soon as it appears so network calls fail fast. */
    if (_stricmp(base, "ws2_32.dll") == 0 && !s_real_Connect)
        shg_install_connect_inline();
    /* icmp.dll loads lazily too - install inline hook immediately on load. */
    if (_stricmp(base, "icmp.dll") == 0 && !s_real_IcmpSE)
        shg_install_icmp_inline();
}

static HMODULE WINAPI shg_LoadLibraryA(LPCSTR name) {
    HMODULE h = s_real_LLA ? s_real_LLA(name) : NULL;
    if (name) {
        const char *base = strrchr(name, '\\');
        shg_check_and_patch(h, base ? base + 1 : name);
    }
    return h;
}

static HMODULE WINAPI shg_LoadLibraryW(LPCWSTR name) {
    HMODULE h = s_real_LLW ? s_real_LLW(name) : NULL;
    if (name) {
        char base8[MAX_PATH] = {0};
        const WCHAR *b = wcsrchr(name, L'\\');
        WideCharToMultiByte(CP_ACP, 0, b ? b+1 : name, -1, base8, MAX_PATH-1, NULL, NULL);
        shg_check_and_patch(h, base8);
    }
    return h;
}

/* LoadLibraryExA/W - intercept delay-load thunks and explicit Ex calls.
 * icmp.dll is loaded via LoadLibraryExA by the delay-load stub, NOT via
 * LoadLibraryA, so we must hook these too. */
static HMODULE WINAPI shg_LoadLibraryExA(LPCSTR name, HANDLE hf, DWORD flags) {
    HMODULE h = s_real_LLEA ? s_real_LLEA(name, hf, flags) : NULL;
    /* Only patch when actually mapping the DLL (LOAD_LIBRARY_AS_DATAFILE etc skip). */
    if (name && !(flags & 0x62)) { /* 0x62 = DATAFILE|IMAGE|RESOURCE bits */
        const char *base = strrchr(name, '\\');
        shg_check_and_patch(h, base ? base + 1 : name);
    }
    return h;
}

static HMODULE WINAPI shg_LoadLibraryExW(LPCWSTR name, HANDLE hf, DWORD flags) {
    HMODULE h = s_real_LLEW ? s_real_LLEW(name, hf, flags) : NULL;
    if (name && !(flags & 0x62)) {
        char base8[MAX_PATH] = {0};
        const WCHAR *b = wcsrchr(name, L'\\');
        WideCharToMultiByte(CP_ACP, 0, b ? b+1 : name, -1, base8, MAX_PATH-1, NULL, NULL);
        shg_check_and_patch(h, base8);
    }
    return h;
}

/* NOTE: function-byte (JMP) detours are NOT used for kernel32!Sleep /
 * WaitForSingleObject - those start with indirect-JMP stubs (FF 25 ...) that
 * span > 5 bytes; a 5-byte copy produces an invalid trampoline that crashes.
 * winmm!timeSetEvent uses the classic hot-patch prolog (8B FF 55 8B EC) which
 * IS safe to inline-hook, and we must do so: kacbookkeeping.dll caches the real
 * timeSetEvent address at its own DllMain (before ours runs) and calls it
 * directly, bypassing our IAT hook.  Inline-hooking the function bytes
 * intercepts ALL callers regardless of how they obtained the address.          */
static void shg_install_tse_inline(void) {
    HMODULE hw = GetModuleHandleA("winmm.dll");
    if (!hw) { SLOG("inline-TSE: winmm.dll not loaded"); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hw, "timeSetEvent");
    if (!fn) { SLOG("inline-TSE: GetProcAddress failed"); return; }
    SLOG("inline-TSE: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    /* Skip indirect-JMP stubs (FF 25 = JMP [mem32]) and short JMPs. */
    if (fn[0] == 0xFF || fn[0] == 0xE9 || fn[0] == 0xEB) {
        SLOG("inline-TSE: JMP stub at fn[0]=0x%02X, IAT-only path", fn[0]);
        return;
    }
    /* Trampoline: [5 saved bytes] [JMP rel32 → fn+5]. */
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("inline-TSE: VirtualAlloc failed %lu", (unsigned long)GetLastError()); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    /* Patch fn[0..4]: JMP shg_timeSetEvent. */
    DWORD old_p = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p)) {
        SLOG("inline-TSE: VirtualProtect failed %lu", (unsigned long)GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return;
    }
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_timeSetEvent - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    s_real_TSE = (pfnTimeSetEvent)tramp;
    SLOG("inline-TSE: installed → shg_timeSetEvent=%p tramp=%p",
         (void*)shg_timeSetEvent, (void*)tramp);
}

/* ── ws2_32!connect inline hook ─────────────────────────────────────────────
 * Intercepts ALL connect() callers (kacbookkeeping, any audio/network DLL)
 * so e-amusement TCP timeouts (~120 s each) don't stall the main thread.    */
static void shg_install_connect_inline(void) {
    HMODULE hw = GetModuleHandleA("ws2_32.dll");
    if (!hw) { SLOG("connect-inline: ws2_32.dll not in process"); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hw, "connect");
    if (!fn) { SLOG("connect-inline: connect export not found"); return; }
    SLOG("connect-inline: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    if (fn[0] == 0xFF || fn[0] == 0xE9 || fn[0] == 0xEB) {
        SLOG("connect-inline: already a JMP (0x%02X), skip inline, IAT-only", fn[0]);
        return;
    }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("connect-inline: VirtualAlloc failed %lu", (unsigned long)GetLastError()); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p)) {
        SLOG("connect-inline: VirtualProtect failed %lu", (unsigned long)GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return;
    }
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_connect - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    s_real_Connect = (pfnConnect)(ULONG_PTR)tramp;
    SLOG("connect-inline: installed → shg_connect=%p tramp=%p",
         (void*)shg_connect, (void*)tramp);
}

/* ── icmp.dll!IcmpSendEcho inline hook ──────────────────────────────────────
 * KSHG.exe either delay-loads icmp.dll or loads it at runtime via
 * LoadLibraryA - the static IAT is unpopulated at our DllMain time, so
 * shg_iat_hook("icmp.dll",...) silently fails.  Patch the function bytes
 * directly so the hook intercepts ALL callers (IAT, cached pointer, GPA).   */
static void shg_install_icmp_inline(void) {
    HMODULE hw = GetModuleHandleA("icmp.dll");
    if (!hw) { SLOG("icmp-inline: icmp.dll not in process"); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hw, "IcmpSendEcho");
    if (!fn) { SLOG("icmp-inline: IcmpSendEcho not found"); return; }
    SLOG("icmp-inline: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    if (fn[0] == 0xFF || fn[0] == 0xE9 || fn[0] == 0xEB) {
        SLOG("icmp-inline: already a JMP (0x%02X), skip", fn[0]);
        return;
    }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("icmp-inline: VirtualAlloc failed %lu", (unsigned long)GetLastError()); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p)) {
        SLOG("icmp-inline: VirtualProtect failed %lu", (unsigned long)GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return;
    }
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_IcmpSendEcho - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    s_real_IcmpSE = (pfnIcmpSE)(ULONG_PTR)tramp;
    SLOG("icmp-inline: installed → shg_IcmpSendEcho=%p tramp=%p",
         (void*)shg_IcmpSendEcho, (void*)tramp);
}

/* ── ntdll!NtWaitForSingleObject inline hook ────────────────────────────────
 * The game's post-VEH attract loop at 0x00656FE3 calls directly into ntdll
 * via a DLL that bypasses our kernel32 IAT hooks.  Inline-hook ntdll's own
 * NtWaitForSingleObject so ALL callers are intercepted regardless of path.
 * After VEH: log handle + timeout (ms), cap to 300ms to prevent multi-second
 * parks that block the render loop from ever starting.                       */
typedef LONG (NTAPI *pfnNtWFSO_nt)(HANDLE, BOOLEAN, LONGLONG *);
static pfnNtWFSO_nt s_real_NtWFSO_nt = NULL;
static volatile LONG s_ntwfso_cnt = 0;

static LONG NTAPI shg_NtWFSO(HANDLE h, BOOLEAN alert, LONGLONG *ptimeout) {
    LONGLONG ms_val = -1;
    if (ptimeout) {
        LONGLONG t = *ptimeout;
        ms_val = (t < 0) ? ((-t) / 10000LL) : (t / 10000LL);
    }
    LONG cnt = InterlockedIncrement(&s_ntwfso_cnt);
    /* Log first 20 unconditionally to confirm hook is live; then gate on g_veh_fired */
    if (cnt <= 20 || (g_veh_fired && (cnt <= 300 || !(cnt % 50000))))
        SLOG("NtWFSO[%ld]: h=%p alert=%d timeout=%I64dms veh=%d", (long)cnt, (void*)h, (int)alert, ms_val, g_veh_fired);
    /* Post-VEH: fake WAIT_OBJECT_0 for ALL threads' INFINITE NtWFSO waits.
       The render thread blocks on NtWFSO(INFINITE) waiting for a GHL security
       event that is never signaled without real hardware.  Returning WAIT_OBJECT_0
       immediately tells every waiting thread "your event fired, proceed".
       Previously this was scoped to the game thread only, but the render thread
       (a separate TID) needs the same treatment or BeginScene never fires.
       NOTE: returning WAIT_TIMEOUT (0x102) instead caused the game to detect
       "hardware failure" and call ExitProcess; WAIT_OBJECT_0 is safe here. */
    LONGLONG capped;
    DWORD cur_tid = GetCurrentThreadId();
    int is_game_thread = (s_game_thread_id != 0 && cur_tid == s_game_thread_id);
    if (g_veh_fired && (!ptimeout || ms_val < 0)) {
        if (!is_game_thread) {
            static volatile LONG s_ngg_cnt = 0;
            LONG ngg = InterlockedIncrement(&s_ngg_cnt);
            if (ngg <= 5 || !(ngg % 50000))
                SLOG("NtWFSO[%ld]: fake WAIT_OBJECT_0 h=%p (non-game tid=%lu INFINITE) [total=%ld]",
                     (long)cnt, (void*)h, (unsigned long)cur_tid, (long)ngg);
        }
        return 0L;  /* WAIT_OBJECT_0 - skip real wait, simulate signal */
    }
    /* For finite timeouts > 300ms, cap to 50ms so the game stays responsive. */
    if (g_veh_fired && ms_val > 300) {
        capped = -50LL * 10000LL;
        ptimeout = &capped;
    }
    LONG st = s_real_NtWFSO_nt ? s_real_NtWFSO_nt(h, alert, ptimeout) : 0x102L;
    if (cnt <= 20 || (g_veh_fired && (cnt <= 300 || !(cnt % 50000))))
        SLOG("NtWFSO[%ld]: ret=0x%08lX", (long)cnt, (unsigned long)(DWORD)st);
    /* Render-gate probe: log [0x6E3A10] (gate byte) + [0x6CE7A4] (render-id str) every 5000 post-VEH calls */
    if (g_veh_fired && (cnt % 5000 == 0)) {
        volatile BYTE *gate = (volatile BYTE *)0x6E3A10;
        void *rptr = (void *)0x6CE7A4;
        if (!IsBadReadPtr((void*)gate, 1) && !IsBadReadPtr(rptr, 8)) {
            char rbuf[9]; memcpy(rbuf, rptr, 8); rbuf[8] = '\0';
            SLOG("GATE[cnt=%ld] gate=0x%02x str='%.8s'", (long)cnt, (unsigned)*gate, rbuf);
        }
    }
    return st;
}

static void shg_install_ntwfso_inline(void) {
    HMODULE hNt = GetModuleHandleA("ntdll.dll");
    if (!hNt) { SLOG("NtWFSO-inline: ntdll.dll not loaded"); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hNt, "NtWaitForSingleObject");
    if (!fn) { SLOG("NtWFSO-inline: GPA failed"); return; }
    SLOG("NtWFSO-inline: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    if (fn[0] == 0xE9 || fn[0] == 0xEB) { SLOG("NtWFSO-inline: already patched"); return; }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("NtWFSO-inline: VirtualAlloc failed"); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p);
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_NtWFSO - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    s_real_NtWFSO_nt = (pfnNtWFSO_nt)tramp;
    SLOG("NtWFSO-inline: installed → shg_NtWFSO=%p tramp=%p", (void*)shg_NtWFSO, (void*)tramp);
}

/* ── ntdll!NtWaitForKeyedEvent inline hook ───────────────────────────────────
 * Watchdog shows 77E0393C stuck for ~900ms after VEH[256] - that address is
 * NOT NtWaitForSingleObject; it's NtWaitForKeyedEvent, used internally by
 * critical sections and DirectX DLL synchronization primitives.
 * Cap long waits post-VEH so they don't block the render loop.               */
typedef LONG (NTAPI *pfnNtWFKE)(HANDLE, PVOID, BOOLEAN, LONGLONG *);
static pfnNtWFKE s_real_NtWFKE = NULL;
static volatile LONG s_ntwfke_cnt = 0;

static LONG NTAPI shg_NtWFKE(HANDLE hkev, PVOID key, BOOLEAN alert, LONGLONG *ptimeout) {
    LONGLONG ms_val = -1;
    if (ptimeout) {
        LONGLONG t = *ptimeout;
        ms_val = (t < 0) ? ((-t) / 10000LL) : (t / 10000LL);
    }
    LONG cnt = InterlockedIncrement(&s_ntwfke_cnt);
    if (cnt <= 20 || (g_veh_fired && (cnt <= 300 || !(cnt % 50))))
        SLOG("NtWFKE[%ld]: hkev=%p key=%p alert=%d timeout=%I64dms veh=%d",
             (long)cnt, (void*)hkev, key, (int)alert, ms_val, g_veh_fired);
    LONGLONG capped;
    if (g_veh_fired && ptimeout && (ms_val < 0 || ms_val > 300)) {
        capped = -300LL * 10000LL;
        ptimeout = &capped;
    }
    LONG st = s_real_NtWFKE ? s_real_NtWFKE(hkev, key, alert, ptimeout) : 0x102L;
    if (cnt <= 20 || (g_veh_fired && (cnt <= 300 || !(cnt % 50))))
        SLOG("NtWFKE[%ld]: ret=0x%08lX", (long)cnt, (unsigned long)(DWORD)st);
    return st;
}

static void shg_install_ntwfke_inline(void) {
    HMODULE hNt = GetModuleHandleA("ntdll.dll");
    if (!hNt) { SLOG("NtWFKE-inline: ntdll.dll not loaded"); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hNt, "NtWaitForKeyedEvent");
    if (!fn) { SLOG("NtWFKE-inline: fn not found (Win10 may export it)"); return; }
    SLOG("NtWFKE-inline: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    if (fn[0] == 0xE9 || fn[0] == 0xEB) { SLOG("NtWFKE-inline: already patched"); return; }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("NtWFKE-inline: VirtualAlloc failed"); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p);
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_NtWFKE - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    s_real_NtWFKE = (pfnNtWFKE)tramp;
    SLOG("NtWFKE-inline: installed → shg_NtWFKE=%p tramp=%p", (void*)shg_NtWFKE, (void*)tramp);
}

/* ── ntdll!NtWaitForMultipleObjects inline hook ──────────────────────────────
 * Some DX/audio DLLs use NtWFMO directly.  Log and cap.                      */
typedef LONG (NTAPI *pfnNtWFMO_nt)(ULONG, HANDLE *, BOOLEAN, BOOLEAN, LONGLONG *);
static pfnNtWFMO_nt s_real_NtWFMO_nt = NULL;
static volatile LONG s_ntwfmo_cnt = 0;

static LONG NTAPI shg_NtWFMO(ULONG cnt_h, HANDLE *hs, BOOLEAN waitall, BOOLEAN alert, LONGLONG *ptimeout) {
    LONGLONG ms_val = -1;
    if (ptimeout) {
        LONGLONG t = *ptimeout;
        ms_val = (t < 0) ? ((-t) / 10000LL) : (t / 10000LL);
    }
    LONG cnt = InterlockedIncrement(&s_ntwfmo_cnt);
    if (cnt <= 20 || (g_veh_fired && (cnt <= 200 || !(cnt % 5000))))
        SLOG("NtWFMO[%ld]: n=%lu waitall=%d alert=%d timeout=%I64dms veh=%d",
             (long)cnt, (unsigned long)cnt_h, (int)waitall, (int)alert, ms_val, g_veh_fired);
    /* Log every n=1 call with handle + caller address */
    HANDLE n1_h0 = (cnt_h == 1 && hs && !IsBadReadPtr(hs, sizeof(HANDLE))) ? hs[0] : NULL;
    if (cnt_h == 1)
        SLOG("NtWFMO[%ld] N1: waitall=%d timeout=%I64dms ra=%p tid=%lu h=%p",
             (long)cnt, (int)waitall, ms_val, __builtin_return_address(0),
             (unsigned long)GetCurrentThreadId(), (void*)n1_h0);
    /* Do NOT cap finite timeouts - the game uses specific durations for synchronization.
     * A previous 300ms cap was prematurely timing out the game's 500ms waits (which
     * would have signaled at ~350ms), returning TIMEOUT instead of success and driving
     * the game into a C++ exception handling loop that blocked the render path. */
    LONG st = s_real_NtWFMO_nt ? s_real_NtWFMO_nt(cnt_h, hs, waitall, alert, ptimeout) : 0x102L;
    if (cnt <= 20 || (g_veh_fired && (cnt <= 200 || !(cnt % 5000))))
        SLOG("NtWFMO[%ld]: ret=0x%08lX", (long)cnt, (unsigned long)(DWORD)st);
    if (cnt_h == 1) {
        SLOG("NtWFMO[%ld] N1-ret: 0x%08lX", (long)cnt, (unsigned long)(DWORD)st);
        /* Board-ready auto-signal: kacbookkeeping exits early via ExitThread, so the
         * event it was supposed to signal is never set.  After 30 consecutive TIMEOUT
         * returns on the same N=1 handle, call SetEvent so the main thread's polling
         * loop unblocks and can initialize [0x6DF614] / [0x6DF610] board objects. */
        if ((DWORD)st == 0x102UL && n1_h0) {
            static volatile LONG s_n1_tmo = 0;
            static HANDLE s_n1_h = NULL;
            s_n1_h = n1_h0;
            LONG n = InterlockedIncrement(&s_n1_tmo);
            if (n == 30) {
                SLOG("NtWFMO N1: 30 timeouts on h=%p → SetEvent (board-ready stim)",
                     (void*)s_n1_h);
                SetEvent(s_n1_h);
            }
        }
    }
    return st;
}

static void shg_install_ntwfmo_inline(void) {
    HMODULE hNt = GetModuleHandleA("ntdll.dll");
    if (!hNt) return;
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hNt, "NtWaitForMultipleObjects");
    if (!fn) { SLOG("NtWFMO-inline: fn not found"); return; }
    SLOG("NtWFMO-inline: fn=%p bytes=[%02X %02X %02X %02X %02X]",
         (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
    if (fn[0] == 0xE9 || fn[0] == 0xEB) { SLOG("NtWFMO-inline: already patched"); return; }
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("NtWFMO-inline: VirtualAlloc failed"); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD *)(tramp + 6) = (DWORD)((BYTE *)fn + 5 - (tramp + 10));
    DWORD old_p = 0;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old_p);
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)shg_NtWFMO - (fn + 5));
    VirtualProtect(fn, 5, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    s_real_NtWFMO_nt = (pfnNtWFMO_nt)tramp;
    SLOG("NtWFMO-inline: installed → shg_NtWFMO=%p tramp=%p", (void*)shg_NtWFMO, (void*)tramp);
}

/* ── user32!GetMessage{A,W}/PeekMessage{A,W}/WaitMessage inline hooks ───────
 * None of NtWaitForSingleObject, NtWaitForKeyedEvent, or NtWaitForMultipleObjects
 * (all inline-hooked above, catching every caller process-wide) show ANY
 * activity after the render-gate-keeper's ~10s "state->2" log line, yet CPU
 * sampling shows the process genuinely parked at 0% CPU - a real block, not a
 * spin.  GetMessage/PeekMessage/WaitMessage's blocking wait is implemented by
 * the Win32 USER subsystem (NtUserGetMessage etc., reached through user32.dll
 * and serviced by win32k in the kernel) - a completely separate wait
 * mechanism from the generic Nt*-object-wait primitives hooked above, and
 * invisible to those hooks no matter how thoroughly they cover every DLL's
 * IAT.  If the game's main thread is parked in GetMessage waiting for a
 * message that a helper thread was supposed to Post but never did (e.g.
 * because that thread was ExitThread(0)'d by the VEH before it could post),
 * this hook will show the call going in with no matching "RET=" line ever
 * following it.
 *
 * Inline (function-byte) hooks, not IAT - catch every caller in the process
 * regardless of which DLL's import table it goes through.  Pure diagnostic
 * passthrough: does not alter behavior. */
typedef BOOL (WINAPI *pfnGetMessageA)(LPMSG, HWND, UINT, UINT);
typedef BOOL (WINAPI *pfnGetMessageW)(LPMSG, HWND, UINT, UINT);
typedef BOOL (WINAPI *pfnPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *pfnPeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *pfnWaitMessage)(void);

static pfnGetMessageA  s_real_GMA = NULL;
static pfnGetMessageW  s_real_GMW = NULL;
static pfnPeekMessageA s_real_PMA = NULL;
static pfnPeekMessageW s_real_PMW = NULL;
static pfnWaitMessage  s_real_WM  = NULL;
static volatile LONG s_gma_cnt = 0, s_gmw_cnt = 0, s_pma_cnt = 0, s_pmw_cnt = 0, s_wm_cnt = 0;

static BOOL WINAPI shg_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMin, UINT wMax) {
    LONG cnt = InterlockedIncrement(&s_gma_cnt);
    DWORD tid = GetCurrentThreadId();
    int verbose = (cnt <= 20) || !(cnt % 200);
    if (verbose)
        SLOG("GetMessageA[%ld]: ENTER hwnd=%p min=%u max=%u tid=%lu (blocks until a message arrives)",
             (long)cnt, (void*)hWnd, wMin, wMax, (unsigned long)tid);
    BOOL r = s_real_GMA ? s_real_GMA(lpMsg, hWnd, wMin, wMax) : -1;
    if (verbose)
        SLOG("GetMessageA[%ld]: RET=%d msg=0x%04X hwnd=%p tid=%lu",
             (long)cnt, (int)r, (unsigned)(lpMsg ? lpMsg->message : 0),
             (void*)(lpMsg ? lpMsg->hwnd : NULL), (unsigned long)tid);
    return r;
}
static BOOL WINAPI shg_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMin, UINT wMax) {
    LONG cnt = InterlockedIncrement(&s_gmw_cnt);
    DWORD tid = GetCurrentThreadId();
    int verbose = (cnt <= 20) || !(cnt % 200);
    if (verbose)
        SLOG("GetMessageW[%ld]: ENTER hwnd=%p min=%u max=%u tid=%lu (blocks until a message arrives)",
             (long)cnt, (void*)hWnd, wMin, wMax, (unsigned long)tid);
    BOOL r = s_real_GMW ? s_real_GMW(lpMsg, hWnd, wMin, wMax) : -1;
    if (verbose)
        SLOG("GetMessageW[%ld]: RET=%d msg=0x%04X hwnd=%p tid=%lu",
             (long)cnt, (int)r, (unsigned)(lpMsg ? lpMsg->message : 0),
             (void*)(lpMsg ? lpMsg->hwnd : NULL), (unsigned long)tid);
    return r;
}
static BOOL WINAPI shg_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMin, UINT wMax, UINT wRemove) {
    LONG cnt = InterlockedIncrement(&s_pma_cnt);
    DWORD tid = GetCurrentThreadId();
    int verbose = (cnt <= 20) || !(cnt % 3000);
    if (verbose)
        SLOG("PeekMessageA[%ld]: ENTER hwnd=%p min=%u max=%u remove=%u tid=%lu",
             (long)cnt, (void*)hWnd, wMin, wMax, wRemove, (unsigned long)tid);
    BOOL r = s_real_PMA ? s_real_PMA(lpMsg, hWnd, wMin, wMax, wRemove) : 0;
    if (verbose || r)
        SLOG("PeekMessageA[%ld]: RET=%d msg=0x%04X hwnd=%p tid=%lu",
             (long)cnt, (int)r, (unsigned)((r && lpMsg) ? lpMsg->message : 0),
             (void*)((r && lpMsg) ? lpMsg->hwnd : NULL), (unsigned long)tid);
    return r;
}
static BOOL WINAPI shg_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMin, UINT wMax, UINT wRemove) {
    LONG cnt = InterlockedIncrement(&s_pmw_cnt);
    DWORD tid = GetCurrentThreadId();
    int verbose = (cnt <= 20) || !(cnt % 3000);
    if (verbose)
        SLOG("PeekMessageW[%ld]: ENTER hwnd=%p min=%u max=%u remove=%u tid=%lu",
             (long)cnt, (void*)hWnd, wMin, wMax, wRemove, (unsigned long)tid);
    BOOL r = s_real_PMW ? s_real_PMW(lpMsg, hWnd, wMin, wMax, wRemove) : 0;
    if (verbose || r)
        SLOG("PeekMessageW[%ld]: RET=%d msg=0x%04X hwnd=%p tid=%lu",
             (long)cnt, (int)r, (unsigned)((r && lpMsg) ? lpMsg->message : 0),
             (void*)((r && lpMsg) ? lpMsg->hwnd : NULL), (unsigned long)tid);
    return r;
}
static BOOL WINAPI shg_WaitMessage(void) {
    LONG cnt = InterlockedIncrement(&s_wm_cnt);
    DWORD tid = GetCurrentThreadId();
    SLOG("WaitMessage[%ld]: ENTER tid=%lu (blocks until any message is queued)",
         (long)cnt, (unsigned long)tid);
    BOOL r = s_real_WM ? s_real_WM() : 0;
    SLOG("WaitMessage[%ld]: RET=%d tid=%lu", (long)cnt, (int)r, (unsigned long)tid);
    return r;
}

/* Shared trampoline installer - same relative-JMP technique as
 * shg_install_ntwfso_inline et al., factored once since we now need it for
 * several DLLs in a row.  Handles two prologue shapes:
 *   - the standard >=5-byte MS hotpatch prologue (8B FF 55 8B EC = mov
 *     edi,edi / push ebp / mov ebp,esp), which is what NtWaitForSingleObject
 *     and friends, plus GetMessage{A,W}/PeekMessage{A,W}, all turned out to
 *     have (confirmed via the bytes= dump in the log) - 5 bytes stolen; and
 *   - a bare 6-byte "FF 25 xx xx xx xx" (JMP DWORD PTR [addr]) forwarding
 *     thunk, which is what WaitMessage turned out to be on this Windows
 *     build - stealing only 5 bytes here would truncate the instruction's
 *     4-byte address operand and corrupt it, so this shape steals 6 and pads
 *     the dangling 6th original byte with a NOP. */
static void shg_install_dll_inline(const char *dllname, const char *fnname, void *hook_fn, void **out_real) {
    HMODULE hD = GetModuleHandleA(dllname);
    if (!hD) { SLOG("%s!%s-inline: %s not loaded", dllname, fnname, dllname); return; }
    BYTE *fn = (BYTE *)(ULONG_PTR)GetProcAddress(hD, fnname);
    if (!fn) { SLOG("%s!%s-inline: GPA failed", dllname, fnname); return; }
    SLOG("%s!%s-inline: fn=%p bytes=[%02X %02X %02X %02X %02X %02X]",
         dllname, fnname, (void*)fn, fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);
    if (fn[0] == 0xE9 || fn[0] == 0xEB) { SLOG("%s!%s-inline: already patched", dllname, fnname); return; }
    int steal = (fn[0] == 0xFF && fn[1] == 0x25) ? 6 : 5;
    BYTE *tramp = (BYTE *)VirtualAlloc(NULL, 16, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) { SLOG("%s!%s-inline: VirtualAlloc failed", dllname, fnname); return; }
    memcpy(tramp, fn, steal);
    tramp[steal] = 0xE9;
    *(DWORD *)(tramp + steal + 1) = (DWORD)((BYTE *)fn + steal - (tramp + steal + 5));
    DWORD old_p = 0;
    VirtualProtect(fn, steal, PAGE_EXECUTE_READWRITE, &old_p);
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)((BYTE *)hook_fn - (fn + 5));
    if (steal > 5) memset(fn + 5, 0x90, (size_t)(steal - 5));  /* NOP-pad dangling byte(s) */
    VirtualProtect(fn, steal, old_p, &old_p);
    FlushInstructionCache(GetCurrentProcess(), fn, steal);
    *out_real = tramp;
    SLOG("%s!%s-inline: installed → hook=%p tramp=%p steal=%d", dllname, fnname, hook_fn, (void*)tramp, steal);
}

static void shg_install_msgloop_inline(void) {
    shg_install_dll_inline("user32.dll", "GetMessageA",  (void*)shg_GetMessageA,  (void**)&s_real_GMA);
    shg_install_dll_inline("user32.dll", "GetMessageW",  (void*)shg_GetMessageW,  (void**)&s_real_GMW);
    shg_install_dll_inline("user32.dll", "PeekMessageA", (void*)shg_PeekMessageA, (void**)&s_real_PMA);
    shg_install_dll_inline("user32.dll", "PeekMessageW", (void*)shg_PeekMessageW, (void**)&s_real_PMW);
    shg_install_dll_inline("user32.dll", "WaitMessage",  (void*)shg_WaitMessage,  (void**)&s_real_WM);
}

/* ── ntdll!NtSignalAndWaitForSingleObject + NtWaitForAlertByThreadId ────────
 * Two more wait mechanisms invisible to NtWaitForSingleObject/KeyedEvent/
 * MultipleObjects and to GetMessage/PeekMessage/WaitMessage.
 * NtSignalAndWaitForSingleObject backs kernel32!SignalObjectAndWait, used
 * internally by some CRT/COM/thread-pool handoff patterns.
 * NtWaitForAlertByThreadId is the Windows 8+ primitive behind contended
 * slim-reader/writer locks and condition variables (ucrtbase.dll and
 * combase.dll - both loaded in this process per the module list - use these
 * internally).  Both are pure diagnostic passthroughs. */
typedef LONG (NTAPI *pfnNtSAWFSO)(HANDLE, HANDLE, BOOLEAN, LONGLONG *);
static pfnNtSAWFSO s_real_NtSAWFSO = NULL;
static volatile LONG s_ntsawfso_cnt = 0;

static LONG NTAPI shg_NtSAWFSO(HANDLE hSignal, HANDLE hWait, BOOLEAN alert, LONGLONG *ptimeout) {
    LONGLONG ms_val = -1;
    if (ptimeout) { LONGLONG t = *ptimeout; ms_val = (t < 0) ? ((-t) / 10000LL) : (t / 10000LL); }
    LONG cnt = InterlockedIncrement(&s_ntsawfso_cnt);
    DWORD tid = GetCurrentThreadId();
    if (cnt <= 40 || (g_veh_fired && (cnt <= 300 || !(cnt % 5000))))
        SLOG("NtSAWFSO[%ld]: signal=%p wait=%p alert=%d timeout=%I64dms tid=%lu veh=%d",
             (long)cnt, (void*)hSignal, (void*)hWait, (int)alert, ms_val, (unsigned long)tid, g_veh_fired);
    LONG st = s_real_NtSAWFSO ? s_real_NtSAWFSO(hSignal, hWait, alert, ptimeout) : 0x102L;
    if (cnt <= 40 || (g_veh_fired && (cnt <= 300 || !(cnt % 5000))))
        SLOG("NtSAWFSO[%ld]: ret=0x%08lX tid=%lu", (long)cnt, (unsigned long)(DWORD)st, (unsigned long)tid);
    return st;
}

typedef LONG (NTAPI *pfnNtWFABTI)(PVOID, LONGLONG *);
static pfnNtWFABTI s_real_NtWFABTI = NULL;
static volatile LONG s_ntwfabti_cnt = 0;

/* 2026-07-23: known-dead-address short-circuit. Repeatedly re-registering a
 * wait on the SAME address with the kernel (even capped to 300ms and even
 * though each individual call returns cleanly) turned out NOT to be free:
 * live testing found a genuine ntdll-internal crash (AV inside/near
 * RtlWakeAddressAll's wait-block hash-bucket unlink, confirmed via disasm --
 * a WRITE through a corrupt "prev" pointer) that reproduces under exactly
 * this repeated-contended-wait pattern with multiple threads hammering the
 * same address. Once that happens the VEH either sacrifices the faulting
 * thread (safe now that resume-in-place is tried first, see below) or --
 * when it lands on the MAIN thread with no nearby KSHG return address to
 * resume to -- kills the whole process outright (unhandled AV). Cutting the
 * repeated kernel-side registration/deregistration traffic after an address
 * has ALREADY proven itself dead (timed out twice for real) should reduce
 * how often the corruption is even reached, without changing behavior for
 * any address that might still resolve normally. */
#define SHG_DEAD_ADDR_MAX 8
/* 2026-07-31: reduced from 2 to 1.  The corruption in RtlWakeAddressAll
 * (doubly-linked wait-block-list unlink AV) fires during the FIRST TWO real
 * kernel calls when multiple threads race on the same dead address.  With
 * threshold=2 we still make 2 real kernel-side NtWFABTI registrations per
 * address per thread before short-circuiting - enough to race with each
 * other on the first round.  With threshold=1 we make exactly ONE real call
 * per address per thread (the first, to confirm it is dead), then every
 * subsequent call skips the kernel entirely, eliminating the repeated
 * contention that triggers the hash-bucket corruption.  Safe because the
 * existing comment already established: "SRW-locks/condvars are required by
 * contract to tolerate spurious wakeups" - a fast fake STATUS_TIMEOUT is
 * semantically identical to a real 300ms-cap return on a dead address. */
#define SHG_DEAD_ADDR_THRESHOLD 1
static PVOID       s_dead_addr[SHG_DEAD_ADDR_MAX];
static volatile LONG s_dead_hits[SHG_DEAD_ADDR_MAX];
static volatile LONG s_dead_n = 0;

static LONG shg_dead_addr_lookup(PVOID address) {
    LONG n = s_dead_n;
    for (LONG i = 0; i < n && i < SHG_DEAD_ADDR_MAX; i++)
        if (s_dead_addr[i] == address) return s_dead_hits[i];
    return -1;
}
static void shg_dead_addr_note_timeout(PVOID address) {
    LONG n = s_dead_n;
    for (LONG i = 0; i < n && i < SHG_DEAD_ADDR_MAX; i++) {
        if (s_dead_addr[i] == address) { InterlockedIncrement(&s_dead_hits[i]); return; }
    }
    if (n < SHG_DEAD_ADDR_MAX) {
        LONG idx = InterlockedIncrement(&s_dead_n) - 1;
        if (idx < SHG_DEAD_ADDR_MAX) { s_dead_addr[idx] = address; s_dead_hits[idx] = 1; }
    }
}

/* NEVER HAND STATUS_TIMEOUT TO AN *INFINITE* WAIT.
 *
 * This is the bug that crashed Silent Hill: The Arcade at ntdll+0xA0311.
 *
 * The reasoning above - "SRW-locks/condvars are required by contract to
 * tolerate spurious wakeups, so a timeout is always safe to hand back" - holds
 * for SRW locks and condition variables. It does NOT hold for a CRITICAL
 * SECTION. ntdll!RtlpWaitOnCriticalSection reacts to STATUS_TIMEOUT by taking
 * its deadlock-diagnostic branch, which prints the timeout value:
 *
 *     000A02FD  mov  eax,[esp+1Ch]     ; the PLARGE_INTEGER Timeout the caller passed
 *     000A0311  push dword ptr [eax+4] ; <-- ACCESS VIOLATION when Timeout was NULL
 *
 * and on an infinite wait that pointer is legitimately NULL. Stock Windows can
 * never reach that instruction, because a real infinite wait never returns
 * STATUS_TIMEOUT. We made it reachable, so every thread contending on that
 * critical section died there - which is exactly what the crash log showed:
 * three threads, one EIP, EDI pointing at the same critical section.
 *
 * STATUS_ALERTED is the correct answer for breaking an infinite wait. It means
 * "you were woken, re-check your condition", which every caller of this
 * primitive must already handle, and it is what the kernel itself returned on
 * the real calls in the same log (ret=0x00000101). */
#define SHG_STATUS_ALERTED  0x101L
#define SHG_STATUS_TIMEOUT  0x102L
/* WHAT TO RETURN WHEN WE BREAK A WAIT -- AND WHY THIS IS A SWITCH.
 *
 * There is no answer here that is right in every case, which is the whole
 * problem. Measured, on this game:
 *
 *   TIMEOUT (0x102) -- what the code did originally. Callers give up, the boot
 *       progresses furthest (window created, upscale armed), and then any
 *       thread contending on a CRITICAL SECTION dies in ntdll's deadlock
 *       diagnostic, which dereferences the NULL Timeout of an infinite wait.
 *
 *   ALERTED (0x101) -- correct by the letter of the API: "spurious wake,
 *       re-check". It cannot crash ntdll. But for a wait that will NEVER be
 *       satisfied the caller simply waits again, so the game livelocked EARLIER
 *       than it used to crash - no window at all. Tried 2026-09-16, worse.
 *
 *   SUCCESS (0x00) -- tells the caller it was signaled. Riskier in principle,
 *       but it is the only one of the three that both avoids the ntdll
 *       diagnostic AND lets the caller move on. Untested here.
 *
 * Default is TIMEOUT, i.e. the behavior that gets furthest today, so this file
 * is never worse than it was. ULTRA_SHG_WFABTI=alerted|success|timeout picks
 * another without a rebuild, which is what makes this settleable in one run
 * each instead of three build cycles. */
static LONG shg_wfabti_break(LONGLONG *ptimeout) {
    const char *m = getenv("ULTRA_SHG_WFABTI");
    (void)ptimeout;
    if (m && *m) {
        if (m[0] == 'a' || m[0] == 'A') return SHG_STATUS_ALERTED;
        if (m[0] == 's' || m[0] == 'S') return 0L;
    }
    return SHG_STATUS_TIMEOUT;
}

static LONG NTAPI shg_NtWFABTI(PVOID address, LONGLONG *ptimeout) {
    LONGLONG ms_val = -1;
    if (ptimeout) { LONGLONG t = *ptimeout; ms_val = (t < 0) ? ((-t) / 10000LL) : (t / 10000LL); }
    LONG cnt = InterlockedIncrement(&s_ntwfabti_cnt);
    DWORD tid = GetCurrentThreadId();
    if (cnt <= 60 || (g_veh_fired && (cnt <= 300 || !(cnt % 5000))))
        SLOG("NtWFABTI[%ld]: addr=%p timeout=%I64dms tid=%lu veh=%d io=%d",
             (long)cnt, address, ms_val, (unsigned long)tid, g_veh_fired, g_io_enabled);

    /* 2026-07-31: render-thread fast-path. Declared here before use. */
    int shg_is_main_thread = (g_main_thread_id != 0 && tid == g_main_thread_id);

    /* 2026-07-31: render-thread fast-path.  The render thread (tid=s_render_tid)
     * has no legitimate waits on any SRW/condvar object this game uses - it only
     * calls NtWFABTI because it runs inside the game's render dispatch chain,
     * which was never designed to run outside the GHL hardware callback context.
     * Every NtWFABTI from the render thread is therefore on a dead address.
     * Return STATUS_TIMEOUT immediately without ANY real kernel call, eliminating
     * the render thread's contribution to the RtlWakeAddressAll contention that
     * corrupts the wait-block hash-bucket list and crashes at 0x7732FAED. */
    if (s_render_tid && tid == s_render_tid) {
        if (cnt <= 60 || !(cnt % 5000))
            SLOG("NtWFABTI[%ld]: render-thread fast-path addr=%p -> STATUS_TIMEOUT (no kernel call)",
                 (long)cnt, address);
        shg_dead_addr_note_timeout(address);
        return shg_wfabti_break(ptimeout);   /* NEVER 0x102 on an infinite wait */
    }
    /* CONFIRMED via shg_watchdog EIP capture (2026-07-21 investigation): the
     * main thread parks FOREVER in exactly this call - EIP froze at
     * ntdll!NtWaitForAlertByThreadId+0xC (the post-patch original bytes,
     * i.e. genuinely inside the kernel wait, not one of our trampolines) for
     * 30+ consecutive 300ms watchdog samples with zero movement.  The specific
     * blocking call was on an INFINITE wait, address ending in "...589C" -
     * the same low-16-bit suffix recurs across independent runs (different
     * ASLR base each time), meaning it's a fixed offset into some heap
     * object, almost certainly an SRW-lock or C++11 condition-variable wait
     * that nothing will ever alert without real GHL hardware on the other
     * side.  NtWaitForAlertByThreadId backs general-purpose locks all over
     * the process (ucrtbase.dll/combase.dll internals), not one specific
     * known-dead wait like the render thread's NtWFSO(INFINITE) case - so
     * cap rather than fake success outright.  SRW-locks/condvars are
     * required by contract to tolerate spurious wakeups by re-checking their
     * condition, so a timeout here is always safe to hand back to the
     * caller's own retry loop; asserting "you were alerted" when we don't
     * know that's true would not be. */
    /* 2026-07-23: the three checks below originally gated only on
     * g_veh_fired||g_io_enabled -- both are set no earlier than
     * shaiolib_initialize(), which the MAIN thread itself must reach by
     * continuing past whatever it is currently doing.  Live-reproduced
     * (kshg_wdbg.exe, a passive kernel-level Win32 debugger -- DebugActiveProcess
     * + WaitForDebugEvent, always continued with DBG_EXCEPTION_NOT_HANDLED so
     * it changes nothing about game/VEH behavior, purely observational) a
     * genuine indefinite hang: the confirmed main thread (tid matched
     * g_main_thread_id, captured at the top of DllMain) parks in exactly this
     * call with an INFINITE timeout at ~32ms post-attach -- BEFORE either flag
     * is ever set -- so the existing cap never engaged and the whole process
     * sat frozen (0 exceptions, 0 further SLOG output) for 60+ real seconds of
     * live kernel-level observation with no sign of ever resolving on its own.
     * Chicken-and-egg: g_io_enabled can only become true once initialize()
     * runs, and initialize() can only run once the main thread gets past THIS
     * wait. Extend the same already-established reasoning this file already
     * relies on for this exact primitive ("a timeout here is always safe to
     * hand back to the caller's own retry loop" -- SRW-locks/condvars are
     * required BY CONTRACT to tolerate spurious wakeups) to the confirmed main
     * thread specifically, even pre-boot. Deliberately narrower than capping
     * every thread from t=0: leaves every OTHER, never-observed-to-hang early
     * caller completely untouched, while guaranteeing the ONE thread the whole
     * game depends on can never be stuck here permanently. */
    /* shg_is_main_thread already declared above (2026-07-31 fix) */
    if ((g_veh_fired || g_io_enabled || shg_is_main_thread) && shg_dead_addr_lookup(address) >= SHG_DEAD_ADDR_THRESHOLD) {
        if (cnt <= 60 || (g_veh_fired && (cnt <= 300 || !(cnt % 5000))))
            SLOG("NtWFABTI[%ld]: addr=%p already proven dead (>=%d timeouts) -- skipping real syscall",
                 (long)cnt, address, SHG_DEAD_ADDR_THRESHOLD);
        return shg_wfabti_break(ptimeout);  /* no kernel-side wait-block registration at all */
    }
    LONGLONG capped;
    if ((g_veh_fired || g_io_enabled || shg_is_main_thread) && (!ptimeout || ms_val < 0 || ms_val > 300)) {
        capped = -300LL * 10000LL;
        ptimeout = &capped;
        if (shg_is_main_thread && !g_veh_fired && !g_io_enabled)
            SLOG("NtWFABTI[%ld]: pre-boot main-thread INFINITE wait -> capped 300ms (addr=%p)", (long)cnt, address);
    }
    LONG st = s_real_NtWFABTI ? s_real_NtWFABTI(address, ptimeout)
                              : shg_wfabti_break(ptimeout);
    if ((g_veh_fired || g_io_enabled || shg_is_main_thread) && st == 0x102L)
        shg_dead_addr_note_timeout(address);
    if (cnt <= 60 || (g_veh_fired && (cnt <= 300 || !(cnt % 5000))))
        SLOG("NtWFABTI[%ld]: ret=0x%08lX tid=%lu", (long)cnt, (unsigned long)(DWORD)st, (unsigned long)tid);
    return st;
}

static void shg_install_hidden_wait_inline(void) {
    shg_install_dll_inline("ntdll.dll", "NtSignalAndWaitForSingleObject",
                            (void*)shg_NtSAWFSO,  (void**)&s_real_NtSAWFSO);
    shg_install_dll_inline("ntdll.dll", "NtWaitForAlertByThreadId",
                            (void*)shg_NtWFABTI,  (void**)&s_real_NtWFABTI);
}

/* ── Vectored Exception Handler: null/near-null function-pointer guard ─────
 * After Sleep(1984204624) is forced to return (50ms cap) the game's post-
 * calibration code attempts to call a function pointer that was never populated
 * by the GHL hardware interrupt handler - the pointer holds a small integer
 * (0x0F, 0x13, etc.) instead of a valid code address.
 *
 * The VEH catches ACCESS_VIOLATION at address < 0x10000 while g_io_enabled is
 * set, simulates the bad vtable call returning 0 by popping the return address
 * from the stack (Method 0), or walks EBP/stack to find a KSHG resume point.
 * Capped at VEH_MAX_CATCHES fires to prevent infinite loops. */

/* ── KSHG.exe binary patches (applied from DllMain) ──────────────────────
 * Patches three locations in 0x43F2D0 (board vtable dispatch) to always-skip
 * the vtable calls when board objects are absent/garbage, and two locations in
 * 0x4D0030 (calibration wait loop) to force a single-pass exit so we don't
 * spin forever waiting for hardware that isn't there. */
static BOOL shg_patch_byte(DWORD va, BYTE expected, BYTE newval) {
    BYTE *p = (BYTE*)(ULONG_PTR)va;
    DWORD op;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &op)) {
        SLOG("patch_byte 0x%08X: VirtualProtect FAIL %lu", va, (unsigned long)GetLastError());
        return FALSE;
    }
    if (*p != expected) {
        VirtualProtect(p, 1, op, &op);
        SLOG("patch_byte 0x%08X: expected 0x%02X got 0x%02X, SKIP", va, expected, *p);
        return FALSE;
    }
    *p = newval;
    VirtualProtect(p, 1, op, &op);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    SLOG("patch_byte 0x%08X: 0x%02X→0x%02X OK", va, expected, newval);
    return TRUE;
}

static void shg_apply_game_patches(void) {
    /* 0x43F2D0: three board vtable-call sequences.
     * Each has: TEST ECX,ECX; JE +5; MOV EAX,[ECX]; CALL [EAX+offset]
     * Change JE (0x74 disp=0x05) → JMP (0xEB disp=0x05) so the vtable call
     * is ALWAYS skipped, regardless of whether the pointer is NULL or garbage.
     * This prevents the ACCESS_VIOLATION crashes without disturbing the
     * calibration loop's own logic or the real [0x6802FC] poll function. */
    shg_patch_byte(0x43F2D8, 0x74, 0xEB);   /* skip CALL *0x40(%eax) */
    shg_patch_byte(0x43F2E7, 0x74, 0xEB);   /* skip CALL *0x18(%edx) */
    shg_patch_byte(0x43F2F6, 0x74, 0xEB);   /* skip CALL *0x14(%eax) */

    /* 0x411120: NOP whole function → XOR EAX,EAX (33 C0) + RET 4 (C2 04 00).
     * vtable[1] of the game sub-object = 0x3D800000 (IEEE float, not fn ptr).
     * Returning 0 lets the render chain skip this call cleanly. */
    shg_patch_byte(0x411120, 0x56, 0x33);
    shg_patch_byte(0x411121, 0x57, 0xC0);
    shg_patch_byte(0x411122, 0x8B, 0xC2);
    shg_patch_byte(0x411123, 0x7C, 0x04);
    shg_patch_byte(0x411124, 0x24, 0x00);

    /* 0x415CF9: JNZ → JG - render-list sentinel fix.
     * The linked-list traversal in 0x425856 has TEST EAX,EAX; JNZ +0xF which
     * continues for any non-zero pointer.  When the list ends with the sentinel
     * value 0xFF560774 (no real HW to update it), JNZ is taken and crashes.
     * JG (jump if greater, signed: ZF=0 AND SF=OF) skips both null (ZF=1)
     * and high-address sentinels (SF=1, OF=0 after TEST) but continues for
     * valid user-mode pointers 0x00000001-0x7FFFFFFF. */
    shg_patch_byte(0x415CF9, 0x75, 0x7F);   /* JNZ → JG */

    SLOG("shg_apply_game_patches: done (9 patches applied)");
}

/* ── Watchdog: samples main-thread EIP after VEH[256] via GetThreadContext ──
 * Uses WriteFile to a dedicated log (shg_watchdog.log) - never SLOG, never
 * acquires any user-mode lock, so suspending the main thread is safe. */
static HANDLE g_wdog_log_h = INVALID_HANDLE_VALUE;

static DWORD WINAPI shg_watchdog_proc(LPVOID arg) {
    (void)arg;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    strncat(tmp, "shg_watchdog.log", MAX_PATH - strlen(tmp) - 1);
    g_wdog_log_h = CreateFileA(tmp, GENERIC_WRITE, FILE_SHARE_READ,
                                NULL, CREATE_ALWAYS, 0, NULL);
    {
        DWORD wr;
        WriteFile(g_wdog_log_h, "=== shg_watchdog started ===\r\n", 30, &wr, NULL);
    }
    /* Let the post-VEH sleeps finish (Sleep(0) + Sleep(50ms cap)) */
    Sleep(2500);

    int same_count = 0;
    DWORD last_eip = 0;
    int i;
    for (i = 0; i < 200; i++) {  /* up to 60 s of sampling */
        Sleep(300);
        HANDLE h = g_main_thread_h;
        if (!h) continue;

        DWORD tid = GetThreadId(h);
        DWORD susp = SuspendThread(h);
        if (susp == (DWORD)-1) {
            DWORD wr;
            WriteFile(g_wdog_log_h, "SuspendThread failed\r\n", 22, &wr, NULL);
            continue;
        }

        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &ctx)) {
            if (ctx.Eip == last_eip) same_count++;
            else { same_count = 0; last_eip = ctx.Eip; }

            char line[300];
            int len = _snprintf(line, sizeof(line)-1,
                "[%5lu] WDOG tid=%lu EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX ECX=%08lX%s\r\n",
                (unsigned long)(GetTickCount() - s_log_t0),
                (unsigned long)tid,
                (unsigned long)ctx.Eip, (unsigned long)ctx.Esp,
                (unsigned long)ctx.Ebp, (unsigned long)ctx.Eax, (unsigned long)ctx.Ecx,
                same_count >= 3 ? " [STUCK]" : "");
            DWORD wr;
            WriteFile(g_wdog_log_h, line, (DWORD)len, &wr, NULL);
        }
        ResumeThread(h);
    }
    {
        DWORD wr;
        WriteFile(g_wdog_log_h, "=== watchdog done ===\r\n", 23, &wr, NULL);
    }
    CloseHandle(g_wdog_log_h);
    g_wdog_log_h = INVALID_HANDLE_VALUE;
    return 0;
}

static void shg_start_watchdog(void) {
    if (g_main_thread_h) return; /* idempotent */
    g_main_thread_h = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, GetCurrentThreadId());
    CreateThread(NULL, 0, shg_watchdog_proc, NULL, 0, NULL);
    SLOG("watchdog: started for main tid=%lu", (unsigned long)GetCurrentThreadId());
}

#define VEH_MAX_CATCHES 0x7FFFFFFF  /* effectively unlimited - detection loop is infinite on PC without hardware */
static volatile LONG s_veh_count = 0;
static PVOID         s_veh_handle = NULL;

/* KSHG.exe code range - PE loads at default base 0x00400000, code to ~0x710000. */
#define KSHG_CODE_LO 0x00401000u
#define KSHG_CODE_HI 0x00710000u

/* Our DLL's code range - used by VEH to locate render-thread recovery points */
static DWORD s_dll_code_lo = 0;
static DWORD s_dll_code_hi = 0;
static void shg_cache_dll_range(HINSTANCE h) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)(void*)h;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS*)((BYTE*)h + dos->e_lfanew);
    s_dll_code_lo = (DWORD)(ULONG_PTR)(void*)h;
    s_dll_code_hi = s_dll_code_lo + nt->OptionalHeader.SizeOfImage;
}

static LONG WINAPI shg_veh(EXCEPTION_POINTERS *ep) {
    DWORD excode = ep->ExceptionRecord->ExceptionCode;

    /* Diagnostic: log EVERY exception that reaches us, of ANY code, before
     * the access-violation-only filter below.  We're registered FIRST in the
     * VEH chain (AddVectoredExceptionHandler(1, ...)) so we see everything.
     * The main thread goes silent very early (~400ms) with no further
     * activity from ANY hooked API; if that's because it's hitting an
     * exception type we don't otherwise handle here - illegal instruction,
     * stack overflow, a C++/SEH exception (0xE06D7363), a guard-page probe,
     * etc. - this is the only place that can see it.  Purely additive: does
     * NOT change behavior for non-AV codes, still CONTINUE_SEARCH below. */
    {
        static volatile LONG s_any_exc_cnt = 0;
        LONG an = InterlockedIncrement(&s_any_exc_cnt);
        SLOG("VEH-ANY[%ld]: code=0x%08lX addr=0x%08X tid=%lu firstchance av=%d",
             (long)an, (unsigned long)excode,
             (unsigned)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress,
             (unsigned long)GetCurrentThreadId(),
             (int)(excode == EXCEPTION_ACCESS_VIOLATION));

        /* READ WHAT THE GAME IS ACTUALLY SAYING.
         *
         * 0x40010006 is DBG_PRINTEXCEPTION_C -- an OutputDebugStringA. We have
         * been counting these for weeks and never reading one, which is why a
         * burst of about twenty of them immediately before the game called
         * shaiolib_terminate() told us nothing at all. The string is right
         * there in the record: ExceptionInformation[0] is its length including
         * the NUL, [1] is the pointer.
         *
         * This is the game's own error text at the exact moment it decides to
         * quit. There is no cheaper diagnostic available anywhere in this
         * project, and it costs one guarded read. */
        if ((excode == 0x40010006u || excode == 0x4001000Au) &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR len = ep->ExceptionRecord->ExceptionInformation[0];
            const void *p = (const void *)ep->ExceptionRecord->ExceptionInformation[1];
            if (p && len && len < 1024 && !IsBadReadPtr(p, len)) {
                char buf[512];
                size_t n = (size_t)(len < sizeof buf ? len : sizeof buf - 1);
                size_t i;
                if (excode == 0x4001000Au) {          /* wide variant */
                    const wchar_t *w = (const wchar_t *)p;
                    n = 0;
                    while (n < sizeof buf - 1 && w[n]) {
                        buf[n] = (w[n] >= 0x20 && w[n] < 0x7F) ? (char)w[n] : '.';
                        ++n;
                    }
                } else {
                    memcpy(buf, p, n);
                    for (i = 0; i < n; ++i)
                        if (buf[i] && (buf[i] < 0x20 || (unsigned char)buf[i] >= 0x7F))
                            buf[i] = '.';
                }
                buf[n] = 0;
                while (n && (buf[n-1] == '.' || buf[n-1] == ' ')) buf[--n] = 0;
                if (buf[0]) SLOG("DBGSTR: %s", buf);
            }
        }
    }

    /* ── SxS file-hash-missing: suppress and return to caller ─────────────
     * STATUS_SXS_FILE_HASH_MISSING (0xC0150011) is raised by ntdll's SxS
     * activation-context code when a replaced DLL fails its manifest hash
     * check.  The function that raises it is a 1-arg NTAPI (STDCALL) so the
     * callee must clean 4+4 = 8 bytes off the stack. */
    /* ── Near-null function-pointer call: walk stack to KSHG.exe ──────────
     * After Sleep(1984204624) is capped, the GHL callback pointer (never
     * populated by absent hardware) holds a small integer.  Resuming at the
     * WOW64 thunk (ret_addr = 0x77895B4B) causes a secondary SxS crash.
     * Instead, walk the stack looking for a KSHG.exe return address and
     * unwind there, skipping the entire winmm/WOW64 chain. */
    /* 2026-07-31: STATUS_STACK_OVERFLOW (0xC00000FD) used to fall straight
     * through to CONTINUE_SEARCH here, same as any other non-AV code. That is
     * the "caught once, dies shortly after" signature: the render call chain
     * (0x425856 walks game linked-lists/vtables several levels deep) blows the
     * stack, the thread's guard page is consumed by that FIRST overflow, and
     * -- because this handler never restores it -- the very next stack growth
     * past the same limit has no guard page left to trip. That usually can't
     * even be delivered as a clean second exception (the dispatcher itself
     * needs a little stack headroom), so the process just dies with no
     * further VEH log line. Fix: accept STACK_OVERFLOW into the same
     * recovery path AND call _resetstkoflw() to re-arm the guard page before
     * anything else touches the stack, so recovery is durable instead of
     * one-shot. (EXCEPTION_STACK_OVERFLOW is already a recognized fault class
     * elsewhere in this codebase -- core/native/lindbergh/src/signals.c. */
    if (excode != EXCEPTION_ACCESS_VIOLATION && excode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (excode == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
        SLOG("VEH: STACK_OVERFLOW tid=%lu -- guard page reset",
             (unsigned long)GetCurrentThreadId());
    }
    ULONG_PTR fault = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;

    /* ── Render thread: longjmp back to render loop checkpoint ─────────────
     * The old VEH stack-walk recovery was broken: VEH resumed inside
     * shg_render_call at `popl %%esi`, then the subsequent `ret` used an ntdll
     * address from crash_ESP+48 as the return target.  We now use setjmp in the
     * render loop; VEH just saves fault info and calls longjmp to return there. */
    if (s_render_tid && GetCurrentThreadId() == s_render_tid) {
        CONTEXT *rctx = ep->ContextRecord;

        /* 2026-07-31: if the fault is INSIDE ntdll (RtlWakeAddressAll wait-block
         * corruption, EIP=0x7732FAED class), do NOT longjmp - ntdll's internal
         * wait-block list is in a corrupt, partially-unlinked state, and resuming
         * in the render loop while ntdll holds corrupt internal structures causes
         * cascading secondary crashes.  Instead, ExitThread(0) the render thread
         * cleanly so only the render function is lost (the loop respawns on the
         * next shg_direct_render_thread call from rgate_keeper's restart logic).
         * The render-thread fast-path in shg_NtWFABTI above (added 2026-07-31)
         * prevents this from ever firing in practice, but keep this guard as a
         * safety net for any ntdll-internal AV the fast-path doesn't catch. */
        {
            /* ntdll range: get ntdll base dynamically so ASLR doesn't matter */
            static DWORD s_ntdll_lo = 0, s_ntdll_hi = 0;
            if (!s_ntdll_lo) {
                HMODULE hnt = GetModuleHandleA("ntdll.dll");
                if (hnt) {
                    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)hnt;
                    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS*)((BYTE*)hnt + dos->e_lfanew);
                    s_ntdll_lo = (DWORD)(ULONG_PTR)hnt;
                    s_ntdll_hi = s_ntdll_lo + nt->OptionalHeader.SizeOfImage;
                }
            }
            if (s_ntdll_lo && fault >= (ULONG_PTR)s_ntdll_lo && fault < (ULONG_PTR)s_ntdll_hi) {
                SLOG("VEH: render-thread AV INSIDE ntdll at 0x%08X (wait-block corruption) -- "
                     "ExitThread(0) to avoid corrupt-state longjmp",
                     (unsigned)fault);
                /* Reset jmp guard so the render loop doesn't misfire on the dead jmp_buf */
                InterlockedExchange(&s_render_jmp_armed, 0);
                s_render_tid = 0;   /* prevent VEH re-entry on this thread */
                InterlockedExchange(&s_render_thread_started, 0);  /* allow rgate_keeper restart */
                ExitThread(0);
                /* ExitThread does not return */
            }
        }
        /* Log first-3 stack traces so we can find the exact call site */
        {
            static volatile LONG s_rt_stk_cnt = 0;
            LONG sc = InterlockedIncrement(&s_rt_stk_cnt);
            if (sc <= 3) {
                DWORD *sp = (DWORD *)(ULONG_PTR)rctx->Esp;
                int ii;
                SLOG("VEH: render-thread AV fault=0x%08X EIP=0x%08X "
                     "EAX=0x%08X ECX=0x%08X EDX=0x%08X "
                     "ESI=0x%08X EDI=0x%08X EBX=0x%08X ESP=0x%08X",
                     (unsigned)fault, (unsigned)rctx->Eip,
                     (unsigned)rctx->Eax, (unsigned)rctx->Ecx,
                     (unsigned)rctx->Edx, (unsigned)rctx->Esi,
                     (unsigned)rctx->Edi, (unsigned)rctx->Ebx,
                     (unsigned)rctx->Esp);
                for (ii = 0; ii < 16; ii++) {
                    if (IsBadReadPtr(sp + ii, 4)) break;
                    DWORD v = sp[ii];
                    int in_kshg = (v >= 0x400000u && v < 0x700000u);
                    SLOG("VEH: render-thread stack[%02d]=0x%08lX%s",
                         ii, (unsigned long)v, in_kshg ? " (KSHG)" : "");
                }
            }
        }
        /* 0x412DBD: every-frame null-ECX crash.  Log ExceptionInformation[0/1] and
         * the instruction bytes once so we can identify the exact instruction and
         * the null member offset relative to the render object. */
        if (rctx->Eip == 0x412DBDu && s_render_jmp_armed) {
            static volatile LONG s_412_logged = 0;
            if (!InterlockedCompareExchange(&s_412_logged, 1, 0)) {
                ULONG_PTR exc0 = ep->ExceptionRecord->ExceptionInformation[0];
                ULONG_PTR exc1 = ep->ExceptionRecord->ExceptionInformation[1];
                DWORD ro  = !IsBadReadPtr((void*)0x6DE498u,4) ? *(DWORD*)0x6DE498u : 0;
                DWORD mbr = ro ? (!IsBadReadPtr((void*)(ULONG_PTR)(ro+(DWORD)exc1),4)
                                  ? *(DWORD*)(ULONG_PTR)(ro+(DWORD)exc1) : 0xDEADu) : 0;
                BYTE *p = (BYTE*)0x412DBDu;
                SLOG("VEH: 0x412DBD detail: ExcInfo[0]=%lu [1]=0x%08X render_obj=0x%08X "
                     "offset=0x%08X [obj+off]=0x%08X",
                     (unsigned long)exc0, (unsigned)exc1, (unsigned)ro,
                     (unsigned)exc1, (unsigned)mbr);
                if (!IsBadReadPtr(p, 12))
                    SLOG("VEH: 0x412DBD bytes: %02X %02X %02X %02X %02X %02X %02X %02X "
                         "%02X %02X %02X %02X",
                         p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                         p[8],p[9],p[10],p[11]);
            }
        }
        /* 0x412DBD: MOV EBX,[EDI] with EDI=0.  EDI is a pointer to an item-count
         * (valid range 0-7); it comes from board-object members that are null
         * because kacbookkeeping never initialized them.  Both in-range branches
         * (JL at +4, JGE at +9) jump to 0x412DFE - the "nothing to render" exit.
         * Skip there directly; the function epilogue at 0x412DFE restores saved
         * registers from the stack and returns normally. */
        if (rctx->Eip == 0x412DBDu && s_render_jmp_armed && rctx->Edi == 0) {
            static volatile LONG s_412_skip = 0;
            LONG n = InterlockedIncrement(&s_412_skip);
            if (n <= 5 || !(n % 1000))
                SLOG("VEH: 0x412DBD EDI=0 → skip to 0x412DFE (n=%ld)", (long)n);
            rctx->Eip = 0x412DFEu;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* 0x0042044E: REP MOVSD (F3 A5) writing 16 dwords to hardware shared RAM at
         * 0x00002100.  On real Konami hardware the board maps its state buffer to
         * low physical memory; on PC that address is unmapped and causes a WRITE AV.
         * Skip the instruction: simulate completion (ECX=0, advance ESI/EDI by
         * count*4, EIP += 2) so the copy is a no-op. */
        if (rctx->Eip == 0x42044Eu && s_render_jmp_armed) {
            static volatile LONG s_4204_skip = 0;
            LONG n = InterlockedIncrement(&s_4204_skip);
            DWORD cnt   = rctx->Ecx;
            DWORD bytes = cnt * 4;
            if (n <= 5 || !(n % 1000))
                SLOG("VEH: 0x42044E REP MOVSD → skip (n=%ld cnt=%lu EDI=0x%08X)",
                     (long)n, (unsigned long)cnt, (unsigned)rctx->Edi);
            rctx->Ecx  = 0;
            rctx->Esi += bytes;
            rctx->Edi += bytes;
            rctx->Eip  = 0x420450u;   /* skip 2-byte F3 A5 */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* 0x616C3E: MOVLPS XMM0,[ECX] where ECX=0x2140 (hardware shared RAM).
         * The function (reached via JMP-table at 0x5126D1→[0x006D0E3C]) is a
         * block of SSE128 loads from the board's low-memory registers.
         * Stack at crash: [ESP]=saved_EBP, [ESP+4]=ret(0x42045D), [ESP+8]=arg_0x2180,
         * [ESP+12]=arg_EBP.  Simulate RETN 8 (stdcall 2-arg cleanup):
         *   restore EBP, return to 0x42045D, skip 2 args (8 bytes). */
        if (rctx->Eip == 0x616C3Eu && s_render_jmp_armed) {
            static volatile LONG s_616_skip = 0;
            LONG n = InterlockedIncrement(&s_616_skip);
            if (n <= 5 || !(n % 1000)) {
                DWORD *stk = (DWORD*)(uintptr_t)rctx->Esp;
                SLOG("VEH: 0x616C3E hw-SSE-read → fake RETN8 to 0x%08X (n=%ld ESP=0x%08X)",
                     !IsBadReadPtr(stk+1,4) ? stk[1] : 0u, (long)n, (unsigned)rctx->Esp);
            }
            if (!IsBadReadPtr((DWORD*)(uintptr_t)rctx->Esp, 16)) {
                DWORD *stk  = (DWORD*)(uintptr_t)rctx->Esp;
                rctx->Ebp   = stk[0];           /* restore caller's EBP */
                rctx->Eip   = stk[1];           /* return address = 0x42045D */
                rctx->Esp  += 16;               /* pop saved_EBP(4)+ret(4)+2_args(8) */
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* 0x42045D: CMP BYTE PTR [EBX], 0  / JE → 0x420491
         * EBX=0 (null board-state pointer).  Take the JE branch.
         * F() has no local-variable stack allocation beyond the 4 pushed saved-
         * regs (verified: at 0x42044E ESP=[FC90], return_addr at [FC90+16]=DLL).
         * ESP is already correct for the epilogue's POPs; jump directly. */
        if (rctx->Eip == 0x42045Du && s_render_jmp_armed && rctx->Ebx == 0) {
            static volatile LONG s_4205_skip = 0;
            LONG n = InterlockedIncrement(&s_4205_skip);
            if (n <= 5 || !(n % 1000))
                SLOG("VEH: 0x42045D CMP[EBX=0] → take JE to 0x420491 (n=%ld)", (long)n);
            rctx->Eip = 0x420491u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* 0x3F490FDB: DLL caller of F() resumes with EAX=0 (F() returned null).
         * Instruction: MOV CL, [EAX]  (8A 08) - crashes because EAX=0 (null).
         * Set CL=0 (simulate read of 0) and skip the 2-byte instruction.
         * Note: 0x3F490FDB is inside the DLL that called F() and is stable within
         * this process lifetime; it may differ between sessions due to ASLR. */
        /* 0x3F490FDB: EXECUTE FAULT - DLL page is mapped without execute permission
         * (the page at 0x3F490000 is DATA-mapped by the DLL loader but the game
         * calls into it as code).  VirtualProtect adds PAGE_EXECUTE_READ so the
         * CPU can actually fetch and run the instruction.  We also fix EAX (which
         * F() nulled via XOR EAX,EAX) to point to a zero-filled fake block so the
         * subsequent MOV CL,[EAX+N] reads in that DLL function return 0 safely. */
        if (rctx->Eip == 0x3F490FDBu && s_render_jmp_armed) {
            static BYTE  s_hw_fake[256];      /* BSS, zero-initialized */
            static volatile LONG s_3f49_n = 0;
            LONG n = InterlockedIncrement(&s_3f49_n);
            if (n == 1) {
                DWORD old_prot = 0;
                BOOL ok = VirtualProtect((void*)0x3F490000u, 0x1000,
                                         PAGE_EXECUTE_READ, &old_prot);
                SLOG("VEH: 0x3F490FDB exec-fault → VirtualProtect(PAGE_EXECUTE_READ) %s old=0x%lX",
                     ok ? "OK" : "FAIL", (unsigned long)old_prot);
            }
            if (n <= 5 || !(n % 1000))
                SLOG("VEH: 0x3F490FDB EAX=0x%08X → fake hw-block 0x%08X (n=%ld)",
                     (unsigned)rctx->Eax, (unsigned)(ULONG_PTR)s_hw_fake, (long)n);
            rctx->Eax = (DWORD)(ULONG_PTR)s_hw_fake;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* Frame-0 shader-buffer crash: MOVZX EAX, BYTE PTR [ESI] at 0x418072.
         * ESI points into a GPU vertex/shader buffer that isn't CPU-accessible.
         * Fake the read as 0 (byte = null shader-type) and resume - this lets
         * frame 0 complete instead of corrupting the render object for frame 1+. */
        if (rctx->Eip == 0x418072u && s_render_jmp_armed) {
            rctx->Eax = 0;     /* simulate MOVZX EAX, 0 */
            rctx->Eip = 0x418075u;  /* skip 3-byte MOVZX EAX, BYTE [ESI] */
            SLOG("VEH: 0x418072 shader-buf skip: ESI=0x%08X → EAX=0, resume 0x418075",
                 (unsigned)rctx->Esi);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* Extra detail for out-of-range or first-seen crashes */
        {
            static volatile LONG s_last_unk = -1;
            LONG eip32 = (LONG)(DWORD)rctx->Eip;
            DWORD code_lo = 0x00401000u, code_hi = 0x00710000u;
            int out_of_range = ((DWORD)rctx->Eip < code_lo || (DWORD)rctx->Eip > code_hi);
            if (out_of_range || InterlockedCompareExchange(&s_last_unk, eip32, (LONG)(-1)) != (LONG)(-1)) {
                ULONG_PTR exc0 = ep->ExceptionRecord->ExceptionInformation[0];
                ULONG_PTR exc1 = ep->ExceptionRecord->ExceptionInformation[1];
                BYTE *p = (BYTE*)(uintptr_t)rctx->Eip;
                SLOG("VEH: render-thread DETAIL: ExcInfo[0]=%lu [1]=0x%08X out=%d "
                     "EAX=0x%08X ECX=0x%08X ESI=0x%08X EDI=0x%08X EBX=0x%08X ESP=0x%08X",
                     (unsigned long)exc0, (unsigned)exc1, (int)out_of_range,
                     (unsigned)rctx->Eax, (unsigned)rctx->Ecx,
                     (unsigned)rctx->Esi, (unsigned)rctx->Edi,
                     (unsigned)rctx->Ebx, (unsigned)rctx->Esp);
                if (!IsBadReadPtr(p, 8))
                    SLOG("VEH: render-thread DETAIL bytes@EIP: %02X %02X %02X %02X %02X %02X %02X %02X",
                         p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
                /* Stack context */
                DWORD *stk = (DWORD*)(uintptr_t)rctx->Esp;
                if (!IsBadReadPtr(stk, 24))
                    SLOG("VEH: render-thread DETAIL stack: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X [4]=0x%08X",
                         stk[0],stk[1],stk[2],stk[3],stk[4]);
            }
        }
        SLOG("VEH: render-thread AV fault=0x%08X EIP=0x%08X → longjmp",
             (unsigned)fault, (unsigned)rctx->Eip);
        if (InterlockedExchange(&s_render_jmp_armed, 0)) {
            s_render_exc_fault = (DWORD)fault;
            s_render_exc_eip   = rctx->Eip;
            s_render_exc_edx   = rctx->Edx;  /* vtable ptr for call-via-vtable crashes */
            longjmp(s_render_jmp, 1);
            /* longjmp does not return */
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* ── Non-render-thread, high fault: exit the faulting thread cleanly ───
     * High-address crashes (e.g. kacbookkeeping jumping into render-object
     * memory at 0x04234A57) are garbage-vtable calls from the detection
     * thread.  The main thread is blocked in WaitForMultipleObjects(n=2,
     * INFINITE) waiting for this thread's handle to be signaled.  Calling
     * ExitThread(0) here terminates the detection thread with exit-code 0,
     * which signals its handle, unblocks the main thread's WFO, and allows
     * the main thread to proceed with render-object initialization. */
    if (fault >= 0x10000) {
        DWORD cur_tid = GetCurrentThreadId();
        /* WHICH THREAD IS "MAIN"? Both ids, because g_main_thread_id ALONE IS
         * THE WRONG ONE.
         *
         * g_main_thread_id is assigned in DllMain, and DllMain runs on the
         * CreateRemoteThread INJECTION thread -- never the game's main thread.
         * The 2026-09-17 log proves it outright: the watchdog started "for main
         * tid=7116" (the DllMain thread) while the game's real main thread,
         * the one that called initialize/update/is_boot_all, was tid=12148.
         *
         * So the July guard below -- written after an investigation that
         * correctly identified this exact failure -- never matched, and
         * ExitThread(0) killed the game's main thread anyway. Right idea,
         * inert in practice, for two months.
         *
         * s_game_thread_id is the real one: recorded from inside the game's own
         * call to initialize()/update(). Test BOTH. */
        int is_main = (g_main_thread_id && cur_tid == g_main_thread_id) ||
                      (s_game_thread_id && cur_tid == s_game_thread_id);
        /* CONFIRMED via log (2026-07-21 investigation, run with main tid=35524):
         * this branch's own "detection thread" assumption is not always true.
         * It fired with tid=35524 in the log, which independently confirmed-
         * via-CreateMutexA IS the game's one true main thread, not an auxiliary
         * detection thread.  ExitThread(0) on the ACTUAL main thread doesn't
         * unblock anything -- it just permanently kills the one thread the
         * whole game depends on, while rgate_keeper/render/watchdog threads
         * keep the process alive and looping forever with nothing left to
         * drive it: 0% CPU, no window, indistinguishable from a genuine block
         * without a debugger (which is exactly how this was found -- the
         * watchdog's SuspendThread started failing, meaning the handle it had
         * opened no longer pointed at a live thread).  Only take the
         * sacrifice-this-thread shortcut for threads OTHER than the main one;
         * for the main thread itself, fall through to the general KSHG-resume
         * recovery below (vtable-RET / EBP-walk / stack-scan) instead of
         * killing it. */
        if (!is_main) {
            /* REVERTED 2026-07-23 (user report: game booted before this change,
             * stopped after it -- back out the risky part, keep the flag fix).
             * The 2026-07-23 "try resume-in-place first" change here (Method 0/
             * A/B before ExitThread) was live-tested to sometimes cascade into
             * ntdll's heap-corruption fail-fast (0xC000071C) on a second such
             * fault -- strictly worse than the plain ExitThread(0) it replaced,
             * and the likely cause of the new-crash regression. Restored to the
             * original immediate ExitThread(0) for non-main threads. The
             * g_veh_fired=1-before-exiting fix (an EARLIER, separate, same-day
             * fix -- see shg_NtWFSO's fake-WAIT_OBJECT_0 gating) is kept: it's a
             * plain flag assignment, not a control-flow change, and isn't
             * implicated in the crash. If this specific non-render-thread high-
             * fault path needs the resume-in-place treatment again in future,
             * re-derive it more carefully (e.g. only attempt resume for fault
             * patterns confirmed NOT mid-lock) rather than restoring this as-is. */
            g_veh_fired = 1;
            if (g_io_enabled)
                SLOG("VEH: AV fault=0x%08X EIP=0x%08X ESP=0x%08X tid=%lu (non-main high-fault: exiting thread) [dllmain_tid=%lu game_tid=%lu]",
                     (unsigned)fault, (unsigned)ep->ContextRecord->Eip,
                     (unsigned)ep->ContextRecord->Esp, (unsigned long)cur_tid,
                     (unsigned long)g_main_thread_id,
                     (unsigned long)s_game_thread_id);
            ExitThread(0);
            /* ExitThread does not return */
        } else {
            SLOG("VEH: AV fault=0x%08X EIP=0x%08X ESP=0x%08X tid=%lu (HIGH FAULT ON MAIN THREAD -- "
                 "NOT killing it; falling through to KSHG-resume recovery instead)",
                 (unsigned)fault, (unsigned)ep->ContextRecord->Eip,
                 (unsigned)ep->ContextRecord->Esp, (unsigned long)cur_tid);
            g_veh_fired = 1;
        }
        /* Non-main threads already ExitThread(0)'d above and never reach here.
         * Only the main-thread branch falls through to the general
         * g_io_enabled-gated recovery below. */
    }
    if (!g_io_enabled)
        return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&s_veh_count);
    if (n > VEH_MAX_CATCHES)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *ctx = ep->ContextRecord;
    SLOG("VEH[%ld]: AV fault=0x%08X EIP=0x%08X ESP=0x%08X EBP=0x%08X tid=%lu",
         (long)n, (unsigned)fault, (unsigned)ctx->Eip,
         (unsigned)ctx->Esp, (unsigned)ctx->Ebp,
         (unsigned long)GetCurrentThreadId());
    /* Log dispatcher's register context for first few fires (tells us what ESI
     * the game's render dispatch thread has - needed for render ESI setup) */
    if (n <= 5)
        SLOG("VEH[%ld]: disp-regs ESI=0x%08X EDI=0x%08X EBX=0x%08X EAX=0x%08X",
             (long)n, (unsigned)ctx->Esi, (unsigned)ctx->Edi,
             (unsigned)ctx->Ebx, (unsigned)ctx->Eax);

    /* ── Method 0: vtable crash → simulate RET from [ESP] ──────────────────
     * The crashing EIP (0x13, 0x14...) is the value of the board vtable slot,
     * read from an uninitialised object.  The CALL instruction that jumped here
     * already pushed the return address onto the stack at [ESP].  Pop it to
     * resume exactly where the vtable call would have returned.
     * Also zero the three board-object pointers so future 0x43F2D0 iterations
     * skip all vtable calls without crashing. */
    {
        /* Board object pointer zeroing REMOVED.
         * The JMP patches at 0x43F2D0 already prevent kacbookkeeping from
         * dereferencing these pointers - zeroing them here was belt-and-suspenders
         * that actively broke the render chain (which reads the same pointers to
         * find game objects for vtable dispatch).  Log them for diagnostics only. */
        if (n <= 3) {
            volatile DWORD *p614 = (volatile DWORD*)0x6DF614;
            volatile DWORD *p610 = (volatile DWORD*)0x6DF610;
            volatile DWORD *p60c = (volatile DWORD*)0x6DF60C;
            SLOG("VEH[%ld]: board ptrs: [0x6DF614]=%08lX [0x6DF610]=%08lX [0x6DF60C]=%08lX",
                 (long)n,
                 !IsBadReadPtr((void*)0x6DF614,4) ? (unsigned long)*p614 : 0xDEADUL,
                 !IsBadReadPtr((void*)0x6DF610,4) ? (unsigned long)*p610 : 0xDEADUL,
                 !IsBadReadPtr((void*)0x6DF60C,4) ? (unsigned long)*p60c : 0xDEADUL);
        }
        DWORD *stk = (DWORD*)(ULONG_PTR)ctx->Esp;
        if (!IsBadReadPtr(stk, 4)) {
            DWORD ret = *stk;
            /* Only accept a KSHG.exe address: an ntdll/winmm address at [ESP]
             * means the crash is deep inside a Windows DLL call chain; jumping
             * back into ntdll causes a secondary SxS crash.  Fall through to
             * EBP walk + stack scan to find the nearest KSHG.exe caller. */
            if (ret >= KSHG_CODE_LO && ret < KSHG_CODE_HI) {
                ctx->Eip = ret;
                ctx->Esp += 4;
                /* GHL hardware probe (EIP<0x10000 = null/ordinal fn ptr): return 1 (ok).
                 * Real vtbl/code crashes: return 0 to zero board state. */
                ctx->Eax = (fault < 0x10000u) ? 1 : 0;
                SLOG("VEH[%ld]: vtable-RET → 0x%08X eax=%lu", (long)n, (unsigned)ret, (unsigned long)ctx->Eax);
                g_veh_fired = 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            SLOG("VEH[%ld]: [ESP]=0x%08X not KSHG range, fall to EBP walk", (long)n, (unsigned)ret);
        } else {
            SLOG("VEH[%ld]: [ESP] unreadable, fall to EBP walk", (long)n);
        }
    }

    /* Method A: EBP chain walk - follow saved-EBP pointers until we find a
     * KSHG.exe return address.  Standard MSVC frames: [EBP]=saved_EBP,
     * [EBP+4]=return_addr.  This avoids the winmm/ntdll chain entirely. */
    {
        DWORD ebp = ctx->Ebp;
        int depth;
        SLOG("VEH: EBP-walk start EBP=0x%08X", (unsigned)ebp);
        for (depth = 0; depth < 32; depth++) {
            if (ebp < 0x00010000u || ebp >= 0x80000000u) break;
            if (IsBadReadPtr((void*)(ULONG_PTR)ebp, 8)) {
                SLOG("VEH: EBP[%d]=0x%08X not readable", depth, (unsigned)ebp);
                break;
            }
            DWORD next_ebp = *(DWORD*)(ULONG_PTR)ebp;
            DWORD ret_addr = *(DWORD*)(ULONG_PTR)(ebp + 4);
            SLOG("VEH: EBP[%d]=0x%08X next=0x%08X ret=0x%08X",
                 depth, (unsigned)ebp, (unsigned)next_ebp, (unsigned)ret_addr);
            if (ret_addr >= KSHG_CODE_LO && ret_addr < KSHG_CODE_HI) {
                SLOG("VEH: EBP-walk → KSHG ret=0x%08X depth=%d, resuming",
                     (unsigned)ret_addr, depth);
                ctx->Eip = ret_addr;
                ctx->Esp = ebp + 8;
                ctx->Ebp = next_ebp;
                ctx->Eax = (fault < 0x10000u) ? 1 : 0;
                g_veh_fired = 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (next_ebp <= ebp) { SLOG("VEH: EBP[%d] non-ascending 0x%08X→0x%08X, stop", depth, (unsigned)ebp, (unsigned)next_ebp); break; }
            ebp = next_ebp;
        }
        SLOG("VEH: EBP-walk: no KSHG addr found in %d frames", depth);
    }

    /* Method B: Linear stack scan with CALL opcode filter.
     * For each KSHG-range address on the stack, check if the byte at addr-5
     * is 0xE8 (CALL rel32).  A genuine return address always has 0xE8 at -5;
     * vtable thunks and data pointers (like 0x0066CE68) have 0xE9 (JMP) or
     * 0xCC (INT3) and are skipped.
     * On the first catch: scan full range and log ALL KSHG candidates to map
     * the outer calibration call chain. */
    {
        DWORD *sp = (DWORD*)(ULONG_PTR)ctx->Esp;
        int si;
        int resume_si = -1;
        DWORD resume_v = 0;
        int full_scan = (n <= 2);  /* log everything on first two catches */
        SLOG("VEH: stack-scan from ESP=0x%08X (full=%d)", (unsigned)ctx->Esp, full_scan);
        for (si = 0; si < 256; si++) {
            if (IsBadReadPtr(sp + si, 4)) { SLOG("VEH: scan stop at [%d] (unreadable)", si); break; }
            DWORD v = sp[si];
            if (v < KSHG_CODE_LO || v >= KSHG_CODE_HI) continue;
            if (IsBadReadPtr((void*)(ULONG_PTR)(v - 5), 1)) continue;
            BYTE pre = *(BYTE*)(ULONG_PTR)(v - 5);
            SLOG("VEH: scan[%d]=0x%08X pre[-5]=0x%02X", si, (unsigned)v, (unsigned)pre);
            if (pre == 0xE8 && resume_si < 0) {
                resume_si = si;
                resume_v  = v;
                if (!full_scan) break;  /* only need first match on later catches */
            }
        }
        if (resume_si >= 0) {
            SLOG("VEH: resume → KSHG 0x%08X at stack[%d] (ESP+=%d)",
                 (unsigned)resume_v, resume_si, (resume_si + 1) * 4);
            ctx->Eip  = resume_v;
            ctx->Esp += (DWORD)((resume_si + 1) * 4);
            ctx->Eax  = (fault < 0x10000u) ? 1 : 0;
            g_veh_fired = 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        SLOG("VEH: scan-walk: no CALL-preceded KSHG addr in %d entries", si);
    }

    SLOG("VEH[%ld]: all resume methods failed", (long)n);
    /* Last-resort sacrifice: resume-in-place genuinely failed (no KSHG-range
     * return address found near the fault by any of the three methods above).
     * Only now, after resume has had its shot, fall back to the original
     * ExitThread(0) behavior for non-main threads -- see the long comment
     * where this branch is entered above for why it is no longer the FIRST
     * move. Never do this for the main thread (unchanged from before: killing
     * it just wedges the process with nothing left to drive it). */
    {
        DWORD cur_tid2 = GetCurrentThreadId();
        int is_main2 = (g_main_thread_id && cur_tid2 == g_main_thread_id) ||
                       (s_game_thread_id && cur_tid2 == s_game_thread_id);
        if (!is_main2) {
            SLOG("VEH[%ld]: non-main tid=%lu resume failed -- ExitThread(0) last resort", (long)n, (unsigned long)cur_tid2);
            ExitThread(0);
            /* ExitThread does not return */
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── DllMain ────────────────────────────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) {
    if (r == DLL_PROCESS_DETACH) {
        /* Tell the life watchdog this is an ORDERLY exit. The main thread ends
         * during a normal shutdown too, and without this flag the watchdog
         * would race a clean exit and force-terminate a process that was
         * already leaving politely. */
        g_shutting_down = 1;
        if (s_veh_handle) { RemoveVectoredExceptionHandler(s_veh_handle); s_veh_handle = NULL; }
    }
    if (r == DLL_PROCESS_ATTACH) {
        /* Capture the main thread's TID as the very first thing we do -
         * DllMain runs on it (loader injects via QueueUserAPC on the primary
         * thread; see konami_pc_main.c).  Must happen before AddVectoredExceptionHandler
         * is installed further down so shg_veh can rely on it from the start. */
        g_main_thread_id = GetCurrentThreadId();
        /* Fixed-path probe: written unconditionally so we know DllMain ran
         * even when ULTRA_SHAIOLIB_LOG is not in the environment. */
        { char _ep[MAX_PATH]; GetTempPathA(MAX_PATH, _ep); strncat(_ep, "shg_early.log", MAX_PATH-16);
          FILE *_ef = fopen(_ep, "w");
          if (_ef) { fprintf(_ef, "DllMain: ATTACH pid=%lu\n", (unsigned long)GetCurrentProcessId()); fflush(_ef); fclose(_ef); } }
        /* Force the log file open NOW (before any game code runs) so SetupDi /
         * HID shim calls that happen before shaiolib_initialize() are captured. */
        shg_logf();
        SLOG("DllMain: DLL_PROCESS_ATTACH, installing IAT hooks");

        /* HID file shim: intercept direct HID board opens so the game gets clean
         * zero reports (trigger=0, axes=0) instead of an INVALID_HANDLE_VALUE
         * from the missing GHL I/O board. */
        shg_iat_hook("kernel32.dll", "CreateFileA",     shg_CreateFileA,     (void**)&s_real_CFA);
        shg_iat_hook("kernel32.dll", "CreateFileW",     shg_CreateFileW,     (void**)&s_real_CFW);
        shg_iat_hook("kernel32.dll", "ReadFile",        shg_ReadFile,        (void**)&s_real_RF);
        shg_iat_hook("kernel32.dll", "DeviceIoControl", shg_DeviceIoControl, (void**)&s_real_DIC);
        shg_iat_hook("kernel32.dll", "CloseHandle",     shg_CloseHandle,     (void**)&s_real_CH);
        /* Diagnostic only (see comment at definition): trace what KSHG.exe's
         * direct CreateMutexA import is used for. */
        shg_iat_hook("kernel32.dll", "CreateMutexA",    shg_CreateMutexA,    (void**)&s_real_CMA);
        /* SetupAPI + HidD shim: intercept GetProcAddress so dynamic setupapi
         * lookups return our stubs that report one fake GHL device.  Without
         * this the game gets ERROR_NO_MORE_ITEMS → enters error mode → I/O
         * CHECK shows all buttons stuck ON and DOWN permanently pressed. */
        shg_iat_hook("kernel32.dll", "GetProcAddress",       shg_GetProcAddress,  (void**)&s_real_GPA);
        shg_iat_hook("kernel32.dll", "LoadLibraryA",        shg_LoadLibraryA,    (void**)&s_real_LLA);
        shg_iat_hook("kernel32.dll", "LoadLibraryW",        shg_LoadLibraryW,    (void**)&s_real_LLW);
        shg_iat_hook("kernel32.dll", "LoadLibraryExA",      shg_LoadLibraryExA,  (void**)&s_real_LLEA);
        shg_iat_hook("kernel32.dll", "LoadLibraryExW",      shg_LoadLibraryExW,  (void**)&s_real_LLEW);
        /* Calibration visibility: intercept Sleep + waits so we can see what
         * the calibration is blocking on after is_boot_board_gun returns 1. */
        shg_iat_hook("kernel32.dll", "Sleep",               shg_Sleep,           (void**)&s_real_Slp);
        shg_iat_hook("kernel32.dll", "WaitForSingleObject", shg_WFSO,            (void**)&s_real_WFSO);
        shg_iat_hook("kernel32.dll", "WaitForMultipleObjects", shg_WFMO,         (void**)&s_real_WFMO);
        /* Belt-and-suspenders: zero raw mouse button flags on GetRawInputData. */
        shg_iat_hook("user32.dll",   "GetRawInputData",     shg_GetRawInputData, (void**)&s_real_GetRID);
        /* Main game loop: intercept MsgWaitForMultipleObjects so we can log its
         * timeout and cap pre-IO waits that would stall frame delivery. */
        shg_iat_hook("user32.dll",   "MsgWaitForMultipleObjects", shg_MWFMO,     (void**)&s_real_MWFMO2);
        /* Auto-calibration: intercept both key-state functions so the GUN
         * CALIBRATION tight loop sees "LMB pressed" for 500 ms post-security.
         * GetAsyncKeyState is updated by SendInput (hardware path).
         * GetKeyState is only updated when the message pump processes messages;
         * hooking it covers games that use GKS in a frozen-pump tight loop. */
        shg_iat_hook("user32.dll",   "GetAsyncKeyState",   shg_GetAsyncKeyState,   (void**)&s_real_GAKS);
        shg_iat_hook("user32.dll",   "GetKeyState",        shg_GetKeyState,        (void**)&s_real_GKS);
        /* Suppress "ERR shaiolib_initialize" and similar blocking error dialogs. */
        shg_iat_hook("user32.dll",   "MessageBoxA",        shg_MessageBoxA,        (void**)&s_real_MBA);
        shg_iat_hook("user32.dll",   "MessageBoxW",        shg_MessageBoxW,        (void**)&s_real_MBW);
        /* e-amusement server ping: KSHG.exe calls IcmpSendEcho(…,timeout) to test
         * reachability.  Without a real server the call blocks until timeout elapses
         * (~30-120 s).  Return failure immediately.
         * Note: shg_iat_hook silently fails if icmp.dll isn't in the process yet
         * (delay-loaded).  shg_install_icmp_inline() + shg_check_and_patch cover
         * the lazy-load case; GPA hook covers runtime GetProcAddress. */
        shg_iat_hook("icmp.dll",     "IcmpSendEcho",       shg_IcmpSendEcho,       (void**)&s_real_IcmpSE);
        SLOG("DllMain: IcmpSendEcho IAT hook %s (real=%p)",
             s_real_IcmpSE ? "OK" : "MISS (icmp.dll not in IAT yet, inline will cover)",
             (void*)s_real_IcmpSE);

        SLOG("DllMain: hooks armed (CFA=%p GPA=%p GetRID=%p GAKS=%p GKS=%p)",
             (void*)s_real_CFA, (void*)s_real_GPA, (void*)s_real_GetRID,
             (void*)s_real_GAKS, (void*)s_real_GKS);

        /* What name did this process actually inherit for the loader's input
         * block? The liveness half of this probe runs in update() at frame 600;
         * this half just records the name, because the name alone distinguishes
         * the loader's publisher from the module's own replacement. */
        {   char nm[128];
            DWORD nlen = GetEnvironmentVariableA("ULTRA_IO_SHM", nm, (DWORD)sizeof nm);
            SLOG("SHM-PROBE: ULTRA_IO_SHM=[%s] (len=%lu)",
                 (nlen && nlen < sizeof nm) ? nm : "(unset)", (unsigned long)nlen);
        }

        /* winmm callback protection: intercept waveOutOpen + timeSetEvent so any
         * near-null callback pointer (never filled in by absent GHL hardware) is
         * replaced with a safe no-op before it reaches winmm.  Covers both the
         * static-IAT path (KSHG.exe) and dynamic-load paths (GPA hook above). */
        shg_iat_hook("winmm.dll", "waveOutOpen",  shg_waveOutOpen,  (void**)&s_real_WOO);
        shg_iat_hook("winmm.dll", "timeSetEvent", shg_timeSetEvent, (void**)&s_real_TSE);
        /* Observe-only. NOT logged as "armed": shg_iat_hook fills *orig from
         * GetProcAddress BEFORE it attempts the patch, so a non-NULL pointer
         * proves the export resolved, not that the IAT slot was rewritten. The
         * MMIO: lines in the log are the only honest proof it took. */
        shg_iat_hook("winmm.dll", "mmioOpenA",    shg_mmioOpenA,    (void**)&s_real_MMIOA);
        SLOG("DllMain: winmm IAT hooks armed (WOO=%p TSE=%p)",
             (void*)s_real_WOO, (void*)s_real_TSE);
        /* D3D9 windowed-mode: patch KSHG.exe's static IAT entry.
         * Game statically imports Direct3DCreate9 (Windows loader fills it before
         * DllMain, bypassing GetProcAddress).  Patch the IAT slot directly. */
        shg_iat_hook("d3d9.dll", "Direct3DCreate9", shg_Direct3DCreate9, (void**)&s_real_D3DC9);
        SLOG("DllMain: d3d9 IAT Direct3DCreate9 %s (real=%p)",
             s_real_D3DC9 ? "OK" : "MISS", (void*)s_real_D3DC9);
        /* Runs here because this DllMain precedes KSHG's entry point, so it
         * lands before the boot display-mode choice and before the render
         * targets are built. Default level 0 = nothing patched. */
        shg_apply_internal_res();
        /* Inline-hook timeSetEvent and CreateTimerQueueTimer at function-byte level
         * so any cached pointer (obtained before our GPA hook was active) also
         * goes through our bad-callback filters. */
        shg_install_tse_inline();
        shg_install_ctqt_inline();
        shg_install_quapc_inline();
        shg_install_connect_inline();
        /* The Nt*-wait-primitive inline family (NtWaitForSingleObject/
         * NtWaitForKeyedEvent/NtWaitForMultipleObjects capping, the message-
         * loop visibility shims, and - critically - NtSignalAndWaitForSingleObject/
         * NtWaitForAlertByThreadId, installed by shg_install_hidden_wait_inline())
         * is exactly the layer implicated in the ntdll-internal RtlWakeAddressAll
         * wait-block-list race documented in docs/STOCK_TAKE.md and reproduced
         * fresh this pass (VEH-ANY access violation at DLL_PROCESS_ATTACH time,
         * before shaiolib_initialize()/any of this file's own threads even run).
         * Opt-in only now (ULTRA_KONAMI_DEEP_HOOKS=1) - see the header comment
         * near shg_deep_hooks_enabled() above for the full rationale. */
        if (shg_deep_hooks_enabled()) {
        shg_install_ntwfso_inline();
        shg_install_ntwfke_inline();
        shg_install_ntwfmo_inline();
        /* Message-loop visibility: GetMessage/PeekMessage/WaitMessage's block
         * is serviced by win32k, invisible to the Nt*-wait-primitive hooks
         * above.  See comment at shg_install_msgloop_inline definition. */
        shg_install_msgloop_inline();
        /* Two more wait mechanisms invisible to everything above - see
         * comment at shg_install_hidden_wait_inline definition. */
        shg_install_hidden_wait_inline();
        } else {
            SLOG("DllMain: ULTRA_KONAMI_DEEP_HOOKS=0 -- skipping NtWFSO/NtWFKE/"
                 "NtWFMO-cap, msgloop-inline, hidden-wait-inline (debug opt-out; "
                 "the main thread's kernel wait will NOT be capped and will likely "
                 "block forever -- expect a stall, not a crash). This is also the "
                 "layer implicated in the ntdll RtlWakeAddressAll race, so disabling "
                 "it is useful ONLY to isolate whether a future crash comes from here.");
        }
        /* CreateFileA/W + ReadFile are so far only IAT-patched on an explicit
         * allowlist of modules (KSHG.exe, shaerror/libutil/libbms/kacbookkeeping/
         * tabooword.dll).  msvcrt.dll and ucrtbase.dll (both loaded - confirmed
         * via 32-bit-PowerShell module dump of a hung instance) do their own
         * buffered file I/O (fopen/fread) through their OWN private IATs, which
         * none of the per-module patches above touch - a blocking open/read
         * from CRT code would be completely invisible to every hook installed
         * so far.  shg_CreateFileA/W and shg_ReadFile already prefer
         * s_c[fa|fw]_tramp / s_rf_tramp over s_real_CFA/CFW/RF (see their
         * bodies) - that trampoline plumbing was already in place but never
         * wired to an actual inline installer.  Do that now: patch
         * kernel32.dll's real entry points so every caller in the process,
         * regardless of which DLL's IAT it goes through, is covered. */
        shg_install_dll_inline("kernel32.dll", "CreateFileA", (void*)shg_CreateFileA, (void**)&s_cfa_tramp);
        shg_install_dll_inline("kernel32.dll", "CreateFileW", (void*)shg_CreateFileW, (void**)&s_cfw_tramp);
        shg_install_dll_inline("kernel32.dll", "ReadFile",    (void*)shg_ReadFile,    (void**)&s_rf_tramp);
        /* NOTE: do NOT preload icmp.dll here - doing so signals network availability
         * to libbms.dll, causing an 87-second early boot stall.  The inline hook is
         * installed lazily via shg_LoadLibraryExA when the game actually loads icmp. */

        /* Cache our DLL's code range for VEH render-thread stack scanning */
        shg_cache_dll_range(h);

        /* Binary patches: fix board vtable crash in 0x43F2D0 and force calibration
         * loop exit in 0x4D0030.  Applied here so they are in place before any
         * game code executes. */
        shg_apply_game_patches();

        /* Null/near-null function-pointer guard: belt-and-suspenders VEH in case
         * any callback-registration path is missed by the winmm hooks above. */
        s_veh_handle = AddVectoredExceptionHandler(1, shg_veh);
        SLOG("DllMain: VEH installed (null-fn-ptr guard) handle=%p", s_veh_handle);

        /* shaerror.dll does its own GHL board detection via LoadLibraryA +
         * GetProcAddress (its IAT only imports KERNEL32).  Our KSHG.exe hooks
         * above never touch shaerror.dll's IAT, so it calls the real SetupDi,
         * finds no GHL board, and enters error mode - causing the gray precheck
         * page to show all inputs stuck ON (DOWN + TRIGGER held) before the
         * main game even loads.  Patch shaerror.dll's own IAT with the same
         * GHL-board-faking hooks so its board detection succeeds. */
        HMODULE hSE = GetModuleHandleA("shaerror.dll");
        if (hSE) {
            shg_iat_hook_mod(hSE, "kernel32.dll", "GetProcAddress",  shg_GetProcAddress);
            shg_iat_hook_mod(hSE, "kernel32.dll", "CreateFileA",     shg_CreateFileA);
            shg_iat_hook_mod(hSE, "kernel32.dll", "CreateFileW",     shg_CreateFileW);
            shg_iat_hook_mod(hSE, "kernel32.dll", "CloseHandle",     shg_CloseHandle);
            shg_iat_hook_mod(hSE, "user32.dll",   "MessageBoxA",     shg_MessageBoxA);
            shg_iat_hook_mod(hSE, "user32.dll",   "MessageBoxW",     shg_MessageBoxW);
            SLOG("DllMain: shaerror.dll IAT patched (GPA+CFA+CFW+CH+MBA+MBW)");
        } else {
            SLOG("DllMain: WARNING shaerror.dll not found in process");
        }

        /* libutil.dll and libbms.dll use dynamic loading (LoadLibraryA +
         * GetProcAddress) to resolve HID, SetupDi, and GetAsyncKeyState at
         * calibration time.  Those GPA calls go through each DLL's OWN IAT,
         * bypassing the KSHG.exe IAT patch above.  Patch their GPA IAT slot
         * so any dynamic lookup for GAKS / CFA / ReadFile / SetupDi* returns
         * our stubs - the same shg_GetProcAddress that KSHG.exe and
         * shaerror.dll already use. */
        {
            static const char *const s_supp[] = { "libutil.dll", "libbms.dll", NULL };
            const char *const *sd;
            for (sd = s_supp; *sd; ++sd) {
                HMODULE hMod = GetModuleHandleA(*sd);
                if (!hMod) { SLOG("DllMain: %s not in process (skip)", *sd); continue; }
                shg_iat_hook_mod(hMod, "kernel32.dll", "GetProcAddress",        shg_GetProcAddress);
                shg_iat_hook_mod(hMod, "kernel32.dll", "CreateFileA",           shg_CreateFileA);
                shg_iat_hook_mod(hMod, "kernel32.dll", "CreateFileW",           shg_CreateFileW);
                shg_iat_hook_mod(hMod, "kernel32.dll", "ReadFile",              shg_ReadFile);
                shg_iat_hook_mod(hMod, "kernel32.dll", "CloseHandle",           shg_CloseHandle);
                shg_iat_hook_mod(hMod, "kernel32.dll", "LoadLibraryA",          shg_LoadLibraryA);
                shg_iat_hook_mod(hMod, "kernel32.dll", "LoadLibraryW",          shg_LoadLibraryW);
                shg_iat_hook_mod(hMod, "kernel32.dll", "LoadLibraryExA",        shg_LoadLibraryExA);
                shg_iat_hook_mod(hMod, "kernel32.dll", "LoadLibraryExW",        shg_LoadLibraryExW);
                /* Cap waits so libbms.dll e-amusement network init doesn't stall boot. */
                shg_iat_hook_mod(hMod, "kernel32.dll", "Sleep",                 shg_Sleep);
                shg_iat_hook_mod(hMod, "kernel32.dll", "WaitForSingleObject",   shg_WFSO);
                shg_iat_hook_mod(hMod, "kernel32.dll", "WaitForMultipleObjects",shg_WFMO);
                /* Direct icmp.dll import (if statically linked - rare). */
                shg_iat_hook_mod(hMod, "icmp.dll",     "IcmpSendEcho",          shg_IcmpSendEcho);
                SLOG("DllMain: %s IAT patched (GPA+CFA+CFW+RF+CH+LLA+LLW+LLExA+LLExW+Slp+WFSO+WFMO+Icmp)", *sd);
            }
        }

        /* kacbookkeeping.dll and tabooword.dll are statically imported by KSHG.exe
         * but their OWN IATs are unpatched - any CFA/ReadFile/Sleep calls from
         * within those DLLs bypass all our hooks.  Patch them now. */
        {
            HMODULE hKac = GetModuleHandleA("kacbookkeeping.dll");
            if (hKac) {
                shg_iat_hook_mod(hKac, "kernel32.dll", "GetProcAddress",       shg_GetProcAddress);
                shg_iat_hook_mod(hKac, "kernel32.dll", "CreateFileA",          shg_CreateFileA);
                shg_iat_hook_mod(hKac, "kernel32.dll", "ReadFile",             shg_ReadFile);
                shg_iat_hook_mod(hKac, "kernel32.dll", "CloseHandle",          shg_CloseHandle);
                shg_iat_hook_mod(hKac, "kernel32.dll", "Sleep",                shg_Sleep);
                shg_iat_hook_mod(hKac, "kernel32.dll", "WaitForSingleObject",  shg_WFSO);
                shg_iat_hook_mod(hKac, "kernel32.dll", "LoadLibraryA",         shg_LoadLibraryA);
                shg_iat_hook_mod(hKac, "winmm.dll",    "waveOutOpen",          shg_waveOutOpen);
                shg_iat_hook_mod(hKac, "winmm.dll",    "timeSetEvent",         shg_timeSetEvent);
                shg_iat_hook_mod(hKac, "ws2_32.dll",   "connect",              shg_connect);
                SLOG("DllMain: kacbookkeeping.dll IAT patched");
            } else {
                SLOG("DllMain: kacbookkeeping.dll not found");
            }
            HMODULE hTab = GetModuleHandleA("tabooword.dll");
            if (hTab) {
                shg_iat_hook_mod(hTab, "kernel32.dll", "GetProcAddress",  shg_GetProcAddress);
                shg_iat_hook_mod(hTab, "kernel32.dll", "CreateFileA",     shg_CreateFileA);
                shg_iat_hook_mod(hTab, "kernel32.dll", "LoadLibraryA",    shg_LoadLibraryA);
                SLOG("DllMain: tabooword.dll IAT patched");
            } else {
                SLOG("DllMain: tabooword.dll not found");
            }
        }

        /* Populate the interface object slot that shaiolib.dll owns at [0x680D88].
         * The original shaiolib.dll wrote a real C++ interface here at DLL-init
         * time.  Without it, the game reads the raw PE binary default (a code
         * pointer, 0x0065089D) and calls [code_bytes + 0x14] as a vtable method,
         * getting garbage bytes → crash every render frame. */
        shg_install_interface_680D88();

        /* EaxMan.dll, dsound.dll, d3d9.dll - statically imported by KSHG.exe.
         * Their internal WaitForSingleObject/Sleep calls bypass KSHG.exe's IAT
         * and bypass all of our hooks unless we patch their OWN IATs. */
        {
            static const char *const s_audio[] = {
                "EaxMan.dll", "dsound.dll", "d3d9.dll", "dinput8.dll", NULL };
            const char *const *ad;
            for (ad = s_audio; *ad; ++ad) {
                HMODULE hA = GetModuleHandleA(*ad);
                if (!hA) { SLOG("DllMain: %s not in process (skip)", *ad); continue; }
                shg_iat_hook_mod(hA, "kernel32.dll", "Sleep",               shg_Sleep);
                shg_iat_hook_mod(hA, "kernel32.dll", "WaitForSingleObject", shg_WFSO);
                shg_iat_hook_mod(hA, "kernel32.dll", "WaitForMultipleObjects", shg_WFMO);
                shg_iat_hook_mod(hA, "kernel32.dll", "LoadLibraryA",        shg_LoadLibraryA);
                shg_iat_hook_mod(hA, "kernel32.dll", "LoadLibraryExA",      shg_LoadLibraryExA);
                shg_iat_hook_mod(hA, "user32.dll",   "MsgWaitForMultipleObjects", shg_MWFMO);
                SLOG("DllMain: %s IAT patched (Slp+WFSO+WFMO+LLA+LLExA+MWFMO)", *ad);
            }
        }

        /* EaxManagerCreate hook: intercept to log EAX object creation */
        {
            HMODULE hEax = GetModuleHandleA("EaxMan.dll");
            if (hEax) {
                s_real_EaxCreate = (pfnEaxCreate)GetProcAddress(hEax, "EaxManagerCreate");
                shg_iat_hook_mod(GetModuleHandleA(NULL), "EaxMan.dll",
                                 "EaxManagerCreate", shg_EaxManagerCreate);
                SLOG("EaxManagerCreate: hooked (real=%p)", (void*)s_real_EaxCreate);
            } else {
                SLOG("EaxManagerCreate: EaxMan.dll not loaded yet");
            }
        }

        /* EIP watchdog: fully implemented above (shg_watchdog_proc /
         * shg_start_watchdog) but never actually wired up anywhere in this
         * file - it was prepared for a VEH[256]-triggered scenario that
         * doesn't match what we're actually seeing: the main thread goes
         * completely silent (no further activity from ANY of the ~25 hooked
         * APIs above, covering every mainstream wait/IO primitive) within
         * well under a second, with no exception, no crash, no log line.
         * We need to know its EIP directly rather than keep guessing at API
         * names.  DllMain runs ON the main thread (the loader injects via
         * QueueUserAPC on the primary thread - see konami_pc_main.c), so
         * calling this here, unconditionally, correctly captures the right
         * thread via shg_start_watchdog()'s GetCurrentThreadId().  Output
         * goes to %TEMP%\shg_watchdog.log (raw WriteFile, not SLOG) so it
         * keeps working even while the main thread is suspended for
         * sampling. */
        shg_start_watchdog();

        /* No function-byte detours - IAT + GPA hooks are sufficient. */
    }
    (void)h; (void)v;
    return TRUE;
}
