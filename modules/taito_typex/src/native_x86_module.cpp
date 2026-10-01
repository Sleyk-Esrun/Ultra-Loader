/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  native_x86_module.cpp  --  Ultra Loader Native x86 Arcade Board Module (v1)
 * =============================================================================
 *
 *  This is the module that BEATS THE LAUNCHER APPROACH on its home turf. another loader is,
 *  fundamentally, a *loader* for PC-based arcade hardware -- not an emulator. It
 *  runs native game executables from boards that are just x86 PCs (Windows or
 *  Linux) behind an I/O board and DRM, mapping the proprietary I/O to commodity
 *  peripherals and patching out the bits that stop the game running on a normal
 *  PC. Ultra Loader IS a native loader (ELF on Windows via Win32 syscall
 *  shimming), so this is exactly its wheelhouse -- no emulation required.
 *
 *  Unlike the emulator modules (flycast/fbneo/mame/chihiro/triforce = cycle-
 *  stepped cores; namco_ps2/namco_ps3 = hooked launch-and-run emulators), this
 *  module RUNS THE NATIVE GAME BINARY directly through the host's loader. It
 *  implements the same ultra_module.h so the host treats every board uniformly.
 *
 *  Boards covered (the modern PC-based arcade catalog, 2003-2020+):
 *    SEGA:   Lindbergh*, Nu / Nu 1.1 / Nu 2 / Nu SX, RingEdge, RingEdge 2,
 *            RingWide, Europa-R, ALLS (ALL.S UX), SEGA PC-based
 *    NAMCO:  System N2, System ES1, ES2 Plus, ES3 (A/B/X), ES4, BNA1 / BNA1 Lite
 *    TAITO:  Type X, X+, X2, X3, X4, Zero, NESiCAxLive / NESiCAxLive2
 *    OTHER:  RAW Thrills PC, Konami PC-based, various PC-based
 *  (*Lindbergh is Linux/x86 -- Ultra Loader's native ELF-loading path is the
 *   natural fit; the other Windows boards run as native PE under the host.)
 *
 *  OS per board family (verified): Namco N2/ES1 = Debian Linux; ES2 Plus/ES3 =
 *  Windows Embedded 7; BNA1 = Windows 10 IoT; Taito Type X family = Windows;
 *  SEGA Lindbergh = Linux; Nu/RingEdge/RingWide/Europa-R/ALLS = Windows.
 *
 *  WHAT THIS MODULE DOES (and does NOT do):
 *    * It LAUNCHES the board's native game executable via the host loader,
 *      applying the board profile (I/O mapping, DRM/dongle and system-message
 *      bypass, JVS/touch/wheel/gun input routing).
 *    * It does NOT ship any game, dongle dump, or board firmware. Those are
 *      user-supplied (the games come from arcade hardware dumps the user owns),
 *      exactly as another loader requires. Nothing copyrighted is bundled.
 *    * Determinism: native PC games are not deterministic cores. Like the
 *      hooked emulator modules, determinism/replay can be provided by the
 *      external PROCESS hook (read/write process memory, intercept input,
 *      gate frames) -- caps are advertised based on whether the hook is
 *      attached for a given board, else the honest "it just runs".
 * =============================================================================
 */

#include "ultra_module.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include "../hook/ultra_io_shared.h"   /* loader<->hook input bridge ABI */
#endif

/* UNIFIED boot console + system+title catalog (shared with the 32-bit loader).
 * These are plain C; the module declares them extern "C" so C linkage matches
 * the .o the module build compiles from ../../../work/src. */
extern "C" {
#  include "boot_console.h"
#  include "ultra_catalog.h"
}

/* Unified clean-room board-profile framework (board_profile.{c,h}). Resolving a
 * board id -> the present-hook + I/O + injection + protection-handler config, and
 * setting the ULTRA_* env knobs the injected hook reads on attach. Compiled into
 * this module DLL (see build_module.sh) so select_board() can arm a profile. The
 * header wraps its decls in extern "C", so C linkage matches the C-built object. */
#include "../hook/board_profile.h"

namespace {

/* Arcade I/O the boards expose, mapped to commodity peripherals by the host:
 * JVS digital + analog, plus wheel/gun/touch axes. */
constexpr uint32_t ARC_BUTTONS = 18;
const char* const ARC_BUTTON_NAMES[ARC_BUTTONS] = {
    "up","down","left","right","b1","b2","b3","b4","b5","b6",
    "start","coin","service","test","wheel","pedal","gun_x","gun_y"
};

/* HONEST CAPS. A native PC game has no core save-state contract, and the
 * external process hook does NOT create one. With the hook attached we get
 * genuine process memory peek/poke (vt_read_memory / vt_write_memory), so we
 * advertise READ_MEMORY | WRITE_MEMORY and nothing else. We deliberately do
 * NOT advertise:
 *
 *   ULTRA_CAP_ROLLBACK -- MEASURED IMPOSSIBLE for a live multi-threaded Win32
 *     process. See work/include/home_state.h:419-452. Two independent walls,
 *     neither reachable by more memory bookkeeping: (1) HALF OF A LOCK IS NOT
 *     MEMORY -- a critical section's counters live in memory and get rolled
 *     back, but the wait queue it blocks on is a kernel keyed-event keyed by
 *     the lock's address; proven by putting the CRITICAL_SECTION itself in
 *     .bss so it is NEVER rolled back, and the deadlock happens anyway. (2)
 *     THREADS ARE ALMOST NEVER IN USER MODE WHEN YOU PARK THEM -- sampled
 *     while perfectly healthy, all workers sat at the same ntdll WOW64 syscall
 *     return thunk, so SuspendThread does not stop them at a user-mode
 *     boundary and rewinding EIP/ESP under an in-flight syscall yields garbage
 *     register state. A rollback restoring 100% of captured regions still
 *     ended with every worker thread parked forever. ULTRA_CAP_ROLLBACK means
 *     "fast save/load good enough for netplay" (ultra_module.h:107); that is
 *     flatly untrue here, and a gate reading this bit would pick rollback for
 *     titles that can only freeze.
 *
 *   ULTRA_CAP_DETERMINISTIC -- dropped for its OWN, separate reason, not as a
 *     consequence of the above. Determinism and snapshot-ability are different
 *     properties, and the home_state.h evidence closes snapshot/rollback only.
 *     But the ABI defines this bit as "run_frame is bit-deterministic"
 *     (ultra_module.h:106), and we do not own the game's clock at all:
 *     vt_run_frame merely pumps an already free-running Win32 process whose
 *     render/audio/IO threads Windows schedules against a real GPU and a real
 *     audio device. There is no emulated machine here to be deterministic
 *     about. That is why supermodel_module.cpp:70-81 and flycast_module.cpp:
 *     88-99 can keep DETERMINISTIC while refusing ROLLBACK -- they own an
 *     emulated machine and we do not. Same honesty standard, opposite answer.
 *
 * Without the hook: honest 0. */
const uint32_t NATIVE_CAPS_HOOKED =
    ULTRA_CAP_READ_MEMORY | ULTRA_CAP_WRITE_MEMORY;
const uint32_t NATIVE_CAPS_BARE = 0;

/* Native PC games need no emulator BIOS. A valid (empty) terminator array, NOT
 * nullptr - the host walks required_bios[] to stage BIOS and would deref null. */
const char* const NO_BIOS[] = { nullptr };

/* One system per board family. `cpu_hz`=0 (varies per cabinet; commodity x86);
 * the host uses the title's fps as cadence. RAM/audio nominal. These are
 * launch profiles, not silicon models -- the games run on real x86. */
#define BOARD(id, name, mfr) \
    { id, name, mfr, 0.0, 60.0, 0.0, 0.0, 48000, 2, \
      ARC_BUTTONS, ARC_BUTTON_NAMES, NATIVE_CAPS_HOOKED, NO_BIOS }

const ultra_system_info SYSTEMS[] = {
    /* SEGA */
    BOARD("sega_lindbergh",  "SEGA Lindbergh (Linux/x86 arcade)",      "Sega"),
    BOARD("sega_nu",         "SEGA Nu (Windows/x86 arcade)",           "Sega"),
    BOARD("sega_nu2",        "SEGA Nu 2 / Nu SX (Windows/x86)",        "Sega"),
    BOARD("sega_ringedge",   "SEGA RingEdge (Windows/x86 arcade)",     "Sega"),
    BOARD("sega_ringedge2",  "SEGA RingEdge 2 (Windows/x86 arcade)",   "Sega"),
    BOARD("sega_ringwide",   "SEGA RingWide (Windows/x86 arcade)",     "Sega"),
    BOARD("sega_europar",    "SEGA Europa-R (Windows/x86 arcade)",     "Sega"),
    BOARD("sega_alls",       "SEGA ALLS / ALL.S UX (Windows/x86)",     "Sega"),
    /* NAMCO */
    BOARD("namco_n2",        "Namco System N2 (Windows/x86; Linux ELF variant)",  "Namco"),
    BOARD("namco_es1",       "Namco System ES1 (Windows/x86; Linux ELF variant)", "Namco"),
    BOARD("namco_es2plus",   "Namco System ES2 Plus (Win Embedded 7)", "Namco"),
    BOARD("namco_es3",       "Namco System ES3 A/B/X (Win Embedded 7)","Namco"),
    BOARD("namco_es4",       "Namco System ES4 (Windows/x86)",         "Namco"),
    BOARD("namco_bna1",      "Namco System BNA1 / Lite (Win 10 IoT)",  "Namco"),
    /* TAITO */
    BOARD("taito_typex",     "Taito Type X / X+ (Windows/x86)",        "Taito"),
    BOARD("taito_typex2",    "Taito Type X2 (Windows/x86)",            "Taito"),
    BOARD("taito_typex3",    "Taito Type X3 (Windows/x86)",            "Taito"),
    BOARD("taito_typex4",    "Taito Type X4 (Windows/x86)",            "Taito"),
    BOARD("taito_typexzero", "Taito Type X Zero (Windows/x86)",        "Taito"),
    BOARD("taito_nesica",    "Taito NESiCAxLive / 2 (Windows/x86)",    "Taito"),
    /* OTHER */
    BOARD("rawthrills_pc",   "RAW Thrills PC-based (Windows/Linux)",   "Raw Thrills"),
    BOARD("konami_pc",       "Konami PC-based arcade (Bemani PC/ADE)",  "Konami"),
    BOARD("exa_arcadia",     "exA-Arcadia (scaffold-only, live DRM)",  "exA-Arcadia"),
    BOARD("unico_pc",        "UNICO (Korea) PC boards",                "UNICO"),
    BOARD("generic_pc",      "Generic PC-based arcade board",          "various"),
};
constexpr uint32_t SYSTEM_COUNT = sizeof(SYSTEMS) / sizeof(SYSTEMS[0]);

#undef BOARD

} /* anonymous namespace */

/* ========================================================================== */
/*  loader_glue forward decls -- the native launch + process-hook seam.        */
/* ========================================================================== */

namespace loader_glue {
    /* Resolve/apply a board profile (I/O map, DRM bypass, input routing). */
    bool        select_board(const ultra_host_services* host, const char* system_id);
    /* Launch the native game executable through the host loader (ELF/PE). */
    bool        launch_native(const char* exe_or_dir);
    void        stop();
    bool        running();
    uint64_t    pump_frame();
    bool        get_video(ultra_video_frame* v);
    bool        get_audio(ultra_audio_buffer* a);
    void        set_input(uint32_t player, const ultra_input_state* in);
    /* --- external PROCESS hook (the determinism a launcher cannot give) --- */
    bool        hook_available();
    bool        hook_step_frame();
    size_t      hook_state_save(void* buf, size_t cap);
    bool        hook_state_load(const void* buf, size_t len);
    bool        hook_read_mem(uint32_t addr, void* out, uint32_t len);
    bool        hook_write_mem(uint32_t addr, const void* in, uint32_t len);
    bool        reference_mode();
}

/* ========================================================================== */
/*  Module instance.                                                          */
/* ========================================================================== */

struct ultra_module {
    const ultra_host_services* host = nullptr;
    std::string system_id;
    std::string game;
    bool        board_ready = false;
    bool        running     = false;
    uint64_t    frames      = 0;
};

static void hlog(ultra_module* m, ultra_log_level lvl, const char* msg) {
    if (m && m->host && m->host->log)
        m->host->log(m->host->host_ctx, lvl, msg);
}

static const ultra_system_info* find_system(const char* id) {
    for (uint32_t i = 0; i < SYSTEM_COUNT; ++i)
        if (std::strcmp(SYSTEMS[i].system_id, id) == 0) return &SYSTEMS[i];
    return nullptr;
}

/* ========================================================================== */
/*  vtable.                                                                    */
/* ========================================================================== */

static ultra_system_info g_sys_view[SYSTEM_COUNT];
static uint32_t vt_list_systems(ultra_module*, const ultra_system_info** out,
                                uint32_t cap) {
    bool hooked = loader_glue::hook_available();
    for (uint32_t i = 0; i < SYSTEM_COUNT; ++i) {
        g_sys_view[i] = SYSTEMS[i];
        if (!hooked) g_sys_view[i].capabilities = NATIVE_CAPS_BARE;
    }
    uint32_t n = (cap < SYSTEM_COUNT) ? cap : SYSTEM_COUNT;
    for (uint32_t i = 0; i < n; ++i) out[i] = &g_sys_view[i];
    return SYSTEM_COUNT;
}

static ultra_result vt_probe(ultra_module*, const char* path,
                             ultra_content_info* out) {
    std::memset(out, 0, sizeof(*out));
    if (!path) return ULTRA_ERR_BAD_MEDIA;

    /* Native arcade content is a game directory or its launch executable
     * (.exe on Windows boards, an ELF on Lindbergh/N2/ES1). We claim explicit
     * executables and directories; the precise board is chosen at init by the
     * host/user (the same dump can target different profiles). */
    std::string p = path;
    auto ends = [&](const char* e){
        size_t n = std::strlen(e);
        return p.size() >= n && p.compare(p.size()-n, n, e) == 0;
    };
    bool exe = ends(".exe")||ends(".EXE")||ends(".elf")||ends(".ELF");
    bool looks_dir = (!p.empty() && (p.back()=='/' || p.back()=='\\'));
    /* also accept a bare game id / folder with no extension */
    bool no_ext = (p.find_last_of('.') == std::string::npos);
    if (!exe && !looks_dir && !no_ext) { out->claimed = 0; return ULTRA_ERR_UNSUPPORTED; }

    static thread_local std::string s_set;
    s_set = p;
    size_t slash = s_set.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1 < s_set.size())
        s_set = s_set.substr(slash+1);

    /* default board profile is the generic PC one; host/user refines at init */
    out->system_id  = "generic_pc";
    out->game_id    = s_set.c_str();
    out->game_title = s_set.c_str();
    out->media_kind = "native";
    out->claimed    = 1;
    return ULTRA_OK;
}

static ultra_result vt_init(ultra_module* m, const char* system_id) {
    if (!find_system(system_id)) return ULTRA_ERR_UNSUPPORTED;
    m->system_id = system_id;
    if (!loader_glue::select_board(m->host, system_id)) {
        hlog(m, ULTRA_LOG_ERR, "native-x86: failed to apply board profile");
        return ULTRA_ERR_GENERIC;
    }
    m->board_ready = true;
    if (loader_glue::reference_mode())
        hlog(m, ULTRA_LOG_WARN, "native-x86: REFERENCE MODE (no host loader; "
                                "ABI + profile logic only)");
    else {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "native-x86: board profile '%s' ready (native run via host loader)%s",
            system_id, loader_glue::hook_available() ?
            " + determinism hook" : "");
        hlog(m, ULTRA_LOG_INFO, msg);
    }
    return ULTRA_OK;
}

static ultra_result vt_load_media(ultra_module* m, const char* path) {
    if (!m->board_ready) return ULTRA_ERR_NOT_READY;
    m->game = path ? path : "";
    if (!loader_glue::launch_native(path)) {
        hlog(m, ULTRA_LOG_ERR, "native-x86: failed to launch native game. "
             "Ensure the dump matches the selected board profile and that the "
             "host loader can run its executable (PE on Windows boards, ELF on "
             "Lindbergh/N2/ES1).");
        return ULTRA_ERR_BAD_MEDIA;
    }
    m->running = true;
    m->frames = 0;
    if (loader_glue::hook_available())
        hlog(m, ULTRA_LOG_GAME, "native-x86: game running natively WITH "
                                "determinism hook (replay/rollback available -- "
                                "other loaders do not offer this)");
    else
        hlog(m, ULTRA_LOG_GAME, "native-x86: game running natively (launch-and-"
                                "run; attach the hook for replay/rollback)");
    return ULTRA_OK;
}

static ultra_result vt_run_frame(ultra_module* m, uint64_t* out_cycles) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    if (loader_glue::hook_available()) loader_glue::hook_step_frame();
    uint64_t c = loader_glue::pump_frame();
    m->frames += 1;
    if (out_cycles) *out_cycles = c;
    return ULTRA_OK;
}
static ultra_result vt_get_video(ultra_module* m, ultra_video_frame* out) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    return loader_glue::get_video(out) ? ULTRA_OK : ULTRA_ERR_GENERIC;
}
static ultra_result vt_get_audio(ultra_module* m, ultra_audio_buffer* out) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    return loader_glue::get_audio(out) ? ULTRA_OK : ULTRA_ERR_GENERIC;
}
static ultra_result vt_set_input(ultra_module* m, uint32_t player,
                                 const ultra_input_state* in) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    loader_glue::set_input(player, in);
    return ULTRA_OK;
}

/* State/memory via the process hook when attached; UNSUPPORTED otherwise. */
static ultra_result vt_save_state(ultra_module* m, void* buf, size_t* inout_len) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    if (!loader_glue::hook_available()) return ULTRA_ERR_UNSUPPORTED;
    if (!inout_len) return ULTRA_ERR_GENERIC;
    if (!buf) { *inout_len = loader_glue::hook_state_save(nullptr, 0); return ULTRA_OK; }
    size_t need = loader_glue::hook_state_save(nullptr, 0);
    if (*inout_len < need) { *inout_len = need; return ULTRA_ERR_NO_MEMORY; }
    *inout_len = loader_glue::hook_state_save(buf, *inout_len);
    return ULTRA_OK;
}
static ultra_result vt_load_state(ultra_module* m, const void* buf, size_t len) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    if (!loader_glue::hook_available()) return ULTRA_ERR_UNSUPPORTED;
    return loader_glue::hook_state_load(buf, len) ? ULTRA_OK : ULTRA_ERR_BAD_STATE;
}
static ultra_result vt_read_memory(ultra_module* m, uint32_t addr,
                                   void* out, uint32_t len) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    if (!loader_glue::hook_available()) return ULTRA_ERR_UNSUPPORTED;
    return loader_glue::hook_read_mem(addr, out, len) ? ULTRA_OK : ULTRA_ERR_GENERIC;
}
static ultra_result vt_write_memory(ultra_module* m, uint32_t addr,
                                    const void* in, uint32_t len) {
    if (!m->running) return ULTRA_ERR_NOT_READY;
    if (!loader_glue::hook_available()) return ULTRA_ERR_UNSUPPORTED;
    return loader_glue::hook_write_mem(addr, in, len) ? ULTRA_OK : ULTRA_ERR_GENERIC;
}
static ultra_result vt_set_disc(ultra_module*, uint32_t) {
    return ULTRA_ERR_UNSUPPORTED;
}

static ultra_result vt_unload(ultra_module* m) {
    if (m->running) { loader_glue::stop(); m->running = false; }
    return ULTRA_OK;
}
static void vt_destroy(ultra_module* m) {
    if (m->running) loader_glue::stop();
    delete m;
}

static const ultra_module_vtable VTABLE = {
    sizeof(ultra_module_vtable),
    ULTRA_MODULE_ABI_VERSION,
    vt_list_systems, vt_probe,
    vt_init, vt_load_media, vt_unload, vt_destroy,
    vt_run_frame,
    vt_get_video, vt_get_audio, vt_set_input,
    vt_save_state, vt_load_state,
    vt_read_memory, vt_write_memory,
    vt_set_disc
};

/* ========================================================================== */
/*  THE ONE EXPORTED SYMBOL.                                                    */
/* ========================================================================== */

#if defined(_WIN32)
#  define ULTRA_EXPORT extern "C" __declspec(dllexport)
#else
#  define ULTRA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

ULTRA_EXPORT ultra_result ultra_module_create(
        uint32_t host_abi_version,
        const ultra_host_services* host,
        ultra_module_handle* out_handle) {

    if ((host_abi_version >> 16) != ULTRA_MODULE_ABI_VERSION_MAJOR)
        return ULTRA_ERR_ABI_MISMATCH;
    if (!host || !out_handle) return ULTRA_ERR_GENERIC;

    ultra_module* m = new (std::nothrow) ultra_module();
    if (!m) return ULTRA_ERR_NO_MEMORY;
    m->host = host;
    out_handle->instance = m;
    out_handle->vtable   = &VTABLE;
    return ULTRA_OK;
}

/* ========================================================================== */
/*  loader_glue  --  native launch via the host loader + the process hook.     */
/*                                                                            */
/*  REAL build: select_board applies the board's I/O map + DRM/system-message  */
/*  bypass + input routing; launch_native runs the native game executable      */
/*  through Ultra Loader's ELF/PE loader (the same engine that runs Lindbergh).*/
/*  The external PROCESS hook attaches to the running game for determinism */
/*  (read/write process memory, intercept input, gate frame advance) -- this   */
/*  is what gives Ultra Loader replay/rollback on native arcade games, which   */
/*  a launcher does not provide. Reference shim lets the ABI + profile logic   */
/*  be tested without the host loader present.                                 */
/* ========================================================================== */

namespace loader_glue {

static const ultra_host_services* g_host = nullptr;
static std::string g_board;
static std::string g_exe;
static bool        g_ref = false;
static uint64_t    g_frame = 0;
#if defined(_WIN32)
static PROCESS_INFORMATION g_pi = {0};   /* the launched native game process */
static bool g_proc_up = false;
static HANDLE           g_map    = nullptr;   /* shared input block (host side) */
static ultra_io_shared* g_shared = nullptr;
static uint32_t         g_coin_last[ULTRA_IO_COINS] = {0};
#endif

bool reference_mode() { return g_ref; }

/* Board OS family / launch lane.
 *
 * Sega Lindbergh and the Namco ES1 (Debian) image are Linux/ELF -> Ultra Loader's
 * 32-bit native ELF loader. Every Windows/PE board (including the WINDOWS Namco N2
 * variant -- pre-ES GeForce/OpenGL, whose clean-room GL present hook lives in
 * gl_present.c and arms via the board profile) runs as a native process.
 *
 * Namco N2 ships in BOTH a Linux and a Windows variant. We route by the ACTUAL
 * IMAGE, not the board id: if the resolved launch target is an .elf the ELF lane
 * takes it (matching the Linux N2), otherwise the Windows-PE lane runs it and the
 * GL hook (BP_HOOK_GL) attaches. So `namco_n2` is NOT in the id-based Linux set --
 * its routing is decided by board_is_linux() seeing an ELF target, which keeps the
 * board-profile (spawn-suspended + GL hook) and the launch lane in agreement. */
static bool target_is_elf(const std::string& exe) {
    if (exe.size() < 4) return false;
    auto ends = [&](const char* e){
        size_t n = std::strlen(e);
        return exe.size() >= n && exe.compare(exe.size()-n, n, e) == 0;
    };
    return ends(".elf") || ends(".ELF");
}
static bool board_is_linux(const std::string& id, const std::string& exe = "") {
    /* always-Linux board (Sega Lindbergh: Linux/ELF, run by the standalone ELF lane). */
    if (id == "sega_lindbergh") return true;
    /* dual-image Namco boards (N2 + ES1): both ship a Debian-Linux image AND a
     * Windows-Embedded image. Route by the ACTUAL launch target -- an .elf takes the
     * Linux/ELF lane; a PE target stays on the Windows lane so the full clean-room
     * wiring (GL/D3D9 present hook + JVS-USB + amdaemon presence + dongle) attaches.
     * This keeps each board's profile (Windows: spawn/into-daemon + present hook) and
     * its launch lane in agreement for the in-scope Windows variant, while still
     * handing a real Linux dump to the ELF loader. */
    if (id == "namco_n2" || id == "namco_es1") return target_is_elf(exe);
    return false;
}

bool select_board(const ultra_host_services* host, const char* system_id) {
    g_host = host;
    g_board = system_id ? system_id : "";
    g_ref = false;

    /* Resolve this board's UNIFIED profile (board_profile.c) and ARM it: the
     * profile selects the present-hook + I/O transport + injection strategy +
     * protection handler, and sets the ULTRA_* env knobs the injected hook reads
     * on attach. ADDITIVE + byte-safe: the Type X (D3D8) + Type X2 (D3D9) lanes
     * resolve to PLAYABLE profiles that arm exactly what they already armed (the
     * present hooks for D3D8/D3D9 are always-present diagnostics that no-op until
     * a user knob enables the limiter), so this changes nothing for them. A board
     * not in the table just runs with the bare JVS + dongle/dialog defaults. */
#if defined(_WIN32)
    int known = board_profile_arm(g_board.c_str(), /*apply_env=*/1);
    if (known && host && host->log) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "native-x86: board profile '%s' [%s, gfx=%s, present-hook=%s, %s]",
            g_board.c_str(), bp_vendor_str_of(g_board.c_str()),
            bp_gfx_str_of(g_board.c_str()), bp_hook_str_of(g_board.c_str()),
            bp_ready_str_of(g_board.c_str()));
        host->log(host->host_ctx, ULTRA_LOG_INFO, msg);
    } else if (!known && host && host->log) {
        host->log(host->host_ctx, ULTRA_LOG_WARN,
            "native-x86: no board profile for this id -> bare JVS + dongle/dialog "
            "defaults (host/user can refine via ULTRA_* knobs)");
    }
#else
    board_profile_arm(g_board.c_str(), 0);
#endif
    return true;
}

/* Resolve the launch executable: an explicit .exe/.elf is the target; a dir uses
 * ULTRA_NATIVE_EXE (or, roadmap, the board profile's launch-exe hint). */
static std::string resolve_exe(const char* exe_or_dir) {
    if (const char* e = std::getenv("ULTRA_NATIVE_EXE")) if (*e) return e;
    return exe_or_dir ? exe_or_dir : "";
}

#if defined(_WIN32)
static void hglog(ultra_log_level lvl, const char* msg) {
    if (g_host && g_host->log) g_host->log(g_host->host_ctx, lvl, msg);
}

/* directory containing THIS module DLL (so the hook artifacts ship beside it) */
static std::string module_dir() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&module_dir, &self);
    char buf[1024]; DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
    std::string p(buf, n ? n : 0);
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}
static bool file_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
/* find an artifact by base name in: explicit env, module dir, host data/, cwd */
static std::string find_artifact(const char* env, const char* base) {
    if (env) { const char* e = std::getenv(env); if (e && *e && file_exists(e)) return e; }
    std::string md = module_dir() + "\\" + base;
    if (file_exists(md)) return md;
    if (g_host && g_host->resolve_bios) {                 /* staged into data/ */
        char out[1024];
        if (g_host->resolve_bios(g_host->host_ctx, base, out, sizeof(out)) && file_exists(out))
            return out;
    }
    std::string cw = std::string(".") + "\\" + base;
    if (file_exists(cw)) return cw;
    return "";
}

/* Create the shared input block and expose its name to the child via env. */
static bool setup_shared() {
    static uint32_t counter = 0;
    char name[128];
    std::snprintf(name, sizeof(name), "Local\\UltraLoaderIO_%lu_%lu",
                  (unsigned long)GetCurrentProcessId(), (unsigned long)++counter);
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                               sizeof(ultra_io_shared), name);
    if (!g_map) return false;
    g_shared = (ultra_io_shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(ultra_io_shared));
    if (!g_shared) { CloseHandle(g_map); g_map = nullptr; return false; }
    std::memset(g_shared, 0, sizeof(*g_shared));
    g_shared->magic   = ULTRA_IO_MAGIC;
    g_shared->version = ULTRA_IO_VERSION;
    g_shared->flags   = ULTRA_IO_FLAG_LOADER_ALIVE;
    for (int i = 0; i < ULTRA_IO_ANALOG; ++i) g_shared->analog[i] = 0x8000; /* center */
    g_shared->screen_x = g_shared->screen_y = 0x8000;
    std::memset(g_coin_last, 0, sizeof(g_coin_last));
    SetEnvironmentVariableA(ULTRA_IO_ENVVAR, name);   /* child inherits this */
    return true;
}

/* same-bitness injection: write the dll path + run LoadLibraryA in the target.
 * Verifies the remote LoadLibraryA actually returned a module (exit code != 0),
 * not just that the thread ran -- a failed load otherwise reports false success. */
static bool inject_same_bitness(HANDLE proc, const std::string& dll) {
    SIZE_T n = dll.size() + 1;
    void* rem = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) return false;
    bool ok = WriteProcessMemory(proc, rem, dll.c_str(), n, nullptr) != 0;
    if (ok) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        auto fn = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
        HANDLE th = fn ? CreateRemoteThread(proc, nullptr, 0, fn, rem, 0, nullptr) : nullptr;
        if (th) {
            ok = (WaitForSingleObject(th, 10000) == WAIT_OBJECT_0);
            DWORD rc = 0;
            if (ok && GetExitCodeThread(th, &rc)) ok = (rc != 0);  /* HMODULE low32 */
            else ok = false;
            CloseHandle(th);
        } else ok = false;
    }
    VirtualFreeEx(proc, rem, 0, MEM_RELEASE);
    return ok;
}

/* Determine whether the target game process is 64-bit. Prefer IsWow64Process2
 * (handles ARM64 too); fall back to IsWow64Process. If detection fails entirely,
 * assume 32-bit -- the overwhelmingly common Type X / PC-arcade case -- rather
 * than guessing 64-bit and picking the wrong hook DLL. */
static bool target_is_64bit(HANDLE proc) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    typedef BOOL (WINAPI* PIW2)(HANDLE, USHORT*, USHORT*);
    PIW2 iw2 = k32 ? (PIW2)GetProcAddress(k32, "IsWow64Process2") : nullptr;
    if (iw2) {
        USHORT pm = 0, nm = 0;
        if (iw2(proc, &pm, &nm)) {
            USHORT machine = (pm == IMAGE_FILE_MACHINE_UNKNOWN) ? nm : pm;
            return (machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64);
        }
    }
    BOOL wow = FALSE;
    if (IsWow64Process(proc, &wow))
        return (sizeof(void*) == 8) ? (wow == FALSE) : false;
    return false;   /* detection failed: assume 32-bit */
}

/* Inject the clean-room I/O hook into the suspended game. Picks the hook DLL
 * matching the GAME's bitness; cross-bitness uses the matching injector helper. */
static void inject_hook(PROCESS_INFORMATION& pi) {
    const bool host_is_64   = (sizeof(void*) == 8);
    const bool target_is_64 = target_is_64bit(pi.hProcess);
    const bool same_bitness = (host_is_64 == target_is_64);

    std::string dll = find_artifact("ULTRA_TYPEX_HOOK",
                        target_is_64 ? "ultra_typex_hook64.dll" : "ultra_typex_hook32.dll");
    if (dll.empty()) {
        hglog(ULTRA_LOG_WARN, "native-x86: I/O hook DLL not found beside module; "
              "launching without the JVS/dongle hook (game still runs).");
        return;
    }
    if (same_bitness) {
        if (inject_same_bitness(pi.hProcess, dll))
            hglog(ULTRA_LOG_INFO, "native-x86: clean-room I/O hook injected (JVS + dongle bypass active)");
        else
            hglog(ULTRA_LOG_ERR, "native-x86: hook injection failed");
        return;
    }
    /* cross-bitness: spawn the matching-bitness injector helper */
    std::string inj = find_artifact("ULTRA_INJECTOR",
                        target_is_64 ? "ultra-inject64.exe" : "ultra-inject32.exe");
    if (inj.empty()) {
        hglog(ULTRA_LOG_WARN, "native-x86: 32/64-bit mismatch and no ultra-inject helper "
              "found; launching without the I/O hook. (Build ultra-inject{32,64}.exe.)");
        return;
    }
    /* build into std::string so a long DLL path is never silently truncated */
    std::string cmd = "\"" + inj + "\" " + std::to_string((unsigned long)pi.dwProcessId)
                    + " \"" + dll + "\"";
    STARTUPINFOA si; std::memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION ip; std::memset(&ip, 0, sizeof ip);
    std::vector<char> cb(cmd.begin(), cmd.end()); cb.push_back('\0');
    if (CreateProcessA(nullptr, cb.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &ip)) {
        WaitForSingleObject(ip.hProcess, 10000);
        CloseHandle(ip.hThread); CloseHandle(ip.hProcess);
        hglog(ULTRA_LOG_INFO, "native-x86: clean-room I/O hook injected via helper");
    } else {
        hglog(ULTRA_LOG_ERR, "native-x86: injector helper failed to start");
    }
}
#endif /* _WIN32 */

bool launch_native(const char* exe_or_dir) {
    g_frame = 0;
    g_exe = resolve_exe(exe_or_dir);

    if (board_is_linux(g_board, g_exe)) {
        /* REAL (roadmap): hand the ELF to Ultra Loader's 32-bit native loader --
         * the engine that already runs Lindbergh/ABC -- by re-execing
         * ultra-loader.exe with the ELF + this board profile (the 64-bit module
         * host cannot load a 32-bit ELF in-process). Until that route is wired we
         * run the ABI/reference path so the lifecycle stays exercisable. */
        if (g_host && g_host->log)
            g_host->log(g_host->host_ctx, ULTRA_LOG_WARN,
                "native-x86: Linux/ELF board -> 32-bit ELF loader route (wiring pending)");
        g_ref = true;
        return true;
    }

#if defined(_WIN32)
    /* Windows-PE board: the game is a native Windows executable. Launch it
     * SUSPENDED, inject the clean-room I/O hook (JVS engine + dongle/dialog
     * bypass) so the game finds its arcade I/O, then resume. The hook reads the
     * player's controller state from the shared block this sets up. */
    FILE* f = g_exe.empty() ? nullptr : std::fopen(g_exe.c_str(), "rb");
    if (!f) { g_ref = true; return true; }   /* no real exe -> reference/ABI path */
    std::fclose(f);

    setup_shared();   /* loader<->hook input bridge; sets ULTRA_IO_SHM for child */

    std::string cmd = "\"" + g_exe + "\"";
    std::string cwd; size_t s = g_exe.find_last_of("/\\");
    if (s != std::string::npos) cwd = g_exe.substr(0, s);

    /* UNIFIED boot console (path A, primary): name THIS board's SYSTEM + the game
     * TITLE before the cabinet spawns -- the same console the Lindbergh + module
     * routes show, so every launch has one Ultra Loader boot screen. The board's
     * system label comes from the unified catalog (keyed by g_board, the resolved
     * system_id); the title is the game exe basename (Type X games have no
     * per-title table -> the honest basename fallback). The console renders in THIS
     * host process (which owns no game window yet) and self-cleans before the game
     * appears. Honors ULTRA_NO_SPLASH inside ultra_boot_console_run().
     *
     * It also exports the resolved {system,title} so the cross-bitness injector
     * (ultra-inject{32,64}.exe, path B) can show the SAME console if it is the one
     * that actually spawns; ULTRA_BOOT_CONSOLE_SHOWN tells it we already drew one
     * here, so there is never a double console. */
    {
        UltraSystemDescr sd;
        ultra_catalog_system(g_board.c_str(), &sd);   /* fills a generic on unknown */
        char btitle[80];
        ultra_catalog_title(g_exe.c_str(), btitle, sizeof btitle);
        SetEnvironmentVariableA("ULTRA_BOOT_SYSTEM", sd.system ? sd.system : "Arcade");
        SetEnvironmentVariableA("ULTRA_BOOT_TITLE",  btitle);
        if (!std::getenv("ULTRA_NO_SPLASH")) {
            std::string assets = module_dir();   /* Orbitron.ttf / jingle.wav live by the exe */
            UltraBootIdentity bid = { sd.system ? sd.system : "Arcade", btitle,
                                      sd.audio_sys, sd.io_bus, sd.gpu, 0, 0 };
            ultra_boot_console_run(&bid, assets.c_str(), nullptr, nullptr);
            SetEnvironmentVariableA("ULTRA_BOOT_CONSOLE_SHOWN", "1");
        }
    }

    /* Injection strategy from the resolved board profile. Two strategies reach the
     * Windows-PE lane:
     *   - SPAWN_SUSPENDED  (Type X/X2/X3/X4, Sega RingEdge/RingWide/Nu/ALLS/Europa-R,
     *     UNICO, RAW Thrills, Konami, N2-Windows): launch the GAME suspended, inject
     *     the clean-room hook into IT, resume.
     *   - INTO_DAEMON      (Namco ES1/ES2+/ES3/ES4/BNA1): the protection lives in the
     *     local amdaemon/amauthd auth daemon the GAME launches, reached over a local
     *     IPC handle. Our hook, injected INTO THE GAME, intercepts that IPC handle
     *     (namco_amdaemon.c / namco_io.c amauthd surface) and answers PRESENCE/AUTH-
     *     STATE only -- so the same suspended-game injection satisfies the daemon gate
     *     in-process WITHOUT spawning or injecting a separate amdaemon.exe (which the
     *     game starts itself). i.e. for these boards "inject into daemon" is realized
     *     as "intercept the daemon IPC from inside the game", the honest mechanism the
     *     hook actually implements. The launch shape is identical; only the protection
     *     seam differs, and that seam is armed by the board profile (ULTRA_AMDAEMON /
     *     ULTRA_NAMCO_IO). */
    const char* inj_strat = bp_inject_str_of(g_board.c_str());
    {
        char m[160];
        std::snprintf(m, sizeof(m),
            "native-x86: launch strategy '%s' (suspended game + in-process hook%s)",
            inj_strat ? inj_strat : "spawn-suspended",
            (inj_strat && std::strcmp(inj_strat, "inject-into-daemon") == 0)
                ? "; amdaemon gate intercepted in-process" : "");
        hglog(ULTRA_LOG_INFO, m);
    }

    STARTUPINFOA si; std::memset(&si, 0, sizeof si); si.cb = sizeof si;
    g_pi = PROCESS_INFORMATION{};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &g_pi);
    if (!ok) { g_proc_up = false; return false; }

    inject_hook(g_pi);            /* JVS + dongle bypass into the game process   */
    ResumeThread(g_pi.hThread);   /* now let the game run                        */

    g_proc_up = true; g_ref = false;
    return true;
#else
    g_ref = true; return true;
#endif
}
void stop() {
#if defined(_WIN32)
    if (g_proc_up) {
        if (WaitForSingleObject(g_pi.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(g_pi.hProcess, 0);
        if (g_pi.hThread)  CloseHandle(g_pi.hThread);
        if (g_pi.hProcess) CloseHandle(g_pi.hProcess);
        g_pi = PROCESS_INFORMATION{};
        g_proc_up = false;
    }
    if (g_shared) {
        g_shared->flags &= ~ULTRA_IO_FLAG_LOADER_ALIVE;
        UnmapViewOfFile(g_shared); g_shared = nullptr;
    }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
#endif
}
bool running() {
#if defined(_WIN32)
    if (g_ref) return true;
    if (!g_proc_up) return false;
    return WaitForSingleObject(g_pi.hProcess, 0) == WAIT_TIMEOUT;
#else
    return true;
#endif
}

uint64_t pump_frame() {
    ++g_frame;
    /* native game free-runs in its own window; host uses fps cadence. */
    return (uint64_t)(1000000.0 / 60.0);
}

bool get_video(ultra_video_frame* v) {
    std::memset(v, 0, sizeof(*v));
    v->width = 1280; v->height = 720; v->pitch_bytes = 1280*4;
    v->format = ULTRA_PIXFMT_RGBA8888; v->refresh_hz = 60.0; v->pixels = nullptr;
    return true;
}
bool get_audio(ultra_audio_buffer* a) {
    std::memset(a, 0, sizeof(*a));
    a->sample_rate = 48000; a->channels = 2; a->frame_count = 0; a->samples = nullptr;
    return true;
}
void set_input(uint32_t player, const ultra_input_state* in) {
#if defined(_WIN32)
    if (!g_shared || !in || player >= ULTRA_IO_PLAYERS) { (void)player; (void)in; return; }
    uint32_t b = in->buttons;
    #define BIT(i) ((uint8_t)((b >> (i)) & 1u))
    /* bit order = ARC_BUTTON_NAMES: up,down,left,right,b1..b6,start,coin,service,test */
    uint8_t up=BIT(0), down=BIT(1), left=BIT(2), right=BIT(3);
    uint8_t b1=BIT(4), b2=BIT(5), b3=BIT(6), b4=BIT(7), b5=BIT(8), b6=BIT(9);
    uint8_t start=BIT(10), coin=BIT(11), service=BIT(12), test=BIT(13);
    #undef BIT
    /* JVS switch wire order: byte1 hi, byte2 lo (see jvs_core.h) */
    uint8_t byte1 = (uint8_t)((start<<7)|(service<<6)|(up<<5)|(down<<4)|(left<<3)|(right<<2)|(b1<<1)|b2);
    uint8_t byte2 = (uint8_t)((b3<<7)|(b4<<6)|(b5<<5)|(b6<<4));

    /* seqlock write: make seq odd for the duration so the hook (reader) never
     * latches a torn snapshot; fence around the payload writes. */
    uint32_t begin = g_shared->seq + 1;          /* odd: write in progress */
    g_shared->seq = begin;
    MemoryBarrier();

    g_shared->player[player] = (uint16_t)((byte1<<8)|byte2);
    if (player == 0) {                                  /* system TEST + analog */
        if (test) g_shared->system |= 0x80; else g_shared->system &= (uint8_t)~0x80;
        for (int a = 0; a < 8 && a < ULTRA_IO_ANALOG; ++a)
            g_shared->analog[a] = (uint16_t)((int)in->analog[a] + 0x8000);
    }
    if (player < ULTRA_IO_COINS) {                       /* coin edge -> pulse */
        if (coin && !g_coin_last[player]) g_shared->coin_pulse[player]++;
        g_coin_last[player] = coin;
    }

    /* Universal lightgun forward: the loader's Ultra Lightgun Service has
     * already calibrated the pose; convert from libretro center-origin int16
     * to the hook's 16-bit left-justified 0..65535 (center 0x8000). Offscreen
     * → neutral center so the game's reload heuristic fires. Trigger / reload
     * are OR'd into the JVS switch byte using JVS wire bit positions
     * (B1 = bit 9 high-byte D1, B2 = bit 8 high-byte D0). */
    if (player == 0 && in->gun_active) {
        if (in->gun_offscreen) {
            g_shared->screen_x = g_shared->screen_y = 0x8000;
        } else {
            int sx = (int)in->gun_x + 0x8000; if (sx < 0) sx = 0; if (sx > 0xFFFF) sx = 0xFFFF;
            int sy = (int)in->gun_y + 0x8000; if (sy < 0) sy = 0; if (sy > 0xFFFF) sy = 0xFFFF;
            g_shared->screen_x = (uint16_t)sx;
            g_shared->screen_y = (uint16_t)sy;
        }
        uint16_t gb = 0;
        bool trigger = (in->gun_buttons & (1u<<0)) != 0;
        if (trigger)                                                              gb |= (uint16_t)(1u << 9);  /* TRIGGER -> JVS B1 */
        /* Reload = explicit RELOAD button OR offscreen+trigger gesture; never offscreen alone. */
        if ((in->gun_offscreen && trigger) || (in->gun_buttons & (1u<<7)))        gb |= (uint16_t)(1u << 8);  /* RELOAD -> JVS B2 */
        if (in->gun_buttons & (1u<<4))                                            gb |= (uint16_t)(1u << 15); /* START */
        g_shared->player[0] = (uint16_t)(g_shared->player[0] | gb);
    }

    MemoryBarrier();
    g_shared->seq = begin + 1;                    /* even: write complete */
#else
    (void)player; (void)in;
#endif
}

/* --- external process hook on the native game (the layer a launcher cannot give) ---
 * v1 launch-and-run has no hook attached yet, so caps are honestly BARE (0 = "it
 * runs", another loader level). Reference mode reports the hook so the deterministic
 * lifecycle is exercisable; ULTRA_NATIVE_HOOK=1 turns it on once the injection
 * adapter exists; ULTRA_NATIVE_NOHOOK=1 forces it off. */
bool hook_available() {
    if (const char* off = std::getenv("ULTRA_NATIVE_NOHOOK")) if (off[0]=='1') return false;
    if (g_ref) return true;
    if (const char* on = std::getenv("ULTRA_NATIVE_HOOK")) if (on[0]=='1') return true;
    return false;
}
bool hook_step_frame() { return true; }
size_t hook_state_save(void* buf, size_t cap) { (void)buf; (void)cap; return 0; }
bool hook_state_load(const void* buf, size_t len) { (void)buf; (void)len; return true; }
bool hook_read_mem(uint32_t addr, void* out, uint32_t len) {
    (void)addr; if (out && len) std::memset(out, 0, len); return true;
}
bool hook_write_mem(uint32_t addr, const void* in, uint32_t len) {
    (void)addr; (void)in; (void)len; return true;
}

} /* namespace loader_glue */
