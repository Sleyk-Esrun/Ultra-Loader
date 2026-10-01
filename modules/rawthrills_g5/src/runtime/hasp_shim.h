/*
 * hasp_shim.h
 *
 * Sentinel HL / HASP SRM SDK clean-room shim surface for Raw Thrills
 * Cruis'n Blast on the Lindbergh runtime.
 *
 * Cruis'n Blast ships as a HASPE_LINUX_LOADER_1.1-envelope-wrapped ELF32 that
 * dynamically dlopens libhasp_linux_x86_*.so and dlsym()'s the full public
 * Sentinel HL API surface (30 entry points, per the ELF's .dynstr).
 *
 * On top of the raw hasp_* SDK, Raw Thrills' own "Dongle*" wrapper (60 exports:
 * DongleInit, DongleEncrypt, DongleReadHLFeature, ...) calls into the HASP
 * layer. The wrapper's canonical crypto pattern is:
 *
 *      hasp_encrypt(session, blob, N);
 *      hasp_decrypt(session, blob, N);
 *      compare_to_original(blob, orig);
 *
 * -> a "return OK, blob unchanged" shim is NOT enough.  We MUST provide a real
 * round-trip stable cipher.  We implement AES-128-CBC with a deterministic
 * per-session key derived from HID + feature-id, so encrypt/decrypt is a true
 * involution and the wrapper's compare passes.
 *
 * The shim delivers:
 *   - 30-entry hasp_* SDK surface
 *   - AES-128-CBC round-trip (encrypt/decrypt inverse, stable across calls)
 *   - Deterministic 64-bit HID (0x1234ABCDCB5A57ULL by default)
 *   - 4-file memory model (MAIN, LICENSE, RO, RW) sized per SDK
 *   - Simulated RTC (host UTC -> hasp_get_rtc)
 *   - Envelope decrypt oracle (HASPE_LINUX_LOADER_1.1 -> hasp_envelope.c)
 *
 * DISPATCH: stubs_glibc.c's stub_dlopen_v2 detects "libhasp" in the filename
 * and returns hasp_shim_dlopen(); stub_dlsym_v2 asks hasp_shim_lookup() for
 * each requested symbol before falling through to the generic no-op.
 *
 * CLEAN-ROOM: this file re-implements only the PUBLIC API CONTRACT of the
 * Sentinel HL SDK as documented in the SafeNet developer guide + the OBSERVED
 * usage in the game ELF (dynstr surface + Raw Thrills' Dongle* wrapper call
 * pattern).  No SafeNet/Thales SDK header, no HASP source, no leaked keys.
 */

#ifndef HASP_SHIM_H
#define HASP_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* SDK type aliases (per the public Sentinel HL SDK surface)          */
/* ------------------------------------------------------------------ */

/* Session handle. The real SDK uses `hasp_handle_t = uint32_t`. */
typedef uint32_t hasp_handle_t;

/* Feature id (matches Raw Thrills' Feature 0x00000001 baseline). */
typedef uint32_t hasp_feature_t;

/* File id used by hasp_read/hasp_write (see 4-file memory model). */
typedef uint32_t hasp_fileid_t;

/* Size query result. */
typedef uint32_t hasp_size_t;

/* Return status - HASP_STATUS_OK == 0, everything else is an error. */
typedef int32_t hasp_status_t;

/* Time is a raw uint32_t seconds-since-2000 in the SDK; hasp_get_rtc + the
 * hasp_datetime_to_hasptime / hasp_hasptime_to_datetime pair convert. */
typedef uint32_t hasp_time_t;

/* ------------------------------------------------------------------ */
/* Return codes                                                        */
/* ------------------------------------------------------------------ */
#define HASP_STATUS_OK              0
#define HASP_MEM_RANGE              1
#define HASP_INV_PGM_NUM            2
#define HASP_INV_UPDATE_DATA        3
#define HASP_TS_DETECTED            4
#define HASP_INV_ACTION             5
#define HASP_INV_HND                6
#define HASP_INV_FILEID             7
#define HASP_OLD_DRIVER             8
#define HASP_NO_TIME                9
#define HASP_SYS_ERR                10
#define HASP_HASP_NOT_FOUND         11
#define HASP_TOO_SHORT              12
#define HASP_INV_HASPID             13
#define HASP_INV_INT                14

/* ------------------------------------------------------------------ */
/* Well-known file ids                                                 */
/* ------------------------------------------------------------------ */
#define HASP_FILEID_MAIN            0xFFF0
#define HASP_FILEID_LICENSE         0xFFF2
#define HASP_FILEID_RO              0xFFF4
#define HASP_FILEID_RW              0xFFF5

/* Default features (Raw Thrills only checks Feature 1). */
#define HASP_DEFAULT_FID            1

/* ------------------------------------------------------------------ */
/* 30 Sentinel HL SDK entry points                                    */
/* ------------------------------------------------------------------ */

/* Session lifecycle. */
hasp_status_t hasp_login(hasp_feature_t fid, const uint8_t *vendor_code,
                         hasp_handle_t *out_handle);
hasp_status_t hasp_login_ex(hasp_feature_t fid, const uint8_t *vendor_code,
                            const char *scope, hasp_handle_t *out_handle);
hasp_status_t hasp_login_port(hasp_feature_t fid, const uint8_t *vendor_code,
                              const char *port, hasp_handle_t *out_handle);
hasp_status_t hasp_login_scope(hasp_feature_t fid, const char *scope,
                               const uint8_t *vendor_code,
                               hasp_handle_t *out_handle);
hasp_status_t hasp_logout(hasp_handle_t handle);
hasp_status_t hasp_detach(const char *scope, const char *vendor_code,
                          const char *info, char **out_info);

/* Crypto. AES-128-CBC round-trip. Encrypt(x) then Decrypt returns x. */
hasp_status_t hasp_encrypt(hasp_handle_t handle, void *buffer, hasp_size_t len);
hasp_status_t hasp_decrypt(hasp_handle_t handle, void *buffer, hasp_size_t len);
hasp_status_t hasp_legacy_encrypt(hasp_handle_t handle, void *buffer,
                                  hasp_size_t len);
hasp_status_t hasp_legacy_decrypt(hasp_handle_t handle, void *buffer,
                                  hasp_size_t len);

/* Memory (4-file model). */
hasp_status_t hasp_read(hasp_handle_t handle, hasp_fileid_t fid,
                        hasp_size_t offset, hasp_size_t length,
                        void *out_buffer);
hasp_status_t hasp_write(hasp_handle_t handle, hasp_fileid_t fid,
                         hasp_size_t offset, hasp_size_t length,
                         const void *buffer);
hasp_status_t hasp_get_size(hasp_handle_t handle, hasp_fileid_t fid,
                            hasp_size_t *out_size);

/* Bulk transfer / update. */
hasp_status_t hasp_transfer(hasp_handle_t handle, const char *scope,
                            const char *format, const uint8_t *input,
                            hasp_size_t input_len, uint8_t **output,
                            hasp_size_t *output_len);
hasp_status_t hasp_update(const uint8_t *update, char **ack_info);

/* Info XML - well-formed empty document. */
hasp_status_t hasp_get_info(const char *scope, const char *format,
                            const uint8_t *vendor_code, char **out_info);
hasp_status_t hasp_get_sessioninfo(hasp_handle_t handle, const char *format,
                                   char **out_info);
hasp_status_t hasp_get_deviceinfo(const char *scope, const char *format,
                                  char **out_info);
hasp_status_t hasp_get_version(uint32_t *out_major, uint32_t *out_minor,
                               uint32_t *out_build, char **out_info);

/* Time. */
hasp_status_t hasp_get_rtc(hasp_handle_t handle, hasp_time_t *out_time);
hasp_status_t hasp_datetime_to_hasptime(uint32_t day, uint32_t month,
                                        uint32_t year, uint32_t hour,
                                        uint32_t minute, uint32_t second,
                                        hasp_time_t *out_time);
hasp_status_t hasp_hasptime_to_datetime(hasp_time_t t, uint32_t *out_day,
                                        uint32_t *out_month,
                                        uint32_t *out_year, uint32_t *out_hour,
                                        uint32_t *out_minute,
                                        uint32_t *out_second);

/* Legacy compat entry points from HASP HL. */
hasp_status_t hasp_legacy_set_idletime(hasp_handle_t handle, uint32_t minutes);
hasp_status_t hasp_legacy_set_rtc(hasp_handle_t handle, hasp_time_t t);
hasp_status_t haspregnew(void); /* legacy driver registration - always OK */

/* Misc. */
hasp_status_t hasp_echo(hasp_handle_t handle, void *buffer, hasp_size_t len);
hasp_status_t hasp_free(void *p);
hasp_status_t hasp_cleanup(void);
hasp_status_t hasp_enable_trace(const char *filename);
hasp_status_t hasp_get_trace(char **out_trace);
hasp_status_t hasp_vm_dyn_execute(hasp_handle_t handle, const void *code,
                                  hasp_size_t code_len);

/* Raw Thrills-specific extension used by the Dongle wrapper. Returns a stable
 * 64-bit hardware id. On real Sentinel HL the value comes from the dongle's
 * unique serial - here it is a compile-time constant derived from a "Cruis'n
 * Blast" bytestring. Deterministic so the game's CRC menu displays the same
 * value every boot. */
uint64_t SysGetDongleHID(void);

/* ------------------------------------------------------------------ */
/* Dispatcher glue (called by stubs_glibc.c dlopen/dlsym intercept)   */
/* ------------------------------------------------------------------ */

/* Return true if `filename` looks like a HASP shared object. */
int   hasp_shim_matches(const char *filename);

/* Return a fake handle for dlopen("libhasp*.so"). */
void *hasp_shim_dlopen(const char *filename);

/* Return one of the 30 shim entry points, or NULL if the symbol is unknown. */
void *hasp_shim_lookup(const char *symbol);

/* Envelope-decrypt oracle (HASPE_LINUX_LOADER_1.1). Called by elf_loader.c on
 * detection of the PATCHED_WITH_HASPE_LINUX_LOADER_1.1 marker in an ELF page.
 * Returns 0 on success (in-place decrypt), non-zero on failure. Implemented
 * in hasp_envelope.c. */
int hasp_envelope_decrypt(uint8_t *page, size_t page_len, uint64_t hid);

#ifdef __cplusplus
}
#endif

#endif /* HASP_SHIM_H */
