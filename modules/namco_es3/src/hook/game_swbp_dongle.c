/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_swbp_dongle.c -- STAR WARS: Battle Pod (namco_es3) HASP dongle
 *                        validation bypass (SWArcGame-Win64-Shipping.exe)
 * =============================================================================
 *
 *  WHAT THIS FIXES
 *  ---------------------------------------------------------------------------
 *  Star Wars Battle Pod protects itself with a Sentinel HASP SRM USB dongle,
 *  NOT the Namco USB dongle (that is a separate system handled by
 *  namco_dongle_es3.c). SWArcGame-Win64-Shipping.exe STATICALLY imports four
 *  functions BY ORDINAL from hasp_windows_x64_100610.dll:
 *
 *      ordinal 13 -> hasp_login       IAT 0x140f40238
 *      ordinal 15 -> hasp_read        IAT 0x140f40228
 *      ordinal  2 -> hasp_decrypt     IAT 0x140f40220
 *      ordinal 14 -> hasp_logout      IAT 0x140f40230
 *
 *  (A replacement hasp_windows_x64_100610.dll stub in the game's Binaries\Win64
 *  folder already answers all four with success; that is what got the game past
 *  "19-21 USB DONGLE ERROR 1" = login failure. This file fixes the NEXT wall,
 *  "19-23 USB DONGLE ERROR 3".)
 *
 *  GROUND TRUTH (Ghidra decompile + disassembly of THIS exe, not a guess)
 *  ---------------------------------------------------------------------------
 *  One function does the whole dongle check: FUN_1409dcc60 (RVA 0x9dcc60).
 *  It returns 0=OK, 1, 2, or 3; its caller FUN_1409dce20 maps those directly
 *  to "USB DONGLE ERROR 1/2/3" (return 3 -> the observed 19-23). Shape:
 *
 *      status = hasp_login(0xffff0000, vendor_code, &handle);
 *      switch (status) {
 *        case 0:
 *          hasp_read (handle, 0xfff0, 0xd00, 0x40, buf);   // 64 bytes -> buf
 *          hasp_decrypt(handle, buf, 0x40);                // decrypt in place
 *          // buf == the decrypted 64-byte dongle memory image (global
 *          //        DAT_141f07890)
 *          if (~buf[0x3e] != buf[0x3f]) return 3;          // GATE 1 (checksum)
 *          copy buf[0..12] -> caller's out buffer;
 *          if (buf[0..3] != "2740") return 2;              // GATE 2a (version)
 *          if (buf[4]     != param2 + '0') return 2;        // GATE 2b (index)
 *          return 0;                                        // SUCCESS
 *        case 7: case 0x1f:            return 1;            // -> ERROR 1
 *        case 0xb: case 0xe: case 0x16:return 2;            // -> ERROR 2
 *        default:                      return 3;            // -> ERROR 3
 *      }
 *
 *  With the DLL stub returning success + our previous XOR-0xA5 "decrypt", buf
 *  became all 0xA5, so ~buf[0x3e](0x5A) != buf[0x3f](0xA5) -> GATE 1 fails ->
 *  return 3 -> "19-23 USB DONGLE ERROR 3". Exactly the symptom.
 *
 *  We cannot satisfy GATE 2b generically: param2 is the caller's loop index
 *  (DAT_141ee7a04), which varies call-to-call, so no fixed dongle-memory value
 *  the stub could return would match every iteration. The correct, precise fix
 *  is therefore to neutralize the three conditional branches so the validator
 *  always reaches its own SUCCESS return once login/read/decrypt succeed
 *  (which the stub guarantees):
 *
 *      RVA 0x9dcd58  75 6b  jne (checksum gate -> was ERROR 3)  -> 90 90
 *      RVA 0x9dcd93  75 0d  jne (version 0..3  -> was ERROR 2)  -> 90 90
 *      RVA 0x9dcd9c  75 04  jne (version [4]   -> was ERROR 2)  -> 90 90
 *
 *  Each patch verifies a byte signature that includes the preceding compare
 *  instruction, so a different build (or the 32-bit hook loaded into a 32-bit
 *  game) simply skips rather than corrupting anything.
 *
 *  STRICT PER-GAME GATING
 *  ---------------------------------------------------------------------------
 *  This hook DLL is shared by every namco_es3 title (Pokken, Mario Kart GP DX,
 *  Tekken 7, ...). The constructor below fires the patch ONLY when the host
 *  process is SWArcGame-Win64-Shipping.exe (basename match, or exact file size
 *  24,058,368 bytes, or ULTRA_SWBP_ID=1 escape hatch). It can never touch any
 *  other game: a non-matching process returns before patching, and even if it
 *  somehow reached the patch, the signature check would not match another
 *  exe's bytes. Mirrors game_mkdx_iauth.c's identity-gate exactly.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

static void sw_log(const char* fmt, ...) {
    /* GetEnvironmentVariableA, not getenv: this constructor can run very early
     * in the hook DLL's load, before the CRT getenv table is reliably
     * populated (documented repeatedly elsewhere in this project). */
    static char path[MAX_PATH]; static int checked = 0;
    if (!checked) {
        DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", path, sizeof path);
        if (!(n > 0 && n < sizeof path)) path[0] = 0;
        checked = 1;
    }
    if (!path[0]) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[SW_HASP] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* Verify `siglen` bytes at base+rva equal `sig`, then overwrite `patchlen`
 * bytes at base+rva+patch_off with `patch`. Signature-first, so a mismatch is
 * a logged no-op rather than memory corruption (same discipline as
 * namco_dongle_es3.c's patch_bytes). Returns 1 if applied, 0 if skipped. */
static int sw_patch(BYTE* base, unsigned rva,
                    const BYTE* sig, size_t siglen,
                    unsigned patch_off, const BYTE* patch, size_t patchlen,
                    const char* tag) {
    BYTE* at = base + rva;
    if (memcmp(at, sig, siglen) != 0) {
        sw_log("PATCH %s: signature mismatch at rva 0x%X (base=%p) -- skipped",
               tag, rva, (void*)base);
        return 0;
    }
    BYTE* dst = at + patch_off;
    DWORD old;
    if (!VirtualProtect(dst, patchlen, PAGE_EXECUTE_READWRITE, &old)) {
        sw_log("PATCH %s: VirtualProtect failed (%lu) -- skipped", tag, GetLastError());
        return 0;
    }
    memcpy(dst, patch, patchlen);
    VirtualProtect(dst, patchlen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, patchlen);
    sw_log("PATCH %s: applied at rva 0x%X (%zu bytes)", tag, rva + patch_off, patchlen);
    return 1;
}

static int sw_is_battlepod(void) {
    const char* e = getenv("ULTRA_SWBP_ID");
    if (e && e[0] == '1') return 1;
    char p[MAX_PATH];
    if (!GetModuleFileNameA(NULL, p, MAX_PATH)) return 0;
    /* exact shipping-exe size (bytes) -- primary, survives a rename */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 24058368u)
        return 1;
    /* basename fallback */
    const char* base = strrchr(p, '\\');
    base = base ? base + 1 : p;
    if (_stricmp(base, "SWArcGame-Win64-Shipping.exe") == 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 *  XInput Start/Back mask
 * ---------------------------------------------------------------------------
 *  SWArcGame statically imports XINPUT1_3.dll (confirmed by objdump) and reads
 *  the pad through it DIRECTLY, in parallel with JVS and our DirectInput8
 *  emulation. That direct read is why clearing JVS START and the DI START button
 *  did not stop the game closing: pressing Xbox START reaches the game's own
 *  XInputGetState untouched, and the game treats START as EXIT. So we wrap the
 *  game's XInputGetState and strip START (0x0010) + BACK (0x0020) before it sees
 *  them. Everything else (sticks, triggers, A/B/X/Y) passes through, so any
 *  XInput-driven gameplay is unaffected. The controller is imported BY ORDINAL
 *  (no "XInputGetState" name string in the exe), so we match the IAT slot by the
 *  resolved function address, not by name. */
typedef DWORD (WINAPI *pXInputGetState)(DWORD, void*);
static pXInputGetState Real_XInputGetState = NULL;

static DWORD WINAPI Hook_XInputGetState(DWORD idx, void* state) {
    DWORD r = Real_XInputGetState ? Real_XInputGetState(idx, state)
                                  : (DWORD)1167 /* ERROR_DEVICE_NOT_CONNECTED */;
    if (r == 0 /* ERROR_SUCCESS */ && state) {
        /* XINPUT_STATE = { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; }
         * XINPUT_GAMEPAD begins with WORD wButtons at offset 4, then the two
         * trigger bytes (6,7) and the four stick shorts (8..15).
         *
         * Mask ONLY the buttons the game mishandles over its own XInput read:
         *   START 0x0010 -> quits the process
         *   BACK  0x0020 -> same quit path
         *   L3    0x0040 / R3 0x0080 -> null-deref crash (ACCESS_VIOLATION @0x2224)
         * Everything else (D-pad, LB/RB, A/B/X/Y) and ALL analog (triggers +
         * both sticks) pass through untouched, so the game keeps the XInput input
         * it had when the controls were working -- we only strip the 4 buttons
         * that exit or crash it. Zeroing ALL buttons (the previous approach) also
         * killed the game's own button handling, which is what went wrong. */
        WORD* btn = (WORD*)((BYTE*)state + 4);
        WORD bad = (WORD)(0x0010 | 0x0020 | 0x0040 | 0x0080);
        if (*btn & bad) {
            static int once = 0;
            if (!once) { once = 1;
                sw_log("XInput: masking START/BACK/L3/R3 only (exit+crash buttons); all others pass through"); }
            *btn &= (WORD)~bad;
        }
    }
    return r;
}

/* Redirect the game's XINPUT1_3.dll!XInputGetState IAT slot to Hook_XInputGetState.
 * Returns the number of slots patched. */
static int sw_hook_xinput(BYTE* base) {
    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = LoadLibraryA("xinput1_3.dll");
    if (!xi) { sw_log("XInput: xinput1_3.dll not present -- cannot hook"); return 0; }
    FARPROC real = GetProcAddress(xi, "XInputGetState");
    if (!real) { sw_log("XInput: GetProcAddress(XInputGetState) failed"); return 0; }
    Real_XInputGetState = (pXInputGetState)(void*)real;

    IMAGE_DOS_HEADER*  dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS*  nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) { sw_log("XInput: no import directory"); return 0; }

    IMAGE_IMPORT_DESCRIPTOR* imp =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + dd->VirtualAddress);
    int patched = 0;
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, "xinput1_3.dll") != 0) continue;
        void** thunk = (void**)(base + imp->FirstThunk);
        for (; *thunk; ++thunk) {
            if (*thunk == (void*)real) {
                DWORD old;
                if (VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &old)) {
                    *thunk = (void*)Hook_XInputGetState;
                    VirtualProtect(thunk, sizeof(void*), old, &old);
                    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(void*));
                    patched++;
                    sw_log("XInput: IAT hook installed on XInputGetState (slot %p)", (void*)thunk);
                }
            }
        }
    }
    if (!patched)
        sw_log("XInput: XInputGetState IAT slot not found -- game may not close cleanly on START");
    return patched;
}

/* ---------------------------------------------------------------------------
 *  Embedded control map -- the module IS the settings
 * ---------------------------------------------------------------------------
 *  The control map is baked into the module here, and the module writes it out
 *  and points ULTRA_CONTROLS_CONFIG at it during its own constructor. This makes
 *  the module self-contained: there is no separate .controls.ini staged next to
 *  it that can go missing or fall out of sync (which happened, dropping the game
 *  to default controls). One artifact, code + settings together.
 *
 *  Operator mapping:
 *    Right stick = bank (X ch0, Y ch1)   Left stick = accel/brake (raw XInput)
 *    L2 + R2 = fire/enter/start (0xC00)   Start = START   Select = coin
 *    L1 = service menu                    R1 = test menu
 */
static const char SWBP_CONTROLS_INI[] =
    "[meta]\n"
    "genre = flightstick\n"
    "[buttons]\n"
    "p1.push1   = xinput:0xC00\n"
    "p1.start   = xinput:start\n"
    "p1.service = xinput:lb\n"
    "p1.coin    = xinput:back\n"
    "sys.test   = xinput:rb\n"
    "[axes]\n"
    "p1.stick_x = xinput:rx 0 0 65535 0 8 linear\n"
    "p1.stick_y = xinput:ry 1 0 65535 0 8 linear\n";

static void sw_stage_embedded_controls(void) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof dir);
    if (n > 0 && n < sizeof dir)
        snprintf(path, sizeof path, "%s\\Ultra Loader\\swbp_controls.ini", dir);
    else
        snprintf(path, sizeof path, "swbp_controls.ini");   /* fallback: CWD */
    FILE *f = fopen(path, "wb");
    if (!f) { sw_log("controls: could not self-stage embedded map to %s", path); return; }
    fwrite(SWBP_CONTROLS_INI, 1, sizeof SWBP_CONTROLS_INI - 1, f);
    fclose(f);
    /* Point the board's control layer at OUR copy and force it active, so the
     * loader's separate staged file (present or not) is irrelevant. */
    SetEnvironmentVariableA("ULTRA_CONTROLS_CONFIG", path);
    SetEnvironmentVariableA("ULTRA_CONTROLS", "1");
    /* Disable the Mario Kart menu-axis fold for SWBP. That shared-code fold
     * forces JVS analog ch0 to an extreme whenever an UP/DOWN direction bit is
     * set -- and SWBP's LEFT stick (accel/brake, pushed up/down) sets those
     * bits, pinning ch0 and stopping the RIGHT stick from driving the menu
     * right. With it off, the right stick owns ch0 (menu left AND right) and
     * ch1 (bank up/down) cleanly. */
    SetEnvironmentVariableA("ULTRA_ES3_MENU_AXIS", "0");
    _putenv("ULTRA_ES3_MENU_AXIS=0");   /* fold reads getenv(); keep the CRT copy in sync */
    sw_log("controls: embedded map self-staged -> %s (module carries its own settings); "
           "MKGP menu-axis fold disabled for SWBP", path);
}

static const BYTE NOP2[2] = { 0x90, 0x90 };

/* Set to 1 once Star Wars Battle Pod is confirmed as the host process. Read by
 * jvs_serial_hook_es3.c's window/quit hooks so they can swallow the close for
 * this title only (the cab has no operator quit; the game exits itself on JVS
 * START at the title, and a loader-overlay focus change trips the same path).
 * Other namco_es3 titles keep the default behaviour. */
int g_ultra_swbp = 0;

static void __attribute__((constructor)) game_swbp_dongle_autoregister(void) {
    if (!sw_is_battlepod()) return;
    g_ultra_swbp = 1;

    /* Settings live IN the module: write the embedded control map and point the
     * control layer at it before that layer initializes. */
    sw_stage_embedded_controls();

    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) { sw_log("GetModuleHandleA(NULL) returned NULL -- cannot patch"); return; }

    sw_log("Star Wars Battle Pod detected -- neutralizing HASP dongle "
           "validation gates in FUN_1409dcc60 (base=%p)", (void*)base);

    int n = 0;

    /* GATE 1 -- checksum: cmp al,[rip 141f078cf]; jne 0x9dcdc5  (-> ERROR 3)
     *   0x9dcd52: 3A 05 77 AB 52 01 75 6B     (jne at +6) */
    {
        static const BYTE sig[] = { 0x3A,0x05,0x77,0xAB,0x52,0x01, 0x75,0x6B };
        n += sw_patch(base, 0x9dcd52, sig, sizeof sig, 6, NOP2, 2, "checksum_gate(ERR3)");
    }
    /* GATE 2a -- version[0..3]: cmp [rsi],ecx; jne 0x9dcda2  (-> ERROR 2)
     *   0x9dcd91: 3B 0E 75 0D                 (jne at +2) */
    {
        static const BYTE sig[] = { 0x3B,0x0E, 0x75,0x0D };
        n += sw_patch(base, 0x9dcd91, sig, sizeof sig, 2, NOP2, 2, "version0_3_gate(ERR2)");
    }
    /* GATE 2b -- version[4]/index: cmp [rsi+4],cl; jne 0x9dcda2  (-> ERROR 2)
     *   0x9dcd99: 3A 4E 04 75 04              (jne at +3) */
    {
        static const BYTE sig[] = { 0x3A,0x4E,0x04, 0x75,0x04 };
        n += sw_patch(base, 0x9dcd99, sig, sizeof sig, 3, NOP2, 2, "version4_gate(ERR2)");
    }

    sw_log("HASP dongle validation bypass: %d/3 gates neutralized "
           "(19-23 USB DONGLE ERROR 3 cleared when all 3 land)", n);

    /* The game reads the Xbox pad directly via XINPUT1_3 and exits on START.
     * Mask START/BACK out of its XInputGetState so pressing Start can no longer
     * close it. (JVS START and the DI START button are already suppressed.) */
    sw_hook_xinput(base);
}

#endif /* _WIN32 */
