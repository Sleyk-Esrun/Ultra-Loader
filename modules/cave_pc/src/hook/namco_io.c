/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  namco_io.c -- Namco System ES JVS-over-USB I/O + amauthd LOCAL presence
 *                (clean-room, env-gated, default OFF, byte-safe).
 * =============================================================================
 *
 *  Modern Namco PC boards (System ES1+/ES3/ES4/BNA1, and the older N2) do NOT put
 *  their cabinet I/O on a plain serial COM port the way Type X does. The game's
 *  runtime ("ESIF" / the jvs.dll-class layer) opens a USB JVS-BRIDGE and talks to
 *  it. Two physical shapes are seen on the public board images:
 *
 *    (A) USB-CDC virtual COM -- the bridge enumerates as a virtual \\.\COMx and the
 *        game opens it like any serial port. That path is ALREADY covered by
 *        jvs_serial_hook.c (it claims the COM handle and bridges JVS); the only
 *        thing the profile must do for case (A) is point ULTRA_JVS_COM at the right
 *        port. No new transport code is needed -- this file documents it and does
 *        not fight the serial hook for those handles.
 *
 *    (B) raw WinUSB / vendor pipe -- the bridge is a WinUSB device opened by its
 *        device-interface path (\\?\usb#vid_xxxx&pid_xxxx#...). The game writes a
 *        JVS request to an OUT pipe / device handle and reads the JVS response from
 *        an IN pipe / the same handle. This file owns case (B): it intercepts the
 *        device OPEN by VID/PID, hands back a synthetic handle, and services the
 *        ReadFile/WriteFile (and WinUsb_Read/WritePipe) traffic on it.
 *
 *  THE KEY FACT that keeps this clean-room and small: the framing INSIDE the USB
 *  wrapper is STANDARD JVS (E0 sync, FD/DF escapes, node SET_ADDRESS, READ_SWITCH,
 *  READ_COIN, READ_ANALOG ...). i.e. the PAYLOAD is exactly what jvs_core.c already
 *  speaks. So this file is the IO4 transport shell (sega_io4.c) re-pointed at a
 *  Namco VID/PID, with the bytes bridged through jvs_host_bytes() (the same call
 *  jvs_serial_hook.c uses). It is purely an I/O RE-PACKING of the loader's existing
 *  ultra_jvs_input block + clean-room JVS node -- exactly like sega_io4.c /
 *  taito_fastio.c. It owns NO security/crypto.
 *
 *  PRESENCE: this file also answers the LOCAL amdaemon/amauthd presence handshake
 *  (an alias surface that mirrors namco_amdaemon.c's posture): "is the local auth
 *  daemon present, and is local/offline status OK". That is PRESENCE / AUTH-STATE
 *  ONLY. The ETHICAL LINE is hard-coded into the answer surface:
 *    - presence ping / local-status / version / region / play-count  -> answered.
 *    - ANYTHING that derives a key, decrypts content, or reaches a LIVE online
 *      service (ALL.Net / Namco billing / account / key material)     -> DEFER
 *      (return 0). Never answered, never impersonated.
 *  It ships NO keys, NO account data, NO game content, and is OFF unless armed.
 *
 *  HONEST GAP: the bridge's vendor VID/PID, the vendor bRequest for ident/reset,
 *  and the SENSE/addressing-done byte position are firmware-specific and not in the
 *  public protocol record. They are env-PINNED against a real dump (ULTRA_NAMCO_IO_*
 *  knobs), NEVER baked in -- the same "pinned to a dump" stance the Konami node map,
 *  the Fast-I/O offsets, and the amdaemon framing already use.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device API (CreateFile/
 *  ReadFile/WriteFile + WinUSB/SetupAPI surface), the open JVS spec (payload already
 *  in jvs_core.c), and the public description of the ES auth-daemon gate. No
 *  third-party loader source, profile, descriptor, or daemon binary was read.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core.h"

extern void ultra_d3d8_log(const char* fmt, ...);
#define nlog ultra_d3d8_log

/* from jvs_serial_hook.c / hook_dllmain.c -- the IAT patcher + the live JVS node
 * and the controller snapshot. We reuse the SAME node the serial hook drives, so a
 * board that mixes COM-JVS and USB-JVS stays consistent. */
extern void            ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
extern ultra_jvs_node* ultra_hook_node(void);
extern void            ultra_hook_pull_input(ultra_jvs_node* n);

static int g_enabled = 0;   /* ULTRA_NAMCO_IO=1  (set by a Namco ES/N2 profile)    */

/* ---------------------------------------------------------------------------
 *  Namco JVS-USB bridge identity (case B).
 *
 *  The VID/PID of the ESIF USB JVS bridge is firmware-specific. We DO NOT bake a
 *  guess: the device match is OFF until ULTRA_NAMCO_IO_VIDPID pins it (form
 *  "vid_xxxx&pid_yyyy", lower-case hex, as it appears in the Windows device path).
 *  A user with a real ES dump reads the VID/PID off the bridge and sets the knob.
 *  This keeps case (B) inert unless a real bridge id is supplied.
 * ------------------------------------------------------------------------- */
static char g_vidpid[64] = "";   /* e.g. "vid_0f0d&pid_00XX" (no default)          */

/* sentinel handle for the synthetic JVS-USB bridge endpoint. */
#define NAMCO_JVS_SENTINEL ((HANDLE)(ULONG_PTR)0x4A325531)  /* 'JV2U' */

/* ---------------------------------------------------------------------------
 *  Response queue for the synthetic bridge handle. A WriteFile of a JVS request
 *  produces (via jvs_core) a framed JVS reply which the next ReadFile drains --
 *  exactly the queue jvs_serial_hook.c keeps per COM slot, but for one USB handle.
 * ------------------------------------------------------------------------- */
static unsigned char g_resp[2048];
static int           g_resp_len = 0;
static int           g_resp_pos = 0;
static CRITICAL_SECTION g_lock;
static int              g_lock_init = 0;

/* Bridge a host->bridge JVS request through the shared JVS node, queueing the
 * framed reply for the next read. Returns the number of reply bytes queued.
 * Exposed (non-static) so the unit test can drive the bridge with no device. */
int ultra_namco_io_bridge(const unsigned char* req, int req_len) {
    if (!g_lock_init) return 0;
    EnterCriticalSection(&g_lock);
    ultra_jvs_node* node = ultra_hook_node();
    if (node) ultra_hook_pull_input(node);          /* latest controller state     */

    /* compact any already-drained reply bytes so the buffer doesn't creep. */
    if (g_resp_pos) {
        memmove(g_resp, g_resp + g_resp_pos, g_resp_len - g_resp_pos);
        g_resp_len -= g_resp_pos;
        g_resp_pos  = 0;
    }
    unsigned char tmp[2048];
    size_t got = node ? jvs_host_bytes(node, req, (size_t)(req_len < 0 ? 0 : req_len),
                                       tmp, sizeof tmp)
                      : 0;
    size_t room = sizeof g_resp - (size_t)g_resp_len;
    if (got > room) got = room;
    if (got) { memcpy(g_resp + g_resp_len, tmp, got); g_resp_len += (int)got; }
    LeaveCriticalSection(&g_lock);
    return (int)got;
}

/* Drain queued reply bytes into a caller buffer (the ReadFile body). Returns the
 * number of bytes copied. Exposed for the unit test. */
int ultra_namco_io_drain(unsigned char* buf, int cap) {
    if (!g_lock_init || !buf || cap <= 0) return 0;
    EnterCriticalSection(&g_lock);
    int avail = g_resp_len - g_resp_pos;
    int give  = (cap < avail) ? cap : avail;
    if (give > 0) { memcpy(buf, &g_resp[g_resp_pos], give); g_resp_pos += give; }
    LeaveCriticalSection(&g_lock);
    return give > 0 ? give : 0;
}

/* How many reply bytes are queued (the WinUsb/overlapped status path reads this). */
int ultra_namco_io_queued(void) {
    if (!g_lock_init) return 0;
    EnterCriticalSection(&g_lock);
    int q = g_resp_len - g_resp_pos;
    LeaveCriticalSection(&g_lock);
    return q < 0 ? 0 : q;
}

/* ---------------------------------------------------------------------------
 *  SENSE / addressing-done seam.
 *
 *  On the serial path the JVS SENSE line is read via GetCommModemStatus and is
 *  DYNAMIC: unaddressed -> "address me", addressed -> "chain done". On the USB
 *  bridge the same signal is a status byte in an IN report or a vendor control
 *  response. We expose the same dynamic value (keyed off whether our node has taken
 *  an address) so a Namco JVS init walk terminates. The exact byte values are
 *  firmware-specific -> ULTRA_NAMCO_IO_SENSE0 (unaddressed) / _SENSE1 (addressed).
 * ------------------------------------------------------------------------- */
unsigned char ultra_namco_io_sense(void) {
    ultra_jvs_node* node = ultra_hook_node();
    int addressed = (node && jvs_address(node) != 0);
    const char* e = getenv(addressed ? "ULTRA_NAMCO_IO_SENSE1" : "ULTRA_NAMCO_IO_SENSE0");
    if (e && *e) return (unsigned char)strtoul(e, NULL, 0);
    /* benign default: unaddressed = 0 ("address me"), addressed = 1 ("done"). */
    return addressed ? 0x01 : 0x00;
}

/* ---------------------------------------------------------------------------
 *  amauthd / amdaemon LOCAL presence answer surface.
 *
 *  This mirrors namco_amdaemon.c's posture for the ES auth daemon, exposed here so
 *  a board armed via ULTRA_NAMCO_IO gets the local presence handshake from one unit.
 *  It answers ONLY non-secret, LOCAL / AUTH-STATE classes and DEFERS (returns 0) on
 *  every key/content/online class -- the ethical line, hard-coded:
 *
 *    0x00 presence ping        -> ack {00,00}
 *    0x01 local status query   -> "auth OK / network OK (offline)" {00,00}
 *    0x02 version query        -> ack + a benign version triple {00, maj,min,rev}
 *    0x03 region query         -> ack + region byte (ULTRA_NAMCO_IO_REGION, default 0=export-any)
 *    0x04 play-count query     -> ack + 0 (local, no billing) {00,00,00}
 *    default                   -> DEFER (0): online auth/billing/account, key
 *                                 material, content decryption -> never answered.
 *
 *  Returns the response length, or 0 on a deferred/unknown class.
 *  Exposed (non-static) for the unit test.
 * ------------------------------------------------------------------------- */
int ultra_namco_io_auth_answer(const unsigned char* req, int req_len,
                               unsigned char* resp, int resp_cap) {
    if (!req || req_len < 1 || !resp || resp_cap < 2) return 0;
    switch (req[0]) {
        case 0x00:                       /* presence ping */
            resp[0] = 0x00; resp[1] = 0x00; return 2;
        case 0x01:                       /* LOCAL status -> auth OK / network OK */
            resp[0] = 0x00; resp[1] = 0x00; return 2;
        case 0x02: {                     /* version query (AUTH-STATE metadata) */
            if (resp_cap < 4) return 0;
            resp[0] = 0x00;              /* ok */
            resp[1] = 0x01;              /* maj (benign placeholder) */
            resp[2] = 0x00;              /* min */
            resp[3] = 0x00;              /* rev */
            return 4;
        }
        case 0x03: {                     /* region query (IDENTITY metadata) */
            unsigned char rg = 0x00;     /* 0 = export/any (no region lock asserted) */
            const char* e = getenv("ULTRA_NAMCO_IO_REGION");
            if (e && *e) rg = (unsigned char)strtoul(e, NULL, 0);
            resp[0] = 0x00; resp[1] = rg; return 2;
        }
        case 0x04:                       /* play-count query (LOCAL, no billing) */
            if (resp_cap < 3) return 0;
            resp[0] = 0x00; resp[1] = 0x00; resp[2] = 0x00; return 3;
        default:
            /* online auth / billing / account / KEY / CONTENT -> DEFER. The forbidden
             * line: we never derive a key, decrypt content, or stand in for a live
             * service. Returning 0 lets the caller fall through to its real path. */
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 *  Transport seam (case B): intercept the device OPEN by VID/PID, and the
 *  ReadFile/WriteFile against the synthetic handle.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL   (WINAPI *pReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL   (WINAPI *pWriteFile)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static pCreateFileA Real_CreateFileA = NULL;
static pCreateFileW Real_CreateFileW = NULL;
static pReadFile    Real_ReadFile    = NULL;
static pWriteFile   Real_WriteFile   = NULL;

/* A device path names the Namco JVS-USB bridge when it carries the pinned VID/PID
 * token (case-insensitive). With no ULTRA_NAMCO_IO_VIDPID set, NOTHING matches --
 * case (B) stays inert (case A is the serial hook's job). */
static int namco_path_matches(const char* n) {
    if (!n || !g_vidpid[0]) return 0;
    char low[300]; int i = 0;
    for (; n[i] && i < 299; ++i) { char c = n[i]; low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    low[i] = 0;
    return strstr(low, g_vidpid) != NULL;
}
static int namco_path_matches_w(LPCWSTR n) {
    if (!n) return 0;
    char buf[300]; int i = 0;
    for (; n[i] && i < 299; ++i) buf[i] = (char)n[i];
    buf[i] = 0;
    return namco_path_matches(buf);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && namco_path_matches(name)) {
        HANDLE real = Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;     /* real bridge present */
        nlog("[namco_io] synthesizing JVS-USB bridge presence (no physical ESIF bridge)\n");
        return NAMCO_JVS_SENTINEL;
    }
    return Real_CreateFileA ? Real_CreateFileA(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD sh,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_enabled && namco_path_matches_w(name)) {
        HANDLE real = Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                                       : INVALID_HANDLE_VALUE;
        if (real != INVALID_HANDLE_VALUE) return real;
        nlog("[namco_io] synthesizing JVS-USB bridge presence (W) (no physical ESIF bridge)\n");
        return NAMCO_JVS_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* WriteFile against the synthetic bridge handle = a JVS request -> bridge + queue
 * the reply; everything else falls through. */
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == NAMCO_JVS_SENTINEL) {
        ultra_namco_io_bridge((const unsigned char*)buf, (int)n);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
/* ReadFile against the synthetic bridge handle drains the queued JVS reply. */
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_enabled && h == NAMCO_JVS_SENTINEL) {
        int g = ultra_namco_io_drain((unsigned char*)buf, (int)n);
        if (got) *got = (DWORD)g;
        (void)ov;
        return TRUE;          /* a 0-byte read just means "no reply yet", not an error */
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 *  WinUsb pipe seam (case B, WinUSB shape). When the bridge is opened via
 *  WinUsb_Initialize on a real device handle rather than a CreateFile sentinel, the
 *  game moves JVS bytes with WinUsb_WritePipe / WinUsb_ReadPipe. We intercept those
 *  two so a WinUSB-style bridge also bridges through the shared JVS node. The
 *  WinUsb interface handle the game holds is opaque; we cannot reliably synthesize
 *  it without the device, so this path is armed only when the device opened through
 *  our CreateFile sentinel (we match the sentinel as the interface handle too).
 *  This keeps the shims byte-safe: a real WinUSB handle is never our sentinel, so
 *  real traffic always falls through.
 * ------------------------------------------------------------------------- */
typedef BOOL (WINAPI *pWinUsb_WritePipe)(void*,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
typedef BOOL (WINAPI *pWinUsb_ReadPipe)(void*,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
static pWinUsb_WritePipe Real_WinUsb_WritePipe = NULL;
static pWinUsb_ReadPipe  Real_WinUsb_ReadPipe  = NULL;

static BOOL WINAPI Hook_WinUsb_WritePipe(void* h, UCHAR pipe, PUCHAR buf, ULONG len,
                                         PULONG xfer, LPOVERLAPPED ov) {
    if (g_enabled && (HANDLE)h == NAMCO_JVS_SENTINEL) {
        ultra_namco_io_bridge((const unsigned char*)buf, (int)len);
        if (xfer) *xfer = len;
        (void)pipe; (void)ov;
        return TRUE;
    }
    return Real_WinUsb_WritePipe ? Real_WinUsb_WritePipe(h, pipe, buf, len, xfer, ov) : FALSE;
}
static BOOL WINAPI Hook_WinUsb_ReadPipe(void* h, UCHAR pipe, PUCHAR buf, ULONG len,
                                        PULONG xfer, LPOVERLAPPED ov) {
    if (g_enabled && (HANDLE)h == NAMCO_JVS_SENTINEL) {
        int g = ultra_namco_io_drain((unsigned char*)buf, (int)len);
        if (xfer) *xfer = (ULONG)g;
        (void)pipe; (void)ov;
        return TRUE;
    }
    return Real_WinUsb_ReadPipe ? Real_WinUsb_ReadPipe(h, pipe, buf, len, xfer, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 *  Install. Env-gated (ULTRA_NAMCO_IO=1, set by a Namco ES/N2 board profile).
 *  Default OFF so a normal native_x86 run is byte-identical. Idempotent. With no
 *  ULTRA_NAMCO_IO_VIDPID pinned, the device match never fires (case A stays the
 *  serial hook's job), so arming alone changes nothing observable until a real
 *  bridge id is supplied.
 * ------------------------------------------------------------------------- */
void ultra_namco_io_install(void) {
    const char* e = getenv("ULTRA_NAMCO_IO");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }

    const char* vp = getenv("ULTRA_NAMCO_IO_VIDPID");
    if (vp && vp[0]) {
        strncpy(g_vidpid, vp, sizeof(g_vidpid) - 1);
        g_vidpid[sizeof(g_vidpid) - 1] = 0;
        for (char* q = g_vidpid; *q; ++q) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
    }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (k) {
        if (!Real_CreateFileA) Real_CreateFileA = (pCreateFileA)(void*)GetProcAddress(k, "CreateFileA");
        if (!Real_CreateFileW) Real_CreateFileW = (pCreateFileW)(void*)GetProcAddress(k, "CreateFileW");
        if (!Real_ReadFile)    Real_ReadFile    = (pReadFile)(void*)GetProcAddress(k, "ReadFile");
        if (!Real_WriteFile)   Real_WriteFile   = (pWriteFile)(void*)GetProcAddress(k, "WriteFile");
    }
    ultra_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    ultra_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    ultra_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    ultra_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    /* WinUSB pipe shims: only patched if winusb.dll is in the import space. The
     * patcher is a no-op for a name the game does not import, so listing them is
     * harmless when the bridge is the CreateFile/CDC shape instead. */
    HMODULE wu = GetModuleHandleA("winusb.dll");
    if (wu) {
        if (!Real_WinUsb_WritePipe) Real_WinUsb_WritePipe = (pWinUsb_WritePipe)(void*)GetProcAddress(wu, "WinUsb_WritePipe");
        if (!Real_WinUsb_ReadPipe)  Real_WinUsb_ReadPipe  = (pWinUsb_ReadPipe)(void*)GetProcAddress(wu, "WinUsb_ReadPipe");
    }
    ultra_iat_patch_all("winusb.dll", "WinUsb_WritePipe", (void*)Hook_WinUsb_WritePipe);
    ultra_iat_patch_all("winusb.dll", "WinUsb_ReadPipe",  (void*)Hook_WinUsb_ReadPipe);

    nlog("[namco_io] Namco JVS-over-USB I/O + amauthd LOCAL presence armed "
         "(vidpid~='%s'%s). JVS payload bridged through the shared clean-room node; "
         "presence answers ping/status/version/region/play-count ONLY -- online "
         "auth/billing + ANY key/content op is DEFERRED. NOTE: VID/PID + SENSE byte "
         "are firmware-specific -> ULTRA_NAMCO_IO_* knobs (case A virtual-COM bridges "
         "stay on the serial hook via ULTRA_JVS_COM).\n",
         g_vidpid, g_vidpid[0] ? "" : " (UNSET -> USB-device match inert)");
}

int ultra_namco_io_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
