/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  namco_io_es2plus.c -- Namco System ES2+ JVS-over-USB I/O + amauthd LOCAL
 *                        presence (clean-room, env-gated, default OFF,
 *                        byte-safe). namco_es2plus's OWN independent copy.
 * =============================================================================
 *
 *  Namco System ES2+ (Windows-Embedded variant) does NOT put its cabinet I/O on
 *  a plain serial COM port. The game's runtime ("ESIF" / the jvs.dll-class
 *  layer) opens a USB JVS-BRIDGE and talks to it. Two physical shapes are seen
 *  on the public board images:
 *
 *    (A) USB-CDC virtual COM -- the bridge enumerates as a virtual \\.\COMx.
 *        This board's hook does not include a serial transport (ES2+ has no
 *        confirmed COM-JVS path); if a real dump needs it, ULTRA_JVS_COM-style
 *        serial framing can be added later without touching case (B) below.
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
 *  READ_COIN, READ_ANALOG ...). i.e. the PAYLOAD is exactly what jvs_core_es2plus.c
 *  already speaks. So this file is an IO4-style transport shell re-pointed at a
 *  Namco VID/PID, with the bytes bridged through jvs_host_bytes() (the shared
 *  clean-room JVS node this board's hook owns). It is purely an I/O RE-PACKING
 *  of the hook's own ultra_jvs_input block + clean-room JVS node. It owns NO
 *  security/crypto.
 *
 *  PRESENCE: this file also answers the LOCAL amdaemon/amauthd presence
 *  handshake: "is the local auth daemon present, and is local/offline status
 *  OK". That is PRESENCE / AUTH-STATE ONLY. The ETHICAL LINE is hard-coded into
 *  the answer surface, PRESERVED EXACTLY from the reference this was copied
 *  from:
 *    - presence ping / local-status / version / region / play-count  -> answered.
 *    - ANYTHING that derives a key, decrypts content, or reaches a LIVE online
 *      service (ALL.Net / Namco billing / account / key material)     -> DEFER
 *      (return 0). Never answered, never impersonated.
 *  It ships NO keys, NO account data, NO game content, and is OFF unless armed.
 *
 *  HONEST GAP: the bridge's vendor VID/PID, the vendor bRequest for ident/reset,
 *  and the SENSE/addressing-done byte position are firmware-specific and not in
 *  the public protocol record. They are env-PINNED against a real dump
 *  (ULTRA_ES2P_IO_* knobs), NEVER baked in -- same TPM/LUKS+HASP presence-only
 *  wall as ES1; never implement real key unwrap/decrypt.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 device API
 *  (CreateFile/ReadFile/WriteFile + WinUSB/SetupAPI surface), the open JVS spec
 *  (payload already in jvs_core_es2plus.c), and the public description of the
 *  ES auth-daemon gate. No third-party loader source, profile, descriptor, or
 *  daemon binary was read.
 *
 *  INDEPENDENCE: this is namco_es2plus's own private copy (built ONLY into
 *  ultra_namco_es2plus_hook{32,64}.dll). Reference read-only from
 *  modules/native_x86/src/hook/namco_io.c; no shared runtime binary, no link
 *  against native_x86's compiled output.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "jvs_core_es2plus.h"

/* from hook_dllmain_es2plus.c -- this board's own IAT patcher + shared JVS node
 * + controller snapshot. */
extern void            es2p_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
extern ultra_jvs_node* es2p_hook_node(void);
extern void            es2p_hook_pull_input(ultra_jvs_node* n);
extern void            es2p_log(const char* fmt, ...);

static int g_enabled = 0;   /* ULTRA_ES2P_IO=1 (set by the namco_es2plus module) */

/* ---------------------------------------------------------------------------
 *  Namco JVS-USB bridge identity (case B).
 *
 *  The VID/PID of the ESIF USB JVS bridge is firmware-specific. We DO NOT bake a
 *  guess: the device match is OFF until ULTRA_ES2P_IO_VIDPID pins it (form
 *  "vid_xxxx&pid_yyyy", lower-case hex, as it appears in the Windows device path).
 *  A user with a real ES2+ dump reads the VID/PID off the bridge and sets the
 *  knob. This keeps case (B) inert unless a real bridge id is supplied.
 * ------------------------------------------------------------------------- */
static char g_vidpid[64] = "";   /* e.g. "vid_0f0d&pid_00XX" (no default)          */

/* sentinel handle for the synthetic JVS-USB bridge endpoint. */
#define ES2P_JVS_SENTINEL ((HANDLE)(ULONG_PTR)0x4A325532)  /* 'JV2U'+1 (distinct from any other board's sentinel) */

/* ---------------------------------------------------------------------------
 *  Response queue for the synthetic bridge handle. A WriteFile of a JVS request
 *  produces (via jvs_core_es2plus) a framed JVS reply which the next ReadFile
 *  drains -- one queue for one USB handle. */
static unsigned char g_resp[2048];
static int           g_resp_len = 0;
static int           g_resp_pos = 0;
static CRITICAL_SECTION g_lock;
static int              g_lock_init = 0;

/* Bridge a host->bridge JVS request through the shared JVS node, queueing the
 * framed reply for the next read. Returns the number of reply bytes queued. */
int es2p_namco_io_bridge(const unsigned char* req, int req_len) {
    if (!g_lock_init) return 0;
    EnterCriticalSection(&g_lock);
    ultra_jvs_node* node = es2p_hook_node();
    if (node) es2p_hook_pull_input(node);          /* latest controller state     */

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
 * number of bytes copied. */
int es2p_namco_io_drain(unsigned char* buf, int cap) {
    if (!g_lock_init || !buf || cap <= 0) return 0;
    EnterCriticalSection(&g_lock);
    int avail = g_resp_len - g_resp_pos;
    int give  = (cap < avail) ? cap : avail;
    if (give > 0) { memcpy(buf, &g_resp[g_resp_pos], give); g_resp_pos += give; }
    LeaveCriticalSection(&g_lock);
    return give > 0 ? give : 0;
}

/* How many reply bytes are queued (the WinUsb/overlapped status path reads this). */
int es2p_namco_io_queued(void) {
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
 *  response. We expose the same dynamic value (keyed off whether our node has
 *  taken an address) so a Namco JVS init walk terminates. The exact byte values
 *  are firmware-specific -> ULTRA_ES2P_IO_SENSE0 (unaddressed) / _SENSE1
 *  (addressed).
 * ------------------------------------------------------------------------- */
unsigned char es2p_namco_io_sense(void) {
    ultra_jvs_node* node = es2p_hook_node();
    int addressed = (node && jvs_address(node) != 0);
    const char* e = getenv(addressed ? "ULTRA_ES2P_IO_SENSE1" : "ULTRA_ES2P_IO_SENSE0");
    if (e && *e) return (unsigned char)strtoul(e, NULL, 0);
    /* benign default: unaddressed = 0 ("address me"), addressed = 1 ("done"). */
    return addressed ? 0x01 : 0x00;
}

/* ---------------------------------------------------------------------------
 *  amauthd / amdaemon LOCAL presence answer surface.
 *
 *  This mirrors the ES auth daemon's posture for namco_es2plus's own hook. It
 *  answers ONLY non-secret, LOCAL / AUTH-STATE classes and DEFERS (returns 0) on
 *  every key/content/online class -- the ethical line, hard-coded:
 *
 *    0x00 presence ping        -> ack {00,00}
 *    0x01 local status query   -> "auth OK / network OK (offline)" {00,00}
 *    0x02 version query        -> ack + a benign version triple {00, maj,min,rev}
 *    0x03 region query         -> ack + region byte (ULTRA_ES2P_IO_REGION, default 0=export-any)
 *    0x04 play-count query     -> ack + 0 (local, no billing) {00,00,00}
 *    default                   -> DEFER (0): online auth/billing/account, key
 *                                 material, content decryption -> never answered.
 *
 *  Returns the response length, or 0 on a deferred/unknown class.
 * ------------------------------------------------------------------------- */
int es2p_namco_io_auth_answer(const unsigned char* req, int req_len,
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
            const char* e = getenv("ULTRA_ES2P_IO_REGION");
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
 * token (case-insensitive). With no ULTRA_ES2P_IO_VIDPID set, NOTHING matches --
 * case (B) stays inert. */
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
        es2p_log("[namco_io_es2plus] synthesizing JVS-USB bridge presence (no physical ESIF bridge)\n");
        return ES2P_JVS_SENTINEL;
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
        es2p_log("[namco_io_es2plus] synthesizing JVS-USB bridge presence (W) (no physical ESIF bridge)\n");
        return ES2P_JVS_SENTINEL;
    }
    return Real_CreateFileW ? Real_CreateFileW(name, acc, sh, sa, disp, flags, tmpl)
                            : INVALID_HANDLE_VALUE;
}

/* WriteFile against the synthetic bridge handle = a JVS request -> bridge + queue
 * the reply; everything else falls through. */
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov) {
    if (g_enabled && h == ES2P_JVS_SENTINEL) {
        es2p_namco_io_bridge((const unsigned char*)buf, (int)n);
        if (wrote) *wrote = n;
        (void)ov;
        return TRUE;
    }
    return Real_WriteFile ? Real_WriteFile(h, buf, n, wrote, ov) : FALSE;
}
/* ReadFile against the synthetic bridge handle drains the queued JVS reply. */
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (g_enabled && h == ES2P_JVS_SENTINEL) {
        int g = es2p_namco_io_drain((unsigned char*)buf, (int)n);
        if (got) *got = (DWORD)g;
        (void)ov;
        return TRUE;          /* a 0-byte read just means "no reply yet", not an error */
    }
    return Real_ReadFile ? Real_ReadFile(h, buf, n, got, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 *  WinUsb pipe seam (case B, WinUSB shape). When the bridge is opened via
 *  WinUsb_Initialize on a real device handle rather than a CreateFile sentinel,
 *  the game moves JVS bytes with WinUsb_WritePipe / WinUsb_ReadPipe. We
 *  intercept those two so a WinUSB-style bridge also bridges through the
 *  shared JVS node. This path is armed only when the device opened through our
 *  CreateFile sentinel (we match the sentinel as the interface handle too). A
 *  real WinUSB handle is never our sentinel, so real traffic always falls
 *  through. */
typedef BOOL (WINAPI *pWinUsb_WritePipe)(void*,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
typedef BOOL (WINAPI *pWinUsb_ReadPipe)(void*,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
static pWinUsb_WritePipe Real_WinUsb_WritePipe = NULL;
static pWinUsb_ReadPipe  Real_WinUsb_ReadPipe  = NULL;

static BOOL WINAPI Hook_WinUsb_WritePipe(void* h, UCHAR pipe, PUCHAR buf, ULONG len,
                                         PULONG xfer, LPOVERLAPPED ov) {
    if (g_enabled && (HANDLE)h == ES2P_JVS_SENTINEL) {
        es2p_namco_io_bridge((const unsigned char*)buf, (int)len);
        if (xfer) *xfer = len;
        (void)pipe; (void)ov;
        return TRUE;
    }
    return Real_WinUsb_WritePipe ? Real_WinUsb_WritePipe(h, pipe, buf, len, xfer, ov) : FALSE;
}
static BOOL WINAPI Hook_WinUsb_ReadPipe(void* h, UCHAR pipe, PUCHAR buf, ULONG len,
                                        PULONG xfer, LPOVERLAPPED ov) {
    if (g_enabled && (HANDLE)h == ES2P_JVS_SENTINEL) {
        int g = es2p_namco_io_drain((unsigned char*)buf, (int)len);
        if (xfer) *xfer = (ULONG)g;
        (void)pipe; (void)ov;
        return TRUE;
    }
    return Real_WinUsb_ReadPipe ? Real_WinUsb_ReadPipe(h, pipe, buf, len, xfer, ov) : FALSE;
}

/* ---------------------------------------------------------------------------
 *  Install. Env-gated (ULTRA_ES2P_IO=1, set by the namco_es2plus board module
 *  or a user override). Default OFF so a normal run is byte-identical. With no
 *  ULTRA_ES2P_IO_VIDPID pinned, the device match never fires, so arming alone
 *  changes nothing observable until a real bridge id is supplied.
 * ------------------------------------------------------------------------- */
void es2p_namco_io_install(void) {
    const char* e = getenv("ULTRA_ES2P_IO");
    /* default-ON for this board (unlike the shared native_x86 stub): this is
     * ES2+'s OWN dedicated hook, so its one I/O transport should be armed
     * unless a user explicitly opts out with ULTRA_ES2P_IO=0. */
    g_enabled = !(e && e[0] == '0');
    if (!g_enabled) return;

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }

    const char* vp = getenv("ULTRA_ES2P_IO_VIDPID");
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
    es2p_iat_patch_all("kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA);
    es2p_iat_patch_all("kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW);
    es2p_iat_patch_all("kernel32.dll", "ReadFile",    (void*)Hook_ReadFile);
    es2p_iat_patch_all("kernel32.dll", "WriteFile",   (void*)Hook_WriteFile);

    /* WinUSB pipe shims: only patched if winusb.dll is in the import space. The
     * patcher is a no-op for a name the game does not import, so listing them is
     * harmless when the bridge is the CreateFile/CDC shape instead. */
    HMODULE wu = GetModuleHandleA("winusb.dll");
    if (wu) {
        if (!Real_WinUsb_WritePipe) Real_WinUsb_WritePipe = (pWinUsb_WritePipe)(void*)GetProcAddress(wu, "WinUsb_WritePipe");
        if (!Real_WinUsb_ReadPipe)  Real_WinUsb_ReadPipe  = (pWinUsb_ReadPipe)(void*)GetProcAddress(wu, "WinUsb_ReadPipe");
    }
    es2p_iat_patch_all("winusb.dll", "WinUsb_WritePipe", (void*)Hook_WinUsb_WritePipe);
    es2p_iat_patch_all("winusb.dll", "WinUsb_ReadPipe",  (void*)Hook_WinUsb_ReadPipe);

    es2p_log("[namco_io_es2plus] Namco ES2+ JVS-over-USB I/O + amauthd LOCAL presence armed "
         "(vidpid~='%s'%s). JVS payload bridged through this board's own clean-room node; "
         "presence answers ping/status/version/region/play-count ONLY -- online "
         "auth/billing + ANY key/content op is DEFERRED. NOTE: VID/PID + SENSE byte "
         "are firmware-specific -> ULTRA_ES2P_IO_* knobs.\n",
         g_vidpid, g_vidpid[0] ? "" : " (UNSET -> USB-device match inert)");
}

int es2p_namco_io_enabled(void) { return g_enabled; }

#endif /* _WIN32 */
