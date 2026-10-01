/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  felica_spoof_typex4.c -- fake felica_nfc_library.dll (Sony FeliCa SDK)
 * =============================================================================
 *
 *  Gunslinger Stratos 3 gates its boot on a "touch sensor" (Taito's label for
 *  its Amusement-IC/FeliCa card-touch panel) and shows a hard error --
 *  "Touch sensor error (0xffffffff) / not connected" -- when Sony's real
 *  FeliCa reader driver isn't present. nfc.dll (the game's direct import)
 *  is a thin wrapper that forwards straight to Sony's own official
 *  "FeliCa NFC Library" (felica_nfc_library.dll), which talks to a real Sony
 *  reader over IOCTL_FELICA_GET_DRIVER_STATUS -- a private wire protocol we
 *  have no hardware to capture and no public spec for.
 *
 *  WHAT THIS DOES, HONESTLY: nfc.dll imports exactly 8 named functions from
 *  felica_nfc_library.dll. This IAT-patches THOSE 8 imports (never the raw
 *  IOCTL/driver layer) to our own stand-ins, so nfc.dll never talks to the
 *  real Sony library at all in this process -- there is no half-real,
 *  half-faked state to go inconsistent. Every stand-in returns a plain
 *  integer status matching the library's own error-constant naming
 *  convention (FELICA_NFC_E_*, implying 0 = success): the init/open/poll
 *  path reports success so the boot gate passes, teardown calls report
 *  success too, get_last_error reports "no error", and the one function that
 *  would actually move card data (felicalib_nfc_thru) reports failure
 *  WITHOUT touching any parameter -- we do not know its buffer layout, so it
 *  never writes to memory it does not understand.
 *
 *  THIS IS A GUESS, not a verified protocol capture -- there is no real
 *  FeliCa reader on this machine to check the guess against. The reasoning
 *  (small-int status codes, 0 = success) matches every visible string in the
 *  DLL, but the project owner explicitly accepted this risk over waiting for
 *  real hardware. Env-gated OFF by default (ULTRA_FELICA_STUB=1 arms it) so
 *  every other Type X4 title is unaffected either way.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>

void ultra_tx4_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
void ultra_tx4_log(const char* fmt, ...);

static int g_enabled = 0;

/* Real signatures are unknown -- x64's calling convention lets a callee
 * declare fewer parameters than the caller passes with no stack-corruption
 * risk (the caller always owns cleanup), so every stand-in below simply
 * ignores whatever it was handed and returns a plain status code. */
static int WINAPI Stub_OK(void)     { return 0; }   /* init/open/poll/teardown: success */
static int WINAPI Stub_NoError(void){ return 0; }   /* get_last_error: no error pending */
static int WINAPI Stub_NoCard(void) { return 1; }   /* thru: "failed" -- touches nothing */

void ultra_tx4_felica_stub_install(void) {
    const char* e = getenv("ULTRA_FELICA_STUB");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;

    const char* dll = "felica_nfc_library.dll";
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_initialize",             (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_open",                   (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_poll_and_start_dev_access", (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_stop_dev_access",        (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_close",                  (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_uninitialize",           (void*)Stub_OK);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_get_last_error",         (void*)Stub_NoError);
    ultra_tx4_iat_patch_all(dll, "felicalib_nfc_thru",                   (void*)Stub_NoCard);

    ultra_tx4_log("[felica] UNVERIFIED stub armed (ULTRA_FELICA_STUB=1) -- "
                  "nfc.dll's 8 felica_nfc_library.dll imports redirected; "
                  "no real Sony FeliCa reader was ever consulted");
}

#endif /* _WIN32 */
