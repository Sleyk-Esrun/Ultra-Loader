/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  happio_europar.c -- Sega Europa-R (Ford Racing) HAPP UGCI raw-HID emulation.
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS (2026-08-05, user directive: "building raw-HID
 *  emulation layer is better since it will work with all racing games"):
 *
 *  dinput8_europar.c's DirectInput proxy was built on the assumption that
 *  Ford Racing reads its wheel/pedals/buttons through DirectInput -- it
 *  doesn't. Static disassembly of fordracing.exe (objdump, no live debugger
 *  available or needed) proved the game's real "Joystick System"
 *  (.\Input\Pc\Joystick.c, per its own embedded debug strings) talks
 *  DIRECTLY to a HAPP Controls UGCI (Universal Game Control Interface)
 *  board over raw USB HID -- SetupDiGetClassDevsA/SetupDiEnumDeviceInterfaces/
 *  SetupDiGetDeviceInterfaceDetailA/CreateFileA/ReadFile -- with ZERO
 *  DirectInput involvement. No DirectInput proxy, however correct, could
 *  ever reach this path; dinput8_europar.c remains in place because it's
 *  still what drives keyboard/menu input and (for boards that genuinely do
 *  use DirectInput for driving) is the right layer -- Ford Racing's
 *  DRIVING controls specifically just aren't one of those.
 *
 *  THE FULLY REVERSE-ENGINEERED PROTOCOL (traced byte-by-byte from the
 *  game's own enumeration and polling-thread code, not guessed):
 *
 *    1. HidD_GetHidGuid(&guid)                         -- the well-known
 *       generic HID class GUID (4D1E55B2-F16F-11CF-88CB-001111000030), not
 *       anything HAPP-specific.
 *    2. SetupDiGetClassDevsA(&guid, NULL, NULL,
 *                            DIGCF_PRESENT|DIGCF_DEVICEINTERFACE /*0x12*(/)
 *       -- enumerate every present HID device on the system.
 *    3. SetupDiEnumDeviceInterfaces() in a loop (MemberIndex 0,1,2,...)
 *       until it fails with ERROR_NO_MORE_ITEMS.
 *    4. SetupDiGetDeviceInterfaceDetailA() per interface, standard two-pass
 *       (NULL buffer to get required size, then a real call) -- yields each
 *       device's DevicePath string.
 *    5. CreateFileA(path, GENERIC_READ|GENERIC_WRITE,
 *                   FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
 *                   0, NULL) on EVERY enumerated HID device.
 *    6. HidD_GetAttributes(handle, &attrs) on each -- the game keeps the
 *       ONE whose VendorID==0x078B && ProductID==0x0010 (confirmed twice
 *       independently: the literal ASCII string "USB\VID_078B&PID_0010"
 *       lives in .rdata, AND the live comparison code does
 *       `cmp $0x78b, <vid>` / `cmp $0x10, <pid>` directly against the
 *       HIDD_ATTRIBUTES fields). VID 0x078B is registered to HAPP Controls,
 *       Inc.; this exact VID:PID pair is documented publicly as HAPP's UGCI
 *       "Flying"-class control interface (see suzohapp.com's UGCI manuals --
 *       na.suzohapp.com/images/pdf/ugci.pdf, ins-0037.pdf -- which describe
 *       UGCI as "completely USB HID compliant"; the manuals don't publish
 *       the exact byte layout, so bytes 1-5 below are this file's own
 *       reverse-engineering from fordracing.exe's OWN consumer code, not
 *       from HAPP's documentation).
 *    7. HidD_GetPreparsedData + HidP_GetCaps as a sanity check (report
 *       length, usage page/usage) -- NOT used for dynamic axis/button
 *       discovery. Confirmed: HID.DLL's ENTIRE import table for this exe is
 *       exactly {HidD_GetAttributes, HidD_GetPreparsedData, HidP_GetCaps,
 *       HidD_GetHidGuid} -- no HidP_GetButtonCaps/GetValueCaps/GetUsages
 *       anywhere. The game does NOT dynamically discover axis/button
 *       semantics via the HID parser API; it reads RAW REPORT BYTES with a
 *       layout it already knows in advance. This is what makes emulation
 *       tractable without replicating Microsoft's undocumented, internal
 *       PHIDP_PREPARSED_DATA binary format: HidP_GetCaps is the only
 *       HidP_* call made at all, so it's the only one that needs its own
 *       fake-pointer-aware hook (see Hook_HidP_GetCaps below); everything
 *       past it is plain ReadFile.
 *    8. A DEDICATED POLLING THREAD then loops: ReadFile(handle, buf8, 8,
 *       &n, NULL). buf[6]==7 is treated as a "not ready" sentinel (skip,
 *       retry immediately, no data processed); any other ReadFile failure
 *       sleeps 500ms before retrying. On a real 8-byte read with
 *       buf[6]!=7, the thread takes a named "HAPP" mutex/event, copies
 *       buf[1..5] verbatim into a staging area, and OR-ACCUMULATES buf[6]
 *       and buf[7] (a 16-bit button bitmask, buf[6]=buttons 0-7,
 *       buf[7]=buttons 8-15) onto whatever was already staged -- a classic
 *       "catch brief taps between consumer polls" producer pattern. Bit
 *       0x08 of buf[6] additionally gets its OWN separate one-shot
 *       edge-latch into two extra globals, independent of the general
 *       bitmask -- CONFIRMED LIVE 2026-08-05 to be CHANGE_VIEW, not
 *       START/COIN as originally guessed here (a camera-view cycle wants
 *       exactly this kind of one-shot-per-press edge handling too, so the
 *       "why is this bit special" reasoning was right, just not the specific
 *       action; see happ_compute_report()'s own header for the live-test
 *       report that pinned this down).
 *    9. The MAIN game thread drains the staged report by TRY-LOCKING the
 *       same mutex (0ms timeout, non-blocking): if it gets the lock, it
 *       copies out buf[1..5] and the accumulated button bytes, resets the
 *       "accumulating" flag (so the NEXT producer write overwrites instead
 *       of OR-accumulating -- i.e. this read just "consumed" the pending
 *       button state), and releases the lock.
 *
 *  buf[0] is never read by this consumer at all (ignored -- almost
 *  certainly a HID report-ID byte on a single-report device, always 0).
 *
 *  2026-08-05 correction (found by going one level deeper -- tracing the
 *  ONE actual caller of the drain function, at 0x4a1af0 in Joystick.c,
 *  after a live test showed only the confirmed CHANGE_VIEW button worked
 *  and nothing analog did): that caller runs a repeating per-axis
 *  calibration loop, `(raw - min) / (max - min)` against a calibration
 *  table this same init code populates from FIXED, non-zero .rdata
 *  constants for exactly THREE axis slots -- sourced from buf[1] (steer),
 *  buf[3], and buf[5] -- while every other axis slot's calibration min/max
 *  was left at the zero this init code's own memset put there and never
 *  set, making them vestigial for this board profile regardless of what's
 *  written to their source bytes. buf[2] and buf[4] are NOT among the
 *  three real, calibrated inputs -- this file originally (wrongly) put the
 *  combined accel/brake value in buf[2], which is why steering may have
 *  had a chance to work but pedals never could. Fixed: buf[1]=steering,
 *  buf[3]=accelerator, buf[5]=brake (independent signals, not combined --
 *  Ford Racing apparently reads real separate accel/brake pots, not a
 *  single combined pedal), buf[2]/buf[4] centered/unused. Button-bit
 *  assignments beyond bit3 (=CHANGE_VIEW, confirmed live -- see
 *  happ_compute_report()'s own header) remain this file's own best-effort
 *  assignment, NOT independently re-derived per bit from the disassembly
 *  (that would require finding and decoding Ford Racing's
 *  higher-level "interpret these bytes as gear/handbrake/view/boost"
 *  function, a further reverse-engineering pass, not attempted this pass).
 *  Flip EUROPAR_HAPP_STEER_INVERT/PEDAL_INVERT or remap HAPP_BTN_* below if
 *  live testing shows any of these are wrong -- same pattern as this
 *  board's other files (see dinput8_europar.c's EUROPAR_STEER_INVERT).
 *
 *  MECHANISM: this is a raw Win32 API interception -- SetupAPI/HID.DLL/
 *  kernel32 IAT hooks installed process-wide via THIS BOARD'S OWN
 *  ultra_iat_patch_all_europar() (owned by jvs_serial_hook_europar.c,
 *  already linked into this same DLL; reused directly, not reimplemented --
 *  unlike dinput8_europar.c's relationship to native_x86, THIS file and
 *  jvs_serial_hook_europar.c are siblings in the SAME board's SAME injected
 *  hook DLL, so sharing that one utility function is the established,
 *  intentional convention this board's other hook files already use, see
 *  audio_hook_europar.c/sega_keychip_europar.c). Real SetupDiEnumDevice-
 *  Interfaces/SetupDiGetClassDevsA/HidD_GetHidGuid calls are left
 *  UNTOUCHED (real HID devices, if any, keep enumerating normally); only
 *  SetupDiEnumDeviceInterfaces is hooked, to inject ONE extra synthetic
 *  entry once the real enumeration is exhausted, and only calls that touch
 *  THAT specific synthetic device (recognized via a sentinel Reserved value
 *  on the way in, then a real-but-repurposed HANDLE from CreateEventA once
 *  "opened", then a heap-allocated sentinel pointer for the fake preparsed
 *  data) get answered here; everything else passes straight through to the
 *  real API, so genuine HID devices (a real keyboard's HID collection,
 *  etc.) are entirely unaffected.
 *
 *  This technique generalizes directly to any other PC-based arcade
 *  cabinet conversion using the same HAPP UGCI board (a very plausible
 *  shared platform across Sega's whole Europa-R lineup: fordracing.exe's
 *  own embedded PDB path is "c:\sega\source\Release\sega_arcade.pdb" --
 *  literally Sega's own shared arcade-PC codebase, not a Ford-Racing-
 *  specific one). Porting to another board: copy this file, confirm the
 *  target's VID:PID (may not be 078B:0010 -- HAPP ships several UGCI
 *  variants for different control classes), and re-derive the analog/
 *  button byte assignments the same way (find the target's own consumer
 *  function via objdump, exactly as this file's own header explains).
 * =============================================================================
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <xinput.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define LOG(...) do { fprintf(stderr, "[sega_europar_happ] " __VA_ARGS__); fflush(stderr); } while (0)

/* owned by jvs_serial_hook_europar.c, already linked into this same DLL */
void ultra_iat_patch_all_europar(const char *impdll, const char *impfn, void *newfn);

#define HAPP_VID          0x078Bu
#define HAPP_PID          0x0010u
#define HAPP_REPORT_LEN   8
#define HAPP_REPORT_LEN_MAX 15   /* the longest profile (segarally3) */

/* PER-GAME I/O IDENTITY AND REPORT SHAPE.
 *
 * These were compile-time constants for the whole board, which was wrong the
 * moment a second title appeared on it. Sega Rally 3 renders fine and has NO
 * controls at all for exactly this reason: Rally.exe enumerates raw HID itself
 * and keeps ONLY the device whose VendorID==0x1043 && ProductID==0x0000
 * (Rally.exe 0x57b45e / 0x57b467); every other device is CloseHandle()d at
 * 0x57b47f, and if nothing matched it aborts the whole joystick subsystem at
 * 0x57b4af and never starts its input thread. We answered with Ford Racing's
 * HAPP UGCI identity, so our device was opened, rejected and thrown away.
 *
 * That enumeration is also ONE-SHOT and latched (0x57baa0-0x57baf6: it marks
 * 'attempted' before it checks the result and returns early forever after), so
 * there is no retry and no hot-plug -- the right identity has to be in place
 * before the game's first frame. happio_europar_install() runs at
 * DLL_PROCESS_ATTACH in a suspended process, which satisfies that.
 *
 * Defaults are Ford Racing's existing values, so Ford cannot move.
 *
 *   ULTRA_EUROPAR_HID_VID       default 0x078B
 *   ULTRA_EUROPAR_HID_PID       default 0x0010
 *   ULTRA_EUROPAR_HAPP_PROFILE  fordracing | segarally3   (default fordracing)
 *   ULTRA_EUROPAR_HAPPIO        0 disables the emulation entirely
 */
#define HAPP_PROFILE_FORDRACING 0
#define HAPP_PROFILE_SEGARALLY3 1

static int happ_profile(void) {
    static int p = -1;
    if (p < 0) {
        const char *e = getenv("ULTRA_EUROPAR_HAPP_PROFILE");
        p = (e && (*e == 's' || *e == 'S')) ? HAPP_PROFILE_SEGARALLY3
                                            : HAPP_PROFILE_FORDRACING;
    }
    return p;
}
static DWORD happ_report_len(void) {
    return happ_profile() == HAPP_PROFILE_SEGARALLY3 ? 15u : (DWORD)HAPP_REPORT_LEN;
}
static unsigned happ_env_hex(const char *k, unsigned dflt) {
    const char *e = getenv(k);
    if (!e || !*e) return dflt;
    return (unsigned)strtoul(e, NULL, 0);   /* accepts 0x1043 and 4163 alike */
}
static unsigned happ_vid(void) {
    static unsigned v = 0xFFFFFFFFu;
    if (v == 0xFFFFFFFFu) v = happ_env_hex("ULTRA_EUROPAR_HID_VID", HAPP_VID);
    return v;
}
static unsigned happ_pid(void) {
    static unsigned v = 0xFFFFFFFFu;
    if (v == 0xFFFFFFFFu) v = happ_env_hex("ULTRA_EUROPAR_HID_PID", HAPP_PID);
    return v;
}
#define HAPP_FAKE_MAGIC   0x55484150u   /* 'UHAP', tags our synthetic SP_DEVICE_INTERFACE_DATA */

/* Pedal raw values matching data/Calibration.ini ([Accelerator] Up=35/Down=216,
 * [Brake] Up=39/Down=193). "Up" = pedal released (idle), "Down" = floored. The
 * game normalizes (raw-Up)/(Down-Up), so emitting the calibrated endpoints makes
 * rest == 0 throttle and press == full, instead of 0x80/0xFF (which read ~50%
 * throttle at rest through that same calibration). */
#define EUROPAR_ACCEL_IDLE  35
#define EUROPAR_ACCEL_FULL  216
#define EUROPAR_BRAKE_IDLE  39
#define EUROPAR_BRAKE_FULL  193

static const char HAPP_FAKE_PATH[] =
    "\\\\?\\hid#vid_078b&pid_0010#ultra_happ_emulated#{4d1e55b2-f16f-11cf-88cb-001111000030}";

#define EUROPAR_HAPP_STEER_INVERT  0
#define EUROPAR_HAPP_PEDAL_INVERT  0

/* ---- real function pointers, resolved once at install time --------------- */
typedef BOOL     (WINAPI *pSetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL     (WINAPI *pSetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOLEAN  (WINAPI *pHidD_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN  (WINAPI *pHidD_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA *);
typedef NTSTATUS (WINAPI *pHidP_GetCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef HANDLE   (WINAPI *pCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL     (WINAPI *pReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

static pSetupDiEnumDeviceInterfaces        Real_SetupDiEnumDeviceInterfaces;
static pSetupDiGetDeviceInterfaceDetailA   Real_SetupDiGetDeviceInterfaceDetailA;
static pHidD_GetAttributes                 Real_HidD_GetAttributes;
static pHidD_GetPreparsedData              Real_HidD_GetPreparsedData;
static pHidP_GetCaps                       Real_HidP_GetCaps;
static pCreateFileA                        Real_CreateFileA;
static pReadFile                           Real_ReadFile;

/* ---- per-enumeration-session state ---------------------------------------
 * Tracks, per HDEVINFO handle, whether our synthetic device has already been
 * delivered THIS enumeration pass (SetupDiEnumDeviceInterfaces is called
 * repeatedly with an incrementing MemberIndex; we must inject our fake entry
 * exactly once, right when the real enumeration reports exhaustion, not on
 * every subsequent call). Bounded array, not a hash map -- this only ever
 * needs to hold as many concurrent enumerations as a game genuinely runs,
 * which is 1 in every case seen so far. */
#define MAX_TRACKED_DEVINFO 8
static HDEVINFO g_tracked[MAX_TRACKED_DEVINFO];
static int      g_tracked_delivered[MAX_TRACKED_DEVINFO];
static int      g_tracked_count = 0;

static HANDLE g_fake_handle    = NULL;   /* our synthetic device's HANDLE once "opened" */
static void  *g_fake_preparsed = NULL;   /* our synthetic device's fake PHIDP_PREPARSED_DATA */

/* ---- input reading --------------------------------------------------------
 * Independent copy of dinput8_europar.c's read_wheel_input()/xinput_find_pad()
 * -- same technique, deliberately not shared code: this file builds into the
 * INJECTED ultra_sega_europar_hook32.dll, dinput8_europar.c into the
 * file-swapped dinput8.CRK.dll, two separate DLLs with no runtime link
 * between them, matching this board's own established per-file-independence
 * convention (see e.g. pause_menu_wire_europar.c's header). */
static int s_xi_index = -1;

static int xinput_find_pad(XINPUT_STATE *xs) {
    if (s_xi_index >= 0 && s_xi_index <= 3) {
        if (XInputGetState((DWORD)s_xi_index, xs) == ERROR_SUCCESS) return 1;
        s_xi_index = -1;
    }
    for (DWORD i = 0; i < 4; ++i) {
        memset(xs, 0, sizeof *xs);
        if (XInputGetState(i, xs) == ERROR_SUCCESS) { s_xi_index = (int)i; return 1; }
    }
    return 0;
}

/* Builds one 8-byte HAPP UGCI input report matching the exact layout
 * fordracing.exe's own polling-thread code (see this file's header, point 8)
 * copies out of.
 *
 * 2026-08-05 CONFIRMED LIVE (first real signal from this whole layer): user
 * reported "the start button on the gamepad changes the view when we start
 * the race" -- i.e. pressing gamepad Start (which this function ties to
 * bit3, the one bit Ford Racing's own code singles out for special
 * edge-latch handling -- see header point 8) triggers an in-race view
 * change, not a start/pause action. This makes complete sense in hindsight:
 * a camera-view cycle is EXACTLY the kind of action a game wants
 * edge-triggered (one cycle per press, not continuously while held), the
 * same reason coin/start would be -- this file's original guess of
 * "probably START" picked the wrong specific action for the right kind of
 * bit. Relabeled bit3 CHANGE_VIEW accordingly (keeping its proven-working
 * Enter/gamepad-Start trigger unchanged) and freed the other, now-redundant
 * view binding (bit2, was also 'C'/Y) for LOOK_BEHIND instead, matching
 * dinput8_europar.c's own distinct GEAR/VIEW/LOOK_BEHIND scheme. Every
 * OTHER bit below remains an unconfirmed best-effort guess. */
static void happ_compute_report(BYTE out[HAPP_REPORT_LEN_MAX]) {
    memset(out, 0, happ_report_len());

    int left  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) || (GetAsyncKeyState('A') & 0x8000);
    int right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState('D') & 0x8000);
    int accel = (GetAsyncKeyState(VK_UP)    & 0x8000) || (GetAsyncKeyState('W') & 0x8000);
    int brake = (GetAsyncKeyState(VK_DOWN)  & 0x8000) || (GetAsyncKeyState('S') & 0x8000);

    /* Only buf[6] BITS 0..3 are read by the game (its drain extracts exactly
     * those four into internal button indices; bits 4..7 and all of buf[7] are
     * copied but never inspected -- see europar_patch_gear_buttons()). With the
     * gear patch active the four live bits resolve to real actions:
     *   bit0 -> button0 = GEAR_DOWN   (patched from BOOST)
     *   bit1 -> button1 = GEAR_UP     (patched from unused button6)
     *   bit2 -> button2 = LOOK_BEHIND
     *   bit3 -> button4 = CHANGE_VIEW (confirmed live)
     * Bits 4..7 below are retained for completeness/telemetry but are INERT in
     * this game build (nothing reads them); handbrake/boost have no free HAPP
     * bit to live on. Coin/service reach the game through other paths, not here. */
    WORD buttons = 0;
    if (GetAsyncKeyState('Z')        & 0x8000) buttons |= 0x0001;   /* bit0 GEAR_DOWN (live) */
    if (GetAsyncKeyState('X')        & 0x8000) buttons |= 0x0002;   /* bit1 GEAR_UP   (live) */
    if (GetAsyncKeyState('V')        & 0x8000) buttons |= 0x0004;   /* bit2 = btn2 = START (was mislabelled LOOK_BEHIND) */
    if ((GetAsyncKeyState(VK_RETURN) & 0x8000) ||
        (GetAsyncKeyState('C')       & 0x8000)) buttons |= 0x0008;  /* bit3 CHANGE_VIEW (live) */
    if (GetAsyncKeyState(VK_SPACE)   & 0x8000) buttons |= 0x0010;   /* bit4 HANDBRAKE (inert) */
    if (GetAsyncKeyState(VK_LSHIFT)  & 0x8000) buttons |= 0x0020;   /* bit5 BOOST (inert)     */
    if (GetAsyncKeyState('5')        & 0x8000) buttons |= 0x0040;   /* bit6 COIN (inert here) */
    if (GetAsyncKeyState(VK_F2)      & 0x8000) buttons |= 0x0080;   /* bit7 SERVICE (inert)   */

    XINPUT_STATE xs; memset(&xs, 0, sizeof xs);
    if (xinput_find_pad(&xs)) {
        SHORT lx = xs.Gamepad.sThumbLX;
        if (lx < -8000) left  = 1;
        if (lx >  8000) right = 1;
        if (xs.Gamepad.bRightTrigger > 30) accel = 1;
        if (xs.Gamepad.bLeftTrigger  > 30) brake = 1;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  buttons |= 0x0001;   /* GEAR_DOWN */
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) buttons |= 0x0002;   /* GEAR_UP   */
        /* START LIVES ON BIT 2, NOT BIT 3. Verified from fordracing.exe, 2026-09-20.
         *
         * The start/credit gate at 0x00435360 fires on joystick BUTTON INDEX 2:
         *     83 F8 02     cmp  eax,2            ; event code == button index 2
         *     83 79 08 02  cmp  dword [ecx+8],2  ; && class 2 (a control event)
         *     E8 ..        call 0x004678c0       ; has_credits()
         *     E8 ..        call 0x00467950       ; spend_credit()
         *     C7 05 2C C4 54 00 01000000         ; [0x54c42c] = 1  -- START pressed
         * and the report decoder at 0x004a1af0 maps byte6 bit2 -> [esp+0x4a] -> index
         * 2, byte6 bit3 -> [esp+0x4c] -> index 4. We had pad START on bit3, i.e. on
         * button 4 -- which is CHANGE_VIEW. That is exactly the 2026-08-05 live-test
         * note in this file's header ("the start button on the gamepad changes the
         * view"): the observation was right, the conclusion drawn from it was wrong.
         * Pad START could never start a race, because it was never on START's bit.
         *
         * PROFILE-SCOPED ON PURPOSE. `buttons` is built here, BEFORE the per-game
         * report split further down, so an ungated edit rewrites Sega Rally 3's
         * out[6] too -- and Rally has no ULTRA_EUROPAR_HAPPIO=0 in its container, so
         * this code really does run for it. Rally keeps its existing bits untouched. */
        if (happ_profile() == HAPP_PROFILE_FORDRACING) {
            if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_START)          buttons |= 0x0004; /* btn2 = START/credit gate */
            if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_Y)              buttons |= 0x0008; /* btn4 = CHANGE_VIEW       */
        } else {
            if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_Y)              buttons |= 0x0004;
            if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_START)          buttons |= 0x0008;
        }
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_B)              buttons |= 0x0010;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_X)              buttons |= 0x0020;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)           buttons |= 0x0040;
    }

    /* --- Steering: spring-return analog ramp -------------------------------
     * A keyboard/D-pad press is digital, but the game feeds buf[1] into a real
     * analog wheel-position calibration, and the FRONT-END menus navigate off
     * that wheel position. Slamming instant full lock (0x00/0xFF) makes a single
     * tap jump many menu items. Emulate a real spring wheel: ramp toward the held
     * direction, spring back to center when released. A quick tap = a small brief
     * deflection (one menu step); a sustained hold still reaches full lock for
     * real cornering. Tunable at runtime (no rebuild):
     *   ULTRA_EUROPAR_STEER_RAMP_MS    center->full-lock time    (default 450)
     *   ULTRA_EUROPAR_STEER_RETURN_MS  full-lock->center spring  (default 130) */
    BYTE steer;
    {
        static double s_pos  = 128.0;   /* persistent wheel position, 0..255 */
        static DWORD  s_last = 0;
        static int    s_cfg  = 0;
        static double ramp_per_ms = 255.0 / 450.0;
        static double ret_per_ms  = 255.0 / 130.0;
        if (!s_cfg) {
            s_cfg = 1;
            const char *rr = getenv("ULTRA_EUROPAR_STEER_RAMP_MS");
            const char *rb = getenv("ULTRA_EUROPAR_STEER_RETURN_MS");
            double rm = rr ? atof(rr) : 0.0;
            double bm = rb ? atof(rb) : 0.0;
            if (rm >= 30.0) ramp_per_ms = 255.0 / rm;
            if (bm >= 20.0) ret_per_ms  = 255.0 / bm;
        }
        DWORD now = GetTickCount();
        double dt = s_last ? (double)(now - s_last) : 16.0;
        if (dt < 0.0)   dt = 0.0;
        if (dt > 100.0) dt = 100.0;     /* clamp across pauses/breakpoints */
        s_last = now;

        if (left && !right)      s_pos -= ramp_per_ms * dt;
        else if (right && !left) s_pos += ramp_per_ms * dt;
        else {                          /* spring back toward center */
            if (s_pos > 128.0)      { s_pos -= ret_per_ms * dt; if (s_pos < 128.0) s_pos = 128.0; }
            else if (s_pos < 128.0) { s_pos += ret_per_ms * dt; if (s_pos > 128.0) s_pos = 128.0; }
        }
        if (s_pos < 0.0)   s_pos = 0.0;
        if (s_pos > 255.0) s_pos = 255.0;
        steer = (BYTE)(s_pos + 0.5);
    }
#if EUROPAR_HAPP_STEER_INVERT
    steer = 0xFF - steer;
#endif

    /* 2026-08-05 fix (real bug, found by tracing the ONE actual caller of the
     * drain function, 0x4a1af0 in fordracing.exe's Joystick.c): this used to
     * put a single combined accel/brake value in out[2]. That byte is never
     * read by any calibrated axis at all -- the game's own axis-calibration
     * loop pulls its THREE real, non-zero-calibrated inputs from
     * local_buffer[1] (steer, already correct), local_buffer[3], and
     * local_buffer[5]; every other axis slot's calibration min/max was left
     * zeroed by this same init code's own memset and never set, so they're
     * vestigial for this board profile regardless of what's written there.
     * Accel and brake are now two independent signals on the two axes that
     * are actually confirmed live, instead of one combined value on a byte
     * nothing reads. */
    BYTE accel_b = accel ? EUROPAR_ACCEL_FULL : EUROPAR_ACCEL_IDLE;
    BYTE brake_b = brake ? EUROPAR_BRAKE_FULL : EUROPAR_BRAKE_IDLE;
#if EUROPAR_HAPP_PEDAL_INVERT
    accel_b = (BYTE)(EUROPAR_ACCEL_IDLE + EUROPAR_ACCEL_FULL - accel_b);
    brake_b = (BYTE)(EUROPAR_BRAKE_IDLE + EUROPAR_BRAKE_FULL - brake_b);
#endif

    if (happ_profile() == HAPP_PROFILE_SEGARALLY3) {
        /* SEGA RALLY 3. A different cabinet board and a different report:
         * Rally.exe does ReadFile(h, buf, 15) and its decoder swaps brake and
         * accel relative to Ford's layout and reads 24 button bits. It also has
         * NO [CalibrationData] in its config.ini, so the game's own defaults
         * apply (0 released / 255 floored, 0x80 centered) -- Ford's 35/216/39/193
         * endpoints come from Ford's OWN Calibration.ini and must NOT be reused. */
        out[0]  = 0;
        out[1]  = steer;
        out[2]  = 0x80;
        out[3]  = brake ? 0xFF : 0x00;   /* brake BEFORE accel on this board */
        out[4]  = accel ? 0xFF : 0x00;
        out[5]  = 0x80;
        out[6]  = (BYTE)(buttons & 0xFF);
        out[7]  = (BYTE)((buttons >> 8) & 0xFF);
        out[8]  = (BYTE)((buttons >> 16) & 0xFF);
        out[9]  = 0x80;
        out[10] = 0x80;
        out[11] = 0x80;
        out[12] = 0x80;   /* byte 12 IS read as a calibrated analog channel */
        out[13] = 0x00;
        out[14] = 0x00;
        return;
    }
    out[0] = 0;        /* never read by the consumer we traced -- HID report-ID byte */
    out[1] = steer;     /* axis0, confirmed calibrated/used */
    out[2] = 0x80;      /* not read by any calibrated axis -- centered/unused */
    out[3] = accel_b;   /* axis1, confirmed calibrated/used */
    out[4] = 0x80;      /* not read by any calibrated axis -- centered/unused */
    out[5] = brake_b;   /* axis5, confirmed calibrated/used */
    out[6] = (BYTE)(buttons & 0xFF);
    if (out[6] == 7) out[6] = 6;   /* 7 is the game's own "not ready" sentinel */
    out[7] = (BYTE)((buttons >> 8) & 0xFF);
}

/* ---- SetupDiEnumDeviceInterfaces: inject one synthetic entry -------------- */
static BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO devinfo, PSP_DEVINFO_DATA devdata,
                                                     const GUID *cls, DWORD idx, PSP_DEVICE_INTERFACE_DATA out) {
    BOOL r = Real_SetupDiEnumDeviceInterfaces(devinfo, devdata, cls, idx, out);
    if (r) return r;                                    /* a real device -- pass through untouched */

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_ITEMS) { SetLastError(err); return FALSE; }   /* a genuine error */

    int slot = -1;
    for (int i = 0; i < g_tracked_count; ++i) if (g_tracked[i] == devinfo) { slot = i; break; }
    if (slot < 0 && g_tracked_count < MAX_TRACKED_DEVINFO) {
        slot = g_tracked_count++;
        g_tracked[slot] = devinfo;
        g_tracked_delivered[slot] = 0;
    }
    if (slot < 0 || g_tracked_delivered[slot]) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }

    g_tracked_delivered[slot] = 1;
    memset(out, 0, sizeof *out);
    out->cbSize = sizeof *out;
    out->InterfaceClassGuid = *cls;
    out->Flags = SPINT_ACTIVE;
    /* ONLY EVER PRETEND TO BE A HID DEVICE. We used to inject this synthetic
     * entry into EVERY SetupAPI enumeration, including the one the game's own
     * xinput1_3.dll runs over the XUSB interface class. That caller then passed
     * our entry to the REAL SetupDiGetDeviceInterfaceDetailW, which dereferenced
     * Reserved -- our magic cookie, not a pointer -- and took the access
     * violation in DEVOBJ.dll recorded in segarally3.log line 28. */
    {
        static const GUID hidg = {0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
        if (!cls || memcmp(cls, &hidg, sizeof hidg) != 0) {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
    }
    out->Reserved = HAPP_FAKE_MAGIC;
    LOG("SetupDiEnumDeviceInterfaces: injecting synthetic HAPP UGCI device (idx=%lu)\n", idx);
    return TRUE;
}

/* ---- SetupDiGetDeviceInterfaceDetailA: fake DevicePath for our entry ----- */
static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HDEVINFO devinfo, PSP_DEVICE_INTERFACE_DATA ifdata,
                                                          PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD detail_sz,
                                                          PDWORD required, PSP_DEVINFO_DATA devdata) {
    if (ifdata && ifdata->Reserved == HAPP_FAKE_MAGIC) {
        DWORD need = (DWORD)(sizeof(DWORD) + sizeof(HAPP_FAKE_PATH));
        if (required) *required = need;
        if (!detail || detail_sz < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        detail->cbSize = sizeof(DWORD) + sizeof(CHAR);   /* the well-known SP_DEVICE_INTERFACE_DETAIL_DATA_A cbSize quirk */
        memcpy(detail->DevicePath, HAPP_FAKE_PATH, sizeof(HAPP_FAKE_PATH));
        return TRUE;
    }
    return Real_SetupDiGetDeviceInterfaceDetailA(devinfo, ifdata, detail, detail_sz, required, devdata);
}

/* ---- CreateFileA: recognize our fake path, hand back a real-but-repurposed
 * HANDLE (a plain event object) rather than a made-up pointer value -- so
 * anything unexpected the game does with it (CloseHandle, GetHandleInformation)
 * behaves safely instead of touching an invalid handle. ---------------------- */
static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                       DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name && _stricmp(name, HAPP_FAKE_PATH) == 0) {
        if (!g_fake_handle) {
            g_fake_handle = CreateEventA(NULL, FALSE, FALSE, NULL);
            LOG("CreateFileA: opened synthetic HAPP UGCI device -> handle %p\n", (void*)g_fake_handle);
        }
        return g_fake_handle;
    }
    return Real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

/* ---- HidD_GetAttributes: the actual VID:PID match point ------------------- */
static BOOLEAN WINAPI Hook_HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES attr) {
    if (h && h == g_fake_handle && attr) {
        attr->Size = sizeof *attr;
        attr->VendorID  = (USHORT)happ_vid();
        attr->ProductID = (USHORT)happ_pid();
        attr->VersionNumber = 0x0100;
        return TRUE;
    }
    return Real_HidD_GetAttributes(h, attr);
}

/* ---- HidD_GetPreparsedData / HidP_GetCaps: the ONLY HidP_* call this game
 * makes (confirmed via the complete HID.DLL import table -- see this file's
 * header). No HidP_GetButtonCaps/GetValueCaps/GetUsages exist to intercept,
 * so a heap-allocated sentinel pointer is enough; it's never handed to the
 * real hid.dll parser, only ever compared against by our own hook. --------- */
static BOOLEAN WINAPI Hook_HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *out) {
    if (h && h == g_fake_handle && out) {
        if (!g_fake_preparsed) g_fake_preparsed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32);
        *out = (PHIDP_PREPARSED_DATA)g_fake_preparsed;
        return TRUE;
    }
    return Real_HidD_GetPreparsedData(h, out);
}

static NTSTATUS WINAPI Hook_HidP_GetCaps(PHIDP_PREPARSED_DATA pd, PHIDP_CAPS caps) {
    if (pd && (void*)pd == g_fake_preparsed && caps) {
        memset(caps, 0, sizeof *caps);
        caps->Usage           = 0x04;   /* Generic Desktop: Joystick */
        caps->UsagePage       = 0x01;
        caps->InputReportByteLength = (USHORT)happ_report_len();
        return HIDP_STATUS_SUCCESS;
    }
    return Real_HidP_GetCaps(pd, caps);
}

/* ---- ReadFile: deliver the live report ------------------------------------ */
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD read, LPOVERLAPPED ov) {
    if (h && h == g_fake_handle) {
        BYTE report[HAPP_REPORT_LEN_MAX];
        happ_compute_report(report);
        DWORD rl = happ_report_len();
        DWORD n = (len < rl) ? len : rl;
        memcpy(buf, report, n);
        if (read) *read = n;
        return TRUE;
    }
    return Real_ReadFile(h, buf, len, read, ov);
}

/* ---- Gear-button code patch ------------------------------------------------
 * Root cause of "gears don't work" (found 2026-08-14 via live cdb + static
 * trace of the HAPP drain 0x4a1af0): the drain extracts report buf[6] bits 0..3
 * into the game's internal 128-entry button array at FIXED indices 5, 6, 2, 4:
 *     0x4a1b63:  mov %cl,0x4d(%esp)   ; bit0 -> button index 5
 *     0x4a1b73:  mov %dl,0x4e(%esp)   ; bit1 -> button index 6
 *     0x4a1b77:  mov %cl,0x4a(%esp)   ; bit2 -> button index 2
 *     0x4a1b7b:  mov %al,0x4c(%esp)   ; bit3 -> button index 4
 * Cross-referencing CTRLLERS.CDB's "Logitech Formula GP Racing Wheel" action map
 * -- which this game applies to the HAPP device (CONFIRMED live: bit3 -> index 4
 * == CHANGE_VIEW) -- the action set is: btn0=GEAR_DOWN, btn1=GEAR_UP,
 * btn2=LOOK_BEHIND, btn3=HANDBRAKE, btn4=CHANGE_VIEW, btn5=BOOST. So the four
 * HAPP bits land on {BOOST, unused-btn6, LOOK_BEHIND, VIEW} and NEVER on the two
 * gear buttons (0/1) -- the shipped HAPP path literally cannot send a shift.
 * Two one-byte displacement edits reroute bit0 -> index 0 (GEAR_DOWN) and
 * bit1 -> index 1 (GEAR_UP), trading BOOST + the unused button for working
 * manual gears. Applied at runtime; the user's fordracing.exe on disk is never
 * touched, and the pre-patch bytes (4D/4E) are verified first so we never
 * scribble on an unexpected build. Opt out: ULTRA_EUROPAR_GEAR_PATCH=0. */
static void europar_patch_gear_buttons(void) {
    const char *e = getenv("ULTRA_EUROPAR_GEAR_PATCH");
    if (e && e[0] == '0') { LOG("gear-button patch disabled (ULTRA_EUROPAR_GEAR_PATCH=0)\n"); return; }

    BYTE *base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) { LOG("gear patch: GetModuleHandleA(NULL) failed\n"); return; }
    BYTE *p1 = base + 0xa1b66;   /* disp byte of  mov %cl,0x4d(%esp)  (bit0 dest idx) */
    BYTE *p2 = base + 0xa1b76;   /* disp byte of  mov %dl,0x4e(%esp)  (bit1 dest idx) */

    if (*p1 != 0x4d || *p2 != 0x4e) {
        LOG("gear patch SKIPPED: unexpected bytes p1=0x%02X p2=0x%02X (want 4D/4E) -- wrong build?\n",
            *p1, *p2);
        return;
    }
    SIZE_T span = (SIZE_T)(p2 - p1) + 1;
    DWORD oldp = 0, tmp = 0;
    if (!VirtualProtect(p1, span, PAGE_EXECUTE_READWRITE, &oldp)) {
        LOG("gear patch: VirtualProtect failed (err=%lu)\n", GetLastError());
        return;
    }
    *p1 = 0x48;   /* bit0 -> game button 0 (GEAR_DOWN) */
    *p2 = 0x49;   /* bit1 -> game button 1 (GEAR_UP)   */
    VirtualProtect(p1, span, oldp, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p1, span);
    LOG("gear-button patch applied: HAPP bit0->GEAR_DOWN(btn0), bit1->GEAR_UP(btn1)\n");
}

/* ---- Install ---------------------------------------------------------------
 * SETUPAPI.dll and HID.DLL are both in fordracing.exe's own static import
 * table (confirmed), so they're already loaded by the time this runs --
 * GetModuleHandleA is enough, no LoadLibraryA needed. */
void happio_europar_install(void) {
    /* PER-GAME OFF SWITCH. This whole emulation exists for Ford Racing's HAPP
     * UGCI wheel. Sega Rally 3 on this same board uses a DIFFERENT cabinet I/O
     * board and never wants a HAPP device -- and worse, our synthetic SetupAPI
     * entry was being handed to Rally's OWN xinput1_3.dll enumeration, which
     * dereferenced the magic cookie and crashed in DEVOBJ.dll. A container can
     * switch the whole thing off:  ULTRA_EUROPAR_HAPPIO = 0 */
    { const char *e = getenv("ULTRA_EUROPAR_HAPPIO");
      if (e && *e && atoi(e) == 0) { LOG("happio: DISABLED by ULTRA_EUROPAR_HAPPIO=0\n"); return; } }
    HMODULE k32      = GetModuleHandleA("kernel32.dll");
    HMODULE setupapi = GetModuleHandleA("setupapi.dll");
    HMODULE hid      = GetModuleHandleA("hid.dll");
    if (!k32 || !setupapi || !hid) {
        LOG("ERROR: kernel32/setupapi.dll/hid.dll not all loaded, skipping install (k32=%p setupapi=%p hid=%p)\n",
            (void*)k32, (void*)setupapi, (void*)hid);
        return;
    }

    Real_CreateFileA                      = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
    Real_ReadFile                         = (pReadFile)(void*)GetProcAddress(k32, "ReadFile");
    Real_SetupDiEnumDeviceInterfaces      = (pSetupDiEnumDeviceInterfaces)(void*)GetProcAddress(setupapi, "SetupDiEnumDeviceInterfaces");
    Real_SetupDiGetDeviceInterfaceDetailA = (pSetupDiGetDeviceInterfaceDetailA)(void*)GetProcAddress(setupapi, "SetupDiGetDeviceInterfaceDetailA");
    Real_HidD_GetAttributes               = (pHidD_GetAttributes)(void*)GetProcAddress(hid, "HidD_GetAttributes");
    Real_HidD_GetPreparsedData            = (pHidD_GetPreparsedData)(void*)GetProcAddress(hid, "HidD_GetPreparsedData");
    Real_HidP_GetCaps                     = (pHidP_GetCaps)(void*)GetProcAddress(hid, "HidP_GetCaps");

    if (!Real_CreateFileA || !Real_ReadFile || !Real_SetupDiEnumDeviceInterfaces ||
        !Real_SetupDiGetDeviceInterfaceDetailA || !Real_HidD_GetAttributes ||
        !Real_HidD_GetPreparsedData || !Real_HidP_GetCaps) {
        LOG("ERROR: one or more real function pointers unresolved, aborting install\n");
        return;
    }

    ultra_iat_patch_all_europar("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_iat_patch_all_europar("kernel32.dll", "ReadFile", (void*)Hook_ReadFile);
    ultra_iat_patch_all_europar("setupapi.dll", "SetupDiEnumDeviceInterfaces", (void*)Hook_SetupDiEnumDeviceInterfaces);
    ultra_iat_patch_all_europar("setupapi.dll", "SetupDiGetDeviceInterfaceDetailA", (void*)Hook_SetupDiGetDeviceInterfaceDetailA);
    ultra_iat_patch_all_europar("hid.dll", "HidD_GetAttributes", (void*)Hook_HidD_GetAttributes);
    ultra_iat_patch_all_europar("hid.dll", "HidD_GetPreparsedData", (void*)Hook_HidD_GetPreparsedData);
    ultra_iat_patch_all_europar("hid.dll", "HidP_GetCaps", (void*)Hook_HidP_GetCaps);

    europar_patch_gear_buttons();

    LOG("installed: emulating HAPP UGCI VID=0x%04X PID=0x%04X, %d-byte reports\n",
        HAPP_VID, HAPP_PID, HAPP_REPORT_LEN);
}

#endif /* _WIN32 */
