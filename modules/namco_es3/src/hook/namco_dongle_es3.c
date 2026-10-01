/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/namco_dongle.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * =============================================================================
 *  namco_dongle_es3.c  --  Namco ES3 USB dongle bypass
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
/* Returns 1 if the patch actually went in, 0 if it was skipped. Callers that
 * apply a SET of patches use this to report how many landed, so a partial
 * match shows up in the log instead of looking like a clean success. */
static int patch_bytes(BYTE* addr, const BYTE* expected, const BYTE* patch, size_t n, const char* tag) {
    if (memcmp(addr, expected, n) != 0) {
        dlog("PATCH %s: signature mismatch at %p, skipped", tag, (void*)addr);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        dlog("PATCH %s: VirtualProtect failed (%lu), skipped", tag, GetLastError());
        return 0;
    }
    memcpy(addr, patch, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    dlog("PATCH %s: applied %zu bytes at %p", tag, n, (void*)addr);
    return 1;
}

/* Which of the three binary patches to apply. Default is all three, exactly as
 * before, so normal play is unchanged.
 *
 * WHY THIS IS SWITCHABLE (2026-09-10): patch 3 replaces the whole of
 * DongleDevice::Update with "mov al,1; ret". If that function also INITIALIZES
 * state -- not just validates it -- then skipping it leaves memory
 * uninitialised that the game later calls through, and the failure is
 * non-deterministic because it depends on whatever garbage was already there.
 * Measured on Mario Kart GP DX: roughly one launch in two died with an
 * EXECUTE/DEP fault at a mid-instruction address inside MSVCR100.dll, with all
 * eight stack frames in the game's own code and none in ours -- the signature
 * of a corrupted function pointer, not of a bad hook.
 *
 *   ULTRA_NDONGLE_PATCHES=poll,ctor,update   (default: all three)
 *   ULTRA_NDONGLE_PATCHES=poll,ctor          leave Update running
 *   ULTRA_NDONGLE_PATCHES=none               no binary patching at all
 */
static int ndongle_patch_enabled(const char *which) {
    char buf[64];
    const char *e = getenv("ULTRA_NDONGLE_PATCHES");
    if (!e || !*e) {
        if (GetEnvironmentVariableA("ULTRA_NDONGLE_PATCHES", buf, sizeof buf) > 0)
            e = buf;
        else
            return 1;                       /* default: every patch, as before */
    }
    if (_stricmp(e, "none") == 0) return 0;
    if (_stricmp(e, "all")  == 0) return 1;
    /* substring match is safe here: the three names share no prefix */
    return strstr(e, which) != NULL;
}

/* ---- Mario Kart Arcade GP DX: STR PCB (steering board) bypass -------------
 *
 * E22-12 "STR PCB communication error" is the last wall on MK_AGP3_FINAL.exe.
 * The STR board is NOT a second JVS node and NOT a second COM port - it is a
 * BYTE STREAM tunnelled through the Namco manufacturer command 0x70 on node 1.
 * Bytes the board sends are pushed into a 256-entry ring at node+0x484 by the
 * writer at 0x839AD0, and the driver at 0x5ECxxx pops them with a read(buf,n)
 * at 0x839B90.
 *
 * Five handlers read a reply and gate on one byte of it. Every one has the
 * same shape:
 *
 *      ring_read(&this[0x88], N);          N = 7, 7, 0x16, 0x0A, 7
 *      cmp byte [this+0x8C], '1'           resp[4] must be ASCII '1'
 *      jne  fail                           <- the gate
 *      ...                                 success: state = 6
 *
 * with the last one inverted (resp[4] must be neither 0 nor '0'). We answer
 * every 0x70 read request with "0 bytes available", so the buffer never fills,
 * resp[4] is never '1', every handler fails, and the driver eventually gives
 * up with E22-12.
 *
 * Emulating the board properly means reimplementing its serial protocol (the
 * command builder at 0x5ECFA0 frames 02 'A' .. so it is an STX + ASCII
 * protocol). That is a large unknown. The cheaper route, and the one asked
 * for, is to remove the gate: NOP the seven conditional jumps so each handler
 * takes its success path regardless of what the board did or did not say.
 *
 * Consequence to be honest about: the game then advances on a buffer that is
 * all zeroes, so anything it reads back from the STR board (steering feedback,
 * board status) is zero. Wheel INPUT is unaffected - that arrives over the
 * normal JVS analog channels, not this stream. If a later screen needs real
 * values from the board, emulation becomes the fallback.
 *
 * ULTRA_NAMCO_STR_BYPASS=0 disables this. */
static void ultra_namco_str_bypass(BYTE* base) {
    static const struct { unsigned rva; BYTE exp[2]; const char* tag; } G[] = {
        { 0x1ECF7F, { 0x75, 0x17 }, "str_ack1"    },  /* resp[4]=='1', 7-byte  */
        { 0x1ED03F, { 0x75, 0x17 }, "str_ack2"    },  /* resp[4]=='1', 7-byte  */
        { 0x1ED0EF, { 0x75, 0x47 }, "str_status"  },  /* resp[4]=='1', 22-byte */
        { 0x1ED1DD, { 0x75, 0x32 }, "str_param_a" },  /* resp[4]=='1', 10-byte */
        { 0x1ED1E7, { 0x74, 0x28 }, "str_param_b" },  /* resp[6]!=0            */
        { 0x1ED2B0, { 0x74, 0x1B }, "str_alive_a" },  /* resp[4]!=0            */
        { 0x1ED2B4, { 0x74, 0x17 }, "str_alive_b" },  /* resp[4]!='0'          */
    };
    static const BYTE NOP2[2] = { 0x90, 0x90 };
    /* DEFAULT OFF as of 2026-09-11, because it does not work and actively makes
     * things worse. Tested live: all 7 gates patch cleanly, and the JVS traffic
     * afterwards is byte-for-byte identical - same E22-12.
     *
     * Why: the driver's dispatcher at 0x5ECE2C reads its state from [edi+4] and
     * does `cmp eax,4 / ja skip` before indexing the handler table. Each handler
     * sets state = 6 on SUCCESS, and 6 > 4, so 6 is the terminal idle state.
     * Forcing handler 1 to succeed therefore parks the entire driver at "done"
     * before handlers 2..5 ever run, so their result flags (+0x115, +0x118,
     * +0x11B) stay unset and the game still declares the board missing.
     *
     * The gates are not the check - they are five steps of a handshake that has
     * to actually happen. Getting past this needs real replies fed into the ring
     * buffer, not removed conditionals. Kept here (opt-in via
     * ULTRA_NAMCO_STR_BYPASS=1) only as a documented dead end so nobody spends
     * another evening re-deriving it. */
    const char* e = getenv("ULTRA_NAMCO_STR_BYPASS");
    int i, applied = 0;
    if (!(e && e[0] == '1')) { dlog("STR bypass: OFF (proven ineffective; set ULTRA_NAMCO_STR_BYPASS=1 to force)"); return; }
    for (i = 0; i < (int)(sizeof G / sizeof G[0]); ++i) {
        /* patch_bytes verifies the signature first, so a different build or a
         * mis-derived base skips rather than corrupting code. */
        if (patch_bytes(base + G[i].rva, G[i].exp, NOP2, 2, G[i].tag)) applied++;
    }
    dlog("STR bypass: %d/%d gates neutralized (E22-12 path)", applied,
         (int)(sizeof G / sizeof G[0]));
}

static void ultra_namco_dongle_patch_exe(void) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) return;

    ultra_namco_str_bypass(base);

    /* Patch 1: periodic dongle check -> mov al,1; ret */
    {
        static const BYTE exp[] = { 0x55, 0x8B, 0xEC };
        static const BYTE pat[] = { 0xB0, 0x01, 0xC3 };
        if (ndongle_patch_enabled("poll"))
            patch_bytes(base + 0x1EFCC0, exp, pat, 3, "dongle_poll");
        else dlog("PATCH dongle_poll: DISABLED by ULTRA_NDONGLE_PATCHES");
    }
    /* Patch 2: constructor - NOP the init call, force dl=1 */
    {
        static const BYTE exp[] = { 0xE8,0x42,0x07,0x00,0x00, 0x8A,0x97,0xE0,0x0C,0x00,0x00 };
        static const BYTE pat[] = { 0x90,0x90,0x90,0x90,0x90, 0xB2,0x01, 0x90,0x90,0x90,0x90 };
        if (ndongle_patch_enabled("ctor"))
            patch_bytes(base + 0x1EF669, exp, pat, 11, "dongle_ctor");
        else dlog("PATCH dongle_ctor: DISABLED by ULTRA_NDONGLE_PATCHES");
    }
    /* Patch 3: DongleDevice::Update - the validation state machine
     * Has 9 flag-clear sites and 2 error-state writes (0x53) that reset
     * the dongle status even after ctor and periodic check patches.
     * Kill the entire function: mov al,1; ret */
    {
        static const BYTE exp[] = { 0x55, 0x8B, 0xEC };
        static const BYTE pat[] = { 0xB0, 0x01, 0xC3 };
        if (ndongle_patch_enabled("update"))
            patch_bytes(base + 0x1EF7D0, exp, pat, 3, "dongle_update");
        else dlog("PATCH dongle_update: DISABLED by ULTRA_NDONGLE_PATCHES");
    }

    /* Patch 4 (Pokken Tournament, ferrum_app.exe 64-bit): stop SYSTEM ERROR
     * 19-x being raised at all.
     *
     * The three patches above are Mario Kart's 32-bit dongle; their signatures
     * simply will not match here, and patch_bytes logs a mismatch and skips.
     * This one is the equivalent for this title, and it is a better fix than
     * the error-gate byte because it removes the error instead of declaring it
     * harmless: no corner entry, no record registered, nothing to latch on.
     *
     * The cabinet's serial comes from a USB dongle over 6 retries with 500 ms
     * sleeps. There is no dongle, so the retries run out and the S/N check
     * stores an error id into the global at 0x140E9CBBC. These four writes are
     * the ONLY writers of that global - verified by scanning the image - so
     * with all four NOPed it keeps its initial 0 and the poll that reads it
     * reports healthy forever:
     *
     *     0x67F428   mov dword ptr [0x140E9CBBC], 1303h
     *     0x67F474   mov dword ptr [0x140E9CBBC], 1302h
     *     0x67F4C0   mov dword ptr [0x140E9CBBC], 1303h
     *     0x67F56E   mov dword ptr [0x140E9CBBC], 1301h     <- the one we saw
     *
     * Each is a 10-byte `C7 05 disp32 imm32`, and each expected pattern below
     * was read out of this exact dump, so a different build mismatches and is
     * skipped rather than silently corrupted.
     *
     * This does NOT make the S/N row read OK - that row is written from a
     * different source - so SYSTEM CHECK will still show S/N NG!. It stops the
     * missing dongle from ending the run, which is the part that matters.
     *
     *     ULTRA_NDONGLE_PATCHES=poll,ctor,update   omit "sn19" to leave it in
     */
    {
        /* disp32 bytes in each C7 05 instruction are ASLR-relative: adjust at
         * runtime from the preferred base (0x00400000) to the actual load base. */
        static const struct { unsigned rva; DWORD pref_disp32; BYTE imm[4]; const char* tag; } SN19[] = {
            { 0x67F428, 0x0081D78A, {0x03,0x13,0x00,0x00}, "sn19_a" },
            { 0x67F474, 0x0081D73E, {0x02,0x13,0x00,0x00}, "sn19_b" },
            { 0x67F4C0, 0x0081D6F2, {0x03,0x13,0x00,0x00}, "sn19_c" },
            { 0x67F56E, 0x0081D644, {0x01,0x13,0x00,0x00}, "sn19_d" },
        };
        static const BYTE NOP10[10] = {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
        if (ndongle_patch_enabled("sn19")) {
            DWORD delta32 = (DWORD)((uintptr_t)base - (uintptr_t)0x00400000);
            int applied = 0, i;
            for (i = 0; i < (int)(sizeof SN19 / sizeof SN19[0]); i++) {
                DWORD adj = SN19[i].pref_disp32 + delta32;
                BYTE exp[10] = {
                    0xC7, 0x05,
                    (BYTE)(adj), (BYTE)(adj>>8), (BYTE)(adj>>16), (BYTE)(adj>>24),
                    SN19[i].imm[0], SN19[i].imm[1], SN19[i].imm[2], SN19[i].imm[3]
                };
                if (patch_bytes(base + SN19[i].rva, exp, NOP10, 10, SN19[i].tag))
                    applied++;
            }
            if (applied)
                dlog("PATCH sn19: %d/4 SYSTEM ERROR 19-x raise sites removed"
                     " (missing dongle no longer ends the run)", applied);
        } else dlog("PATCH sn19: DISABLED by ULTRA_NDONGLE_PATCHES");
    }
}

/* ---- install ------------------------------------------------------------- */

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* ---- Config Manager (PnP device-tree) probe -------------------------------
 *
 * WHY: Mario Kart Arcade GP DX imports these from SETUPAPI.dll --
 *
 *     CM_Locate_DevNodeW   CM_Get_Child   CM_Get_Sibling   CM_Get_Parent
 *     CM_Get_DevNode_Registry_PropertyW
 *
 * -- and walks the PnP device tree with them. Nothing in this tree emulates
 * that path: we emulate SetupDi* only. So a board the game finds by walking
 * the device tree is invisible to us, and MKDX stops with
 * "E22-12  STR PCB communication error" (STR = the steering / force-feedback
 * board). It is not a serial device: measured 2026-09-10, the game opens
 * \\.\COM1, configures it with SetupComm/SetCommState/SetCommTimeouts, and
 * then never reads or writes a single byte on it.
 *
 * WHAT THIS IS: a PASS-THROUGH PROBE, not an emulation. Every call is
 * forwarded to the real implementation unchanged; we only log. We cannot
 * fabricate a device node until we know which instance IDs the game looks up
 * and which registry properties it reads, and a probe physically cannot
 * regress a board that works today. The emulation is written afterwards, from
 * what the log shows.
 *
 * Gated on ULTRA_NAMCO_CM_PROBE=1 -- off unless explicitly asked for.
 * ------------------------------------------------------------------------- */

/* Declared locally rather than including <cfgmgr32.h>: the mingw header pulls
 * in a different SDK level across the board copies, and all we need is the
 * ABI, which has been stable since Windows 2000. */
typedef DWORD  UJ_CONFIGRET;
typedef DWORD  UJ_DEVINST;

typedef UJ_CONFIGRET (WINAPI *pCM_Locate_DevNodeW)(UJ_DEVINST*, PCWSTR, ULONG);
typedef UJ_CONFIGRET (WINAPI *pCM_Get_Child)(UJ_DEVINST*, UJ_DEVINST, ULONG);
typedef UJ_CONFIGRET (WINAPI *pCM_Get_Sibling)(UJ_DEVINST*, UJ_DEVINST, ULONG);
typedef UJ_CONFIGRET (WINAPI *pCM_Get_Parent)(UJ_DEVINST*, UJ_DEVINST, ULONG);
typedef UJ_CONFIGRET (WINAPI *pCM_Get_DevNode_Registry_PropertyW)(
    UJ_DEVINST, ULONG, PULONG, PVOID, PULONG, ULONG);

static pCM_Locate_DevNodeW                Real_CM_Locate_DevNodeW;
static pCM_Get_Child                      Real_CM_Get_Child;
static pCM_Get_Sibling                    Real_CM_Get_Sibling;
static pCM_Get_Parent                     Real_CM_Get_Parent;
static pCM_Get_DevNode_Registry_PropertyW Real_CM_GetProp;

static int g_cm_probe;

/* CM_DRP_* values we bother to name in the log. */
static const char *cm_prop_name(ULONG p)
{
    switch (p) {
    case 0x01: return "DEVICEDESC";
    case 0x02: return "HARDWAREID";
    case 0x03: return "COMPATIBLEIDS";
    case 0x05: return "SERVICE";
    case 0x08: return "CLASS";
    case 0x09: return "CLASSGUID";
    case 0x0A: return "DRIVER";
    case 0x0C: return "MFG";
    case 0x0D: return "FRIENDLYNAME";
    case 0x0E: return "LOCATION_INFORMATION";
    case 0x0F: return "PHYSICAL_DEVICE_OBJECT_NAME";
    case 0x12: return "ENUMERATOR_NAME";
    default:   return "?";
    }
}

/* Log a wide value without dragging in a wide printf: the strings we care
 * about (hardware IDs, device descriptions) are ASCII in practice. */
static void cm_log_wide(const char *tag, const wchar_t *w, ULONG bytes)
{
    char a[256];
    int n;
    if (!w || !bytes) { dlog("  %s = (empty)", tag); return; }
    n = WideCharToMultiByte(CP_ACP, 0, w, -1, a, (int)sizeof a - 1, NULL, NULL);
    if (n <= 0) { dlog("  %s = (%lu bytes, not convertible)", tag, bytes); return; }
    a[n] = 0;
    dlog("  %s = \"%s\"", tag, a);
}

static UJ_CONFIGRET WINAPI Wrap_CM_Locate_DevNodeW(UJ_DEVINST *dn, PCWSTR id, ULONG flags)
{
    UJ_CONFIGRET r = Real_CM_Locate_DevNodeW
                   ? Real_CM_Locate_DevNodeW(dn, id, flags) : 0x0D /* CR_FAILURE */;
    char a[256];
    if (id) {
        int n = WideCharToMultiByte(CP_ACP, 0, id, -1, a, (int)sizeof a - 1, NULL, NULL);
        if (n > 0) a[n] = 0; else strcpy(a, "(?)");
    } else {
        strcpy(a, "(NULL = device tree root)");
    }
    dlog("CM_Locate_DevNodeW(\"%s\", flags=0x%lX) -> cr=%lu devinst=%lu",
         a, flags, r, (dn && r == 0) ? *dn : 0);
    return r;
}

static UJ_CONFIGRET WINAPI Wrap_CM_Get_Child(UJ_DEVINST *child, UJ_DEVINST parent, ULONG flags)
{
    UJ_CONFIGRET r = Real_CM_Get_Child ? Real_CM_Get_Child(child, parent, flags) : 0x0D;
    dlog("CM_Get_Child(parent=%lu) -> cr=%lu child=%lu",
         parent, r, (child && r == 0) ? *child : 0);
    return r;
}

static UJ_CONFIGRET WINAPI Wrap_CM_Get_Sibling(UJ_DEVINST *sib, UJ_DEVINST cur, ULONG flags)
{
    UJ_CONFIGRET r = Real_CM_Get_Sibling ? Real_CM_Get_Sibling(sib, cur, flags) : 0x0D;
    dlog("CM_Get_Sibling(cur=%lu) -> cr=%lu sibling=%lu",
         cur, r, (sib && r == 0) ? *sib : 0);
    return r;
}

static UJ_CONFIGRET WINAPI Wrap_CM_Get_Parent(UJ_DEVINST *par, UJ_DEVINST cur, ULONG flags)
{
    UJ_CONFIGRET r = Real_CM_Get_Parent ? Real_CM_Get_Parent(par, cur, flags) : 0x0D;
    dlog("CM_Get_Parent(cur=%lu) -> cr=%lu parent=%lu",
         cur, r, (par && r == 0) ? *par : 0);
    return r;
}

static UJ_CONFIGRET WINAPI Wrap_CM_Get_DevNode_Registry_PropertyW(
    UJ_DEVINST dn, ULONG prop, PULONG type, PVOID buf, PULONG len, ULONG flags)
{
    ULONG want = len ? *len : 0;
    UJ_CONFIGRET r = Real_CM_GetProp
                   ? Real_CM_GetProp(dn, prop, type, buf, len, flags) : 0x0D;
    dlog("CM_Get_DevNode_Registry_PropertyW(devinst=%lu, %s(0x%lX), inlen=%lu) -> cr=%lu outlen=%lu",
         dn, cm_prop_name(prop), prop, want, r, len ? *len : 0);
    /* cr==0 (CR_SUCCESS) with a buffer means we can show what the game saw --
     * this is the line that will name the STR PCB's hardware ID if the tree
     * walk ever reaches it. */
    if (r == 0 && buf && len && *len)
        cm_log_wide(cm_prop_name(prop), (const wchar_t *)buf, *len);
    return r;
}

static void ultra_namco_cm_probe_install(void)
{
    HMODULE m;
    const char *e = getenv("ULTRA_NAMCO_CM_PROBE");
    if (!e || e[0] != '1') {
        char buf[8] = {0};
        if (!(GetEnvironmentVariableA("ULTRA_NAMCO_CM_PROBE", buf, sizeof buf) > 0
              && buf[0] == '1'))
            return;
    }
    /* CM_* live in cfgmgr32.dll and are re-exported by setupapi.dll; the game
     * imports them from setupapi, so patch that IAT entry, but resolve the
     * real implementation from whichever module actually has it. */
    m = GetModuleHandleA("setupapi.dll");
    if (!m) m = LoadLibraryA("setupapi.dll");
    if (!m) { dlog("[CM] setupapi.dll unavailable -- probe not installed"); return; }

    Real_CM_Locate_DevNodeW = (pCM_Locate_DevNodeW)GetProcAddress(m, "CM_Locate_DevNodeW");
    Real_CM_Get_Child       = (pCM_Get_Child)      GetProcAddress(m, "CM_Get_Child");
    Real_CM_Get_Sibling     = (pCM_Get_Sibling)    GetProcAddress(m, "CM_Get_Sibling");
    Real_CM_Get_Parent      = (pCM_Get_Parent)     GetProcAddress(m, "CM_Get_Parent");
    Real_CM_GetProp         = (pCM_Get_DevNode_Registry_PropertyW)
                              GetProcAddress(m, "CM_Get_DevNode_Registry_PropertyW");

    ultra_iat_patch_all("setupapi.dll", "CM_Locate_DevNodeW", (void*)Wrap_CM_Locate_DevNodeW);
    ultra_iat_patch_all("setupapi.dll", "CM_Get_Child",       (void*)Wrap_CM_Get_Child);
    ultra_iat_patch_all("setupapi.dll", "CM_Get_Sibling",     (void*)Wrap_CM_Get_Sibling);
    ultra_iat_patch_all("setupapi.dll", "CM_Get_Parent",      (void*)Wrap_CM_Get_Parent);
    ultra_iat_patch_all("setupapi.dll", "CM_Get_DevNode_Registry_PropertyW",
                        (void*)Wrap_CM_Get_DevNode_Registry_PropertyW);

    g_cm_probe = 1;
    dlog("[CM] PnP device-tree probe ARMED (pass-through + log only)");
}


void ultra_namco_dongle_install(void) {
    /* The PnP device-tree probe is independent of the dongle gate: the
     * device the game hunts for down that path is the STR PCB, not the
     * dongle, so it must arm even when the dongle bypass is off. */
    ultra_namco_cm_probe_install();
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
