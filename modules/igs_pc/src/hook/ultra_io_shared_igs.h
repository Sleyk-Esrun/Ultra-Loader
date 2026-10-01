/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ultra_io_shared_igs.h  --  IPC contract: Ultra Loader (launcher) -> hook
 *                              (game). IGS PC board's OWN copy (self-contained).
 * =============================================================================
 *
 *  The native game runs in its own process; the I/O hook DLL lives INSIDE that
 *  process. Ultra Loader (a separate process) maps its already-resolved
 *  controller state into this shared block each frame, and the hook reads it to
 *  answer the game's JVS polls. This is the one bridge between the loader's input
 *  mapping (gamepad/stick/wheel/gun -> arcade switches/axes) and the in-process
 *  JVS engine.
 *
 *  Transport: a named file mapping. The loader creates it and passes the name to
 *  the child via the ULTRA_IO_SHM environment variable; the hook opens it in
 *  DllMain. Single-writer (loader) / single-reader (hook) for the input fields;
 *  the coin path is a pulse/ack pair so coin inserts are edge-triggered, never
 *  lost or doubled.
 *
 *  Bit layouts match jvs_core_igs.h exactly (see ultra_jvs_input) so the loader
 *  fills these fields and the hook hands them straight to the engine. This is a
 *  data FORMAT contract (a struct layout + an env-var name), kept byte-identical
 *  to the same header used by the rest of the Ultra Loader fleet so the loader's
 *  existing input-mapping code needs no board-specific branch -- it is NOT a
 *  compiled-code dependency on any other board's module (own copy, per the
 *  per-board isolation this whole tree follows).
 * =============================================================================
 */
#ifndef ULTRA_IGS_IO_SHARED_H
#define ULTRA_IGS_IO_SHARED_H

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
    uint8_t  system;       /* TEST/TILT bits (see jvs_core_igs.h)                 */
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

#endif /* ULTRA_IGS_IO_SHARED_H */
