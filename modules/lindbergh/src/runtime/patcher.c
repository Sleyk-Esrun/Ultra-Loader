/*
 * patcher.c - Ultra Loader ELF Patcher
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "loader.h"
#include "patcher.h"

extern uint32_t g_ul_gs_spcb_down, g_ul_gs_spcb_up;

/* ------------------------------------------------------------------ */
/* Patch types                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    PT_NOP = 0,
    PT_RET0,
    PT_RET1,
    PT_SEARCH,
    PT_BYTES,   /* write `replace` bytes at file offset `offset` */
} PType;

typedef struct {
    PType       type;
    uint64_t    offset;
    size_t      length;
    const char *desc;
    /* For PT_SEARCH */
    const uint8_t *find;
    size_t      find_len;
    const uint8_t *replace;
    size_t      replace_len;
} Patch;

/* Common byte sequences */
static const uint8_t B_RET0[]  = {0x48,0x31,0xC0,0xC3};       /* xor rax,rax; ret */
static const uint8_t B_RET1[]  = {0xB8,0x01,0x00,0x00,0x00,0xC3}; /* mov eax,1; ret */
static const uint8_t B_NOP8[]  = {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};

/* Generic dongle branch pattern: test rax,rax / jne -> jmp */
static const uint8_t DONGLE_FIND[]    = {0x48,0x85,0xC0,0x75};
static const uint8_t DONGLE_REPLACE[] = {0x48,0x85,0xC0,0xEB};

/* ------------------------------------------------------------------ */
/* Game database (simple flat arrays - no designated init)            */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    const char *sha256_prefix; /* first 16 hex chars, NULL = fallback */
    bool        already_patched;
    Patch       patches[8];
    int         patch_count;
    int         width, height; /* per-game render resolution; 0,0 = use default */
} Game;

/* PBuffer::create(...) (vaddr 0x080dc3d4) renders shadows into an off-screen
 * GLX pbuffer. It reads the static `PBuffer::display` (the GLX Display*, global
 * 0x09592764): `if (display==0) return -1;` else dereferences display+0x84 to
 * feed glXChooseFBConfig. Our GLX layer leaves PBuffer::display == 1 (a fake
 * non-NULL handle), so the NULL-guard passes and `mov ebx,[edx+0x84]` reads
 * 0x85 -> ACCESS_VIOLATION at 0x080dc513. The whole pbuffer/shadow path is
 * optional: the sole caller Shadow::create checks the result with `js` and, on
 * a negative return, db_tracef's and skips shadows (Shadow::create returns -1,
 * which rndpass_init tolerates). So stub PBuffer::create at ENTRY to return -1
 * (`mov eax,-1; ret`) - the body that derefs the bogus display never runs.
 * File offset 0x943d4. (Real fix later: give PBuffer::display a working GLX
 * Display + FBConfig so shadows render.)
 *
 * NOTE: the previous patch wrote `c3 90` at the TAIL (0x945e1). That was based
 * on the *damaged* abc (0xDC-filled body); in the pristine binary 0x080dc5e1 is
 * `eb ee` = `jmp epilogue`, so that patch would have corrupted the stack teardown. */
static const uint8_t PBUFFER_CREATE_STUB[] = {0xB8,0xFF,0xFF,0xFF,0xFF, 0xC3};

/* CRI ADX std::map<int,AdxInfo> RB-tree header init (the SprMgr bug again).
 * init_adx's static initializer heap-allocates the 28-byte tree at global
 * 0xa072294 via __default_alloc_template::allocate(0x1c), but the _Rb_tree
 * empty-init that follows is NOP'd out (20 0x90 bytes at vaddr 0x8158599) - so
 * _M_parent/_M_left/_M_right are garbage and the FIRST make_adx_info insert
 * walks a corrupt tree -> READ AV at 0x0815a030 (insert_unique). The block ptr
 * is in EAX at that point; restore the canonical empty-tree init:
 *   mov dword[eax+4],0   parent/root = 0  (insert's [block+4]==0 => empty path)
 *   mov [eax+8],eax      _M_left  = self
 *   mov [eax+0xc],eax    _M_right = self
 *   mov byte[eax],0      _M_color = red
 * then 4 NOP pad (fills the 20-byte hole exactly). File offset 0x110599. */
static const uint8_t ADX_MAP_INIT[] = {
    0xC7,0x40,0x04,0x00,0x00,0x00,0x00,  /* mov dword ptr [eax+4], 0 */
    0x89,0x40,0x08,                      /* mov [eax+8], eax         */
    0x89,0x40,0x0C,                      /* mov [eax+0xc], eax       */
    0xC6,0x00,0x00,                      /* mov byte ptr [eax], 0    */
    0x90,0x90,0x90,0x90                  /* pad                      */
};

/* SprMgr::SprMgr() (vaddr 0x08078a1a) allocates 16 std::list<tag_SpriteBuffer>
 * sentinel nodes but the two self-init writes that make each empty list valid
 * - `mov [eax],eax` (89 00) and `mov [eax+4],eax` (89 40 04) - are NOP'd out on
 * disk (both abc and abc.bak). Result: the sentinels keep heap garbage, so the
 * very first thing init_sprite does (clear those 16 lists) walks a tangled,
 * non-terminating node chain forever (confirmed via the hang watchdog + chain
 * walk). Restore the self-init in-memory; the `add ebx,4` between them is intact.
 * File offset 0x30a44 (vaddr 0x08078a44). Restores: 89 00 | 83 c3 04 | 89 40 04 */
static const uint8_t SPRMGR_SELFINIT[] = {0x89,0x00, 0x83,0xc3,0x04, 0x89,0x40,0x04};

/* ptcl_ctxt_list std::list<PtclContext> sentinel self-init - same NOP-obfuscation
 * class as SprMgr. The static ctor at 0x080dc734 allocate(0x14)s the sentinel but
 * the self-link writes 'mov [eax],eax; mov [eax+4],eax' were replaced with 5 NOPs,
 * so the sentinel's _M_next/_M_prev hold heap garbage. efc_make_ptcl_context
 * (0x080dce8e) then walks node->_M_next during MISSION-INIT (after START) and
 * dereferences garbage -> crash at 0x080dcf15. Restore the 5-byte self-link.
 * File offset 0x94759 (vaddr 0x080dc759). 90 90 90 90 90 -> 89 00 | 89 40 04. */
static const uint8_t PTCL_SELFINIT[] = {0x89,0x00, 0x89,0x40,0x04};

/* EXPERIMENTAL (probe): init_io_jvs() (vaddr 0x080943fc) blocks forever in the
 * JVS I/O-board enumeration (amJvsInit -> amJvsSendRequest/amJvsRecvAcknowledge
 * poll with NO timeout) because we don't yet emulate the baseboard-command (bc)
 * shared-memory JVS protocol. `ret` at entry skips JVS bring-up so the game
 * proceeds to the render loop on keyboard input (glut callbacks are already
 * installed before init_io_jvs). Replace with real JVS emulation later.
 * File offset 0x4c3fc. Gate: only applied when env ULTRA_SKIP_JVS=1. */
static const uint8_t SKIP_JVS[] = {0xc3};

/* EXPERIMENTAL (probe): init_sound() (vaddr 0x08156c90) initializes the CRI ADX
 * audio middleware (init_adx), which crashes non-deterministically in its
 * std::map<int,AdxInfo> once init goes multi-threaded. Audio is not needed to
 * render a frame, so `ret` at entry skips it to reach glutMainLoop and validate
 * the render path. Real audio + the corruption fix come later.
 * File offset 0x10ec90. Gate: env ULTRA_SKIP_AUDIO=1. */
static const uint8_t SKIP_AUDIO[] = {0xc3};

/* EXPERIMENTAL (probe): calc_camera_matrix (vaddr 0x08056a6a), called per-camera
 * by ctrl_camera via the disp_io task, suffers a stack-return-slot clobber on
 * one iteration -> ret-to-0 just before glutMainLoop. `ret` at entry returns
 * cleanly (caller's pushed return is intact at [esp]) WITHOUT running the body
 * that corrupts the slot, so the game proceeds to the render loop (cameras
 * uncomputed = wrong view, but it lets us see whether rendering works).
 * File offset 0xea6a. Gate: env ULTRA_SKIP_CAMERA=1. */
static const uint8_t SKIP_CAMERA[] = {0xc3};

/* The game identified at load (by SHA) - published for the boot/init console so
 * it shows the REAL loaded title instead of the (possibly stale) ini game_path. */
const char *g_ul_loaded_title = NULL;

/* After Burner Climax - your confirmed hash */
static const Game GAME_ABC = {
    "After Burner Climax (Rev A)",
    /* Pristine disk0/abc (== abc.bak). NOTE: an earlier session's experimental
     * disk-patching had left abc damaged (sha e3b1c2a1...): main_loop_process's
     * prologue was mangled into `call wait_vsync+0x56` (a jump into the middle
     * of wait_vsync), causing the frame-1 render-loop crash at 0x0807a187, plus
     * `xor eax,eax;ret` stubs over refresh_object/begin_/end_instance_object/
     * billboard_z/downcase_string/amDipsw*, a NOP'd check_security_error, and a
     * 0xDC fill over PBuffer::~PBuffer. Restored abc from abc.bak; this prefix
     * tracks the pristine image so the in-memory patches below still apply. */
    "0acdfb8fd65a71bd",
    false,
    {
        {PT_RET0,   0x4A2380, 4, "Dongle check: force pass",    NULL,0,NULL,0},
        {PT_NOP,    0x4A3100, 8, "Server ping: NOP",            NULL,0,NULL,0},
        {PT_RET0,   0x4A2940, 4, "HASP poll: stub",             NULL,0,NULL,0},
        {PT_BYTES,  0x943d4,  0, "PBuffer::create -> ret -1 (skip shadow pbuffer; bogus GLX display)", NULL,0, PBUFFER_CREATE_STUB, sizeof(PBUFFER_CREATE_STUB)},
        {PT_BYTES,  0x30a44,  0, "SprMgr std::list sentinel self-init (un-NOP)", NULL,0, SPRMGR_SELFINIT, sizeof(SPRMGR_SELFINIT)},
        /* ptcl_ctxt_list is NOT patched in the static ctor: doing so makes the
         * boot-time particle init (init_pause) walk the now-empty list and crash.
         * Instead the render loop self-links the sentinel at runtime (g_ul_ptcl_*),
         * so boot keeps its old behavior and only the post-START mission-init path
         * sees the repaired empty list. */
        /* Force STANDARD (upright, non-motion) cabinet. We don't emulate the
         * motor-drive board, so the Deluxe build halts on "Error ABX01 MOTOR
         * DRIVE COMMUNICATION ERROR". is_config_cabinet_moving() (0x081516ea) and
         * is_config_superdeluxe() (0x08151770) -> 0 makes the game a standard
         * cabinet and skips the motor entirely. (PT_RET0 = dec eax; xor eax,eax; ret.) */
        {PT_RET0,   0x1096ea, 4, "Cabinet: is_config_cabinet_moving -> 0 (skip motor drive)", NULL,0,NULL,0},
        {PT_RET0,   0x109770, 4, "Cabinet: is_config_superdeluxe -> 0",                        NULL,0,NULL,0},
        {PT_BYTES,  0x110599, 0, "CRI ADX std::map RB-tree header init (un-NOP)", NULL,0, ADX_MAP_INIT, sizeof(ADX_MAP_INIT)},
    },
    8,
    /* After Burner Climax is a widescreen Lindbergh Yellow title; its native
     * render resolution is 1360x768. Feeding this via the DIP switches makes
     * the game's resolution-table index (the edx that was selecting garbage
     * FBuffer dimensions under the old 640x480 default) resolve correctly. */
    1360, 768
};

/* LGJ FULLSCREEN-BIT clear (force windowed path).
 * RE chain (disasm-anchored, lgj_glext.log): _sSystem::userInitialize @0x82e1327
 * does `or ecx,0x40` (bytes 83 c9 40) to set the FULLSCREEN flag (bit 6) in the
 * window-flags global [0x8bf00b0], then stores it. sWinProc_MakeWindow(0x8412502)
 * later branches on that bit (`test [0x8bf00b0],0x40; jne fullscreen`): SET takes
 * sWinProc_ChangeFullScreen(0x8411b16), which calls the statically-linked
 * libXxf86vm XF86VidModeGetModeLine(0x84f66c8). That needs a REAL X server
 * (find_display->XextFindDisplay returns 0 against our fake _XDisplay, and the
 * mode read goes through _XReply over a socket we don't have) -> GetModeLine
 * returns 0 -> ChangeFullScreen returns FALSE *without ever calling
 * sWinProc_MakeWindowSub* (the windowed branch that does XCreateColormap/
 * XCreateWindow). FALSE -> _sScene::initializeGraph fails -> _sSystem::initialize
 * early-rets, SKIPPING _sScene::initialize()(0x8401dea) which is the ONLY writer
 * of _sScene::pMapSceneDatabase@0x08865a7c -> map stays NULL -> _sFix::initialize
 * later derefs it (_Rb_tree::find @0x083B491F) and crashes.
 * FIX: clear bit 6 instead of setting it -> `and ecx,0xffffffbf` (83 e1 bf, same
 * 3 bytes) -> MakeWindow takes the WINDOWED sWinProc_MakeWindowSub path ->
 * XCreateWindow (loader stub makes the real Win32 GL window) -> MakeWindow TRUE
 * -> _sScene::initialize runs -> map populated -> userInitialize proceeds.
 * File offset 0x299327 (vaddr 0x82e1327 - LOAD base 0x08048000). Verified the
 * on-disk bytes ARE 83 c9 40 before patching. ADDITIVE: GAME_LGJ-only; ABC never
 * sees this. */
static const uint8_t LGJ_WINDOWED_AND[] = {0x83,0xE1,0xBF}; /* and ecx,~0x40 */

/* Let's Go Jungle! (Special) - Lindbergh title #2 bring-up.
 * REAL data-driven entry (NOT a placeholder): the sha256 below is the actual
 * hash of H:/Lindbergh Titles/Let's Go Jungle/disk0/lgj_final (12,687,190 bytes,
 * == lgj_final.bak, so the original dumped image). ELF32 ET_EXEC EM_386,
 * entry 0x8072d70, GOT JUMP_SLOTs at 0x08771xxx (406 total: 96 core gl*, 18
 * glX*, Cg via libCgGL.so). It uses DIRECT GLX (glXSwapBuffers/MakeCurrent/
 * ChooseVisual) + NVIDIA Cg shaders, and is a LIGHTGUN rail-shooter.
 *
 * No byte-patches yet: unlike ABC, the per-game security/dongle/NOP'd-ctor
 * offsets have NOT been reverse-engineered for LGJ, and inventing offsets would
 * corrupt the image. An EMPTY patch set is correct and safe - LGJ boots on the
 * GENERIC substrate (name-driven GL GOT bind + generic JVS transport +
 * autoprofile self-repair of null GOT calls). The first real boot blocker is
 * discovered from the learning-run log, then RE'd and added here. Render res is
 * the standard Lindbergh widescreen DIP (1360x768); the present bbox-fill
 * pipeline tolerates a mismatch if LGJ's native mode differs. */
static const Game GAME_LGJ = {
    "Let's Go Jungle! Special",
    "53168aca59f7adca",
    false,
    {
        /* Force windowed window-creation path (skip the X-server-only
         * XF86VidMode fullscreen mode-switch that fails on the fake _XDisplay
         * and gates out _sScene::initialize -> null scene-db map crash). */
        {PT_BYTES, 0x299327, 3, "LGJ: clear fullscreen bit (windowed path)",
         NULL, 0, LGJ_WINDOWED_AND, sizeof(LGJ_WINDOWED_AND)},
    },
    1,                                /* patch_count */
    1360, 768
};

/* Ghost Squad Evolution ("GSEVO") - Lindbergh title #2 (COMPLETE dump).
 * REAL data-driven entry: sha256 below is the actual hash of
 * H:/Lindbergh Titles/Ghost Squad Evolution/disk1/vsg_l/vsg (3,116,173 bytes,
 * ELF32 i386 ET_EXEC, dynamically linked, UNSTRIPPED + debug_info so addr2line
 * works). Engine namespace "vsg"; ships librnalindbergh_jr.so + snd_util.so in
 * the same dir (= cwd after the loader auto-cd's to the ELF dir). Loads game
 * data from media/ + shader/ relative to cwd.
 *
 * WINDOWING = pure GLUT (libglut.so.3 + libGL.so.1 + libGLU.so.1 + libX11/Xi/
 * Xmu/Xext) - the SAME path ABC uses (verified: GSEVO's glut imports are a
 * subset of ABC's, NO NVIDIA Cg, NO direct GLX). So it boots on the generic
 * substrate (glut_stub + generic GL GOT name-bind + ctor walker + ELF32
 * cross-.so binder) with NO byte-patches - exactly like the LGJ approach, but
 * GSEVO is content-complete so there is no data gap. An EMPTY patch set is
 * correct and safe until a learning-run log surfaces a real boot blocker (then
 * RE it and add it here). Render res = standard Lindbergh widescreen DIP
 * (1360x768, same as ABC); the present bbox-fill pipeline tolerates a mismatch
 * if GSEVO's native mode differs. SHA-GATED: ABC/LGJ are unaffected. */
static const Game GAME_GSEVO = {
    "Ghost Squad Evolution",
    "505eaba63067c656",
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no byte-patches yet (boots on generic substrate) */
    },
    0,                                /* patch_count = 0 */
    1360, 768
};

/* Hummer Extreme Edition - Lindbergh title #4 bring-up.
 * REAL data-driven entry: sha256 prefix below is the actual hash prefix of
 * I:/Lindbergh Titles/Hummer Extreme Edition/disk0/hummer_Master.elf
 * (86,148,584 bytes, ELF32 i386 ET_EXEC, entry 0x807b2f0).
 *
 * HISTORY: earlier builds bypassed .ctors[5] and .ctors[6] (both crashy
 * during static-init). Root cause was unresolved wchar/locale + C++-EH +
 * OpenSSL EVP guest imports. Now that stubs_glibc.c publishes the wchar/
 * locale surface, unwind_stub.c publishes _Unwind_* + __gxx_personality_v0,
 * ossl_stub.c publishes the EVP/RSA/ERR dongle-verify surface, and
 * segaapi_stub.c publishes the EAX 4.0 GUIDs, the ctors should complete
 * cleanly on their own. The Hummer entry is kept so the loader still
 * identifies the game by SHA and applies the correct resolution, but the
 * ctor byte-patches are removed. If a ctor faults again, add a targeted
 * patch here - do NOT re-add the blanket 0xC3 bypasses. */
static const Game GAME_HUMMER = {
    "Hummer Extreme Edition",
    "3a68e45a2e905e26",
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no byte-patches - real stubs land the ctors */
    },
    0,                                /* patch_count = 0 */
    1360, 768
};

static const Game GAME_OR2 = {
    "Outrun 2 SP SDX",
    "c4d5e6f7a8b9c0d1",
    false,
    {
        {PT_RET0,   0x523400, 4, "Dongle: force pass",          NULL,0,NULL,0},
        {PT_RET1,   0x524100, 6, "License check: force pass",   NULL,0,NULL,0},
        {PT_NOP,    0x525000, 8, "Server check: NOP",           NULL,0,NULL,0},
    },
    3
};

/* Virtua Fighter 5 Rev C - real dump.
 * SHA-256 c5d69b5ff7e82ac1fcaeebc702c259c1a273b97158a1162b03a42720689fd8b6
 * (7,949,560 B). ELF32 i386 ET_EXEC, GLUT-based (like GSEVO), heavily
 * static-linked so amDongle* are internal - no PLT stubs.
 * Force each amDongle wrapper to return 1 (success):
 *   0x080b1cee  amDongleInit wrapper (prints "amDongleInit() failed.(%d)")
 *   0x080b1cee  amDongleUserInfoEx wrapper (shares Init entry - same func)
 *   0x080b2170  amDongleExit wrapper
 *   0x080b2096  amDongleDecryptEx wrapper
 * File offsets = VA - 0x08048000. All wrappers already end `mov eax,1; ret`
 * on the success path, so PT_RET1 is byte-compatible. */
static const Game GAME_VF5 = {
    "Virtua Fighter 5 (Rev C)",
    "c5d69b5ff7e82ac1",
    false,
    {
        {PT_RET1, 0x69cee, 6, "VF5 amDongleInit/UserInfoEx wrapper -> ret 1", NULL,0,NULL,0},
        {PT_RET1, 0x6a170, 6, "VF5 amDongleExit wrapper -> ret 1",            NULL,0,NULL,0},
        {PT_RET1, 0x6a096, 6, "VF5 amDongleDecryptEx wrapper -> ret 1",       NULL,0,NULL,0},
    },
    3,
    1360, 768
};

/* The House of the Dead 4 - real dump.
 * SHA-256 72fdd45830acd9317ec1e4f97de35a270c94746068fdaf827381c91bb47aa296
 * (9,777,800 B). ELF32 i386 ET_EXEC, dynamically linked, WITH debug_info.
 * Uses the C++ Sega Lindbergh SDK (mangled CJvsMgr/CGunMgr/CGameMgr/CEvtMgr) -
 * NOT ABC's C symbols. VA->file delta = 0x08048000.
 *
 * Dongle bypass patches are applied in patcher_run() (below) with the
 * `game == &GAME_HOTD4` block. The patch_count = 0 first pass boots HOTD4 on
 * the generic substrate; empty patch set is CORRECT and safe (LGJ/GSEVO
 * approach - real blockers discovered from learning-run logs). Cabinet-type
 * gate is inlined (no `is_config_cabinet_moving` C symbol), no gun
 * auto-calibration override needed (Ultra Omni-Track handles calibration
 * via the lightgun_get_state overlay in jvs_input.c P1=ch0,1 / P2=ch2,3). */
static const Game GAME_HOTD4 = {
    "The House of the Dead 4",
    "72fdd45830acd931",
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no fixed byte-patches - dongle patches applied in patcher_run */
    },
    0,                                /* patch_count = 0 (LGJ/GSEVO-style empty first pass) */
    1360, 768
};

static const Game GAME_ID4 = {
    "Initial D4 Export",
    "e8f9a0b1c2d3e4f5",
    false,
    {
        {PT_RET0,   0x71A000, 4, "Dongle: force pass",          NULL,0,NULL,0},
        {PT_RET1,   0x71B200, 6, "Card reader: fake success",   NULL,0,NULL,0},
    },
    2
};

/* R-Tuned Ultimate Street Racing (Sega Lindbergh, DSR) - EMPTY patch set.
 * ELF32 i386 (entry 0x08051b50, .text @ file 0x9b50), full amDongle 0.12 /
 * amBackup 0.05 / amJvs / amDipsw / amDrive stack, CRI Sofdec, cabinet gating
 * via `Wrap_cabinet_type` (not `is_config_superdeluxe`). Freeplay is the amBackup
 * FREEPLAY setting (same as ABC). Runtime amDongle/amBackup/amJvs interceptors
 * neutralize security without byte-patches; boots on the generic ELF32 substrate.
 * FFB is Sega drive-board over serial (/dev/tts/0|1) - MotorCtrlTask emits
 * `Drive board is not connected to main board.` if unstubbed; a drive-board
 * shim goes behind ULTRA_RTUNED_DRIVEBOARD=1 (separate file), not here.
 * Render res: Lindbergh Yellow widescreen DIP (1360x768). SHA-GATED. */
static const Game GAME_RTUNED_DSR = {
    "R-Tuned Ultimate Street Racing (DSR)",
    "540335426caa6366",
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no byte-patches yet (boots on generic substrate) */
    },
    0,
    1360, 768
};

static const Game GAME_RTUNED_DSR_HD_FFB = {
    "R-Tuned Ultimate Street Racing (HD/FFB)",
    "778f85f6b11c1163",
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no byte-patches yet (boots on generic substrate) */
    },
    0,
    1360, 768
};

/* Cruis'n Blast (Raw Thrills g5 on Lindbergh-shaped Linux ELF32) - bring-up entry.
 * Board = Raw Thrills g5 (NOT a Sega Lindbergh; the ELF is a Linux i386 ET_EXEC
 * that dlopens libhasp + libftd2xx + libfmodstudio + libfmodex + libPhysXLoader +
 * libtheora + libcri). The Lindbergh runtime is the closest match:
 *   - ELF32 loader/dynlink/patcher pipeline reused unchanged.
 *   - HASP Sentinel HL SDK (30-entry surface) shim goes into hasp_shim.[ch] +
 *     hasp_envelope.c + dongle_wrapper.c on the runtime side.
 *   - FTDI (libftd2xx.so) wheel/pedal board -> ftdi_shim + rio2_device.
 *   - LPT coin/start -> lpt_io_emu + uhb_device.
 *   - fmod/PhysX/theora/cri get no-op stubs today (SOUND_STATUS=silent - NOT
 *     playable-complete per game-complete rule; wheel input works, no audio).
 *
 * Expected layout at runtime (per FINAL PLAN):
 *   <game>/
 *     game                       (the Linux ELF32; may live directly at root)
 *     pm/game                    (alt layout - ELF nested under pm/)
 *     pm/utils/libfmod.so.10     fmod ex low-level bank
 *     pm/utils/libfmodstudio.so.10
 *     utils/run_game.sh          the Linux launch script; presence of this +
 *                                a `game` ELF (root or under pm/) is the
 *                                folder-fingerprint the loader shell uses
 *                                to route the dir to Lindbergh.
 *
 * Command-line the shell should synthesize (matches how the arcade ran it):
 *   game -n -f<WxH> -lpt0378 <racefile>
 *
 * Empty patch set on first pass (LGJ / GSEVO style) - real byte-blockers come
 * from the learning-run log. Render resolution 1360x768 to match the g5 cabinet
 * default; -f<WxH> can override via argv. SHA_PREFIX left as NULL until a real
 * dump lands (the SHA loop skips NULL entries, so nothing false-matches).
 * Meanwhile CB is IDENTIFIED on the loader side (before hand-off to this
 * runtime): the folder-fingerprint helper in the shell's main.c routes any
 * dir with `game`+`utils/run_game.sh` to the Lindbergh module, and the
 * loader's ultra_catalog matches "cruisn"/"cruisin"/"cruis'n" in the path
 * to publish the correct boot-console labels. This entry becomes the SHA
 * lookup once we compute the real hash prefix from a dumped ELF. */
static const Game GAME_CRUISN_BLAST = {
    "Cruis'n Blast (Raw Thrills g5)",
    "dbfaf648214fb0d4",                /* sha256 prefix (first 16 hex chars) of
                                       * this Cruis'n Blast 2.31 (2022-06-21) dump
                                       * - measured via patcher hash log. Enables
                                       * identify_game() to route this ELF to
                                       * GAME_CRUISN_BLAST so the CALL-NOP block
                                       * in patcher_run runs on this dump.
                                       * Original comment (for future dumps):
                                       * NULL keeps the SHA loop from matching
                                       * (loop skips NULL); the folder-fingerprint
                                       * route + name-substring hit in
                                       * ultra_catalog identify CB meanwhile.
                                       * The packer-bypass CALL-NOP patch is
                                       * applied by the `game == &GAME_CRUISN_BLAST`
                                       * block in patcher_run() with an in-image
                                       * byte-signature check (`E8 49 FB 00 00`
                                       * at file offset 0x1EA8D72 + corroborating
                                       * `C7 04 24 1F` at 0x1EA8D6B), so this
                                       * empty patch_count = 0 row is by design
                                       * and matches HOTD4/GSEVO's pattern. */
    false,
    {
        {0,0,0,NULL,NULL,0,NULL,0},   /* no fixed byte-patches - packer bypass
                                       * applied in patcher_run under signature
                                       * gate (HOTD4/GSEVO style). */
    },
    0,                                /* patch_count = 0 */
    1360, 768
};

/* Generic fallback - pattern search, no fixed offsets */
static const Game GAME_GENERIC = {
    "Generic Lindbergh",
    NULL,
    false,
    {
        {PT_SEARCH, 0, 0, "Generic dongle branch inversion",
         DONGLE_FIND, sizeof(DONGLE_FIND),
         DONGLE_REPLACE, sizeof(DONGLE_REPLACE)},
    },
    1
};

static const Game *GAME_DB[] = {
    &GAME_ABC,
    &GAME_LGJ,
    &GAME_GSEVO,
    &GAME_HUMMER,
    &GAME_OR2,
    &GAME_VF5,
    &GAME_HOTD4,
    &GAME_ID4,
    &GAME_RTUNED_DSR,
    &GAME_RTUNED_DSR_HD_FFB,
    &GAME_CRUISN_BLAST,
    &GAME_GENERIC,  /* fallback always last */
};
#define GAME_DB_COUNT 12

/* ------------------------------------------------------------------ */
/* SHA-256 from memory                                                 */
/* ------------------------------------------------------------------ */
static bool sha256_memory(const uint8_t *data, size_t size, char out[65])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return false;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) goto done;

    size_t chunk = 65536;
    for (size_t off = 0; off < size; off += chunk) {
        DWORD n = (DWORD)((size - off < chunk) ? (size - off) : chunk);
        if (!CryptHashData(hash, data + off, n, 0)) goto done;
    }

    BYTE  digest[32];
    DWORD dlen = 32;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) goto done;
    for (int i = 0; i < 32; i++) snprintf(out + i*2, 3, "%02x", digest[i]);
    out[64] = '\0';
    ok = true;
done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Background backup                                                    */
/* ------------------------------------------------------------------ */
typedef struct { char src[MAX_PATH]; char dst[MAX_PATH]; } BackupArgs;

static DWORD WINAPI backup_thread(LPVOID p)
{
    BackupArgs *a = (BackupArgs*)p;
    loader_log(LOG_INFO, "Backup: %s", a->dst);
    if (!CopyFileA(a->src, a->dst, TRUE))
        loader_log(LOG_WARN, "Backup failed: error %lu (non-fatal)", GetLastError());
    else
        loader_log(LOG_INFO, "Backup complete");
    free(a);
    return 0;
}

static void make_backup_async(const char *elf_path)
{
    /* Try backup next to game first; fall back to %TEMP% if denied */
    char bak[MAX_PATH];
    snprintf(bak, MAX_PATH, "%s.bak", elf_path);

    if (GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES) {
        loader_log(LOG_INFO, "Backup exists: %s", bak);
        return;
    }

    /* Test if we can write to that directory */
    HANDLE test = CreateFileA(bak, GENERIC_WRITE, 0, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (test == INVALID_HANDLE_VALUE) {
        /* Can't write next to game - use %TEMP%\ultraloader_backups\ */
        char temp[MAX_PATH];
        GetTempPathA(MAX_PATH, temp);
        char dir[MAX_PATH];
        snprintf(dir, MAX_PATH, "%sultraloader_backups", temp);
        CreateDirectoryA(dir, NULL); /* no-op if exists */

        /* Use just the filename from elf_path */
        const char *fname = strrchr(elf_path, '\\');
        if (!fname) fname = strrchr(elf_path, '/');
        if (!fname) fname = elf_path; else fname++;

        snprintf(bak, MAX_PATH, "%s\\%s.bak", dir, fname);
        loader_log(LOG_INFO, "Game dir read-only; backup -> %s", bak);
    } else {
        CloseHandle(test);
        DeleteFileA(bak); /* remove the test file */
    }

    BackupArgs *a = (BackupArgs*)malloc(sizeof(BackupArgs));
    strncpy(a->src, elf_path, MAX_PATH-1);
    strncpy(a->dst, bak,      MAX_PATH-1);
    a->src[MAX_PATH-1] = 0;
    a->dst[MAX_PATH-1] = 0;
    HANDLE t = CreateThread(NULL, 0, backup_thread, a, 0, NULL);
    if (t) CloseHandle(t);
    else { free(a); }
}

/* ------------------------------------------------------------------ */
/* Apply one patch                                                      */
/* ------------------------------------------------------------------ */
static void apply_patch(uint8_t *img, size_t img_sz, const Patch *p)
{
    if (!p->desc) return;
    loader_log(LOG_INFO, "  [patch] %s", p->desc);

    if (p->type == PT_SEARCH) {
        int found = 0;
        for (size_t i = 0; i + p->find_len <= img_sz; i++) {
            if (memcmp(img + i, p->find, p->find_len) == 0) {
                memcpy(img + i, p->replace, p->replace_len);
                loader_log(LOG_INFO, "    found at 0x%zx", i);
                found++;
                i += p->replace_len;
            }
        }
        if (!found)
            loader_log(LOG_INFO, "    pattern not found (may already be patched)");
        return;
    }

    if (p->offset >= img_sz) {
        loader_log(LOG_WARN, "    offset 0x%llx out of range",
                   (unsigned long long)p->offset);
        return;
    }

    uint8_t *dst = img + p->offset;
    switch (p->type) {
    case PT_NOP:  memset(dst, 0x90, p->length); break;
    case PT_RET0: memcpy(dst, B_RET0, sizeof(B_RET0)); break;
    case PT_RET1: memcpy(dst, B_RET1, sizeof(B_RET1)); break;
    case PT_BYTES:
        if (p->replace && p->replace_len && p->offset + p->replace_len <= img_sz)
            memcpy(dst, p->replace, p->replace_len);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Public: patcher_run                                                  */
/* ------------------------------------------------------------------ */
PatchResult patcher_run(const char *elf_path,
                         uint8_t *image, size_t image_size,
                         PatchOptions opts)
{
    PatchResult result;
    memset(&result, 0, sizeof(result));

    loader_log(LOG_INFO, "=== Ultra Loader Patcher ===");
    loader_log(LOG_INFO, "Target: %s (%zu bytes)", elf_path, image_size);

    /* Hash */
    char sha256[65] = "unknown";
    loader_log(LOG_INFO, "Hashing image...");
    sha256_memory(image, image_size, sha256);
    loader_log(LOG_INFO, "SHA-256: %s", sha256);
    strncpy(result.sha256, sha256, 64);
    /* Publish for the self-learning profile system (keyed by ELF hash). */
    {
        extern char g_elf_sha_hex_buf[65];
        extern const char *g_elf_sha_hex;
        strncpy(g_elf_sha_hex_buf, sha256, 64);
        g_elf_sha_hex = g_elf_sha_hex_buf;
    }

    /* Identify */
    loader_log(LOG_INFO, "Identifying game...");
    const Game *game     = NULL;
    const Game *fallback = &GAME_GENERIC;

    for (int i = 0; i < GAME_DB_COUNT - 1; i++) {
        const Game *g = GAME_DB[i];
        if (!g->sha256_prefix) continue;
        if (_strnicmp(sha256, g->sha256_prefix, 16) == 0 ||
            /* abc.pure = the clean (un-obfuscated) After Burner image: identical
             * layout, NOP'd inits already restored, so it IS GAME_ABC. The byte
             * patches below are exact no-ops on it; the input addresses match. */
            (g == &GAME_ABC && _strnicmp(sha256, "39bb5668b0366099", 16) == 0)) {
            game = g;
            break;
        }
    }

    if (game) {
        loader_log(LOG_INFO, "Game: %s", game->name);
        strncpy(result.game_name, game->name, sizeof(result.game_name)-1);
        g_ul_loaded_title = game->name;   /* boot/init console reads the REAL title */
        result.already_patched = game->already_patched;
        /* Publish the game's native render resolution so the board layer can
         * program the DIP switches to match (env ULTRA_RES still overrides). */
        if (game->width > 0 && game->height > 0) {
            extern void lindbergh_set_default_resolution(int, int);
            lindbergh_set_default_resolution(game->width, game->height);
            loader_log(LOG_INFO, "Native render resolution: %dx%d", game->width, game->height);
        }
    } else {
        loader_log(LOG_INFO, "Game not in database, applying generic patches");
        strncpy(result.game_name, "Unknown Lindbergh Title",
                sizeof(result.game_name)-1);
    }

    if (result.already_patched) {
        loader_log(LOG_INFO, "Community-patched ELF, skipping dongle patches");
        result.success = true;
        return result;
    }

    /* Backup (non-blocking) */
    if (!opts.skip_backup)
        make_backup_async(elf_path);

    /* Apply game-specific patches */
    if (game && game->patch_count > 0) {
        loader_log(LOG_INFO, "Applying %d patch(es)...", game->patch_count);
        for (int i = 0; i < game->patch_count; i++) {
            const Patch *pt = &game->patches[i];
            /* The NOP-restoration patches (SprMgr 0x30a44, ADX 0x110599) repair
             * code the obfuscated image had NOP'd out. On the CLEAN abc.pure the
             * real init is already present (and differs from our reconstruction),
             * so applying them would corrupt it - skip when the site isn't NOP'd. */
            if ((pt->offset == 0x30a44 || pt->offset == 0x110599) &&
                pt->offset < image_size && image[pt->offset] != 0x90) {
                loader_log(LOG_INFO, "  [patch] skip '%s' (clean image, already initialized)", pt->desc);
                continue;
            }
            apply_patch(image, image_size, pt);
            result.patches_applied++;
        }
    }

    /* Tell the render loop where ABC stores its vblank callback pointer
     * (set_vblank_callback at 0x0807a11c writes the cb to ds:0x0956e6c4). The
     * loop ticks it each frame so the game's per-frame logic + input advance. */
    if (game == &GAME_ABC) {
        extern uint32_t g_ul_vblank_cb_addr, g_ul_hwready_addr, g_ul_credit_check_addr,
                        g_ul_credit_byte_addr, g_ul_credit_modflag_addr,
                        g_ul_freeplay_addr, g_ul_freeplay_mirror_addr,
                        g_ul_jvs_node_addr, g_ul_jvs_conn_addr;
        g_ul_vblank_cb_addr      = 0x0956e6c4;
        g_ul_hwready_addr        = 0x09571594; /* frame-update gate (byte the loop reads each frame) */
        g_ul_credit_check_addr   = 0x081e252f; /* amCreditCheck(slot,num,coins,mask) coin processor */
        g_ul_credit_byte_addr    = 0x0a0a0da0; /* playable credit byte-array (amCreditDec spends this) */
        g_ul_credit_modflag_addr = 0x0a0a0df4; /* per-slot "credit modified" flags (df4/df8/dfc) */
        g_ul_freeplay_addr       = 0x0a0a0d87; /* AM_CREDIT_CONFIG.FreePlay (amCreditInfo+0x07): gate */
        g_ul_freeplay_mirror_addr= 0x09f3ab4b; /* game credit-config copy +0x07: drives FREEPLAY HUD */
        g_ul_jvs_node_addr       = 0x0958b636; /* JVS node[0] recv: sys/sw bytes (+6 = 8x analog u16) */
        g_ul_jvs_conn_addr       = 0x0958afc0; /* node-connected flag (+4 = node count) */
        extern uint32_t g_ul_ptcl_head_addr, g_ul_ptcl_count_addr;
        g_ul_ptcl_head_addr      = 0x09592784; /* ptcl_ctxt_list std::list head/sentinel ptr */
        g_ul_ptcl_count_addr     = 0x09592788; /* ptcl_ctxt_num element counter */

        /* Bypass the security-dongle check that shows "Error 15 / Game Program
         * Not Found": is_dongle_validate() (0x8168f72) returns the always-0 dongle
         * flag -> check_security_error -> set_error(2). Force it to return 1
         * (mov eax,1; ret). No-op on the cracked abc (already patched); REQUIRED
         * for the clean abc.pure to boot. Same patch the runnable crack used. */
        static const uint8_t DONGLE_OK[] = {0xB8,0x01,0x00,0x00,0x00,0xC3}; /* mov eax,1; ret */
        Patch dp1 = {PT_BYTES, 0x120f72, 0, "is_dongle_validate -> 1 (bypass Error 15)", NULL,0, DONGLE_OK, sizeof(DONGLE_OK)};
        Patch dp2 = {PT_BYTES, 0x120f7e, 0, "check_dongle_validate -> 1 (periodic re-check)", NULL,0, DONGLE_OK, sizeof(DONGLE_OK)};
        Patch dp3 = {PT_BYTES, 0x120fb8, 0, "check_dongle_validate_internal -> 1", NULL,0, DONGLE_OK, sizeof(DONGLE_OK)};
        apply_patch(image, image_size, &dp1);
        apply_patch(image, image_size, &dp2);
        apply_patch(image, image_size, &dp3);
    }

    /* Ghost Squad Evolution ("GSEVO") - clear the SAME Error-15 dongle gate ABC
     * hits. Disasm-confirmed chain (vsg ELF, unstripped):
     *   check_event_io_error @0x80f6bfe is the gate:
     *     call is_dongle_validate; test al,al; je 0x80f6c34 (-> security error)
     *     (mode 6/9): call check_dongle_validate; test al,al; jne skip; else fall
     *     0x80f6c34: call set_jvs_error_security @0x80e980a
     *                => mov dword[0x8656b68], 0xffffff95 (-107)
     *   ev_io_error_disp @0x80f6a34 reads -107 and draws the bitmap-font
     *   "\x02Error\x0115" / "\x02Game Program Not Found." (@VA 0x81c21a6/0x81c21b2).
     * Both validators are the only writers of -107; NO game-program file/path
     * lookup is involved (baseboard/EEPROM/JVS all init clean in gs.log). So this
     * is the dongle-validation security gate - same CLASS as ABC's Error 15 - not
     * a content/path miss. Without a physical Sega dongle, amDongleIsAvailable()=0
     * leaves the dongle-valid flag byte[0x8652263]=0 forever.
     *   is_dongle_validate @0x80e7edc (file off 0x9fedc): movzx eax,byte[0x8652263]
     *     -> ret  (the always-0 flag read)            -> force mov eax,1; ret
     *   check_dongle_validate @0x80e7ee8 (file off 0x9fee8): real rand +
     *     amDongleDecryptEx + memcmp roundtrip         -> force mov eax,1; ret
     * Both targets are clean function entries (55 = push ebp prologue), so the
     * 6-byte 'mov eax,1; ret' overwrites the prologue and returns before any body
     * stack setup is referenced. SHA-GATED to GSEVO: ABC/LGJ untouched. */
    if (game == &GAME_GSEVO) {
        static const uint8_t GS_DONGLE_OK[] = {0xB8,0x01,0x00,0x00,0x00,0xC3}; /* mov eax,1; ret */
        Patch gdp1 = {PT_BYTES, 0x9fedc, 0, "GSEVO is_dongle_validate -> 1 (bypass Error 15)", NULL,0, GS_DONGLE_OK, sizeof(GS_DONGLE_OK)};
        Patch gdp2 = {PT_BYTES, 0x9fee8, 0, "GSEVO check_dongle_validate -> 1 (periodic re-check)", NULL,0, GS_DONGLE_OK, sizeof(GS_DONGLE_OK)};
        apply_patch(image, image_size, &gdp1);
        apply_patch(image, image_size, &gdp2);

        /* Credit + PC-input injection (closes the credit loop past attract).
         * GSEVO's per-frame JVS gun poll is starved (it lives in a guest thread
         * gated on a recoil semaphore that never fires in attract), so the JVS
         * coin/switch read never advances and the game sits on INSERT COIN. We
         * instead (a) assert the credit lib's FreePlay flag so START works with
         * no coin, (b) deposit a real credit on coin insert, and (c) drive the
         * game's OWN GLUT mouse/keyboard PC-input path each frame (aim+trigger+
         * START), bypassing the wedged thread. All addresses are RE'd from the
         * unstripped vsg ELF (.bss runtime structs), poked under IsBadWritePtr
         * guards by jvs_input.c's poll_inputs. SHA-GATED to GSEVO. */
        extern int      g_ul_gsevo;
        extern uint32_t g_ul_gs_freeplay_addr, g_ul_gs_creditcfg_addr,
                        g_ul_gs_creditarr_addr, g_ul_gs_creditmod_addr,
                        g_ul_gs_xboxmode_addr, g_ul_gs_mainmode_addr,
                        g_ul_gs_mainreq_addr, g_ul_gs_node_addr,
                        g_ul_gs_nodecount_addr, g_ul_gs_gunwork_addr,
                        g_ul_gs_swdata_addr, g_ul_gs_swstate_addr,
                        g_ul_gs_kbcb_down, g_ul_gs_kbcb_up, g_ul_gs_mousecb;
        g_ul_gsevo             = 1;
        g_ul_gs_freeplay_addr  = 0x0ae9fb87; /* amCreditInfo.FreePlay (amCreditIsEnough gate) */
        g_ul_gs_creditcfg_addr = 0x085a1c27; /* credit-config gate (check_spend_credit short-circuit) */
        g_ul_gs_creditarr_addr = 0x0ae9fba0; /* per-slot credit array base (stride 5; slot 0) */
        g_ul_gs_creditmod_addr = 0x0ae9fbf8; /* "credit modified" HUD-refresh flag */
        g_ul_gs_xboxmode_addr  = 0x08655334; /* PC/xbox input-mode flag (check_use_xboxpad source) */
        g_ul_gs_mainmode_addr  = 0x08660420; /* get_main_mode current (2/3/4=attract, 6=game) */
        g_ul_gs_mainreq_addr   = 0x08660424; /* get_main_mode_req; =6 starts mm_game_init */
        /* ── NATURAL JVS input path (ABC-style direct node write) ─────────────
         * Disasm-verified (vsg ELF, unstripped):
         *   make_switch_data @0x080e83ae runs UNCONDITIONALLY every frame from
         *   main_loop_func@0x080ee960 -> io_ctrl@0x080e8828. Its decode loop
         *     mov 0x8655364,%ecx ; cmp $0,ecx ; jle <skip>          (node COUNT)
         *     movzbl 0x8655bc4(%esi) / 0x8655bc5 / 0x8655bc7        (node+4/+5/+7)
         *     add $0x28,%esi                                        (per-node STRIDE)
         *   reads switch bytes at io_jvs_node+4/+5/+7 EVERY frame and feeds the
         *   bitfield through make_switch_action@0x080e81dc -> player[].SWDATA+8,
         *   which game_start_check@0x081083fe tests (testb $1,0x8(player+0xf0)).
         *   So writing those node bytes per-frame delivers START/trigger through
         *   the game's REAL start gate WITHOUT spinning the wedged gun thread -
         *   exactly how ABC feeds g_ul_jvs_node_addr. io_jvs_node lives in
         *   zero-init .bss (.bss base 0x082c94a0) and node COUNT defaults to 0,
         *   so make_switch_data's loop body never runs until we force count=1
         *   (the GSEVO analog of ABC's g_ul_jvs_conn_addr+4 = node count). */
        g_ul_gs_node_addr      = 0x08655bc0; /* io_jvs_node base (per-node stride 0x28) */
        g_ul_gs_nodecount_addr = 0x08655364; /* node-count loop guard (force =1)        */
        /* gun_work base (get_gun_position@0x080e506e / set_gun_flags@0x080e4e18,
         * per-player stride 0x1a0): X@+0x14, Y@+0x18 (centered raster coords:
         * raster_x=stored+320, raster_y=240-stored), valid=(byte[+0xd]&4)==0,
         * flags OR'd into +0x4. Drives the Ultra Omni-Track aim/trigger. */
        g_ul_gs_gunwork_addr   = 0x08651e60;
        /* xbox/PC switch struct the title/demo/credit handlers READ for START.
         * get_xbox_swdata@0x080e9106 = 0x86544b4 + p*0x35c; ev_advertise_ctrl
         * tests `testb $0x4,0x135(swdata)` for START. This is the verified
         * decoded-input field for the natural press-start path. */
        g_ul_gs_swdata_addr    = 0x086544b4;
        /* Consolidated per-player switch state the FSM reads: player[0]+0xf0 ==
         * 0x08653420 (live-RE'd). game_start_check tests bit0 of +8 (0x08653428);
         * the title/menu reads the level word +4 (0x08653424, bit0=START). The
         * make_switch_data output lands in a different struct (0x08653c30) and is
         * consolidated here, so we drive 0x08653420 directly for the start gate. */
        g_ul_gs_swstate_addr   = 0x08653420;
        /* The game's internal GLUT input callbacks (guest VAs). Calling these
         * directly delivers START (SPACE -> word0 0x100) + FIRE (left mouse)
         * through GSEVO's native PC-input encoding - the only path that produces
         * the distinct START bit the title/credit gate reads (the node-byte
         * arcade decode collapses every input to switch bit 0x8). */
        g_ul_gs_kbcb_down      = 0x080e96de; /* glut_keyboard_callback(key,x,y)    */
        g_ul_gs_kbcb_up        = 0x080e964a; /* glut_keyboard_up_callback(key,x,y) */
        /* The GLUT SPECIAL callbacks. START is a special key (F1) on this game, not
         * an ASCII one -- SPACE decodes to PUSH1, the trigger. Without these two the
         * game's real START bit could never be produced. */
        g_ul_gs_spcb_down = 0x080e95c2;
        g_ul_gs_spcb_up   = 0x080e9538;
        g_ul_gs_mousecb        = 0x080e93fc; /* glut_mouse_callback(btn,state,x,y) */
        loader_log(LOG_INFO, "GSEVO: credit/FreePlay + JVS node + native GLUT-cb START/fire + gun feed armed");

        /* ── Heap-0 enlargement: fit the credited-game stage assets ───────────
         * ROOT CAUSE of the credited-game crash (EIP=0x0815c08c, read @[NULL+4]
         * in load_xstset). RE'd allocator chain (vsg ELF, unstripped):
         *   load_xstset @0x0815c008 -> read_file @0x080d8278:
         *     fopen("media/spr_card.bin") OK; ftell -> size=17,185,320 (~16.4MB);
         *     mhMalloc(size,'FBUR',heap=0) @0x080f4692 -> mh_malloc_internal
         *     @0x080f3712. Heap 0's pool is created by init_heap_manager
         *     @0x080f3f94 from the heap-config table @VA 0x81c1e80 (stride 0x18):
         *       entry id=0 @0x81c1ee0: size field [+8] @0x81c1ee8 = 0x01b00000
         *       (27 MB). init_heap_memory @0x080f356c does malloc(that size) when
         *       the table's ptr field is NULL (it is) - so heap 0 == one 27 MB
         *       host malloc.
         *   By the time mm_game_init loads the 17 MB card sprite, heap 0 already
         *   holds other resident stage assets (the media/ dir has spr_debug.bin
         *   15 MB, spr_result.bin 10 MB, mdl_stage*.bin/tex_stage*.bin 6-11 MB
         *   each), so the largest CONTIGUOUS free block in the 27 MB pool is
         *   < 17 MB. mh_malloc_internal's free-list walk finds nothing big enough
         *   and returns NULL -> read_file skips fread, returns NULL -> load_xstset
         *   derefs the NULL block (`mov edx,[ebx+4]`, ebx=0) -> read fault.
         *
         * FIX (clean-room, .rodata one-word patch): enlarge heap 0 from 27 MB to
         * 64 MB so the 17 MB card sprite fits even with the rest of stage 1
         * resident. The size word lives in .rodata (PROGBITS, read-only at run
         * time - the allocator only reads it once at init and the game never
         * rewrites it), so a single boot-time poke holds for the whole run. This
         * only changes the host malloc SIZE at init; every allocation that
         * succeeded before still succeeds at the same pool-relative layout, so it
         * cannot perturb the proven-stable attract loop - it is purely additive
         * headroom. File offset of VA 0x81c1ee8 = 0x179ee8 (section .rodata,
         * addr 0x81ad380, off 0x165380). 64 MB = 0x04000000, little-endian.
         * SHA-GATED to GSEVO; ABC/LGJ never see it. */
        static const uint8_t GS_HEAP0_64MB[] = {0x00,0x00,0x00,0x04}; /* LE 0x04000000 */
        Patch ghp = {PT_BYTES, 0x179ee8, 0,
                     "GSEVO heap-0 pool 27MB -> 64MB (fit credited-game stage assets)",
                     NULL, 0, GS_HEAP0_64MB, sizeof(GS_HEAP0_64MB)};
        apply_patch(image, image_size, &ghp);

        /* ── NATURAL-START trampoline: write the START gate bit in the surviving
         *    window (between io_ctrl and event_ctrl, inside main_loop_func) ──────
         * THE TIMING-CRITICAL FIX. game_start_check@0x081083fe (called from the
         * mode handlers inside event_ctrl) gates the credited game on bit0 of
         * swstate+8 (0x08653428 = player[0]+0xf0 -> +8). That word is REBUILT every
         * frame by make_switch_data (inside io_ctrl), and disasm proved NONE of its
         * decode paths can raise bit0 - so a loader write at poll time (or any time
         * before main_loop_func) is wiped by io_ctrl before event_ctrl reads it
         * (verified live: gsevo_post_vblank wrote 0x101, game read 0x00). The ONLY
         * surviving window is INSIDE main_loop_func, AFTER `call io_ctrl@0x080eea45`
         * and BEFORE `call event_ctrl@0x080eea5b`.
         *
         * We splice that seam: redirect the 5-byte `call event_ctrl` (file 0xa6a5b,
         * orig e8 46 15 fe ff) to a 16-byte stub planted in the .text/.fini->.rodata
         * zero padding at VA 0x081ad363 (file 0x165363, verified all-zero, clear of
         * the 0x081ad374 data). The stub:
         *     A0 18 34 65 08   mov al,[0x08653418]   ; al = loader START flag (.bss)
         *     08 05 28 34 65 08 or  [0x08653428],al  ; swstate+8 |= START bit0
         *     E9 33 2c f2 ff   jmp 0x080cffa6        ; tail-call event_ctrl
         * so the gate bit is set with the freshest possible value, immediately
         * before game_start_check runs, then event_ctrl proceeds normally (its ret
         * returns into main_loop_func).
         *
         * FLAG-BYTE ADDRESS (the critical fix): the flag MUST live OUTSIDE the
         * player switch-state array 0x8653420..0x8653c30 (symbol size 0x810 =
         * 4*0x204). make_switch_data ends with a `memcpy(0x8653420+p*0x204, stack,
         * 0x204)` per player (disasm 0x80e84c7-0x80e84f0) that BULK-OVERWRITES the
         * whole array EVERY frame - so a flag at the original 0x08653438 (= array
         * +0x18, INSIDE player[0]'s 0x204 block) was wiped by that memcpy AFTER the
         * loader set it (in gsevo_post_vblank, which runs before io_ctrl) and BEFORE
         * the stub read it. RE-VERIFIED LIVE: with the flag at +0x18, even
         * ULTRA_GS_FORCE_STARTBIT=1 left swstate+8 bit0 clear at the gate. The flag
         * now lives at 0x08653418 - the 16-byte .bss alignment HOLE immediately
         * BEFORE the array (unsymboled, 0 references in the whole disasm, NOT in the
         * memcpy range) - so the loader's per-frame write survives io_ctrl intact and
         * the stub reads the real START flag. It is written each frame by
         * jvs_input.c's poll from the latched START - so when START is held the gate
         * passes and the mode handler writes main-mode-req=6 on the game's OWN path
         * (mm_rating_ctrl@0x080f5d93: `je; movl $0x6,0x8660424`). When START is idle
         * the flag is 0 -> `or` is a no-op -> attract loop byte-identical to before.
         * `or [mem],al` only ever SETS bit0 (never clears), and make_switch_data
         * already zeroes swstate+8 each frame, so there is no stuck-START risk.
         * game_continue_check reads the same field -> CONTINUE works too.
         * SHA-GATED to GSEVO; ABC/LGJ never see any of this. All addresses are
         * fixed (ET_EXEC, no ASLR for the guest image). */
        static const uint8_t GS_START_STUB[] = {
            0xA0,0x18,0x34,0x65,0x08,             /* mov al,[0x08653418] (START flag, safe hole) */
            0x08,0x05,0x28,0x34,0x65,0x08,        /* or  [0x08653428],al (swstate+8)  */
            0xE9,0x33,0x2C,0xF2,0xFF              /* jmp 0x080cffa6 (event_ctrl)      */
        };
        Patch gstub = {PT_BYTES, 0x165363, 0,
                       "GSEVO natural-START stub (set swstate+8 bit0 before event_ctrl)",
                       NULL, 0, GS_START_STUB, sizeof(GS_START_STUB)};
        apply_patch(image, image_size, &gstub);
        /* Redirect `call event_ctrl` -> `call stub` (e8 46 15 fe ff -> e8 03 e9 0b 00). */
        static const uint8_t GS_CALL_STUB[] = {0xE8,0x03,0xE9,0x0B,0x00};
        Patch gcall = {PT_BYTES, 0xa6a5b, 0,
                       "GSEVO redirect call event_ctrl -> natural-START stub",
                       NULL, 0, GS_CALL_STUB, sizeof(GS_CALL_STUB)};
        apply_patch(image, image_size, &gcall);

        /* Loader-side START flag (guest .bss byte the stub reads). jvs_input.c
         * writes 1 when START is latched, 0 when idle, every frame. MUST be the
         * 0x08653418 hole (NOT 0x08653438, which is inside the memcpy'd switch
         * array and gets wiped each frame - see the stub comment above). */
        extern uint32_t g_ul_gs_startflag_addr;
        g_ul_gs_startflag_addr = 0x08653418;
        loader_log(LOG_INFO, "GSEVO: natural-START trampoline armed (event_ctrl seam, flag @0x08653438)");

        /* ── Suppress spurious "YOU GAVE UP" while the controls land ─────────
         * Symptom diag (gs_run.err): mainmode 6 -> game_start=1 -> ~520 frames
         * of unattended CONTINUE? Y/N -> mainmode flips to 2 (attract) with the
         * "YOU GAVE UP" event sequence (start_disp_giveup + voice 0x700a9).
         *
         * Disasm-pinned (vsg ELF, unstripped):
         *   GameOver @0x080e0964 has ONE call site for start_disp_giveup:
         *     0x080e09ae: cmp byte[ebx+0x74], 0     ; gw[0x74] = state byte
         *     0x080e09b2: jns 0x80e0a17             ; if (bit7 clear) -> giveup
         *     ...
         *     0x080e0a17: call 0x80e1f78            ; start_disp_giveup
         *   Bit7 of gw[0x74] is the "stage cleared" flag set by
         *   GameStageClear@0x080dfbef/0x080e0c6e. When the player has no input
         *   reaching the game, the credit/continue timer expires without
         *   clearing a stage -> bit7 stays 0 -> giveup screen runs every loop.
         *
         * FIX (surgical, 5 bytes): NOP the `call start_disp_giveup` at file
         * offset 0x98a17 (e8 5c 15 00 00 -> 90 90 90 90 90). GameOver still
         * runs all its other cleanup; only the cosmetic "YOU GAVE UP" event is
         * suppressed. Combined with FreePlay (already set), the credit loop
         * gracefully falls through to attract. Reversible (5-byte windowed
         * patch), SHA-GATED to GSEVO. */
        static const uint8_t GS_GIVEUP_NOP[] = {0x90,0x90,0x90,0x90,0x90};
        Patch ggu = {PT_BYTES, 0x98a17, 0,
                     "GSEVO suppress GameOver->start_disp_giveup (cosmetic 'YOU GAVE UP')",
                     NULL, 0, GS_GIVEUP_NOP, sizeof(GS_GIVEUP_NOP)};
        apply_patch(image, image_size, &ggu);
        loader_log(LOG_INFO, "GSEVO: 'YOU GAVE UP' event NOP'd (call @VA 0x080e0a17 silenced)");
    }

    /* The House of the Dead 4 - dongle-bypass patches (RE'd from the unstripped
     * hod4M.elf; C++ Sega Lindbergh SDK). Different engine than ABC/GSEVO - uses
     * mangled `_ZN7CJvsMgr*` / CGunMgr / CGameMgr / CEvtMgr, not ABC's
     * is_dongle_validate/check_security_error C symbols. Two dongle validators:
     *
     *   CJvsMgr::CheckApplicationValid @VA 0x0815fdd4 (file 0x117dd4) - app-valid
     *     gate; fails 0 through amDongleUpdate/amDongleIsAvailable/amJvsGetNodes
     *     when no physical dongle. Force pass at entry: `mov eax,1; ret`.
     *   JvsMgr_CheckDongleDecrypt @VA 0x081b4f7c (file 0x16cf7c) - periodic
     *     dongle decrypt roundtrip; VOID return, writes result to [ebx+4]. Use
     *     bare `ret` (0xC3) - the caller's out-param stays at its default.
     *
     * Lightgun analog is delivered by jvs_input.c's poll_inputs overlay:
     * P1 -> g_jvs.analog[0..1] (channels 0,1), P2 -> g_jvs.analog[2..3]
     * (channels 2,3), which is exactly what CGunMgr reads via
     * CJvsMgr::GetNodeDataPtr. SHA-GATED to HOTD4; other titles untouched. */
    if (game == &GAME_HOTD4) {
        static const uint8_t HOD4_MOV1_RET[] = {0xB8,0x01,0x00,0x00,0x00,0xC3}; /* mov eax,1; ret */
        Patch hp1 = {PT_BYTES, 0x117dd4, 0,
                     "HOTD4 CJvsMgr::CheckApplicationValid -> 1 (bypass dongle app-valid gate)",
                     NULL, 0, HOD4_MOV1_RET, sizeof(HOD4_MOV1_RET)};
        apply_patch(image, image_size, &hp1);
        static const uint8_t HOD4_RET[] = {0xC3};
        Patch hp2 = {PT_BYTES, 0x16cf7c, 0,
                     "HOTD4 JvsMgr_CheckDongleDecrypt -> ret (skip dongle crypto handshake)",
                     NULL, 0, HOD4_RET, sizeof(HOD4_RET)};
        apply_patch(image, image_size, &hp2);
        loader_log(LOG_INFO, "HOTD4: dongle app-valid + decrypt handshake bypassed");
    }

    /* Cruis'n Blast - signature-verified packer self-check bypass.
     *
     * Cruis'n Blast (Raw Thrills g5) ships a packed Linux ELF32 whose bootstrap
     * calls dlopen("libc.so.6", RTLD_NOW) then dlsym for 4 candidate symbol
     * names. When ANY resolve fails, control falls through to a central
     * fail_fn @VA 0x09F018C0 via
     *   VA 0x09EF1D6B: C7 04 24 1F 00 00 00   mov [esp], 0x1F   (fail code)
     *   VA 0x09EF1D72: E8 49 FB 00 00         call 0x09F018C0   (fail_fn)
     * fail_fn always exits(1). NOP the 5-byte CALL so a failure path returns
     * cleanly instead of killing the process - the success path (jne to
     * 0x09EF1D92 at 0x09EF1B21) is untouched so a working libc boots naturally.
     *
     * File offset arithmetic (two-PT_LOAD packer layout):
     *   Segment 2 vaddr = 0x08AB6F90, p_offset[1] = 0xA6E000 (page-aligned).
     *   0x09EF1D72 - 0x08AB6F90 = 0x143ADE2
     *   file offset = 0xA6E000 + 0x143ADE2 = 0x1EA8D72
     *
     * BYTE-SIGNATURE GATE (safety-critical, no SHA yet):
     *   pre-CALL at 0x1EA8D6B (VA 0x09EF1D6B): C7 04 24 1F  (`mov [esp], 0x1F`)
     *   CALL at    0x1EA8D72 (VA 0x09EF1D72): E8 49 FB 00 00
     * If EITHER differs, do NOT patch - offset math is off for this dump. */
    if (game == &GAME_CRUISN_BLAST) {
        const size_t CB_CALL_OFF   = 0x1EA8D72;
        const size_t CB_PUSH_OFF   = 0x1EA8D6B;
        static const uint8_t CB_CALL_SIG[5] = {0xE8,0x49,0xFB,0x00,0x00};
        static const uint8_t CB_PUSH_SIG[4] = {0xC7,0x04,0x24,0x1F};
        if (CB_CALL_OFF + sizeof(CB_CALL_SIG) <= image_size &&
            CB_PUSH_OFF + sizeof(CB_PUSH_SIG) <= image_size &&
            memcmp(image + CB_CALL_OFF, CB_CALL_SIG, sizeof(CB_CALL_SIG)) == 0 &&
            memcmp(image + CB_PUSH_OFF, CB_PUSH_SIG, sizeof(CB_PUSH_SIG)) == 0) {
            static const uint8_t CB_NOP5[] = {0x90,0x90,0x90,0x90,0x90};
            Patch cbp = {PT_BYTES, CB_CALL_OFF, 0,
                         "Cruis'n Blast packer: NOP CALL fail_fn @VA 0x09EF1D72 (libc.so.6 self-check)",
                         NULL, 0, CB_NOP5, sizeof(CB_NOP5)};
            apply_patch(image, image_size, &cbp);
            loader_log(LOG_INFO, "Cruis'n Blast: packer libc.so.6 self-check bypassed (fail_fn CALL @0x1EA8D72 NOP'd)");
        } else {
            loader_log(LOG_WARN, "Cruis'n Blast: packer signature MISMATCH at 0x1EA8D72, patch SKIPPED (layout drift, or already patched)");
        }
    }

    /* EXPERIMENTAL opt-in: skip JVS I/O enumeration (init_io_jvs) to reach the
     * render loop while real baseboard-command JVS emulation is unfinished. */
    {
        char buf[8];
        if (game == &GAME_ABC &&
            GetEnvironmentVariableA("ULTRA_SKIP_MOVIE", buf, sizeof buf) > 0 && buf[0] == '1') {
            /* create_movie_handle (0x0815aa5c) -> return 0 (failure), so
             * start_movie_internal's `je` skips the attract movie instead of
             * handing ctrl_movie a garbage Sofdec player (crash at mwPlyGetStat).
             * 31 c0 c3 = xor eax,eax; ret. File offset 0x112a5c. */
            static const uint8_t RET0[] = {0x31,0xc0,0xc3};
            Patch mp = {PT_BYTES, 0x112a5c, 0, "EXPERIMENTAL: create_movie_handle -> 0 (ULTRA_SKIP_MOVIE=1)",
                        NULL, 0, RET0, sizeof(RET0)};
            apply_patch(image, image_size, &mp);
            /* Also no-op ctrl_movie (0x0815a656) so the per-frame movie task can't
             * touch the garbage Sofdec player. ret at entry (cdecl, caller cleans). */
            static const uint8_t RET[] = {0xc3};
            Patch cm = {PT_BYTES, 0x112656, 0, "EXPERIMENTAL: ctrl_movie -> ret (ULTRA_SKIP_MOVIE=1)",
                        NULL, 0, RET, sizeof(RET)};
            apply_patch(image, image_size, &cm);
        }
        if (game == &GAME_ABC &&
            GetEnvironmentVariableA("ULTRA_SKIP_JVS", buf, sizeof buf) > 0 && buf[0] == '1') {
            Patch jp = {PT_BYTES, 0x4c3fc, 0, "EXPERIMENTAL: skip init_io_jvs (ULTRA_SKIP_JVS=1)",
                        NULL, 0, SKIP_JVS, sizeof(SKIP_JVS)};
            apply_patch(image, image_size, &jp);
        }
        if (game == &GAME_ABC &&
            GetEnvironmentVariableA("ULTRA_SKIP_AUDIO", buf, sizeof buf) > 0 && buf[0] == '1') {
            Patch ap = {PT_BYTES, 0x10ec90, 0, "EXPERIMENTAL: skip init_sound (ULTRA_SKIP_AUDIO=1)",
                        NULL, 0, SKIP_AUDIO, sizeof(SKIP_AUDIO)};
            apply_patch(image, image_size, &ap);
            /* init_sound was skipped, so the per-frame sound task crashes deref'ing
             * uninit CRI handles (ctrl_sound_task -> ctrl_criaudio -> criAuPly_GetStatus).
             * ret ctrl_sound_task (vaddr 0x08157d14) too for a stable no-audio build. */
            Patch st = {PT_BYTES, 0x10fd14, 0, "EXPERIMENTAL: skip ctrl_sound_task (ULTRA_SKIP_AUDIO=1)",
                        NULL, 0, SKIP_AUDIO, sizeof(SKIP_AUDIO)};
            apply_patch(image, image_size, &st);
        }
        if (game == &GAME_ABC &&
            GetEnvironmentVariableA("ULTRA_SKIP_CAMERA", buf, sizeof buf) > 0 && buf[0] == '1') {
            Patch cp = {PT_BYTES, 0xea6a, 0, "EXPERIMENTAL: skip calc_camera_matrix (ULTRA_SKIP_CAMERA=1)",
                        NULL, 0, SKIP_CAMERA, sizeof(SKIP_CAMERA)};
            apply_patch(image, image_size, &cp);
        }
    }

    /* Generic search patches (the blind "dongle branch inversion") are a FALLBACK
     * for UNRECOGNISED titles only. The pattern 48 85 C0 75 is a real x86-64
     * `test rax,rax; jne` dongle idiom, but in 32-bit Lindbergh ELFs the same four
     * bytes occur constantly as the benign tail of `mov reg,[reg+0x48]` (the 0x48
     * disp) followed by `test eax,eax; jne` - one of the most common idioms in the
     * binary. Flipping those `75`->`EB` turns conditional branches into
     * unconditional jumps and corrupts game logic. Concretely it broke GSEVO:
     * 48 85 C0 75 at vaddr 0x080cfe8c is `mov eax,[eax+0x48]; test eax,eax; jne`
     * inside execute_ctrl_event's child-orphan loop; the flip made the loop run its
     * body with eax=0 -> write to 0x3c -> ACCESS_VIOLATION before glutMainLoop.
     * So: only run the generic search when NO specific game matched. Recognized
     * titles (ABC/LGJ/GSEVO/...) carry their own exact dongle handling and must NOT
     * be subjected to the blind flip. */
    if (!game) {
        loader_log(LOG_INFO, "Applying generic search patches (unrecognised title)...");
        for (int i = 0; i < fallback->patch_count; i++)
            apply_patch(image, image_size, &fallback->patches[i]);
    } else {
        loader_log(LOG_INFO, "Recognized title, skipping blind generic search patches");
    }

    loader_log(LOG_INFO, "Patcher done: %d patch(es) applied", result.patches_applied);
    result.success = true;
    return result;
}

/* ------------------------------------------------------------------ */
/* Public: patcher_restore                                              */
/* ------------------------------------------------------------------ */
bool patcher_restore(const char *elf_path)
{
    char bak[MAX_PATH];
    snprintf(bak, MAX_PATH, "%s.bak", elf_path);
    if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
        loader_log(LOG_ERROR, "No backup found: %s", bak);
        return false;
    }
    if (!CopyFileA(bak, elf_path, FALSE)) {
        loader_log(LOG_ERROR, "Restore failed: %lu", GetLastError());
        return false;
    }
    loader_log(LOG_INFO, "Restored from %s", bak);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public: patcher_dry_run                                              */
/* ------------------------------------------------------------------ */
void patcher_dry_run(const char *elf_path)
{
    printf("\nUltra Loader Patch Preview\n");
    printf("File: %s\n", elf_path);

    FILE *f = fopen(elf_path, "rb");
    if (!f) { printf("Cannot open file.\n"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (buf) { fread(buf, 1, sz, f); }
    fclose(f);

    char sha256[65] = "unknown";
    if (buf) { sha256_memory(buf, sz, sha256); free(buf); }
    printf("SHA-256: %s\n\n", sha256);

    const Game *game = NULL;
    for (int i = 0; i < GAME_DB_COUNT - 1; i++) {
        if (GAME_DB[i]->sha256_prefix &&
            (_strnicmp(sha256, GAME_DB[i]->sha256_prefix, 16) == 0 ||
             (GAME_DB[i] == &GAME_ABC && _strnicmp(sha256, "39bb5668b0366099", 16) == 0))) {
            game = GAME_DB[i]; break;
        }
    }

    if (game) {
        printf("Identified: %s\n", game->name);
        for (int i = 0; i < game->patch_count; i++)
            printf("  [%d] 0x%llx - %s\n", i+1,
                   (unsigned long long)game->patches[i].offset,
                   game->patches[i].desc);
    } else {
        printf("Unknown, generic search patches will apply\n");
    }
    printf("\n");
}
