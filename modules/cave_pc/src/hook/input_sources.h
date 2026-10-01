/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  input_sources.h -- SOURCE backend interface for the native_x86 controls layer.
 * =============================================================================
 *
 *  A SOURCE reads ONE class of physical input (keyboard, XInput, DirectInput8,
 *  mouse, Ultra Omni-Track gun, or the test autopilot) and fills the shared
 *  ultra_intent. It never touches ultra_jvs_input or any board sink -- the
 *  orchestrator (controls.c) renders the intent once. The orchestrator runs every
 *  AVAILABLE source in priority order; digital bits OR together, and analog uses
 *  last-active-writer-wins (a moved wheel overrides a synthesized key-steer).
 *
 *  Each source is one translation unit exporting a single ultra_input_source via
 *  its ultra_src_*_get() accessor, so controls.c can assemble the source list
 *  without pulling in the per-source statics.
 * =============================================================================
 */
#ifndef ULTRA_INPUT_SOURCES_H
#define ULTRA_INPUT_SOURCES_H

#include "input_intent.h"

#ifdef __cplusplus
extern "C" {
#endif

struct board_binding;

typedef struct ultra_input_source {
    const char* name;
    /* device present / usable? (XInput pad connected, DI device opened, ...).
     * Returns 0 to be skipped this frame; the orchestrator never errors on it. */
    int  (*available)(void);
    /* fill `out` from this source, using `bind` to know which physical code maps
     * to which intent. Accumulate into `out` (OR digital, ui_ax_drive analog). */
    void (*poll)(ultra_intent* out, const struct board_binding* bind);
    int  priority;   /* higher = its active analog overrides a lower source's */
} ultra_input_source;

/* one accessor per source TU (returns a pointer to its static descriptor). */
const ultra_input_source* ultra_src_keyboard_get(void);
const ultra_input_source* ultra_src_xinput_get(void);
const ultra_input_source* ultra_src_dinput_get(void);
const ultra_input_source* ultra_src_mouse_get(void);
const ultra_input_source* ultra_src_gun_get(void);
const ultra_input_source* ultra_src_autopilot_get(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ULTRA_INPUT_SOURCES_H */
