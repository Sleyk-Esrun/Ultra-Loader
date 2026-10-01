/* SPDX-License-Identifier: MIT
 * NOTE: independent namco_es3 board copy -- adapted from native_x86/src/hook/board_io_ops.c (reference only; no build-time or runtime dependency on native_x86 remains).
 * board_io_ops_es3.c -- registration + input-setter plumbing for board_io_ops.h.
 * See board_io_ops.h for the full rationale. This file is intentionally tiny
 * and 100% game-agnostic -- it must never grow a per-game branch.
 */
#if defined(_WIN32)
#include "board_io_ops.h"
#include <stdio.h>

static const board_io_ops* g_board_ops = NULL;

/* Provided by hook_dllmain.c; this file only forwards into them so a per-game
 * autopilot_tick can affect the same output the generic gamepad path feeds,
 * without every game_*.c file reaching into hook_dllmain.c's statics directly. */
extern void ultra_hook_set_local_input(const ultra_jvs_input* in);
extern void ultra_hook_local_coin_insert(int slot);

/* diagnostic log helper (mirrors the ULTRA_JVS_LOG convention used elsewhere
 * in this hook so ops registration shows up in the same trace). */
static void log_line(const char* fmt, const char* a) {
    const char* p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    fprintf(f, fmt, a);
    fputc('\n', f);
    fclose(f);
}

void ultra_board_ops_register(const board_io_ops* ops) {
    if (!ops) return;
    if (g_board_ops) {
        log_line("board_io_ops: IGNORED second register() attempt (already have '%s')",
                 g_board_ops->name ? g_board_ops->name : "?");
        return;
    }
    g_board_ops = ops;
    log_line("board_io_ops: registered '%s'", ops->name ? ops->name : "?");
}

const board_io_ops* ultra_board_ops_get(void) {
    return g_board_ops;
}

void ultra_board_ops_set_input(const ultra_jvs_input* in) {
    ultra_hook_set_local_input(in);
}

void ultra_board_ops_insert_coin(int slot) {
    ultra_hook_local_coin_insert(slot);
}

#endif /* _WIN32 */
