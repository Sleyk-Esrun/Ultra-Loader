/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  board_io_ops.h -- per-game hook indirection seam (the cross-contamination fix)
 * =============================================================================
 *
 *  BACKGROUND: every "modern arcade" board module (Cave PC, Konami PC, Sega
 *  Europa-R, Taito Type X/X2, ...) is built from ONE shared source tree
 *  (native_x86/src/hook/) and, historically, one shared compiled DLL. Real
 *  per-game hacks (Cave PC/Deathsmiles II's CVKEY handling, Chaos Breaker's
 *  gamestate pokes, Wacky Race's autopilot) were written directly INLINE inside
 *  otherwise-generic dispatcher functions (Hook_ReadFile, Hook_WriteFile,
 *  xinput_thread, ...), gated by env-var checks like `cave_io_active()`. Because
 *  those branches physically lived in the SAME shared .c files/.o/.dll as every
 *  other board, a fix for one game could -- and did -- regress another (Konami
 *  PC's build script had to pin to a frozen historical snapshot of the shared
 *  hook DLL to dodge exactly this).
 *
 *  THE FIX: generic dispatchers call through AT MOST ONE registered board_io_ops
 *  instance instead of branching inline on a per-game env flag. Each game's
 *  logic lives in its OWN small .c file (game_dsii.c, game_chaosbreaker.c, ...)
 *  that a board module's build script links in ONLY if that module actually
 *  targets that game. A board that doesn't link a given game_*.c file cannot
 *  possibly run its code or be affected by a bug in it -- the isolation is
 *  enforced at the SOURCE/BUILD level, not just by a runtime env check.
 *
 *  Registration: each game_*.c file exports its OWN uniquely-named autoregister
 *  function (e.g. game_dsii_autoregister, game_chaosbreaker_autoregister) marked
 *  `__attribute__((constructor))`, so it runs automatically at DLL load time --
 *  before the CRT calls the exported DllMain -- with NO explicit call site
 *  needed anywhere in the shared generic files. Each one decides FOR ITSELF
 *  whether the host process is its target game (typically via the existing
 *  exe-identity-sniff pattern: AddressOfEntryPoint/size, with an explicit
 *  ULTRA_* env override as an escape hatch) and calls ultra_board_ops_register()
 *  if matched. A UNIQUE function name per file is required: a board's build may
 *  link MORE THAN ONE game_*.c file into the SAME binary (e.g. the shared legacy
 *  hook DLL still serves both Chaos Breaker's and Wacky Race's boards today), so
 *  a single shared well-known symbol name (the earlier design of this header)
 *  is a link-time collision the moment two game modules share a binary -- do
 *  NOT reintroduce one. If a board module links NO game_*.c file (e.g. any
 *  board with no per-game hack yet), no constructor runs, ultra_board_ops_get()
 *  stays NULL, and every generic dispatcher below behaves exactly as it does
 *  today -- byte-identical, nothing regresses.
 *
 *  At most one ops struct is ever active in a given injected process (one game
 *  per process). Registering a second one is a caller error (logged, first wins).
 * =============================================================================
 */
#ifndef ULTRA_BOARD_IO_OPS_H
#define ULTRA_BOARD_IO_OPS_H

#if defined(_WIN32)
#include <windows.h>
#include "jvs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct board_io_ops {
    /* For logging only. */
    const char* name;

    /* ---- lifecycle --------------------------------------------------------
     * Called once from ultra_jvs_serial_install(), AFTER the generic Real_*
     * kernel32/COM pointers are resolved. Use this to spawn game-specific
     * threads (keyboard capture, direct-memory-write pokes), install byte
     * patches/NOP guards, etc. May be NULL. */
    void (*on_install)(void);

    /* ---- file / serial interception ---------------------------------------
     * Each callback returns 1 if it fully handled the call (generic dispatcher
     * stops there) or 0 to fall through to the existing generic path. All may
     * be NULL (treated as "never handles it" / always falls through). */

    /* CreateFileA/W: a chance to intern a synthetic handle for a path this
     * game cares about (e.g. Cave PC's CVKEY.222). Checked BEFORE the generic
     * fake-COM/dongle/D-drive branches, since a game-specific path always
     * wins over a generic guess. `path` is always the narrow (ANSI-converted)
     * form even when called from the W hook. */
    int (*on_create_file)(const char* path, HANDLE* out_handle);

    /* CreateFileA only: same contract as on_create_file, but checked BEFORE
     * even the cabinet-drive redirect (ultra_redirect_cabinet_path). May be
     * NULL (the overwhelmingly common case -- every existing board is
     * byte-identical without this).
     *
     * WHY THIS EXISTS (Omatsuri Yasan Kingyo Sukui, 2026-09-24): on_create_file
     * is documented above as running "BEFORE the generic ... D-drive
     * branches", but the actual dispatcher in jvs_serial_hook.c checks the
     * cabinet redirect FIRST -- a path on a redirect-eligible drive letter
     * (default list "DEFGHJ") never reaches on_create_file at all. Kingyo
     * Sukui needs to block every "rqrdlist.rqr" open regardless of which
     * drive letter the game happens to probe (it scans C: through Z:), so it
     * needs to see the path before the redirect can serve it a real file out
     * of ULTRA_GAMEDATA. Reordering the shared dispatcher for every board
     * risked regressing Deathsmiles II's already-working CVKEY.222 handling
     * on E:/F: (both in the default redirect list) for an effect nobody had
     * verified was safe, so this is a new opt-in seam instead: NULL for every
     * board that doesn't set it (including DSII), wired up ONLY for Kingyo
     * Sukui's ops struct. See kingyo_on_create_file in game_cave_omatsuri.c. */
    int (*on_create_file_pre_redirect)(const char* path, HANDLE* out_handle);

    /* ReadFile: return 1 if `buf` was filled and *out_read set (a fully
     * synthetic response for this handle); 0 = fall through to the generic
     * queued-response path. */
    int (*on_read_file)(HANDLE h, void* buf, DWORD want, DWORD* out_read);

    /* WriteFile: return 1 if this write was fully handled (any response the
     * game expects was queued internally by the callback); 0 = fall through
     * to jvs_host_bytes(), the generic JVS-over-serial engine. */
    int (*on_write_file)(HANDLE h, const void* buf, DWORD len);

    /* Notified on EVERY CloseHandle in the process, so a game-specific
     * sentinel can be cleared (e.g. Cave PC's CVKEY handle-recycling fix).
     * Must no-op cleanly on handles it doesn't own. Not a 1/0 return -- this
     * is always additive, never a replacement for the generic close. */
    void (*on_close_handle)(HANDLE h);

    /* Return 1 if EV_RXCHAR should be reported even with no queued bytes
     * (e.g. Cave PC's push-mode protocol has no pre-queued data to signal). */
    int (*on_wait_comm_event_pending)(void);

    /* Return a redirect path for a bare-name LoadLibrary call, or NULL for no
     * redirect (e.g. Cave PC's dinput8.dll -> game-folder-copy redirect).
     * Returned pointer must stay valid for the process lifetime (a static
     * buffer is fine, matches the existing pattern). */
    const char* (*on_load_library_redirect)(const char* name);

    /* ---- input autopilot ---------------------------------------------------
     * If non-NULL, the generic xinput_thread calls this EVERY TICK instead of
     * its own gamepad-polling body. The callback is responsible for calling
     * ultra_board_ops_set_input()/ultra_board_ops_insert_coin() itself with
     * whatever it wants the game to see this tick; the generic loop still owns
     * pacing (Sleep) and shutdown (g_stop). This is how Wacky Race's and Chaos
     * Breaker's autopilot scripts move out of the shared thread body. */
    void (*autopilot_tick)(void);
} board_io_ops;

/* ---- reusable serial "burst-then-quiet" gate --------------------------------
 * Generalized 2026-08-13 from the Cave PC / Deathsmiles II credit-gate fix.
 *
 * PROBLEM CLASS: some arcade games poll an emulated serial/COM device in a
 * TIGHT INNER LOOP that only exits when a read returns 0 bytes. If our hook
 * hands back a valid frame on EVERY read (the natural thing for a push-mode
 * board whose on_read_file always synthesizes fresh input), that inner loop
 * NEVER exits -- and any OTHER periodic reader the game runs on the same thread
 * (a coin/credit poller, a status refresh, a watchdog) is STARVED and never
 * runs. Deathsmiles II's coin reader `input_poll` never executed for months
 * for exactly this reason; teaching the hook to go quiet between bursts (like
 * real hardware) let it run and coins finally credited. See game_dsii.c.
 *
 * USAGE: a board's on_read_file calls this at the top with its own per-device
 * read counter; if it returns 1, return an EMPTY read (*out_read = 0, return 1)
 * for THIS call instead of a frame, so the game's read loop exits and its other
 * pollers get a turn. `cycle`/`gap` shape the burst: cycle=8, gap=2 => 6 frames
 * then 2 empty reads, repeating. Pure function -- does NOT touch the counter
 * (the board owns/increments it).
 *
 * OPT-IN and SAFE: a board that never calls this is byte-identical to before.
 * ONLY apply it to games that show the read-until-empty inner-loop hang/starve
 * symptom -- a game that expects an uninterrupted stream may treat empty reads
 * as an error/timeout, so do NOT enable it blanket-wide. */
int ultra_serial_burst_quiet(unsigned count, int cycle, int gap);

/* Register this process's ops (idempotent no-op if one is already registered;
 * logs a warning in that case -- two games in one process is a bug). */
void ultra_board_ops_register(const board_io_ops* ops);

/* NULL if no game-specific ops registered (the common case -- every generic
 * dispatcher must treat NULL as "run the plain generic path", never crash). */
const board_io_ops* ultra_board_ops_get(void);

/* Setters an autopilot_tick implementation uses instead of touching
 * hook_dllmain.c's g_xinput_in/g_local_coin globals directly -- keeps actual
 * write-access to those centralized even though the VALUES are decided by a
 * per-game file. */
void ultra_board_ops_set_input(const ultra_jvs_input* in);
void ultra_board_ops_insert_coin(int slot);

/* NOTE: there is deliberately no single ultra_board_ops_autoregister()
 * declared here anymore -- see the registration section of the file comment
 * above. Each game_*.c file's own uniquely-named, constructor-attributed
 * function is private to that file (no shared header declaration needed). */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _WIN32 */
#endif /* ULTRA_BOARD_IO_OPS_H */
