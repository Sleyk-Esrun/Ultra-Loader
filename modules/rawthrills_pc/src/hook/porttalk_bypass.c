/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  porttalk_bypass.c -- Raw Thrills PortTalk ring-0 driver bypass.
 * =============================================================================
 *
 *  ENGINE FACTS (from static analysis of sdaemon.exe):
 *    Raw Thrills' RawApp uses the legacy WinXP-Embedded "PortTalk" kernel
 *    driver to gain ring-0 access to the parallel/ISA port so it can drive
 *    wheel force-feedback via direct outb/inb. On boot sdaemon does:
 *        CreateFile("\\.\PortTalk", ...)   -- open the driver
 *        if fail:
 *            copy PortTalk.sys -> %SystemRoot%\System32\Drivers\
 *            OpenSCManager + CreateService("PortTalk") + StartService
 *            retry the open
 *        then per-frame:
 *            DeviceIoControl(handle, IOCTL_IOPM_GRANT_IO_PRIVILEGE, ...)
 *            DeviceIoControl(handle, IOCTL_PORTTALK_OUTPUT_BYTE, port, val)
 *
 *    Distinctive strings: "\\.\PortTalk", "PortTalk.sys",
 *    "\Drivers\PortTalk.sys", "System32\Drivers\PortTalk.sys",
 *    "PortTalk: Couldn't access PortTalk Driver...",
 *    "PortTalk: The PortTalk driver does not exist. Installing driver.",
 *    "PortTalk: Driver successfully installed.",
 *    "Error occured during outportb while talking to PortTalk driver %d",
 *    "Wheel_Init: Could not open PortTalk".
 *
 *    PortTalk.sys is NOT signed for modern Windows (WinXP-era) and will not
 *    load on Win7+ without disabling driver-signature enforcement. The stub
 *    keeps sdaemon happy without ever loading the real driver.
 *
 *  WHAT THIS STUB DOES:
 *    1. CreateFileA / CreateFileW on "\\.\PortTalk" -> return PORTTALK_SENTINEL
 *       (managed handle we own).
 *    2. DeviceIoControl on that sentinel -> swallow the two well-known ioctls
 *       (IOPM_GRANT_IO_PRIVILEGE + PORTTALK_OUTPUT_BYTE) and return success.
 *       The wheel command port+value written by IOCTL_PORTTALK_OUTPUT_BYTE is
 *       forwarded (in the future) to the loader's OmniWheel service; for now
 *       we log it so its shape is verifiable.
 *    3. OpenSCManagerA/W + CreateServiceA/W + StartServiceA/W for the service
 *       name "PortTalk" -> pretend-success (fake SC_HANDLE sentinels) so the
 *       auto-install sequence does not UAC-prompt / fail.
 *    4. CloseServiceHandle on our sentinels -> success.
 *
 *  Env gate: ULTRA_PORTTALK_BYPASS=1. Default OFF.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <winsvc.h>
#include <stdint.h>
#include <string.h>

#include "rawthrills_hook.h"

#define PORTTALK_SENTINEL   ((HANDLE)(ULONG_PTR)0x50727401)   /* 'Prt\x01' */
#define PORTTALK_SCM_H      ((SC_HANDLE)(ULONG_PTR)0x50727402)
#define PORTTALK_SVC_H      ((SC_HANDLE)(ULONG_PTR)0x50727403)

/* Well-known PortTalk IOCTL codes (from the driver's public header
 *   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8xx, METHOD_BUFFERED, FILE_ANY_ACCESS) ). */
#define IOCTL_IOPM_GRANT_IO_PRIVILEGE   0x9C406038u
#define IOCTL_PORTTALK_OUTPUT_BYTE      0x9C402004u
#define IOCTL_PORTTALK_OUTPUT_WORD      0x9C402008u
#define IOCTL_PORTTALK_INPUT_BYTE       0x9C402080u

typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,
                                      DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,
                                      DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pDeviceIoControl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,
                                          LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pCloseHandle)(HANDLE);

typedef SC_HANDLE (WINAPI *pOpenSCManagerA)(LPCSTR,LPCSTR,DWORD);
typedef SC_HANDLE (WINAPI *pOpenSCManagerW)(LPCWSTR,LPCWSTR,DWORD);
typedef SC_HANDLE (WINAPI *pCreateServiceA)(SC_HANDLE,LPCSTR,LPCSTR,DWORD,DWORD,
                       DWORD,DWORD,LPCSTR,LPCSTR,LPDWORD,LPCSTR,LPCSTR,LPCSTR);
typedef SC_HANDLE (WINAPI *pCreateServiceW)(SC_HANDLE,LPCWSTR,LPCWSTR,DWORD,DWORD,
                       DWORD,DWORD,LPCWSTR,LPCWSTR,LPDWORD,LPCWSTR,LPCWSTR,LPCWSTR);
typedef SC_HANDLE (WINAPI *pOpenServiceA)(SC_HANDLE,LPCSTR,DWORD);
typedef SC_HANDLE (WINAPI *pOpenServiceW)(SC_HANDLE,LPCWSTR,DWORD);
typedef BOOL      (WINAPI *pStartServiceA)(SC_HANDLE,DWORD,LPCSTR*);
typedef BOOL      (WINAPI *pStartServiceW)(SC_HANDLE,DWORD,LPCWSTR*);
typedef BOOL      (WINAPI *pCloseServiceHandle)(SC_HANDLE);

static pCreateFileA        Real_CreateFileA        = NULL;
static pCreateFileW        Real_CreateFileW        = NULL;
static pDeviceIoControl    Real_DeviceIoControl    = NULL;
static pCloseHandle        Real_CloseHandle        = NULL;
static pOpenSCManagerA     Real_OpenSCManagerA     = NULL;
static pOpenSCManagerW     Real_OpenSCManagerW     = NULL;
static pCreateServiceA     Real_CreateServiceA     = NULL;
static pCreateServiceW     Real_CreateServiceW     = NULL;
static pOpenServiceA       Real_OpenServiceA       = NULL;
static pOpenServiceW       Real_OpenServiceW       = NULL;
static pStartServiceA      Real_StartServiceA      = NULL;
static pStartServiceW      Real_StartServiceW      = NULL;
static pCloseServiceHandle Real_CloseServiceHandle = NULL;

static int g_enabled = 0;

/* Optional per-frame wheel-value logging so the OmniWheel bridge can pin the
 * game's outb sequence. Off by default (a running game emits ~60 outb/s). */
static int g_wheel_log = 0;

static int is_porttalk_path_a(LPCSTR n) {
    if (!n) return 0;
    /* case-insensitive substring "porttalk" */
    for (const char* p = n; *p; ++p) {
        if ((p[0]|0x20)=='p' && (p[1]|0x20)=='o' && (p[2]|0x20)=='r'
         && (p[3]|0x20)=='t' && (p[4]|0x20)=='t' && (p[5]|0x20)=='a'
         && (p[6]|0x20)=='l' && (p[7]|0x20)=='k') return 1;
    }
    return 0;
}
static int is_porttalk_path_w(LPCWSTR n) {
    if (!n) return 0;
    for (const wchar_t* p = n; *p; ++p) {
        wchar_t c0=p[0]|0x20, c1=p[1]|0x20, c2=p[2]|0x20, c3=p[3]|0x20,
                c4=p[4]|0x20, c5=p[5]|0x20, c6=p[6]|0x20, c7=p[7]|0x20;
        if (c0==L'p' && c1==L'o' && c2==L'r' && c3==L't' && c4==L't'
         && c5==L'a' && c6==L'l' && c7==L'k') return 1;
    }
    return 0;
}
static int is_porttalk_svc_a(LPCSTR n) { return n && is_porttalk_path_a(n); }
static int is_porttalk_svc_w(LPCWSTR n) { return n && is_porttalk_path_w(n); }

static HANDLE WINAPI Hook_CreateFileA(LPCSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && is_porttalk_path_a(n)) {
        ul_rt_log("porttalk: intercepted CreateFileA(%s) -> sentinel\n", n);
        return PORTTALK_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(n, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR n, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && is_porttalk_path_w(n)) {
        ul_rt_log("porttalk: intercepted CreateFileW -> sentinel\n");
        return PORTTALK_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(n, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE h, DWORD ioc,
        LPVOID in_buf, DWORD in_n,
        LPVOID out_buf, DWORD out_n,
        LPDWORD got, LPOVERLAPPED ov) {
    if (h == PORTTALK_SENTINEL) {
        /* Wheel-command port+value forwarding: the OUTPUT_BYTE ioctl input
         * buffer is 4 bytes -- 2 bytes port + 2 bytes value. We log if the
         * wheel log is enabled; in the future this is where the OmniWheel
         * service gets the byte. */
        if (ioc == IOCTL_PORTTALK_OUTPUT_BYTE && g_wheel_log && in_buf && in_n >= 4) {
            uint16_t port = ((uint16_t*)in_buf)[0];
            uint16_t val  = ((uint16_t*)in_buf)[1];
            ul_rt_log("porttalk: outb port=0x%04X val=0x%02X\n", port, val & 0xFF);
        }
        if (ioc == IOCTL_PORTTALK_INPUT_BYTE && out_buf && out_n >= 1) {
            /* Neutral input: 0xFF = "idle high" on a parallel status port. */
            ((uint8_t*)out_buf)[0] = 0xFF;
            if (got) *got = 1;
            return TRUE;
        }
        if (got) *got = 0;
        return TRUE;
    }
    return Real_DeviceIoControl
        ? Real_DeviceIoControl(h, ioc, in_buf, in_n, out_buf, out_n, got, ov)
        : FALSE;
}
static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (h == PORTTALK_SENTINEL) return TRUE;
    return Real_CloseHandle ? Real_CloseHandle(h) : FALSE;
}

/* --- SCM install intercept -- pretend PortTalk service already exists ------ */
static SC_HANDLE WINAPI Hook_OpenSCManagerA(LPCSTR m, LPCSTR d, DWORD a) {
    if (g_enabled) return PORTTALK_SCM_H;
    return Real_OpenSCManagerA ? Real_OpenSCManagerA(m, d, a) : NULL;
}
static SC_HANDLE WINAPI Hook_OpenSCManagerW(LPCWSTR m, LPCWSTR d, DWORD a) {
    if (g_enabled) return PORTTALK_SCM_H;
    return Real_OpenSCManagerW ? Real_OpenSCManagerW(m, d, a) : NULL;
}
static SC_HANDLE WINAPI Hook_CreateServiceA(SC_HANDLE scm, LPCSTR svc,
        LPCSTR disp, DWORD access, DWORD type, DWORD start, DWORD err,
        LPCSTR bin, LPCSTR grp, LPDWORD tag, LPCSTR dep, LPCSTR user, LPCSTR pw) {
    if (scm == PORTTALK_SCM_H || is_porttalk_svc_a(svc)) {
        ul_rt_log("porttalk: CreateServiceA(%s) -> fake handle\n", svc ? svc : "?");
        return PORTTALK_SVC_H;
    }
    return Real_CreateServiceA
        ? Real_CreateServiceA(scm, svc, disp, access, type, start, err, bin, grp, tag, dep, user, pw)
        : NULL;
}
static SC_HANDLE WINAPI Hook_CreateServiceW(SC_HANDLE scm, LPCWSTR svc,
        LPCWSTR disp, DWORD access, DWORD type, DWORD start, DWORD err,
        LPCWSTR bin, LPCWSTR grp, LPDWORD tag, LPCWSTR dep, LPCWSTR user, LPCWSTR pw) {
    if (scm == PORTTALK_SCM_H || is_porttalk_svc_w(svc)) {
        ul_rt_log("porttalk: CreateServiceW -> fake handle\n");
        return PORTTALK_SVC_H;
    }
    return Real_CreateServiceW
        ? Real_CreateServiceW(scm, svc, disp, access, type, start, err, bin, grp, tag, dep, user, pw)
        : NULL;
}
static SC_HANDLE WINAPI Hook_OpenServiceA(SC_HANDLE scm, LPCSTR svc, DWORD a) {
    if (scm == PORTTALK_SCM_H || is_porttalk_svc_a(svc)) return PORTTALK_SVC_H;
    return Real_OpenServiceA ? Real_OpenServiceA(scm, svc, a) : NULL;
}
static SC_HANDLE WINAPI Hook_OpenServiceW(SC_HANDLE scm, LPCWSTR svc, DWORD a) {
    if (scm == PORTTALK_SCM_H || is_porttalk_svc_w(svc)) return PORTTALK_SVC_H;
    return Real_OpenServiceW ? Real_OpenServiceW(scm, svc, a) : NULL;
}
static BOOL WINAPI Hook_StartServiceA(SC_HANDLE svc, DWORD n, LPCSTR* v) {
    if (svc == PORTTALK_SVC_H) return TRUE;
    return Real_StartServiceA ? Real_StartServiceA(svc, n, v) : FALSE;
}
static BOOL WINAPI Hook_StartServiceW(SC_HANDLE svc, DWORD n, LPCWSTR* v) {
    if (svc == PORTTALK_SVC_H) return TRUE;
    return Real_StartServiceW ? Real_StartServiceW(svc, n, v) : FALSE;
}
static BOOL WINAPI Hook_CloseServiceHandle(SC_HANDLE h) {
    if (h == PORTTALK_SCM_H || h == PORTTALK_SVC_H) return TRUE;
    return Real_CloseServiceHandle ? Real_CloseServiceHandle(h) : FALSE;
}

void ul_rt_porttalk_install(void) {
    if (!ul_rt_env_on("ULTRA_PORTTALK_BYPASS")) {
        ul_rt_log("porttalk: skipped (ULTRA_PORTTALK_BYPASS!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;
    g_wheel_log = ul_rt_env_on("ULTRA_PORTTALK_WHEEL_LOG");

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE adv = LoadLibraryA("advapi32.dll");
    if (k32) {
        Real_CreateFileA     = (pCreateFileA)    (void*)GetProcAddress(k32, "CreateFileA");
        Real_CreateFileW     = (pCreateFileW)    (void*)GetProcAddress(k32, "CreateFileW");
        Real_DeviceIoControl = (pDeviceIoControl)(void*)GetProcAddress(k32, "DeviceIoControl");
        Real_CloseHandle     = (pCloseHandle)    (void*)GetProcAddress(k32, "CloseHandle");
    }
    if (adv) {
        Real_OpenSCManagerA     = (pOpenSCManagerA)    (void*)GetProcAddress(adv, "OpenSCManagerA");
        Real_OpenSCManagerW     = (pOpenSCManagerW)    (void*)GetProcAddress(adv, "OpenSCManagerW");
        Real_CreateServiceA     = (pCreateServiceA)    (void*)GetProcAddress(adv, "CreateServiceA");
        Real_CreateServiceW     = (pCreateServiceW)    (void*)GetProcAddress(adv, "CreateServiceW");
        Real_OpenServiceA       = (pOpenServiceA)      (void*)GetProcAddress(adv, "OpenServiceA");
        Real_OpenServiceW       = (pOpenServiceW)      (void*)GetProcAddress(adv, "OpenServiceW");
        Real_StartServiceA      = (pStartServiceA)     (void*)GetProcAddress(adv, "StartServiceA");
        Real_StartServiceW      = (pStartServiceW)     (void*)GetProcAddress(adv, "StartServiceW");
        Real_CloseServiceHandle = (pCloseServiceHandle)(void*)GetProcAddress(adv, "CloseServiceHandle");
    }

    ul_rt_iat_patch_all("kernel32.dll", "CreateFileA",     (void*)Hook_CreateFileA);
    ul_rt_iat_patch_all("kernel32.dll", "CreateFileW",     (void*)Hook_CreateFileW);
    ul_rt_iat_patch_all("kernel32.dll", "DeviceIoControl", (void*)Hook_DeviceIoControl);
    ul_rt_iat_patch_all("kernel32.dll", "CloseHandle",     (void*)Hook_CloseHandle);
    ul_rt_iat_patch_all("advapi32.dll", "OpenSCManagerA",     (void*)Hook_OpenSCManagerA);
    ul_rt_iat_patch_all("advapi32.dll", "OpenSCManagerW",     (void*)Hook_OpenSCManagerW);
    ul_rt_iat_patch_all("advapi32.dll", "CreateServiceA",     (void*)Hook_CreateServiceA);
    ul_rt_iat_patch_all("advapi32.dll", "CreateServiceW",     (void*)Hook_CreateServiceW);
    ul_rt_iat_patch_all("advapi32.dll", "OpenServiceA",       (void*)Hook_OpenServiceA);
    ul_rt_iat_patch_all("advapi32.dll", "OpenServiceW",       (void*)Hook_OpenServiceW);
    ul_rt_iat_patch_all("advapi32.dll", "StartServiceA",      (void*)Hook_StartServiceA);
    ul_rt_iat_patch_all("advapi32.dll", "StartServiceW",      (void*)Hook_StartServiceW);
    ul_rt_iat_patch_all("advapi32.dll", "CloseServiceHandle", (void*)Hook_CloseServiceHandle);
    ul_rt_log("porttalk: armed (SCM install bypassed, ioctls swallowed, wheel_log=%d)\n",
              g_wheel_log);
}

#endif /* _WIN32 */
