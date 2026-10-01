/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_dsii.c -- Cave PC "Deathsmiles II" per-game board_io_ops
 * =============================================================================
 *
 *  EXTRACTION NOTE: this file consolidates every Deathsmiles-II-specific hack
 *  that used to live INLINE inside jvs_serial_hook.c's otherwise-generic
 *  dispatcher functions (Hook_CreateFileA, Hook_ReadFile, Hook_WriteFile,
 *  Hook_CloseHandle, Hook_WaitCommEvent, Hook_LoadLibraryA/W, ...), gated by a
 *  runtime `cave_io_active()` (ULTRA_CAVE_IO) check. Because those branches
 *  physically lived in the SAME shared .c file every other board (Konami PC,
 *  Sega Nu, Namco ES, ...) is built from, a DSII-specific fix could -- and did
 *  -- risk regressing an unrelated board built from the same source tree. See
 *  board_io_ops.h for the full architectural rationale.
 *
 *  Every piece below moved here VERBATIM (RE documentation preserved word for
 *  word -- this is real reverse-engineering effort against `ds2_monitor` /
 *  real CV2000XP hardware behavior, not something to "clean up" in the move):
 *
 *   - is_cvkey_path()               (CVKEY.222 path detection, unchanged)
 *   - Hook_CreateFileA's CVKEY branch          -> dsii_on_create_file()
 *   - Hook_ReadFile's CVKEY synth-key logic     -> dsii_on_read_file()
 *   - Hook_CloseHandle's g_cvkey_fake clear     -> dsii_on_close_handle()
 *   - cave_io_respond()             (Cave COM3 9-byte reply-frame builder, unchanged)
 *   - cave_direct_write_thread()    (~120Hz belt-and-suspenders poke, unchanged)
 *   - cave_kb_thread/cave_ll_kb_proc/cave_ri_wnd_proc (raw-input+LL keyboard, unchanged)
 *   - dsii_nop_input_poll_guard()   (unchanged)
 *   - the GetIfTable NIC-check-bypass shim      -> Hook_GetIfTable() + a new
 *     exported accessor, game_dsii_hook_getproctable() (see KNOWN GAP #2 below)
 *   - Hook_WaitCommEvent's Cave OR-condition    -> dsii_on_wait_comm_event_pending()
 *   - the dinput8.dll LoadLibrary redirect      -> dsii_on_load_library_redirect()
 *
 *  WHAT'S DIFFERENT FROM THE OLD INLINE CODE (deliberate, not accidental):
 *
 *   1. GATING. The old code gated every one of these branches on
 *      cave_io_active() (ULTRA_CAVE_IO=1), a flag board_profile.c's "cave_pc"
 *      board entry documents as shared by SIX Cave titles (Deathsmiles II,
 *      DoDonPachi SaiDaiOuJou, Akai Katana Shin, Espgaluda II, Ketsui, Muchi
 *      Muchi Pork) -- but every VA/offset/protocol detail actually encoded
 *      here (0x774b54/0x774ab8 field offsets, the 3,616,768-byte exe size, the
 *      0x45c37d guard address, the CVKEY reference-key offsets) was reverse-
 *      engineered against ONE specific binary: Deathsmiles II. The other five
 *      titles are BP_READY_DUMP_GATED (never dumped/tested); applying DSII's
 *      exact byte offsets to a different Cave PC binary via a shared flag would
 *      be the EXACT cross-contamination bug board_io_ops.h exists to close, the
 *      moment one of those five is ever dumped and someone sets ULTRA_CAVE_IO=1
 *      on it expecting "generic Cave PC support". So this file's
 *      ultra_board_ops_autoregister() gates registration -- and therefore every
 *      callback below -- on DSII's own exe-identity sniff (exe size, exactly as
 *      the old code already required for JUST the nop-guard/direct-write-thread
 *      pair) or an explicit ULTRA_CAVE_DSII=1 escape hatch, the SAME pattern
 *      gamestate_probe.c/game_chaosbreaker.c use for Chaos Breaker's
 *      AOE_CHAOS_BREAKER check. ULTRA_CAVE_IO is not read anywhere in this file
 *      any more: once this ops is registered, "is this DSII" and "is Cave I/O
 *      active" collapse into the SAME question, decided once, correctly.
 *
 *   2. RESPONSE QUEUING. The old cave_io_respond() output was appended into
 *      jvs_serial_hook.c's private per-handle `com_slot.resp[]` queue (shared
 *      infrastructure, g_slots/slot_for(), both file-scope `static` there).
 *      board_io_ops.h's on_write_file has no output-buffer parameter and its
 *      doc says a successful return means "any response ... was queued
 *      internally by the callback" -- so this file keeps its OWN tiny
 *      single-slot response queue (s_resp_buf/len/pos, s_resp_lock) instead of
 *      reaching into jvs_serial_hook.c's private com_slot table. Behaviorally
 *      equivalent for DSII (one Cave COM3 handle live at a time): same FIFO
 *      byte-exact drain-with-partial-read semantics, just scoped to this file.
 *
 *  See "KNOWN GAPS" further down for the two follow-up edits jvs_serial_hook.c
 *  still needs before this file link-completes and is 100% crash-safe.
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "board_io_ops.h"
#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "jvs_core.h"

/* ============================================================================
 *  Dependencies on shared hook plumbing (hook_dllmain.c / jvs_serial_hook.c)
 * ============================================================================
 *  Kept to the absolute minimum -- everything else in this file is private.
 */

/* hook_dllmain.c -- already non-static/exported today, no gap. */
extern ultra_jvs_node* ultra_hook_node(void);
extern void            ultra_hook_pull_input(ultra_jvs_node* n);

/* jvs_serial_hook.c -- already non-static/exported today (every board's
 * install path already calls this to apply an IAT patch), no gap. */
extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* jvs_serial_hook.c -- KNOWN GAP #1 (blocks this file from LINKING, not just
 * from correctness): `open_fake_com` is declared
 *     static HANDLE open_fake_com(int port) { ... }
 * i.e. file-scope `static`, private to jvs_serial_hook.c. cave_direct_write_thread()
 * below (moved verbatim) needs to call it for its force-init path: the game's
 * own [0x774ab8]/[0x774ac4] COM3 handle fields get POKED with a synthetic
 * handle value, and that value MUST land in jvs_serial_hook.c's shared g_slots
 * table (open_fake_com's only job) so the game's OWN subsequent ReadFile /
 * WriteFile / SetCommMask / GetCommModemStatus / WaitCommEvent calls on that
 * handle are recognized by jvs_serial_hook.c's generic slot_for(h) checks --
 * this file cannot reimplement a parallel handle table without also losing
 * those other generic behaviors for the forced handle. FIX NEEDED: delete the
 * `static` keyword from jvs_serial_hook.c's `open_fake_com` definition (one
 * word). Until that lands, this translation unit will not link. Flagging this
 * explicitly per instructions, rather than silently duplicating the function
 * (which would fragment the handle table and break the other generic hooks
 * for the forced handle) or silently dropping the force-init path (which would
 * reopen the exact "DI8 device never appears under emulation" dead-input bug
 * this thread exists to route around). */
extern HANDLE open_fake_com(int port);

/* ============================================================================
 *  Diagnostic logging (mirrors jvs_serial_hook.c's ULTRA_JVS_LOG convention,
 *  duplicated locally rather than shared -- both jvs_diag() and jvs_diag_hex()
 *  are file-scope `static` there, and this ~15-line helper is cheap enough
 *  that a shared header for it isn't worth a new cross-file dependency).
 * ============================================================================
 */
static void jvs_diag(const char* fmt, ...) {
    static const char* path = NULL; static int checked = 0;
    if (!checked) { path = getenv("ULTRA_JVS_LOG"); checked = 1; }
    if (!path || !*path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void jvs_diag_hex(const char* tag, const unsigned char* b, size_t n) {
    if (!getenv("ULTRA_JVS_LOG")) return;
    char line[512]; size_t o = 0;
    o += (size_t)snprintf(line, sizeof(line), "%s [%zu]:", tag, n);
    for (size_t i = 0; i < n && i < 96 && o + 4 < sizeof(line); ++i)
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    jvs_diag("%s", line);
}

/* ============================================================================
 *  File-scope state -- every one of these was a jvs_serial_hook.c-private
 *  `static` before this extraction; they stay exactly that here, just no
 *  longer shared with (or visible to) any other board's code.
 * ============================================================================
 */
static HANDLE g_cvkey_fake = INVALID_HANDLE_VALUE; /* CVKEY.<NNN> bypass handle */
static HANDLE g_cvupd_fake = INVALID_HANDLE_VALUE; /* CVUPD\... diagnostic bypass handle */

/* HEADLESS TEST MODE (2026-08-02, same env var as d3d9_throttle.c's window-
 * visibility fix). Both the CK() key-read macros below fall back to a direct
 * GetAsyncKeyState() call -- real, global, focus-independent physical
 * keyboard state -- REGARDLESS of whether cave_kb_thread's LL-hook/raw-input
 * capture is running. Live-observed during a headless test run: ambient
 * keystrokes the user typed elsewhere landed in the diag log as real Cave
 * DSII button presses (C/X registered as HYPER/BOMB). An automated test must
 * never let the OUTSIDE world's real input reach the simulated game, so both
 * CK() sites gate on this flag too -- default OFF, byte-identical to today
 * for normal play. */
/* ULTRA_CAVE_QREPORT (2026-08-13): switches the COM3 push-mode response from the
 * legacy 9-byte 0x26 ASCII frame to the 4-byte 'P'/'Q'/'R'/'S'/'?' report
 * protocol the game's LIVE report parser at 0x45c580 reads, uses burst-then-
 * quiet so the parser sweep RETURNS (letting the worker-thread I/O loop advance
 * to input_poll), interleaves a '?' coinage-config report, and seeds the coin-
 * accept state. Together with ULTRA_CAVE_FORCE_IPOLL (removes input_poll's
 * 1000-tick counter gate) this is THE fix that makes coins register credits --
 * VERIFIED 2026-08-13: coin pulse -> credit counters [0x7654dc/e0/e4] increment
 * 0->1, credit SFX plays, input_poll runs (TX/RX 1300+ vs 2 before). DEFAULT
 * ON for Cave PC now (set ULTRA_CAVE_QREPORT=0 to force the old 0x26 path).
 * File-scope so dsii_on_read_file + cave_direct_write_thread share one flag. */
static int ultra_cave_qreport_active(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("ULTRA_CAVE_QREPORT"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

static int ultra_cave_test_headless_flag(void) {
    static int v = -1;
    if (v < 0) {
        /* Also checks ULTRA_CAVE_TEST_NOPAUSE -- see the cave_kb_thread
         * gate above for why: this real-input-suppression concern was
         * split off ULTRA_CAVE_TEST_HEADLESS because THAT flag's window-
         * positioning path self-pauses this game, but CK()'s GetAsyncKeyState
         * fallback must still be blocked under the replacement flag or an
         * automated test is right back to reading the user's real keyboard. */
        const char* h = getenv("ULTRA_CAVE_TEST_HEADLESS");
        const char* np = getenv("ULTRA_CAVE_TEST_NOPAUSE");
        v = ((h && h[0] == '1') || (np && np[0] == '1')) ? 1 : 0;
    }
    return v;
}

static volatile BYTE g_cave_kbd[256];
static HHOOK         g_cave_kb_hook;
/* Shared button state written by cave_direct_write_thread and read by
 * cave_io_respond to build the COM3 response.
 * LATCH: g_dsii_btn_latch/sys_latch accumulate bits written by the direct-write
 * thread; cave_io_respond ORs them in then clears them so a button press that
 * lasts < one ReadFile interval is still guaranteed to reach the game. */
static volatile uint8_t g_dsii_btn_shared = 0, g_dsii_sys_shared = 0;
static volatile uint8_t g_dsii_btn_latch  = 0, g_dsii_sys_latch  = 0;

static char g_cave_di8_path[MAX_PATH];

/* This file's own tiny single-slot response queue -- see the "RESPONSE
 * QUEUING" note in the file header for why this isn't jvs_serial_hook.c's
 * com_slot.resp[]. Cave frames are always exactly 9 bytes; 256 bytes is
 * generous headroom. */
static CRITICAL_SECTION s_resp_lock;
static int              s_resp_lock_init = 0;
static uint8_t          s_resp_buf[256];
static size_t           s_resp_len = 0, s_resp_pos = 0;
/* The handle jvs_serial_hook.c's generic dispatchers pass us that we've
 * recognized as "our" live Cave COM3 handle -- set the first time
 * dsii_on_write_file() is called (the game's own legitimate CreateFileA-then-
 * WriteFile handshake) OR by cave_direct_write_thread's force-init path
 * (open_fake_com(3) directly, when the game never opens COM3 on its own
 * because the DI8-device gate it waits for can never appear under emulation).
 * Cleared on close (see dsii_on_close_handle) for the same handle-recycling
 * reason g_cvkey_fake is cleared on close -- this cache is a NEW addition
 * this extraction introduces (the old code had no single-handle cache; it
 * relied on jvs_serial_hook.c's generic com_slot table + a global
 * cave_io_active() flag instead), so it gets the SAME documented fix. */
static HANDLE s_cave_com_handle = INVALID_HANDLE_VALUE;

typedef DWORD (WINAPI *PFN_GetIfTable)(void*, ULONG*, BOOL);
static PFN_GetIfTable Real_GetIfTable;

/* ── Cave CV2000XP board key file bypass (CVKEY.222) ──────────────────────
 * Cave PC v4.00 (Deathsmiles II) opens E:\CVKEY.222 then F:\CVKEY.222 every
 * attract cycle to authenticate the CV2000XP I/O board.  Without a valid
 * handle the game silently discards all COM3 input even though it keeps
 * reading the port.  We intercept the open, create a temp file, and track
 * the handle so dsii_on_read_file can log/satisfy any subsequent content read. */
/* GENERALIZED (2026-08-02, diagnostic-driven): the literal ".222" suffix was
 * only ever correct for the exe-identity/zero-NIC-entries path. Direct static
 * RE confirms the real filename is built from a printf-style format string
 * embedded in game.exe's own .rdata: "%sCVKEY.%03d" (found verbatim at file
 * offset 0x1d06d2) -- i.e. ANY 3-digit suffix is possible, computed at
 * runtime from board/NIC state we don't fully control (live-observed: 222
 * with our old zero-NIC-entries bypass, 203 with the synthetic single-NIC-
 * entry diagnostic). Matching only "222" meant any OTHER computed suffix
 * silently fell through to the generic (failing) path instead of getting our
 * synthetic key content -- match ANY "CVKEY." followed by exactly 3 digits,
 * case-insensitive, so whichever suffix the game computes this run still
 * gets a byte-identical synthetic response (same kRefKey content -- see
 * dsii_on_read_file's CVKEY branch, which already special-cases only on
 * "g_cvkey_fake handle identity", not on which digits were in the filename,
 * so no change needed there). */
static int is_cvkey_path(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; ++p) {
        if ((p[0]=='C'||p[0]=='c') &&
            (p[1]=='V'||p[1]=='v') &&
            (p[2]=='K'||p[2]=='k') &&
            (p[3]=='E'||p[3]=='e') &&
            (p[4]=='Y'||p[4]=='y') &&
            p[5]=='.' &&
            p[6]>='0' && p[6]<='9' &&
            p[7]>='0' && p[7]<='9' &&
            p[8]>='0' && p[8]<='9')
            return 1;
    }
    return 0;
}

/* NEW (2026-08-02, CVUPD/CVGAME lead): live-observed on the NIC-synth
 * diagnostic run -- once the MAC search actually executes (vs. the old
 * always-zero-entries skip), the game repeatedly opens
 * "E:\CVUPD\CVGAME\res\err.log" (5x in a row) before falling to the
 * CVKEY.<N> sequence. "CVUPD"/"CVGAME" are both real literal strings in
 * game.exe's .rdata (file offsets 0x2015c0/0x2015c6), so this is a genuine
 * code path, not an artifact. Every one of those opens showed match=0 (our
 * old CVKEY-only matcher correctly did NOT claim it) and no visible
 * "-> FAILED", so on this real dev machine the path either silently fails
 * via the generic fallback or something short-circuits before a normal
 * open. Diagnostic step: claim ANY path containing "CVUPD" the same way
 * CVKEY.222 is claimed (temp-file-backed, always "present"), so a
 * present-but-empty CVUPD tree can be compared against the current
 * present-but-absent behavior -- see dsii_on_create_file. */
static int is_cvupd_path(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; ++p) {
        if ((p[0]=='C'||p[0]=='c') && (p[1]=='V'||p[1]=='v') &&
            (p[2]=='U'||p[2]=='u') && (p[3]=='P'||p[3]=='p') &&
            (p[4]=='D'||p[4]=='d'))
            return 1;
    }
    return 0;
}

static int is_dinput8_name(const char* s) {
    if (!s) return 0;
    const char* p = strrchr(s, '\\');
    if (!p) p = strrchr(s, '/');
    if (p) p++; else p = s;
    return (_stricmp(p, "dinput8.dll") == 0);
}

/* ---- Focus-independent keyboard via WH_KEYBOARD_LL ----------------------- */
/* GetAsyncKeyState reads physical key state directly from the kernel - no
 * message-pump dependency.  Works from any thread in the foreground process.
 * The LL keyboard hook (g_cave_kbd) is kept as a secondary fallback but the
 * primary CK macro now uses GetAsyncKeyState so CPU-starved hook timeouts
 * (Windows drops LL hooks after ~300 ms of callback starvation) no longer
 * silence all input. */
static LRESULT CALLBACK cave_ll_kb_proc(int n, WPARAM wp, LPARAM lp) {
    static int s_ll_first = 0;
    if (!s_ll_first) { s_ll_first = 1; jvs_diag("Cave LL: FIRST CALL n=%d wp=0x%X", n, (unsigned)wp); }
    if (n >= 0) {
        const KBDLLHOOKSTRUCT* ks = (const KBDLLHOOKSTRUCT*)lp;
        USHORT vk = (USHORT)(ks->vkCode & 0xFF);
        int dn = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        if (vk > 0) {
            static BYTE s_state[256];
            int cave_key = (vk == 0x35 || vk == 0x31 || vk == 0x5A || vk == 0x58 ||
                            vk == 0x43 || vk == VK_UP || vk == VK_DOWN ||
                            vk == VK_LEFT || vk == VK_RIGHT);
            if (dn != (int)s_state[vk]) {
                g_cave_kbd[vk] = dn ? 1 : 0;
                s_state[vk] = (BYTE)dn;
                if (cave_key)
                    jvs_diag("Cave LL: vk=0x%02X %s", (unsigned)vk, dn?"dn":"up");
            }
        }
    }
    return CallNextHookEx(g_cave_kb_hook, n, wp, lp);
}
/* WM_INPUT handler: writes g_cave_kbd[] directly from raw keyboard events.
 * RIDEV_INPUTSINK fires even when game window is not in focus; no LL-hook
 * timeout exists for raw input, so the game's 1 MHz poll never blocks it. */
static LRESULT CALLBACK cave_ri_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
        if (sz > 0 && sz <= 256) {
            BYTE buf[256]; UINT got = sz;
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &got,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                RAWINPUT* ri = (RAWINPUT*)(void*)buf;
                if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                    USHORT vk = ri->data.keyboard.VKey;
                    USHORT fl = ri->data.keyboard.Flags;
                    if (vk > 0 && vk < 256) {
                        static int s_first_key;
                        if (!s_first_key && !(fl & RI_KEY_BREAK)) {
                            s_first_key = 1;
                            jvs_diag("Cave kbd: first key vk=0x%02X down", (unsigned)vk);
                        }
                        g_cave_kbd[vk] = (fl & RI_KEY_BREAK) ? 0 : 1;
                    }
                }
            }
        }
        return 0;
    }
    if (msg == WM_TIMER) {
        /* Re-register INPUTSINK periodically: the game may call RegisterRawInputDevices
         * when it gains focus (DirectInput init), overriding our registration.  Re-assert
         * every 500ms so INPUTSINK survives focus changes. */
        RAWINPUTDEVICE rid = {0};
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x06;
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid)); /* silent re-assert */
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static DWORD WINAPI cave_kb_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    /* Primary: RegisterRawInput INPUTSINK - no 300ms LL-hook timeout, works
     * focus-independent.  Fallback: WH_KEYBOARD_LL for edge cases. */
    int raw_ok = 0;
    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA(NULL);
    {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = cave_ri_wnd_proc; wc.hInstance = hInst;
        wc.lpszClassName = "UltraCaveKb";
        RegisterClassA(&wc); /* ERROR_CLASS_ALREADY_EXISTS is fine */
    }
    HWND hwnd = CreateWindowExA(0, "UltraCaveKb", NULL, 0,
                                0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (hwnd) {
        RAWINPUTDEVICE rid = {0};
        rid.usUsagePage = 0x01; /* HID_USAGE_PAGE_GENERIC */
        rid.usUsage     = 0x06; /* HID_USAGE_GENERIC_KEYBOARD */
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            jvs_diag("Cave kbd: raw input INPUTSINK active");
            raw_ok = 1;
            SetTimer(hwnd, 1, 500, NULL); /* re-register INPUTSINK every 500ms */
        } else {
            jvs_diag("Cave kbd: RegisterRawInput failed (%lu)", GetLastError());
            DestroyWindow(hwnd);
        }
    } else {
        jvs_diag("Cave kbd: CreateWindow failed (%lu)", GetLastError());
    }
    /* Always install LL hook alongside INPUTSINK: fullscreen games route physical
     * keystrokes through the game's own input path, not raw input delivery to our
     * background window.  The LL hook catches everything regardless of foreground. */
    g_cave_kb_hook = SetWindowsHookExA(WH_KEYBOARD_LL, cave_ll_kb_proc, NULL, 0);
    if (g_cave_kb_hook)
        jvs_diag("Cave kbd: LL hook installed");
    else if (!raw_ok) {
        jvs_diag("Cave kbd: all methods failed (%lu)", GetLastError());
        return 1;
    }
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}

/* ---- GetIfTable hook: bypass NIC-check retry loop ----------------------- */
/* Cave PC enumerates all NICs (29 on a typical Windows 10 install) looking
 * for a specific MAC address.  With many virtual adapters it retries with
 * exponential backoff for ~2 minutes before giving up.  Returning 0 entries
 * makes the game hit its "NIC::Not Entries" fast path and boot immediately. */

/* Manual MIB_IFROW layout (2026-08-02, verified against game.exe's own
 * disassembly, not just the SDK header -- the per-entry loop-advance
 * constant found at VA 0x472185 (`add $0x35c,%eax`) is EXACTLY sizeof(this
 * struct) = 0x35C = 860 bytes, and the bPhysAddr consumer at VA 0x45d4bc
 * reads offset pIfTable+0x218 = 4 (dwNumEntries) + 0x214 (row-relative
 * bPhysAddr offset), both matching this layout byte-for-byte). Defined
 * locally (not pulled from <iphlpapi.h>) to avoid a new SDK dependency for
 * one diagnostic struct; natural DWORD alignment throughout, no #pragma
 * pack needed -- every field before bPhysAddr is already a multiple of 4. */
typedef struct {
    WCHAR wszName[256];
    DWORD dwIndex, dwType, dwMtu, dwSpeed, dwPhysAddrLen;
    BYTE  bPhysAddr[8];
    DWORD dwAdminStatus, dwOperStatus, dwLastChange, dwInOctets, dwInUcastPkts,
          dwInNUcastPkts, dwInDiscards, dwInErrors, dwInUnknownProtos,
          dwOutOctets, dwOutUcastPkts, dwOutNUcastPkts, dwOutDiscards,
          dwOutErrors, dwOutQLen, dwDescrLen;
    BYTE  bDescr[256];
} ultra_mib_ifrow;

static DWORD WINAPI Hook_GetIfTable(void* pIfTable, ULONG* pdwSize, BOOL bOrder) {
    /* Real_GetIfTable is resolved in dsii_on_install(); the NULL guard is a
     * deliberate defensive addition beyond the original code (which assumed
     * it was always resolved by the time this could be called) -- cheap
     * insurance against a call ordering surprise, never fires in practice. */
    if (!Real_GetIfTable) return ERROR_NOT_SUPPORTED;
    DWORD r = Real_GetIfTable(pIfTable, pdwSize, bOrder);
    if (r == 0 /* NO_ERROR */ && pIfTable) {
        DWORD* p = (DWORD*)pIfTable; /* first field = dwNumEntries */
        DWORD real_entries = *p;

        /* DIAGNOSTIC EXPERIMENT (2026-08-02, owner-authorized single live run):
         * the kit's own manual lists a USB dongle accessory; the owner's hypothesis
         * is a USB-Ethernet adapter used as a hardware key, whose MAC is exactly
         * the "specific MAC address" the disassembly's comparison loop (VA
         * 0x472137-0x472182, confirmed via static RE this session) searches
         * GetIfTable's results for. We do NOT have the real dongle's MAC, so this
         * is not the fix -- it's a controlled probe: inject ONE plausible-looking
         * synthetic NIC entry (arbitrary MAC, since we don't have the real one) so
         * the search actually RUNS and reaches "searched, no match" instead of
         * short-circuiting at "zero entries, search skipped". The question being
         * tested is whether the game's behavior differs at all between those two
         * paths (new log lines, different attract-screen behavior, etc.) --
         * NOT whether this specific MAC is accepted (it can't be, it's fake).
         * ULTRA_CAVE_NIC_SYNTH=1 gates this; default OFF keeps the proven
         * always-zero behavior byte-identical for every other run. */
        static int synth_mode = -1;
        if (synth_mode < 0) {
            const char* e = getenv("ULTRA_CAVE_NIC_SYNTH");
            synth_mode = (e && e[0] == '1') ? 1 : 0;
        }
        if (synth_mode && pdwSize && *pdwSize >= (ULONG)(4 + sizeof(ultra_mib_ifrow))) {
            ultra_mib_ifrow* row = (ultra_mib_ifrow*)((BYTE*)pIfTable + 4);
            memset(row, 0, sizeof(*row));
            row->dwIndex        = 1;
            row->dwType         = 6;          /* IF_TYPE_ETHERNET_CSMACD */
            row->dwMtu          = 1500;
            row->dwSpeed        = 100000000;  /* 100 Mbps -- typical cheap USB dongle */
            row->dwPhysAddrLen  = 6;
            /* Arbitrary diagnostic MAC -- NOT the real dongle's address (unknown,
             * no hardware to read it from). Locally-administered bit set (0x02
             * first octet) so it reads as a plausible real adapter, not garbage. */
            row->bPhysAddr[0] = 0x02; row->bPhysAddr[1] = 0xCA; row->bPhysAddr[2] = 0xFE;
            row->bPhysAddr[3] = 0x00; row->bPhysAddr[4] = 0x00; row->bPhysAddr[5] = 0x01;
            row->dwAdminStatus  = 1;          /* MIB_IF_ADMIN_STATUS_UP */
            row->dwOperStatus   = 5;          /* IF_OPER_STATUS_OPERATIONAL */
            strncpy((char*)row->bDescr, "USB Ethernet Adapter", sizeof(row->bDescr) - 1);
            row->dwDescrLen = (DWORD)strlen((char*)row->bDescr);
            *p = 1;
            *pdwSize = (ULONG)(4 + sizeof(ultra_mib_ifrow));
            jvs_diag("GetIfTable: SYNTH MODE -- injected 1 fake NIC (MAC=02:CA:FE:00:00:01, "
                     "arbitrary/diagnostic only) instead of the usual 0-entries bypass "
                     "(real system entries would have been %lu)", (unsigned long)real_entries);
        } else {
            jvs_diag("GetIfTable: cleared %lu NIC entries (NIC-check bypass)", (unsigned long)real_entries);
            *p = 0;
        }
    }
    return r;
}

/* KNOWN GAP #2 (correctness, not a link error -- see game_dsii_hook_getproctable
 * below for the fix this pairs with): dsii_on_install() below IAT-patches every
 * STATIC import of iphlpapi!GetIfTable to Hook_GetIfTable, which covers most
 * callers. But jvs_serial_hook.c's own Hook_GetProcAddress has a hardcoded
 *     {"GetIfTable",(FARPROC)Hook_GetIfTable},
 * entry (its OWN static Hook_GetIfTable, referencing its OWN static
 * Real_GetIfTable) that unconditionally answers ANY GetProcAddress(iphlpapi,
 * "GetIfTable") call in ANY process -- including this one, once this file's
 * ops is registered. jvs_serial_hook.c only resolves ITS Real_GetIfTable
 * inside the legacy `if (cave_io_active() && !ultra_board_ops_get())` block,
 * which is now SKIPPED for DSII (ultra_board_ops_get() is non-NULL once we
 * register) -- so jvs_serial_hook.c's Real_GetIfTable stays NULL for this
 * process, and its Hook_GetIfTable would call through a NULL function
 * pointer and crash. This is exactly why Cave PC (per the existing RE
 * comment on jvs_serial_hook.c's Hook_GetProcAddress map) resolves GetIfTable
 * dynamically via GetProcAddress rather than only through a static import --
 * meaning this is a REAL, not theoretical, crash risk for Deathsmiles II
 * specifically once this file is linked in. FIX NEEDED in jvs_serial_hook.c's
 * Hook_GetProcAddress, before the hardcoded map lookup (or replacing the
 * map's "GetIfTable" line):
 *     { const board_io_ops* ops = ultra_board_ops_get();
 *       void* fn;
 *       if (ops && game_dsii_hook_getproctable(name, &fn)) return (FARPROC)fn; }
 * This function is the accessor that fix needs; it is exported (not static)
 * specifically so jvs_serial_hook.c can call it once that one-time edit lands. */
int game_dsii_hook_getproctable(const char* name, void** out_fn) {
    if (ultra_board_ops_get() == NULL) return 0;   /* not our process */
    if (!name || strcmp(name, "GetIfTable") != 0) return 0;
    if (out_fn) *out_fn = (void*)Hook_GetIfTable;
    return 1;
}

/* Cave PC I/O: proprietary serial protocol on COM3.
 * Commands are 4-byte frames. First byte = command, rest = 0x2A filler.
 * We respond with 4 bytes: echo + input state from our controls engine.
 *
 * Observed commands:
 *   0x40  - status/init poll  → respond 40 00 00 00 (OK)
 *   0x26  - input poll        → respond 26 <p1_btns> <system> 00
 *
 * Player button byte (active-LOW: 0 = pressed):
 *   bit0=UP  bit1=DOWN  bit2=LEFT  bit3=RIGHT
 *   bit4=BTN1(shot)  bit5=BTN2(bomb)  bit6=BTN3(EX)  bit7=START
 *
 * System byte (active-LOW: 0 = pressed):
 *   bit0=COIN  bit1=SERVICE  bit2=TEST
 *
 * The JVS node input uses ACTIVE-HIGH convention; Cave wants ACTIVE-LOW.
 * We invert after mapping. */
static size_t cave_io_respond(ultra_jvs_node* node, const uint8_t* tx, size_t tx_len,
                              uint8_t* rx, size_t rx_cap) {
    if (tx_len < 1 || rx_cap < 9) return 0;
    uint8_t cmd = tx[0];
    ultra_jvs_input in;
    jvs_get_input(node, &in);

    rx[0] = cmd;
    if (cmd == 0x26) {
        /* Map JVS player[0] to Cave button byte.
         * JVS high byte: bit7=START bit6=SVC bit5=UP bit4=DOWN bit3=LEFT bit2=RIGHT bit1=BTN1 bit0=BTN2
         * JVS low byte:  bit7=BTN3 ... */
        uint16_t p = in.player[0];
        uint8_t hi = (uint8_t)(p >> 8);
        uint8_t lo = (uint8_t)(p & 0xFF);
        /* Cave COM3 byte 0 layout (confirmed from ds2_monitor / real EX-BOARD HW):
         *   bit7=START bit6=TEST bit5=UP bit4=DOWN bit3=LEFT bit2=RIGHT bit1=B1(SHOT) bit0=B2(BOMB)
         * Cave COM3 byte 1 layout:
         *   bit7=B3(HYPER) bit0=P1_COIN
         * JVS hi byte matches Cave byte 0 exactly, so direct copy. */
        uint8_t cave_btn = 0;
        if (hi & 0x80) cave_btn |= 0x80; /* START */
        if (hi & 0x40) cave_btn |= 0x40; /* SERVICE/TEST */
        if (hi & 0x20) cave_btn |= 0x20; /* UP */
        if (hi & 0x10) cave_btn |= 0x10; /* DOWN */
        if (hi & 0x08) cave_btn |= 0x08; /* LEFT */
        if (hi & 0x04) cave_btn |= 0x04; /* RIGHT */
        if (hi & 0x02) cave_btn |= 0x02; /* B1/SHOT */
        if (hi & 0x01) cave_btn |= 0x01; /* B2/BOMB */

        uint8_t cave_sys = 0;
        if (lo & 0x80) cave_sys |= 0x80; /* B3/HYPER */

        /* Direct keyboard: prefer g_cave_kbd[] (populated by RIDEV_INPUTSINK raw-input
         * thread - fires even when game window is NOT in focus) then fall back to
         * GetAsyncKeyState (works when game IS the foreground process). */
        #define CK(vk) (!ultra_cave_test_headless_flag() && \
                         (g_cave_kbd[(unsigned)(vk)&0xFF] || ((GetAsyncKeyState((int)(vk)) & 0x8000) != 0)))
        if (CK(VK_UP))    cave_btn |= 0x20; /* UP */
        if (CK(VK_DOWN))  cave_btn |= 0x10; /* DOWN */
        if (CK(VK_LEFT))  cave_btn |= 0x08; /* LEFT */
        if (CK(VK_RIGHT)) cave_btn |= 0x04; /* RIGHT */
        if (CK('Z'))      cave_btn |= 0x02; /* B1/SHOT */
        if (CK('X'))      cave_btn |= 0x01; /* B2/BOMB */
        if (CK('C'))      cave_sys |= 0x80; /* B3/HYPER */
        if (CK('1'))      cave_btn |= 0x80; /* START */
        if (CK('5'))      cave_sys |= 0x01; /* P1 COIN */
        if (CK(VK_F2))    cave_btn |= 0x40; /* TEST */
        #undef CK
        /* Merge current state AND latch from direct-write thread.
         * The latch accumulates any button that fired since the last ReadFile
         * so a press shorter than one ReadFile interval is never lost.
         * Clear the latch after consuming it. */
        cave_btn |= g_dsii_btn_shared;
        cave_sys |= g_dsii_sys_shared;
        cave_btn |= g_dsii_btn_latch;  g_dsii_btn_latch = 0;
        cave_sys |= g_dsii_sys_latch;  g_dsii_sys_latch = 0;

        /* XInput gamepad: D-pad/left-stick=move A=shot B=bomb X=EX START=start BACK/LB=coin */
        {
            typedef DWORD (WINAPI *pfxi)(DWORD, void*);
            static pfxi s_xi; static int s_xi_init;
            if (!s_xi_init) {
                s_xi_init = 1;
                HMODULE h = LoadLibraryA("xinput1_4.dll");
                if (!h) h = LoadLibraryA("xinput1_3.dll");
                if (h) s_xi = (pfxi)(void*)GetProcAddress(h, "XInputGetState");
            }
            /* HEADLESS TEST MODE (2026-08-02, contamination fix): unlike the CK()
             * keyboard macro above (already gated on ultra_cave_test_headless_flag()),
             * this XInput read was unconditional -- it reads the REAL, physically
             * connected pad's live state regardless of headless mode. Live-observed:
             * a headless run with no deliberate input showed the response byte jump
             * (0x30->0xB0, the START bit) mid-run purely from ambient real controller
             * state, contaminating an otherwise-clean automated test. Same fix as
             * CK(): skip the real read entirely when headless testing is active. */
            if (!ultra_cave_test_headless_flag() && s_xi) {
                struct { DWORD pn; struct { WORD w; BYTE lt,rt; SHORT lx,ly,rsx,rsy; } gp; } xs = {0};
                if (s_xi(0, &xs) == 0) {
                    WORD xb = xs.gp.w; SHORT xlx = xs.gp.lx, xly = xs.gp.ly;
                    #define XI(f) (!!(xb & (f)))
                    if (XI(0x0001) || xly > 8000)  cave_btn |= 0x20; /* DU/LY+ → UP   */
                    if (XI(0x0002) || xly <-8000)  cave_btn |= 0x10; /* DD/LY- → DOWN  */
                    if (XI(0x0004) || xlx <-8000)  cave_btn |= 0x08; /* DL/LX- → LEFT  */
                    if (XI(0x0008) || xlx > 8000)  cave_btn |= 0x04; /* DR/LX+ → RIGHT */
                    if (XI(0x1000))                cave_btn |= 0x02; /* A → B1/SHOT    */
                    if (XI(0x2000))                cave_btn |= 0x01; /* B → B2/BOMB    */
                    if (XI(0x4000))                cave_sys |= 0x80; /* X → B3/HYPER   */
                    if (XI(0x0010))                cave_btn |= 0x80; /* START           */
                    if (XI(0x0020) || XI(0x0100))  cave_sys |= 0x01; /* BACK/LB → COIN */
                    #undef XI
                }
            }
        }

        /* Cave PC serial protocol: each data byte is ASCII-encoded.
         * Game subtracts 0x30 from each byte (SUB DL,0x30 at 0x45c497).
         * byte1 → [0x774b54] = P1 buttons, byte2 → [0x774b55] = system/coin,
         * bytes 3-8 → [0x774b56..0x774b5b] = idle channels. */
        rx[1] = 0x30 + cave_btn;
        rx[2] = 0x30 + cave_sys;
        rx[3] = 0x30; rx[4] = 0x30; rx[5] = 0x30;
        rx[6] = 0x30; rx[7] = 0x30; rx[8] = 0x30;
    } else if (cmd == 0x40) {
        /* Cave PC status/init poll. Real hardware returns a non-zero byte in
         * position 1 (firmware version / board-present flag). Game checks this
         * before setting its COM3 state variable; all-zero means "no board."
         * 0x31 - 0x30 = 0x01 (version 1) after the game's SUB 0x30 decode. */
        rx[1] = 0x31;
        rx[2] = 0x30; rx[3] = 0x30; rx[4] = 0x30;
        rx[5] = 0x30; rx[6] = 0x30; rx[7] = 0x30; rx[8] = 0x30;
    } else {
        rx[1] = 0x30; rx[2] = 0x30; rx[3] = 0x30;
        rx[4] = 0x30; rx[5] = 0x30; rx[6] = 0x30;
        rx[7] = 0x30; rx[8] = 0x30;
    }
    return 9;
}

/* -------------------------------------------------------------------------- */
/* Cave PC Deathsmiles II: input fix.
 *
 * Binary analysis (game.exe, 3,616,768 bytes, image base 0x400000):
 *   input_poll at 0x45c370 reads COM3 and decodes 8 button bytes into the
 *   global array [0x774b54..0x774b5b].  A guard at [ESI+4] is SET at the end
 *   of the first successful COM3 read (0x45c51d: MOV [ESI+4],1).  On every
 *   subsequent call the guard check fires at 0x45c37d:
 *       CMP DWORD [ESI+4], 0   (0x45c379)
 *       JNE 0x45c510           (0x45c37d, 6 bytes: 0f 85 8d 01 00 00)
 *   …and the function returns immediately without touching [0x774b54..5b].
 *
 *   PRIMARY FIX (cave_io_respond, 0x40 branch): return rx[1]=0x31 so the
 *   game decodes firmware version=1 ("board present") and sets [0x774ab8]
 *   itself during COM3 init.  [0x774ab8] is the gate that enables push-mode
 *   CaveRD and input_poll.  With all-zeros response the game saw version=0
 *   and left [0x774ab8]=0 → controls dead + slow loading loop.
 *
 *   SECONDARY FIX (dsii_nop_input_poll_guard): NOP the 6-byte JNE so
 *   input_poll always falls through, reads COM3, and refreshes [0x774b54]
 *   every frame.  cave_io_respond builds the correct COM3 response from
 *   GetAsyncKeyState + XInput on every call.
 *
 *   cave_direct_write_thread: belt-and-suspenders at ~120 Hz - writes
 *   GetAsyncKeyState + XInput directly to [0x774b54..55] so buttons land
 *   even if the COM3 push-mode path is delayed. */

/* ── Resource-manager hash-table build diagnostic (2026-08-02) ─────────────
 * Read-only watcher: polls the singleton resource-manager global at VA
 * 0x775730 (image base 0x400000, so base+0x375730) and, once constructed,
 * logs its hash-table fields (+0x30 bucket-table ptr, +0x34 bucket count,
 * +0x38/+0x3c paired counters, +0x40 outer count) whenever they CHANGE.
 * Answers a concrete open question from static RE: does anything ever
 * populate +0x34 (bucket count) before the divide-by-zero crash at VA
 * 0x483801 (mgr->Find(key), reached via sub_476f20 -> singleton getter
 * 0x475fa0 -> 0x4837e0), or does the crash always hit a table that was
 * constructed (deliberately zeroed -- confirmed via static RE of the ctor
 * at 0x475ef0, which explicitly does `mov eax,[esi+0x30]` / `mov eax,[esi+
 * 0x34]` with eax=0) and never subsequently built? Pure read -- never
 * writes game memory, cannot change game behavior, only observes.
 * Env-gated, OFF by default:
 *   ULTRA_CAVE_DIAG_HASHTABLE=1   enable this watcher thread */
static int ultra_cave_diag_hashtable_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("ULTRA_CAVE_DIAG_HASHTABLE");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v;
}

static DWORD WINAPI cave_diag_hashtable_thread(LPVOID arg) {
    (void)arg;
    Sleep(200);
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    volatile uint32_t* p_singleton = (volatile uint32_t*)(base + 0x375730u);
    jvs_diag("HashTableDiag: watching singleton global @ %p (base=%p)", (void*)p_singleton, (void*)base);

    uint32_t last_obj = 0;
    uint32_t last_f30 = 0xFFFFFFFFu, last_f34 = 0xFFFFFFFFu, last_f38 = 0xFFFFFFFFu,
             last_f3c = 0xFFFFFFFFu, last_f40 = 0xFFFFFFFFu;
    int seen_nonzero_bucketcount = 0;

    for (int i = 0; i < 1200; i++) { /* ~1200*50ms = 60s -- matches longest crash-free runs seen so far */
        uint32_t obj = *p_singleton;
        if (obj != last_obj) {
            jvs_diag("HashTableDiag: singleton ptr 0x%08x -> 0x%08x (tick=%u)",
                     (unsigned)last_obj, (unsigned)obj, (unsigned)GetTickCount());
            last_obj = obj;
        }
        if (obj != 0) {
            volatile uint32_t* f = (volatile uint32_t*)(uintptr_t)obj;
            uint32_t f30 = f[0x30/4], f34 = f[0x34/4], f38 = f[0x38/4], f3c = f[0x3c/4], f40 = f[0x40/4];
            if (f30 != last_f30 || f34 != last_f34 || f38 != last_f38 || f3c != last_f3c || f40 != last_f40) {
                jvs_diag("HashTableDiag: obj=0x%08x +30=0x%08x +34=%u +38=0x%08x +3c=0x%08x +40=%u (tick=%u)%s",
                         (unsigned)obj, (unsigned)f30, (unsigned)f34, (unsigned)f38, (unsigned)f3c, (unsigned)f40,
                         (unsigned)GetTickCount(), f34 ? " <-- BUCKET COUNT NONZERO, TABLE BUILT" : "");
                last_f30 = f30; last_f34 = f34; last_f38 = f38; last_f3c = f3c; last_f40 = f40;
                if (f34 != 0) seen_nonzero_bucketcount = 1;
            }
        }
        Sleep(50);
    }
    jvs_diag("HashTableDiag: watcher exiting after ~60s (bucket count ever went nonzero: %s)",
             seen_nonzero_bucketcount ? "YES" : "NO -- table was NEVER built");
    return 0;
}

/* ── Broad memory diff across the coin pulse (2026-08-02) ──────────────────
 * User's own proposed diagnostic: rather than keep guessing which specific
 * address is "the" credit counter, snapshot a wide span of the game's own
 * writable memory right before the TEST COIN pulse fires and again ~2s
 * after, then log every byte that actually changed. [0x774ab8]/[0x774ac4]
 * (the addresses watched so far) are a board-handle/gate flag, not
 * necessarily where a credit count lives -- this finds out empirically
 * instead of assuming. Walks committed, non-guarded, writable pages via
 * VirtualQuery from the module base through a generous +8MB span (every VA
 * referenced anywhere in this file, e.g. 0x6260d4/0x774xxx/0x775730, sits
 * well inside that range). Pure read -- never writes game memory. Env-gated:
 * ULTRA_CAVE_TEST_COIN must be armed (rides on the same one-shot pulse)
 * plus ULTRA_CAVE_MEMDIFF=1. */
#define ULTRA_MEMDIFF_SPAN 0x400000u

static int ultra_cave_memdiff_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("ULTRA_CAVE_MEMDIFF");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v;
}

static uint8_t* ultra_memdiff_snapshot(uintptr_t base) {
    uint8_t* snap = (uint8_t*)calloc(1, ULTRA_MEMDIFF_SPAN);
    if (!snap) return NULL;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = base, end = base + ULTRA_MEMDIFF_SPAN;
    int guard = 0; /* safety cap: a normal process has nowhere near this many
                     * distinct regions in a 4MB span -- if we ever hit this,
                     * something is wrong (or memory is pathologically
                     * fragmented) and we bail rather than run indefinitely. */
    while (addr < end && guard++ < 100000) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            uintptr_t copyStart = addr;
            uintptr_t copyEnd = regionEnd < end ? regionEnd : end;
            if (copyEnd > copyStart) memcpy(snap + (copyStart - base), (void*)copyStart, copyEnd - copyStart);
        }
        if (regionEnd <= addr) break; /* safety against zero-size/non-advancing regions */
        addr = regionEnd;
    }
    return snap;
}

static void ultra_memdiff_compare_and_log(uintptr_t base, const uint8_t* before, const uint8_t* after) {
    int changes = 0;
    for (size_t i = 0; i < ULTRA_MEMDIFF_SPAN; i++) {
        if (before[i] != after[i]) {
            changes++;
            if (changes <= 200)
                jvs_diag("MEMDIFF: [0x%08x] %02x -> %02x", (unsigned)(base + i), before[i], after[i]);
        }
    }
    jvs_diag("MEMDIFF: scan done, %d byte(s) changed total%s", changes,
             changes > 200 ? " (first 200 logged)" : "");
}

static void dsii_nop_input_poll_guard(void) {
    /* JNE at VA 0x45c37d (RVA 0x5c37d): 0f 85 8d 01 00 00 → 6×NOP */
    uint8_t* p = (uint8_t*)(uintptr_t)GetModuleHandleA(NULL) + 0x5c37du;
    if (p[0] != 0x0f || p[1] != 0x85) {
        jvs_diag("Cave DSII guard: unexpected bytes at +0x5c37d: %02x %02x (skip NOP)",
                 (unsigned)p[0], (unsigned)p[1]);
        return;
    }
    DWORD old;
    VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
    memset(p, 0x90, 6);
    VirtualProtect(p, 6, old, &old);
    jvs_diag("Cave DSII: input_poll guard NOP'd @ %p, COM3 read every frame", (void*)p);
}

/* Force input_poll (the coin/credit reader) to run every I/O-loop iteration.
 * (2026-08-13, gated on ULTRA_CAVE_FORCE_IPOLL) Root cause found via dds stack
 * recovery + live tracing: the real I/O driver is the worker-thread loop at
 * VA 0x45ba30 (reached through C++ vtable dispatch, entry thread 0x4910fc).
 * That loop calls the parser EVERY iteration but the input_poll-caller
 * (0x45cbb0) ONLY when a countdown [esi+0x13f0] from 0x3e8=1000 reaches 0
 * (0x45ba67 decrements, 0x45ba75 `jg 0x45baa6` skips input_poll while >0).
 * Live-confirmed the input_poll branch is never taken in any practical
 * window, so input_poll -- which does the FULL coin handshake through OUR
 * hooks (writes '@'/'&' via the WriteFile dispatcher, reads via ReadFile,
 * decodes into [0x774b54..], updates the credit state) and would therefore
 * work if it ran -- never processes our device's coins. NOP the 2-byte jg so
 * the counter-gate is bypassed and input_poll runs every iteration. If it
 * runs, its '@'/'&' command writes show up in the WriteFile diag (count jumps
 * from ~2 to many). Off by default. */
static void dsii_force_input_poll(void) {
    /* DEFAULT ON for Cave PC (2026-08-13): this + QREPORT's burst-then-quiet is
     * the verified coin->credit fix. Set ULTRA_CAVE_FORCE_IPOLL=0 to disable. */
    const char* e = getenv("ULTRA_CAVE_FORCE_IPOLL");
    if (e && e[0] == '0') return;
    /* JG at VA 0x45ba75 (RVA 0x5ba75): 7f 2f → 2×NOP */
    uint8_t* p = (uint8_t*)(uintptr_t)GetModuleHandleA(NULL) + 0x5ba75u;
    if (p[0] != 0x7f || p[1] != 0x2f) {
        jvs_diag("Cave DSII force-ipoll: unexpected bytes at +0x5ba75: %02x %02x (skip)",
                 (unsigned)p[0], (unsigned)p[1]);
        return;
    }
    DWORD old;
    VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0x90; p[1] = 0x90;
    VirtualProtect(p, 2, old, &old);
    jvs_diag("Cave DSII: input_poll counter-gate NOP'd @ %p, input_poll runs every I/O iteration", (void*)p);
}

/* ── Resource-manager hash-table Find() crash guard (2026-08-13) ──────────
 * LIVE-CAUGHT via cdb (first live-debugger session against this game -- see
 * ultra-loader-debugger-installed memory): the resource manager's Find() at
 * VA 0x4837e0 unconditionally does
 *     div DWORD PTR [ecx+0x34]     ; VA 0x483801, [ecx+0x34]=bucket count
 *     ...
 *     add edx,DWORD PTR [ecx+0x30] ; [ecx+0x30]=bucket-table pointer
 *     mov eax,DWORD PTR [edx]      ; VA 0x48380d
 * where [ecx+0x34] is the hash table's bucket count and [ecx+0x30] its
 * bucket-table pointer. First live crash dump (this hook's own vectored
 * exception handler, see hook_dllmain.c) showed +0x34==0 (STATUS_
 * INTEGER_DIVIDE_BY_ZERO at 0x483801). A v1 guard patched just the DIV --
 * that run's own crash dump then showed +0x30==0 TOO (a different call,
 * different object instance), so with the divide-by-zero avoided (edx
 * forced to 0) the very next dereference at 0x48380d faulted instead
 * (STATUS_ACCESS_VIOLATION, mov eax,[edx] with edx=0+0=NULL). So this
 * isn't "one field sometimes lags" -- the whole hashmgr object is either
 * fully built or plainly not-yet-touched (both fields still zero) when
 * this particular Find() call races it; a correct guard has to treat
 * +0x30==0 exactly like +0x34==0, not just the divide. Whether the race is
 * against a background resource-loading thread or something specific to
 * the ~97MB res\_gdata9e2.dat load this crash immediately follows in the
 * log wasn't pinned down -- but a Find() on an empty/not-yet-built table
 * should always safely mean "not found", never a hardware fault, whatever
 * the root cause. 100% reproducible under Ultra Loader this session (2/2
 * fresh launches before any guard existed), where older sessions
 * (STOCK_TAKE.md, 2026-07-21..23) recorded many full attract-loop runs
 * with no crash at all -- something shifted the timing enough to
 * consistently lose whatever race leaves these fields unset, but the
 * guard below is correct either way.
 *
 * Same VirtualProtect-and-patch technique as dsii_nop_input_poll_guard()
 * just above, but the 3-byte DIV can't become plain NOPs -- the code
 * after it consumes EDX (the remainder) unconditionally, all the way
 * through a pointer dereference and two more comparisons:
 *     483801: f7 71 34             div    DWORD PTR [ecx+0x34]
 *     483804: 69 d2 1c 01 00 00    imul   edx,edx,0x11c
 *     48380a: 03 51 30             add    edx,DWORD PTR [ecx+0x30]
 *     48380d: 8b 02                mov    eax,DWORD PTR [edx]
 *     48380f: 85 c0                test   eax,eax
 *     483811: 75 05                jne    0x483818
 *     483813: 39 6a 10             cmp    DWORD PTR [edx+0x10],ebp
 *     483816: 74 4b                je     0x483863          (found)
 *     483818: 83 f8 01             cmp    eax,0x1
 *     48381b: 72 05                jb     0x483822          (bucket empty)
 *     48381d: 39 6a 10             cmp    DWORD PTR [edx+0x10],ebp
 *     483820: 74 41                je     0x483863          (found)
 *             (falls through)  -> 0x483822                  (not found)
 * So: redirect via JMP rel32 (5 bytes, covering the DIV plus the first 2
 * bytes of the IMUL) to an RWX trampoline that:
 *   1. Checks BOTH [ecx+0x34] and [ecx+0x30] up front; if either is zero,
 *      jump straight to VA 0x483822 ("not found in this bucket, outer loop
 *      keeps going") without touching either field again.
 *   2. Otherwise replicates the entire block above verbatim (byte-
 *      identical semantics to the original on every call where the table
 *      really is built -- which is most of them), re-encoding its two
 *      short (rel8) conditional jumps as near (rel32) so they still reach
 *      0x483822/0x483863 regardless of where VirtualAlloc happens to put
 *      this trampoline in the address space. */
static void dsii_guard_hash_divide(void) {
    uint8_t* base = (uint8_t*)(uintptr_t)GetModuleHandleA(NULL);
    uint8_t* p = base + 0x83801u;
    static const uint8_t expect[5] = {0xf7,0x71,0x34, 0x69,0xd2};
    if (memcmp(p, expect, sizeof(expect)) != 0) {
        jvs_diag("Cave DSII hash-div guard: unexpected bytes @+0x83801 (skip patch)");
        return;
    }
    uint8_t* va_483822 = base + 0x83822u;
    uint8_t* va_483863 = base + 0x83863u;

    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        jvs_diag("Cave DSII hash-div guard: VirtualAlloc failed (%lu)", GetLastError());
        return;
    }

    uint8_t buf[128];
    size_t o = 0;
#define EMIT1(b0) (buf[o++] = (uint8_t)(b0))
#define EMIT_REL32_SLOT() (o += 4)

    /* IMPORTANT (2026-08-13 fix): these two MUST land on L_483863 (the
     * function's own fast "return 0, not found" epilogue), NOT L_483822
     * (the outer-loop continue). Live-caught bug in the first cut of this
     * guard: L_483822 re-enters the SAME outer loop whose bound (ebx, read
     * from [ecx+0x40]) is uninitialized garbage on an unbuilt table --
     * confirmed live at ~0x4E414B4C (~1.3 BILLION). Redirecting there
     * doesn't hang, but it doesn't return quickly either: a live cdb trace
     * showed this exact call site getting re-entered roughly every 5
     * seconds, consistent with actually iterating a large fraction of that
     * bound every single call before falling out. That's why the game
     * seemed to make zero forward progress even after minutes -- every
     * lookup against the not-yet-built table was burning several real
     * seconds instead of returning immediately. */
    EMIT1(0x83); EMIT1(0x79); EMIT1(0x34); EMIT1(0x00);            /* cmp DWORD PTR [ecx+0x34],0 */
    size_t j1 = o; EMIT1(0x0f); EMIT1(0x84); EMIT_REL32_SLOT();     /* je L_483863 */
    EMIT1(0x83); EMIT1(0x79); EMIT1(0x30); EMIT1(0x00);            /* cmp DWORD PTR [ecx+0x30],0 */
    size_t j2 = o; EMIT1(0x0f); EMIT1(0x84); EMIT_REL32_SLOT();     /* je L_483863 */

    EMIT1(0xf7); EMIT1(0x71); EMIT1(0x34);                          /* div DWORD PTR [ecx+0x34] */
    EMIT1(0x69); EMIT1(0xd2); EMIT1(0x1c); EMIT1(0x01); EMIT1(0x00); EMIT1(0x00); /* imul edx,edx,0x11c */
    EMIT1(0x03); EMIT1(0x51); EMIT1(0x30);                          /* add edx,[ecx+0x30] */
    EMIT1(0x8b); EMIT1(0x02);                                        /* mov eax,[edx] */
    EMIT1(0x85); EMIT1(0xc0);                                        /* test eax,eax */
    size_t j3 = o; EMIT1(0x0f); EMIT1(0x85); EMIT_REL32_SLOT();     /* jne L_chk1 */
    EMIT1(0x39); EMIT1(0x6a); EMIT1(0x10);                          /* cmp [edx+0x10],ebp */
    size_t j4 = o; EMIT1(0x0f); EMIT1(0x84); EMIT_REL32_SLOT();     /* je L_483863 */

    size_t L_chk1 = o;
    EMIT1(0x83); EMIT1(0xf8); EMIT1(0x01);                          /* cmp eax,1 */
    size_t j5 = o; EMIT1(0x0f); EMIT1(0x82); EMIT_REL32_SLOT();     /* jb L_483822 */
    EMIT1(0x39); EMIT1(0x6a); EMIT1(0x10);                          /* cmp [edx+0x10],ebp */
    size_t j6 = o; EMIT1(0x0f); EMIT1(0x84); EMIT_REL32_SLOT();     /* je L_483863 */
    size_t j7 = o; EMIT1(0xe9); EMIT_REL32_SLOT();                   /* jmp L_483822 (not found) */

    size_t L_483822 = o;
    size_t j8 = o; EMIT1(0xe9); EMIT_REL32_SLOT();                   /* jmp VA 0x483822 */

    size_t L_483863 = o;
    size_t j9 = o; EMIT1(0xe9); EMIT_REL32_SLOT();                   /* jmp VA 0x483863 */

#undef EMIT1
#undef EMIT_REL32_SLOT

#define PATCH_REL32(field_off, target) do { \
        int32_t _rel = (int32_t)((uintptr_t)(target) - (uintptr_t)(tramp + (field_off) + 4)); \
        memcpy(buf + (field_off), &_rel, 4); \
    } while (0)
    PATCH_REL32(j1 + 2, tramp + L_483863);
    PATCH_REL32(j2 + 2, tramp + L_483863);
    PATCH_REL32(j3 + 2, tramp + L_chk1);
    PATCH_REL32(j4 + 2, tramp + L_483863);
    PATCH_REL32(j5 + 2, tramp + L_483822);
    PATCH_REL32(j6 + 2, tramp + L_483863);
    PATCH_REL32(j7 + 1, tramp + L_483822);
    PATCH_REL32(j8 + 1, va_483822);
    PATCH_REL32(j9 + 1, va_483863);
#undef PATCH_REL32

    memcpy(tramp, buf, o);

    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xe9;
    int32_t rel_to_tramp = (int32_t)((uintptr_t)tramp - (uintptr_t)(p + 5));
    memcpy(p + 1, &rel_to_tramp, 4);
    VirtualProtect(p, 5, old, &old);

    jvs_diag("Cave DSII: hash-table Find() divide-by-zero guard installed @ %p -> trampoline %p",
             (void*)p, (void*)tramp);
}

/* ── Hash-table self-construction fallback (2026-08-13, opt-in) ────────────
 * The guard above stops the crash and the outer-loop spin, but the
 * underlying resource-manager singleton at VA 0x775730 never gets its
 * bucket table built at all -- confirmed via ULTRA_CAVE_DIAG_HASHTABLE
 * across multiple 60s+ windows (object allocated once at tick~0, then
 * frozen forever), and via a 25-thread live census showing no thread
 * visibly building it (main thread renders normally every frame; three
 * worker threads busy-poll Sleep() inside unmarked/FPO-optimized code that
 * resisted static tracing -- not confirmed to even be related to this
 * table). Whatever normally populates it -- most likely gated on finishing
 * the ~97MB res\_gdata9e2.dat load in a way our environment's timing never
 * satisfies -- simply never runs, and CVKEY.222 (and everything after it,
 * very plausibly including the credit gate) is never reached as a result.
 *
 * Rather than keep chasing why (a materially bigger, less certain
 * undertaking than every other fix in this file -- see STOCK_TAKE.md),
 * construct a VALID, EMPTY table ourselves once the object exists, so
 * every downstream Find() takes the SAME code path a real "searched a
 * real but empty table, found nothing" would -- not a special case only
 * this file's guard understands. Layout (all fields confirmed via live
 * crash dumps + disassembly of Find(), VA 0x4837e0):
 *   +0x30 bucket-table pointer   -> our own zeroed, real, heap-backed array
 *   +0x34 bucket count (divisor) -> array length (small, e.g. 16)
 *   +0x40 outer-loop bound       -> same length, so the outer loop is real
 *                                   and small instead of reading more
 *                                   uninitialized memory
 *   +0x38 / +0x3c "chain" pair   -> both zero (equal), which makes Find()'s
 *                                   own `cmp edi,[ecx+0x38]` / `je` at VA
 *                                   0x4837fa take the "empty, skip" branch
 *                                   for every bucket without ever
 *                                   dereferencing edi as a pointer.
 * Each bucket "slot" is 0x11c (284) bytes (the `imul edx,edx,0x11c`
 * multiplier Find() uses to index it) -- our array reserves that much per
 * slot, zeroed, so a stray read anywhere inside a slot is real, mapped,
 * zero memory rather than a fault.
 *
 * Gives the real builder a 5s head start (in case it's just slow, not
 * dead) so this never fights a legitimate build in progress; only acts if
 * the table is still unbuilt after that. Opt-in behind
 * ULTRA_CAVE_HASHMGR_SELFBUILD=1 -- untested against real gameplay yet,
 * deliberately not defaulted on until live-verified. */
#define HASHMGR_SINGLETON_RVA 0x375730u
#define HASHMGR_BUCKET_STRIDE 0x11cu
#define HASHMGR_BUCKET_COUNT  16u

static int ultra_cave_hashmgr_selfbuild_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("ULTRA_CAVE_HASHMGR_SELFBUILD");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v;
}

static DWORD WINAPI dsii_hashmgr_selfbuild_thread(LPVOID arg) {
    (void)arg;
    Sleep(5000);   /* head start for the real builder, in case it's merely slow */
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    volatile uint32_t* p_singleton = (volatile uint32_t*)(base + HASHMGR_SINGLETON_RVA);

    for (int tries = 0; tries < 20; tries++) {
        uint32_t obj = *p_singleton;
        if (obj != 0) {
            volatile uint32_t* f = (volatile uint32_t*)(uintptr_t)obj;
            uint32_t bucket_count = f[0x34/4];
            if (bucket_count != 0) {
                jvs_diag("Cave DSII hashmgr self-build: table already built by tick=%d (count=%u), nothing to do",
                         tries, (unsigned)bucket_count);
                return 0;
            }
            size_t bytes = (size_t)HASHMGR_BUCKET_COUNT * HASHMGR_BUCKET_STRIDE;
            void* buckets = calloc(1, bytes);
            if (!buckets) {
                jvs_diag("Cave DSII hashmgr self-build: calloc(%zu) failed", bytes);
                return 0;
            }
            f[0x30/4] = (uint32_t)(uintptr_t)buckets;
            f[0x34/4] = HASHMGR_BUCKET_COUNT;
            f[0x38/4] = 0;
            f[0x3c/4] = 0;
            f[0x40/4] = HASHMGR_BUCKET_COUNT;
            jvs_diag("Cave DSII hashmgr self-build: obj=0x%08x was never built (still 0 after 5s) -- "
                     "constructed a valid empty table (buckets=%p count=%u)",
                     (unsigned)obj, buckets, (unsigned)HASHMGR_BUCKET_COUNT);
            return 0;
        }
        Sleep(1000);
    }
    jvs_diag("Cave DSII hashmgr self-build: singleton never allocated after 25s total, giving up");
    return 0;
}

static DWORD WINAPI cave_direct_write_thread(LPVOID arg) {
    (void)arg;
    Sleep(500); /* let DLL_PROCESS_ATTACH finish and log before first diag line */
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    volatile uint8_t*  p_btn = (volatile uint8_t*) (base + 0x374b54u);
    volatile uint8_t*  p_sys = p_btn + 1;
    volatile uint32_t* p_ab8 = (volatile uint32_t*)(base + 0x374ab8u);
    jvs_diag("Cave DSII direct-write: p1=%p sys=%p [0x774ab8]=0x%08x",
             (void*)p_btn, (void*)p_sys, (unsigned)*p_ab8);
    /* [0x774ab8] is the Cave board handle that gates input processing.
     * In normal Cave PC operation the game sets it during COM3 init (0x40 exchange).
     * BUT: Cave PC only opens COM3 AFTER DInput8 confirms a Cave USB device is
     * present - which never happens without real hardware.  After 5 s (tick 625)
     * we force-init: call open_fake_com(3) directly so a slot lands in our
     * Hook_ReadFile table, then write the handle.  The game sees a non-zero
     * [0x774ab8], enables its input path, and push-mode reads go through
     * cave_io_respond.  The direct-write to *p_btn below is belt-and-suspenders. */

    typedef DWORD (WINAPI *pfxi)(DWORD, void*);
    static pfxi s_xi; static int s_xi_init;
    static int s_cave_forced_once;

    /* ---- ONE-SHOT internal test-coin pulse (2026-08-02 diagnostic, owner-
     * requested) -------------------------------------------------------------
     * Fires EXACTLY ONE coin pulse through this thread's own normal sys-byte
     * pipeline (the SAME `sys |= 0x01` -> *p_sys/g_dsii_sys_shared/
     * g_dsii_sys_latch path a real CK('5') keypress or a real XInput BACK/LB
     * press already uses) so a before/after memory diff can determine whether
     * the game ever acts on a coin insert. Deliberately NOT OS input (no
     * SendInput/keybd_event/window messages) -- this never touches the OS
     * input queue or any window's focus, it is indistinguishable from this
     * thread's own existing internal coin-handling code firing on its own.
     * Env-gated, OFF by default:
     *   ULTRA_CAVE_TEST_COIN=1            arm the one-shot pulse
     *   ULTRA_CAVE_TEST_COIN_DELAY_MS=N   delay before firing (default 25000,
     *                                     comfortably past observed ~20s
     *                                     boot-to-attract time) */
    static int s_test_coin_armed = -1, s_test_coin_fired = 0;
    static unsigned s_test_coin_tick = 0;
    if (s_test_coin_armed < 0) {
        const char* e = getenv("ULTRA_CAVE_TEST_COIN");
        s_test_coin_armed = (e && e[0] == '1') ? 1 : 0;
        if (s_test_coin_armed) {
            unsigned delay_ms = 25000;
            const char* d = getenv("ULTRA_CAVE_TEST_COIN_DELAY_MS");
            if (d && *d) { unsigned v = (unsigned)strtoul(d, NULL, 10); if (v > 0) delay_ms = v; }
            s_test_coin_tick = delay_ms / 8u;
            jvs_diag("TEST COIN: armed, will fire ONE pulse at tick=%u (~%ums, in-process only, no OS input)",
                     s_test_coin_tick, delay_ms);
        }
    }

    unsigned tick = 0;
    uint8_t prev_btn = 0;
    for (;;) {
        Sleep(8); /* ~120 Hz */
        tick++;

        if (s_test_coin_armed && !s_test_coin_fired && tick >= s_test_coin_tick - 250 &&
            tick < s_test_coin_tick && (tick % 125) == 0)
            jvs_diag("TEST COIN: firing in ~%ums (tick=%u/%u)", (s_test_coin_tick - tick) * 8, tick, s_test_coin_tick);

        /* MEMDIFF: snapshot the instant before the pulse takes effect. */
        static uint8_t* s_memdiff_before = NULL;
        static int s_memdiff_after_done = 0;
        if (s_test_coin_armed && ultra_cave_memdiff_enabled() &&
            tick >= s_test_coin_tick && tick <= s_test_coin_tick + 260 && (tick % 20) == 0)
            jvs_diag("MEMDIFF: alive tick=%u (before=%p after_done=%d)",
                     tick, (void*)s_memdiff_before, s_memdiff_after_done);
        if (s_test_coin_armed && ultra_cave_memdiff_enabled() &&
            !s_memdiff_before && tick == s_test_coin_tick - 1) {
            uintptr_t mdbase = (uintptr_t)GetModuleHandleA(NULL);
            jvs_diag("MEMDIFF: taking BEFORE snapshot (tick=%u, base=%p, span=0x%x)",
                     tick, (void*)mdbase, (unsigned)ULTRA_MEMDIFF_SPAN);
            DWORD t0 = GetTickCount();
            s_memdiff_before = ultra_memdiff_snapshot(mdbase);
            jvs_diag("MEMDIFF: BEFORE snapshot %s (%lums)", s_memdiff_before ? "done" : "FAILED (alloc)",
                     (unsigned long)(GetTickCount() - t0));
        }
        /* ~2s after the pulse (tick+250 @ ~8ms/tick): snapshot again and diff. */
        if (s_test_coin_armed && ultra_cave_memdiff_enabled() && s_memdiff_before &&
            !s_memdiff_after_done && tick >= s_test_coin_tick + 250) {
            s_memdiff_after_done = 1;
            uintptr_t mdbase = (uintptr_t)GetModuleHandleA(NULL);
            jvs_diag("MEMDIFF: taking AFTER snapshot (tick=%u, ~2000ms post-fire)", tick);
            DWORD t0 = GetTickCount();
            uint8_t* after = ultra_memdiff_snapshot(mdbase);
            jvs_diag("MEMDIFF: AFTER snapshot %s (%lums)", after ? "done" : "FAILED (alloc)",
                     (unsigned long)(GetTickCount() - t0));
            if (after) {
                ultra_memdiff_compare_and_log(mdbase, s_memdiff_before, after);
                free(after);
            } else {
                jvs_diag("MEMDIFF: AFTER snapshot FAILED (alloc)");
            }
            free(s_memdiff_before);
            s_memdiff_before = NULL;
        }

        /* Key state: prefer g_cave_kbd[] (RIDEV_INPUTSINK, focus-independent) then
         * fall back to GetAsyncKeyState (works when game is the foreground process). */
        #define CK(vk) (!ultra_cave_test_headless_flag() && \
                         (g_cave_kbd[(unsigned)(vk)&0xFF] || ((GetAsyncKeyState((int)(vk)) & 0x8000) != 0)))
        uint8_t btn = 0, sys = 0;
        if (CK(VK_UP))    btn |= 0x20; /* UP */
        if (CK(VK_DOWN))  btn |= 0x10; /* DOWN */
        if (CK(VK_LEFT))  btn |= 0x08; /* LEFT */
        if (CK(VK_RIGHT)) btn |= 0x04; /* RIGHT */
        if (CK('Z'))      btn |= 0x02; /* B1/SHOT */
        if (CK('X'))      btn |= 0x01; /* B2/BOMB */
        if (CK('C'))      sys |= 0x80; /* B3/HYPER */
        if (CK('1'))      btn |= 0x80; /* START */
        if (CK('5'))      sys |= 0x01; /* P1 COIN */
        if (CK(VK_F2))    btn |= 0x40; /* TEST */
        #undef CK

        if (!s_xi_init) {
            s_xi_init = 1;
            HMODULE h = LoadLibraryA("xinput1_4.dll");
            if (!h) h = LoadLibraryA("xinput1_3.dll");
            if (h) s_xi = (pfxi)(void*)GetProcAddress(h, "XInputGetState");
        }
        /* Same headless contamination fix as the block above -- skip the real
         * ambient XInput read entirely during a headless test. */
        if (!ultra_cave_test_headless_flag() && s_xi) {
            struct { DWORD pn; struct { WORD w; BYTE lt,rt; SHORT lx,ly,rsx,rsy; } gp; } xs = {0};
            if (s_xi(0, &xs) == 0) {
                WORD xb = xs.gp.w; SHORT xlx = xs.gp.lx, xly = xs.gp.ly;
                #define XI(f) (!!(xb & (f)))
                if (XI(0x0001) || xly >  8000) btn |= 0x20; /* DU/LY+ → UP   */
                if (XI(0x0002) || xly < -8000) btn |= 0x10; /* DD/LY- → DOWN  */
                if (XI(0x0004) || xlx < -8000) btn |= 0x08; /* DL/LX- → LEFT  */
                if (XI(0x0008) || xlx >  8000) btn |= 0x04; /* DR/LX+ → RIGHT */
                if (XI(0x1000))                btn |= 0x02; /* A → B1/SHOT    */
                if (XI(0x2000))                btn |= 0x01; /* B → B2/BOMB    */
                if (XI(0x4000))                sys |= 0x80; /* X → B3/HYPER   */
                if (XI(0x0010))                btn |= 0x80; /* START           */
                if (XI(0x0020) || XI(0x0100))  sys |= 0x01; /* BACK/LB → COIN */
                #undef XI
            }
        }

        /* TEST COIN: fire the ONE-SHOT pulse exactly at the target tick, through
         * the same sys-byte pipeline every other coin source uses below. Cleared
         * immediately (s_test_coin_fired=1) so this is a single brief pulse, the
         * same shape a real momentary coin-switch closure would produce. */
        /* HELD PULSE (2026-08-13): a single 8ms tick is far shorter than a real
         * coin-switch closure (tens of ms as the coin physically passes the
         * sensor) -- if the game debounces coin input over several consecutive
         * polls to reject electrical noise, a one-tick pulse could read as
         * noise and get silently rejected. Untested hypothesis; hold the bit
         * for 50 ticks (~400ms) instead of one, a duration no real debounce
         * window would plausibly reject. ULTRA_CAVE_TEST_COIN_HOLD_TICKS
         * overrides for experimentation. */
        {
            static int s_hold_ticks = -1;
            if (s_hold_ticks < 0) {
                const char* e = getenv("ULTRA_CAVE_TEST_COIN_HOLD_TICKS");
                s_hold_ticks = (e && *e) ? atoi(e) : 50;
                if (s_hold_ticks < 1) s_hold_ticks = 1;
            }
            if (s_test_coin_armed && !s_test_coin_fired && tick >= s_test_coin_tick &&
                tick < (unsigned)(s_test_coin_tick + (unsigned)s_hold_ticks)) {
                sys |= 0x01;
                if (tick == s_test_coin_tick)
                    jvs_diag("TEST COIN: FIRING NOW (tick=%u), holding sys=0x01 for %d ticks (~%dms)",
                             tick, s_hold_ticks, s_hold_ticks * 8);
                if (tick == (unsigned)(s_test_coin_tick + (unsigned)s_hold_ticks - 1)) {
                    s_test_coin_fired = 1;
                    jvs_diag("TEST COIN: pulse released (tick=%u) [0x774ab8]=0x%08x", tick, (unsigned)*p_ab8);
                }
            }
        }

        *p_btn = btn;
        *p_sys = sys;
        g_dsii_btn_shared = btn;
        g_dsii_sys_shared = sys;
        /* Latch: OR in new bits so cave_io_respond never misses a short press */
        g_dsii_btn_latch |= btn;
        g_dsii_sys_latch |= sys;

        /* If [0x774ab8] is 0, the game didn't self-init COM3 (it needs a real
         * Cave DI8 device for that gate, which -- per the analysis above --
         * never happens without real hardware).  Force it ourselves.
         * Board struct base = 0x774AB4 (ESI+4 == [0x774AB8] guard).
         * input_poll reads [ESI+0x10] == [0x774AC4] as the COM3 handle for ReadFile.
         * We must write our fake handle there too so the generic ReadFile hook
         * (and this file's dsii_on_read_file) intercepts it.
         *
         * OBSERVED (2026-07-20 log trace): a one-shot force-init at tick==2 DOES
         * land -- [0x774ab8] reads back non-zero immediately after -- but by
         * tick~1800 (~14s later) the game has silently cleared it back to 0
         * while leaving [0x774ac4] (the handle field) untouched. Something in
         * the game's own re-validation path (almost certainly gated on the same
         * DI8-device check, or on the COM3 0x40 handshake reply content not
         * matching what real hardware would send) revokes the gate once it
         * notices no real board answered "correctly". Since the DI8 device will
         * never appear under emulation, that revocation would otherwise be
         * permanent. Rather than reverse-engineer the exact clear condition,
         * out-stubborn it: re-assert the gate EVERY tick (~120 Hz) using a
         * cached handle (open_fake_com() is NOT safe to call every tick -- it
         * allocates a fresh slot-table entry each time and the table is
         * fixed-size) so any clear the game performs is undone within ~8 ms. */
        if (*p_ab8 == 0) {
            if (s_cave_com_handle == INVALID_HANDLE_VALUE) {
                HANDLE fh = open_fake_com(3);
                if (fh && fh != INVALID_HANDLE_VALUE) {
                    s_cave_com_handle = fh;
                } else {
                    jvs_diag("Cave DSII: force-init open_fake_com(3) FAILED");
                }
            }
            if (s_cave_com_handle != INVALID_HANDLE_VALUE) {
                volatile uint32_t* p_com3h = (volatile uint32_t*)(base + 0x374ac4u);
                /* CREDIT-STATE FIX (2026-08-13, gated on ULTRA_CAVE_QREPORT): the
                 * old code wrote the COM3 HANDLE value into 0x774ab8 -- but the
                 * disassembly proves 0x774ab8 is the game's coin/credit STATE
                 * variable, written by input_poll (0x45c55b), the '?' coinage
                 * report handler (0x45c7b1 -> -6), and the report-timeout path
                 * (0x45c750 -> -2), and READ by the coin/credit message
                 * selectors at 0x4aa210/0x4bc2e0 (value -6 = coin-accept mode
                 * showing coinage 0x774abc; positive = the wrong analog path).
                 * Writing the handle (a positive number ~0x9ac) parked the game
                 * permanently OUT of coin-accept mode -- one more reason coins
                 * never became credits. The board-present gate the old comment
                 * worried about only needs 0x774ab8 != 0, and -6 satisfies that
                 * AND selects coin-accept. Since our '?' coinage report can't
                 * reach the init-only '?' handler from steady-state push mode,
                 * we seed the coin-accept state (0x774ab8=-6, coinage 0x774abc=1)
                 * directly here instead. The handle still goes to its correct
                 * home, the 0x774ac4 device-handle array. Guard stays `*p_ab8==0`
                 * so once seeded (or once the game itself updates the credit
                 * state) we never clobber it. */
                if (ultra_cave_qreport_active()) {
                    volatile uint32_t* p_abc = (volatile uint32_t*)(base + 0x374abcu);
                    *p_ab8   = 0xfffffffau;                             /* -6: coin-accept state */
                    *p_abc   = 1u;                                      /* 1 coin / 1 credit     */
                    *p_com3h = (uint32_t)(uintptr_t)s_cave_com_handle;  /* handle -> correct array */
                    if (!s_cave_forced_once || (tick % 1800) == 0) {
                        jvs_diag("Cave DSII: seed coin-accept [0x774ab8]=-6 [0x774abc]=1 [0x774ac4]=%p (tick=%u)",
                                 (void*)s_cave_com_handle, tick);
                        s_cave_forced_once = 1;
                    }
                } else {
                    *p_ab8   = (uint32_t)(uintptr_t)s_cave_com_handle;
                    *p_com3h = (uint32_t)(uintptr_t)s_cave_com_handle;
                    if (!s_cave_forced_once || (tick % 1800) == 0) {
                        jvs_diag("Cave DSII: force-init [0x774ab8]=%p [0x774ac4]=%p -> CaveRD armed (tick=%u)",
                                 (void*)s_cave_com_handle, (void*)s_cave_com_handle, tick);
                        s_cave_forced_once = 1;
                    }
                }
            }
        }

        if (btn != prev_btn) {
            jvs_diag("Cave DSII btn: 0x%02x->0x%02x [0x774ab8]=0x%08x",
                     (unsigned)prev_btn, (unsigned)btn, (unsigned)*p_ab8);
            prev_btn = btn;
        }
        /* periodic readback every ~15s */
        if (tick % 1800 == 0) {
            volatile uint32_t* p_com3h = (volatile uint32_t*)(base + 0x374ac4u);
            jvs_diag("Cave DSII tick%u: p_btn=0x%02x p_sys=0x%02x [0x774ab8]=0x%08x [0x774ac4]=0x%08x",
                     tick, (unsigned)*p_btn, (unsigned)*p_sys, (unsigned)*p_ab8, (unsigned)*p_com3h);
        }
    }
    return 0;
}

/* ============================================================================
 *  board_io_ops callbacks
 * ============================================================================
 */

/* CreateFileA/W: CVKEY.222 detection + synthetic handle. Called by
 * jvs_serial_hook.c BEFORE its generic fake-COM/dongle/D-drive branches, for
 * both the narrow and (already ANSI-converted) wide paths. */
static int dsii_on_create_file(const char* path, HANDLE* out_handle) {
    jvs_diag("  -> cvkey_chk: '%s' match=%d", path ? path : "(null)", path ? is_cvkey_path(path) : 0);

    /* CVUPD diagnostic (2026-08-02): gated separately from CVKEY so it can be
     * tested independently. Claims ANY path containing "CVUPD" (e.g.
     * "E:\CVUPD\CVGAME\res\err.log") with a REAL temp-file-backed handle --
     * unlike CVKEY.222's carefully-crafted 56-byte synthetic content, this is
     * a log-file-shaped path (game only appears to WRITE to it, never read a
     * specific expected value back per the live log evidence -- 5 consecutive
     * CreateFileA calls, no ReadFile in between), so a plain real backing
     * file is the right fidelity level for this experiment: "present, whatever
     * gets written just lands on disk" vs. today's "absent, silently fails". */
    static int s_cvupd_diag = -1;
    if (s_cvupd_diag < 0) {
        const char* e = getenv("ULTRA_CAVE_CVUPD_FAKE");
        s_cvupd_diag = (e && e[0] == '1') ? 1 : 0;
    }
    if (s_cvupd_diag && path && is_cvupd_path(path)) {
        char tmp[MAX_PATH], fp[MAX_PATH];
        GetTempPathA(sizeof tmp, tmp);
        _snprintf(fp, sizeof fp, "%sultra_cvupd_%08x.dat", tmp, (unsigned)GetTickCount());
        fp[sizeof fp - 1] = 0;
        HANDLE fh = CreateFileA(fp, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) g_cvupd_fake = fh;
        jvs_diag("  -> CVUPD path faked (present-but-empty): '%s' -> handle=%p (backing=%s)",
                 path, (void*)fh, fp);
        if (out_handle) *out_handle = fh;
        return 1;
    }

    if (!path || !is_cvkey_path(path)) return 0;   /* not ours: fall through */

    /* CV2000XP board key: create a temp file so the auth check passes.
     * The handle is tracked in g_cvkey_fake so dsii_on_read_file can log and
     * satisfy any content read the game issues after the open. */
    char tmp[MAX_PATH], fp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf(fp, sizeof fp, "%sultra_cvkey222.dat", tmp); fp[sizeof fp-1] = 0;
    /* Calling CreateFileA directly here (not a resolved "Real_" pointer) is
     * safe: ultra_iat_patch_all() explicitly excludes THIS module's own
     * imports from IAT patching (`if (mods[i] != self)` in jvs_serial_hook.c),
     * so a direct call from this DLL's own code always reaches the true
     * kernel32 export, never our own game's Hook_CreateFileA. */
    HANDLE fh = CreateFileA(fp, GENERIC_READ|GENERIC_WRITE,
                                 FILE_SHARE_READ|FILE_SHARE_WRITE,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        fh = CreateEventA(NULL, TRUE, TRUE, NULL); /* fallback pseudo-handle */
    if (fh != INVALID_HANDLE_VALUE) g_cvkey_fake = fh;
    jvs_diag("  -> CVKEY.222 faked (handle=%p)", (void*)fh);
    if (out_handle) *out_handle = fh;
    return 1;   /* handled either way -- matches the old code, which never fell
                 * through to Real_CreateFileA once is_cvkey_path matched */
}

/* ReadFile: called by jvs_serial_hook.c TWICE per call -- once unconditionally
 * near the top (for a non-com_slot handle like our CVKEY.222 handle) and once
 * more, only if the generic com_slot queue was empty, for an already-confirmed
 * com_slot handle (our live Cave COM3 handle's push-mode case). Both cases are
 * handled here; whichever call site reaches us, we just check `h` ourselves. */
static int dsii_on_read_file(HANDLE h, void* buf, DWORD want, DWORD* out_read) {
    /* Cave PC push-mode pacing: real COM3 hardware at 115200 baud can't
     * deliver a fresh 9-byte frame faster than ~780us. Cave titles (e.g.
     * Deathsmiles II) busy-poll ReadFile with no throttle of their own,
     * relying on that natural serial latency to pace the loop. Our hook
     * answers instantly every call, so without a cap the poll thread pegs
     * a full CPU core (measured 100%+) for as long as it's waiting --
     * observed to stretch boot-to-attract time from seconds to 60+ real
     * seconds by starving the loader/decompression thread of cycles.
     * NOTE: jvs_serial_hook.c's Hook_ReadFile still has its OWN legacy copy of
     * this exact pacing gated on cave_io_active() (ULTRA_CAVE_IO) -- harmless
     * if both fire (worst case ~2ms instead of ~1ms), but this file no longer
     * reads ULTRA_CAVE_IO at all (see the file header), so this copy is kept
     * to guarantee the fix applies for DSII regardless of whether that env
     * var happens to also be set. */
    {
        static volatile LONG s_dsii_last_tick;
        LONG now = (LONG)GetTickCount();
        LONG last = s_dsii_last_tick;
        if (last != 0 && (DWORD)(now - last) < 1) Sleep(1);
        s_dsii_last_tick = (LONG)GetTickCount();
    }

    /* CVKEY.222 fake handle: return a synthetic key record so the game's
     * CV2000XP board-key auth passes and input processing is enabled.
     * The game reads 56 bytes per check.  Real hardware stores a board
     * identity + simple checksum on removable media (E:\ or F:\).  We
     * synthesize a plausible record: ASCII "CV2K" header, version byte,
     * pad, and a byte-sum checksum in the last byte.
     *
     * DSII coin->credit root cause (2026-07-21, static RE via objdump on
     * game.exe): the CVKEY validator at VA 0x45da55-0x45dacc (and an
     * identical check at 0x45e084-0x45e0fb; both reached from a loop over
     * "type==2" drive-check objects, one iteration per drive letter E:/F:)
     * reads our 56-byte reply and compares BUFFER OFFSET 8..15 (not 4..11 --
     * verified by tracing the exact `lea`/`push` sequence feeding the Read()
     * call to recover the true base-pointer-to-esp delta at the compare site)
     * against a FIXED 8-byte reference embedded in game.exe's own .data at
     * VA 0x6260d4 (never written anywhere in game.exe -- confirmed via a full
     * disassembly grep -- so it is a compile-time constant, safe to hardcode
     * here). Read directly from the game image: 0D 01 09 0E 19 05 09 01. The
     * SAME 8-byte constant also appears at 0x6260dc and 0x6260e4, matching a
     * second/third call site with the identical check, so one fixed value
     * covers all of them. Our old i^0xA5 filler never matched (0xAD,0xAC,...),
     * so the match=1 open always failed cert validation -- match=1 in the log
     * only means "this IS the CVKEY file", NOT "its contents validated" --
     * and coin input, though wired correctly end-to-end (confirmed via
     * CaveRD 0x31 byte), was silently discarded upstream of any credit
     * accounting.  On mismatch the code falls back to a SECOND check
     * (buffer offset 32..39 against the LIVE [0x774b54..5b] input array,
     * which also fails since input state is asynchronous to whatever the
     * CVKEY content is) before giving up entirely.  Placing the real
     * reference bytes at offset 8..15 lets the PRIMARY (clean) validation
     * path succeed instead. Unverified: whether this flag is the direct
     * credit-accept gate or one of several independent checks; this is the
     * strongest lead found and the fix is additive/low-risk (only changes
     * our own synthetic file content), so worth testing live. */
    if (h != INVALID_HANDLE_VALUE && h == g_cvkey_fake && buf && want) {
        DWORD give = (want > 256) ? 256 : want;
        uint8_t* b = (uint8_t*)buf;
        memset(b, 0x01, give);
        static int s_key_off = -1;
        if (s_key_off < 0) {
            const char* e = getenv("ULTRA_CAVE_CVKEY_OFFSET");
            s_key_off = (e && e[0] == '4') ? 4 : 8;
        }
        if (give >= 56) {
            b[0]='C'; b[1]='V'; b[2]='2'; b[3]='K';
            b[4] = 0x04;   /* version */
            b[5] = 0x00;   /* flags   */
            b[6] = 0x01;   /* board type: CV2000XP */
            b[7] = 0x01;   /* region   */
            for (int i = 8; i < 55; i++) b[i] = (uint8_t)(i ^ 0xA5);
            /* Real board-key reference bytes (read from game.exe VA 0x6260d4 /
             * 0x6260dc / 0x6260e4, all identical) -- overwrite the generic
             * filler at the exact offset the validator compares.
             *
             * OFFSET EXPERIMENT (2026-08-02): the validator at VA 0x45da4f-55
             * reads `mov 0x34(%esp),%dl` then compares 8 bytes at esp+0x34..
             * +0x3b against the reference. The 2026-07-21 note below claims
             * (after tracing the lea/push/call sequence) this corresponds to
             * BUFFER offset 8..15. Re-tracing the same sequence today: the
             * buffer is declared via `lea 0x30(%esp),%eax` immediately before
             * TWO vtable calls (Read, then Close) -- the Read call has exactly
             * one pushed stack arg (the buffer pointer) and the Close call has
             * none, and both appear to be callee-cleanup (thiscall-style, no
             * caller-side `add esp,N` visible after either `call`). If that's
             * right, esp nets back to its pre-call value after both returns,
             * which would put the compare at buffer offset 4..11, not 8..15.
             * Genuine disagreement with the earlier trace, not something to
             * resolve by re-reading the same disassembly harder -- test both
             * live instead. Default stays 8 (today's runs all used offset 8
             * with zero credit movement, so it's already been exercised);
             * ULTRA_CAVE_CVKEY_OFFSET=4 switches to the alternate hypothesis
             * for an A/B headless test. Nothing else in this file reads the
             * version/flags/board-type/region bytes this would overwrite when
             * set to 4 (confirmed via grep), so this is safe either way. */
            static const uint8_t kRefKey[8] = {0x0D,0x01,0x09,0x0E,0x19,0x05,0x09,0x01};
            memcpy(b + s_key_off, kRefKey, sizeof(kRefKey));
            uint8_t cksum = 0;
            for (int i = 0; i < 55; i++) cksum += b[i];
            b[55] = cksum;
        }
        if (out_read) *out_read = give;
        jvs_diag("ReadFile CVKEY.222 req=%lu give=%lu (synth key, ref bytes @off=%d) buf=%p",
                 (unsigned long)want, (unsigned long)give, s_key_off, (void*)b);
        return 1;
    }

    /* Cave PC push mode: after the handshake the game reads without writing.
     * Drain our own tiny response queue first (a prior dsii_on_write_file's
     * answer -- e.g. the 0x40 status-poll reply); if it's empty, synthesize a
     * fresh input-poll frame live from current input, exactly like the old
     * code's fallback inside Hook_ReadFile's `if (s) {...}` block did (there,
     * "s" was jvs_serial_hook.c's com_slot; here it's this file's own queue --
     * see the RESPONSE QUEUING note in the file header for why). */
    if (h != INVALID_HANDLE_VALUE && h == s_cave_com_handle) {
        if (!s_resp_lock_init) { InitializeCriticalSection(&s_resp_lock); s_resp_lock_init = 1; }
        EnterCriticalSection(&s_resp_lock);
        size_t avail = s_resp_len - s_resp_pos;
        if (avail) {
            size_t give = (want < avail) ? want : avail;
            memcpy(buf, s_resp_buf + s_resp_pos, give); s_resp_pos += give;
            LeaveCriticalSection(&s_resp_lock);
            jvs_diag("ReadFile req=%lu give=%zu (queued=%zu)", (unsigned long)want, give, avail);
            if (out_read) *out_read = (DWORD)give;
            return 1;
        }
        LeaveCriticalSection(&s_resp_lock);
        if (want >= 1) {
            static unsigned int cave_rd_cnt = 0;
            ultra_jvs_node* node = ultra_hook_node();
            ultra_hook_pull_input(node);
            uint8_t frame[16];
            cave_io_respond(node, (const uint8_t*)"\x26", 1, frame, sizeof(frame));

            /* EXPERIMENT (2026-08-13, ULTRA_CAVE_QREPORT=1): the live report
             * parser at game+0x45c580 -- the ONLY input decoder proven to run
             * every frame -- ignores our 0x26 frame entirely and instead
             * consumes 4-byte 'P'/'Q'/'R'/'S' reports, writing switch bits to
             * [dev+0x28..0x58] which the caller (0x45c9f0 loop) copies into the
             * game's live input object at [+0x270]. input_poll (0x45c370), the
             * consumer of our 9-byte 0x26/ASCII frame, is DEAD (0 live hits over
             * minutes, callers never reached). So we have been feeding the wrong
             * pipe for the game's entire history under Ultra Loader.
             *
             * This mode replaces the push-mode frame with a 4-byte 'Q' switch
             * report built from the SAME input state cave_io_respond just
             * computed (decoded back out of the ASCII frame). dl bit -> game
             * offset mapping reverse-engineered byte-for-byte from the 'Q'
             * handler at game+0x45c634:
             *   dl 0x80 -> [dev+0x38]   dl 0x40 -> [dev+0x50]
             *   dl 0x20 -> [dev+0x24]   dl 0x10 -> [dev+0x26]
             *   dl 0x08 -> [dev+0x28]   dl 0x04 -> [dev+0x2a]
             *   dl 0x02 -> [dev+0x2c]   dl 0x01 -> [dev+0x2e]
             * Coin bit assignment is a first guess (dl 0x40 -> [dev+0x50], a
             * distinctive standalone flag) pending the live copy-loop trace
             * that will reveal the true offset each input lands at. Default OFF
             * -- normal play + every prior test stays byte-identical. */
            int s_qreport = ultra_cave_qreport_active();
            if (s_qreport) {
                /* BURST-THEN-QUIET (2026-08-13): the parser sweep (game+0x45c580)
                 * loops reading reports and only EXITS when a read returns 0
                 * bytes (game+0x45c76f `jne 0x45c5c0` loops while ReadIndexed!=0;
                 * falls through to `ret` when it returns 0). The outer worker-
                 * thread I/O loop (game+0x45ba30) only advances -- and thus only
                 * ever reaches its input_poll branch (the coin/credit reader) --
                 * once the parser sweep RETURNS. If we hand back a valid 'Q'
                 * report on every single read, the sweep never exits and
                 * input_poll is starved forever (live-confirmed: 0 input_poll
                 * '@'/'&' writes over minutes, WriteFile stuck at 2). Real
                 * hardware sends a short burst of reports then goes quiet; mimic
                 * that by returning EMPTY reads for the tail of each cycle so the
                 * sweep exits, the loop advances, and input_poll gets to run
                 * (paired with ULTRA_CAVE_FORCE_IPOLL which removes the 1000-tick
                 * counter gate so it runs promptly). Only the parser's push-mode
                 * reads hit this; input_poll's own WriteFile-triggered handshake
                 * reads drain the response queue above this branch, unaffected. */
                if (ultra_serial_burst_quiet(cave_rd_cnt, 8, 2)) {  /* 2 empty reads at the tail of every 8 */
                    cave_rd_cnt++;
                    if (out_read) *out_read = 0;
                    return 1;
                }
                /* COINAGE CONFIG ('?') report, interleaved so the game enters and
                 * stays in coin-accept state (0x774ab8 = -6). The '?' handler at
                 * game+0x45c7a4 requires buf[0]=='?' AND buf[1]=='?', then reads
                 * buf[2]/buf[3] as the P1/P2 coinage char: '1'=1coin/1credit,
                 * '2'=2/1, '3','4', 'A'=10, 'B'=11, 'C'=12, anything-else=0.
                 * ULTRA_CAVE_COINAGE overrides the char (default '1'). Sent every
                 * 32nd read so the parser's 6000-tick no-report timeout (which
                 * would reset 0x774ab8 to -2 at game+0x45c750) never fires while
                 * still leaving 31/32 reads for live 'Q' switch input. */
                static int s_coinage = -2;
                if (s_coinage == -2) {
                    const char* c = getenv("ULTRA_CAVE_COINAGE");
                    s_coinage = (c && c[0]) ? (unsigned char)c[0] : '1';
                }
                if ((cave_rd_cnt & 31u) == 5u) {
                    uint8_t crep[4] = { 0x3f, 0x3f, (uint8_t)s_coinage, (uint8_t)s_coinage };
                    DWORD cgive = (want < 4) ? want : 4;
                    memcpy(buf, crep, cgive);
                    if (cave_rd_cnt < 200)
                        jvs_diag("CoinCfg #%u -> 3f 3f %02X %02X (coinage='%c')", cave_rd_cnt, s_coinage, s_coinage, s_coinage);
                    cave_rd_cnt++;
                    if (out_read) *out_read = cgive;
                    return 1;
                }
                uint8_t cbtn = (uint8_t)(frame[1] - 0x30);   /* cave_io_respond button byte */
                uint8_t csys = (uint8_t)(frame[2] - 0x30);   /* cave_io_respond system byte  */
                uint8_t dl = 0;
                if (cbtn & 0x20) dl |= 0x20;  /* UP    -> [dev+0x24] */
                if (cbtn & 0x10) dl |= 0x10;  /* DOWN  -> [dev+0x26] */
                if (cbtn & 0x08) dl |= 0x08;  /* LEFT  -> [dev+0x28] */
                if (cbtn & 0x04) dl |= 0x04;  /* RIGHT -> [dev+0x2a] */
                if (cbtn & 0x02) dl |= 0x02;  /* SHOT  -> [dev+0x2c] */
                if (cbtn & 0x01) dl |= 0x01;  /* BOMB  -> [dev+0x2e] */
                if (cbtn & 0x80) dl |= 0x80;  /* START -> [dev+0x38] */
                if (csys & 0x01) dl |= 0x40;  /* COIN  -> [dev+0x50] (guess) */
                uint8_t qrep[4] = { 0x51, dl, 0x00, 0x00 };
                DWORD qgive = (want < 4) ? want : 4;
                memcpy(buf, qrep, qgive);
                if (cave_rd_cnt < 10 || dl != 0)
                    jvs_diag("QRep #%u -> 51 %02X 00 00 (cbtn=%02X csys=%02X)", cave_rd_cnt, dl, cbtn, csys);
                cave_rd_cnt++;
                if (out_read) *out_read = qgive;
                return 1;
            }

            DWORD give = (want < 9) ? want : 9;
            memcpy(buf, frame, give);
            if (cave_rd_cnt < 10 || (cave_rd_cnt % 2000) == 0 ||
                frame[1] != 0x30 || frame[2] != 0x30)
                jvs_diag("CaveRD #%u -> %02X %02X %02X %02X %02X %02X %02X %02X %02X", cave_rd_cnt,
                         frame[0], frame[1], frame[2], frame[3], frame[4],
                         frame[5], frame[6], frame[7], frame[8]);
            cave_rd_cnt++;
            if (out_read) *out_read = give;
            return 1;
        }
        if (out_read) *out_read = 0;
        return 1;
    }
    return 0;   /* not ours: fall through to the generic path */
}

/* WriteFile: jvs_serial_hook.c only calls this from inside its own
 * `com_slot* s = slot_for(h); if (s) {...}` block, i.e. `h` is ALREADY
 * confirmed to be one of its tracked fake-COM handles before we're ever
 * invoked -- so there's no need (and no way, short of an extra extern into
 * jvs_serial_hook.c's private slot table) to re-validate `h` here ourselves. */
static int dsii_on_write_file(HANDLE h, const void* buf, DWORD len) {
    /* Remember which handle is "our" live Cave COM3 handle -- covers the
     * normal path (the game's own legitimate CreateFileA-then-WriteFile
     * handshake on COM3) in addition to cave_direct_write_thread's force-init
     * path (which sets this directly via open_fake_com(3) -- see above). */
    s_cave_com_handle = h;

    ultra_jvs_node* node = ultra_hook_node();
    ultra_hook_pull_input(node);                       /* latest controller state */

    uint8_t tmp[16];
    size_t got = cave_io_respond(node, (const uint8_t*)buf, len, tmp, sizeof(tmp));

    if (!s_resp_lock_init) { InitializeCriticalSection(&s_resp_lock); s_resp_lock_init = 1; }
    EnterCriticalSection(&s_resp_lock);
    if (s_resp_pos) {   /* compact any already-consumed response bytes */
        memmove(s_resp_buf, s_resp_buf + s_resp_pos, s_resp_len - s_resp_pos);
        s_resp_len -= s_resp_pos; s_resp_pos = 0;
    }
    size_t room = sizeof(s_resp_buf) - s_resp_len;
    if (got > room) got = room;
    memcpy(s_resp_buf + s_resp_len, tmp, got);
    s_resp_len += got;
    LeaveCriticalSection(&s_resp_lock);

    /* jvs_serial_hook.c logs "TX" itself right after on_write_file returns 1;
     * we only need to log our own "RX" here. */
    jvs_diag_hex("RX", tmp, got);
    return 1;
}

/* CloseHandle: called on EVERY close in the process, for any handle. */
static void dsii_on_close_handle(HANDLE h) {
    /* CVKEY.222 handle-aliasing fix: g_cvkey_fake is a single global sentinel
     * matched by raw numeric HANDLE value (not by which file the handle
     * actually refers to). Deathsmiles II reopens CVKEY.222 every attract
     * cycle, and each open used to overwrite g_cvkey_fake without ever
     * clearing it on close. Windows recycles handle values almost
     * immediately after CloseHandle, so a LATER CreateFileA for an unrelated
     * file (e.g. the score-ranking data file) could be assigned the exact
     * numeric value the CVKEY temp handle just freed -- and every ReadFile on
     * that file would then silently return synthesized CVKEY bytes instead of
     * real data, which the game parses as corrupt ranking data and crashes
     * on. Clear the sentinel the moment its handle closes so a recycled
     * value can never alias it again. */
    if (h == g_cvkey_fake) g_cvkey_fake = INVALID_HANDLE_VALUE;
    /* Same handle-recycling risk applies to the CVUPD diagnostic handle. */
    if (h == g_cvupd_fake) g_cvupd_fake = INVALID_HANDLE_VALUE;

    /* The SAME handle-recycling risk applies to s_cave_com_handle, a cache
     * this extraction introduces (the old code had no single-handle cache
     * here -- see the file header's RESPONSE QUEUING note). Extending the
     * g_cvkey_fake fix's lesson to it: clear it (and drop any stale queued
     * bytes) the instant its handle closes. */
    if (h == s_cave_com_handle) {
        s_cave_com_handle = INVALID_HANDLE_VALUE;
        if (s_resp_lock_init) {
            EnterCriticalSection(&s_resp_lock);
            s_resp_len = s_resp_pos = 0;
            LeaveCriticalSection(&s_resp_lock);
        }
    }
}

/* WaitCommEvent: Cave push mode has no pre-queued data (the generic
 * com_slot.resp is never populated for this game -- see RESPONSE QUEUING),
 * so we must always signal EV_RXCHAR so the game's WaitCommEvent loop
 * advances to ReadFile, where dsii_on_read_file generates the frame live. */
static int dsii_on_wait_comm_event_pending(void) {
    return 1;
}

/* LoadLibrary redirect: Cave PC's bundled dinput8.dll. jvs_serial_hook.c only
 * calls this for a bare name (no path separators) already, so we don't need
 * to re-check that ourselves. */
static const char* dsii_on_load_library_redirect(const char* name) {
    if (!g_cave_di8_path[0] || !name) return NULL;
    if (!is_dinput8_name(name)) return NULL;
    return g_cave_di8_path;
}

/* on_install: called once from ultra_jvs_serial_install(), after the generic
 * Real_* kernel32/COM pointers resolve (and, per jvs_serial_hook.c's periodic
 * re-patch worker, potentially again roughly every second after that -- every
 * piece below is either cheap to recompute or explicitly once-only guarded,
 * matching what the pre-extraction inline code already did). */
static void dsii_on_install(void) {
    /* Resolve the game-local dinput8.dll path for the LoadLibrary redirect. */
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* bs = strrchr(exe, '\\');
        if (bs) { bs[1] = 0; snprintf(g_cave_di8_path, MAX_PATH, "%sdinput8.dll", exe); }
        if (GetFileAttributesA(g_cave_di8_path) == INVALID_FILE_ATTRIBUTES)
            g_cave_di8_path[0] = 0;
        if (g_cave_di8_path[0])
            jvs_diag("Cave DI8 redirect: %s", g_cave_di8_path);
    }

    /* Hook GetIfTable (static-import sites) to eliminate the NIC-check retry
     * loop. See KNOWN GAP #2 above game_dsii_hook_getproctable() for the
     * dynamic-GetProcAddress-resolution path this does NOT yet cover. */
    {
        HMODULE ip = GetModuleHandleA("iphlpapi.dll");
        if (!ip) ip = LoadLibraryA("iphlpapi.dll");
        if (ip) Real_GetIfTable = (PFN_GetIfTable)(void*)GetProcAddress(ip, "GetIfTable");
    }
    ultra_iat_patch_all("iphlpapi.dll", "GetIfTable", (void*)Hook_GetIfTable);

    /* Focus-independent keyboard: start LL-hook message-pump thread (once only).
     *
     * SKIPPED in headless test mode (ULTRA_CAVE_TEST_HEADLESS=1, off by
     * default -- same var d3d9_throttle.c's window-visibility fix uses).
     * 2026-08-02: this thread's whole point is to read REAL physical
     * keyboard state globally, independent of window focus (see its own
     * header comment: "works from any thread in the foreground process" /
     * "fires even when game window is not in focus"). During an automated
     * headless test run that is a real problem even with the window now
     * hidden: it silently captures and feeds in WHATEVER the user happens to
     * be typing anywhere else on their real keyboard at the time (observed
     * live -- ambient keystrokes showed up as Cave DSII button presses in the
     * diag log during a test run), contaminating test determinism. Headless
     * tests drive input exclusively via the existing in-process
     * ULTRA_CAVE_TEST_COIN mechanism, never real keyboard/OS input, so this
     * thread is simply not needed while testing -- skipping it is pure
     * upside (no functional loss for testing, no change to normal play). */
    { static int s_started; if (!s_started) { s_started = 1;
        /* Checks ULTRA_CAVE_TEST_NOPAUSE too (2026-08-13): that flag was split
         * off from ULTRA_CAVE_TEST_HEADLESS specifically because THAT flag's
         * own window-positioning path (skipping cave_pc_main.c's brief real
         * SetForegroundWindow) turned out to self-pause this game -- but
         * every OTHER "don't let the host's real input/focus reach the test"
         * concern this flag originally bundled, including this thread, still
         * needs to fire under the new flag too, or splitting it re-opens
         * exactly the contamination this comment already documented (live-
         * reconfirmed same day: ambient 'C' keypresses showed up as Cave DSII
         * button presses again once ULTRA_CAVE_TEST_NOPAUSE stopped implying
         * this skip). */
        const char* h = getenv("ULTRA_CAVE_TEST_HEADLESS");
        const char* np = getenv("ULTRA_CAVE_TEST_NOPAUSE");
        if (!((h && h[0] == '1') || (np && np[0] == '1'))) {
            HANDLE t = CreateThread(NULL, 0, cave_kb_thread, NULL, 0, NULL); if (t) CloseHandle(t);
        } else {
            jvs_diag("Cave kbd: SKIPPED (headless test mode -- real keyboard capture disabled)");
        }
    } }

    /* input_poll [ESI+4] guard locks COM3 out after frame 1 -- NOP it, and
     * start the belt-and-suspenders direct-write thread (once only). Unlike
     * the pre-extraction code, no separate exe-size re-check is needed here:
     * ultra_board_ops_autoregister() below already gated OUR registration (and
     * therefore this whole on_install call) on that exact identity check. */
    { static int s_dw; if (!s_dw) { s_dw = 1;
        dsii_nop_input_poll_guard();
        dsii_force_input_poll();          /* ULTRA_CAVE_FORCE_IPOLL: run the coin/credit reader every I/O iteration */
        dsii_guard_hash_divide();
        HANDLE t = CreateThread(NULL, 0, cave_direct_write_thread, NULL, 0, NULL);
        if (t) { jvs_diag("Cave DSII: direct-write thread started"); CloseHandle(t); } } }

    { static int s_hb; if (!s_hb) { s_hb = 1;
        if (ultra_cave_hashmgr_selfbuild_enabled()) {
            HANDLE t = CreateThread(NULL, 0, dsii_hashmgr_selfbuild_thread, NULL, 0, NULL);
            if (t) { jvs_diag("Cave DSII: hashmgr self-build thread started"); CloseHandle(t); }
        } } }

    { static int s_ht; if (!s_ht) { s_ht = 1;
        if (ultra_cave_diag_hashtable_enabled()) {
            HANDLE t = CreateThread(NULL, 0, cave_diag_hashtable_thread, NULL, 0, NULL);
            if (t) { jvs_diag("Cave DSII: hash-table diag thread started"); CloseHandle(t); }
        } } }

    /* Universal transport layer: declare this board's ACTUAL shape so the
     * shared pacing/range/codec core is live and self-describing in the log.
     * Cave is the PUSH case -- after the handshake the game only ever READS,
     * so nothing bounds the data we synthesize and its parser sweep at
     * game+0x45c580 never exits without the 8/2 burst-then-quiet gate.
     * The proven read path in dsii_on_read_file is deliberately NOT rerouted
     * here: that migration is a separate, separately-tested step, and this
     * board is the one with a confirmed credit loop to protect. */
    { static int s_utx; if (!s_utx) { s_utx = 1;
        static utx_profile cave_prof;
        memset(&cave_prof, 0, sizeof cave_prof);
        cave_prof.board           = "cave_pc";
        cave_prof.game            = "deathsmiles2";
        cave_prof.ntransports     = 1;
        cave_prof.t[0].kind       = UTX_SERIAL;
        cave_prof.t[0].mode       = UTX_MODE_PUSH;
        cave_prof.t[0].pace.cycle = 8;   /* live-proven Deathsmiles II setting */
        cave_prof.t[0].pace.gap   = 2;
        utx_install(&cave_prof);
        jvs_diag("%s", utx_status());
    } }
}

/* ============================================================================
 *  Registration / auto-detection
 * ============================================================================
 *  Overrides the weak no-op ultra_board_ops_autoregister() in board_io_ops.c.
 *  Called once from hook_dllmain.c's DllMain, before ultra_jvs_serial_install()
 *  (which is where on_install() above actually fires -- see jvs_serial_hook.c,
 *  already wired: `if (ops && ops->on_install) ops->on_install();`).
 *
 *  Detection replicates EXACTLY the auto-detect logic ultra_jvs_serial_install()
 *  used inline before this extraction (same env var, same exe-size constant):
 *    1. ULTRA_CAVE_DSII=1 -- explicit override (escape hatch for a re-packaged
 *       or patched DSII build whose exe size differs from the dump this was
 *       reverse-engineered against).
 *    2. Exe size == 3,616,768 bytes -- the exact size of the DSII game.exe this
 *       file's VAs/offsets were reverse-engineered against.
 *  Deliberately NOT gated on ULTRA_CAVE_IO (see the "WHAT'S DIFFERENT" note in
 *  the file header for why that would reopen the cross-contamination hole this
 *  extraction exists to close).
 */
static const board_io_ops s_dsii_ops = {
    .name                       = "Cave PC / Deathsmiles II",
    .on_install                 = dsii_on_install,
    .on_create_file             = dsii_on_create_file,
    .on_read_file               = dsii_on_read_file,
    .on_write_file              = dsii_on_write_file,
    .on_close_handle            = dsii_on_close_handle,
    .on_wait_comm_event_pending = dsii_on_wait_comm_event_pending,
    .on_load_library_redirect   = dsii_on_load_library_redirect,
    .autopilot_tick             = NULL,   /* no autopilot script for DSII (yet) */
};

/* Uniquely-named + constructor-attributed: runs automatically at DLL load,
 * before the CRT calls the exported DllMain, with no explicit call site
 * anywhere in the shared generic files. MUST be uniquely named (not the
 * generic "ultra_board_ops_autoregister" this file used before) because a
 * board's build can link MORE THAN ONE game_*.c file into the same binary --
 * a shared well-known symbol name is a link-time collision the moment two
 * game modules share a binary (this was caught for real: game_chaosbreaker.c
 * and game_wackyrace.c both used that name first). See board_io_ops.h's
 * registration section. */
static void __attribute__((constructor)) game_dsii_autoregister(void) {
    int dsii = 0;
    const char *e = getenv("ULTRA_CAVE_DSII");
    if (e && e[0] == '1') dsii = 1;
    if (!dsii) {
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 3616768)
            dsii = 1;
    }
    if (!dsii) return;   /* not Deathsmiles II: stay a no-op */

    ultra_board_ops_register(&s_dsii_ops);
}

#endif /* _WIN32 */
