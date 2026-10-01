/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  dongle_bypass_examu.c  --  Examu eX-BOARD board's OWN error-dialog auto-
 *                              dismiss + profile-driven byte-patch seam.
 * =============================================================================
 *
 *  Independent copy for modules/examu_exboard/src/hook/ only. Adapted from the
 *  PATTERN in modules/native_x86/src/hook/dongle_bypass.c (read as reference
 *  only -- no #include, no link, no runtime dependency on that file). Own
 *  file, own symbols, wired through this board's own IAT patcher
 *  (ultra_iat_patch_all_examu, owned by jvs_serial_hook_examu.c).
 *
 *  The actual USB/parallel security-dongle device-name bypass (Hardlock/HASP/
 *  SentinelSuperPro/SafeNet/Aladdin) lives in jvs_serial_hook_examu.c's
 *  Hook_CreateFileA/W (mirrors the reference's is_dongle_device() placement,
 *  which lives in ITS jvs_serial_hook.c rather than in dongle_bypass.c) --
 *  this file only owns the two pieces that are genuinely board-generic:
 *
 *  CLEAN-ROOM POLICY (important): the *exact* check for each eX-BOARD title
 *  lives in that title's code and can only be pinned down against a dump the
 *  user owns. So this file ships NO per-game patches and NO dongle data:
 *
 *    1) error-dialog auto-dismiss -- intercept MessageBoxA/W so a blocking
 *       hardware-error popup doesn't halt an otherwise-running game. Off by
 *       default; ULTRA_DISMISS_DIALOGS=1 (set by examu_exboard_main.c) arms
 *       it. Every dialog is logged first (title + text via ULTRA_JVS_LOG) so
 *       a test run shows exactly what the game complained about.
 *
 *    2) profile-driven byte patches -- read a plain text patch list the user
 *       writes for their dump ("module+offset=bytes") and apply it in
 *       memory. This is exactly how a JVS/dongle check is neutralized once
 *       located, with nothing proprietary baked into Ultra Loader.
 *       ULTRA_PATCH_FILE names the list.
 *
 *  Everything here is Ultra Loader's own code over the public Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* owned by jvs_serial_hook_examu.c */
extern void ultra_iat_patch_all_examu(const char* impdll, const char* impfn, void* newfn);
extern void ultra_examu_log(const char* fmt, ...);
#define klog ultra_examu_log

/* ---- (1) MessageBox auto-dismiss --------------------------------------- */
typedef int (WINAPI *pMessageBoxA)(HWND,LPCSTR,LPCSTR,UINT);
typedef int (WINAPI *pMessageBoxW)(HWND,LPCWSTR,LPCWSTR,UINT);
static pMessageBoxA Real_MessageBoxA_Examu;
static pMessageBoxW Real_MessageBoxW_Examu;
static int g_dismiss_dialogs = 0;

static int default_button(UINT type) {
    /* Answer so the game gets PAST a blocking error and keeps running: dismiss
     * (don't retry forever, don't abort, don't dive into a test menu). */
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
static int WINAPI Hook_MessageBoxA_Examu(HWND w, LPCSTR t, LPCSTR c, UINT type) {
    klog("MESSAGEBOX(examu) title=\"%s\" text=\"%s\"", c ? c : "", t ? t : "");
    if (g_dismiss_dialogs) return default_button(type);
    return Real_MessageBoxA_Examu(w, t, c, type);
}
static int WINAPI Hook_MessageBoxW_Examu(HWND w, LPCWSTR t, LPCWSTR c, UINT type) {
    char tt[256] = {0}, cc[256] = {0};
    if (t) WideCharToMultiByte(CP_ACP, 0, t, -1, tt, sizeof(tt)-1, NULL, NULL);
    if (c) WideCharToMultiByte(CP_ACP, 0, c, -1, cc, sizeof(cc)-1, NULL, NULL);
    klog("MESSAGEBOX(examu) title=\"%s\" text=\"%s\"", cc, tt);
    if (g_dismiss_dialogs) return default_button(type);
    return Real_MessageBoxW_Examu(w, t, c, type);
}

/* ---- (2) profile-driven byte patches ----------------------------------- *
 * patch file lines (one per patch), '#' starts a comment:
 *     module+0xADDR = 90 90 90        ; ADDR is an RVA (offset from module base)
 *     0x00001234    = EB              ; bare RVA -> applied to the main module
 * Whitespace and case are flexible; bytes are hex pairs.
 */
static int parse_hex_byte(const char* s, unsigned* out) {
    if (!s[0] || !s[1]) return 0;                 /* need two chars; don't read past NUL */
    char b[3] = { s[0], s[1], 0 };
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return 0;
    *out = (unsigned)strtoul(b, NULL, 16);
    return 1;
}

/* mapped image size of a module, for bounds-checking a patch target */
static DWORD module_image_size(HMODULE mod) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}
static void apply_one_patch(const char* modname, uintptr_t off,
                            const unsigned char* bytes, size_t nbytes, FILE* logf) {
    HMODULE mod = modname && *modname ? GetModuleHandleA(modname) : GetModuleHandleA(NULL);
    if (!mod) { if (logf) fprintf(logf, "  [skip] module '%s' not loaded\n", modname?modname:"(main)"); return; }
    /* `off` is an RVA from the module base. Reject anything outside the module's
     * mapped image so a bad profile can't corrupt unrelated memory. */
    DWORD isize = module_image_size(mod);
    if (isize && (off + nbytes > isize)) {
        if (logf) fprintf(logf, "  [skip] %s+0x%zx out of module image (size 0x%lx)\n",
                          modname?modname:"(main)", (size_t)off, (unsigned long)isize);
        return;
    }
    BYTE* tgt = (BYTE*)mod + off;
    DWORD old;
    if (VirtualProtect(tgt, nbytes, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(tgt, bytes, nbytes);
        VirtualProtect(tgt, nbytes, old, &old);
        FlushInstructionCache(GetCurrentProcess(), tgt, nbytes);
        if (logf) fprintf(logf, "  [ok] %s+0x%zx <- %zu bytes\n", modname?modname:"(main)", (size_t)off, nbytes);
    } else if (logf) {
        fprintf(logf, "  [fail] VirtualProtect %s+0x%zx\n", modname?modname:"(main)", (size_t)off);
    }
}
static void apply_patch_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    FILE* logf = fopen("ultra_examu_patch.log", "w");
    if (logf) fprintf(logf, "Ultra Loader eX-BOARD patch profile: %s\n", path);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* hash = strchr(line, '#'); if (hash) *hash = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* lhs = line; char* rhs = eq + 1;
        /* lhs: [module+]offset */
        char modname[256] = {0}; uintptr_t off = 0;
        char* plus = strchr(lhs, '+');
        if (plus) {
            *plus = 0;
            char* m = lhs; while (*m==' '||*m=='\t') ++m;
            char* e = m + strlen(m); while (e>m && (e[-1]==' '||e[-1]=='\t')) *--e=0;
            /* bounded copy that always NUL-terminates (avoids strncpy-truncation) */
            size_t mlen = strlen(m);
            if (mlen > sizeof(modname)-1) mlen = sizeof(modname)-1;
            memcpy(modname, m, mlen);
            modname[mlen] = 0;
            off = (uintptr_t)strtoull(plus+1, NULL, 0);
        } else {
            off = (uintptr_t)strtoull(lhs, NULL, 0);
        }
        /* rhs: hex byte list */
        unsigned char bytes[64]; size_t nb = 0;
        for (char* p = rhs; *p && nb < sizeof(bytes); ) {
            while (*p==' '||*p=='\t'||*p==','||*p=='\r'||*p=='\n') ++p;
            if (!*p) break;
            if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) p += 2;
            unsigned v;
            if (parse_hex_byte(p, &v)) { bytes[nb++] = (unsigned char)v; p += 2; }
            else break;
        }
        if (nb) apply_one_patch(modname, off, bytes, nb, logf);
    }
    if (logf) fclose(logf);
    fclose(f);
}

/* ---- install ----------------------------------------------------------- */
void ultra_dongle_install_examu(void) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        Real_MessageBoxA_Examu = (pMessageBoxA)(void*)GetProcAddress(u32, "MessageBoxA");
        Real_MessageBoxW_Examu = (pMessageBoxW)(void*)GetProcAddress(u32, "MessageBoxW");
        if (Real_MessageBoxA_Examu) ultra_iat_patch_all_examu("user32.dll", "MessageBoxA", (void*)Hook_MessageBoxA_Examu);
        if (Real_MessageBoxW_Examu) ultra_iat_patch_all_examu("user32.dll", "MessageBoxW", (void*)Hook_MessageBoxW_Examu);
    }
    const char* dd = getenv("ULTRA_DISMISS_DIALOGS");
    g_dismiss_dialogs = dd && dd[0] == '1';
    if (g_dismiss_dialogs) klog("dongle_bypass(examu): dialog auto-dismiss armed");

    const char* pf = getenv("ULTRA_PATCH_FILE");
    if (pf && *pf) apply_patch_file(pf);
}

#endif /* _WIN32 */
