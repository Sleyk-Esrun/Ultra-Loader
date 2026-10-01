/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ultra_io_shared_es2plus.h  --  IPC contract: Ultra Loader (launcher) -> hook
 *                                 (namco_es2plus's OWN independent copy)
 * =============================================================================
 *
 *  The native game runs in its own process; this hook DLL lives INSIDE that
 *  process. If/when Ultra Loader (a separate process) maps controller state
 *  into a named file mapping for this board, this is the layout it uses --
 *  identical field-for-field to the contract every other board module shares,
 *  copied here (not included from another module's tree) so this board builds
 *  and ships with zero file dependency outside its own folder.
 *
 *  Transport: a named file mapping. If the loader creates one, it passes the
 *  name via the ULTRA_IO_SHM environment variable; the hook opens it in
 *  DllMain. Single-writer (loader) / single-reader (hook) for the input
 *  fields; the coin path is a pulse/ack pair so coin inserts are edge-
 *  triggered, never lost or doubled. When ULTRA_IO_SHM is unset (namco_es2plus
 *  is not yet wired to a per-frame loader input feed), the hook falls back to
 *  reading a local XInput gamepad directly so a standalone run is still
 *  playable.
 *
 *  Bit layouts match jvs_core_es2plus.h exactly (see ultra_jvs_input) so a
 *  future loader-side writer can fill these fields and the hook hands them
 *  straight to the engine.
 * =============================================================================
 */
#ifndef ULTRA_ES2PLUS_IO_SHARED_H
#define ULTRA_ES2PLUS_IO_SHARED_H

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
    uint8_t  system;       /* TEST/TILT bits (see jvs_core_es2plus.h)            */
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

#endif /* ULTRA_ES2PLUS_IO_SHARED_H */
