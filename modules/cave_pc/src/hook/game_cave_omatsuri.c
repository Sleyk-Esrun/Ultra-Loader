/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_cave_omatsuri.c -- Cave PC "Omatsuri Yasan" series board_io_ops
 * =============================================================================
 *
 *  Covers:
 *    Omatsuri Yasan Ganso Takoyaki  (cvgame.exe, 643,072 bytes)
 *    Omatsuri Yasan Kingyo Sukui    (cvgame.exe, 1,642,496 bytes)
 *
 *  Both run on Cave PC (Windows Embedded), open COM3, and use the same
 *  9-byte ASCII serial protocol as Deathsmiles II -- WITHOUT any of DSII's
 *  CVKEY.222 handling, NOP-guard patches, or direct-write belt-and-suspenders
 *  thread. Those are DSII-specific and must NOT be applied here.
 *
 *  COM3 protocol (identical wire format to cave_io_respond in game_dsii.c):
 *    TX 4 bytes: cmd 2A 2A 2A   (cmd byte, three 0x2A filler bytes)
 *    RX 9 bytes: cmd data[8]    (each data byte = 0x30 + bit-field, ASCII-encoded)
 *
 *    cmd 0x40  board-present / init poll:
 *      RX: 40 31 30 30 30 30 30 30 30
 *      rx[1] = 0x31: game decodes version = 1 = "board present".
 *      If rx[1] = 0x30 (version 0) the game fires Error Code 011 and halts.
 *
 *    cmd 0x26  input poll:
 *      RX[0] = 0x26
 *      RX[1] = 0x30 + P1 button byte (ACTIVE-HIGH bit fields):
 *                bit7=START  bit6=TEST  bit5=UP  bit4=DOWN
 *                bit3=LEFT   bit2=RIGHT bit1=B1  bit0=B2
 *      RX[2] = 0x30 + system byte (ACTIVE-HIGH):
 *                bit7=B3  bit0=COIN
 *      RX[3..8] = 0x30  (idle channels)
 *
 *    cmd 0x25  campaign/print-peripheral command, e.g. TX "25 31 36 30"
 *      ("%160" in ASCII) -- Ganso Takoyaki live JVS log, 2026-09-24.
 *      Confirmed by disassembling cvgame.exe (function at VA 0x402c70,
 *      Takoyaki build): a "%NN" ASCII command like this one is built into a
 *      per-object queue (VA 0x402c18/0x4030a3) and, once queued, THIS EXACT
 *      TICK drains it with a bare WriteFile loop (VA 0x402d46-0x402d9b) and
 *      returns WITHOUT ever calling ReadFile in the same pass -- the standard
 *      4-then-9-byte poll cycle (WriteFile cmd+2A2A2A, ReadFile 4, ReadFile 9,
 *      VA 0x402da0-0x402f3c) only runs on a LATER tick once the queue is
 *      empty. In other words cmd 0x25 is fire-and-forget: the game never
 *      reads a reply to it.
 *      An earlier version of this file treated 0x25 as a board-init-poll
 *      variant and queued a 9-byte ack (rx[1]=0x31) for it, same as cmd 0x40.
 *      That desyncs the single-slot response queue in omatsuri_on_read_file:
 *      the unread 9-byte frame sits at the front and gets handed back on the
 *      NEXT ReadFile call, which the game issues expecting the reply to its
 *      real 0x40/0x26 poll -- shifting every subsequent button/analog read by
 *      9 bytes, permanently. Queue NOTHING for cmd 0x25 (see
 *      omatsuri_build_frame / omatsuri_on_write_file below).
 *
 *  Registration: auto-detected by exe size. No env-var required for normal
 *  play; ULTRA_CAVE_OMATSURI=1 is an escape hatch for repacked/patched builds.
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "board_io_ops.h"
#include "jvs_core.h"

/* From hook_dllmain.c -- non-static/exported. */
extern ultra_jvs_node* ultra_hook_node(void);
extern void             ultra_hook_pull_input(ultra_jvs_node* n);

/* ============================================================================
 *  Response queue -- single-slot, same pattern as game_dsii.c.
 *  on_write_file builds and enqueues the 9-byte reply;
 *  on_read_file drains it one ReadFile call at a time.
 * ============================================================================ */
static uint8_t          s_resp_buf[32];
static volatile LONG    s_resp_len = 0;
static volatile LONG    s_resp_pos = 0;
static CRITICAL_SECTION s_resp_lock;
static int              s_resp_lock_init = 0;

/* The single COM3 handle this game's I/O protocol lives on. Set by
 * omatsuri_on_write_file (jvs_serial_hook.c only invokes on_write_file from
 * inside its own `com_slot* s = slot_for(h); if (s) {...}` block, so `h` is
 * ALREADY confirmed to be a real fake-COM handle whenever we see it there --
 * see the identical comment on dsii_on_write_file in game_dsii.c).
 *
 * REGRESSION FIX (2026-09-24): omatsuri_on_read_file used to ignore `h`
 * entirely and unconditionally intercept every ReadFile in the process --
 * jvs_serial_hook.c's Hook_ReadFile calls ops->on_read_file FIRST, before it
 * even looks at whether `h` is one of its own com_slots, so this swallowed
 * every real asset load (textures, campaign bmps, config files, ...) in the
 * whole game, handing back an empty/garbage read instead of real file bytes.
 * That is strictly worse than having no Omatsuri-specific code linked in at
 * all (the Sep-21 baseline), which is exactly what live testing showed: the
 * Sep-21 build got zero JVS I/O responses but still rendered real gameplay,
 * while this build's asset loads were being hijacked and both titles failed
 * harder (Takoyaki operator-error "blue screen", Kingyo Sukui instant crash).
 * Fix: only ever intercept reads on the one handle this game's own COM3
 * write actually created, exactly like game_dsii.c's s_cave_com_handle/
 * g_cvkey_fake checks already do -- everything else must fall through to the
 * generic path (return 0) unmodified. */
static HANDLE s_cave_com_handle = INVALID_HANDLE_VALUE;

/* ============================================================================
 *  Build a 9-byte ASCII response frame.
 * ============================================================================ */
static size_t omatsuri_build_frame(const uint8_t* tx, size_t tx_len,
                                   uint8_t* rx, size_t rx_cap)
{
    if (tx_len < 1 || rx_cap < 9) return 0;
    uint8_t cmd = tx[0];

    /* Default: echo cmd, neutral 0x30 data. */
    rx[0] = cmd;
    rx[1] = 0x30; rx[2] = 0x30; rx[3] = 0x30;
    rx[4] = 0x30; rx[5] = 0x30; rx[6] = 0x30;
    rx[7] = 0x30; rx[8] = 0x30;

    if (cmd == 0x25) {
        /* Campaign/print-peripheral command ("%" + ASCII param, e.g. "%160").
         * Fire-and-forget -- see the file header comment. Queue NOTHING: the
         * game never issues a matching ReadFile for this write, so any frame
         * queued here would sit unread and desync the next legitimate 0x26/
         * 0x40 reply by 9 bytes. */
        return 0;
    }

    if (cmd == 0x40) {
        /* Board-present poll. rx[1] = 0x31 (version 1) signals "I/O board found."
         * 0x31 - 0x30 = 1 after the game's SUB 0x30 decode. Without this,
         * Error Code 011 fires and the game refuses to proceed. */
        rx[1] = 0x31;
        return 9;
    }

    if (cmd == 0x26) {
        /* Input poll. Build the button byte from the JVS/controls layer. */
        ultra_jvs_node* node = ultra_hook_node();
        ultra_hook_pull_input(node);
        ultra_jvs_input in;
        jvs_get_input(node, &in);

        /* JVS player[0] high byte bit layout (controls.c convention):
         *   bit7=START bit6=SVC bit5=UP bit4=DOWN bit3=LEFT bit2=RIGHT bit1=BTN1 bit0=BTN2
         * Cave COM3 byte 0 bit layout matches JVS high byte exactly:
         *   bit7=START bit6=TEST bit5=UP bit4=DOWN bit3=LEFT bit2=RIGHT bit1=B1  bit0=B2  */
        uint16_t p  = in.player[0];
        uint8_t  hi = (uint8_t)(p >> 8);
        uint8_t  lo = (uint8_t)(p & 0xFF);

        uint8_t cave_btn = 0;
        if (hi & 0x80) cave_btn |= 0x80; /* START */
        if (hi & 0x40) cave_btn |= 0x40; /* SVC/TEST */
        if (hi & 0x20) cave_btn |= 0x20; /* UP */
        if (hi & 0x10) cave_btn |= 0x10; /* DOWN */
        if (hi & 0x08) cave_btn |= 0x08; /* LEFT */
        if (hi & 0x04) cave_btn |= 0x04; /* RIGHT */
        if (hi & 0x02) cave_btn |= 0x02; /* B1 */
        if (hi & 0x01) cave_btn |= 0x01; /* B2 */

        uint8_t cave_sys = 0;
        if (lo & 0x80) cave_sys |= 0x80; /* B3 */
        /* COIN is asserted via keyboard '5' / XInput BACK below */

        /* Keyboard fallback: arrows + Z/X/C/1/5/F2 (same assignments as DSII). */
        if (GetAsyncKeyState(VK_UP)    & 0x8000) cave_btn |= 0x20;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) cave_btn |= 0x10;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) cave_btn |= 0x08;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) cave_btn |= 0x04;
        if (GetAsyncKeyState('Z')      & 0x8000) cave_btn |= 0x02;
        if (GetAsyncKeyState('X')      & 0x8000) cave_btn |= 0x01;
        if (GetAsyncKeyState('C')      & 0x8000) cave_sys |= 0x80;
        if (GetAsyncKeyState('1')      & 0x8000) cave_btn |= 0x80;
        if (GetAsyncKeyState('5')      & 0x8000) cave_sys |= 0x01;
        if (GetAsyncKeyState(VK_F2)    & 0x8000) cave_btn |= 0x40;

        /* XInput gamepad support (same mappings as game_dsii.c). */
        {
            typedef DWORD (WINAPI *pfxi_t)(DWORD, void*);
            static pfxi_t s_xi; static int s_xi_init;
            if (!s_xi_init) {
                s_xi_init = 1;
                HMODULE h = LoadLibraryA("xinput1_4.dll");
                if (!h) h = LoadLibraryA("xinput1_3.dll");
                if (h) s_xi = (pfxi_t)(void*)GetProcAddress(h, "XInputGetState");
            }
            if (s_xi) {
                struct { DWORD pn; struct { WORD w; BYTE lt, rt; SHORT lx, ly, rsx, rsy; } gp; } xs = {0};
                if (s_xi(0, &xs) == 0) {
                    WORD xb = xs.gp.w; SHORT xlx = xs.gp.lx, xly = xs.gp.ly;
                    if ((xb & 0x0001) || xly >  8000) cave_btn |= 0x20; /* DU/LY+ → UP    */
                    if ((xb & 0x0002) || xly < -8000) cave_btn |= 0x10; /* DD/LY- → DOWN  */
                    if ((xb & 0x0004) || xlx < -8000) cave_btn |= 0x08; /* DL/LX- → LEFT  */
                    if ((xb & 0x0008) || xlx >  8000) cave_btn |= 0x04; /* DR/LX+ → RIGHT */
                    if (xb & 0x1000)                  cave_btn |= 0x02; /* A → B1          */
                    if (xb & 0x2000)                  cave_btn |= 0x01; /* B → B2          */
                    if (xb & 0x4000)                  cave_sys |= 0x80; /* X → B3          */
                    if (xb & 0x0010)                  cave_btn |= 0x80; /* START           */
                    if ((xb & 0x0020) || (xb & 0x0100)) cave_sys |= 0x01; /* BACK/LB → COIN */
                }
            }
        }

        rx[1] = (uint8_t)(0x30 + cave_btn);
        rx[2] = (uint8_t)(0x30 + cave_sys);
        return 9;
    }

    return 9; /* unknown cmd: neutral frame is safe */
}

/* ============================================================================
 *  board_io_ops callbacks
 * ============================================================================ */

static void omatsuri_on_install(void)
{
    InitializeCriticalSection(&s_resp_lock);
    s_resp_lock_init = 1;
    fprintf(stderr, "[cave_omatsuri] COM3 I/O handler installed\n");
    fflush(stderr);
}

static int omatsuri_on_write_file(HANDLE h, const void* buf, DWORD len)
{
    /* Remember which handle is "our" live Cave COM3 handle, exactly like
     * dsii_on_write_file does -- see s_cave_com_handle's declaration above
     * for why this is safe to do unconditionally here. */
    s_cave_com_handle = h;
    if (!buf || len < 1) return 0;

    uint8_t cmd = ((const uint8_t*)buf)[0];
    uint8_t frame[16];
    size_t got = omatsuri_build_frame((const uint8_t*)buf, (size_t)len,
                                      frame, sizeof(frame));
    if (!got) {
        /* cmd 0x25 (and any other tx_len<1 edge case) is recognized but
         * fire-and-forget: consume the write, queue no reply, and do NOT
         * fall through to the generic path (return 0 would let the real
         * jvs_host_bytes() engine also try to interpret this write). */
        return (cmd == 0x25) ? 1 : 0;
    }

    if (!s_resp_lock_init) { InitializeCriticalSection(&s_resp_lock); s_resp_lock_init = 1; }
    EnterCriticalSection(&s_resp_lock);
    /* Compact any already-consumed bytes before appending. */
    if (s_resp_pos > 0) {
        LONG rem = s_resp_len - s_resp_pos;
        if (rem > 0) memmove(s_resp_buf, s_resp_buf + s_resp_pos, (size_t)rem);
        else rem = 0;
        s_resp_len = rem;
        s_resp_pos = 0;
    }
    LONG avail = (LONG)sizeof(s_resp_buf) - s_resp_len;
    if (avail > 0) {
        if ((LONG)got > avail) got = (size_t)avail;
        memcpy(s_resp_buf + s_resp_len, frame, got);
        s_resp_len += (LONG)got;
    }
    LeaveCriticalSection(&s_resp_lock);
    return 1; /* fully handled -- response enqueued */
}

static int omatsuri_on_read_file(HANDLE h, void* buf, DWORD want, DWORD* out_read)
{
    /* Only ever intercept reads on the one handle this game's own COM3
     * write created -- everything else (texture/bmp/config loads, any other
     * file the game opens) must fall through to the generic path untouched.
     * See s_cave_com_handle's declaration above for the regression this
     * fixes: this used to ignore `h` and intercept EVERY ReadFile in the
     * process. */
    if (h == INVALID_HANDLE_VALUE || h != s_cave_com_handle) return 0;
    if (!s_resp_lock_init) { *out_read = 0; return 1; }
    EnterCriticalSection(&s_resp_lock);
    LONG avail = s_resp_len - s_resp_pos;
    LONG give  = (avail < (LONG)want) ? avail : (LONG)want;
    if (give > 0) {
        memcpy(buf, s_resp_buf + s_resp_pos, (size_t)give);
        s_resp_pos += give;
    }
    LeaveCriticalSection(&s_resp_lock);
    *out_read = (DWORD)(give > 0 ? give : 0);
    return 1; /* our own COM3 handle: always intercept */
}

static int omatsuri_on_wait_comm_event_pending(void)
{
    return 1; /* push-mode: signal EV_RXCHAR even when our queue is empty */
}

/* Windows recycles handle values -- clear our cached COM3 handle the moment
 * it's closed so a later, unrelated handle that happens to reuse the same
 * value is never mistaken for it. Mirrors dsii_on_close_handle's identical
 * s_cave_com_handle guard in game_dsii.c. Must no-op cleanly for every other
 * handle (the vast majority of closes in the process). */
static void omatsuri_on_close_handle(HANDLE h)
{
    if (h == s_cave_com_handle) s_cave_com_handle = INVALID_HANDLE_VALUE;
}

/* Kingyo Sukui only: block all rqrdlist.rqr opens so the game falls
 * through to the "no campaign server" (Error 009) non-fatal path.
 * The game reads its own Media\rqrdlist.rqr, finds an expired campaign
 * license, and calls ExitProcess(8).  When blocked, it enters the same
 * "no server found" mode Takoyaki uses (no rqrdlist.rqr in that dump)
 * and continues showing Error 009 as a non-fatal overlay.
 *
 * MUST be wired to BOTH on_create_file and on_create_file_pre_redirect
 * (see s_kingyo_ops below). The game scans every drive letter C: through
 * Z: for this file. Drive letters in the loader's default cabinet-redirect
 * list ("DEFGHJ") never reach plain on_create_file at all -- the generic
 * redirect in jvs_serial_hook.c serves them a real file out of
 * ULTRA_GAMEDATA first. Confirmed live 2026-09-24: the loader's gamedata
 * cache for this game already held a genuine (license-expired) rqrdlist.rqr
 * + campaign01/02.bmp -- copied there, with their original 1996 arcade-dump
 * timestamps preserved, by an earlier attempt at this same fix -- and
 * d:\rqrdlist.rqr was observed being REDIRECTed straight to it, bypassing
 * this block entirely and reproducing the exact ExitProcess(8) it was meant
 * to prevent. on_create_file alone is still needed for drive letters the
 * redirect list does NOT claim (C:, I:, K:..Z:). */
static int kingyo_on_create_file(const char* path, HANDLE* out_handle)
{
    if (!path) return 0;
    size_t len = strlen(path);
    static const char needle[] = "rqrdlist.rqr";
    static const size_t nlen   = sizeof(needle) - 1;
    if (len >= nlen && _stricmp(path + len - nlen, needle) == 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        *out_handle = INVALID_HANDLE_VALUE;
        return 1;
    }
    return 0;
}

/* ============================================================================
 *  Registration -- separate ops structs: Takoyaki has no rqrdlist.rqr
 *  in its dump (safe), Kingyo has one with an expired license (blocked).
 * ============================================================================ */

static const board_io_ops s_takoyaki_ops = {
    .name                       = "Cave PC / Omatsuri Yasan (Takoyaki)",
    .on_install                 = omatsuri_on_install,
    .on_create_file             = NULL,
    .on_read_file               = omatsuri_on_read_file,
    .on_write_file              = omatsuri_on_write_file,
    .on_close_handle            = omatsuri_on_close_handle,
    .on_wait_comm_event_pending = omatsuri_on_wait_comm_event_pending,
    .on_load_library_redirect   = NULL,
    .autopilot_tick             = NULL,
};

static const board_io_ops s_kingyo_ops = {
    .name                       = "Cave PC / Omatsuri Yasan (Kingyo Sukui)",
    .on_install                 = omatsuri_on_install,
    .on_create_file             = kingyo_on_create_file,
    .on_create_file_pre_redirect = kingyo_on_create_file,
    .on_read_file               = omatsuri_on_read_file,
    .on_write_file              = omatsuri_on_write_file,
    .on_close_handle            = omatsuri_on_close_handle,
    .on_wait_comm_event_pending = omatsuri_on_wait_comm_event_pending,
    .on_load_library_redirect   = NULL,
    .autopilot_tick             = NULL,
};

/* Known cvgame.exe sizes for the two Omatsuri Yasan titles. */
#define EXE_SZ_TAKOYAKI   643072UL   /* Omatsuri Yasan Ganso Takoyaki */
#define EXE_SZ_KINGYO    1642496UL   /* Omatsuri Yasan Kingyo Sukui   */

/* Uniquely named constructor -- runs at DLL load before DllMain, no explicit
 * call site needed anywhere in the shared generic files. See board_io_ops.h. */
static void __attribute__((constructor)) game_cave_omatsuri_autoregister(void)
{
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa) || fa.nFileSizeHigh != 0)
        return;
    DWORD sz = fa.nFileSizeLow;

    if (sz == EXE_SZ_TAKOYAKI) {
        ultra_board_ops_register(&s_takoyaki_ops);
        fprintf(stderr, "[cave_omatsuri] Takoyaki registered (exe %lu bytes)\n",
                (unsigned long)sz);
        fflush(stderr);
    } else if (sz == EXE_SZ_KINGYO) {
        ultra_board_ops_register(&s_kingyo_ops);
        fprintf(stderr, "[cave_omatsuri] Kingyo Sukui registered (exe %lu bytes)\n",
                (unsigned long)sz);
        fflush(stderr);
    }
}

#endif /* _WIN32 */
