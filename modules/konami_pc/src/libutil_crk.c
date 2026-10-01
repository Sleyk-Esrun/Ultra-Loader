/*
 * libutil_crk.c -- clean-room stub for Konami libutil.dll (KSHG Silent Hill: The Arcade).
 *
 * libutil.dll provides a self-contained input system (input_scan / input_update /
 * input_get_state) that reads from DirectInput / XInput directly, BYPASSING our
 * shaiolib stub.  The real libutil sees the Xbox controller's RT as the gun trigger
 * and reports trigger=1 continuously.  This stub silences all input_* functions so
 * only our shaiolib handles I/O.
 *
 * Non-input exports (config_*, eam_*, mouse_read, etc.) are stubbed with safe
 * no-ops; the game reads its config via shaiolib / command-line and treats missing
 * eam state as "no card reader" (offline acceptable).
 *
 * All functions use __cdecl (C default) so the caller cleans the stack regardless
 * of any parameter-count mismatch in signatures we reverse-inferred.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <string.h>

/* ── config ─────────────────────────────────────────────────────────────── */
__declspec(dllexport) int config_get_attr(const char *sect, const char *key,
                                          char *out, int out_sz) {
    (void)sect; (void)key;
    if ((size_t)out > 0x10000u && out_sz > 0) out[0] = '\0';
    return -1;
}
__declspec(dllexport) int  config_save(void) { return 0; }
__declspec(dllexport) int  config_set_attr(const char *sect, const char *key,
                                           const char *val) {
    (void)sect; (void)key; (void)val; return 0;
}

/* ── e-amusement (offline - no card reader) ─────────────────────────────── */
__declspec(dllexport) const char *eam_get_card_path(void)       { return ""; }
__declspec(dllexport) const char *eam_get_machine_id_path(void) { return ""; }
__declspec(dllexport) int eam_read_card(int p, void *buf, int sz) {
    (void)p; if ((size_t)buf > 0x10000u && sz > 0) memset(buf, 0, sz); return -1;
}
__declspec(dllexport) int eam_read_machine_id(void *buf, int sz) {
    if ((size_t)buf > 0x10000u && sz > 0) memset(buf, 0, sz); return -1;
}
__declspec(dllexport) int eam_set_card_path(const char *p)        { (void)p; return 0; }
__declspec(dllexport) int eam_set_machine_id_path(const char *p)  { (void)p; return 0; }

/* ── input - ALL NEUTRAL ─────────────────────────────────────────────────
 * The real libutil opens the DirectInput / XInput joystick and exposes
 * device-independent control indices.  Our stub pretends there are 0 devices
 * and returns 0 (not-pressed) for every control, so the Xbox controller's RT
 * can no longer leak through as P1 Trigger.  Gun aim and button input are
 * handled entirely by our shaiolib stub. */
__declspec(dllexport) int input_describe(int d, int c, char *buf, int sz) {
    (void)d; (void)c;
    if ((size_t)buf > 0x10000u && sz > 0) buf[0] = '\0';
    return 0;
}
__declspec(dllexport) int  input_get_binding(int d, int c)    { (void)d; (void)c; return -1; }
__declspec(dllexport) const char *input_get_name(int d)       { (void)d; return ""; }
__declspec(dllexport) int  input_get_ncontrols(int d)         { (void)d; return 32; }
__declspec(dllexport) int  input_get_ndevs(void)              { return 1; }
__declspec(dllexport) int  input_get_state(int d, int c, int *val) {
    (void)d; (void)c;
    if ((size_t)val > 0x10000u) *val = 0;  /* all controls = not pressed */
    return 0;
}
__declspec(dllexport) int  input_scan(void)                    { return 0; }
__declspec(dllexport) int  input_set_binding(int d, int c, int vk) {
    (void)d; (void)c; (void)vk; return 0;
}
__declspec(dllexport) int  input_unbind(int d, int c)         { (void)d; (void)c; return 0; }
__declspec(dllexport) int  input_update(void)                  { return 0; }

/* ── libutil lifecycle ───────────────────────────────────────────────────── */
__declspec(dllexport) int  libutil_init(void) { return 0; }
__declspec(dllexport) void libutil_fini(void) {}

/* ── mouse - return real cursor position for games that poll via here ────── */
__declspec(dllexport) int mouse_read(int *x, int *y, int *buttons) {
    POINT pt = {0};
    GetCursorPos(&pt);
    if ((size_t)x       > 0x10000u) *x       = (int)pt.x;
    if ((size_t)y       > 0x10000u) *y       = (int)pt.y;
    if ((size_t)buttons > 0x10000u) *buttons = 0;
    return 0;
}

/* ── debug ───────────────────────────────────────────────────────────────── */
__declspec(dllexport) void panic(const char *fmt, ...) { (void)fmt; /* no-op: don't exit */ }
__declspec(dllexport) void trace(const char *fmt, ...) { (void)fmt; }
