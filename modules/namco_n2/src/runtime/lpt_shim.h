/*
 * lpt_shim.h
 * Parallel-port (LPT) I/O shim - Cruis'n Blast (Raw Thrills Lindbergh).
 *
 * Cruis'n Blast talks to the Raw Thrills UHB (Universal Harness Board) over
 * a real PC LPT port: coin, start, service, test, and the operator front
 * panel. On the Lindbergh box the game issues `iopl(3)` (already returns 0
 * via stubs_cxx.c) then executes raw `IN AL, DX` / `OUT DX, AL` on the LPT
 * base register (default 0x378; overridable via the `-lpt<base>` argv).
 *
 * On the loader host those ring-3 port instructions raise
 * EXCEPTION_PRIV_INSTRUCTION. `lindbergh_boards.c::portio_veh` already traps
 * every IN/OUT for the security-board range; this shim extends that trap so
 * the LPT sub-range is answered by our synthetic UHB state instead of the
 * security board (kept isolated so Sega-board titles behave identically).
 *
 * Why this design over intercepting `outb`/`inb` symbols?
 *
 *   glibc `<sys/io.h>` inlines outb/inb as raw `out dx, al` /
 *   `in al, dx` assembly. They do NOT resolve through the PLT, so an
 *   LD_PRELOAD-style dlsym intercept cannot see them. The only reliable
 *   catch point is the CPU fault the instruction raises. The Lindbergh
 *   runtime already installs that VEH (portio_veh); adding an LPT branch
 *   to it costs nothing and reuses the exact decoder that already handles
 *   0x66 prefixes + imm8/imm16 forms.
 */

#ifndef ULTRA_LPT_SHIM_H
#define ULTRA_LPT_SHIM_H

#include <stdbool.h>
#include <stdint.h>

/* Standard PC LPT base ports. `lpt_shim_configure` accepts any of these plus
 * whatever the `-lpt<hex>` argv override supplies. */
#define LPT_BASE_LPT1  0x378
#define LPT_BASE_LPT2  0x278
#define LPT_BASE_LPT3  0x3BC

/* Configure the shim's active LPT base register. Call once during boot from
 * `rt_g5_init` / autoprofile after argv is parsed. `base==0` picks LPT1 default.
 * Returns the base that was actually installed. */
uint16_t lpt_shim_configure(uint16_t base);

/* Parse a `-lpt<hex>` style argv element (e.g. "-lpt378", "-lpt=0x278"). Returns
 * true and writes the parsed base to *out_base on match; false otherwise. */
bool lpt_shim_parse_argv(const char *arg, uint16_t *out_base);

/* Query: is `port` inside the currently-configured LPT window (base + 0..2)?
 * portio_veh calls this to decide whether to dispatch to LPT or fall through
 * to the security-board handler. */
bool lpt_shim_is_lpt_port(uint16_t port);

/* Read a byte from the LPT window. `port` is the *absolute* port the game
 * accessed (e.g. 0x379); the shim internally derives the base+offset (0=data,
 * 1=status, 2=control). Returns the byte to load into AL. */
uint8_t lpt_shim_read(uint16_t port);

/* Write a byte to the LPT window. Signals from the game write flow through
 * to the UHB device state (coin lockout, ticket dispenser, marquee lamps). */
void    lpt_shim_write(uint16_t port, uint8_t value);

/* Operator inputs - driven by the loader's Omni input services (Omni-Steer for
 * the wheel/pedals, Omni-Play for gamepad-mapped coin/start, or the on-screen
 * pause overlay). Setting these updates the byte returned by the next status
 * read at `base+1`.
 *
 * Bit layout (Raw Thrills UHB, active-low on the wire, active-high in this API
 * because the shim inverts on write to AL):
 *   0x01 coin1
 *   0x02 coin2
 *   0x04 start1
 *   0x08 start2
 *   0x10 service
 *   0x20 test
 *   0x40 (reserved)
 *   0x80 (reserved) */
void    lpt_shim_set_button(unsigned mask, bool pressed);
uint8_t lpt_shim_get_buttons(void);

/* Debug-only: number of IN/OUT the shim has answered since boot. Used by the
 * boot console to confirm the game actually talks to the port. */
unsigned lpt_shim_stats_reads(void);
unsigned lpt_shim_stats_writes(void);

#endif /* ULTRA_LPT_SHIM_H */
