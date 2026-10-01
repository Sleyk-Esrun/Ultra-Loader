/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ultra_io_shared_pm.h -- IPC contract: Ultra Loader (launcher) -> Play
 *                          Mechanix hook (game). This board's OWN copy.
 * =============================================================================
 *
 *  The game runs in its own process; this hook DLL lives INSIDE that process.
 *  Ultra Loader (a separate process) maps its already-resolved controller /
 *  lightgun state into this shared block each frame, and the hook reads it to
 *  answer the game's JVS polls -- the same bridge every other Ultra Loader
 *  board uses between the loader's input mapping (gamepad/gun -> arcade
 *  switches/screen-position) and its own in-process JVS engine.
 *
 *  THIS FILE IS THE play_mechanix MODULE'S OWN INDEPENDENT COPY of the IPC
 *  struct layout (byte-identical to every other board's copy since it is a
 *  fixed wire contract the loader writes the same way for all of them, but a
 *  separate compiled TU/header with zero #include or link dependency on
 *  native_x86/src/hook/ultra_io_shared.h, per this project's per-board
 *  independence directive).
 *
 *  Transport: a named file mapping. The loader creates it and passes the name
 *  to the child via the ULTRA_IO_SHM environment variable; the hook opens it
 *  in DllMain. Single-writer (loader) / single-reader (hook) for the input
 *  fields; the coin path is a pulse/ack pair so coin inserts are edge-
 *  triggered, never lost or doubled.
 *
 *  Bit layouts match jvs_core_pm.h exactly (see ultra_pm_jvs_input) so the
 *  loader fills these fields and the hook hands them straight to the engine.
 * =============================================================================
 */
#ifndef ULTRA_IO_SHARED_PM_H
#define ULTRA_IO_SHARED_PM_H

#include <stdint.h>

#define ULTRA_IO_MAGIC    0x4F494C55u   /* 'ULIO' little-endian */
#define ULTRA_IO_VERSION  1u
#define ULTRA_IO_PLAYERS  2
#define ULTRA_IO_ANALOG   8
#define ULTRA_IO_COINS    2
#define ULTRA_IO_ENVVAR   "ULTRA_IO_SHM"

#pragma pack(push, 4)
typedef struct ultra_io_shared_pm {
    uint32_t magic;        /* ULTRA_IO_MAGIC -- hook validates before trusting   */
    uint32_t version;      /* ULTRA_IO_VERSION                                   */
    uint32_t seq;          /* loader bumps after each coherent input write       */
    uint32_t flags;        /* bit0: loader alive; bit1: hook attached (hook sets)*/

    /* --- input snapshot (loader writes, hook reads) --- */
    uint8_t  system;       /* TEST/TILT bits (see jvs_core_pm.h)                 */
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
} ultra_io_shared_pm;
#pragma pack(pop)

/* flags */
#define ULTRA_IO_FLAG_LOADER_ALIVE  0x1u
#define ULTRA_IO_FLAG_HOOK_ATTACHED 0x2u

#endif /* ULTRA_IO_SHARED_PM_H */
