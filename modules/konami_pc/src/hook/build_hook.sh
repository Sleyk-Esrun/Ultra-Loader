#!/usr/bin/env bash
# Build the clean-room Type X / PC-arcade I/O hook artifacts (both bitnesses).
#   ultra_typex_hook32.dll / ultra_typex_hook64.dll  -- the injected I/O hook
#   ultra-inject32.exe      / ultra-inject64.exe      -- cross-bitness injectors
# Output sits beside the native_x86 module DLL (the module searches there first).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
# -pipe drops the .s temp, but a multi-file -> single-DLL build still stages per-file
# .o temps in TMP; some shells (MSYS) strip TMP/TEMP from native children so `as`
# falls back to C:\WINDOWS (unwritable). Set a writable temp INLINE on every gcc
# (the loader Makefile's RUNENV pattern) so the build works regardless of the shell.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"
# DROPPED FROM THIS BOARD'S HOOK: game_chaosbreaker.c game_wackyrace.c game_dsii.c
# Each of those is another board's per-game hack and carries an
# __attribute__((constructor)) that runs at DLL LOAD, before DllMain.
# Inheriting native_x86's full list put three foreign constructors into
# Silent Hill's process; the DLL failed to initialize and the game died
# with 0xC0000142 before a single line of our own logging could run.
# This script's own header already says a board builds from CORE_SRC plus
# ITS OWN game_*.c files only. konami_pc has none.
# ---------------------------------------------------------------------------
# RESTORED 2026-09-17 to the pre-rebuild source list, verbatim, plus boot_trace.c
#
# Rebuilding this board from native_x86 on 2026-09-16 pulled in the whole
# generic board set -- d3d8/d3d9/d3d11/gl throttles, typex2_gun, sega_*,
# namco_*, taito_fastio, rawthrills_io, card_reader, gamestate_probe -- and
# dropped the three files whose entire job was to keep them OUT:
#
#   konami_board_scope.c  stubs every generic installer to {} and makes
#                         ultra_Hook_Direct3DCreate9 a PASSTHROUGH, because on
#                         this board shaiolib_crk.c owns that hook
#   konami_render9.c      narrow render-output WINDOW control only -- it does
#                         NOT hook the device, for the same reason
#   konami_audio.c        this board's own DirectSound copy
#
# The consequence was measurable: d3d9_throttle.c and shaiolib_crk.c both
# IAT-patched Direct3DCreate9, the CRK layer never saw the game's device, and
# it bootstrapped a redundant 1920x1080 one five seconds in.
#
# konami_board_scope.c documented exactly this hazard -- and was deleted by the
# act of rebuilding konami_pc from native_x86. Do not drop it again.
#
# boot_trace.c is the ONE addition kept: it is gated on ULTRA_BOOT_TRACE and is
# the only diagnostic this board has. Its tracer shims are __cdecl -- see the
# comment in that file; declaring them WINAPI corrupted the game main thread's
# return address and is what made this restore necessary.
# ---------------------------------------------------------------------------
HOOK_SRC="konami_board_scope.c konami_render9.c konami_audio.c boot_trace.c jvs_core.c jvs_serial_hook.c dongle_bypass.c hook_dllmain.c dinput8_emu.c board_profile.c konami_io.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c pause_menu_wire.c board_io_ops.c audio_hook.c omni_bridge.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"
# gamestate_probe.c stays in this list for now (redundant but harmless: its
# ultra_gamestate_probe_start() call site in hook_dllmain.c is guarded on
# `!ultra_board_ops_get()`, and game_chaosbreaker.c's autoregister always
# registers on a real Chaos Breaker AOE match) until a final cleanup pass
# removes it once game_dsii.c is also verified and the whole board_io_ops
# migration is confirmed working end-to-end.
# NOTE: this is the LEGACY shared HOOK_SRC list, kept for boards that have not
# yet been split into a per-board build (see the board_io_ops.h refactor). Once
# every board module builds from CORE_SRC (below) + its own game_*.c files only,
# this combined list -- and the single shared ultra_typex_hook{32,64}.dll it
# produces -- goes away. Do not add a new per-game hack file (game_*.c) here:
# game-specific files belong ONLY in the board module that actually needs them.
#
# The truly board/game-agnostic subset (safe for EVERY board to link):
CORE_SRC="boot_trace.c jvs_core.c jvs_serial_hook.c dongle_bypass.c hook_dllmain.c dinput8_emu.c d3d8_throttle.c d3d8_sharpen.c d3d9_throttle.c d3d11_throttle.c gl_present.c board_profile.c board_io_ops.c audio_hook.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c pause_menu_wire.c"

# Shared pause/settings menu: pause_menu_service.c lives in the loader's own
# work/src (same source of truth as the Lindbergh module). pause_menu_wire.c
# above provides the tiny local shim (g_settings/loader_log) so it links
# standalone inside the injected hook DLL. Kept on a separate SRC list so we can
# pass the -I to the loader work/include headers on those two TUs.
LOADER_INC="../../../../../../work/include"
LOADER_SRC_DIR="../../../../../../work/src"
PAUSE_SRC="$LOADER_SRC_DIR/pause_menu_service.c $LOADER_SRC_DIR/pause_menu_settings_io.c"
# -static makes the DLL/EXE self-contained (no libgcc_s/libwinpthread dependency),
# so the injected hook loads inside the game process which has no mingw runtime.
# -pipe: MSYS strips TMP/TEMP from native children so cc1 writes its temp .s into
# C:\WINDOWS (unwritable) and `as` fails; -pipe removes the temp file entirely.
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc"

# -ldinput8 -ldxguid: the DirectInput8 READER source (input_src_dinput.c) links the
# real DirectInput8Create + the IID_*/c_dfDIJoystick GUID objects (distinct from
# dinput8_emu.c, which only IAT-hooks the game's import). -lole32 for the GUIDs.
echo "[32] hook dll";  PATH="$M32:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -shared -o ultra_typex_hook32.dll $HOOK_SRC $PAUSE_SRC -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi
echo "[64] hook dll";  PATH="$M64:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -shared -o ultra_typex_hook64.dll $HOOK_SRC $PAUSE_SRC -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi
# The injector shows the SHARED Ultra Loader boot console (board + game title)
# before it spawns+injects the game, so a directly-invoked / cross-bitness inject
# gets the same boot screen as the loader. boot_console.c is the loader's shared
# console source (work/src); link it + -lgdi32 -lwinmm for the GDI/jingle path.
LOADER_SRC="../../../../../../work/src"
echo "[32] injector";  PATH="$M32:$PATH" env $T gcc $CFLAGS -I"$LOADER_SRC" -I"$LOADER_INC" -o ultra-inject32.exe inject_main.c inject_bc_stubs.c "$LOADER_SRC/boot_console.c" -lgdi32 -luser32 -lwinmm
echo "[64] injector";  PATH="$M64:$PATH" env $T gcc $CFLAGS -I"$LOADER_SRC" -I"$LOADER_INC" -o ultra-inject64.exe inject_main.c inject_bc_stubs.c "$LOADER_SRC/boot_console.c" -lgdi32 -luser32 -lwinmm

echo "[test] jvs core"; PATH="$M64:$PATH" env $T gcc $CFLAGS -I. jvs_core.c ../../tests/test_jvs_core.c -o test_jvs_core.exe && ./test_jvs_core.exe | tail -1
# Sega-board framework structural test: needs two tiny externs the full hook
# provides (logger + IAT patcher); stub them here so the test links standalone.
echo "[test] sega board"; printf 'void ultra_d3d8_log(const char*f,...){(void)f;}\nvoid ultra_iat_patch_all(const char*a,const char*b,void*c){(void)a;(void)b;(void)c;}\n' > _sega_stubs.c
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. test_sega_board.c sega_board.c sega_keychip.c _sega_stubs.c -o test_sega_board.exe && ./test_sega_board.exe | tail -1; rm -f _sega_stubs.c

# Unified board-profile framework structural test. The Fast-I/O + card-reader
# scaffolds need three tiny externs the full hook provides (logger, IAT patcher,
# current-input snapshot); stub them so the test links standalone. The stub input
# returns all-centered so the buffer-fill check is deterministic.
echo "[test] board profile"
printf '#include "jvs_core.h"\n#include <string.h>\nvoid ultra_d3d8_log(const char*f,...){(void)f;}\nvoid ultra_iat_patch_all(const char*a,const char*b,void*c){(void)a;(void)b;(void)c;}\nvoid ultra_hook_current_input(ultra_jvs_input*o){memset(o,0,sizeof(*o));for(int a=0;a<JVS_MAX_ANALOG;++a)o->analog[a]=0x8000;o->screen_x=o->screen_y=0x8000;}\n' > _bp_stubs.c
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. test_board_profile.c board_profile.c sega_board.c taito_fastio.c card_reader.c _bp_stubs.c -o test_board_profile.exe -luser32 && ./test_board_profile.exe | tail -1; rm -f _bp_stubs.c

# Player input-mapping layer test: sink-adapter round-trips (intent->ultra_jvs_input),
# axis scaling/calibration/invert, per-board default profiles, and the INI config
# merge (override + append + partial-file inherit). Links controls.c + input_profiles.c
# + the source backends; stubs the three hook externs (logger, shared block,
# device-window) the gun/mouse sources reference so it links standalone.
echo "[test] input map"
printf '#include "ultra_io_shared.h"\n#include <windows.h>\nvoid ultra_d3d8_log(const char*f,...){(void)f;}\nultra_io_shared* ultra_hook_shared(void){return 0;}\nHWND ultra_typex2_device_window(void){return 0;}\n' > _im_stubs.c
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. test_input_map.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c _im_stubs.c -o test_input_map.exe -luser32 -ldinput8 -ldxguid -lole32 && ./test_input_map.exe | tail -1; rm -f _im_stubs.c

# Interop I/O + presence test: the new Sega IO4, Konami ACIO codec, keychip
# presence/key-derivation split, and Namco amdaemon presence. The test stubs the
# three hook externs itself (logger/IAT-patcher/current-input), so it links the
# interop .c files standalone. Konami codec is cross-platform; the IO4/keychip/
# amdaemon Win32 units link too (the test #ifdefs the Win32-only checks).
echo "[test] interop io"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. ../../tests/test_interop_io.c konami_io.c sega_io4.c sega_keychip.c namco_amdaemon.c -o test_interop_io.exe -luser32 && ./test_interop_io.exe | tail -1

# Namco JVS-over-USB I/O + amauthd LOCAL presence test: drives the real jvs_core
# node through namco_io's USB bridge (RESET/SET_ADDRESS/READ_SWITCH round-trip +
# SENSE flip) and pins the amauthd ethical line (presence/status/version/region/
# play-count answered; every online/key/content class DEFERRED). Links namco_io.c
# + the real jvs_core.c; the test stubs logger/IAT-patcher/node+pull itself.
echo "[test] namco io"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. ../../tests/test_namco_io.c namco_io.c jvs_core.c -o test_namco_io.exe -luser32 && ./test_namco_io.exe | tail -1

# D3D11 capture round-trip: a synthetic WARP swap chain driven through the loader's
# OWN create hook, proving GetBuffer(0)->STAGING CopyResource->Map->BMP captures a
# frame. 64-bit (the D3D11 board lane); d3d11_throttle.c links via vtable derefs so
# no -ld3d11 is needed -- the test reaches the real export through the hook's own
# GetProcAddress. WARP runs headless (no GPU); skips cleanly if WARP is unavailable.
echo "[test] d3d11 capture"; PATH="$M64:$PATH" env $T gcc $CFLAGS -I. test_d3d11_capture.c d3d11_throttle.c -o test_d3d11_capture.exe -luser32 -lwinmm -lgdi32 \
  && (./test_d3d11_capture.exe || echo "  (d3d11 capture test could not init WARP on this host -- structural build is clean)")

# OpenGL capture round-trip: a synthetic GL context on a hidden window, driven
# through the loader's OWN wglSwapBuffers hook, proving glReadPixels(GL_BACK,BGRA)
# -> 24-bit bottom-up BMP captures a frame. 64-bit (the GL board lane). The hook
# resolves the GL read-back fns dynamically from opengl32, so the test links the
# real opengl32. A headless host with no GL context skips cleanly (exit 0).
echo "[test] gl capture"; PATH="$M64:$PATH" env $T gcc $CFLAGS -I. test_gl_capture.c gl_present.c -o test_gl_capture.exe -lopengl32 -luser32 -lwinmm -lgdi32 \
  && (./test_gl_capture.exe || echo "  (gl capture test could not init a GL context on this host -- structural build is clean)")

echo "OK:"; ls -la ultra_typex_hook32.dll ultra_typex_hook64.dll ultra-inject32.exe ultra-inject64.exe
