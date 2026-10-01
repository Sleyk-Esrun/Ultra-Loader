/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  namco_dongle.c  --  Namco ES3 USB dongle bypass
 * =============================================================================
 *
 *  Namco ES3 games (MKDX, Tekken 7, WMMT5, ...) check for a proprietary USB
 *  dongle via SetupDi* APIs loaded DYNAMICALLY from setupapi.dll. Without the
 *  physical dongle, SetupDiEnumDeviceInterfaces returns nothing and the game
 *  shows "E19-10 USB Dongle Error 1".
 *
 *  This bypass intercepts the dynamically-resolved SetupDi* functions via the
 *  GetProcAddress hook, fakes one USB dongle device in the enumeration, and
 *  handles the subsequent CreateFile + DeviceIoControl conversation so the game
 *  believes a dongle is present.
 *
 *  Gated by ULTRA_NAMCO_DONGLE=1 (set by namco_es3_main.c).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern void ultra_d3d8_log(const char* fmt, ...);
#define dlog(...) ultra_d3d8_log("[NDONGLE] " __VA_ARGS__)

static int g_enabled = 0;

/* ---- real setupapi function pointers (loaded once) ----------------------- */
typedef HDEVINFO (WINAPI *pSetupDiGetClassDevsW)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL     (WINAPI *pSetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL     (WINAPI *pSetupDiGetDeviceInterfaceDetailW)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL     (WINAPI *pSetupDiDestroyDeviceInfoList)(HDEVINFO);

static pSetupDiGetClassDevsW              Real_GetClassDevs;
static pSetupDiEnumDeviceInterfaces       Real_EnumInterfaces;
static pSetupDiGetDeviceInterfaceDetailW  Real_GetDetail;
static pSetupDiDestroyDeviceInfoList      Real_DestroyList;
static HMODULE                            g_setupapi;

static void load_real_setupapi(void) {
    if (g_setupapi) return;
    g_setupapi = LoadLibraryA("setupapi.dll");
    if (!g_setupapi) return;
    Real_GetClassDevs    = (pSetupDiGetClassDevsW)             GetProcAddress(g_setupapi, "SetupDiGetClassDevsW");
    Real_EnumInterfaces  = (pSetupDiEnumDeviceInterfaces)      GetProcAddress(g_setupapi, "SetupDiEnumDeviceInterfaces");
    Real_GetDetail       = (pSetupDiGetDeviceInterfaceDetailW) GetProcAddress(g_setupapi, "SetupDiGetDeviceInterfaceDetailW");
    Real_DestroyList     = (pSetupDiDestroyDeviceInfoList)     GetProcAddress(g_setupapi, "SetupDiDestroyDeviceInfoList");
}

/* ---- fake dongle state --------------------------------------------------- */
/* We track HDEVINFO sets where the real OS found NO devices, which means the
 * game is looking for something that isn't plugged in (i.e. the dongle). For
 * those sets, we inject one fake device. */
#define MAX_FAKE_SETS 8
static HDEVINFO g_fake_sets[MAX_FAKE_SETS];
static GUID     g_fake_guids[MAX_FAKE_SETS];
static int      g_nfake = 0;

static int is_our_fake_set(HDEVINFO info) {
    for (int i = 0; i < g_nfake; ++i)
        if (g_fake_sets[i] == info) return i;
    return -1;
}

/* The fake device path CreateFile will see. Recognizable by the
 * ULTRA_NAMCO_DONGLE_EMU marker. */
static const wchar_t FAKE_DEVICE_PATH[] =
    L"\\\\?\\USB#VID_0B9A&PID_0168#ULTRA_NAMCO_DONGLE_EMU";

/* The fake handle returned by CreateFile for the dongle device. */
static HANDLE g_fake_handle = NULL;

/* ---- SetupDi wrappers ---------------------------------------------------- */

static HDEVINFO WINAPI Wrap_SetupDiGetClassDevsW(
    const GUID* guid, PCWSTR enumerator, HWND parent, DWORD flags)
{
    HDEVINFO real = Real_GetClassDevs ? Real_GetClassDevs(guid, enumerator, parent, flags) : INVALID_HANDLE_VALUE;

    if (guid) {
        dlog("SetupDiGetClassDevsW GUID={%08lX-%04X-%04X-...} flags=%lx -> %p",
             guid->Data1, guid->Data2, guid->Data3, flags, (void*)real);
    }

    /* If the real OS returned a valid set, check if it actually contains any
     * devices. If it doesn't (first enum fails), register it as a fake set
     * so we can inject our dongle. */
    if (real != INVALID_HANDLE_VALUE && Real_EnumInterfaces) {
        SP_DEVICE_INTERFACE_DATA test;
        memset(&test, 0, sizeof test);
        test.cbSize = sizeof test;
        BOOL has = Real_EnumInterfaces(real, NULL, guid, 0, &test);
        if (!has) {
            /* No real devices - this is likely the dongle query. Register it. */
            if (g_nfake < MAX_FAKE_SETS) {
                g_fake_sets[g_nfake] = real;
                if (guid) g_fake_guids[g_nfake] = *guid;
                else      memset(&g_fake_guids[g_nfake], 0, sizeof(GUID));
                g_nfake++;
                dlog("  -> empty set, registered as fake #%d (will inject dongle)", g_nfake);
            }
        } else {
            dlog("  -> set has real devices, pass-through");
        }
    }
    return real;
}

static BOOL WINAPI Wrap_SetupDiEnumDeviceInterfaces(
    HDEVINFO info, PSP_DEVINFO_DATA devdata, const GUID* guid,
    DWORD idx, PSP_DEVICE_INTERFACE_DATA data)
{
    int fi = is_our_fake_set(info);
    if (fi >= 0) {
        if (idx == 0 && data) {
            memset(data, 0, sizeof *data);
            data->cbSize = sizeof *data;
            data->InterfaceClassGuid = g_fake_guids[fi];
            data->Flags = 1; /* SPINT_ACTIVE */
            data->Reserved = (ULONG_PTR)0xABCD0001;
            dlog("SetupDiEnumDeviceInterfaces(fake#%d, idx=0) -> TRUE (injected)", fi);
            return TRUE;
        }
        SetLastError(ERROR_NO_MORE_ITEMS);
        dlog("SetupDiEnumDeviceInterfaces(fake#%d, idx=%lu) -> FALSE (end)", fi, idx);
        return FALSE;
    }
    return Real_EnumInterfaces ? Real_EnumInterfaces(info, devdata, guid, idx, data) : FALSE;
}

static BOOL WINAPI Wrap_SetupDiGetDeviceInterfaceDetailW(
    HDEVINFO info, PSP_DEVICE_INTERFACE_DATA iface,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD sz,
    PDWORD required, PSP_DEVINFO_DATA devinfo)
{
    int fi = is_our_fake_set(info);
    if (fi >= 0) {
        DWORD need = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) +
                     sizeof(FAKE_DEVICE_PATH));
        if (required) *required = need;
        if (!detail) {
            /* MKDX calls detail=NULL as a pure presence probe - it never allocates a
               buffer and calls again.  Return TRUE so the game sees "dongle reachable". */
            dlog("SetupDiGetDeviceInterfaceDetailW(fake#%d) detail=NULL need=%lu -> TRUE (presence)", fi, need);
            return TRUE;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        memcpy(detail->DevicePath, FAKE_DEVICE_PATH, sizeof(FAKE_DEVICE_PATH));
        if (devinfo) {
            memset(devinfo, 0, sizeof *devinfo);
            devinfo->cbSize = sizeof *devinfo;
        }
        dlog("SetupDiGetDeviceInterfaceDetailW(fake#%d) sz=%lu -> path=%ls", fi, sz, detail->DevicePath);
        return TRUE;
    }
    return Real_GetDetail ? Real_GetDetail(info, iface, detail, sz, required, devinfo) : FALSE;
}

static const char FAKE_DEVICE_PATH_A[] =
    "\\\\?\\USB#VID_0B9A&PID_0168#ULTRA_NAMCO_DONGLE_EMU";

static BOOL WINAPI Wrap_SetupDiGetDeviceInterfaceDetailA(
    HDEVINFO info, PSP_DEVICE_INTERFACE_DATA iface,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD sz,
    PDWORD required, PSP_DEVINFO_DATA devinfo)
{
    int fi = is_our_fake_set(info);
    if (fi >= 0) {
        DWORD need = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) +
                     sizeof(FAKE_DEVICE_PATH_A));
        if (required) *required = need;
        if (!detail) {
            dlog("SetupDiGetDeviceInterfaceDetailA(fake#%d) detail=NULL need=%lu -> TRUE (presence)", fi, need);
            return TRUE;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        memcpy(detail->DevicePath, FAKE_DEVICE_PATH_A, sizeof(FAKE_DEVICE_PATH_A));
        if (devinfo) {
            memset(devinfo, 0, sizeof *devinfo);
            devinfo->cbSize = sizeof *devinfo;
        }
        dlog("SetupDiGetDeviceInterfaceDetailA(fake#%d) sz=%lu -> path=%s", fi, sz, detail->DevicePath);
        return TRUE;
    }
    typedef BOOL (WINAPI *pDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
    static pDetailA Real_GetDetailA;
    if (!Real_GetDetailA && g_setupapi)
        Real_GetDetailA = (pDetailA)GetProcAddress(g_setupapi, "SetupDiGetDeviceInterfaceDetailA");
    return Real_GetDetailA ? Real_GetDetailA(info, iface, detail, sz, required, devinfo) : FALSE;
}

static BOOL WINAPI Wrap_SetupDiDestroyDeviceInfoList(HDEVINFO info)
{
    int fi = is_our_fake_set(info);
    if (fi >= 0) {
        g_fake_sets[fi] = g_fake_sets[g_nfake - 1];
        g_fake_guids[fi] = g_fake_guids[g_nfake - 1];
        g_nfake--;
    }
    return Real_DestroyList ? Real_DestroyList(info) : TRUE;
}

/* ---- GetProcAddress interception ----------------------------------------- */

FARPROC ultra_namco_dongle_getproc(HMODULE mod, LPCSTR name) {
    if (!g_enabled) return NULL;
    if (!name || ((uintptr_t)name >> 16) == 0) return NULL;

    load_real_setupapi();

    /* Only intercept when the module is setupapi.dll */
    if (g_setupapi && mod == g_setupapi) {
        if (strcmp(name, "SetupDiGetClassDevsW") == 0)              return (FARPROC)Wrap_SetupDiGetClassDevsW;
        if (strcmp(name, "SetupDiEnumDeviceInterfaces") == 0)       return (FARPROC)Wrap_SetupDiEnumDeviceInterfaces;
        if (strcmp(name, "SetupDiGetDeviceInterfaceDetailW") == 0)  return (FARPROC)Wrap_SetupDiGetDeviceInterfaceDetailW;
        if (strcmp(name, "SetupDiGetDeviceInterfaceDetailA") == 0)  return (FARPROC)Wrap_SetupDiGetDeviceInterfaceDetailA;
        if (strcmp(name, "SetupDiDestroyDeviceInfoList") == 0)      return (FARPROC)Wrap_SetupDiDestroyDeviceInfoList;
    }
    return NULL;
}

/* ---- CreateFile interception --------------------------------------------- */

int ultra_namco_dongle_is_device_path(const char* path) {
    if (!g_enabled || !path) return 0;
    return strstr(path, "ULTRA_NAMCO_DONGLE_EMU") != NULL;
}

int ultra_namco_dongle_is_device_pathW(const wchar_t* path) {
    if (!g_enabled || !path) return 0;
    return wcsstr(path, L"ULTRA_NAMCO_DONGLE_EMU") != NULL;
}

HANDLE ultra_namco_dongle_open_device(void) {
    if (!g_fake_handle)
        g_fake_handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    dlog("CreateFile(dongle) -> fake handle %p", (void*)g_fake_handle);
    return g_fake_handle ? g_fake_handle : (HANDLE)(uintptr_t)0xDEAD0002;
}

/* ---- DeviceIoControl interception ---------------------------------------- */

int ultra_namco_dongle_is_handle(HANDLE h) {
    return g_enabled && g_fake_handle && h == g_fake_handle;
}

BOOL ultra_namco_dongle_ioctl(HANDLE h, DWORD code,
    LPVOID in_buf, DWORD in_sz, LPVOID out_buf, DWORD out_sz,
    LPDWORD bytes_ret, LPOVERLAPPED ov)
{
    (void)h; (void)ov;
    dlog("DeviceIoControl(dongle) code=0x%08lX in=%lu out=%lu", code, in_sz, out_sz);

    if (in_buf && in_sz > 0) {
        char hex[128]; int pos = 0;
        for (DWORD i = 0; i < in_sz && pos < 120; ++i)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", ((BYTE*)in_buf)[i]);
        dlog("  -> in: %s", hex);
    }

    /* Return success with zeroed output. The game will check the response;
     * if it doesn't like all-zeros, we'll see the next error code in the logs
     * and can adjust. For a first pass, this gets us past E19-10. */
    if (out_buf && out_sz > 0)
        memset(out_buf, 0, out_sz);
    if (bytes_ret)
        *bytes_ret = out_sz;
    return TRUE;
}

/* ---- MKDX binary patches (SHA-gated) ------------------------------------ */
/* MKDX checks the dongle via two code paths that read DongleData/dongle.bin
 * and talk to the USB device.  API hooking alone is insufficient because the
 * game only uses SetupDi as a presence probe (detail=NULL) and never opens
 * the device through that path.  Instead we patch the check functions:
 *
 *  Patch 1  RVA 0x1EFCC0  -  periodic dongle poll
 *           Original: 55 8B EC  (push ebp; mov ebp,esp)
 *           Patched:  B0 01 C3  (mov al,1; ret)   -> always "dongle OK"
 *
 *  Patch 2  RVA 0x1EF669  -  DongleDevice constructor call to init fn
 *           Original: E8 42 07 00 00 8A 97 E0 0C 00 00
 *                     (call dongle_init; mov dl,[edi+0xCE0])
 *           Patched:  90 90 90 90 90 B2 01 90 90 90 90
 *                     (5 NOPs; mov dl,1; 4 NOPs)    -> skip init, force ok
 */
static void patch_bytes(BYTE* addr, const BYTE* expected, const BYTE* patch, size_t n, const char* tag) {
    if (memcmp(addr, expected, n) != 0) {
        dlog("PATCH %s: signature mismatch at %p, skipped", tag, (void*)addr);
        return;
    }
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        dlog("PATCH %s: VirtualProtect failed (%lu), skipped", tag, GetLastError());
        return;
    }
    memcpy(addr, patch, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    dlog("PATCH %s: applied %zu bytes at %p", tag, n, (void*)addr);
}

static void ultra_namco_dongle_patch_exe(void) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) return;

    /* Patch 1: periodic dongle check -> mov al,1; ret */
    {
        static const BYTE exp[] = { 0x55, 0x8B, 0xEC };
        static const BYTE pat[] = { 0xB0, 0x01, 0xC3 };
        patch_bytes(base + 0x1EFCC0, exp, pat, 3, "dongle_poll");
    }
    /* Patch 2: constructor - NOP the init call, force dl=1 */
    {
        static const BYTE exp[] = { 0xE8,0x42,0x07,0x00,0x00, 0x8A,0x97,0xE0,0x0C,0x00,0x00 };
        static const BYTE pat[] = { 0x90,0x90,0x90,0x90,0x90, 0xB2,0x01, 0x90,0x90,0x90,0x90 };
        patch_bytes(base + 0x1EF669, exp, pat, 11, "dongle_ctor");
    }
    /* Patch 3: DongleDevice::Update - the validation state machine
     * Has 9 flag-clear sites and 2 error-state writes (0x53) that reset
     * the dongle status even after ctor and periodic check patches.
     * Kill the entire function: mov al,1; ret */
    {
        static const BYTE exp[] = { 0x55, 0x8B, 0xEC };
        static const BYTE pat[] = { 0xB0, 0x01, 0xC3 };
        patch_bytes(base + 0x1EF7D0, exp, pat, 3, "dongle_update");
    }
}

/* ---- install ------------------------------------------------------------- */

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

void ultra_namco_dongle_install(void) {
    const char* e = getenv("ULTRA_NAMCO_DONGLE");
    if (!e || e[0] != '1') {
        char buf[8] = {0};
        if (GetEnvironmentVariableA("ULTRA_NAMCO_DONGLE", buf, sizeof buf) > 0
            && buf[0] == '1') {
            e = "1";
        } else {
            return;
        }
    }
    g_enabled = 1;
    load_real_setupapi();

    ultra_iat_patch_all("setupapi.dll", "SetupDiGetClassDevsW",              (void*)Wrap_SetupDiGetClassDevsW);
    ultra_iat_patch_all("setupapi.dll", "SetupDiEnumDeviceInterfaces",       (void*)Wrap_SetupDiEnumDeviceInterfaces);
    ultra_iat_patch_all("setupapi.dll", "SetupDiGetDeviceInterfaceDetailW",  (void*)Wrap_SetupDiGetDeviceInterfaceDetailW);
    ultra_iat_patch_all("setupapi.dll", "SetupDiGetDeviceInterfaceDetailA",  (void*)Wrap_SetupDiGetDeviceInterfaceDetailA);
    ultra_iat_patch_all("setupapi.dll", "SetupDiDestroyDeviceInfoList",      (void*)Wrap_SetupDiDestroyDeviceInfoList);

    ultra_namco_dongle_patch_exe();

    dlog("Namco USB dongle bypass ARMED (IAT + GetProcAddress + binary patch)");
}

#endif /* _WIN32 */
