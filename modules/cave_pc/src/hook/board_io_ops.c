/* SPDX-License-Identifier: MIT
 * board_io_ops.c -- registration + input-setter plumbing for board_io_ops.h.
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

/* Reusable serial "burst-then-quiet" gate -- see board_io_ops.h for the full
 * rationale (generalized from the Cave PC / Deathsmiles II coin-reader
 * starvation fix). Pure: returns 1 for the last `gap` reads of every `cycle`
 * reads (so the caller should return an empty read then), 0 otherwise. Bad
 * args -> 0 (never gates), so a mis-config can't accidentally starve I/O. */
int ultra_serial_burst_quiet(unsigned count, int cycle, int gap) {
    if (cycle <= 0 || gap <= 0 || gap >= cycle) return 0;
    return ((count % (unsigned)cycle) >= (unsigned)(cycle - gap)) ? 1 : 0;
}

void ultra_board_ops_set_input(const ultra_jvs_input* in) {
    ultra_hook_set_local_input(in);
}

void ultra_board_ops_insert_coin(int slot) {
    ultra_hook_local_coin_insert(slot);
}

#endif /* _WIN32 */
