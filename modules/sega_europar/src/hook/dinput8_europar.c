/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  dinput8_europar.c -- Sega Europa-R (Ford Racing) DEDICATED DirectInput8 proxy.
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS (2026-07-23 investigation, see also docs/STOCK_TAKE.md
 *  and docs/X86_BOARDS.md "Sega Europa-R" rows):
 *
 *  Ford Racing never responded to ANY input (not even START) through the
 *  shared native_x86/src/hook/ JVS/IO4 layer, even after that layer's board-id
 *  and axis fixes were applied. Root-caused by reverse-engineering the ACTUAL
 *  game binary instead of guessing:
 *
 *    1. `objdump -p fordracing.exe` shows NO static import of dinput8.dll,
 *       xinput*.dll, or any JVS/serial API. It statically imports HID.DLL +
 *       SETUPAPI.dll, which turned out to be a RED HERRING: those back a
 *       separate, unrelated "HAPP API" real-cabinet-I/O-board path (see the
 *       embedded strings "Failed InitHappAPI", "SET HARDWARE REV",
 *       "READ EEPROM", "COIN DROP", "WATCHDOG", "TRACKBALL" -- a HAPP Controls
 *       arcade I/O board, NOT Sega's own IO4 board, and NOT the game's normal
 *       driving controls).
 *    2. The game's REAL wheel/pedal/button input goes through DirectInput8,
 *       loaded DYNAMICALLY: `strings fordracing.exe` contains the literal
 *       tokens "DirectInput8Create" and "dinput8.dll" right next to
 *       ".\Input\Pc\Joystick.c" / "Joystick System" / "Failed to create
 *       DirectInput Object" -- i.e. the game does
 *       `GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create")`
 *       itself, at runtime, instead of statically importing it.
 *    3. THIS is exactly why the shared hook's dinput8_emu.c never fired: its
 *       `ultra_iat_patch_all("dinput8.dll","DirectInput8Create",...)` (see
 *       jvs_serial_hook.c) only overwrites *existing* static IAT slots in
 *       *already-loaded* modules -- fordracing.exe has no such slot (proven by
 *       the objdump above), and the shared hook's LoadLibraryA hook only
 *       redirects dinput8.dll loads for the Cave PC board
 *       (`cave_io_active()` gate in Hook_LoadLibraryA) -- never for
 *       sega_europar. So no matter what ULTRA_BOARD/ULTRA_CONTROLS/ULTRA_IO4
 *       knobs were set, the synthetic DirectInput8Create hook was
 *       *structurally unreachable* for this game. That is the "diagnostic
 *       that should confirm the input layer activated never appears" anomaly
 *       from the 2026-07-23 test notes -- fully explained, not mysterious.
 *    4. The game's OWN bundled controller-mapping data file,
 *       DATA/SA_1_EU_ASCII/CTRLLERS/CTRLLERS.CDB (read directly from this
 *       project's Ford Racing dump -- not a third-party source), lists
 *       exactly 4 recognized DirectInput joysticks by the low 32 bits of
 *       their DirectInput `guidProduct` (decimal, one BEGIN: block each):
 *         3003384911 (0xB304044F) Thrustmaster Firestorm Dual Power 3 Gamepad
 *         3255567469 (0xC20C046D) Logitech Wingman Precision USB Gamepad
 *         3256222829 (0xC216046D) Logitech Dual Action USB Gamepad
 *         3255698541 (0xC20E046D) Logitech Formula GP Racing Wheel
 *       (sanity check: the low 16 bits of the 3 Logitech entries are all
 *       0x046D -- Logitech's real USB vendor ID -- and the Thrustmaster
 *       entry's low 16 bits are 0x044F -- Thrustmaster's real USB vendor ID;
 *       both independently confirm Data1 = (product_id<<16)|vendor_id and
 *       that these are genuine consumer peripheral IDs the original
 *       developers hard-coded, not anything sourced from a third party here).
 *       A joystick whose guidProduct does not hash to one of those 4 values
 *       has NO axis/button mapping the game knows how to use -- "Calling
 *       model_joystick_steering with an unsupported controller_type %d" is a
 *       literal string in the binary. The shared hook's synthetic joystick
 *       (dinput8_emu.c) reports an all-zero guidProduct, which cannot match
 *       any of the 4 -- a SECOND, independent reason that path could never
 *       have driven the car even if it had been reachable at all.
 *
 *  THE FIX: don't fight the shared hook's IAT-patch model at all. Ford
 *  Racing loads "dinput8.dll" by bare name (no path) via LoadLibraryA, and
 *  Win32's DLL search order checks the application directory BEFORE
 *  System32. Dropping a same-named replacement DLL directly into the game's
 *  own folder (before CreateProcess, exactly like
 *  konami_pc/src/dinput8_crk.c's already-proven `dinput8.CRK.dll` ->
 *  `dinput8.dll` swap-in technique -- read as a reference for the technique,
 *  not reused code) intercepts the load with ZERO process injection, ZERO
 *  IAT patching, and ZERO dependency on native_x86/src/hook/*.
 *
 *  This DLL:
 *    - Impersonates ONE joystick whose guidProduct matches the "Logitech
 *      Formula GP Racing Wheel" entry above (chosen because it's the only
 *      wheel-class -- not gamepad-class -- entry, and its axis layout,
 *      steer on a single X axis + a single shared accelerator/brake slider,
 *      is exactly a wheel+combo-pedal cabinet control scheme):
 *        STEER_LEFT/STEER_RIGHT (XAXIS)     -> DIJOYSTATE.lX
 *        ACCELERATOR/BRAKE      (SLIDER1)   -> DIJOYSTATE.rglSlider[0]
 *        GEAR_DOWN/GEAR_UP                  -> BUTTON0 / BUTTON1
 *        LOOK_BEHIND/HANDBRAKE              -> BUTTON2 / BUTTON3
 *        CHANGE_VIEW/BOOST                  -> BUTTON4 / BUTTON5
 *      fed from real keyboard (arrows/WASD + Z/X/V/Space/C/LeftShift) and,
 *      if connected, a real XInput pad (left stick + triggers + face
 *      buttons) as an alternate source.
 *    - Forwards the REAL keyboard 1:1 (every DIK_* scan code reflects the
 *      corresponding physical key's actual GetAsyncKeyState) so menu
 *      navigation / START / credits -- whatever key the game's own PC input
 *      scheme actually wants (".\Input\Pc\Keyboard.c" polls a DirectInput
 *      keyboard device, not window messages) -- just works without us having
 *      to guess which single key means "start". This directly answers the
 *      "not even START responds" mystery: we are not trying to synthesize a
 *      specific JVS-style START chord at all, we are a transparent keyboard.
 *    - Forwards a minimal real mouse (delta position + 3 buttons) since
 *      ".\Input\Pc\Mouse.c" also creates a DirectInput mouse device and an
 *      always-DI_OK-but-uninitialised one is needless risk.
 *
 *  Clean-room: Ultra Loader's own code over the public DirectInput8/XInput
 *  Win32 APIs plus facts read directly out of this project's own Ford Racing
 *  dump (its PE import table and its own bundled CTRLLERS.CDB data file).
 *  No third-party arcade I/O emulator was referenced.
 * =============================================================================
 */

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define LOG(...) do { fprintf(stderr, "[sega_europar_di8] " __VA_ARGS__); fflush(stderr); } while (0)

/* 2026-08-05: user directive -- stop guessing at DirectInput plumbing,
 * examine what fordracing.exe itself actually does with our data. Ford
 * Racing is a non-ASLR 32-bit exe with a fixed ImageBase (confirmed via
 * `objdump -p fordracing.exe` -> ImageBase 00400000) -- no live-scriptable
 * debugger is installed (no cdb/windbg/x64dbg; IDA Freeware 7.0 here has no
 * batch idat.exe), but a captured return address from a LIVE call maps
 * directly onto a static disassembly of the same, unmodified exe file, with
 * zero translation needed. __builtin_return_address(0) reads the calling
 * instruction's address directly off the stack (reliable at this level
 * regardless of frame-pointer omission), letting `objdump -d
 * --start-address=<addr>` show exactly what game code called us and what it
 * does with the result immediately after. Logged once per distinct call
 * site (not per call) so a whole play session doesn't spam identical lines. */
static void *s_seen_callers[32];
static int   s_seen_caller_count = 0;

/* 2026-08-05 addition: a first pass just logged the raw address, which
 * turned out to live outside BOTH fordracing.exe's own 0x00400000-based
 * range AND either of its two bundled DLLs (dinput8.dll, inpout32.dll) --
 * and its static import table has no third-party engine DLL at all, only
 * ordinary Windows system DLLs. Resolve the actual module via
 * GetModuleHandleExA(FROM_ADDRESS) + GetModuleFileNameA instead of guessing
 * which file that address belongs to.
 *
 * 2026-08-05 SECOND fix (self-caught, confirmed by disassembling our own
 * just-built DLL): the module resolved to "dinput8.dll" itself every time --
 * this function used to call __builtin_return_address(0) INSIDE ITSELF,
 * which returns THIS function's own return address (i.e. the instruction in
 * whichever d_* wrapper called log_caller_once), not that wrapper's OWN
 * caller. `objdump -d` on the exact logged offset (translated from the
 * runtime-rebased VA back to this DLL's declared ImageBase) landed squarely
 * on the instruction right after "call log_caller_once" inside d_Acquire --
 * proof, not a guess. Fixed by capturing __builtin_return_address(0) AT EACH
 * CALL SITE (inline in every wrapper function, see below) and passing it in,
 * so level 0 is always the actual game caller, with no dependency on
 * frame-pointer chain walking. */
static void log_caller_once(const char *what, void *ra) {
    for (int i = 0; i < s_seen_caller_count; ++i) if (s_seen_callers[i] == ra) return;
    if (s_seen_caller_count >= 32) return;   /* defensive cap */
    s_seen_callers[s_seen_caller_count++] = ra;

    HMODULE hmod = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)ra, &hmod) && hmod) {
        if (!GetModuleFileNameA(hmod, path, sizeof path)) strncpy(path, "?", sizeof path - 1);
    }
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    LOG("%s called from %s+0x%08lX (VA=0x%08lX, module base=0x%08lX)\n",
        what, base, (unsigned long)((uintptr_t)ra - (uintptr_t)hmod),
        (unsigned long)(uintptr_t)ra, (unsigned long)(uintptr_t)hmod);
}

/* =============================================================================
 *  Real keyboard forwarding: DIK_* scan code -> VK_* virtual key.
 *
 *  Built mostly by MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK), which correctly
 *  resolves the whole non-extended block (letters, digits, F-keys, numpad,
 *  punctuation) with zero hand-written entries. The exception is the
 *  "extended" (E0-prefixed) keys: DirectInput folds the E0 prefix into the
 *  scan code by setting the high bit (base_code | 0x80), a convention
 *  MapVirtualKeyA's plain MAPVK_VSC_TO_VK does not know about (it wants
 *  Windows' own extended-flag encoding instead). Left uncorrected, DIK_UP
 *  (0xC8) reduces to base scancode 0x48 and MapVirtualKeyA reports
 *  VK_NUMPAD8, not VK_UP. Those ~20 extended keys are corrected explicitly
 *  below; everything else comes from the generic pass.
 * ============================================================================= */
static BYTE g_dik_to_vk[256];
static int  g_dik_vk_ready = 0;

static void init_dik_vk_table(void) {
    if (g_dik_vk_ready) return;
    for (int i = 0; i < 256; ++i) {
        UINT vk = MapVirtualKeyA((UINT)i, MAPVK_VSC_TO_VK);
        g_dik_to_vk[i] = (vk <= 0xFF) ? (BYTE)vk : 0;
    }
    g_dik_to_vk[DIK_UP & 0xFF]          = (BYTE)VK_UP;
    g_dik_to_vk[DIK_DOWN & 0xFF]        = (BYTE)VK_DOWN;
    g_dik_to_vk[DIK_LEFT & 0xFF]        = (BYTE)VK_LEFT;
    g_dik_to_vk[DIK_RIGHT & 0xFF]       = (BYTE)VK_RIGHT;
    g_dik_to_vk[DIK_HOME & 0xFF]        = (BYTE)VK_HOME;
    g_dik_to_vk[DIK_END & 0xFF]         = (BYTE)VK_END;
    g_dik_to_vk[DIK_PRIOR & 0xFF]       = (BYTE)VK_PRIOR;
    g_dik_to_vk[DIK_NEXT & 0xFF]        = (BYTE)VK_NEXT;
    g_dik_to_vk[DIK_INSERT & 0xFF]      = (BYTE)VK_INSERT;
    g_dik_to_vk[DIK_DELETE & 0xFF]      = (BYTE)VK_DELETE;
    g_dik_to_vk[DIK_NUMPADENTER & 0xFF] = (BYTE)VK_RETURN;
    g_dik_to_vk[DIK_RCONTROL & 0xFF]    = (BYTE)VK_CONTROL;
    g_dik_to_vk[DIK_RMENU & 0xFF]       = (BYTE)VK_MENU;
    g_dik_to_vk[DIK_LWIN & 0xFF]        = (BYTE)VK_LWIN;
    g_dik_to_vk[DIK_RWIN & 0xFF]        = (BYTE)VK_RWIN;
    g_dik_to_vk[DIK_APPS & 0xFF]        = (BYTE)VK_APPS;
    /* Left-hand modifiers: prefer the unambiguous L-variant VK over whatever
     * the generic pass produced, so held state reads correctly per side. */
    g_dik_to_vk[DIK_LSHIFT & 0xFF]      = (BYTE)VK_LSHIFT;
    g_dik_to_vk[DIK_RSHIFT & 0xFF]      = (BYTE)VK_RSHIFT;
    g_dik_to_vk[DIK_LCONTROL & 0xFF]    = (BYTE)VK_LCONTROL;
    g_dik_to_vk[DIK_LMENU & 0xFF]       = (BYTE)VK_LMENU;
    g_dik_vk_ready = 1;
}

/* fill a caller-supplied, already-zeroed 256-byte DirectInput keyboard state
 * buffer from the REAL physical keyboard (or SendInput-synthesized presses,
 * since GetAsyncKeyState reads the same OS-level key-state table either way). */
static void fill_keyboard_state(BYTE *kb) {
    init_dik_vk_table();
    for (int i = 0; i < 256; ++i) {
        BYTE vk = g_dik_to_vk[i];
        if (vk && (GetAsyncKeyState(vk) & 0x8000)) kb[i] = 0x80;
    }
}

/* =============================================================================
 *  Synthetic "Logitech Formula GP Racing Wheel" joystick.
 * ============================================================================= */
#define EUROPAR_WHEEL_GUIDPRODUCT_DATA1  0xC20E046Du   /* from CTRLLERS.CDB; see header */

static const GUID g_wheel_product = {
    EUROPAR_WHEEL_GUIDPRODUCT_DATA1, 0x0000, 0x0000,
    { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 }   /* "..PIDVID", standard DI HID convention */
};
static const GUID g_wheel_instance = {
    0x554c5445, 0x5250, 0x4157,
    { 0x45, 0x55, 0x52, 0x4f, 0x50, 0x41, 0x52, 0x31 }   /* arbitrary but fixed/stable */
};

/* Flip either of these to 1 if live verification shows an axis is backwards. */
#define EUROPAR_STEER_INVERT  0
#define EUROPAR_PEDAL_INVERT  0

/* 2026-08-05 fix (real bug, found by inspection after a report of "no
 * controls from gamepad" even with the buffered-GetDeviceData fix in place):
 * this used to hardcode XInputGetState(0, ...) -- user index 0 ONLY. XInput
 * exposes 4 independent slots (0-3); Windows assigns them by USB
 * enumeration/driver order, not anything this proxy controls, so a pad that
 * isn't literally the first one Windows ever saw can sit at 1, 2, or 3 for a
 * given process. The loader's own Omni-Play service (a completely separate
 * subsystem -- this proxy does not consume it, see the file header) reported
 * "idx=0" for ITS OWN read, but that's a different process's XInput view and
 * is not proof this DLL's own call sees the same slot. Scan all 4 and latch
 * onto whichever one first answers ERROR_SUCCESS; log the transition once so
 * a live test log shows definitively whether a pad was ever found here at
 * all, and at which index. */
static int s_xi_index = -1;   /* -1 = unknown/not searched this call, -2 = confirmed none connected */

static int xinput_find_pad(XINPUT_STATE *xs) {
    if (s_xi_index >= 0 && s_xi_index <= 3) {
        if (XInputGetState((DWORD)s_xi_index, xs) == ERROR_SUCCESS) return 1;
        LOG("XInput pad at index %d disconnected; rescanning\n", s_xi_index);
        s_xi_index = -1;
    }
    for (DWORD i = 0; i < 4; ++i) {
        memset(xs, 0, sizeof *xs);
        if (XInputGetState(i, xs) == ERROR_SUCCESS) {
            s_xi_index = (int)i;
            LOG("XInput pad found at user index %lu: LX=%d LT=%u RT=%u buttons=0x%04X\n",
                i, (int)xs->Gamepad.sThumbLX, xs->Gamepad.bLeftTrigger,
                xs->Gamepad.bRightTrigger, xs->Gamepad.wButtons);
            return 1;
        }
    }
    if (s_xi_index != -2) {
        s_xi_index = -2;
        LOG("XInput: no pad connected on any of indices 0-3 -- gamepad input impossible until one is\n");
    }
    return 0;
}

/* Raw input reader shared by both output encodings below. Keyboard is
 * primary (arrows or WASD to steer, up/down or W/S for the combined
 * accel/brake pedal); a real XInput pad, if connected, is an alternate
 * source blended in on top. */
/* ---- ANALOG WHEEL / PEDAL INPUT -------------------------------------------
 *
 * WHAT THIS USED TO DO, AND WHY IT FELT WRONG. read_wheel_input() returned four
 * BOOLEANS, and compute_wheel_axes() turned them into exactly three axis values
 * -- 0, 32767 or 65535. Steering was hard left / center / hard right and the
 * pedal was full brake / coasting / full throttle, with nothing in between, no
 * matter what the player was actually holding. The magnitude of the stick and
 * the depth of the triggers were read and then thrown away one line later.
 *
 * NOW: every source reports a MAGNITUDE and the axes are continuous.
 *
 *   steer   -32768..+32767   (negative = left)
 *   accel        0..255
 *   brake        0..255
 *
 * THREE SOURCES, and they coexist -- the strongest deflection wins per control,
 * so a wheel, a pad and the keyboard can all be connected at once:
 *
 *  1. A REAL ANALOG RACING WHEEL, read through winmm joyGetPosEx(). This is new
 *     -- there was no real-wheel path here at all before, only keyboard and
 *     XInput. joyGetPosEx is used deliberately INSTEAD of DirectInput because
 *     THIS FILE IS the process's dinput8.dll: enumerating DirectInput from
 *     inside the DirectInput proxy would recurse into our own implementation.
 *     winmm is a separate, older API onto the same HID devices, so it sees the
 *     real wheel without any of that. It is bound at RUNTIME
 *     (LoadLibrary/GetProcAddress) so this board's build script needs no new
 *     -lwinmm.
 *
 *  2. An XInput pad. The triggers are now read as 0..255 instead of being
 *     thresholded at 30, so L2/R2 give real partial throttle and braking, and
 *     the left stick's deflection drives proportional steering.
 *
 *  3. The keyboard, unchanged in meaning: a held key is full deflection.
 *
 * PEDAL AXIS LAYOUT ON A REAL WHEEL. Ford Racing binds ACCELERATOR and BRAKE to
 * the SAME axis (CTRLLERS.CDB: ACCELERATOR:SLIDER1 and BRAKE:SLIDER1) -- it
 * models a combined-pedal cabinet. A wheel in combined mode reports exactly
 * that on one axis, so the default here is combined-on-Y. Wheels in separate
 * mode differ by vendor, so the axes are container-settable, not guessed:
 *
 *   ULTRA_EUROPAR_WHEEL        0 = ignore real wheels entirely (default: auto)
 *   ULTRA_EUROPAR_WHEEL_ID     joystick index 0..15         (default: first found)
 *   ULTRA_EUROPAR_WHEEL_STEER  axis for steering            (default x)
 *   ULTRA_EUROPAR_WHEEL_ACCEL  axis for throttle            (default y)
 *   ULTRA_EUROPAR_WHEEL_BRAKE  axis for brake; UNSET means the accel axis is a
 *                              COMBINED pedal axis          (default unset)
 *   ULTRA_EUROPAR_WHEEL_DZ     steering deadzone 0..30000   (default 1200)
 *   axis names: x y z r u v -- prefix with a minus to invert, e.g. -r
 */
#define EUROPAR_JOY_RETURNALL 0x000000FFu

typedef struct {
    DWORD dwSize, dwFlags;
    DWORD dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
    DWORD dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
} EUROPAR_JOYINFOEX;

typedef UINT (WINAPI *pfn_joyGetPosEx)(UINT, EUROPAR_JOYINFOEX*);

static pfn_joyGetPosEx s_joyGetPosEx;
static int  s_wheel_ready;          /* 0 = not tried, 1 = bound, -1 = unavailable */
static int  s_wheel_on   = 1;
static int  s_wheel_id   = -1;      /* -1 = probe for the first present device */
static int  s_wheel_dz   = 1200;
static char s_ax_steer[8] = "x";
static char s_ax_accel[8] = "y";
static char s_ax_brake[8] = "";     /* empty = the accel axis is combined */

static void wheel_env_str(const char *k, char *dst, size_t n) {
    const char *v = getenv(k);
    size_t i = 0;
    if (!v || !*v) return;
    while (v[i] && i + 1 < n) { dst[i] = v[i]; i++; }
    dst[i] = 0;
}

static void wheel_bind_once(void) {
    const char *e;
    HMODULE mm;
    if (s_wheel_ready) return;
    e = getenv("ULTRA_EUROPAR_WHEEL");
    if (e && *e && atoi(e) == 0) { s_wheel_on = 0; s_wheel_ready = -1; return; }
    if ((e = getenv("ULTRA_EUROPAR_WHEEL_ID")) && *e) s_wheel_id = atoi(e);
    if ((e = getenv("ULTRA_EUROPAR_WHEEL_DZ")) && *e) {
        s_wheel_dz = atoi(e);
        if (s_wheel_dz < 0)     s_wheel_dz = 0;
        if (s_wheel_dz > 30000) s_wheel_dz = 30000;
    }
    wheel_env_str("ULTRA_EUROPAR_WHEEL_STEER", s_ax_steer, sizeof s_ax_steer);
    wheel_env_str("ULTRA_EUROPAR_WHEEL_ACCEL", s_ax_accel, sizeof s_ax_accel);
    wheel_env_str("ULTRA_EUROPAR_WHEEL_BRAKE", s_ax_brake, sizeof s_ax_brake);

    mm = LoadLibraryA("winmm.dll");
    if (!mm) { s_wheel_ready = -1; return; }
    s_joyGetPosEx = (pfn_joyGetPosEx)(void*)GetProcAddress(mm, "joyGetPosEx");
    s_wheel_ready = s_joyGetPosEx ? 1 : -1;
}

/* Pick one axis out of a JOYINFOEX by the container's name. Returns 0..65535,
 * or -1 when the name is empty or unrecognised, so the caller can tell "not
 * configured" apart from "reads zero". */
static int wheel_axis(const EUROPAR_JOYINFOEX *ji, const char *name) {
    const char *n = name;
    int inv = 0;
    DWORD v;
    if (*n == '-') { inv = 1; n++; }
    switch (*n) {
        case 'x': case 'X': v = ji->dwXpos; break;
        case 'y': case 'Y': v = ji->dwYpos; break;
        case 'z': case 'Z': v = ji->dwZpos; break;
        case 'r': case 'R': v = ji->dwRpos; break;
        case 'u': case 'U': v = ji->dwUpos; break;
        case 'v': case 'V': v = ji->dwVpos; break;
        default: return -1;
    }
    if (v > 65535) v = 65535;
    return inv ? (int)(65535 - v) : (int)v;
}

/* Read a real wheel. Returns 1 when a device answered, 0 otherwise. */
static int read_real_wheel(int *steer, int *accel, int *brake) {
    EUROPAR_JOYINFOEX ji;
    int id, lo, hi, i;
    wheel_bind_once();
    if (s_wheel_ready != 1 || !s_wheel_on) return 0;

    id = s_wheel_id;
    lo = (id >= 0) ? id : 0;
    hi = (id >= 0) ? id : 15;
    for (i = lo; i <= hi; ++i) {
        int sx, a, b;
        memset(&ji, 0, sizeof ji);
        ji.dwSize  = sizeof ji;
        ji.dwFlags = EUROPAR_JOY_RETURNALL;
        if (s_joyGetPosEx((UINT)i, &ji) != 0) continue;   /* JOYERR_NOERROR == 0 */

        sx = wheel_axis(&ji, s_ax_steer);
        if (sx >= 0) {
            int c = sx - 32768;                            /* -32768..+32767 */
            if (c > -s_wheel_dz && c < s_wheel_dz)  c = 0;
            else if (c > 0) c = (c - s_wheel_dz) * 32767 / (32767 - s_wheel_dz);
            else            c = (c + s_wheel_dz) * 32768 / (32768 - s_wheel_dz);
            *steer = c;
        }

        a = wheel_axis(&ji, s_ax_accel);
        b = (*s_ax_brake) ? wheel_axis(&ji, s_ax_brake) : -1;
        if (b >= 0) {
            /* Separate pedals: each axis is its own 0..65535 travel. */
            if (a >= 0) *accel = a * 255 / 65535;
            *brake = b * 255 / 65535;
        } else if (a >= 0) {
            /* COMBINED pedal axis: center is coasting, one half is throttle and
             * the other is brake -- the same shape the game's own SLIDER1
             * binding expects, so it passes straight through. */
            int c = a - 32768;
            if (c > 0)      *accel = c * 255 / 32767;
            else if (c < 0) *brake = (-c) * 255 / 32768;
        }
        return 1;
    }
    return 0;
}

static int iabs_(int v) { return v < 0 ? -v : v; }

static void read_wheel_input(int *out_steer, int *out_accel, int *out_brake, BYTE btn[8]) {
    int steer = 0, accel = 0, brake = 0;
    int kl, kr;
    XINPUT_STATE xs;
    int ws, wa, wb;

    memset(btn, 0, 8);

    /* ---- keyboard: a held key is full deflection ---- */
    kl = (GetAsyncKeyState(VK_LEFT)  & 0x8000) || (GetAsyncKeyState('A') & 0x8000);
    kr = (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState('D') & 0x8000);
    if (kl && !kr)      steer = -32768;
    else if (kr && !kl) steer =  32767;
    if ((GetAsyncKeyState(VK_UP)   & 0x8000) || (GetAsyncKeyState('W') & 0x8000)) accel = 255;
    if ((GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000)) brake = 255;

    btn[0] = (GetAsyncKeyState('Z')       & 0x8000) ? 0x80 : 0;   /* GEAR_DOWN   */
    btn[1] = (GetAsyncKeyState('X')       & 0x8000) ? 0x80 : 0;   /* GEAR_UP     */
    btn[2] = (GetAsyncKeyState('V')       & 0x8000) ? 0x80 : 0;   /* LOOK_BEHIND */
    btn[3] = (GetAsyncKeyState(VK_SPACE)  & 0x8000) ? 0x80 : 0;   /* HANDBRAKE   */
    btn[4] = (GetAsyncKeyState('C')       & 0x8000) ? 0x80 : 0;   /* CHANGE_VIEW */
    btn[5] = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ? 0x80 : 0;   /* BOOST       */

    /* ---- XInput pad: PROPORTIONAL, not thresholded ---- */
    memset(&xs, 0, sizeof xs);
    if (xinput_find_pad(&xs)) {
        /* STEERING RESPONSE CURVE. Owner, 2026-09-20: "the analog stick for steering
         * is a bit too sensitive. It needs to be less sensitive."
         *
         * The first analog pass mapped the stick STRAIGHT onto the axis: past the
         * deadzone, half a stick meant half lock. On a wheel that is a full 900-degree
         * sweep squeezed into about 10 mm of thumb travel, which is exactly the twitch
         * being described -- the car darts on inputs meant to be small corrections.
         *
         * Two knobs, and they do different jobs:
         *   GAMMA scales the SHAPE. The normalized deflection is raised to a power, so
         *         small movements near center move the wheel much less while full
         *         deflection still reaches full lock. 2.0 (squared) is the default:
         *         half a stick now gives a quarter lock instead of half.
         *   SCALE caps the TOTAL. 100 = full lock available; 70 means the stick can
         *         only ever reach 70% lock, for a calmer car on a twitchy cabinet.
         *
         * A real wheel is deliberately NOT curved -- see read_real_wheel(). Its travel
         * is physical and 1:1 is what the driver expects; bending that would be wrong.
         *
         *   ULTRA_EUROPAR_STEER_GAMMA  response curve x10 (default 20 = 2.0; 10 = linear)
         *   ULTRA_EUROPAR_STEER_SCALE  max lock as a percentage      (default 100)
         *   ULTRA_EUROPAR_STEER_DZ     stick deadzone, 0..30000      (default 7000) */
        static int s_sdz = -1, s_sgamma = 20, s_sscale = 100;
        if (s_sdz < 0) {
            const char *e;
            s_sdz = ((e = getenv("ULTRA_EUROPAR_STEER_DZ")) && *e) ? atoi(e) : 7000;
            if ((e = getenv("ULTRA_EUROPAR_STEER_GAMMA")) && *e) s_sgamma = atoi(e);
            if ((e = getenv("ULTRA_EUROPAR_STEER_SCALE")) && *e) s_sscale = atoi(e);
            if (s_sdz    < 0)     s_sdz    = 0;
            if (s_sdz    > 30000) s_sdz    = 30000;
            if (s_sgamma < 10)    s_sgamma = 10;    /* below 1.0 would make it TWITCHIER */
            if (s_sgamma > 40)    s_sgamma = 40;
            if (s_sscale < 10)    s_sscale = 10;
            if (s_sscale > 100)   s_sscale = 100;
        }
        {
            int lx = xs.Gamepad.sThumbLX;
            int neg = lx < 0;
            int mag = neg ? -lx : lx;
            int px  = 0;
            if (mag > s_sdz) {
                /* normalize past the deadzone to 0..1000, apply the curve, then scale */
                int n = (mag - s_sdz) * 1000 / (32767 - s_sdz);
                int c;
                if (n < 0)    n = 0;
                if (n > 1000) n = 1000;
                if (s_sgamma <= 10)      c = n;                         /* linear   */
                else if (s_sgamma >= 30) c = n * n / 1000 * n / 1000;   /* cubic    */
                else                     c = n * n / 1000;              /* squared  */
                c = c * s_sscale / 100;
                px = c * 32767 / 1000;
                if (px > 32767) px = 32767;
                if (neg) px = -px;
            }
            if (iabs_(px) > iabs_(steer)) steer = px;
        }

        /* L2 / R2 are the pedals, at their real depth. */
        if (xs.Gamepad.bRightTrigger > accel) accel = xs.Gamepad.bRightTrigger;
        if (xs.Gamepad.bLeftTrigger  > brake) brake = xs.Gamepad.bLeftTrigger;

        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btn[0] = 0x80;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btn[1] = 0x80;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_Y)              btn[2] = 0x80;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_B)              btn[3] = 0x80;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_X)              btn[4] = 0x80;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_A)              btn[5] = 0x80;
    }

    /* ---- a real wheel joins in: strongest deflection per control wins ---- */
    ws = steer; wa = accel; wb = brake;
    if (read_real_wheel(&ws, &wa, &wb)) {
        if (iabs_(ws) > iabs_(steer)) steer = ws;
        if (wa > accel) accel = wa;
        if (wb > brake) brake = wb;
    }

    if (steer < -32768) steer = -32768;
    if (steer >  32767) steer =  32767;
    if (accel < 0)   accel = 0;
    if (accel > 255) accel = 255;
    if (brake < 0)   brake = 0;
    if (brake > 255) brake = 255;

    *out_steer = steer;
    *out_accel = accel;
    *out_brake = brake;
}

/* Absolute encoding: 0/32767/65535, DirectInput's own default axis range.
 * Correct for a JOYSTICK-class device (GetDeviceState's DIJOYSTATE fallback,
 * or any future real GUID_Joystick CreateDevice) -- axes on a real joystick
 * report an absolute stick/pedal POSITION. */
static void compute_wheel_axes(LONG *out_x, LONG *out_slider, BYTE btn[8]) {
    int steer, accel, brake;
    LONG x, s;
    read_wheel_input(&steer, &accel, &brake, btn);

    /* Steering: -32768..+32767 maps onto the full 0..65535 axis, center 32768. */
    x = (LONG)(steer + 32768);
    if (x < 0)     x = 0;
    if (x > 65535) x = 65535;
#if EUROPAR_STEER_INVERT
    x = 65535 - x;
#endif
    *out_x = x;

    /* Pedals: ONE combined axis, because that is what the game asks for --
     * CTRLLERS.CDB binds both ACCELERATOR and BRAKE to SLIDER1. Center is
     * coasting, throttle drives it up, brake drives it down. Holding both
     * partially now BLENDS instead of canceling to exactly center, so
     * trail-braking works. */
    s = 32767 + (LONG)(((long long)accel - (long long)brake) * 32768 / 255);
    if (s < 0)     s = 0;
    if (s > 65535) s = 65535;
#if EUROPAR_PEDAL_INVERT
    s = 65535 - s;
#endif
    *out_slider = s;
}

/* 2026-08-05 fix (real, well-evidenced bug -- found by examining the actual
 * game, not guessing): a live capture of fordracing.exe's own CreateDevice
 * calls (see log_caller_once()'s header below) proved the game NEVER once
 * requests a joystick -- it calls CreateDevice with GUID_SysMouse
 * (0x6F1D2B60) and negotiates the "7 objects, 16 bytes" custom format on
 * THAT device. This matches this file's own header research: Ford Racing's
 * real cabinet I/O board is HAPP-style, and HAPP boards conventionally
 * report a wheel encoder's rotation as relative mouse-X movement (universal
 * HID mouse support, no custom driver needed) rather than as a joystick
 * axis. DirectInput mice fundamentally report DELTAS since the last poll,
 * never an absolute position -- compute_wheel_axes()'s 0/32767/65535
 * ABSOLUTE values, jumping straight to a "position" and back, are nonsense
 * to something expecting small continuous movement deltas, which plausibly
 * explains everything observed: format negotiation succeeds, buffered
 * events genuinely get computed and delivered (both independently proven
 * live), yet nothing perceptibly happens in game. This function encodes the
 * SAME held-direction input as a steady delta instead, matching how a
 * spinning wheel encoder (or someone holding a direction key, standing in
 * for one) would actually report to a mouse-shaped HID interface: a fixed
 * magnitude every poll for as long as the direction is held, zero when it
 * isn't. GetDeviceData's emission rule for these fields is ALSO changed
 * (see the DIDFT_RELAXIS-vs-continuous check there) to fire every poll
 * while nonzero, matching real relative-mouse reporting -- edge-triggering
 * on "changed since last value" (correct for buttons and for the absolute
 * joystick path) would otherwise only report the single instant the key
 * was pressed, not the continuous motion a held key is meant to represent. */
#define EUROPAR_MOUSE_STEER_DELTA   400
#define EUROPAR_MOUSE_PEDAL_DELTA   400

static void compute_wheel_deltas(LONG *out_dx, LONG *out_dpedal, BYTE btn[8]) {
    int steer, accel, brake;
    LONG dx, dp;
    read_wheel_input(&steer, &accel, &brake, btn);

    /* Relative path: the delta is now PROPORTIONAL to deflection rather than a
     * fixed +/-400 step, so a half-pressed trigger moves the value half as fast. */
    dx = (LONG)((long long)steer * EUROPAR_MOUSE_STEER_DELTA / 32767);
#if EUROPAR_STEER_INVERT
    dx = -dx;
#endif
    *out_dx = dx;

    dp = (LONG)(((long long)accel - (long long)brake) * EUROPAR_MOUSE_PEDAL_DELTA / 255);
#if EUROPAR_PEDAL_INVERT
    dp = -dp;
#endif
    *out_dpedal = dp;
}

/* =============================================================================
 *  Synthetic IDirectInputDevice8A. One object type serves keyboard, mouse and
 *  joystick alike.
 *
 *  2026-08-04 fix (real, live bug -- confirmed by the reporter's own test,
 *  not theoretical): GetDeviceState used to pick behavior PURELY from the
 *  caller-supplied `cb` (buffer size) and, for anything joystick-sized,
 *  blindly wrote a stock DIJOYSTATE layout (lX at offset 0, rgbButtons at
 *  offset 0x38, ...). That's only correct if the game's own SetDataFormat
 *  happens to use that exact layout. Most DirectInput racing titles --
 *  extremely likely this one too, given how specifically this file's own
 *  header had to reverse-engineer its bundled CTRLLERS.CDB just to find the
 *  right guidProduct -- define their OWN custom DIDATAFORMAT with different
 *  offsets. Writing DIJOYSTATE-shaped data into a differently-shaped buffer
 *  silently corrupts everything except whatever field coincidentally lands
 *  on a byte the game also uses for something harmless -- consistent with
 *  the observed symptom (Enter, read via a separate path, worked; every
 *  wheel/pedal/button field driven through GetDeviceState did not).
 *
 *  Fix: actually store the DIDATAFORMAT the game hands us in SetDataFormat
 *  and, in GetDeviceState, walk ITS object list -- writing each axis/slider/
 *  button at the OFFSET AND WIDTH THE GAME ITSELF DECLARED -- instead of
 *  assuming any fixed layout. Falls back to the old fixed-layout write only
 *  if the game never set a format (defensive; every real game does). */
typedef struct {
    const IDirectInputDevice8AVtbl *lpVtbl;
    LONG ref;
    DIOBJECTDATAFORMAT *rgodf;   /* deep copy of the game's own object list */
    DWORD               numObjs;
    DWORD               dataSize;
    LONG               *last_val;  /* one slot per rgodf[i]; last value reported via GetDeviceData */
    DWORD               seq;       /* DIDEVICEOBJECTDATA.dwSequence counter for buffered mode */
    LONG                rangeMin;  /* DIPROP_RANGE, device-wide (DirectInput default: 0..65535) */
    LONG                rangeMax;
    HANDLE              notifyEvent;  /* SetEventNotification target; signaled from Poll()/GetDeviceData() */
    int                 is_mouse;     /* set in f_CreateDevice from the requested GUID -- see compute_wheel_deltas()'s header */
} ul_dev;

static HRESULT STDMETHODCALLTYPE d_QI(IDirectInputDevice8A *T, REFIID r, void **p)
{ (void)r; if (p) { *p = T; ++((ul_dev*)T)->ref; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE d_AddRef(IDirectInputDevice8A *T) { return (ULONG)++((ul_dev*)T)->ref; }
static ULONG STDMETHODCALLTYPE d_Release(IDirectInputDevice8A *T) {
    ul_dev *d = (ul_dev*)T; LONG r = --d->ref;
    if (r <= 0) { free(d->rgodf); free(d->last_val); free(d); return 0; }
    return (ULONG)r;
}
/* 2026-08-05 fix (real inconsistency, found via systematic audit after 5+
 * consecutive live tests all proved GetDeviceData computes and stages
 * correct button/axis values yet the game still showed zero response): this
 * claimed dwAxes=2, dwButtons=8 -- guesses from before a live format dump
 * existed. The game's REAL negotiated format (confirmed live, see the
 * SetDataFormat dump above) has 3 axis-type objects (X/Y/Z) and 4 button
 * objects, not 2 and 8. A defensive game that cross-checks GetCaps' claimed
 * counts against what its own format actually uses could treat that
 * mismatch as "this device doesn't really back up what it claims" and
 * distrust or ignore it, even though format negotiation itself succeeded.
 * Numbers now match what EnumObjects (below) actually enumerates. */
static HRESULT STDMETHODCALLTYPE d_GetCaps(IDirectInputDevice8A *T, LPDIDEVCAPS c) {
    (void)T;
    if (c) {
        DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags   = DIDC_ATTACHED | DIDC_POLLEDDEVICE;
        c->dwDevType = DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_COMBINEDPEDALS << 8);
        c->dwAxes = 3; c->dwButtons = 4; c->dwPOVs = 0;
    }
    return DI_OK;
}

/* 2026-08-05 fix (same audit): this used to never invoke the callback at
 * all -- a game that calls EnumObjects to validate the device's real object
 * list (independent of whatever format it later negotiates) got back an
 * empty enumeration for a device that claims to have 3 axes + 4 buttons.
 * Now actually reports them: 3 axes tagged with the real GUID_XAxis/YAxis/
 * ZAxis (so a game matching by GUID gets a real answer) and 4 generic
 * (NULL-GUID) push buttons, mirroring the real negotiated format's own
 * shape 1:1. dwFlags' low byte is a type filter (DIDFT_ALL=0 means "no
 * filter, everything"); honored so a game asking for buttons-only or
 * axes-only doesn't get the other kind mixed in. */
static HRESULT STDMETHODCALLTYPE d_EnumObjects(IDirectInputDevice8A *T, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD fl) {
    (void)T;
    if (!cb) return DI_OK;
    DWORD type_filter = fl & 0xFF;
    int want_axis   = (type_filter == DIDFT_ALL) || (type_filter & DIDFT_AXIS)   ? 1 : 0;
    int want_button = (type_filter == DIDFT_ALL) || (type_filter & DIDFT_BUTTON) ? 1 : 0;
    DIDEVICEOBJECTINSTANCEA inst;
    if (want_axis) {
        static const GUID *axis_guid[3] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis };
        static const char *axis_name[3] = { "X Axis", "Y Axis", "Z Axis" };
        for (int i = 0; i < 3; ++i) {
            memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
            inst.guidType = *axis_guid[i];
            inst.dwOfs    = (DWORD)(i * 4);
            inst.dwType   = (DWORD)DIDFT_MAKEINSTANCE(i) | DIDFT_ABSAXIS;
            strncpy(inst.tszName, axis_name[i], MAX_PATH - 1);
            if (cb(&inst, ref) == DIENUM_STOP) return DI_OK;
        }
    }
    if (want_button) {
        for (int i = 0; i < 4; ++i) {
            memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
            inst.guidType = GUID_Button;
            inst.dwOfs    = (DWORD)(12 + i);
            inst.dwType   = (DWORD)DIDFT_MAKEINSTANCE(i) | DIDFT_PSHBUTTON;
            snprintf(inst.tszName, MAX_PATH, "Button %d", i);
            if (cb(&inst, ref) == DIENUM_STOP) return DI_OK;
        }
    }
    return DI_OK;
}
/* 2026-08-05 fix (real bug, found by inspection after buttons AND axes were
 * both proven -- via a live log showing exact 0/128 and 0/32767/65535
 * transitions -- to compute and stage correctly, yet the game still showed
 * zero response in an actual race): GetProperty used to be `(void)h; return
 * DI_OK`, meaning it NEVER wrote to the caller's buffer at all. DIPROP_RANGE
 * is a standard, commonly-queried joystick property -- if Ford Racing reads
 * it before trusting axis data (the normal DirectInput pattern: read the
 * device's real range, then interpret/normalize incoming values against
 * it), it would have read back uninitialized stack garbage as lMin/lMax and
 * scaled every real, correctly-computed axis value by that garbage --
 * exactly consistent with "the data is right but nothing happens in game".
 * SetProperty was the same no-op, so if the game instead SETS its own
 * preferred range we'd silently keep emitting the old range regardless,
 * same mismatch from the other direction. Fixed: store a real (default
 * DirectInput-standard 0..65535, see make_device()) device-wide range,
 * honor whatever the game explicitly sets, answer queries with the truth,
 * and rescale computed axis values to whatever range was actually
 * negotiated (see rescale_axis() / compute_object_values()) instead of
 * assuming raw 0..65535 always matches what the game expects.
 *
 * DIPROP_RANGE/DEADZONE/etc. are DirectInput's classic integer-valued
 * pseudo-GUIDs (MAKEDIPROP(n) casts a small int to REFGUID) -- compared by
 * POINTER VALUE, never memcmp/dereference (unlike the real 16-byte object
 * GUIDs like GUID_XAxis used elsewhere in this file). */
static HRESULT STDMETHODCALLTYPE d_GetProperty(IDirectInputDevice8A *T, REFGUID g, LPDIPROPHEADER h) {
    ul_dev *d = (ul_dev*)T;
    if (!h) return E_POINTER;
    if (g == DIPROP_RANGE && h->dwSize >= sizeof(DIPROPRANGE)) {
        LPDIPROPRANGE r = (LPDIPROPRANGE)h;
        r->lMin = d->rangeMin;
        r->lMax = d->rangeMax;
        return DI_OK;
    }
    if (g == DIPROP_DEADZONE && h->dwSize >= sizeof(DIPROPDWORD)) {
        ((LPDIPROPDWORD)h)->dwData = 0;        /* no built-in deadzone: our output is already digital */
        return DI_OK;
    }
    if (g == DIPROP_SATURATION && h->dwSize >= sizeof(DIPROPDWORD)) {
        ((LPDIPROPDWORD)h)->dwData = 10000;    /* 100%, DirectInput's convention */
        return DI_OK;
    }
    return DI_OK;   /* anything else: leave the caller's buffer untouched, as before this fix */
}
static HRESULT STDMETHODCALLTYPE d_SetProperty(IDirectInputDevice8A *T, REFGUID g, LPCDIPROPHEADER h) {
    ul_dev *d = (ul_dev*)T;
    if (h && g == DIPROP_RANGE && h->dwSize >= sizeof(DIPROPRANGE)) {
        LPCDIPROPRANGE r = (LPCDIPROPRANGE)h;
        d->rangeMin = r->lMin;
        d->rangeMax = r->lMax;
        LOG("SetProperty(DIPROP_RANGE): [%ld, %ld]\n", (long)d->rangeMin, (long)d->rangeMax);
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_Acquire(IDirectInputDevice8A *T)   { (void)T; log_caller_once("Acquire", __builtin_return_address(0)); return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Unacquire(IDirectInputDevice8A *T) { (void)T; return DI_OK; }

/* Compute the CURRENT value of every object in d->rgodf[], in one pass, into
 * a caller-supplied array (must hold >= d->numObjs LONGs). Shared by both
 * polling paths below (GetDeviceState's format-aware write and GetDeviceData's
 * buffered edge-detection) so they always agree on which physical object
 * (steer axis vs pedal slider vs button N) lives at each index -- computing
 * this twice, independently, would risk the two paths silently disagreeing
 * about the positional axis_seen assignment. Axis assignment: the game's own
 * pguid wins when it specified GUID_XAxis/GUID_Slider explicitly; otherwise
 * (very common in a hand-built custom format -- pguid is often NULL there)
 * the first axis-type object seen is steer and the next axis-or-slider
 * object is the combined accel/brake channel, matching how this device is
 * described everywhere else in this file (one steer axis, one combined pedal
 * slider). Buttons are keyed by DirectInput's own per-object instance number
 * (DIDFT_GETINSTANCE) WHEN the game actually supplied one (0-7). Ford Racing's
 * real format doesn't: a live dump (2026-08-05) showed every one of its 7
 * objects -- axes and buttons alike -- declared with instance=0x0000FFFF
 * (DIDFT_ANYINSTANCE, DirectInput's wildcard for "match by GUID/order, not a
 * specific slot"). DIDFT_GETINSTANCE() on a wildcard object returns 65535,
 * which failed the `inst < 8` bounds check unconditionally -- every button on
 * this device was silently forced to 0 for every input source, every frame,
 * full stop; that is the actual reason nothing but keyboard's separate Enter/
 * Space ever responded. Axes disambiguate by GUID instead (GUID_XAxis, etc.,
 * handled below) so they were unaffected. Buttons have no such tag, so a
 * wildcard-instance button is identified by ITS OWN POSITION among the
 * button-type objects encountered so far -- the same "order of appearance"
 * principle already used for axis_seen above -- falling back to the game's
 * real instance number whenever one actually is < 8. */
/* compute_wheel_axes() always produces raw values in DirectInput's own
 * default 0..65535 axis range. Rescale to whatever range is actually
 * negotiated for this device (make_device() defaults it to 0..65535 too, so
 * this is a no-op unless/until the game calls SetProperty(DIPROP_RANGE) --
 * see that fix's header above d_GetProperty for why this exists at all). */
static LONG rescale_axis(LONG v, LONG dst_min, LONG dst_max) {
    if (dst_min == 0 && dst_max == 65535) return v;
    long long span   = (long long)dst_max - (long long)dst_min;
    long long scaled = (long long)dst_min + (span * (long long)v) / 65535;
    return (LONG)scaled;
}

static void compute_object_values(ul_dev *d, LONG *vals) {
    LONG x, s; BYTE btn[8];
    if (d->is_mouse) compute_wheel_deltas(&x, &s, btn);   /* relative: see that function's header */
    else             compute_wheel_axes(&x, &s, btn);     /* absolute: real joystick-shaped position */
    int axis_seen = 0;
    int button_seen = 0;
    for (DWORD i = 0; i < d->numObjs; ++i) {
        DIOBJECTDATAFORMAT *o = &d->rgodf[i];
        DWORD type = o->dwType;
        if (type & DIDFT_BUTTON) {
            DWORD inst = DIDFT_GETINSTANCE(type);
            if (inst >= 8) inst = (DWORD)button_seen;   /* wildcard instance: assign by encounter order */
            vals[i] = (inst < 8) ? btn[inst] : 0;
            ++button_seen;
        } else if (type & DIDFT_POV) {
            vals[i] = (LONG)0xFFFFFFFFu;   /* no hat pressed */
        } else if (type & (DIDFT_ABSAXIS | DIDFT_RELAXIS)) {
            LONG raw;
            if (o->pguid && !memcmp(o->pguid, &GUID_XAxis, sizeof(GUID)))       raw = x;
            else if (o->pguid && !memcmp(o->pguid, &GUID_Slider, sizeof(GUID))) raw = s;
            else if (!axis_seen)                                               { raw = x; axis_seen = 1; }
            else                                                               raw = s;
            /* DIPROP_RANGE is an absolute-axis concept; a relative delta is
             * already in its own native units and is never rescaled. */
            vals[i] = d->is_mouse ? raw : rescale_axis(raw, d->rangeMin, d->rangeMax);
        } else {
            vals[i] = 0;
        }
    }
}

/* Format-aware write: place each of the game's own declared objects at the
 * OFFSET AND WIDTH IT DECLARED, instead of assuming a fixed struct layout.
 * Axes/sliders are 4-byte LONGs; buttons and POVs follow DirectInput's own
 * convention (button = 1 byte, POV = 4-byte DWORD). Every write is
 * bounds-checked against the caller's actual buffer size (cb) -- never trust
 * dwOfs alone. */
static void write_formatted_joystick(ul_dev *d, DWORD cb, LPVOID data) {
    LONG stackvals[64]; LONG *vals = stackvals; LONG *heap = NULL;
    if (d->numObjs > 64) { heap = (LONG*)malloc((size_t)d->numObjs * sizeof(LONG)); vals = heap; if (!vals) return; }
    compute_object_values(d, vals);
    for (DWORD i = 0; i < d->numObjs; ++i) {
        DIOBJECTDATAFORMAT *o = &d->rgodf[i];
        DWORD type = o->dwType;
        if (type & DIDFT_BUTTON) {
            if (o->dwOfs >= cb) continue;
            *((BYTE*)data + o->dwOfs) = (BYTE)vals[i];
        } else if (type & DIDFT_POV) {
            if (o->dwOfs + sizeof(DWORD) > cb) continue;
            *(DWORD*)((BYTE*)data + o->dwOfs) = (DWORD)vals[i];
        } else if (type & (DIDFT_ABSAXIS | DIDFT_RELAXIS)) {
            if (o->dwOfs + sizeof(LONG) > cb) continue;
            *(LONG*)((BYTE*)data + o->dwOfs) = vals[i];
        }
    }
    free(heap);
}

/* Buffered-mode polling: DirectInput games that read joystick/wheel input via
 * GetDeviceData (instead of, or in addition to, GetDeviceState) expect a
 * queue of "what changed since my last call" events -- one DIDEVICEOBJECTDATA
 * per object whose value differs from what was last reported, each carrying
 * the object's dwOfs (so the game knows which field) and its new value.
 *
 * 2026-08-04 fix (real, live bug): this was previously a permanent stub that
 * always returned *pdwInOut = 0 -- i.e. "nothing ever changed", forever.
 * SetDataFormat succeeding (confirmed live: "7 objects, dwDataSize=16" in the
 * log) proves the game DOES negotiate a real format for this device, yet
 * steering/pedals/gears/handbrake/view/boost still didn't respond even after
 * GetDeviceState became format-aware -- while the keyboard device (which
 * only this game ever polls via GetDeviceState, since cb==256 is unambiguous)
 * worked the whole time. That split is consistent with Ford Racing reading
 * the WHEEL specifically through buffered acquisition, which this function
 * never fed. Uses the exact same compute_object_values() as GetDeviceState so
 * both polling styles agree on what each declared object means. */
static void ensure_last_val(ul_dev *d) {
    if (d->last_val || !d->numObjs) return;
    d->last_val = (LONG*)calloc(d->numObjs, sizeof(LONG));
    if (d->last_val) compute_object_values(d, d->last_val);   /* baseline: report no stale "changes" on first poll */
}

static int s_gdd_call_logged = 0;
static int s_gdd_change_logged = 0;   /* caps how many "object changed" lines we ever print, so a long play session can't spam the log */

static HRESULT STDMETHODCALLTYPE d_GetDeviceData(IDirectInputDevice8A *T, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl) {
    (void)cb;
    ul_dev *d = (ul_dev*)T;
    if (!io) return E_POINTER;
    DWORD want = *io;
    *io = 0;
    if (!s_gdd_call_logged) {
        s_gdd_call_logged = 1;
        LOG("GetDeviceData: first call, cb=%lu want=%lu numObjs=%lu peek=%d\n",
            cb, want, d->numObjs, (fl & DIGDD_PEEK) ? 1 : 0);
    }
    log_caller_once("GetDeviceData", __builtin_return_address(0));
    if (!d->rgodf || !d->numObjs) return DI_OK;    /* unformatted device: no buffered queue */
    if (d->dataSize == 256) {
        /* KEYBOARD BUFFERED MODE (2026-08-13 fix -- user reported "no controls
         * in menu and no way to press start"). This path used to be a bare
         * `return DI_OK` on the assumption the keyboard is only ever read via
         * immediate GetDeviceState (cb==256). That's wrong for MENUS: DirectInput
         * menu/UI code overwhelmingly reads discrete key-DOWN events through
         * BUFFERED GetDeviceData (SetProperty DIPROP_BUFFERSIZE + GetDeviceData),
         * not held-state polling -- so with this stubbed, every menu keypress
         * (and START) was silently dropped while in-race held-key reads (which do
         * use GetDeviceState) worked. Now we diff the live keyboard against the
         * last reported state and emit one DIDEVICEOBJECTDATA per changed key:
         * dwOfs = the DIK_ scan-code index (== byte offset in c_dfDIKeyboard),
         * dwData = 0x80 (down) / 0 (up). PEEK doesn't consume. Only reports on
         * change, so it never interferes with the still-working GetDeviceState
         * held-key path. */
        static BYTE s_kb_last[256];
        BYTE kb[256]; memset(kb, 0, sizeof kb);
        fill_keyboard_state(kb);
        DWORD now = GetTickCount();
        DWORD n = 0;
        for (int i = 0; i < 256; ++i) {
            if (kb[i] == s_kb_last[i]) continue;
            if (od) {
                if (n >= want) break;      /* caller buffer full -- rest reported next poll */
                od[n].dwOfs       = (DWORD)i;
                od[n].dwData      = kb[i];
                od[n].dwTimeStamp = now;
                od[n].dwSequence  = ++d->seq;
#if(DIRECTINPUT_VERSION >= 0x0800)
                od[n].uAppData    = 0;
#endif
            }
            ++n;
            if (od && !(fl & DIGDD_PEEK)) s_kb_last[i] = kb[i];
        }
        *io = n;
        if (n && s_gdd_change_logged < 8) { ++s_gdd_change_logged;
            LOG("GetDeviceData: KEYBOARD buffered, %lu key event(s) this poll\n", n); }
        return DI_OK;
    }
    ensure_last_val(d);
    if (!d->last_val) return DI_OK;

    LONG stackvals[64]; LONG *vals = stackvals; LONG *heap = NULL;
    if (d->numObjs > 64) { heap = (LONG*)malloc((size_t)d->numObjs * sizeof(LONG)); vals = heap; if (!vals) return DI_OK; }
    compute_object_values(d, vals);

    DWORD now = GetTickCount();
    DWORD n = 0;
    for (DWORD i = 0; i < d->numObjs; ++i) {
        DWORD type = d->rgodf[i].dwType;
        /* Buttons (and anything else) edge-trigger: report only the instant
         * the value changes, standard DirectInput semantics. A relative axis
         * on the mouse-kind device is different -- see compute_wheel_deltas()'s
         * header -- it must report EVERY poll while nonzero, the same way
         * real mouse hardware sends a fresh movement packet each tick a
         * physical wheel keeps turning, not just once at the start. */
        int is_axis    = (type & (DIDFT_ABSAXIS | DIDFT_RELAXIS)) && !(type & DIDFT_BUTTON);
        int continuous = d->is_mouse && is_axis;
        int report     = continuous ? (vals[i] != 0) : (vals[i] != d->last_val[i]);
        if (!report) continue;
        if (s_gdd_change_logged < 8) {
            ++s_gdd_change_logged;
            LOG("GetDeviceData: object[%lu] ofs=%lu %ld -> %ld\n", i, d->rgodf[i].dwOfs, d->last_val[i], vals[i]);
        }
        if (od) {
            if (n >= want) break;   /* caller's buffer is full */
            od[n].dwOfs       = d->rgodf[i].dwOfs;
            od[n].dwData       = (DWORD)vals[i];
            od[n].dwTimeStamp  = now;
            od[n].dwSequence   = ++d->seq;
#if(DIRECTINPUT_VERSION >= 0x0800)
            od[n].uAppData     = 0;
#endif
        }
        ++n;
        if (od && !(fl & DIGDD_PEEK)) d->last_val[i] = vals[i];
    }
    *io = n;
    free(heap);
    return DI_OK;
}

/* 2026-08-05 diagnostic: GetDeviceData is now confirmed (by a live log) to
 * compute and stage correct button/axis values, but this device may ALSO be
 * polled the other way -- many racing titles read continuous wheel/pedal
 * axes via immediate-mode GetDeviceState every frame while only using
 * buffered GetDeviceData for discrete button-press events on the SAME
 * device. There was no visibility at all into whether THIS function is ever
 * reached for the joystick, or which of its three outcomes (custom format /
 * DIJOYSTATE fallback / unrecognised-size no-op) it lands in -- add it. */
static int s_gds_joy_logged = 0;

static HRESULT STDMETHODCALLTYPE d_GetDeviceState(IDirectInputDevice8A *T, DWORD cb, LPVOID data) {
    ul_dev *d = (ul_dev*)T;
    if (!data) return E_POINTER;
    log_caller_once("GetDeviceState", __builtin_return_address(0));
    memset(data, 0, cb);

    /* Keyboard and mouse formats are checked FIRST and unconditionally, by
     * their well-known standard sizes, exactly as before this fix -- a
     * keyboard/mouse device calls SetDataFormat too (almost always the
     * standard c_dfDIKeyboard/c_dfDIMouse*, both real DIDATAFORMATs with
     * dwDataSize 256 / sizeof(DIMOUSESTATE*)), so d->rgodf/d->dataSize get
     * populated for THOSE devices as well. Without checking size first, the
     * format-aware joystick path below would wrongly match a same-sized
     * keyboard/mouse format and feed it joystick button data instead of
     * real key/mouse state. Only a size that ISN'T one of these falls
     * through to the joystick's own format-aware handling. */
    if (cb == 256) {                                              /* keyboard */
        fill_keyboard_state((BYTE*)data);
        return DI_OK;
    }
    if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) {  /* mouse */
        DIMOUSESTATE *ms = (DIMOUSESTATE*)data;
        static LONG s_last_x = -1, s_last_y = -1;
        POINT pt; GetCursorPos(&pt);
        if (s_last_x >= 0) { ms->lX = pt.x - s_last_x; ms->lY = pt.y - s_last_y; }
        s_last_x = pt.x; s_last_y = pt.y;
        ms->rgbButtons[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 0x80 : 0;
        ms->rgbButtons[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 0x80 : 0;
        ms->rgbButtons[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 0x80 : 0;
        return DI_OK;
    }

    /* Everything else is the joystick/wheel device. The game's own custom
     * format takes priority whenever one was actually set for THIS handle
     * and matches the size it's now asking for -- see
     * write_formatted_joystick()'s header above. */
    if (d->rgodf && d->numObjs && d->dataSize == cb) {
        if (!s_gds_joy_logged) { s_gds_joy_logged = 1; LOG("GetDeviceState: joystick, custom-format path, cb=%lu\n", cb); }
        write_formatted_joystick(d, cb, data);
        return DI_OK;
    }
    if (cb >= sizeof(DIJOYSTATE)) {              /* wheel, no custom format seen -- fallback */
        if (!s_gds_joy_logged) { s_gds_joy_logged = 1; LOG("GetDeviceState: joystick, DIJOYSTATE fallback path, cb=%lu (no custom format matched)\n", cb); }
        DIJOYSTATE *js = (DIJOYSTATE*)data;
        for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;   /* no hat pressed */
        BYTE btn[8]; LONG x, s;
        compute_wheel_axes(&x, &s, btn);
        js->lX = x;
        js->lY = js->lZ = js->lRx = js->lRy = js->lRz = 32767;     /* unused axes, centered */
        js->rglSlider[0] = s;
        js->rglSlider[1] = s;   /* hedge: in case the engine's "SLIDER1" is 1-indexed */
        for (int i = 0; i < 6; ++i) js->rgbButtons[i] = btn[i];
        return DI_OK;
    }
    if (!s_gds_joy_logged) { s_gds_joy_logged = 1; LOG("GetDeviceState: joystick, UNRECOGNISED size cb=%lu (dataSize=%lu) -- returning zeroed buffer, silently\n", cb, d->dataSize); }
    return DI_OK;   /* unrecognised/small format: already zeroed above */
}

/* 2026-08-05 diagnostic (added after live testing showed object[1] and
 * object[2] BOTH tracking pedal-shaped values -- proof the "first axis-type
 * object seen is steer, every one after is the pedal" positional guess in
 * compute_object_values() is wrong for however many axis-type objects this
 * format actually has). d_EnumObjects is a no-op stub, so there has been no
 * ground truth about the game's real object list -- only inference from
 * which byte offsets change. Print the REAL list once, so the next log shows
 * exactly what Ford Racing declared instead of guessing from behavior. */
static const char *object_type_name(DWORD type) {
    if (type & DIDFT_BUTTON)  return "BUTTON";
    if (type & DIDFT_POV)     return "POV";
    if (type & DIDFT_ABSAXIS) return "ABSAXIS";
    if (type & DIDFT_RELAXIS) return "RELAXIS";
    return "OTHER";
}
static const char *object_guid_name(const GUID *g) {
    if (!g)                                     return "NULL";
    if (!memcmp(g, &GUID_XAxis,  sizeof(GUID))) return "GUID_XAxis";
    if (!memcmp(g, &GUID_YAxis,  sizeof(GUID))) return "GUID_YAxis";
    if (!memcmp(g, &GUID_ZAxis,  sizeof(GUID))) return "GUID_ZAxis";
    if (!memcmp(g, &GUID_RxAxis, sizeof(GUID))) return "GUID_RxAxis";
    if (!memcmp(g, &GUID_RyAxis, sizeof(GUID))) return "GUID_RyAxis";
    if (!memcmp(g, &GUID_RzAxis, sizeof(GUID))) return "GUID_RzAxis";
    if (!memcmp(g, &GUID_Slider, sizeof(GUID))) return "GUID_Slider";
    if (!memcmp(g, &GUID_Button, sizeof(GUID))) return "GUID_Button";
    if (!memcmp(g, &GUID_POV,    sizeof(GUID))) return "GUID_POV";
    if (!memcmp(g, &GUID_Key,    sizeof(GUID))) return "GUID_Key";
    return "unknown-guid";
}
static void dump_data_format(ul_dev *d) {
    for (DWORD i = 0; i < d->numObjs; ++i) {
        DIOBJECTDATAFORMAT *o = &d->rgodf[i];
        LOG("  object[%lu]: ofs=%lu type=0x%08lX (%s) instance=%lu guid=%s\n",
            i, o->dwOfs, o->dwType, object_type_name(o->dwType),
            (unsigned long)DIDFT_GETINSTANCE(o->dwType), object_guid_name(o->pguid));
    }
}

static HRESULT STDMETHODCALLTYPE d_SetDataFormat(IDirectInputDevice8A *T, LPCDIDATAFORMAT df) {
    ul_dev *d = (ul_dev*)T;
    log_caller_once("SetDataFormat", __builtin_return_address(0));
    free(d->rgodf); d->rgodf = NULL; d->numObjs = 0; d->dataSize = 0;
    free(d->last_val); d->last_val = NULL; d->seq = 0;
    if (df && df->rgodf && df->dwNumObjs) {
        d->rgodf = (DIOBJECTDATAFORMAT*)calloc(df->dwNumObjs, sizeof(DIOBJECTDATAFORMAT));
        if (d->rgodf) {
            memcpy(d->rgodf, df->rgodf, (size_t)df->dwNumObjs * sizeof(DIOBJECTDATAFORMAT));
            d->numObjs  = df->dwNumObjs;
            d->dataSize = df->dwDataSize;
            LOG("SetDataFormat: %lu objects, dwDataSize=%lu\n", d->numObjs, d->dataSize);
            if (d->dataSize != 256) dump_data_format(d);   /* skip the keyboard's 256-object dump */
        }
    }
    return DI_OK;
}
/* 2026-08-05 fix (same audit as GetCaps/EnumObjects above): used to store
 * nothing and never signal. If the game's real input-reading architecture is
 * event-driven (a worker thread blocked in WaitForSingleObject(hEvent) that
 * only calls GetDeviceData AFTER the event fires, rather than pure per-frame
 * polling) that thread would never wake up at all -- our data could be
 * perfectly correct and it would never be read. Store the handle and signal
 * it from d_Poll() below: this device declares DIDC_POLLEDDEVICE (see
 * GetCaps), which is itself a contract that the application calls Poll()
 * regularly, so piggybacking the signal there reaches an event-driven
 * consumer on roughly the same cadence a polling one would already get. */
static HRESULT STDMETHODCALLTYPE d_SetEventNotification(IDirectInputDevice8A *T, HANDLE h) {
    ul_dev *d = (ul_dev*)T;
    d->notifyEvent = h;
    if (h) LOG("SetEventNotification: event handle registered\n");
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SetCoopLevel(IDirectInputDevice8A *T, HWND w, DWORD fl) { (void)T; (void)w; (void)fl; log_caller_once("SetCooperativeLevel", __builtin_return_address(0)); return DI_OK; }

/* 2026-08-05 fix (same audit): used to leave the caller's LPDIDEVICEOBJECTINSTANCEA
 * completely untouched -- the same class of garbage-buffer bug just fixed in
 * GetProperty. Fills real data (matched against the game's own negotiated
 * format by offset) when possible; otherwise zeroes the struct instead of
 * leaving it as whatever was on the caller's stack. */
static HRESULT STDMETHODCALLTYPE d_GetObjectInfo(IDirectInputDevice8A *T, LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h) {
    ul_dev *d = (ul_dev*)T;
    if (!i) return E_POINTER;
    DWORD sz = i->dwSize; memset(i, 0, sz); i->dwSize = sz;
    if (h == DIPH_BYOFFSET && d->rgodf) {
        for (DWORD idx = 0; idx < d->numObjs; ++idx) {
            if (d->rgodf[idx].dwOfs != o) continue;
            DWORD type = d->rgodf[idx].dwType;
            i->dwOfs  = o;
            i->dwType = type;
            if (type & DIDFT_BUTTON) {
                i->guidType = GUID_Button;
                snprintf(i->tszName, MAX_PATH, "Button %lu", (unsigned long)DIDFT_GETINSTANCE(type));
            } else if (type & DIDFT_POV) {
                i->guidType = GUID_POV;
                strncpy(i->tszName, "POV", MAX_PATH - 1);
            } else {
                i->guidType = d->rgodf[idx].pguid ? *d->rgodf[idx].pguid : GUID_XAxis;
                strncpy(i->tszName, "Axis", MAX_PATH - 1);
            }
            return DI_OK;
        }
    }
    return DI_OK;   /* unmatched: zeroed struct, not garbage */
}
static HRESULT STDMETHODCALLTYPE d_GetDeviceInfo(IDirectInputDevice8A *T, LPDIDEVICEINSTANCEA i) {
    (void)T;
    if (i) {
        DWORD sz = i->dwSize; memset(i, 0, sz); i->dwSize = sz;
        i->guidInstance = g_wheel_instance;
        i->guidProduct  = g_wheel_product;
        i->dwDevType    = DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_COMBINEDPEDALS << 8);
        strncpy(i->tszInstanceName, "Ultra Loader Wheel", MAX_PATH - 1);
        strncpy(i->tszProductName,  "Ultra Loader Wheel", MAX_PATH - 1);
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_RunControlPanel(IDirectInputDevice8A *T, HWND w, DWORD fl) { (void)T; (void)w; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Initialize(IDirectInputDevice8A *T, HINSTANCE h, DWORD v, REFGUID g) { (void)T; (void)h; (void)v; (void)g; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_CreateEffect(IDirectInputDevice8A *T, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT *ef, LPUNKNOWN u)
{ (void)T; (void)g; (void)e; (void)u; if (ef) *ef = NULL; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_EnumEffects(IDirectInputDevice8A *T, LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t)
{ (void)T; (void)cb; (void)r; (void)t; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetEffectInfo(IDirectInputDevice8A *T, LPDIEFFECTINFOA i, REFGUID g) { (void)T; (void)i; (void)g; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_GetFFState(IDirectInputDevice8A *T, LPDWORD o) { (void)T; if (o) *o = 0; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SendFFCmd(IDirectInputDevice8A *T, DWORD c) { (void)T; (void)c; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_EnumCreated(IDirectInputDevice8A *T, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD fl)
{ (void)T; (void)cb; (void)r; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Escape(IDirectInputDevice8A *T, LPDIEFFESCAPE e) { (void)T; (void)e; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Poll(IDirectInputDevice8A *T) {
    ul_dev *d = (ul_dev*)T;
    log_caller_once("Poll", __builtin_return_address(0));
    if (d->notifyEvent) SetEvent(d->notifyEvent);   /* see d_SetEventNotification's header above */
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SendDeviceData(IDirectInputDevice8A *T, DWORD cb, LPCDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{ (void)T; (void)cb; (void)od; (void)io; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_EnumEffectsInFile(IDirectInputDevice8A *T, LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID r, DWORD fl)
{ (void)T; (void)f; (void)cb; (void)r; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_WriteEffectToFile(IDirectInputDevice8A *T, LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl)
{ (void)T; (void)f; (void)n; (void)e; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_BuildActionMap(IDirectInputDevice8A *T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl)
{ (void)T; (void)a; (void)u; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetActionMap(IDirectInputDevice8A *T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl)
{ (void)T; (void)a; (void)u; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetImageInfo(IDirectInputDevice8A *T, LPDIDEVICEIMAGEINFOHEADERA h) { (void)T; (void)h; return DI_OK; }

static const IDirectInputDevice8AVtbl g_dev_vtbl = {
    d_QI, d_AddRef, d_Release,
    d_GetCaps, d_EnumObjects, d_GetProperty, d_SetProperty,
    d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData,
    d_SetDataFormat, d_SetEventNotification, d_SetCoopLevel,
    d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize,
    d_CreateEffect, d_EnumEffects, d_GetEffectInfo, d_GetFFState,
    d_SendFFCmd, d_EnumCreated, d_Escape, d_Poll, d_SendDeviceData,
    d_EnumEffectsInFile, d_WriteEffectToFile,
    d_BuildActionMap, d_SetActionMap, d_GetImageInfo
};

static IDirectInputDevice8A *make_device(void) {
    ul_dev *d = (ul_dev*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->lpVtbl   = &g_dev_vtbl;
    d->ref      = 1;
    d->rangeMin = 0;       /* DirectInput's own default axis range until SetProperty(DIPROP_RANGE) changes it */
    d->rangeMax = 65535;
    return (IDirectInputDevice8A*)d;
}

/* =============================================================================
 *  Synthetic IDirectInput8A. EnumDevices reports exactly one joystick, with
 *  guidProduct set to the recognized "Formula GP Racing Wheel" hash -- this
 *  is the field Ford Racing's Controller Database lookup actually reads, so
 *  it must be correct here (CreateDevice itself doesn't need to distinguish
 *  by GUID at all; see the device object above).
 * ============================================================================= */
typedef struct { const IDirectInput8AVtbl *lpVtbl; LONG ref; } ul_fac;

static HRESULT STDMETHODCALLTYPE f_QI(IDirectInput8A *T, REFIID r, void **p)
{ (void)r; if (p) { *p = T; ++((ul_fac*)T)->ref; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE f_AddRef(IDirectInput8A *T) { return (ULONG)++((ul_fac*)T)->ref; }
static ULONG STDMETHODCALLTYPE f_Release(IDirectInput8A *T) {
    ul_fac *f = (ul_fac*)T; LONG r = --f->ref;
    if (r <= 0) { free(f); return 0; }
    return (ULONG)r;
}
static int s_create_device_count = 0;

static HRESULT STDMETHODCALLTYPE f_CreateDevice(IDirectInput8A *T, REFGUID g, LPDIRECTINPUTDEVICE8A *dev, LPUNKNOWN u) {
    (void)T; (void)u;
    ++s_create_device_count;
    log_caller_once("CreateDevice", __builtin_return_address(0));
    int is_mouse = g && !memcmp(g, &GUID_SysMouse, sizeof(GUID));
    LOG("CreateDevice: call #%d, requested guid Data1=0x%08lX%s\n",
        s_create_device_count, g ? (unsigned long)g->Data1 : 0, is_mouse ? " (GUID_SysMouse)" : "");
    if (!dev) return E_POINTER;
    IDirectInputDevice8A *newdev = make_device();
    if (newdev) ((ul_dev*)newdev)->is_mouse = is_mouse;
    *dev = newdev;
    return *dev ? DI_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE f_EnumDevices(IDirectInput8A *T, DWORD dtype, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD fl) {
    (void)T; (void)dtype; (void)fl;
    if (cb) {
        DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
        inst.guidInstance = g_wheel_instance;
        inst.guidProduct  = g_wheel_product;
        inst.dwDevType    = DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_COMBINEDPEDALS << 8);
        strncpy(inst.tszInstanceName, "Ultra Loader Wheel", MAX_PATH - 1);
        strncpy(inst.tszProductName,  "Ultra Loader Wheel", MAX_PATH - 1);
        cb(&inst, ref);
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetDeviceStatus(IDirectInput8A *T, REFGUID g) { (void)T; (void)g; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_RunControlPanel(IDirectInput8A *T, HWND w, DWORD fl) { (void)T; (void)w; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_Initialize(IDirectInput8A *T, HINSTANCE h, DWORD v) { (void)T; (void)h; (void)v; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_FindDevice(IDirectInput8A *T, REFGUID g, LPCSTR n, LPGUID out) { (void)T; (void)g; (void)n; (void)out; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_EnumBySemantics(IDirectInput8A *T, LPCSTR u, LPDIACTIONFORMATA a,
                                                    LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID r, DWORD fl)
{ (void)T; (void)u; (void)a; (void)cb; (void)r; (void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_ConfigureDevices(IDirectInput8A *T, LPDICONFIGUREDEVICESCALLBACK cb,
                                                     LPDICONFIGUREDEVICESPARAMSA p, DWORD fl, LPVOID r)
{ (void)T; (void)cb; (void)p; (void)fl; (void)r; return DI_OK; }

static const IDirectInput8AVtbl g_fac_vtbl = {
    f_QI, f_AddRef, f_Release,
    f_CreateDevice, f_EnumDevices,
    f_GetDeviceStatus, f_RunControlPanel, f_Initialize,
    f_FindDevice, f_EnumBySemantics, f_ConfigureDevices
};

/* =============================================================================
 *  Exported entry point -- this is what fordracing.exe finds when it does
 *  GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create").
 * ============================================================================= */
__declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN unk) {
    (void)inst; (void)ver; (void)riid; (void)unk;
    LOG("DirectInput8Create() called -- Ford Racing's own dynamic load reached our proxy\n");
    if (!out) return E_POINTER;
    ul_fac *f = (ul_fac*)calloc(1, sizeof *f);
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_fac_vtbl;
    f->ref    = 1;
    *out = f;
    return DI_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) {
    (void)h; (void)v;
    if (r == DLL_PROCESS_ATTACH) LOG("dinput8.dll proxy loaded into game process\n");
    return TRUE;
}
