/* SPDX-License-Identifier: MIT
 *
 *  patch_file_europar.c -- ULTRA_PATCH_FILE runtime byte patcher, Sega Europa-R
 *
 *  Ported 2026-09-20 from konami_pc/src/hook/dongle_bypass.c (read as a
 *  reference; no #include, link, or runtime dependency on that board). Four
 *  boards already carry this mechanism and Europa-R did not.
 *
 *  WHY IT EXISTS HERE. GameShell.exe -- Ford Racing's operator shell, which owns
 *  the coin mech, the credit arithmetic and the service menu -- hardcodes two
 *  absolute paths that cannot resolve on a PC:
 *
 *      RVA 0x3677C   d:\sega\gameshell\resources\graphics\segalogo.bmp
 *      RVA 0x367B0   d:\sega\gameshell\resources\graphics\sysmenu.bmp
 *
 *  Each literal has exactly ONE reference (0x41A11F and 0x41A135 respectively),
 *  and each sits in a NUL-padded span, so both can be overwritten in place with
 *  the RELATIVE forms. They then resolve against the working directory by the
 *  identical mechanism colourbars.bmp (0x4367E4) and grid.bmp (0x436808) already
 *  use -- and those two already work.
 *
 *  This is deliberately chosen over mapping a real D: drive. DefineDosDevice
 *  acts on the whole logon session, not one process, so it would shadow the
 *  user's actual D: (a DVD drive on this machine) for every program, and a
 *  crashed loader would leave the mapping behind.
 *
 *  WHAT IT COSTS: nothing in the dump. The patch lands in memory only, so the
 *  game's exe_sha256 container identity still matches and no module rebuild is
 *  needed to change a patch -- edit the profile file and relaunch.
 *
 *  FORMAT (# comments, one patch per line, offsets are RVAs from the module
 *  base -- a VA will be bounds-rejected):
 *
 *      [module+]<offset> = <hex bytes>
 *      0x3677C = 72 65 73 ...
 *      d3d9.dll+0x1234 = 90 90
 *
 *  ONE SIDE EFFECT, inherited from the original: the log is opened relative to
 *  the CWD, which is the game folder, so it writes one small ultra_patch.log
 *  into the dump. ULTRA_PATCH_LOG=0 suppresses that.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int eur_hex_byte(const char *s, unsigned *out) {
    char b[3];
    b[0] = s[0]; b[1] = s[1]; b[2] = 0;
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return 0;
    *out = (unsigned)strtoul(b, NULL, 16);
    return 1;
}

/* mapped image size, for bounds-checking a target. Guarded the same way the
 * rest of this board's PE reads are: a module can unload under us. */
static DWORD eur_image_size(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    if (!mod) return 0;
    if (IsBadReadPtr(dos, sizeof *dos)) return 0;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof *nt)) return 0;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

static void eur_apply_one(const char *modname, uintptr_t off,
                          const unsigned char *bytes, size_t nbytes, FILE *logf) {
    HMODULE mod = (modname && *modname) ? GetModuleHandleA(modname) : GetModuleHandleA(NULL);
    DWORD isize;
    BYTE *tgt;
    DWORD old;
    if (!mod) {
        if (logf) fprintf(logf, "  [skip] module '%s' not loaded\n", modname ? modname : "(main)");
        return;
    }
    isize = eur_image_size(mod);
    if (isize && (off + nbytes > isize)) {
        if (logf) fprintf(logf, "  [skip] %s+0x%lx outside image (size 0x%lx)\n",
                          modname ? modname : "(main)", (unsigned long)off, (unsigned long)isize);
        return;
    }
    tgt = (BYTE *)mod + off;
    if (VirtualProtect(tgt, nbytes, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(tgt, bytes, nbytes);
        VirtualProtect(tgt, nbytes, old, &old);
        FlushInstructionCache(GetCurrentProcess(), tgt, nbytes);
        if (logf) fprintf(logf, "  [ok] %s+0x%lx <- %u bytes\n",
                          modname ? modname : "(main)", (unsigned long)off, (unsigned)nbytes);
    } else if (logf) {
        fprintf(logf, "  [fail] VirtualProtect %s+0x%lx (err=%lu)\n",
                modname ? modname : "(main)", (unsigned long)off, GetLastError());
    }
}

static void eur_apply_patch_file(const char *path) {
    FILE *f = fopen(path, "r");
    FILE *logf = NULL;
    char line[512];
    char wantlog[8] = {0};
    if (!f) return;
    GetEnvironmentVariableA("ULTRA_PATCH_LOG", wantlog, sizeof wantlog);
    if (!(wantlog[0] && atoi(wantlog) == 0)) {
        logf = fopen("ultra_patch.log", "w");
        if (logf) fprintf(logf, "Ultra Loader patch profile: %s\n", path);
    }
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        char *eq, *lhs, *rhs, *plus, *p;
        char modname[256];
        uintptr_t off = 0;
        unsigned char bytes[256];
        size_t nb = 0;
        if (hash) *hash = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        lhs = line; rhs = eq + 1;
        modname[0] = 0;
        plus = strchr(lhs, '+');
        if (plus) {
            char *m, *e;
            size_t mlen;
            *plus = 0;
            m = lhs; while (*m == ' ' || *m == '\t') ++m;
            e = m + strlen(m); while (e > m && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            mlen = strlen(m);
            if (mlen > sizeof(modname) - 1) mlen = sizeof(modname) - 1;
            memcpy(modname, m, mlen);
            modname[mlen] = 0;
            off = (uintptr_t)strtoull(plus + 1, NULL, 0);
        } else {
            off = (uintptr_t)strtoull(lhs, NULL, 0);
        }
        for (p = rhs; *p && nb < sizeof bytes; ) {
            unsigned v;
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') ++p;
            if (!*p) break;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            if (eur_hex_byte(p, &v)) { bytes[nb++] = (unsigned char)v; p += 2; }
            else break;
        }
        if (nb) eur_apply_one(modname, off, bytes, nb, logf);
    }
    if (logf) fclose(logf);
    fclose(f);
}

/* Called from this board's DllMain, synchronously, into a still-SUSPENDED
 * process -- so patches land before the game's first instruction. */
void ultra_europar_patch_install(void) {
    char pf[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("ULTRA_PATCH_FILE", pf, sizeof pf)) return;
    if (!pf[0]) return;
    eur_apply_patch_file(pf);
}
