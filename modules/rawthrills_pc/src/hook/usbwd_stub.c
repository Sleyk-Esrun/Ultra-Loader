/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  usbwd_stub.c -- Raw Thrills USBWD (USB watchdog) presence stub.
 * =============================================================================
 *
 *  ENGINE FACTS (from static analysis of sdaemon.exe -- TFAFSB dump):
 *    The USBWD board is a second Raw Thrills HID device that kicks the cabinet
 *    watchdog. If the game does not receive a periodic "board present" reply
 *    from USBWD (typical timeout: a few seconds), it prints
 *      "WAITING FOR USBWD BOARD (%d)"
 *    and eventually enters a fault loop that keeps the game off the screen.
 *
 *    Distinctive strings: "USBWD Version: %s %c%02d",
 *    "USBWD VERSION: %s %c%02d, WDOGGED=%s", "WAITING FOR USBWD BOARD (%d)".
 *
 *    Like JAMMAUSB, USBWD is reached via CreateFileW on a HID interface path
 *    followed by WriteFile / ReadFile (or HID feature-report writes). We
 *    disambiguate it from JAMMAUSB by handing back a DIFFERENT sentinel handle
 *    when the JAMMA sentinel is already handed out for this path -- but in
 *    practice we do not need to: the JAMMAUSB stub owns HID enum, and every
 *    HID CreateFileW that JAMMAUSB does not steal (returns sentinel A) rolls
 *    into the same neutral-report path. The USBWD firmware personality is
 *    additionally served by responding to two watchdog IOCTLs (kick + version)
 *    that some titles use in place of the HID feature-report path.
 *
 *  WHAT THIS STUB DOES:
 *    - Hooks DeviceIoControl to accept the USBWD watchdog IOCTLs the RawApp
 *      firmware exposes (kick + get-version). Returns success + a synthetic
 *      version blob so the "WAITING FOR USBWD" loop clears.
 *    - Is otherwise transparent (falls through to the real DeviceIoControl for
 *      handles it does not recognize).
 *    - Does NOT re-hook CreateFileW/ReadFile/WriteFile -- the JAMMAUSB stub
 *      already handles the shared HID transport. USBWD's HID reports come out
 *      neutral there, which is exactly what the "board present, no fault"
 *      state looks like.
 *
 *  IOCTL codes (well-known RawApp/USBWD ioctl set -- CTL_CODE(FILE_DEVICE_UNKNOWN,
 *  0x800+, METHOD_BUFFERED, FILE_ANY_ACCESS)):
 *      0x80002000  USBWD_KICK        -- swallow, return success
 *      0x80002004  USBWD_GET_VERSION -- fill 32-byte version blob
 *      0x80002008  USBWD_GET_STATUS  -- return "WDOGGED=OK" style byte
 *      0x8000200C  USBWD_SET_TIMEOUT -- swallow
 *  Titles that pre-date the IOCTL personality use HID feature reports instead
 *  (already covered by the JAMMAUSB stub's HidD_SetFeature accept-all).
 *
 *  Env gate: ULTRA_USBWD=1. Default OFF.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "rawthrills_hook.h"

typedef BOOL (WINAPI *pDeviceIoControl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,
                                        LPDWORD,LPOVERLAPPED);
static pDeviceIoControl Real_DeviceIoControl = NULL;

static int g_enabled = 0;

/* Version blob shape: "USBWD Version: Ultra 100" plus zero pad. The exact
 * bytes the game printf-formats are consumed by a %s -- any nul-terminated
 * ascii string is legal. We use the loader's own signature. */
static const char k_usbwd_version[] = "Ultra Raw Thrills USBWD 1.00";

static BOOL WINAPI Hook_DeviceIoControl(HANDLE h, DWORD ioc,
        LPVOID in_buf, DWORD in_n,
        LPVOID out_buf, DWORD out_n,
        LPDWORD got, LPOVERLAPPED ov) {
    (void)in_buf; (void)in_n;
    /* We do not filter by handle -- the game IOCTLs targeted at USBWD are
     * distinctive (0x8000_20xx) and no other subsystem uses that range in
     * sdaemon. Falling through to the real DeviceIoControl for everything
     * else keeps the stub byte-safe for non-USBWD handles. */
    switch (ioc) {
    case 0x80002000: {  /* USBWD_KICK */
        if (got) *got = 0;
        return TRUE;
    }
    case 0x80002004: {  /* USBWD_GET_VERSION */
        if (out_buf && out_n) {
            size_t n = sizeof(k_usbwd_version);
            if (n > out_n) n = out_n;
            memcpy(out_buf, k_usbwd_version, n);
        }
        if (got) *got = (DWORD)sizeof(k_usbwd_version);
        return TRUE;
    }
    case 0x80002008: {  /* USBWD_GET_STATUS */
        if (out_buf && out_n) {
            memset(out_buf, 0, out_n);
            ((uint8_t*)out_buf)[0] = 0x01;   /* "WDOGGED=OK" flag */
        }
        if (got) *got = 1;
        return TRUE;
    }
    case 0x8000200C: {  /* USBWD_SET_TIMEOUT */
        if (got) *got = 0;
        return TRUE;
    }
    default: break;
    }
    return Real_DeviceIoControl
        ? Real_DeviceIoControl(h, ioc, in_buf, in_n, out_buf, out_n, got, ov)
        : FALSE;
}

void ul_rt_usbwd_install(void) {
    if (!ul_rt_env_on("ULTRA_USBWD")) {
        ul_rt_log("usbwd: skipped (ULTRA_USBWD!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) Real_DeviceIoControl =
        (pDeviceIoControl)(void*)GetProcAddress(k32, "DeviceIoControl");

    ul_rt_iat_patch_all("kernel32.dll", "DeviceIoControl",
                        (void*)Hook_DeviceIoControl);
    ul_rt_log("usbwd: armed (kick+ver+status+timeout swallowed)\n");
}

#endif /* _WIN32 */
