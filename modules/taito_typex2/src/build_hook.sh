#!/usr/bin/env bash
# Build taito_typex2's OWN independent hook DLL (ultra_taito_typex2_hook{32,64}.dll).
#
# 2026-08-01: this board was found to still stage its hook by copying
# modtest/ultra_typex_hook{32,64}.dll at build time -- a violation of the
# standing directive that every board module must have its own fully
# independent, self-contained hook DLL with no shared runtime binary or
# build/run dependency on native_x86/src/hook/ or modtest/. HIGH STAKES:
# Haunted Museum and Wacky Race are both real, verified-playable games on
# this board (full credit loop, screenshot-confirmed) -- so this fix compiles
# this board's OWN existing (already-copied, just never actually built)
# source tree, unchanged, rather than rewriting the I/O logic from scratch.
# hook_dllmain.c's inline ULTRA_WR_AUTO Wacky Race autopilot branch is already
# sitting in this exact source tree (copied 2026-06-28) -- carried over
# byte-for-byte.
#
# 2026-09-24 REMOVED: gamestate_probe.c was ALSO carried over in that same
# 2026-06-28 copy, but it should never have been -- it is a Chaos Breaker-only
# diagnostic (every VA it reads/pokes is a hardcoded offset into that one
# Type X game's memory layout; Chaos Breaker is not a Type X2 title at all).
# Its poke side was correctly gated on the host image's entry-point RVA, but
# its read side was not, so it ran unconditionally for every Type X2 game
# whenever a log path was set (i.e. virtually always) and dereferenced
# whatever garbage those Chaos-Breaker-specific offsets happened to land on
# in an unrelated game's memory -- crashed Haunted Museum II reading a bogus
# menu-object pointer at a hardcoded VA that meant nothing on this title.
# Deleted rather than re-gated: this file has no legitimate reason to be
# built into this board at all.
#
# This tree's hook_dllmain.c is BYTE-IDENTICAL to taito_typex's own copy
# (same 29528-byte file, confirmed via diff) but this board's src/ was
# missing 3 files hook_dllmain.c actually calls: dinput8_emu.c
# (ultra_dinput8_install), d3d8_throttle.c (ultra_d3d8_teardown), and its
# sibling d3d8_sharpen.c -- copied byte-for-byte from taito_typex's src/
# (same Jun 28 provenance, already proven to link+run together in typex's own
# just-verified independent build) rather than reintroducing a dependency on
# native_x86 or modtest. Only genuinely new addition on top of that: audio_hook.c
# (DirectSound+XAudio2 -- this capability did not exist anywhere in the
# project before this session).
#
# inject_main.c (builds ultra-inject32/64.exe, a standalone same-bitness DLL
# injector helper) and native_x86_module.cpp (a leftover full copy of the
# native_x86 module's own C++ entry point) are NOT part of the hook DLL --
# correctly excluded below, same as they were never part of taito_typex's
# HOOK_SRC either.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

HOOK_SRC="jvs_core.c jvs_serial_hook.c dongle_bypass.c hook_dllmain.c dinput8_emu.c d3d8_throttle.c d3d8_sharpen.c d3d9_throttle.c typex2_gun.c sega_board.c d3d11_throttle.c gl_present.c sega_keychip.c board_profile.c taito_fastio.c card_reader.c sega_io4.c konami_io.c namco_amdaemon.c namco_io.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c audio_hook.c omni_bridge.c pause_menu_wire_typex2.c ../../../../../work/src/ultra_transport.c ../../../../../work/src/ultra_tracer.c"

# pause_menu_wire_typex2.c links the loader's OWN shared pause/settings-menu
# service (work/src/pause_menu_service.c) -- same source of truth every other
# board uses, not a copy. omni_bridge.c reads its tuning from the same
# loader's shared, header-only work/include/omni_client.h.
LOADER_INC="../../../../../work/include"
LOADER_SRC_DIR="../../../../../work/src"
PAUSE_SRC="$LOADER_SRC_DIR/pause_menu_service.c"
PAUSE_SRC_IO="$LOADER_SRC_DIR/pause_menu_settings_io.c"

CFLAGS="-std=c11 -O2 -Wall -w -pipe -static -static-libgcc -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi -ldsound"

echo "[typex2-hook] 32-bit"
PATH="$M32:$PATH" env $T gcc $CFLAGS -I. -I"$LOADER_INC" -shared -o ultra_taito_typex2_hook32.dll $HOOK_SRC "$PAUSE_SRC" "$PAUSE_SRC_IO" $LIBS

echo "[typex2-hook] 64-bit"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. -I"$LOADER_INC" -shared -o ultra_taito_typex2_hook64.dll $HOOK_SRC "$PAUSE_SRC" "$PAUSE_SRC_IO" $LIBS

ls -la ultra_taito_typex2_hook32.dll ultra_taito_typex2_hook64.dll
