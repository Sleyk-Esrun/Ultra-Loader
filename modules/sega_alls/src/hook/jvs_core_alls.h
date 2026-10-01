/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_core_alls.h -- Sega ALLS board's OWN copy of the clean-room JVS engine
 * =============================================================================
 *
 *  This is a CLEAN-ROOM implementation of the JAMMA Video Standard (JVS) I/O
 *  protocol, written from the published open specification. It contains NO
 *  code from any other arcade loader project. It is a private
 *  copy owned by the sega_alls board module -- it is NOT the same compiled
 *  object as native_x86's jvs_core.c/.h (that file was read only as reference
 *  for the wire-format shape; this file is compiled ONLY into
 *  ultra_sega_alls_hook{32,64}.dll and never links against native_x86).
 *
 *  Sega ALLS's primary I/O transport is IO4-USB (see sega_io4_alls.c), which
 *  reads the cabinet input straight out of the `ultra_jvs_input` struct this
 *  header defines -- the wire codec (jvs_host_bytes) is kept here too so a
 *  future ALLS cabinet variant with a legacy JVS-over-serial connector can be
 *  wired up without pulling in any other board's code.
 * =============================================================================
 */
#ifndef ULTRA_JVS_CORE_ALLS_H
#define ULTRA_JVS_CORE_ALLS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JVS_ALLS_MAX_PLAYERS   2
/* Touch/slider ALLS titles (maimai DX / CHUNITHM) poll more analog/misc
 * channels than a 2-stick fighter; size generously so every polled channel
 * gets a real (centered) value instead of reading as a hard-deflected axis. */
#define JVS_ALLS_MAX_ANALOG    16
#define JVS_ALLS_MAX_COINS     2

typedef struct ultra_jvs_input {
    uint8_t  system;
    uint16_t player[JVS_ALLS_MAX_PLAYERS];
    uint16_t analog[JVS_ALLS_MAX_ANALOG];
    uint16_t screen_x, screen_y;      /* light-gun / touch position */
} ultra_jvs_input;

typedef struct ultra_jvs_node ultra_jvs_node;

ultra_jvs_node* jvs_alls_create(void);
void            jvs_alls_destroy(ultra_jvs_node* n);

void jvs_alls_set_id(ultra_jvs_node* n, const char* id);
void jvs_alls_set_features(ultra_jvs_node* n,
                           uint8_t players, uint8_t switches_per_player,
                           uint8_t coin_slots, uint8_t analog_channels,
                           uint8_t analog_bits, uint8_t gpo_slots,
                           uint8_t has_screen_pos);

void jvs_alls_set_input(ultra_jvs_node* n, const ultra_jvs_input* in);
void jvs_alls_get_input(const ultra_jvs_node* n, ultra_jvs_input* out);
void jvs_alls_insert_coin(ultra_jvs_node* n, int slot, int count);

size_t jvs_alls_host_bytes(ultra_jvs_node* n,
                           const uint8_t* in, size_t in_len,
                           uint8_t* out, size_t out_cap);

uint8_t  jvs_alls_address(const ultra_jvs_node* n);
uint32_t jvs_alls_coin_count(const ultra_jvs_node* n, int slot);

/* ---- Protocol constants (wire format) -------------------------------------- */
#define JVS_ALLS_SYNC        0xE0
#define JVS_ALLS_ESCAPE      0xD0
#define JVS_ALLS_BROADCAST   0xFF
#define JVS_ALLS_HOST_ADDR   0x00

#define JVS_ALLS_CMD_RESET        0xF0
#define JVS_ALLS_CMD_SET_ADDR     0xF1
#define JVS_ALLS_CMD_SET_COMMS    0xF2
#define JVS_ALLS_CMD_IOIDENT      0x10
#define JVS_ALLS_CMD_CMD_REV      0x11
#define JVS_ALLS_CMD_JVS_REV      0x12
#define JVS_ALLS_CMD_COMM_VER     0x13
#define JVS_ALLS_CMD_FEATCHK      0x14
#define JVS_ALLS_CMD_MAIN_ID      0x15
#define JVS_ALLS_CMD_READ_SWITCH  0x20
#define JVS_ALLS_CMD_READ_COIN    0x21
#define JVS_ALLS_CMD_READ_ANALOG  0x22
#define JVS_ALLS_CMD_READ_ROTARY  0x23
#define JVS_ALLS_CMD_READ_KEYCODE 0x24
#define JVS_ALLS_CMD_READ_SCRPOS  0x25
#define JVS_ALLS_CMD_READ_MISC    0x26
#define JVS_ALLS_CMD_RETRANSMIT   0x2F
#define JVS_ALLS_CMD_COIN_DEC     0x30
#define JVS_ALLS_CMD_GPO          0x32
#define JVS_ALLS_CMD_ANALOG_OUT   0x33
#define JVS_ALLS_CMD_CHAR_OUT     0x34
#define JVS_ALLS_CMD_COIN_INC     0x35
#define JVS_ALLS_CMD_GPO_BYTE     0x38

#define JVS_ALLS_STATUS_NORMAL    0x01
#define JVS_ALLS_STATUS_UNKNOWN   0x02
#define JVS_ALLS_STATUS_SUM_ERR   0x03
#define JVS_ALLS_STATUS_OVERFLOW  0x04
#define JVS_ALLS_REPORT_NORMAL    0x01
#define JVS_ALLS_REPORT_PARAM_ERR 0x02
#define JVS_ALLS_REPORT_DATA_ERR  0x03
#define JVS_ALLS_REPORT_BUSY      0x04

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ULTRA_JVS_CORE_ALLS_H */
