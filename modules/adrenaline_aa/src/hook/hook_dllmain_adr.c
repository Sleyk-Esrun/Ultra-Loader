/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  hook_dllmain_adr.c -- ultra_adrenaline_aa_hook{32,64}.dll entry point
 * =============================================================================
 *
 *  Adrenaline Amusements's own, fully independent I/O hook. Injected into a Adrenaline Amusements
 *  game (suspended, resumed right after) by modules/adrenaline_aa/src/
 *  adrenaline_aa_main.c. No shared source file, no shared compiled binary with
 *  modules/native_x86/src/hook/'s multi-board hook or any other board's
 *  module -- see hook_adr.h for the full rationale.
 *
 *  On attach it installs, synchronously (the module injects us into the
 *  SUSPENDED game and resumes right after, so hooks must be in place before
 *  the game's first thread runs):
 *    - the JVS-over-serial path (jvs_serial_hook_adr.c -> jvs_core_adr.c)
 *    - the clean-room DirectInput8 emulation (dinput8_emu_adr.c)
 *    - the DirectSound/XAudio2 diagnostic hook (audio_hook_adr.c)
 *  (the D3D9 present/throttle hook arms itself the first time the game's own
 *  Direct3DCreate9 call is intercepted -- see d3d9_throttle_adr.c)
 *  then starts a short re-patch worker to catch any module the game loads
 *  fractionally after our first pass (delay-loaded imports).
 *
 *  Adrenaline Amusements has no real game dump on hand yet (framework-only per
 *  docs/STOCK_TAKE.md), so the local input source below is a plain, direct
 *  keyboard + XInput reader (no per-game profile system is needed or ported --
 *  that whole framework lives in modules/native_x86/src/hook/input_profiles.c
 *  and friends and was correctly left out of this board's independent reference
 *  set). Default mapping (standard MAME-like convention):
 *    Arrows = P1 dpad, LCtrl/LAlt/Space/LShift/Z/X = P1 buttons 1-6,
 *    1 = P1 start, F2 = test, Tab = service, 5/6 = coin slot 1/2.
 *  XInput controller 0 (if connected) drives the same fields: dpad + A/B/X/Y/
 *  LB/RB = buttons 1-6, Start = start, Back = service, RightShoulder = coin.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hook_adr.h"
#include "omni_bridge.h"

#include "ultra_transport.h"   /* shared universal I/O transport layer */
#include "ultra_tracer.h"

/* ---------------------------------------------------------------------------
 *  Managed-DLL file patcher: ULTRA_MANAGED_PATCH
 *
 *  Applies byte patches to Mono/Unity managed assemblies on disk at DllMain
 *  time -- before GameLauncher.exe spawns Game.exe, so Mono sees the patched
 *  IL when it JIT-compiles each method. Completely separate from
 *  ULTRA_PATCH_FILE (which patches loaded native images in memory): this one
 *  edits the actual files.
 *
 *  Format (# comments; one patch per line):
 *      <path-relative-to-cwd>:<hex-file-offset> = <hex bytes>
 *  Example:
 *      Game_Data/Managed/Assembly-CSharp.dll:0x5EDAE = 17
 *
 *  Idempotent: a byte run that already matches the target is skipped silently.
 * ---------------------------------------------------------------------------*/
static void adr_apply_managed_patches(void) {
    char pf[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("ULTRA_MANAGED_PATCH", pf, sizeof pf)) return;
    if (!pf[0]) return;

    FILE *list = fopen(pf, "r");
    if (!list) {
        ultra_adr_log("MANAGED_PATCH: cannot open spec '%s'", pf);
        return;
    }
    ultra_adr_log("MANAGED_PATCH: applying spec '%s'", pf);

    char line[512];
    while (fgets(line, sizeof line, list)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *lhs = line, *rhs = eq + 1;

        char *e = lhs + strlen(lhs);
        while (e > lhs && (unsigned char)e[-1] <= ' ') *--e = '\0';
        while (*lhs == ' ' || *lhs == '\t') ++lhs;

        char *colon = strrchr(lhs, ':');
        if (!colon) continue;
        *colon = '\0';
        char *path_part  = lhs;
        uintptr_t offset = (uintptr_t)strtoull(colon + 1, NULL, 0);

        unsigned char bytes[256];
        size_t nb = 0;
        char *p = rhs;
        while (*p && nb < sizeof bytes) {
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') ++p;
            if (!*p) break;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
            char b2[3] = {p[0], p[1], '\0'};
            bytes[nb++] = (unsigned char)strtoul(b2, NULL, 16);
            p += 2;
        }
        if (!nb) continue;

        FILE *gf = fopen(path_part, "r+b");
        if (!gf) {
            ultra_adr_log("MANAGED_PATCH: cannot open '%s'", path_part);
            continue;
        }
        if (fseek(gf, (long)offset, SEEK_SET) != 0) {
            ultra_adr_log("MANAGED_PATCH: seek failed in '%s' at 0x%lx",
                          path_part, (unsigned long)offset);
            fclose(gf); continue;
        }
        unsigned char existing[256];
        size_t nr = fread(existing, 1, nb, gf);
        if (nr == nb && memcmp(existing, bytes, nb) == 0) {
            ultra_adr_log("MANAGED_PATCH: '%s'+0x%lx already patched",
                          path_part, (unsigned long)offset);
            fclose(gf); continue;
        }
        if (fseek(gf, (long)offset, SEEK_SET) != 0) { fclose(gf); continue; }
        size_t nw = fwrite(bytes, 1, nb, gf);
        fclose(gf);
        ultra_adr_log("MANAGED_PATCH: '%s'+0x%lx <- %zu byte(s)",
                      path_part, (unsigned long)offset, nw);
    }
    fclose(list);
}

/* -------------------------------------------------------------------------- */
/*  Shared diagnostic sink is defined in jvs_serial_hook_adr.c (ultra_adr_log) */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/*  The one JVS node for this process                                          */
/* -------------------------------------------------------------------------- */
static ultra_jvs_node_adr* g_node = NULL;

ultra_jvs_node_adr* ultra_hook_node_adr(void) {
    if (!g_node) {
        g_node = jvs_adr_create();
        ultra_adr_log("JVS: node created (Adrenaline Amusements clean-room engine)");
    }
    return g_node;
}

/* -------------------------------------------------------------------------- */
/*  Local input source: keyboard + XInput controller 0                         */
/* -------------------------------------------------------------------------- */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } adr_xpad;
typedef struct { DWORD packet; adr_xpad pad; } adr_xstate;
typedef DWORD (WINAPI *pXInputGetState)(DWORD, adr_xstate*);

static pXInputGetState s_xget  = NULL;
static int             s_xtried = 0;

static void xinput_try_load(void) {
    if (s_xtried) return;
    s_xtried = 1;
    static const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xget; ++i) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) s_xget = (pXInputGetState)(void*)GetProcAddress(h, "XInputGetState");
    }
}

/* Reads the live local input (keyboard OR'd with XInput pad 0) into a plain
 * ultra_jvs_input_adr. Used by BOTH the JVS engine (via ultra_hook_pull_input_adr)
 * and the DirectInput8 emulation (via ultra_hook_current_input_adr) so both
 * transports agree on what the player is doing. */
void ultra_hook_current_input_adr(ultra_jvs_input_adr* out) {
    memset(out, 0, sizeof(*out));
    for (int a = 0; a < JVS_ADR_MAX_ANALOG; ++a) out->analog[a] = 0x8000;   /* centered */
    out->screen_x = out->screen_y = 0x8000;

    int test = 0, service = 0, start = 0;
    int up = 0, down = 0, left = 0, right = 0;
    int b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    int coin0 = 0, coin1 = 0, coin2 = 0, coin3 = 0;
    /* Gun trigger for the Kaboom sButton Player1B2 bit -- see the big comment
     * below at "out->player[0] |= 0x0002" for why this exists and where it was
     * recovered from. Real VK_LBUTTON state: it is set by a genuine mouse/
     * lightgun click AND by our own OMNI-BRIDGE gamepad path (omni_bridge.c's
     * mouse_set() drives it with real SendInput, not PostMessage), so both
     * input sources are covered by reading this one flag. */
    int fire = 0;

    /* keyboard -- TRA controls: C=P1coin V=P2coin B=P3coin N=P4coin
     *             Numpad1/3/7/9=P1-P4 start  O=service  F2=test */
    fire    |= (GetAsyncKeyState(VK_LBUTTON)  & 0x8000) != 0;
    test    |= (GetAsyncKeyState(VK_F2)       & 0x8000) != 0;
    service |= (GetAsyncKeyState('O')         & 0x8000) != 0;
    start   |= (GetAsyncKeyState(VK_NUMPAD1)  & 0x8000) != 0;
    up      |= (GetAsyncKeyState(VK_UP)       & 0x8000) != 0;
    down    |= (GetAsyncKeyState(VK_DOWN)     & 0x8000) != 0;
    left    |= (GetAsyncKeyState(VK_LEFT)     & 0x8000) != 0;
    right   |= (GetAsyncKeyState(VK_RIGHT)    & 0x8000) != 0;
    b1      |= (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    b2      |= (GetAsyncKeyState(VK_LMENU)    & 0x8000) != 0;
    b3      |= (GetAsyncKeyState(VK_SPACE)    & 0x8000) != 0;
    b4      |= (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) != 0;
    b5      |= (GetAsyncKeyState('Z')         & 0x8000) != 0;
    b6      |= (GetAsyncKeyState('X')         & 0x8000) != 0;
    coin0   |= (GetAsyncKeyState('C')         & 0x8000) != 0;
    coin1   |= (GetAsyncKeyState('V')         & 0x8000) != 0;
    coin2   |= (GetAsyncKeyState('B')         & 0x8000) != 0;
    coin3   |= (GetAsyncKeyState('N')         & 0x8000) != 0;

    /* XInput controllers 0-3 (optional; each player gets their own pad) */
    xinput_try_load();
    if (s_xget) {
        /* P1: controller 0 */
        {
            adr_xstate st; memset(&st, 0, sizeof st);
            if (s_xget(0, &st) == 0) {
                const adr_xpad* p = &st.pad;
                WORD b = p->wButtons;
                up    |= (b & 0x0001) != 0;   /* DPAD_UP */
                down  |= (b & 0x0002) != 0;   /* DPAD_DOWN */
                left  |= (b & 0x0004) != 0;   /* DPAD_LEFT */
                right |= (b & 0x0008) != 0;   /* DPAD_RIGHT */
                start   |= (b & 0x0010) != 0; /* START */
                coin0   |= (b & 0x0020) != 0; /* BACK/SELECT -> coin (UNIVERSAL) */
                test    |= (b & 0x0040) != 0; /* L3 -> test   (UNIVERSAL: TEST=L3) */
                service |= (b & 0x0080) != 0; /* R3 -> service (UNIVERSAL: SERVICE=R3) */
                b1 |= (b & 0x1000) != 0;      /* A */
                b2 |= (b & 0x2000) != 0;      /* B */
                b3 |= (b & 0x4000) != 0;      /* X */
                b4 |= (b & 0x8000) != 0;      /* Y */
                b5 |= (p->bRT > 30);          /* right trigger as button 5 */
                b6 |= (b & 0x0200) != 0;      /* RIGHT_SHOULDER as button 6 */
                out->analog[0] = (uint16_t)(p->lx + 32768);
                out->analog[1] = (uint16_t)(-p->ly + 32767);
                out->analog[2] = (uint16_t)(p->rx + 32768);
                out->analog[3] = (uint16_t)(-p->ry + 32767);
            }
        }
        /* P2: controller 1 */
        {
            adr_xstate st2; memset(&st2, 0, sizeof st2);
            if (s_xget(1, &st2) == 0) {
                const adr_xpad* p = &st2.pad;
                WORD b = p->wButtons;
                uint16_t sw2 = 0;
                if (b & 0x0010) sw2 |= 0x8000; /* START */
                if (b & 0x0080) sw2 |= 0x4000; /* R3 -> SERVICE (UNIVERSAL: SERVICE=R3) */
                if (b & 0x0001) sw2 |= 0x2000; /* DPAD_UP */
                if (b & 0x0002) sw2 |= 0x1000; /* DPAD_DOWN */
                if (b & 0x0004) sw2 |= 0x0800; /* DPAD_LEFT */
                if (b & 0x0008) sw2 |= 0x0400; /* DPAD_RIGHT */
                if (b & 0x1000) sw2 |= 0x0200; /* A -> B1 */
                if (b & 0x2000) sw2 |= 0x0100; /* B -> B2 */
                if (b & 0x4000) sw2 |= 0x0080; /* X -> B3 */
                if (b & 0x8000) sw2 |= 0x0040; /* Y -> B4 */
                if (b & 0x0100) sw2 |= 0x0020; /* LEFT_SHOULDER -> B5 */
                if (p->bRT > 30) sw2 |= 0x0010; /* RT -> B6 */
                coin1 |= (b & 0x0020) != 0;     /* BACK/SELECT -> coin P2 (UNIVERSAL) */
                out->player[1] = sw2;
            }
        }
        /* P3: controller 2 */
        {
            adr_xstate st3; memset(&st3, 0, sizeof st3);
            if (s_xget(2, &st3) == 0) {
                const adr_xpad* p = &st3.pad;
                WORD b = p->wButtons;
                uint16_t sw3 = 0;
                if (b & 0x0010) sw3 |= 0x8000;
                if (b & 0x0020) sw3 |= 0x4000;
                if (b & 0x0001) sw3 |= 0x2000;
                if (b & 0x0002) sw3 |= 0x1000;
                if (b & 0x0004) sw3 |= 0x0800;
                if (b & 0x0008) sw3 |= 0x0400;
                if (b & 0x1000) sw3 |= 0x0200;
                if (b & 0x2000) sw3 |= 0x0100;
                if (b & 0x4000) sw3 |= 0x0080;
                if (b & 0x8000) sw3 |= 0x0040;
                if (b & 0x0100) sw3 |= 0x0020;
                if (p->bRT > 30) sw3 |= 0x0010;
                coin2 |= (b & 0x0200) != 0;
                out->player[2] = sw3;
            }
        }
        /* P4: controller 3 */
        {
            adr_xstate st4; memset(&st4, 0, sizeof st4);
            if (s_xget(3, &st4) == 0) {
                const adr_xpad* p = &st4.pad;
                WORD b = p->wButtons;
                uint16_t sw4 = 0;
                if (b & 0x0010) sw4 |= 0x8000;
                if (b & 0x0020) sw4 |= 0x4000;
                if (b & 0x0001) sw4 |= 0x2000;
                if (b & 0x0002) sw4 |= 0x1000;
                if (b & 0x0004) sw4 |= 0x0800;
                if (b & 0x0008) sw4 |= 0x0400;
                if (b & 0x1000) sw4 |= 0x0200;
                if (b & 0x2000) sw4 |= 0x0100;
                if (b & 0x4000) sw4 |= 0x0080;
                if (b & 0x8000) sw4 |= 0x0040;
                if (b & 0x0100) sw4 |= 0x0020;
                if (p->bRT > 30) sw4 |= 0x0010;
                coin3 |= (b & 0x0200) != 0;
                out->player[3] = sw4;
            }
        }
    }

    if (left)  out->analog[0] = 0x0000; else if (right) out->analog[0] = 0xFFFF;
    if (up)    out->analog[1] = 0x0000; else if (down)  out->analog[1] = 0xFFFF;

    out->system = (uint8_t)((test ? 0x80 : 0) | (0 /* tilt unused */));
    uint16_t sw = 0;
    if (start)   sw |= 0x8000;
    if (service) sw |= 0x4000;
    if (up)      sw |= 0x2000;
    if (down)    sw |= 0x1000;
    if (left)    sw |= 0x0800;
    if (right)   sw |= 0x0400;
    if (b1)      sw |= 0x0200;
    if (b2)      sw |= 0x0100;
    if (b3)      sw |= 0x0080;
    if (b4)      sw |= 0x0040;
    if (b5)      sw |= 0x0020;
    if (b6)      sw |= 0x0010;
    out->player[0] = sw;

    /* ---- Tomb Raider Arcade root-cause fix: Kaboom "sButton" bits ----------
     * This same player[0] word is forwarded VERBATIM as the FE/Adrio5 wire's
     * "sButton" field by fe_adr_host_bytes() (cmd 0x02/0x39/0x40 in
     * jvs_serial_hook_adr.c -- unchanged, no edit needed there). That field is
     * NOT read as the generic JVS switch map documented at the top of
     * jvs_core_adr.h; recovered via ildasm from the pristine
     * Assembly-CSharp.dll.ultrabak
     * (KaboomAdrioFxPlusFourFeedersDllWrapper.KaboomAdrioFxPlusFourFeedersDll/
     * AdrioReadInputTy_unmarshal .ctor), its bits are the Kaboom board's own
     * per-player buttons: bit0=Player1B1 bit1=Player1B2 bit2=Player2B1
     * bit3=Player2B2 bit4=Player3B1 bit5=Player3B2 bit6=Player4B1
     * bit7=Player4B2. Bits 8-15 (the JVS-style bits set above) are never
     * inspected by that decoder, so the two layouts coexist safely in one
     * word -- this board has no other confirmed dump reading player[0] over
     * genuine JVS-over-serial (see UNTESTED.txt), only this FE/Kaboom path.
     *
     * Player1B1 is the exact bit SBKInputManager::GetPlayerKeyDown() and
     * ArcadeManager::CheckForReadyPlayer() poll to mark a player "ready" once
     * a credit is in. Without it, START was never visible on the real
     * hardware path and the "SELECT A MISSION / GET READY!" screen could
     * never advance -- credits could still be added (that path is a separate
     * KaboomManager debug-keyboard shortcut, unaffected). Mirror the existing
     * "start" flag onto bit0.
     *
     * FOLLOW-UP (one level deeper, same class of bug): getting past mission
     * select into gameplay needs the GUN TRIGGER, and on this hardware class
     * the trigger is a SECOND Kaboom button, not something read off the
     * Sixense/STEM tracker. Traced through InputManager::GunControl() (the
     * per-frame Update delegate ChooseInput() installs once GunDetection
     * reports a tracker present, which our sxCore.dll shim always makes
     * true): for each gun index it does
     *     m_Shoot[i] = InputManager.KaboomGun[i] ? true : false
     * every frame, and KaboomGun[] is a plain array TOGGLED ONLY by
     * InputManager::OnKeyEvent(KaboomButton.Button) on
     * BUTTON_PLAYER1_B2_PRESS(=10)/RELEASE(=12) for P1 (14/16, 18/20, 22/24
     * for P2-4) -- i.e. Player1B2, not anything from SixenseCore.Tracker.
     * m_Shoot[] is what InputManager::GunControl() checks before calling
     * PlayerManager::PlayerPressTrigger() (the same call UICrosshair uses to
     * raycast and click a mission icon) and PlayerManager::PlayerShoot(). With
     * bit1 never set, KaboomGun[0] was permanently false, so the trigger was
     * invisible on the real hardware path no matter how the reticle was
     * aimed or how hard the trigger was pulled -- mission-select and normal
     * gameplay firing share this exact same gate. Mirror the existing "fire"
     * flag (real VK_LBUTTON state, see its declaration above) onto bit1.
     *
     * Players 2-4 and their B2 flags are intentionally left at 0: this reply
     * only ever carries player[0] (see the standing comment on cmd 0x02 in
     * jvs_serial_hook_adr.c -- the multi-player wire format is not yet
     * confirmed from a protocol capture), so wiring them here would be
     * guesswork beyond this fix's scope. */
    if (start) out->player[0] |= 0x0001;   /* Kaboom sButton bit0: Player1B1 */
    if (fire)  out->player[0] |= 0x0002;   /* Kaboom sButton bit1: Player1B2 (trigger) */

    /* coin edge-detect + insert straight into the shared node (best-effort:
     * harmless if called before the node exists -- pull happens right after). */
    static int coin0_was = 0, coin1_was = 0, coin2_was = 0, coin3_was = 0;
    if (coin0 && !coin0_was && g_node) jvs_adr_insert_coin(g_node, 0, 1);
    if (coin1 && !coin1_was && g_node) jvs_adr_insert_coin(g_node, 1, 1);
    if (coin2 && !coin2_was && g_node) jvs_adr_insert_coin(g_node, 2, 1);
    if (coin3 && !coin3_was && g_node) jvs_adr_insert_coin(g_node, 3, 1);
    coin0_was = coin0; coin1_was = coin1; coin2_was = coin2; coin3_was = coin3;
}

void ultra_hook_pull_input_adr(ultra_jvs_node_adr* n) {
    if (!n) return;
    ultra_jvs_input_adr in;
    ultra_hook_current_input_adr(&in);   /* also edge-detects+inserts coins above */
    jvs_adr_set_input(n, &in);
}

/* -------------------------------------------------------------------------- */
/*  Presence-only online/live-service auth probe (DEFERRED)                    */
/* -------------------------------------------------------------------------- */
static void adr_online_auth_presence_probe(void) {
    /* Adrenaline Amusements is a Chinese PC arcade board and prior project notes flag a
     * possible live-service/online-auth component for some titles on this
     * family. Per the project owner's standing instruction, that concept is
     * DEFERRED here: this only logs whether a module with a network/auth-
     * looking name is already loaded in the process -- it does NOT probe the
     * network, does NOT answer any handshake, and does NOT implement a bypass.
     * If a real dump surfaces and needs this, the work lands here, isolated to
     * this board. */
    static const char* needles[] = {
        "adrenaline", "wlnet", "netauth", "onlineauth", "license", "activation", NULL
    };
    HMODULE mods[256]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    DWORD count = need / sizeof(HMODULE);
    if (count > 256) count = 256;
    for (DWORD i = 0; i < count; ++i) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH)) continue;
        char low[MAX_PATH]; size_t k = 0;
        for (; name[k] && k < sizeof(low) - 1; ++k) low[k] = (char)tolower((unsigned char)name[k]);
        low[k] = 0;
        for (int j = 0; needles[j]; ++j) {
            if (strstr(low, needles[j])) {
                ultra_adr_log("online-auth: module '%s' matches '%s' -- presence logged, DEFERRED (no implementation)",
                              name, needles[j]);
                break;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Re-patch worker + crash diagnostics                                        */
/* -------------------------------------------------------------------------- */
static volatile LONG g_stop = 0;

static DWORD WINAPI adr_worker(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < 30 && !g_stop; ++i) {
        Sleep(1000);
        ultra_jvs_serial_install_adr();
        ultra_dinput8_install_adr();
        ultra_audio_install_adr();
    }
    return 0;
}

static LONG WINAPI adr_crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "(unknown)";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, MAX_PATH);
        ultra_adr_log("CRASH: ACCESS_VIOLATION at EIP=%p in %s", addr, modname);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* API-agnostic game-window finder. ultra_adr_d3d9_window() returns NULL for
 * D3D11/12 titles (Unity games like Drakons never create a D3D9 device), which
 * left the Omni-Bridge unable to aim and made it bail before sending any
 * buttons/keys. Fall back to this process's own main visible top-level window. */
HWND ultra_adr_d3d9_window(void);
typedef struct { DWORD pid; HWND hwnd; } adr_wnd_ctx;
static BOOL CALLBACK adr_enum_wnd(HWND h, LPARAM lp) {
    adr_wnd_ctx *c = (adr_wnd_ctx*)lp;
    DWORD wp = 0; GetWindowThreadProcessId(h, &wp);
    if (wp != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    char cls[64] = {0}; GetClassNameA(h, cls, sizeof cls);
    if (strcmp(cls, "UltraLoaderSplash") == 0) return TRUE;
    RECT r;
    if (GetClientRect(h, &r) && (r.right - r.left) > 64 && (r.bottom - r.top) > 64) {
        c->hwnd = h; return FALSE;
    }
    return TRUE;
}
static HWND adr_game_window(void) {
    HWND w = ultra_adr_d3d9_window();       /* D3D9 titles: exact device window */
    if (w) return w;
    adr_wnd_ctx c = { GetCurrentProcessId(), NULL };
    EnumWindows(adr_enum_wnd, (LPARAM)&c);  /* D3D11/12/Unity: own main window */
    return c.hwnd;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* Managed DLL patches land BEFORE the tracer and before GameLauncher.exe
         * spawns Game.exe -- Mono gets the patched IL on first load. */
        adr_apply_managed_patches();

        /* FIRST. The hook is injected into a SUSPENDED process, so this
         * runs before the game's entry point: the tracer therefore sees the
         * very first fault and the very first failed load. Observe-only -
         * it swallows no exception and changes no return value.
         * ULTRA_TRACE=0 disables. See work/include/ultra_tracer.h. */
        ultra_tracer_install();


    {   /* Universal transport layer: declare this board's shape so the shared
         * pacing/range/codec core is live and self-describing in the log.
         * Serial boards are REQUEST/RESPONSE -- their reply queue already ends
         * the game's read loop, so pacing is deliberately inert here; injecting
         * empty reads would truncate a reply frame. Behavior is unchanged. */
        static int s_utx_once;
        if (!s_utx_once) { static utx_profile utxp;
            s_utx_once = 1;
            memset(&utxp, 0, sizeof utxp);
            utxp.board        = "adrenaline_aa";
            utxp.ntransports  = 1;
            utxp.t[0].kind    = UTX_SERIAL;
            utxp.t[0].mode    = UTX_MODE_REQRESP;
            utx_install(&utxp);
        }
    }

        DisableThreadLibraryCalls(inst);
        AddVectoredExceptionHandler(1, adr_crash_handler);
        ultra_adr_log("=== ultra_adrenaline_aa_hook.dll attached (Adrenaline Amusements-only, independent) ===");
        ultra_hook_node_adr();
        ultra_jvs_serial_install_adr();   /* JVS transport + D3D9 present hook */
        /* Follow GameLauncher.exe self-relaunches so the hook is present in the
         * instance that actually opens the FE serial port (see child_follow_adr.c).
         * Must run AFTER ultra_jvs_serial_install_adr so ultra_iat_patch_all_adr's
         * infra is live. */
        { extern void ultra_child_follow_install_adr(void); ultra_child_follow_install_adr(); }
        ultra_dinput8_install_adr();
        ultra_audio_install_adr();
        adr_online_auth_presence_probe();
        /* Ultra Omni-Bridge (KBM emulation for raw-input-only game dumps).
         * Opt-in (ULTRA_BRIDGE_ENABLE, default OFF -- see omni_client_read_bridge()),
         * so this is a no-op unless a user/future session turns it on for a
         * confirmed raw-keyboard/mouse Adrenaline Amusements title. Installed after the
         * JVS serial hook above so the D3D9 present hook (which the window
         * getter depends on) is already armed. This board has no lightgun/
         * shared-pose source of its own (see d3d9_throttle_adr.c's header
         * comment), so get_pose is NULL: gamepad-only. */
/* Shared pause/settings overlay -- own self-contained poll thread. */        extern void ultra_pausemenu_install(HWND (*get_hwnd)(void));        extern void ultra_pausemenu_shutdown(void);
        ultra_omni_bridge_install(adr_game_window, NULL);
ultra_pausemenu_install(adr_game_window);
        CreateThread(NULL, 0, adr_worker, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        ultra_omni_bridge_stop();
ultra_pausemenu_shutdown();
        break;
    default: break;
    }
    return TRUE;
}

/* An explicit export so the loader/user can confirm the right DLL was
 * injected (and that it is the independent Adrenaline Amusements-only build). */
__declspec(dllexport) const char* ultra_adrenaline_aa_hook_id(void) {
    return "Ultra Loader Adrenaline Amusements I/O hook (independent, clean-room) v1";
}

#endif /* _WIN32 */
