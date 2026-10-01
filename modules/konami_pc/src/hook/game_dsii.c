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
static HANDLE g_cvkey_fake = INVALID_HANDLE_VALUE; /* CVKEY.222 bypass handle */

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
static int is_cvkey_path(const char *s) {
    if (!s || !*s) return 0;
    /* Substring scan: match CVKEY.222 anywhere in the path, case-insensitive */
    for (const char *p = s; *p; ++p) {
        if ((*p=='C'||*p=='c') &&
            (p[1]=='V'||p[1]=='v') &&
            (p[2]=='K'||p[2]=='k') &&
            (p[3]=='E'||p[3]=='e') &&
            (p[4]=='Y'||p[4]=='y') &&
            p[5]=='.' && p[6]=='2' && p[7]=='2' && p[8]=='2')
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
static DWORD WINAPI Hook_GetIfTable(void* pIfTable, ULONG* pdwSize, BOOL bOrder) {
    /* Real_GetIfTable is resolved in dsii_on_install(); the NULL guard is a
     * deliberate defensive addition beyond the original code (which assumed
     * it was always resolved by the time this could be called) -- cheap
     * insurance against a call ordering surprise, never fires in practice. */
    if (!Real_GetIfTable) return ERROR_NOT_SUPPORTED;
    DWORD r = Real_GetIfTable(pIfTable, pdwSize, bOrder);
    if (r == 0 /* NO_ERROR */ && pIfTable) {
        DWORD* p = (DWORD*)pIfTable; /* first field = dwNumEntries */
        jvs_diag("GetIfTable: cleared %lu NIC entries (NIC-check bypass)", (unsigned long)*p);
        *p = 0;
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
        #define CK(vk) (g_cave_kbd[(unsigned)(vk)&0xFF] || ((GetAsyncKeyState((int)(vk)) & 0x8000) != 0))
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
            if (s_xi) {
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

    unsigned tick = 0;
    uint8_t prev_btn = 0;
    for (;;) {
        Sleep(8); /* ~120 Hz */
        tick++;

        /* Key state: prefer g_cave_kbd[] (RIDEV_INPUTSINK, focus-independent) then
         * fall back to GetAsyncKeyState (works when game is the foreground process). */
        #define CK(vk) (g_cave_kbd[(unsigned)(vk)&0xFF] || ((GetAsyncKeyState((int)(vk)) & 0x8000) != 0))
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
        if (s_xi) {
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
                *p_ab8   = (uint32_t)(uintptr_t)s_cave_com_handle;
                *p_com3h = (uint32_t)(uintptr_t)s_cave_com_handle;
                if (!s_cave_forced_once || (tick % 1800) == 0) {
                    jvs_diag("Cave DSII: force-init [0x774ab8]=%p [0x774ac4]=%p -> CaveRD armed (tick=%u)",
                             (void*)s_cave_com_handle, (void*)s_cave_com_handle, tick);
                    s_cave_forced_once = 1;
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
        if (give >= 56) {
            b[0]='C'; b[1]='V'; b[2]='2'; b[3]='K';
            b[4] = 0x04;   /* version */
            b[5] = 0x00;   /* flags   */
            b[6] = 0x01;   /* board type: CV2000XP */
            b[7] = 0x01;   /* region   */
            for (int i = 8; i < 55; i++) b[i] = (uint8_t)(i ^ 0xA5);
            /* Real board-key reference bytes (read from game.exe VA 0x6260d4 /
             * 0x6260dc / 0x6260e4, all identical) -- overwrite the generic
             * filler at the exact offset the validator compares. */
            static const uint8_t kRefKey[8] = {0x0D,0x01,0x09,0x0E,0x19,0x05,0x09,0x01};
            memcpy(b + 8, kRefKey, sizeof(kRefKey));
            uint8_t cksum = 0;
            for (int i = 0; i < 55; i++) cksum += b[i];
            b[55] = cksum;
        }
        if (out_read) *out_read = give;
        jvs_diag("ReadFile CVKEY.222 req=%lu give=%lu (synth key, ref bytes @8-15)", (unsigned long)want, (unsigned long)give);
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

    /* Focus-independent keyboard: start LL-hook message-pump thread (once only) */
    { static int s_started; if (!s_started) { s_started = 1;
        HANDLE t = CreateThread(NULL, 0, cave_kb_thread, NULL, 0, NULL); if (t) CloseHandle(t); } }

    /* input_poll [ESI+4] guard locks COM3 out after frame 1 -- NOP it, and
     * start the belt-and-suspenders direct-write thread (once only). Unlike
     * the pre-extraction code, no separate exe-size re-check is needed here:
     * ultra_board_ops_autoregister() below already gated OUR registration (and
     * therefore this whole on_install call) on that exact identity check. */
    { static int s_dw; if (!s_dw) { s_dw = 1;
        dsii_nop_input_poll_guard();
        HANDLE t = CreateThread(NULL, 0, cave_direct_write_thread, NULL, 0, NULL);
        if (t) { jvs_diag("Cave DSII: direct-write thread started"); CloseHandle(t); } } }
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
