/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ioemulation_gate_txp.c -- put Gundam Spirits of Zeon on its JVS GUN path
 * =============================================================================
 *
 *  THE SWITCH THIS FILE FLIPS IS ALL-OR-NOTHING. Read this before enabling it.
 *
 *  Gundam has a config option called `ioemulation`, default TRUE, which chooses
 *  between two COMPLETELY SEPARATE input schemes. The decision is made once, at
 *  Game.exe 0x4D100D, inside the input-init routine 0x4D0F70:
 *
 *      TRUE  -> registers five callbacks, among them
 *                 setMouseMotionFunction(0x4D0350)     <- the ONLY writer of P1 aim
 *                 setMouseButtonDownFunction(0x4D02F0) <- trigger
 *                 setMouseButtonUpFunction(0x4D0320)
 *               This is the scheme the game is PLAYABLE on today: the Omni-Bridge
 *               puts a pad or a gun on the cursor and the game reads the cursor.
 *
 *      FALSE -> jumps to 0x4D118A and registers the KEYBOARD callbacks only.
 *               The mouse callbacks are never installed at all. So this does not
 *               ADD a gun path alongside the working one - it DELETES the
 *               working one and commits to JVS.
 *
 *  Which is why it is gated, defaults OFF, and lives behind a container key.
 *  With ULTRA_TXP_JVS_GUNS unset this file installs nothing and the module is
 *  byte-for-byte the game everyone has been playing.
 *
 *  HOW THE FLIP IS DONE, AND WHY NOT BY PATCHING MEMORY.
 *
 *  The option byte is at *(BYTE**)0x64A614 + 0x14 - but 0x64A614 is a POINTER TO
 *  A STACK LOCAL of main(), published at 0x437A12 and dead when main() returns.
 *  There is no fixed address to patch, and a poll-then-write loses the race with
 *  the option reader. The value is authored by the game's own embedded INI text
 *  (no file on disk can change it: the text is handed to igSetAlchemyIniContents
 *  and served from memory), and read exactly once through
 *
 *      libIGUtils.dll ! igGetRegistryValue(igRegistry*, int, const char* name,
 *                                          bool* out, bool def, bool)
 *
 *  at Game.exe 0x42AE0F. Hooking that import is precise, name-matched, has a
 *  wide timing window, and needs no writes into the game's memory: we let the
 *  real function answer every other option and only override this one.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "hook_txp.h"

/* The exact decorated name in libIGUtils.dll's export table. The board's IAT
 * patcher matches the import name with strcmp, so this must be byte-exact. */
#define IG_GETREGVALUE_BOOL \
    "?igGetRegistryValue@Utils@Gap@@YA_NPAVigRegistry@Core@2@HPBDAA_N_N3@Z"

/* __cdecl, six dword arguments. The caller cleans the stack (add esp,0x18 per
 * call at the call site), so the signature only has to agree on the count. */
typedef int (__cdecl *pIgGetRegBool)(void* reg, int section, const char* name,
                                     unsigned char* out, int def, int arg6);

static pIgGetRegBool Real_igGetRegBool = NULL;

static int ultra_txp_gun_mode(void)
{
    /* A container may FORCE the mode for a cabinet that is always guns.
     * Absent that, the user owns it: "Gun cabinet (2 lightguns)" in the
     * loader settings and "Gun Cabinet (2P)" in the pause menu, both of
     * which relaunch the game because the choice is read once at startup. */
    const char* f = getenv("ULTRA_TXP_JVS_GUNS");
    if (f && *f) return f[0] == '1';
    {
        const char* u = getenv("ULTRA_GUN_CABINET");
        return (u && u[0] == '1') ? 1 : 0;
    }
}


static int __cdecl Hook_igGetRegBool(void* reg, int section, const char* name,
                                     unsigned char* out, int def, int arg6)
{
    /* Seven bool options come through this one import. Only ours is touched;
     * every other name is handed straight to the real function, unchanged. */
    if (name && strcmp(name, "ioemulation") == 0) {
        if (out) *out = 0;
        ultra_txp_log("IOGATE: ioemulation forced FALSE -> the game takes its JVS "
                      "gun path (mouse aim is now OFF by design)");
        return 1;                      /* "the option was present", value = false */
    }
    if (!Real_igGetRegBool) return 0;
    return Real_igGetRegBool(reg, section, name, out, def, arg6);
}

/* Install from DLL_PROCESS_ATTACH. libIGUtils.dll is a static import of
 * Game.exe, and the game re-reads this IAT slot when its option reader runs
 * (0x42AD46 loads it into ebp at run time), so an early patch is honored.
 * The same mechanism already carries this board's Direct3DCreate9 hook. */
void ultra_txp_install_iogate(void)
{
    if (!ultra_txp_gun_mode()) return;  /* default: change nothing at all */

    /* Resolve the REAL function from libIGUtils' EXPORT table, not through the
     * game's import table. Resolving through the IAT after patching it would
     * hand us our own hook and recurse until the stack dies - this project has
     * already lost a day to exactly that on every board's renderer wrappers. */
    {
        HMODULE h = GetModuleHandleA("libIGUtils.dll");
        if (!h) h = LoadLibraryA("libIGUtils.dll");
        if (h) Real_igGetRegBool =
            (pIgGetRegBool)(void*)GetProcAddress(h, IG_GETREGVALUE_BOOL);
    }
    if (!Real_igGetRegBool) {
        ultra_txp_log("IOGATE: libIGUtils!igGetRegistryValue not resolvable - "
                      "NOT hooking, the game stays on its mouse path");
        return;
    }
    ultra_iat_patch_all_txp("libIGUtils.dll", IG_GETREGVALUE_BOOL,
                            (void*)Hook_igGetRegBool);
    ultra_txp_log("IOGATE: hooked libIGUtils!igGetRegistryValue "
                  "(two-gun JVS cabinet mode armed)");
}

#endif /* _WIN32 */
