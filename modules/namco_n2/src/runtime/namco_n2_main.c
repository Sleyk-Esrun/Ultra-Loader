/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  namco_n2_main.c -- Namco System N2 module entry point.
 * =============================================================================
 *
 *  Real System N2 hardware is a genuine Linux PC (Supermicro C2SBM-Q, Core 2
 *  Duo, nForce2-class chipset era successor) running Arcade Linux (Debian
 *  4.0-based) -- NOT Windows. The correct shape for this board is the same
 *  ELF32 + dynlink guest-Linux-under-Windows pipeline this project already
 *  built for Sega Lindbergh, not a Windows DLL-injection hook (which this
 *  module used to be scaffolded as -- see _archive/old_namco_n2_es1_windows_
 *  hook_2026-08-02/ for the retired Windows-shaped attempt, kept for
 *  reference, not deleted).
 *
 *  This module owns its OWN copy of the generic Linux-guest engine (elf_load/
 *  dynlink/syscall_dispatch/threads/signals/etc., ported from
 *  modules/lindbergh/src/runtime/ -- the files with no Lindbergh-board-
 *  specific content, see each file's own header) -- no shared compiled binary
 *  with lindbergh or any other module, matching this project's per-board
 *  independence rule.
 *
 *  STATUS: scaffold. The generic engine (ELF load/relocate/execute, syscall
 *  trap, glibc/C++ stub layers) is real and wired end-to-end. Board-specific
 *  pieces are NOT yet built: N2's real rendering API (nForce2-era OpenGL,
 *  not Lindbergh's Cg/SegaAPI stack), N2's real audio API, and N2's real I/O
 *  protocol are all still open -- there is no dump on hand to reverse them
 *  against. This gets N2 to the same honest "boots the loader, content-
 *  gated" tier every other framework-only board in this project carries.
 * =============================================================================
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "loader.h"
#include "patcher.h"

__declspec(dllexport) int ul_namco_n2_run(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2 || !argv[1] || !*argv[1]) {
        loader_log(LOG_ERROR, "[namco_n2] no game path (argc=%d)", argc);
        return 1;
    }
    const char *elf_path = argv[1];
    loader_log(LOG_INFO, "[namco_n2] launching %s", elf_path);

    LoaderCtx *ctx = loader_ctx_create();
    if (!ctx) {
        loader_log(LOG_ERROR, "[namco_n2] loader_ctx_create failed");
        return 1;
    }

    stubs_register_all(ctx);           /* glibc / C++ EH / zlib stub symbols */
    if (!syscall_trap_install(ctx)) {
        loader_log(LOG_ERROR, "[namco_n2] syscall_trap_install failed");
        loader_ctx_destroy(ctx);
        return 1;
    }

    LoadedElf *exe = elf_load(ctx, elf_path);
    if (!exe) {
        loader_log(LOG_ERROR, "[namco_n2] elf_load failed: %s", elf_path);
        syscall_trap_remove(ctx);
        loader_ctx_destroy(ctx);
        return 1;
    }
    loader_log(LOG_INFO, "[namco_n2] ELF loaded and mapped");

    if (!elf_relocate_all(ctx)) {
        loader_log(LOG_ERROR, "[namco_n2] elf_relocate_all failed");
        syscall_trap_remove(ctx);
        loader_ctx_destroy(ctx);
        return 1;
    }
    loader_log(LOG_INFO, "[namco_n2] relocations applied -- entering guest entry point");

    /* Board-specific render/audio/I/O init would go here, before execute(),
     * once a real dump exists to reverse the N2 rendering/audio/JVS-over-USB
     * surface against. None of that exists yet -- this call is the honest
     * current ceiling: the guest will run until it hits the first syscall or
     * symbol this scaffold doesn't yet implement. */
    elf_execute(ctx, exe, argc - 1, argv + 1);

    /* elf_execute() does not return on success (it jumps into guest code and
     * the process lives inside the guest from here on); reaching this line
     * means the guest returned or the jump itself failed. */
    loader_log(LOG_WARN, "[namco_n2] guest entry point returned -- exiting");
    syscall_trap_remove(ctx);
    loader_ctx_destroy(ctx);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
