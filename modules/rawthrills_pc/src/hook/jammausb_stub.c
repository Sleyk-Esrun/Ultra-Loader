/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jammausb_stub.c -- Raw Thrills JAMMAUSB (USB-JAMMA I/O board) stub.
 * =============================================================================
 *
 *  ENGINE FACTS (from static analysis of sdaemon.exe -- TFAFSB dump):
 *    The JAMMAUSB board is NOT reached via a "JammaUSB.dll". It is a HID device
 *    -- sdaemon imports setupapi + hid.dll and enumerates the HID class GUID:
 *        HidD_GetHidGuid        -> GUID for HID class
 *        SetupDiGetClassDevsExW -> enumerate devices of that GUID
 *        SetupDiEnumDeviceInterfaces / SetupDiGetDeviceInterfaceDetailW
 *        CreateFileW(device-interface-path, ...)   -> open the HID handle
 *        HidD_GetAttributes     -> match VID/PID
 *        HidD_GetPreparsedData + HidP_GetCaps -> report sizes
 *        ReadFile / WriteFile   -> HID input/output/feature reports
 *
 *  Distinctive strings that name this device inside sdaemon:
 *        "JAMMAUSB", "JUSB_WHEEL", "JUSB_GetReport(): ERROR!",
 *        "JUSB Version: %s %x", "JAMMAUSB DISCONNECTED!!!"
 *        "Unable to open RT Jamma USB device"
 *
 *  WHAT THIS STUB DOES:
 *    - Hooks HidD_GetAttributes to advertise our synthetic device (VID/PID that
 *      matches the well-known Raw Thrills JAMMAUSB signature) so the game's
 *      enum accepts our sentinel HID handle as its I/O board.
 *    - Hooks CreateFileW on HID device-interface paths and returns a sentinel
 *      handle we own (JAMMA_SENTINEL). The game's later ReadFile / WriteFile /
 *      HidD_GetPreparsedData calls on that handle route to us.
 *    - Hooks HidD_GetPreparsedData / HidP_GetCaps to return canonical report
 *      sizes (input 64B, output 64B, feature 64B -- the RawApp JammaUSB report
 *      convention).
 *    - Hooks ReadFile / WriteFile on the sentinel: WriteFile is swallowed
 *      (lamps/solenoids -- we log if requested); ReadFile returns a neutral
 *      controller snapshot (all buttons released, analogs centered, coin
 *      counters latched).
 *    - Hooks HidD_SetFeature / HidD_GetFeature (LAMP + SOLENOID + WATCHDOG_PAT
 *      classic JAMMAUSB feature reports) with success/neutral defaults.
 *
 *  Fallback semantics chosen so the game boots cleanly:
 *    - "board present, all inputs neutral, all outputs accepted".
 *    - Real input arrives via the Ultra Loader universal-input service once
 *      integration is wired (env ULTRA_JAMMAUSB=1 + ULTRA_CONTROLS=1); until
 *      then the stub is transparent-neutral so the game gets to attract mode.
 *
 *  Env gate: ULTRA_JAMMAUSB=1 (set by rawthrills_pc_main.c). Default OFF (do
 *  nothing) so an accidental load into a non-Raw-Thrills process is byte-safe.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "rawthrills_hook.h"

#define JAMMA_SENTINEL ((HANDLE)(ULONG_PTR)0x4A55B001)   /* "JUSB\1" */

/* HID API function pointer types. We keep our own typedefs so the stub does
 * not depend on the Windows DDK's <hidsdi.h> being present in the mingw sysroot
 * (mingw32 ships hidusage.h but not hidsdi.h consistently). */
typedef USHORT UL_USAGE;
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; }
    UL_HIDD_ATTRIBUTES;
typedef struct { UL_USAGE Usage, UsagePage;
                 USHORT InputReportByteLength, OutputReportByteLength,
                        FeatureReportByteLength;
                 USHORT Reserved[17];
                 USHORT NumberLinkCollectionNodes;
                 USHORT NumberInputButtonCaps, NumberInputValueCaps,
                        NumberInputDataIndices;
                 USHORT NumberOutputButtonCaps, NumberOutputValueCaps,
                        NumberOutputDataIndices;
                 USHORT NumberFeatureButtonCaps, NumberFeatureValueCaps,
                        NumberFeatureDataIndices; } UL_HIDP_CAPS;

typedef BOOLEAN (WINAPI *pHidD_GetAttributes)(HANDLE, UL_HIDD_ATTRIBUTES*);
typedef BOOLEAN (WINAPI *pHidD_GetPreparsedData)(HANDLE, PVOID*);
typedef BOOLEAN (WINAPI *pHidD_FreePreparsedData)(PVOID);
typedef LONG    (WINAPI *pHidP_GetCaps)(PVOID, UL_HIDP_CAPS*);
typedef BOOLEAN (WINAPI *pHidD_SetFeature)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *pHidD_GetFeature)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *pHidD_GetInputReport)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *pHidD_SetOutputReport)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *pHidD_GetProductString)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *pHidD_GetManufacturerString)(HANDLE, PVOID, ULONG);

/* ---- HidP_* PARSE surface (the crash path) ---------------------------------
 * After opening our sentinel the game may parse the report via the HidP_*
 * family, passing our fake preparsed-data pointer (UL_JAMMA_PPD). Those calls
 * would otherwise reach the real hid.dll parser and dereference 0xC0DEC0DE ->
 * access violation. We hook the whole family and, for UL_JAMMA_PPD, return
 * benign HIDP_STATUS_SUCCESS with empty lists -- consistent with the caps we
 * advertise (0 button/value caps), which tells the game to read the RAW report
 * itself (the JammaUSB board is a custom report, not standard HID usages). For
 * any other (real) preparsed data we pass straight through. HIDP_REPORT_TYPE is
 * an int enum; opaque pointers are PVOID. NTSTATUS-style LONG returns. */
#define UL_HIDP_STATUS_SUCCESS       ((LONG)0x00110000)
#define UL_HIDP_STATUS_INVALID_PPD   ((LONG)0xC0110001)
typedef LONG (WINAPI *pHidP_GetButtonCaps)(int, PVOID, PUSHORT, PVOID);
typedef LONG (WINAPI *pHidP_GetSpecificButtonCaps)(int, USHORT, USHORT, USHORT, PVOID, PUSHORT, PVOID);
typedef LONG (WINAPI *pHidP_GetValueCaps)(int, PVOID, PUSHORT, PVOID);
typedef LONG (WINAPI *pHidP_GetSpecificValueCaps)(int, USHORT, USHORT, USHORT, PVOID, PUSHORT, PVOID);
typedef LONG (WINAPI *pHidP_GetUsages)(int, USHORT, USHORT, PUSHORT, PULONG, PVOID, PCHAR, ULONG);
typedef LONG (WINAPI *pHidP_GetUsageValue)(int, USHORT, USHORT, USHORT, PULONG, PVOID, PCHAR, ULONG);
typedef LONG (WINAPI *pHidP_GetData)(int, PVOID, PULONG, PVOID, PCHAR, ULONG);
typedef ULONG (WINAPI *pHidP_MaxUsageListLength)(int, USHORT, PVOID);
typedef ULONG (WINAPI *pHidP_MaxDataListLength)(int, PVOID);

typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,
                                      DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);
typedef void   (WINAPI *pHidD_GetHidGuid)(LPGUID);

/* ---- SetupAPI device-interface enumeration (the DISCOVERY path) -------------
 * sdaemon finds its JAMMAUSB board by enumerating HID device interfaces via
 * SetupAPI BEFORE it ever calls CreateFileW: HidD_GetHidGuid -> the HID class
 * GUID, SetupDiGetClassDevs(W) -> a device-info set, then
 * SetupDiEnumDeviceInterfaces(index 0,1,2,...) until it runs out, and
 * SetupDiGetDeviceInterfaceDetailW to turn each into a \\?\hid#... path it can
 * open. On a cabinet the JAMMA board is one of those interfaces. On a bare PC
 * with no real board, our HidD_* / CreateFileW / ReadFile hooks are correct but
 * UNREACHABLE: the game never discovers a path to open, so it never calls them
 * and prints "Unable to open RT Jamma USB device" / "JAMMAUSB DISCONNECTED!!!".
 *
 * We keep our own minimal SetupAPI struct typedefs (same reason as the HID ones
 * above -- do not depend on the DDK's <setupapi.h> being in the mingw sysroot).
 * ABI matches the SDK on 32-bit: SP_DEVICE_INTERFACE_DATA = 28 bytes. */
typedef struct { DWORD cbSize; GUID InterfaceClassGuid; DWORD Flags;
                 ULONG_PTR Reserved; } UL_SP_DEVICE_INTERFACE_DATA;
typedef struct { DWORD cbSize; WCHAR DevicePath[1]; }
    UL_SP_DEVICE_INTERFACE_DETAIL_W;
#ifndef SPINT_ACTIVE
#define SPINT_ACTIVE 0x00000001
#endif

typedef HANDLE (WINAPI *pSetupDiGetClassDevsW)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL   (WINAPI *pSetupDiEnumDeviceInterfaces)(HANDLE, PVOID, const GUID*,
                    DWORD, UL_SP_DEVICE_INTERFACE_DATA*);
typedef BOOL   (WINAPI *pSetupDiGetDeviceInterfaceDetailW)(HANDLE,
                    UL_SP_DEVICE_INTERFACE_DATA*, UL_SP_DEVICE_INTERFACE_DETAIL_W*,
                    DWORD, PDWORD, PVOID);

/* ---- ANSI SetupAPI + CreateFileA (THE PATH BOTH TARGET TITLES ACTUALLY USE) -
 * Static RE of both game binaries (2026-09-30) proved they import and call the
 * ANSI variants, never the wide ones:
 *   H2Overdrive (cbw32.dll): SetupDiGetClassDevsA, SetupDiEnumDeviceInterfaces,
 *     SetupDiGetDeviceInterfaceDetailA, CreateFileA. It matches the board by the
 *     device-interface PATH substring "VID_09DB&PID_00xx&MI_0" (class cbUsbSsrFS,
 *     whitelist of PIDs 0x0075..0x00A4), then CreateFileA(path, OVERLAPPED).
 *   Super Bikes (sdaemon.exe): SetupDiGetClassDevsA, SetupDiEnumDeviceInterfaces,
 *     SetupDiGetDeviceInterfaceDetailA, CreateFileA, then HidD_GetAttributes and
 *     matches VendorID==0x0483 ProductID==0x0003 (at 0x4CA09F/0x4CA0A8).
 * Hooking only the W variants (the original stub) meant the synthetic board was
 * enumerated but the game's ANSI GetDeviceInterfaceDetailA + CreateFileA never
 * reached it -- the exact "no difference" the owner saw. SP_DEVICE_INTERFACE_
 * DETAIL_DATA_A on 32-bit has cbSize==5 (DWORD + first CHAR); DevicePath is at
 * offset 4. */
typedef struct { DWORD cbSize; CHAR DevicePath[1]; }
    UL_SP_DEVICE_INTERFACE_DETAIL_A;
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,
                                      DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pSetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL   (WINAPI *pSetupDiGetDeviceInterfaceDetailA)(HANDLE,
                    UL_SP_DEVICE_INTERFACE_DATA*, UL_SP_DEVICE_INTERFACE_DETAIL_A*,
                    DWORD, PDWORD, PVOID);

/* Synthetic preparsed-data sentinel we hand back to the game so HidP_GetCaps
 * has something distinctive to key on. It is opaque to the game. */
#define UL_JAMMA_PPD ((PVOID)(ULONG_PTR)0xC0DEC0DE)

/* Well-known Raw Thrills JAMMAUSB HID VID/PID (Cypress-derived, the "RT Jamma
 * USB" identity strings match this pair). Overridable via env. */
static USHORT g_jamma_vid = 0x0B9A;   /* Raw Thrills-registered VID */
static USHORT g_jamma_pid = 0x0006;   /* JAMMAUSB PID */

static int g_enabled = 0;
static pHidD_GetAttributes         Real_HidD_GetAttributes         = NULL;
static pHidD_GetPreparsedData      Real_HidD_GetPreparsedData      = NULL;
static pHidD_FreePreparsedData     Real_HidD_FreePreparsedData     = NULL;
static pHidP_GetCaps               Real_HidP_GetCaps               = NULL;
static pHidD_SetFeature            Real_HidD_SetFeature            = NULL;
static pHidD_GetFeature            Real_HidD_GetFeature            = NULL;
static pHidD_GetInputReport        Real_HidD_GetInputReport        = NULL;
static pHidD_SetOutputReport       Real_HidD_SetOutputReport       = NULL;
static pHidD_GetProductString      Real_HidD_GetProductString      = NULL;
static pHidD_GetManufacturerString Real_HidD_GetManufacturerString = NULL;

static pCreateFileW Real_CreateFileW = NULL;
static pReadFile    Real_ReadFile    = NULL;
static pWriteFile   Real_WriteFile   = NULL;
static pCloseHandle Real_CloseHandle = NULL;
static pHidD_GetHidGuid Real_HidD_GetHidGuid = NULL;

static pSetupDiGetClassDevsW             Real_SetupDiGetClassDevsW             = NULL;
static pSetupDiEnumDeviceInterfaces      Real_SetupDiEnumDeviceInterfaces      = NULL;
static pSetupDiGetDeviceInterfaceDetailW Real_SetupDiGetDeviceInterfaceDetailW = NULL;

static pCreateFileA                      Real_CreateFileA                      = NULL;
static pSetupDiGetClassDevsA             Real_SetupDiGetClassDevsA             = NULL;
static pSetupDiGetDeviceInterfaceDetailA Real_SetupDiGetDeviceInterfaceDetailA = NULL;

static pHidP_GetButtonCaps         Real_HidP_GetButtonCaps         = NULL;
static pHidP_GetSpecificButtonCaps Real_HidP_GetSpecificButtonCaps = NULL;
static pHidP_GetValueCaps          Real_HidP_GetValueCaps          = NULL;
static pHidP_GetSpecificValueCaps  Real_HidP_GetSpecificValueCaps  = NULL;
static pHidP_GetUsages             Real_HidP_GetUsages             = NULL;
static pHidP_GetUsageValue         Real_HidP_GetUsageValue         = NULL;
static pHidP_GetData               Real_HidP_GetData               = NULL;
static pHidP_MaxUsageListLength    Real_HidP_MaxUsageListLength    = NULL;
static pHidP_MaxDataListLength     Real_HidP_MaxDataListLength     = NULL;

/* Live input: fill the JammaUSB input report from a real controller so the game
 * reads coins/buttons/wheel/throttle from the emulated board (the board is the
 * game's ONLY input path; it does not read the keyboard). Default ON for
 * driving titles; opt out with ULTRA_JAMMAUSB_LIVE=0. g_driving selects the
 * driving idle profile (throttle/brake released = 0x00, wheel centred = 0x80)
 * vs the plain neutral profile for non-driving titles (analogs 0x80). */
static int g_live    = 1;
static int g_driving = 0;

/* Local XInput mirror so we need no -lxinput import lib (same trick as
 * omni_bridge.c). Loaded via LoadLibrary in install. */
#pragma pack(push, 1)
typedef struct { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } ul_xi_pad;
typedef struct { DWORD dwPacketNumber; ul_xi_pad Gamepad; } ul_xi_state;
#pragma pack(pop)
typedef DWORD (WINAPI *pfnXInputGetState)(DWORD, ul_xi_state*);
static pfnXInputGetState s_xi = NULL;
#define UXI_DPAD_UP    0x0001
#define UXI_DPAD_DOWN  0x0002
#define UXI_DPAD_LEFT  0x0004
#define UXI_DPAD_RIGHT 0x0008
#define UXI_START      0x0010
#define UXI_BACK       0x0020
#define UXI_A          0x1000
#define UXI_B          0x2000
#define UXI_X          0x4000
#define UXI_Y          0x8000
#define UXI_LB         0x0100
#define UXI_RB         0x0200

/* Synthetic-device injection into the SetupAPI enumeration. DEFAULT ON: the
 * 2026-09-30 trace proved both H2Overdrive and Super Bikes enumerate ~24 real
 * HID interfaces, never see the JAMMA VID/PID (0x0B9A/0x0006), hit end-of-list
 * and then access-violate on the NULL board pointer. No emulated Raw Thrills
 * cabinet has a real 0B9A:0006 board attached, so appending one synthetic JAMMA
 * interface at the end of the enumeration is what lets the game find + open its
 * board. Opt OUT with ULTRA_JAMMAUSB_SYNTH=0 (e.g. if a real board is present).
 * The injection only APPENDS after the real list, so it never disturbs the real
 * devices the game already enumerated. */
static int g_synth = 1;

/* Marker stamped into a synthetic SP_DEVICE_INTERFACE_DATA.Reserved so the
 * matching GetDeviceInterfaceDetailW call recognizes it as ours. */
#define UL_JAMMA_SYNTH_MAGIC ((ULONG_PTR)0x4A534E54u)   /* "JSNT" */
/* The synthetic interface path, built at install() from the active VID/PID so
 * it is a FAITHFUL imitation of the real board's HID interface path. This is
 * what makes H2Overdrive's path-substring match ("vid_09db&pid_00xx&mi_0")
 * accept our device, and it still contains "hid#" so path_is_hid()/path_is_hid_a
 * match it in the CreateFile hooks; the real open then fails (no such device)
 * and we hand back JAMMA_SENTINEL. Lowercase, exactly as the OS emits interface
 * paths (a real board's path is lowercase and cabinets accept it, so the game's
 * compare is case-insensitive / case-folding -- lowercase is the correct form).
 * Both an ANSI and a wide copy are kept because the two titles call the ANSI
 * detail path and any future title might call the wide one. */
static char    g_synth_path_a[160];
static wchar_t g_synth_path_w[160];

/* Per-HDEVINFO record of the index at which we injected our synthetic entry, so
 * repeated Enum calls terminate and Detail calls can be matched even if the
 * caller does not carry Reserved forward. Tiny fixed table; on overflow we
 * simply skip synthesis for the extra set (no crash, no leak). */
#define UL_SYNTH_NONE 0xFFFFFFFFu   /* synth_index sentinel = not yet injected */
typedef struct { HANDLE hdi; DWORD synth_index; int used; } ul_synth_slot;
static ul_synth_slot g_synth_tab[8];

/* Rate-limited logging for the per-frame I/O paths (ReadFile/WriteFile run
 * ~125x/s). Log the first few, then every 512th, so the file shows the traffic
 * shape without flooding. */
static int ul_rl(volatile LONG* counter) {
    LONG c = InterlockedIncrement(counter);
    return (c <= 8) || (c % 512 == 0);
}

/* Match any HID device-interface path. The GUID that identifies HID devices
 * appears in the path as "{4d1e55b2-f16f-11cf-88cb-001111000030}". Matching on
 * that GUID substring keeps us from intercepting non-HID CreateFileW. */
static const wchar_t k_hid_iface_guid[] =
    L"{4d1e55b2-f16f-11cf-88cb-001111000030}";

static int path_is_hid(LPCWSTR path) {
    if (!path) return 0;
    /* Cheap substring match. Case-insensitive would need a fold; the OS emits
     * this GUID as lowercase, so lowercase-literal match is enough in practice.
     * We check both the lowercase and uppercase-B0 form (the leading "\\?\hid#"
     * plus the guid tail). */
    for (const wchar_t* p = path; *p; ++p) {
        if ((p[0] == L'h' || p[0] == L'H')
            && (p[1] == L'i' || p[1] == L'I')
            && (p[2] == L'd' || p[2] == L'D')
            && p[3] == L'#') return 1;
    }
    /* Fallback: HID interface guid in the path. */
    (void)k_hid_iface_guid;
    return 0;
}

/* Neutral 64-byte HID input report -- centered analogs (0x80), no buttons.
 * Byte layout used by the RawApp JAMMAUSB firmware (public reverse-engineering
 * of the sdaemon report parser): [0]=report id, [1..4]=button bitfield,
 * [5]=coin1, [6]=coin2, [7]=service, [8..15]=analog bytes (wheel/pedal/etc),
 * remaining = reserved zero. */
#define UL_BTN_START1  0x00000001u
#define UL_BTN_A       0x00000004u
#define UL_BTN_B       0x00000008u
#define UL_BTN_X       0x00000010u
#define UL_BTN_Y       0x00000020u

static void ul_fill_report(uint8_t* buf, DWORD n) {
    if (!buf || n == 0) return;
    memset(buf, 0, n);
    if (n >= 1) buf[0] = 0x01;   /* report id */

    /* idle analog defaults */
    if (g_driving) {
        if (n > 8)  buf[8]  = 0x80;              /* wheel centre            */
        /* buf[9]=throttle, buf[10]=brake stay 0x00 (pedals released)      */
        for (DWORD i = 11; i < 16 && i < n; ++i) buf[i] = 0x80;
    } else {
        for (DWORD i = 8; i < 16 && i < n; ++i)  buf[i] = 0x80;
    }

    if (!g_live) return;

    uint32_t btn = 0;
    int coin = 0, service = 0;

    if (s_xi) {
        ul_xi_state st; memset(&st, 0, sizeof st);
        if (s_xi(0, &st) == 0) {
            WORD b = st.Gamepad.wButtons;
            if (b & UXI_START) btn |= UL_BTN_START1;
            if (b & UXI_A)     btn |= UL_BTN_A;
            if (b & UXI_B)     btn |= UL_BTN_B;
            if (b & UXI_X)     btn |= UL_BTN_X;
            if (b & UXI_Y)     btn |= UL_BTN_Y;
            if (b & UXI_BACK)  coin = 1;
            if (b & UXI_LB)    service |= 0x01;   /* service */
            if (b & UXI_RB)    service |= 0x02;   /* test    */
            if (g_driving) {
                int wheel = 0x80 + (st.Gamepad.sThumbLX / 256);
                if (wheel < 0)   wheel = 0;
                if (wheel > 255) wheel = 255;
                if (n > 8)  buf[8]  = (uint8_t)wheel;
                if (n > 9)  buf[9]  = st.Gamepad.bRightTrigger;   /* throttle */
                if (n > 10) buf[10] = st.Gamepad.bLeftTrigger;    /* brake    */
            }
        }
    }

    /* Keyboard fallback for coin/start/service/test (conventional arcade keys)
     * so a user without a pad can still feed the board. */
    if (GetAsyncKeyState('5') & 0x8000) coin = 1;             /* coin    */
    if (GetAsyncKeyState('1') & 0x8000) btn |= UL_BTN_START1; /* start   */
    if (GetAsyncKeyState('9') & 0x8000) service |= 0x01;      /* service */
    if (GetAsyncKeyState('0') & 0x8000) service |= 0x02;      /* test    */

    if (n > 4) { buf[1] = (uint8_t)(btn);       buf[2] = (uint8_t)(btn >> 8);
                 buf[3] = (uint8_t)(btn >> 16); buf[4] = (uint8_t)(btn >> 24); }
    if (n > 5) buf[5] = coin ? 1 : 0;
    if (n > 7) buf[7] = (uint8_t)service;
}

static BOOLEAN WINAPI Hook_HidD_GetAttributes(HANDLE h, UL_HIDD_ATTRIBUTES* a) {
    if (h == JAMMA_SENTINEL && a) {
        a->Size = sizeof(*a);
        a->VendorID       = g_jamma_vid;
        a->ProductID      = g_jamma_pid;
        a->VersionNumber  = 0x0100;
        ul_rt_log("jammausb: GetAttributes(sentinel) -> VID=0x%04X PID=0x%04X\n",
                  g_jamma_vid, g_jamma_pid);
        return TRUE;
    }
    /* Real handle: log what the game actually sees so we learn the VID/PID it
     * is hunting for (the whole point of the first diagnostic run). */
    {
        BOOLEAN ok = Real_HidD_GetAttributes ? Real_HidD_GetAttributes(h, a) : FALSE;
        if (ok && a)
            ul_rt_log("jammausb: GetAttributes(real h=%p) -> VID=0x%04X PID=0x%04X ver=0x%04X\n",
                      (void*)h, a->VendorID, a->ProductID, a->VersionNumber);
        return ok;
    }
}

static BOOLEAN WINAPI Hook_HidD_GetPreparsedData(HANDLE h, PVOID* ppd) {
    if (h == JAMMA_SENTINEL && ppd) { *ppd = UL_JAMMA_PPD; return TRUE; }
    return Real_HidD_GetPreparsedData ? Real_HidD_GetPreparsedData(h, ppd) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_FreePreparsedData(PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) return TRUE;
    return Real_HidD_FreePreparsedData ? Real_HidD_FreePreparsedData(ppd) : TRUE;
}
static LONG WINAPI Hook_HidP_GetCaps(PVOID ppd, UL_HIDP_CAPS* caps) {
    if (ppd == UL_JAMMA_PPD && caps) {
        memset(caps, 0, sizeof(*caps));
        caps->Usage     = 0x04;       /* HID Joystick */
        caps->UsagePage = 0x01;       /* Generic Desktop */
        caps->InputReportByteLength   = 64;
        caps->OutputReportByteLength  = 64;
        caps->FeatureReportByteLength = 64;
        return 0x00110000;            /* HIDP_STATUS_SUCCESS */
    }
    return Real_HidP_GetCaps ? Real_HidP_GetCaps(ppd, caps) : (LONG)0xC0000001;
}

/* ---- HidP_* parse family: intercept our fake PPD so the real hid.dll parser
 *      never dereferences 0xC0DEC0DE (the AV path). We advertise 0 button/value
 *      caps (GetCaps above), so a correct caller reads the RAW report instead;
 *      these return benign empty-success. Any real PPD passes straight through.
 *      Each is logged (rate-limited) so the trace shows whether the game uses
 *      the HidP path at all -- if it does, we implement real parsing next. */
static volatile LONG g_hidp_ctr = 0;
static LONG WINAPI Hook_HidP_GetButtonCaps(int rt, PVOID caps, PUSHORT len, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) {
        (void)rt; (void)caps; if (len) *len = 0;
        if (ul_rl(&g_hidp_ctr)) ul_rt_log("jammausb: HidP_GetButtonCaps(synthetic) -> 0 caps\n");
        return UL_HIDP_STATUS_SUCCESS;
    }
    return Real_HidP_GetButtonCaps ? Real_HidP_GetButtonCaps(rt, caps, len, ppd) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetSpecificButtonCaps(int rt, USHORT up, USHORT lc, USHORT us, PVOID caps, PUSHORT len, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) { (void)rt;(void)up;(void)lc;(void)us;(void)caps; if (len) *len = 0; return UL_HIDP_STATUS_SUCCESS; }
    return Real_HidP_GetSpecificButtonCaps ? Real_HidP_GetSpecificButtonCaps(rt, up, lc, us, caps, len, ppd) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetValueCaps(int rt, PVOID caps, PUSHORT len, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) {
        (void)rt; (void)caps; if (len) *len = 0;
        if (ul_rl(&g_hidp_ctr)) ul_rt_log("jammausb: HidP_GetValueCaps(synthetic) -> 0 caps\n");
        return UL_HIDP_STATUS_SUCCESS;
    }
    return Real_HidP_GetValueCaps ? Real_HidP_GetValueCaps(rt, caps, len, ppd) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetSpecificValueCaps(int rt, USHORT up, USHORT lc, USHORT us, PVOID caps, PUSHORT len, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) { (void)rt;(void)up;(void)lc;(void)us;(void)caps; if (len) *len = 0; return UL_HIDP_STATUS_SUCCESS; }
    return Real_HidP_GetSpecificValueCaps ? Real_HidP_GetSpecificValueCaps(rt, up, lc, us, caps, len, ppd) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetUsages(int rt, USHORT up, USHORT lc, PUSHORT list, PULONG len, PVOID ppd, PCHAR rep, ULONG replen) {
    if (ppd == UL_JAMMA_PPD) {
        (void)rt;(void)up;(void)lc;(void)list;(void)rep;(void)replen; if (len) *len = 0;
        if (ul_rl(&g_hidp_ctr)) ul_rt_log("jammausb: HidP_GetUsages(synthetic) -> 0 usages (game uses HidP parse path!)\n");
        return UL_HIDP_STATUS_SUCCESS;
    }
    return Real_HidP_GetUsages ? Real_HidP_GetUsages(rt, up, lc, list, len, ppd, rep, replen) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetUsageValue(int rt, USHORT up, USHORT lc, USHORT us, PULONG val, PVOID ppd, PCHAR rep, ULONG replen) {
    if (ppd == UL_JAMMA_PPD) {
        (void)rt;(void)up;(void)lc;(void)us;(void)rep;(void)replen; if (val) *val = 0;
        if (ul_rl(&g_hidp_ctr)) ul_rt_log("jammausb: HidP_GetUsageValue(synthetic) -> 0 (game uses HidP parse path!)\n");
        return UL_HIDP_STATUS_SUCCESS;
    }
    return Real_HidP_GetUsageValue ? Real_HidP_GetUsageValue(rt, up, lc, us, val, ppd, rep, replen) : UL_HIDP_STATUS_INVALID_PPD;
}
static LONG WINAPI Hook_HidP_GetData(int rt, PVOID data, PULONG len, PVOID ppd, PCHAR rep, ULONG replen) {
    if (ppd == UL_JAMMA_PPD) { (void)rt;(void)data;(void)rep;(void)replen; if (len) *len = 0; return UL_HIDP_STATUS_SUCCESS; }
    return Real_HidP_GetData ? Real_HidP_GetData(rt, data, len, ppd, rep, replen) : UL_HIDP_STATUS_INVALID_PPD;
}
static ULONG WINAPI Hook_HidP_MaxUsageListLength(int rt, USHORT up, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) { (void)rt;(void)up; return 0; }
    return Real_HidP_MaxUsageListLength ? Real_HidP_MaxUsageListLength(rt, up, ppd) : 0;
}
static ULONG WINAPI Hook_HidP_MaxDataListLength(int rt, PVOID ppd) {
    if (ppd == UL_JAMMA_PPD) { (void)rt; return 0; }
    return Real_HidP_MaxDataListLength ? Real_HidP_MaxDataListLength(rt, ppd) : 0;
}

static BOOLEAN WINAPI Hook_HidD_SetFeature(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) {
        (void)buf; (void)n;
        /* Accept lamp/solenoid/watchdog-pat feature reports silently. */
        return TRUE;
    }
    return Real_HidD_SetFeature ? Real_HidD_SetFeature(h, buf, n) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_GetFeature(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) {
        if (buf && n) memset(buf, 0, n);
        return TRUE;
    }
    return Real_HidD_GetFeature ? Real_HidD_GetFeature(h, buf, n) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_GetInputReport(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) {
        ul_fill_report((uint8_t*)buf, n);
        return TRUE;
    }
    return Real_HidD_GetInputReport ? Real_HidD_GetInputReport(h, buf, n) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_SetOutputReport(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) { (void)buf; (void)n; return TRUE; }
    return Real_HidD_SetOutputReport ? Real_HidD_SetOutputReport(h, buf, n) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_GetProductString(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) {
        static const wchar_t kProd[] = L"JAMMAUSB";
        DWORD sz = (DWORD)sizeof(kProd);
        if (buf && n >= sz) memcpy(buf, kProd, sz);
        return TRUE;
    }
    return Real_HidD_GetProductString ? Real_HidD_GetProductString(h, buf, n) : FALSE;
}
static BOOLEAN WINAPI Hook_HidD_GetManufacturerString(HANDLE h, PVOID buf, ULONG n) {
    if (h == JAMMA_SENTINEL) {
        static const wchar_t kMfg[] = L"Raw Thrills";
        DWORD sz = (DWORD)sizeof(kMfg);
        if (buf && n >= sz) memcpy(buf, kMfg, sz);
        return TRUE;
    }
    return Real_HidD_GetManufacturerString
           ? Real_HidD_GetManufacturerString(h, buf, n) : FALSE;
}

/* HidD_GetHidGuid: log-only passthrough. Seeing this in the trace confirms the
 * game has begun HID class enumeration (the discovery path) at all. */
static void WINAPI Hook_HidD_GetHidGuid(LPGUID g) {
    if (Real_HidD_GetHidGuid) Real_HidD_GetHidGuid(g);
    ul_rt_log("jammausb: HidD_GetHidGuid() -- game begins HID enumeration\n");
}

/* ---- SetupAPI discovery hooks -------------------------------------------- */
static ul_synth_slot* synth_slot_for(HANDLE hdi, int create) {
    for (int i = 0; i < 8; ++i)
        if (g_synth_tab[i].used && g_synth_tab[i].hdi == hdi)
            return &g_synth_tab[i];
    if (!create) return NULL;
    for (int i = 0; i < 8; ++i)
        if (!g_synth_tab[i].used) {
            g_synth_tab[i].used = 1; g_synth_tab[i].hdi = hdi;
            g_synth_tab[i].synth_index = UL_SYNTH_NONE;
            return &g_synth_tab[i];
        }
    return NULL;
}

static HANDLE WINAPI Hook_SetupDiGetClassDevsW(const GUID* guid, PCWSTR enumr,
        HWND hwnd, DWORD flags) {
    HANDLE h = Real_SetupDiGetClassDevsW
        ? Real_SetupDiGetClassDevsW(guid, enumr, hwnd, flags)
        : INVALID_HANDLE_VALUE;
    ul_rt_log("jammausb: SetupDiGetClassDevsW(flags=0x%08lX) -> %p\n",
              (unsigned long)flags, (void*)h);
    return h;
}

static BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HANDLE hdi, PVOID devinfo,
        const GUID* iface, DWORD index, UL_SP_DEVICE_INTERFACE_DATA* ifdata) {
    BOOL ok = Real_SetupDiEnumDeviceInterfaces
        ? Real_SetupDiEnumDeviceInterfaces(hdi, devinfo, iface, index, ifdata)
        : FALSE;
    if (ok) {
        ul_rt_log("jammausb: EnumDeviceInterfaces[%lu] -> real device\n",
                  (unsigned long)index);
        return TRUE;
    }
    /* Real list ended at this index. Optionally append exactly one synthetic
     * JAMMAUSB interface so the game discovers a board to open. */
    if (g_synth && ifdata && ifdata->cbSize >= sizeof(*ifdata)) {
        ul_synth_slot* s = synth_slot_for(hdi, 1);
        if (s && s->synth_index == UL_SYNTH_NONE) {   /* not yet injected */
            s->synth_index = index;
            ifdata->InterfaceClassGuid = iface ? *iface : ifdata->InterfaceClassGuid;
            ifdata->Flags    = SPINT_ACTIVE;
            ifdata->Reserved = UL_JAMMA_SYNTH_MAGIC;
            ul_rt_log("jammausb: EnumDeviceInterfaces[%lu] -> SYNTHETIC JAMMAUSB "
                      "(no real board; ULTRA_JAMMAUSB_SYNTH)\n", (unsigned long)index);
            return TRUE;
        }
    }
    ul_rt_log("jammausb: EnumDeviceInterfaces[%lu] -> end of list\n",
              (unsigned long)index);
    return FALSE;
}

static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailW(HANDLE hdi,
        UL_SP_DEVICE_INTERFACE_DATA* ifdata, UL_SP_DEVICE_INTERFACE_DETAIL_W* detail,
        DWORD size, PDWORD reqsize, PVOID devinfo) {
    if (ifdata && ifdata->Reserved == UL_JAMMA_SYNTH_MAGIC) {
        DWORD need = (DWORD)(FIELD_OFFSET(UL_SP_DEVICE_INTERFACE_DETAIL_W, DevicePath)
                             + (wcslen(g_synth_path_w) + 1) * sizeof(WCHAR));
        if (!detail) {                          /* size-probe call */
            if (reqsize) *reqsize = need;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        if (size < need) {
            if (reqsize) *reqsize = need;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(detail->DevicePath, g_synth_path_w,
               (wcslen(g_synth_path_w) + 1) * sizeof(WCHAR));
        if (reqsize) *reqsize = need;
        ul_rt_log("jammausb: GetDeviceInterfaceDetailW(synthetic) -> %ls\n", g_synth_path_w);
        return TRUE;
    }
    return Real_SetupDiGetDeviceInterfaceDetailW
        ? Real_SetupDiGetDeviceInterfaceDetailW(hdi, ifdata, detail, size, reqsize, devinfo)
        : FALSE;
}

/* Rate-limit counter for the CreateFile hooks (defined here so the ANSI
 * CreateFileA hook below can use it; the wide hook uses it too). */
static volatile LONG g_cf_ctr = 0;

/* ANSI twins of the two discovery hooks (the variants both titles call). */
static HANDLE WINAPI Hook_SetupDiGetClassDevsA(const GUID* guid, PCSTR enumr,
        HWND hwnd, DWORD flags) {
    HANDLE h = Real_SetupDiGetClassDevsA
        ? Real_SetupDiGetClassDevsA(guid, enumr, hwnd, flags)
        : INVALID_HANDLE_VALUE;
    ul_rt_log("jammausb: SetupDiGetClassDevsA(flags=0x%08lX) -> %p\n",
              (unsigned long)flags, (void*)h);
    return h;
}

static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HANDLE hdi,
        UL_SP_DEVICE_INTERFACE_DATA* ifdata, UL_SP_DEVICE_INTERFACE_DETAIL_A* detail,
        DWORD size, PDWORD reqsize, PVOID devinfo) {
    if (ifdata && ifdata->Reserved == UL_JAMMA_SYNTH_MAGIC) {
        DWORD plen = (DWORD)strlen(g_synth_path_a);
        DWORD need = (DWORD)(FIELD_OFFSET(UL_SP_DEVICE_INTERFACE_DETAIL_A, DevicePath)
                             + plen + 1);
        if (!detail || size < need) {           /* size-probe / too small */
            if (reqsize) *reqsize = need;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(detail->DevicePath, g_synth_path_a, plen + 1);
        if (reqsize) *reqsize = need;
        ul_rt_log("jammausb: GetDeviceInterfaceDetailA(synthetic) -> %s\n", g_synth_path_a);
        return TRUE;
    }
    return Real_SetupDiGetDeviceInterfaceDetailA
        ? Real_SetupDiGetDeviceInterfaceDetailA(hdi, ifdata, detail, size, reqsize, devinfo)
        : FALSE;
}

/* CreateFileA: match the HID device-interface path family (ANSI), return
 * sentinel. Same logic as the W hook -- try the real open first (real board
 * wins), else hand back JAMMA_SENTINEL. */
static int path_is_hid_a(LPCSTR path) {
    if (!path) return 0;
    for (const char* p = path; *p; ++p) {
        if ((p[0] == 'h' || p[0] == 'H')
            && (p[1] == 'i' || p[1] == 'I')
            && (p[2] == 'd' || p[2] == 'D')
            && p[3] == '#') return 1;
    }
    return 0;
}
static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_hid_a(name)) {
        HANDLE real = Real_CreateFileA
            ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
            : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) {
            if (ul_rl(&g_cf_ctr))
                ul_rt_log("jammausb: CreateFileA(HID) -> real handle %p\n", (void*)real);
            return real;
        }
        ul_rt_log("jammausb: CreateFileA(HID) real open failed -> JAMMA_SENTINEL (%s)\n",
                  name ? name : "(null)");
        return JAMMA_SENTINEL;
    }
    return Real_CreateFileA
        ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
        : INVALID_HANDLE_VALUE;
}

/* CreateFileW: match the HID device-interface path family, return sentinel. */
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && path_is_hid(name)) {
        /* Try the real open first; if there is a real HID device, use it. */
        HANDLE real = Real_CreateFileW
            ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
            : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) {
            if (ul_rl(&g_cf_ctr))
                ul_rt_log("jammausb: CreateFileW(HID) -> real handle %p\n", (void*)real);
            return real;
        }
        ul_rt_log("jammausb: CreateFileW(HID) real open failed -> JAMMA_SENTINEL\n");
        return JAMMA_SENTINEL;
    }
    return Real_CreateFileW
        ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
        : INVALID_HANDLE_VALUE;
}

static volatile LONG g_rd_ctr = 0;
static volatile LONG g_wr_ctr = 0;
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got,
                                 LPOVERLAPPED ov) {
    if (h == JAMMA_SENTINEL) {
        ul_fill_report((uint8_t*)buf, n);
        if (got) *got = n;
        if (ul_rl(&g_rd_ctr)) {
            const uint8_t* r = (const uint8_t*)buf;
            ul_rt_log("jammausb: ReadFile(sentinel) %lu bytes ov=%p (game IS polling the board); "
                      "report[0..10]=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      (unsigned long)n, (void*)ov,
                      n>0?r[0]:0, n>1?r[1]:0, n>2?r[2]:0, n>3?r[3]:0, n>4?r[4]:0,
                      n>5?r[5]:0, n>6?r[6]:0, n>7?r[7]:0, n>8?r[8]:0, n>9?r[9]:0, n>10?r[10]:0);
        }
        /* Both target titles open the board OVERLAPPED and take the async path:
         * ReadFile -> FALSE + ERROR_IO_PENDING, WaitForSingleObject(ov.hEvent),
         * GetOverlappedResult(handle, &ov). We complete the request immediately
         * but honour that contract: mark the OVERLAPPED complete (Internal =
         * STATUS_SUCCESS, InternalHigh = bytes) and signal its event, then return
         * FALSE/PENDING. The wait returns at once (event already set) and
         * GetOverlappedResult sees Internal != PENDING, so it returns success
         * without ever touching our fake handle. A synchronous caller (ov NULL)
         * gets the classic TRUE + *got. */
        if (ov) {
            ov->Internal     = 0;          /* STATUS_SUCCESS */
            ov->InternalHigh = n;
            if (ov->hEvent) SetEvent(ov->hEvent);
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
        return TRUE;
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote,
                                  LPOVERLAPPED ov) {
    if (h == JAMMA_SENTINEL) {
        (void)buf;
        if (wrote) *wrote = n;   /* lamps/solenoids accepted */
        (void)ov;
        if (ul_rl(&g_wr_ctr))
            ul_rt_log("jammausb: WriteFile(sentinel) %lu bytes accepted (lamps/solenoids)\n",
                      (unsigned long)n);
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (h == JAMMA_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

void ul_rt_jammausb_install(void) {
    if (!ul_rt_env_on("ULTRA_JAMMAUSB")) {
        ul_rt_log("jammausb: skipped (ULTRA_JAMMAUSB!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    /* VID/PID overrides so a title with an atypical JAMMAUSB firmware still
     * matches -- overridable via env for future titles. */
    /* ul_rt_env() returns a pointer into a SINGLE static buffer, so a second
     * call overwrites the first result. Consume each value (strtoul copies the
     * number out) BEFORE the next lookup, or VID silently inherits PID's text.
     * That aliasing armed VID=0x0003 (the PID) instead of 0x0483 and the game's
     * VendorID==0x0483 check rejected the board -> no controls. */
    {
        const char* v = ul_rt_env("ULTRA_JAMMAUSB_VID");
        if (v) g_jamma_vid = (USHORT)strtoul(v, NULL, 0);
    }
    {
        const char* p = ul_rt_env("ULTRA_JAMMAUSB_PID");
        if (p) g_jamma_pid = (USHORT)strtoul(p, NULL, 0);
    }

    /* Build the synthetic device-interface path from the active VID/PID so it is
     * a faithful copy of the real board's HID interface path (lowercase, exactly
     * what the OS emits). H2Overdrive accepts the board by this substring
     * ("vid_09db&pid_0075&mi_0"); Super Bikes ignores the path and matches the
     * VID/PID we return from HidD_GetAttributes(sentinel). The "&mi_0" form is a
     * prefix of the real "&mi_00", so a title matching either still hits. */
    {
        static const char guidtail[] =
            "#7&ultrajusb&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        wsprintfA(g_synth_path_a, "\\\\?\\hid#vid_%04x&pid_%04x&mi_0%s",
                  (unsigned)g_jamma_vid, (unsigned)g_jamma_pid, guidtail);
        /* widen */
        int i = 0;
        for (; g_synth_path_a[i] && i < (int)(sizeof(g_synth_path_w)/sizeof(wchar_t))-1; ++i)
            g_synth_path_w[i] = (wchar_t)(unsigned char)g_synth_path_a[i];
        g_synth_path_w[i] = 0;
    }
    /* Default ON (see g_synth declaration). Explicit opt-out only. */
    {
        const char* sv = ul_rt_env("ULTRA_JAMMAUSB_SYNTH");
        if (sv && sv[0] == '0' && sv[1] == 0) g_synth = 0;
        else if (sv && sv[0] == '1' && sv[1] == 0) g_synth = 1;
    }
    /* Live input default ON; opt out with ULTRA_JAMMAUSB_LIVE=0. */
    {
        const char* lv = ul_rt_env("ULTRA_JAMMAUSB_LIVE");
        if (lv && lv[0] == '0' && lv[1] == 0) g_live = 0;
    }
    /* Driving-title detection picks the pedal idle profile (throttle/brake
     * released = 0x00) and the wheel/throttle/brake analog mapping. */
    {
        const char* t = ul_rt_env("ULTRA_RAWTHRILLS_TITLE");
        if (t && (strstr(t, "tfafsb") || strstr(t, "fnf") || strstr(t, "h2overdrive")
                  || strstr(t, "nitro") || strstr(t, "nascar") || strstr(t, "dirty")
                  || strstr(t, "alpine") || strstr(t, "snocross") || strstr(t, "snowboard")
                  || strstr(t, "cars")))
            g_driving = 1;
        const char* dv = ul_rt_env("ULTRA_JAMMAUSB_DRIVING");   /* explicit override */
        if (dv && dv[0] == '1') g_driving = 1;
        if (dv && dv[0] == '0') g_driving = 0;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE hid = LoadLibraryA("hid.dll");
    HMODULE sup = LoadLibraryA("setupapi.dll");
    if (g_live && !s_xi) {
        const char* xd[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
        for (int i = 0; xd[i] && !s_xi; ++i) {
            HMODULE xh = LoadLibraryA(xd[i]);
            if (xh) s_xi = (pfnXInputGetState)(void*)GetProcAddress(xh, "XInputGetState");
        }
    }
    if (k32) {
        Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k32, "CreateFileW");
        Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k32, "CreateFileA");
        Real_ReadFile    = (pReadFile)   (void*)GetProcAddress(k32, "ReadFile");
        Real_WriteFile   = (pWriteFile)  (void*)GetProcAddress(k32, "WriteFile");
        Real_CloseHandle = (pCloseHandle)(void*)GetProcAddress(k32, "CloseHandle");
    }
    if (hid) {
        Real_HidD_GetAttributes         = (pHidD_GetAttributes)        (void*)GetProcAddress(hid, "HidD_GetAttributes");
        Real_HidD_GetPreparsedData      = (pHidD_GetPreparsedData)     (void*)GetProcAddress(hid, "HidD_GetPreparsedData");
        Real_HidD_FreePreparsedData     = (pHidD_FreePreparsedData)    (void*)GetProcAddress(hid, "HidD_FreePreparsedData");
        Real_HidP_GetCaps               = (pHidP_GetCaps)              (void*)GetProcAddress(hid, "HidP_GetCaps");
        Real_HidD_SetFeature            = (pHidD_SetFeature)           (void*)GetProcAddress(hid, "HidD_SetFeature");
        Real_HidD_GetFeature            = (pHidD_GetFeature)           (void*)GetProcAddress(hid, "HidD_GetFeature");
        Real_HidD_GetInputReport        = (pHidD_GetInputReport)       (void*)GetProcAddress(hid, "HidD_GetInputReport");
        Real_HidD_SetOutputReport       = (pHidD_SetOutputReport)      (void*)GetProcAddress(hid, "HidD_SetOutputReport");
        Real_HidD_GetProductString      = (pHidD_GetProductString)     (void*)GetProcAddress(hid, "HidD_GetProductString");
        Real_HidD_GetManufacturerString = (pHidD_GetManufacturerString)(void*)GetProcAddress(hid, "HidD_GetManufacturerString");
        Real_HidD_GetHidGuid            = (pHidD_GetHidGuid)           (void*)GetProcAddress(hid, "HidD_GetHidGuid");
        Real_HidP_GetButtonCaps         = (pHidP_GetButtonCaps)        (void*)GetProcAddress(hid, "HidP_GetButtonCaps");
        Real_HidP_GetSpecificButtonCaps = (pHidP_GetSpecificButtonCaps)(void*)GetProcAddress(hid, "HidP_GetSpecificButtonCaps");
        Real_HidP_GetValueCaps          = (pHidP_GetValueCaps)         (void*)GetProcAddress(hid, "HidP_GetValueCaps");
        Real_HidP_GetSpecificValueCaps  = (pHidP_GetSpecificValueCaps) (void*)GetProcAddress(hid, "HidP_GetSpecificValueCaps");
        Real_HidP_GetUsages             = (pHidP_GetUsages)            (void*)GetProcAddress(hid, "HidP_GetUsages");
        Real_HidP_GetUsageValue         = (pHidP_GetUsageValue)        (void*)GetProcAddress(hid, "HidP_GetUsageValue");
        Real_HidP_GetData               = (pHidP_GetData)             (void*)GetProcAddress(hid, "HidP_GetData");
        Real_HidP_MaxUsageListLength    = (pHidP_MaxUsageListLength)   (void*)GetProcAddress(hid, "HidP_MaxUsageListLength");
        Real_HidP_MaxDataListLength     = (pHidP_MaxDataListLength)    (void*)GetProcAddress(hid, "HidP_MaxDataListLength");
    }
    if (sup) {
        Real_SetupDiGetClassDevsW             = (pSetupDiGetClassDevsW)            (void*)GetProcAddress(sup, "SetupDiGetClassDevsW");
        Real_SetupDiEnumDeviceInterfaces      = (pSetupDiEnumDeviceInterfaces)     (void*)GetProcAddress(sup, "SetupDiEnumDeviceInterfaces");
        Real_SetupDiGetDeviceInterfaceDetailW = (pSetupDiGetDeviceInterfaceDetailW)(void*)GetProcAddress(sup, "SetupDiGetDeviceInterfaceDetailW");
        Real_SetupDiGetClassDevsA             = (pSetupDiGetClassDevsA)            (void*)GetProcAddress(sup, "SetupDiGetClassDevsA");
        Real_SetupDiGetDeviceInterfaceDetailA = (pSetupDiGetDeviceInterfaceDetailA)(void*)GetProcAddress(sup, "SetupDiGetDeviceInterfaceDetailA");
    }

    /* Patch every game module's IAT for the calls we intercept. Capture the
     * previous IAT target as our Real_ so we CHAIN to any CreateFile hook
     * installed before us (porttalk) instead of jumping straight to raw
     * kernel32 and shadowing it. A stub installed AFTER us (rawart) chains to us
     * the same way -- that is what lets the synthetic HID open reach our
     * sentinel even though rawart owns the IAT slot. */
    { void* pw = ul_rt_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
      if (pw) Real_CreateFileW = (pCreateFileW)pw; }
    { void* pa = ul_rt_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
      if (pa) Real_CreateFileA = (pCreateFileA)pa; }
    ul_rt_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ul_rt_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);
    ul_rt_iat_patch_all("kernel32.dll", "CloseHandle", (void*)Hook_CloseHandle);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetAttributes",         (void*)Hook_HidD_GetAttributes);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetPreparsedData",      (void*)Hook_HidD_GetPreparsedData);
    ul_rt_iat_patch_all("hid.dll", "HidD_FreePreparsedData",     (void*)Hook_HidD_FreePreparsedData);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetCaps",               (void*)Hook_HidP_GetCaps);
    ul_rt_iat_patch_all("hid.dll", "HidD_SetFeature",            (void*)Hook_HidD_SetFeature);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetFeature",            (void*)Hook_HidD_GetFeature);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetInputReport",        (void*)Hook_HidD_GetInputReport);
    ul_rt_iat_patch_all("hid.dll", "HidD_SetOutputReport",       (void*)Hook_HidD_SetOutputReport);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetProductString",      (void*)Hook_HidD_GetProductString);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetManufacturerString", (void*)Hook_HidD_GetManufacturerString);
    ul_rt_iat_patch_all("hid.dll", "HidD_GetHidGuid",            (void*)Hook_HidD_GetHidGuid);

    /* HidP_* PARSE family: prevent the fake-PPD AV and satisfy a HidP-based
     * reader with benign-empty results (see the family above). */
    ul_rt_iat_patch_all("hid.dll", "HidP_GetButtonCaps",         (void*)Hook_HidP_GetButtonCaps);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetSpecificButtonCaps", (void*)Hook_HidP_GetSpecificButtonCaps);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetValueCaps",          (void*)Hook_HidP_GetValueCaps);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetSpecificValueCaps",  (void*)Hook_HidP_GetSpecificValueCaps);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetUsages",             (void*)Hook_HidP_GetUsages);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetUsageValue",         (void*)Hook_HidP_GetUsageValue);
    ul_rt_iat_patch_all("hid.dll", "HidP_GetData",               (void*)Hook_HidP_GetData);
    ul_rt_iat_patch_all("hid.dll", "HidP_MaxUsageListLength",    (void*)Hook_HidP_MaxUsageListLength);
    ul_rt_iat_patch_all("hid.dll", "HidP_MaxDataListLength",     (void*)Hook_HidP_MaxDataListLength);

    /* SetupAPI discovery hooks: tracing always, synthetic injection only when
     * g_synth (default on). Patched into every module that imports them. */
    ul_rt_iat_patch_all("setupapi.dll", "SetupDiGetClassDevsW",             (void*)Hook_SetupDiGetClassDevsW);
    ul_rt_iat_patch_all("setupapi.dll", "SetupDiEnumDeviceInterfaces",      (void*)Hook_SetupDiEnumDeviceInterfaces);
    ul_rt_iat_patch_all("setupapi.dll", "SetupDiGetDeviceInterfaceDetailW", (void*)Hook_SetupDiGetDeviceInterfaceDetailW);
    /* ANSI twins -- the variants BOTH target titles actually import + call. */
    ul_rt_iat_patch_all("setupapi.dll", "SetupDiGetClassDevsA",             (void*)Hook_SetupDiGetClassDevsA);
    ul_rt_iat_patch_all("setupapi.dll", "SetupDiGetDeviceInterfaceDetailA", (void*)Hook_SetupDiGetDeviceInterfaceDetailA);

    ul_rt_log("jammausb: armed (VID=0x%04X PID=0x%04X synth=%d live=%d driving=%d xinput=%d) "
              "synthpath=%s\n",
              g_jamma_vid, g_jamma_pid, g_synth, g_live, g_driving, s_xi ? 1 : 0,
              g_synth_path_a);
}

#endif /* _WIN32 */
