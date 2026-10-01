/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  rawthrills_g5_main.c -- Raw Thrills g5 module entry point (Cruis'n Blast).
 * =============================================================================
 *
 *  Raw Thrills g5 is a real, Lindbergh-shaped Linux ELF32 hardware generation
 *  (HASP dongle + FTDI RIO2/UHB I/O + OpenGL), distinct from the later
 *  Windows-Embedded-based Raw Thrills PC generation (Fast & Furious SuperCars,
 *  Jurassic Park Arcade -- see modules/rawthrills_pc/, which stays Windows).
 *  The only currently-known g5 title is Cruis'n Blast.
 *
 *  This module owns its OWN copy of the generic Linux-guest engine, ported
 *  from modules/lindbergh/src/runtime/ (the non-Lindbergh-board-specific
 *  files only) -- no shared compiled binary with lindbergh or any other
 *  module. Cruis'n Blast's own stub set (stubs_cruisn.c, FTDI RIO2/UHB
 *  synthetic TTY devices in ftdi_shim.c/vfs.c) is this board's real,
 *  game-specific I/O -- kept as this board's own copy, not a Lindbergh
 *  dependency, matching the per-board independence rule.
 *
 *  Previously, work/src/main.c special-cased Cruis'n Blast folders and
 *  routed them straight into ul_run_lindbergh() ("rides on the same ELF32 +
 *  dynlink pipeline") -- i.e. this title depended on Lindbergh's compiled
 *  DLL. That dependency is what this module replaces: main.c now calls
 *  ul_rawthrills_g5_run() instead (see main.c's dispatcher).
 *
 *  Folder layout Cruis'n Blast ships as (a disk-dump directory, not a bare
 *  .elf -- main.c's ul_folder_is_cruisn_blast() already knows this shape and
 *  resolves it to an absolute ELF path before calling here):
 *      <root>/game                    (the Linux i386 ELF; or under pm/)
 *      <root>/pm/game                 (alt layout)
 *      <root>/utils/run_game.sh       (Linux launch script, unused here)
 *      <root>/pm/utils/run_game.sh    (alt layout)
 *
 *  STATUS: scaffold. Generic engine + Cruis'n Blast's own FTDI/HASP stub
 *  layer are wired end-to-end. Content-gated -- no dump on hand this pass
 *  to verify the guest actually reaches a renderable frame.
 * =============================================================================
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "loader.h"
#include "patcher.h"

__declspec(dllexport) int ul_rawthrills_g5_run(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2 || !argv[1] || !*argv[1]) {
        loader_log(LOG_ERROR, "[rawthrills_g5] no game path (argc=%d)", argc);
        return 1;
    }
    const char *elf_path = argv[1];
    loader_log(LOG_INFO, "[rawthrills_g5] launching %s", elf_path);

    LoaderCtx *ctx = loader_ctx_create();
    if (!ctx) {
        loader_log(LOG_ERROR, "[rawthrills_g5] loader_ctx_create failed");
        return 1;
    }

    stubs_register_all(ctx);           /* glibc / C++ EH / zlib / Cruis'n Blast stub symbols */
    if (!syscall_trap_install(ctx)) {
        loader_log(LOG_ERROR, "[rawthrills_g5] syscall_trap_install failed");
        loader_ctx_destroy(ctx);
        return 1;
    }

    LoadedElf *exe = elf_load(ctx, elf_path);
    if (!exe) {
        loader_log(LOG_ERROR, "[rawthrills_g5] elf_load failed: %s", elf_path);
        syscall_trap_remove(ctx);
        loader_ctx_destroy(ctx);
        return 1;
    }
    loader_log(LOG_INFO, "[rawthrills_g5] ELF loaded and mapped");

    if (!elf_relocate_all(ctx)) {
        loader_log(LOG_ERROR, "[rawthrills_g5] elf_relocate_all failed");
        syscall_trap_remove(ctx);
        loader_ctx_destroy(ctx);
        return 1;
    }

    /* Patch GOT to prevent null-pointer crashes from unresolved PLT.
     *
     * Ported from modules/lindbergh's own copy of this pattern (see its
     * lindbergh_main.c, right after its own elf_relocate_all() call). This
     * module's dynlink.c already carries dynlink_install_null_got_traps()
     * (it walks the slotmap populated during relocation and arms every
     * still-null JMP_SLOT with a per-symbol trampoline that logs the exact
     * missing symbol name before ExitProcess(1)), but nothing in this file
     * was calling it -- a plain drift between the two "own copies" of the
     * same engine, not a deliberate omission. Left uncalled, any unresolved
     * PLT entry on this board (this container's manifest already expects the
     * I/O handshake to be the first one, since this cabinet's g6 I/O boards
     * are not in this g5-module's g5/Cruis'n-Blast-shaped name tables) would
     * crash at EIP=0 with no symbol name in the log instead of a clear
     * "NULL-GOT-CALL: <symbol>" line. Purely additive and safe: it only ever
     * touches slots that are still exactly zero after relocation. Lindbergh's
     * OWN extra step (2), a hardcoded GOT[2] fallback write at a fixed
     * address for one specific Lindbergh title, is deliberately NOT ported
     * here -- it is that other title's own memory layout, not board-generic. */
    {
        extern int dynlink_install_null_got_traps(void);
        int per_slot = dynlink_install_null_got_traps();
        loader_log(LOG_INFO, "[rawthrills_g5] NULL-GOT traps: %d slot(s) armed", per_slot);
    }

    /* Second, DIFFERENT hole in the same GOT: slots with NO relocation
     * entry at all, still holding a real address from the ORIGINAL Linux
     * box's shared libraries (prelink-style pre-resolution baked into the
     * file, discarding the relocation once solved). Confirmed 2026-09-24 on
     * Walking Dead: the null-GOT trap above only ever sees slots that had a
     * JMP_SLOT relocation AND resolved to exactly 0; this table's ~800
     * remaining stale slots have neither, so they were jumping straight
     * into unmapped memory (EIP in no module, EXECUTE-type access
     * violation) instead of tripping anything diagnostic. See dynlink.c's
     * dynlink_trap_stale_prelink_slots() for the full story. */
    {
        extern int dynlink_trap_stale_prelink_slots(LoaderCtx *ctx);
        int stale = dynlink_trap_stale_prelink_slots(ctx);
        loader_log(LOG_INFO, "[rawthrills_g5] STALE-PRELINK traps: %d slot(s) armed", stale);
    }

    loader_log(LOG_INFO, "[rawthrills_g5] relocations applied -- entering guest entry point");

    elf_execute(ctx, exe, argc - 1, argv + 1);

    loader_log(LOG_WARN, "[rawthrills_g5] guest entry point returned -- exiting");
    syscall_trap_remove(ctx);
    loader_ctx_destroy(ctx);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
