/* ---------------------------------------------------------------------------
 *  lindbergh_advanced_stubs.c -- Advanced Features symbols, stubbed.
 *
 *  WHY THIS FILE EXISTS.
 *
 *  settings.c is compiled into this runtime DLL so the module can show the
 *  loader's settings UI. When the Advanced Features panel landed, settings.c
 *  gained references to three of the loader's services: ULDP seating
 *  (ul_slotmap), the ULCM connected mesh (ul_mesh) and the Ultra UI kit
 *  (ul_ui). The module then would not link -- ~50 undefined references -- and
 *  for a while it simply stopped being produced at all, which is why every
 *  Lindbergh title reported "Sega Lindbergh module not installed".
 *
 *  Linking the REAL implementations fixed the link and broke the games: the
 *  module crashed on entry, before a single frame, on EVERY Lindbergh title
 *  (2026-09-19). Linking is not running.
 *
 *  None of those three services belongs in a game module. ULDP seating and the
 *  ULCM mesh are the LOADER's, owned and driven by the loader process; the UI
 *  kit paints the loader's settings window, which a module never shows. The
 *  module needs these symbols to EXIST, not to DO anything.
 *
 *  So each one is defined here as an inert stub with the header's own
 *  signature, returning a value that reads as "not available": 0 / NULL /
 *  "off". Any code path in a module that actually reaches one of these is a
 *  bug in that path, not something to fix by linking the real service.
 *
 *  IF YOU EVER NEED ONE FOR REAL: link that single TU, and re-test the module
 *  BY RUNNING A GAME. A clean link proves nothing here -- that is precisely the
 *  mistake this file was created to undo.
 *
 *  Generated from work/include/ul_mesh.h, ul_slotmap.h and ul_ui.h.
 * ------------------------------------------------------------------------- */

#include <windows.h>
#include <stddef.h>
#include "ul_mesh.h"
#include "ul_slotmap.h"
#include "ul_ui.h"

const char * ul_mesh_cab_id(void)
{
    return 0;
}

void ul_mesh_config_defaults(ul_mesh_config_t *cfg)
{
}

int ul_mesh_identity_init(const char *ini_dir)
{
    return 0;
}

int ul_mesh_identity_set_name(const char *name)
{
    return 0;
}

int ul_mesh_is_running(void)
{
    return 0;
}

void ul_mesh_last_error(char *out, size_t cap)
{
}

int ul_mesh_net_advance(void)
{
    return 0;
}

int ul_mesh_net_checksum_register(ul_mesh_net_checksum_fn fn, void *user)
{
    return 0;
}

uint32_t ul_mesh_net_frame(void)
{
    return 0;
}

int ul_mesh_net_inputs(uint32_t frame, ul_mesh_net_frame_t *out)
{
    return 0;
}

int ul_mesh_net_push_local(const uint32_t *words, int nwords, uint32_t *applied_frame)
{
    return 0;
}

int ul_mesh_net_ready(uint32_t frame)
{
    return 0;
}

int ul_mesh_net_start(const ul_mesh_net_opts_t *o)
{
    return 0;
}

int ul_mesh_net_status(ul_mesh_net_status_t *out)
{
    return 0;
}

int ul_mesh_net_stop(void)
{
    return 0;
}

int ul_mesh_peer_at(int i, ul_mesh_peer_t *out)
{
    return 0;
}

int ul_mesh_peer_count(void)
{
    return 0;
}

int ul_mesh_peers_matching(const char *board, const char *exe_sha256_hex, ul_mesh_peer_t *out, int max)
{
    return 0;
}

int ul_mesh_poll_event(ul_mesh_event_t *out)
{
    return 0;
}

int ul_mesh_session_accept(const uint8_t sid[UL_MESH_SID_BYTES])
{
    return 0;
}

int ul_mesh_session_create(const ul_mesh_session_opts_t *o, ul_mesh_session_info_t *out)
{
    return 0;
}

int ul_mesh_session_decline(const uint8_t sid[UL_MESH_SID_BYTES], int reason)
{
    return 0;
}

int ul_mesh_session_info(ul_mesh_session_info_t *out)
{
    return 0;
}

int ul_mesh_session_invite(uint64_t peer_uid)
{
    return 0;
}

int ul_mesh_start(const ul_mesh_config_t *cfg)
{
    return 0;
}

int ul_mesh_stats(ul_mesh_stats_t *out)
{
    return 0;
}

void ul_mesh_stop(void)
{
}

int ul_slotmap_answer_queue(int role, int index, int remember)
{
    return 0;
}

int ul_slotmap_answer_restore(int role, int index, int remember)
{
    return 0;
}

int ul_slotmap_claim_is_open(int role, int index)
{
    return 0;
}

int ul_slotmap_claim_open(int role, int index, unsigned window_ms)
{
    return 0;
}

int ul_slotmap_claim_report_xinput(int xinput_index, unsigned buttons)
{
    return 0;
}

int ul_slotmap_count(void)
{
    return 0;
}

int ul_slotmap_event_pop(ul_seat_event_t *out)
{
    return 0;
}

int ul_slotmap_hold_for(int role, int index, ul_slotmap_hold_t *out)
{
    return 0;
}

int ul_slotmap_ready(void)
{
    return 0;
}

void ul_slotmap_set_return_policy(int role, int policy)
{
}

void ul_slotmap_set_seat_mode(int role, int mode)
{
}

void ul_slotmap_tick(void)
{
}

int ul_ui_available(void)
{
    return 0;
}

HWND ul_ui_card_create(HWND parent, int x, int y, int w, int h)
{
    return 0;
}

int ul_ui_ctlcolor(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out)
{
    return 0;
}

int ul_ui_init(void)
{
    return 0;
}

HWND ul_ui_list_create(HWND parent, int x, int y, int w, int h, int id, const int *col_x, int ncols)
{
    return 0;
}

int ul_ui_list_sel(HWND list)
{
    return 0;
}

void ul_ui_list_set(HWND list, const UlUiRow *rows, int n)
{
}

void ul_ui_list_set_mono(HWND list, unsigned col_mask)
{
}

void ul_ui_set_host(const UlUiHost *host)
{
}

void ul_ui_shutdown(void)
{
}

int ul_ui_skin(HWND c, UlUiKind kind, unsigned flags)
{
    return 0;
}
