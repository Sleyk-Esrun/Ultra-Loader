/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  sega_keychip_europar.c -- Sega Europa-R board's OWN keychip presence shim,
 *                             inpout32.dll neutralizer, and dialog auto-dismiss.
 * =============================================================================
 *
 *  Independent copy for modules/sega_europar/src/hook/ only. Adapted from the
 *  PATTERNS in modules/native_x86/src/hook/sega_keychip.c (mxparallel presence
 *  shim) and dongle_bypass.c (MessageBox auto-dismiss), both read as reference
 *  only -- no #include, no link, no runtime dependency on those files. This
 *  file also implements the inpout32.dll bypass that sega_europar_main.c has
 *  documented and env-gated (ULTRA_INPOUT32_BYPASS) since before this board
 *  had any hook DLL at all -- a genuinely NEW capability, not present anywhere
 *  in the reference tree either (grep confirms no inpout32 handling exists in
 *  native_x86/src/hook/); this is this board's own original implementation of
 *  the contract sega_europar_main.c's header already describes.
 *
 *  Three env-gated, independent pieces, all wired through
 *  jvs_serial_hook_europar.c's kernel32 IAT patcher:
 *
 *  (1) ULTRA_SEGA_KEYCHIP=1 -- Europa-R reuses SEGA_BOARD_RINGEDGE for its
 *      keychip detail (board_profile.c: "Reuses SEGA_BOARD_RINGEDGE for the
 *      keychip/TrueCrypt detail"), i.e. a PIC parallel dongle reached via
 *      CreateFile("\\.\mxparallel"). When enabled and no physical keychip
 *      answers that open, we hand back a synthetic handle so the *presence*
 *      gate passes. This does NOT and CANNOT derive a still-encrypted
 *      retail container's game key (that secret lives in keychip silicon and
 *      never appears in any dump) -- it only lets an already-unprotected /
 *      operator-decrypted image get past the "is a keychip attached" check.
 *
 *  (2) ULTRA_INPOUT32_BYPASS=1 -- Europa-R titles ship inpout32.dll for
 *      parallel-port I/O. Its DllMain unconditionally CreateFile()s a kernel
 *      driver device that does not exist on a user machine without the
 *      inpout32 driver installed, which crashes during DLL_PROCESS_ATTACH --
 *      and because that happens inside WOW64 init, the whole process hangs
 *      with only ntdll64+wow64.dll mapped (exactly the symptom
 *      sega_europar_main.c's header documents). Fix: intercept the game's
 *      LoadLibraryA/W("inpout32.dll") and hand back a fake, harmless in-
 *      process module handle instead of ever touching the real DLL, then
 *      answer GetProcAddress for that handle with local no-op stubs for the
 *      well-known inpout32 export surface. The real keychip PRESENCE answer
 *      still comes from (1)'s mxparallel shim; these stubs report "no legacy
 *      parallel port hardware" rather than fabricating port I/O.
 *
 *  (3) ULTRA_DISMISS_DIALOGS=1 -- auto-answer MessageBoxA/W so a blocking
 *      hardware-error popup (dongle/keychip/parallel-port complaint) does not
 *      halt an otherwise-running game; every dialog is logged first so a test
 *      run shows exactly what the game complained about.
 *
 *  Clean-room: Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* owned by jvs_serial_hook_europar.c */
extern void ultra_iat_patch_all_europar(const char* impdll, const char* impfn, void* newfn);
extern void ultra_europar_log(const char* fmt, ...);
#define klog ultra_europar_log

/* =============================================================================
 * (1) mxparallel keychip presence shim
 * ============================================================================= */
static int g_keychip_enabled = 0;
static char g_region = 'A';

#define SEGA_KEYCHIP_EUROPAR_SENTINEL ((HANDLE)(ULONG_PTR)0x5EE60001)

/* jvs_serial_hook_europar.c's Hook_CreateFileW already converts the wide name
 * to narrow (ACP) before calling europar_keychip_try_createfile() below, so
 * only the narrow-name matcher is needed here. */
static int is_mxparallel_a(LPCSTR n) {
    return n && (strstr(n, "mxparallel") || strstr(n, "MXPARALLEL"));
}

/* Called from jvs_serial_hook_europar.c's Hook_CreateFileA/W before the
 * generic COM/passthrough path. Returns 1 (and fills *out) if this call is
 * the keychip device and we're answering it; 0 to fall through. */
int europar_keychip_try_createfile(const char* name, HANDLE* out) {
    if (!g_keychip_enabled || !is_mxparallel_a(name)) return 0;
    /* Give the real device first refusal (a genuine operator keychip always
     * wins over the synthetic presence answer). */
    HANDLE real = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (real != INVALID_HANDLE_VALUE) { *out = real; return 1; }
    klog("keychip(europar): synthesizing mxparallel presence (no physical keychip)");
    *out = SEGA_KEYCHIP_EUROPAR_SENTINEL;
    return 1;
}
/* wide-name variant funnels through the same narrow-name check (jvs_serial_hook_europar.c
 * already converts CreateFileW's name to narrow before calling this). */

/* =============================================================================
 * (2) inpout32.dll neutralizer
 * ============================================================================= */
static int g_inpout32_bypass = 0;
static void* g_inpout32_page = NULL;   /* real, harmless backing memory for our fake HMODULE */

HMODULE europar_inpout32_sentinel(void) {
    if (!g_inpout32_page)
        g_inpout32_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return (HMODULE)g_inpout32_page;
}

/* Legacy inpout32 export surface. We report "no legacy parallel port hardware"
 * rather than fabricate I/O -- the real keychip PRESENCE answer comes from the
 * mxparallel shim above; these exist purely so the game's own port-probe code
 * path executes safely instead of dereferencing a driver handle that was
 * never opened. */
static short  __stdcall stub_Inp32(short PortAddress)                    { (void)PortAddress; return 0; }
static void   __stdcall stub_Out32(short PortAddress, short Data)        { (void)PortAddress; (void)Data; }
static BOOL   __stdcall stub_IsInpOutDriverOpen(void)                     { return FALSE; }
static BOOL   __stdcall stub_IsXP64Bit(void)                              { return FALSE; }
static unsigned char __stdcall stub_DlPortReadPortUchar(unsigned short p) { (void)p; return 0xFF; }
static void   __stdcall stub_DlPortWritePortUchar(unsigned short p, unsigned char d) { (void)p; (void)d; }
static unsigned short __stdcall stub_DlPortReadPortUshort(unsigned short p) { (void)p; return 0xFFFF; }
static void   __stdcall stub_DlPortWritePortUshort(unsigned short p, unsigned short d) { (void)p; (void)d; }
static unsigned long __stdcall stub_DlPortReadPortUlong(unsigned short p) { (void)p; return 0xFFFFFFFFu; }
static void   __stdcall stub_DlPortWritePortUlong(unsigned short p, unsigned long d) { (void)p; (void)d; }
static BOOL   __stdcall stub_GetPhysLong(PVOID a, PDWORD b)               { (void)a; if (b) *b = 0; return FALSE; }
static BOOL   __stdcall stub_SetPhysLong(PVOID a, DWORD b)                { (void)a; (void)b; return FALSE; }

/* Called from jvs_serial_hook_europar.c's Hook_LoadLibraryA/W. Returning ""
 * (empty, non-NULL) tells the caller "don't touch the real file, hand back
 * our synthesized module instead"; returning NULL means "no redirect". */
const char* europar_loadlibrary_redirect(const char* name) {
    if (!g_inpout32_bypass || !name) return NULL;
    if (_stricmp(name, "inpout32.dll") == 0 || _stricmp(name, "inpoutx64.dll") == 0)
        return "";
    return NULL;
}

int europar_getprocaddress_override(HMODULE mod, const char* name, FARPROC* out) {
    if (!g_inpout32_bypass || mod != europar_inpout32_sentinel() || !name || !out) return 0;
    struct { const char* n; FARPROC f; } map[] = {
        {"Inp32",               (FARPROC)stub_Inp32},
        {"Out32",               (FARPROC)stub_Out32},
        {"IsInpOutDriverOpen",  (FARPROC)stub_IsInpOutDriverOpen},
        {"IsXP64Bit",           (FARPROC)stub_IsXP64Bit},
        {"DlPortReadPortUchar", (FARPROC)stub_DlPortReadPortUchar},
        {"DlPortWritePortUchar",(FARPROC)stub_DlPortWritePortUchar},
        {"DlPortReadPortUshort",(FARPROC)stub_DlPortReadPortUshort},
        {"DlPortWritePortUshort",(FARPROC)stub_DlPortWritePortUshort},
        {"DlPortReadPortUlong", (FARPROC)stub_DlPortReadPortUlong},
        {"DlPortWritePortUlong",(FARPROC)stub_DlPortWritePortUlong},
        {"GetPhysLong",         (FARPROC)stub_GetPhysLong},
        {"SetPhysLong",         (FARPROC)stub_SetPhysLong},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
        if (strcmp(name, map[i].n) == 0) { *out = map[i].f; return 1; }
    /* Unknown export on our fake module: hand back NULL rather than crash a
     * real resolver against a non-PE "module". */
    *out = NULL;
    return 1;
}

/* =============================================================================
 * (3) MessageBox auto-dismiss
 * ============================================================================= */
typedef int (WINAPI *pMessageBoxA)(HWND,LPCSTR,LPCSTR,UINT);
typedef int (WINAPI *pMessageBoxW)(HWND,LPCWSTR,LPCWSTR,UINT);
static pMessageBoxA Real_MessageBoxA_Europar;
static pMessageBoxW Real_MessageBoxW_Europar;
static int g_dismiss_dialogs = 0;

static int default_button(UINT type) {
    switch (type & 0xF) {
        case MB_OK:                return IDOK;
        case MB_OKCANCEL:          return IDCANCEL;
        case MB_YESNO:             return IDNO;
        case MB_YESNOCANCEL:       return IDNO;
        case MB_RETRYCANCEL:       return IDCANCEL;
        case MB_ABORTRETRYIGNORE:  return IDIGNORE;
        case MB_CANCELTRYCONTINUE: return IDCONTINUE;
        default:                   return IDOK;
    }
}
static int WINAPI Hook_MessageBoxA_Europar(HWND w, LPCSTR t, LPCSTR c, UINT type) {
    klog("MESSAGEBOX(europar) title=\"%s\" text=\"%s\"", c ? c : "", t ? t : "");
    if (g_dismiss_dialogs) return default_button(type);
    return Real_MessageBoxA_Europar(w, t, c, type);
}
static int WINAPI Hook_MessageBoxW_Europar(HWND w, LPCWSTR t, LPCWSTR c, UINT type) {
    char tt[256] = {0}, cc[256] = {0};
    if (t) WideCharToMultiByte(CP_ACP, 0, t, -1, tt, sizeof(tt)-1, NULL, NULL);
    if (c) WideCharToMultiByte(CP_ACP, 0, c, -1, cc, sizeof(cc)-1, NULL, NULL);
    klog("MESSAGEBOX(europar) title=\"%s\" text=\"%s\"", cc, tt);
    if (g_dismiss_dialogs) return default_button(type);
    return Real_MessageBoxW_Europar(w, t, c, type);
}

/* =============================================================================
 * Install. All three pieces are independently env-gated.
 * ============================================================================= */
void ultra_sega_keychip_install_europar(void) {
    const char* kc = getenv("ULTRA_SEGA_KEYCHIP");
    g_keychip_enabled = kc && kc[0] == '1';
    if (g_keychip_enabled) {
        const char* r = getenv("ULTRA_SEGA_REGION");
        if (r && r[0]) g_region = r[0];
        klog("keychip(europar): mxparallel presence shim armed (region=%c). NOTE: cannot "
             "decrypt a retail container; needs an unprotected/operator-decrypted image.", g_region);
    }

    const char* ib = getenv("ULTRA_INPOUT32_BYPASS");
    g_inpout32_bypass = ib && ib[0] == '1';
    if (g_inpout32_bypass)
        klog("keychip(europar): inpout32.dll neutralized (fake module + no-op port stubs)");

    const char* dd = getenv("ULTRA_DISMISS_DIALOGS");
    g_dismiss_dialogs = dd && dd[0] == '1';
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        Real_MessageBoxA_Europar = (pMessageBoxA)(void*)GetProcAddress(u32, "MessageBoxA");
        Real_MessageBoxW_Europar = (pMessageBoxW)(void*)GetProcAddress(u32, "MessageBoxW");
        if (Real_MessageBoxA_Europar) ultra_iat_patch_all_europar("user32.dll", "MessageBoxA", (void*)Hook_MessageBoxA_Europar);
        if (Real_MessageBoxW_Europar) ultra_iat_patch_all_europar("user32.dll", "MessageBoxW", (void*)Hook_MessageBoxW_Europar);
    }
    if (g_dismiss_dialogs)
        klog("keychip(europar): dialog auto-dismiss armed");
}

#endif /* _WIN32 */
