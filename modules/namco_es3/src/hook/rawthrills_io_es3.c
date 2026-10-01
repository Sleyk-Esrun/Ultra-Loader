/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/rawthrills_io.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * rawthrills_io_es3.c -- Raw Thrills JammaUSB / USBWD / PortTalk stubs
 *
 * Raw Thrills PC-based cabs (TFAFSB, BBH, H2Overdrive, ...) talk to a
 * proprietary JammaUSB I/O board.  Without the board, the factory function
 * returns NULL and the game crashes writing to the NULL object.
 *
 * We patch the factory so it ALWAYS enters the success path (allocating a
 * game I/O object even when the USB device isn't present).  The device handle
 * stored in the object will be NULL, so actual reads return zeros - the game
 * sees "no buttons pressed / no axis movement", which is fine for bring-up.
 *
 * Gated by ULTRA_JAMMAUSB=1 (set by rawthrills_pc_main.c).
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern void ultra_d3d8_log(const char* fmt, ...);
#define dlog(...) ultra_d3d8_log("[RTIO] " __VA_ARGS__)

static unsigned image_entry_rva(BYTE* base) {
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return (unsigned)nt->OptionalHeader.AddressOfEntryPoint;
}

static void patch_bytes_rt(BYTE* addr, const BYTE* expected, const BYTE* patch,
                           size_t n, const char* tag)
{
    if (memcmp(addr, expected, n) != 0) {
        dlog("PATCH %s: sig mismatch at %p, skipped", tag, (void*)addr);
        return;
    }
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        dlog("PATCH %s: VirtualProtect failed (%lu)", tag, GetLastError());
        return;
    }
    memcpy(addr, patch, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    dlog("PATCH %s: applied %zu bytes at %p", tag, n, (void*)addr);
}

/* ---------- per-title patch profiles ---------- */

/* TFAFSB (The Fast and the Furious: Super Bikes)
 * sdaemon.exe entry RVA = 0x000F15D6, image base 0x00400000, no ASLR.
 *
 * Factory @ VA 0x4E6C47: 75 1A (jne +0x1A - skip if device found)
 *   -> change to EB 1A (jmp - always enter success path)
 *
 * This forces the I/O object to be allocated even when JammaUSB hardware
 * isn't present.  The device handle stored at [obj+0x30] will be NULL;
 * runtime reads return all-zeros (no input).
 */
#define TFAFSB_ENTRY_RVA  0x000F15D6u

static void patch_tfafsb(BYTE* base) {
    /* Factory conditional jump: jne -> jmp */
    {
        static const BYTE exp[] = { 0x75, 0x1A };
        static const BYTE pat[] = { 0xEB, 0x1A };
        patch_bytes_rt(base + 0x0E6C47, exp, pat, 2, "jammausb_factory");
    }
}

/* H2Overdrive - TODO: add entry RVA + patch offsets when tested */
/* Big Buck Hunter - TODO: add entry RVA + patch offsets when tested */

void ultra_rawthrills_io_install(void) {
    {
        const char* e = getenv("ULTRA_JAMMAUSB");
        if (!e || e[0] != '1') return;
    }

    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) return;
    unsigned aoe = image_entry_rva(base);

    if (aoe == TFAFSB_ENTRY_RVA) {
        dlog("detected TFAFSB (entry RVA %08X), applying JammaUSB factory patch", aoe);
        patch_tfafsb(base);
    } else {
        dlog("unknown Raw Thrills title (entry RVA %08X), no patches available", aoe);
    }
}

#endif /* _WIN32 */
