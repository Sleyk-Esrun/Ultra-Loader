/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  controls.h -- Ultra Loader native_x86 player input-mapping layer (public API).
 * =============================================================================
 *
 *  The orchestrator that owns the per-frame ultra_intent, runs the active SOURCE
 *  backends to fill it from PHYSICAL input, applies the resolved board PROFILE, and
 *  renders the intent into ultra_jvs_input via the single sink adapter. It is the
 *  alternative producer for hook_dllmain.c's standalone branch:
 *
 *     if (ultra_input_map_active()) ultra_input_map_current(&in);  // real player input
 *     else                          in = g_xinput_in;              // legacy default
 *
 *  DEFAULT-OFF: the layer is dormant (active()==0) unless a board profile/config is
 *  resolved OR ULTRA_CONTROLS=1. With neither, the legacy gamepad/autopilot path
 *  runs unchanged, so the three playable games stay byte-identical.
 *
 *  CLEAN-ROOM: Ultra Loader's own code over the public Win32 input APIs.
 * =============================================================================
 */
#ifndef ULTRA_CONTROLS_H
#define ULTRA_CONTROLS_H

#include "input_intent.h"
#include "jvs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle (called from DllMain DLL_PROCESS_ATTACH) ------------------- *
 * Resolves the controls profile for the current board (ULTRA_BOARD, set by the
 * board profile) + loads any config file, then decides whether the layer is
 * active. Safe to call once; idempotent. */
void ultra_input_map_install(void);

/* 1 if the mapping layer should drive input (a profile/config resolved, or
 * ULTRA_CONTROLS=1); 0 = stay on the legacy producer. */
int  ultra_input_map_active(void);

/* Fill `out` with the player input the game should see, mapped from physical
 * sources via the active profile. Only meaningful when active()==1. Thread-safe
 * for the single-reader producer use in ultra_hook_current_input. */
void ultra_input_map_current(ultra_jvs_input* out);

/* The active profile's JVS output range for an analog channel (out_min/out_max
 * of whichever binding renders into it). Returns 1 if the channel is bound.
 *
 * ultra_hook_current_input uses this to put the loader's GENERIC published axis
 * (0..65535, 0x8000 center) into the range THIS game calibrates -- without it a
 * centered wheel lands 1232 counts left of a Wacky Race cabinet's real center and
 * the car pulls left forever. */
int  ultra_input_map_channel_range(int channel, uint16_t* omin, uint16_t* omax,
                                  int* bipolar);


/* The monotonic coin-insert count the mapping layer has produced for a slot. The
 * coin path in hook_dllmain pulls the delta and feeds jvs_insert_coin, exactly as
 * it does for the gamepad. */
uint32_t ultra_input_map_coin(int slot);

/* ---- internal seams used by controls.c + the unit test -------------------- */

/* The one sink adapter: render an intent into ultra_jvs_input using a profile's
 * axis calibration. Pure function, fully unit-testable (no OS deps). */
void intent_to_jvs_input(const ultra_intent* it, const board_binding* bind,
                         ultra_jvs_input* out);

/* Resolve a board profile by system_id (compiled-in default table). The config
 * loader may override individual bindings on top of this. */
const board_binding* board_binding_lookup(const char* system_id);
int                  board_binding_count(void);
const board_binding* board_binding_at(int index);

/* 1 if the last board_profiles_load_config() actually found + parsed a config FILE
 * on disk (as opposed to falling back to the compiled default). The orchestrator
 * uses this to keep the layer DEFAULT-OFF: a compiled default alone never activates
 * the layer; only ULTRA_CONTROLS=1 or a real config file does. */
int board_profiles_config_found(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ULTRA_CONTROLS_H */
