/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  codepage_spoof.c -- make GetACP()/GetOEMCP() report Shift-JIS (932)
 * =============================================================================
 *
 *  Several Japan-only Type X titles were built (and only ever tested) on a
 *  Japanese Windows install, where GetACP() returns 932. Their own text/font
 *  code calls GetACP() to decide whether a byte pair is a Shift-JIS lead+trail
 *  pair or two separate single-byte glyphs. On any other Windows locale
 *  GetACP() returns something else (1252 on this machine), so ASCII bytes
 *  still look right (ASCII is identical in both codepages) but every Shift-JIS
 *  double-byte character gets decoded as two wrong single-byte glyphs -- which
 *  is exactly the "some lines clean, some garbled" mojibake Block King Ball
 *  Shooter's boot screen showed.
 *
 *  This does not touch the game's files or the OS locale. It only makes this
 *  one injected process see 932 when it asks Windows what codepage it is.
 *  CP932's conversion tables ship with Windows itself, so no language pack is
 *  required for MultiByteToWideChar/GetCPInfo to actually decode it once the
 *  game believes it should.
 *
 *  Env-gated, OFF by default: ULTRA_FORCE_JP_CODEPAGE=1 arms it for a title
 *  whose board profile sets that flag (see taito_typex_main.c's per-title
 *  environment table).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

#define ULTRA_JP_CODEPAGE 932u

static int g_enabled = 0;

static UINT WINAPI Hook_GetACP(void) {
    return g_enabled ? ULTRA_JP_CODEPAGE : GetACP();
}

static UINT WINAPI Hook_GetOEMCP(void) {
    return g_enabled ? ULTRA_JP_CODEPAGE : GetOEMCP();
}

void ultra_codepage_spoof_install(void) {
    const char* e = getenv("ULTRA_FORCE_JP_CODEPAGE");
    g_enabled = e && e[0] == '1';
    if (!g_enabled) return;

    ultra_iat_patch_all("kernel32.dll", "GetACP",   (void*)Hook_GetACP);
    ultra_iat_patch_all("kernel32.dll", "GetOEMCP", (void*)Hook_GetOEMCP);
}

#endif /* _WIN32 */
