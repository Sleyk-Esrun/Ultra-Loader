/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ultra_io_shared.h  --  IPC contract: Ultra Loader (launcher) -> hook (game)
 *                         Sega RingEdge 2 board's OWN independent copy.
 * =============================================================================
 *
 *  The native game runs in its own process; the I/O hook DLL lives INSIDE that
 *  process. Ultra Loader (a separate process) maps its already-resolved
 *  controller state into this shared block each frame, and the hook reads it to
 *  answer the game's JVS polls. This is the one bridge between the loader's input
 *  mapping (gamepad/stick/wheel/gun -> arcade switches/axes) and the in-process
 *  JVS engine.
 *
 *  This is a physically independent copy of the same documented IPC layout used
 *  by the other Ultra Loader board modules (native_x86, taito_typex, taito_typex2)
 *  -- the field layout, magic number and env-var name are a cross-process wire
 *  contract with the loader itself, not native_x86 hook internals, so they are
 *  reproduced identically here (not #included from native_x86/src/hook/) so this
 *  board keeps interoperating with the same loader while staying build/link
 *  independent of any other board's hook.
 *
 *  Transport: a named file mapping. The loader creates it and passes the name to
 *  the child via the ULTRA_IO_SHM environment variable; the hook opens it in
 *  DllMain. Single-writer (loader) / single-reader (hook) for the input fields;
 *  the coin path is a pulse/ack pair so coin inserts are edge-triggered, never
 *  lost or doubled.
 *
 *  Bit layouts match jvs_core_ringedge2.h exactly (see ultra_jvs_input_ringedge2)
 *  so the loader fills these fields and the hook hands them straight to the
 *  engine.
 * =============================================================================
 */
#ifndef ULTRA_IO_SHARED_H
#define ULTRA_IO_SHARED_H

#include <stdint.h>

#define ULTRA_IO_MAGIC    0x4F494C55u   /* 'ULIO' little-endian */
#define ULTRA_IO_VERSION  1u
#define ULTRA_IO_PLAYERS  2
#define ULTRA_IO_ANALOG   8
#define ULTRA_IO_COINS    2
#define ULTRA_IO_ENVVAR   "ULTRA_IO_SHM"

#pragma pack(push, 4)
typedef struct ultra_io_shared {
    uint32_t magic;        /* ULTRA_IO_MAGIC -- hook validates before trusting   */
    uint32_t version;      /* ULTRA_IO_VERSION                                   */
    uint32_t seq;          /* loader bumps after each coherent input write       */
    uint32_t flags;        /* bit0: loader alive; bit1: hook attached (hook sets)*/

    /* --- input snapshot (loader writes, hook reads) --- */
    uint8_t  system;       /* TEST/TILT bits (see jvs_core_ringedge2.h)          */
    uint8_t  _pad0[3];
    uint16_t player[ULTRA_IO_PLAYERS];   /* switch bitfields, JVS wire order     */
    uint16_t analog[ULTRA_IO_ANALOG];    /* 16-bit left-justified axes           */
    uint16_t screen_x, screen_y;         /* light-gun position                   */

    /* --- coin path (edge-triggered) --- */
    uint32_t coin_pulse[ULTRA_IO_COINS]; /* loader increments to insert a coin   */
    uint32_t coin_ack[ULTRA_IO_COINS];   /* hook copies pulse after consuming it  */

    /* --- back-channel telemetry (hook writes, loader may read) --- */
    uint32_t jvs_address;  /* node address the game assigned (0 until SET_ADDR)  */
    uint32_t poll_count;   /* JVS packets the hook has answered (liveness)       */
    uint32_t reserved[8];
} ultra_io_shared;
#pragma pack(pop)

/* flags */
#define ULTRA_IO_FLAG_LOADER_ALIVE  0x1u
#define ULTRA_IO_FLAG_HOOK_ATTACHED 0x2u

/* reserved[3]: ANALOG OWNERSHIP MASK -- bit N set means the loader is driving
 * analog[N] from a real physical axis this tick; clear means the channel holds
 * only the neutral placeholder and carries no information.
 *
 * Without this a hook cannot tell "wheel centered" from "no wheel attached":
 * both arrive as 0x8000. The two want opposite handling. A centered wheel must
 * land in the MIDDLE of the game's own calibrated steering range (Wacky Race's
 * sv/WR0.CFG says iostmin=4000 iostmax=64000, so its center is 34000 -- feeding
 * a raw 0x8000=32768 instead is a permanent left bias that reads in-game as a
 * wheel stuck turning left). An unowned PEDAL channel, by contrast, must read
 * as RELEASED -- the range floor -- because the middle of a throttle range is
 * a half-pressed accelerator and the car creeps forward on its own.
 *
 * Purely additive: reserved[] was already in the layout and older hooks ignore
 * it, so a new loader talking to an old hook behaves exactly as before.
 * Word 3, not 0: reserved[0..2] are already a lightgun diagnostic mirror. */
#define ULTRA_IO_ANALOG_OWNED(blk)  ((blk)->reserved[3])


#endif /* ULTRA_IO_SHARED_H */
