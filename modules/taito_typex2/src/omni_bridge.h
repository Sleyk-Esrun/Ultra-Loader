/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  omni_bridge.h -- Ultra Omni-Bridge: keyboard/mouse emulation from any
 *                    already-normalized input source (gamepad, lightgun).
 * =============================================================================
 *
 *  Reference implementation. Per this project's standing per-board
 *  independence rule, every board's own hook DLL carries its OWN copy of
 *  this file (adapted to that board's naming conventions) -- no board links
 *  against this file's compiled form directly. Config comes from the shared
 *  work/include/omni_client.h (omni_client_read_bridge()); this file only
 *  needs two board-supplied accessors so it stays independent of any one
 *  board's D3D hook / shared-memory struct layout.
 * =============================================================================
 */
#ifndef ULTRA_OMNI_BRIDGE_H
#define ULTRA_OMNI_BRIDGE_H

#include <windows.h>

/* Returns the current render/device window, or NULL if not known yet (the
 * bridge stays fully inert -- never grabs an arbitrary foreground window). */
typedef HWND (*ultra_bridge_window_fn)(void);

/* Returns 1 and fills *x, *y (0..65535 screen-normalized) and *trigger (0/1)
 * from a live, already-normalized lightgun pose (e.g. Ultra Omni-Track, read
 * from this board's own shared-memory block); returns 0 if no live pose is
 * present right now. Pass NULL if this board has no lightgun/pose source at
 * all -- the bridge then drives the cursor from a gamepad only. */
typedef int (*ultra_bridge_pose_fn)(int *x, int *y, int *trigger);

/* Start the bridge (idempotent). Spawns one poll thread that:
 *   - feeds the OS cursor + a mouse button from get_pose() when it reports a
 *     live pose (a real lightgun always wins over the gamepad for aiming);
 *   - otherwise, when config.gamepad_source is on, feeds the cursor from an
 *     XInput stick (config.stick_right selects left/right) and a button from
 *     A / the right trigger -- going fully inert (never touching the cursor)
 *     when neither a live pose nor a connected gamepad is present, so a real
 *     mouse is never fought;
 *   - taps config.vk_start / vk_coin / vk_service / vk_test on the gamepad's
 *     START / BACK / (LB) / (RB) button edges.
 * Two independent slots (pad index 0 and 1) map to player 1 (left mouse
 * button) and player 2 (left or right, per config.p2_button_right). */
void ultra_omni_bridge_install(ultra_bridge_window_fn get_window, ultra_bridge_pose_fn get_pose);

/* Stop the bridge and guarantee both OS mouse buttons are released. Safe to
 * call from DLL_PROCESS_DETACH; does not join the thread (avoids a loader-
 * lock deadlock), just signals stop and force-releases buttons. */
void ultra_omni_bridge_stop(void);

#endif /* ULTRA_OMNI_BRIDGE_H */
