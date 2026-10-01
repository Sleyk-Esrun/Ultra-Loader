#!/usr/bin/env bash
# Build taito_typex's OWN independent hook DLL (ultra_taito_typex_hook{32,64}.dll).
#
# 2026-08-01: this board was found to still stage its hook by copying
# modtest/ultra_typex_hook{32,64}.dll at build time -- a violation of the
# standing directive that every board module must have its own fully
# independent, self-contained hook DLL with no shared runtime binary or
# build/run dependency on native_x86/src/hook/ or modtest/. HIGH STAKES:
# Chaos Breaker is a real, verified-playable game on this board (full credit
# loop, attract->coin->char-select->gameplay, screenshot-confirmed) -- so this
# fix compiles this board's OWN existing (already-copied, just never actually
# built) source tree, unchanged, rather than rewriting the I/O logic from
# scratch. gamestate_probe.c's exit-gate probe (the AOE_CHAOS_BREAKER-gated
# poke that keeps the JVS I/O watchdog from exiting the game) and
# hook_dllmain.c's inline ULTRA_DEMO_INPUT demo-autopilot branch are BOTH
# already sitting in this exact source tree (copied 2026-06-28, before the
# native_x86 board_io_ops refactor existed) -- carried over byte-for-byte.
# Only genuinely new addition: audio_hook.c (DirectSound+XAudio2 -- this
# capability did not exist anywhere in the project before this session).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# Exactly the file set this board's src/ already had copied (2026-06-28, plus
# the individually-re-synced board_profile.c/jvs_core.c/d3d9/d3d11/gl_present
# from 2026-07-05/06) -- i.e. byte-identical composition to the shared hook
# DLL this board was verified playable against, plus the new audio_hook.c.
# namco_dongle.c/rawthrills_io.c/pause_menu_wire.c were added to native_x86
# AFTER this board's copy was made (2026-06-28) -- they aren't part of the
# file set Chaos Breaker was actually verified playable against, so they are
# correctly absent here, not an oversight.
HOOK_SRC="jvs_core.c jvs_serial_hook.c dongle_bypass.c hook_dllmain.c dinput8_emu.c gamestate_probe.c d3d8_throttle.c d3d8_sharpen.c d3d9_throttle.c typex2_gun.c sega_board.c d3d11_throttle.c gl_present.c sega_keychip.c board_profile.c taito_fastio.c card_reader.c codepage_spoof.c sega_io4.c konami_io.c namco_amdaemon.c namco_io.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c audio_hook.c omni_bridge.c pause_menu_wire_typex.c ../../../../../work/src/ultra_transport.c"

# pause_menu_wire_typex.c links the loader's OWN shared pause/settings-menu
# service (work/src/pause_menu_service.c) -- same source of truth every other
# board uses, not a copy. omni_bridge.c reads its tuning from the same
# loader's shared, header-only work/include/omni_client.h. Both ADDED
# 2026-08-02 (see this script's header note above about them being absent
# from the original 2026-06-28 copy) -- the project owner explicitly asked
# for pause menu + Omni-Bridge across every modern board this pass.
LOADER_INC="../../../../../work/include"
LOADER_SRC_DIR="../../../../../work/src"
PAUSE_SRC="$LOADER_SRC_DIR/pause_menu_service.c"
PAUSE_SRC_IO="$LOADER_SRC_DIR/pause_menu_settings_io.c"

CFLAGS="-std=c11 -O2 -Wall -w -pipe -static -static-libgcc -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi -ldsound"

echo "[typex-hook] 32-bit"
PATH="$M32:$PATH" env $T gcc $CFLAGS -I. -I"$LOADER_INC" -shared -o ultra_taito_typex_hook32.dll $HOOK_SRC "$PAUSE_SRC" "$PAUSE_SRC_IO" $LIBS

echo "[typex-hook] 64-bit"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I. -I"$LOADER_INC" -shared -o ultra_taito_typex_hook64.dll $HOOK_SRC "$PAUSE_SRC" "$PAUSE_SRC_IO" $LIBS

ls -la ultra_taito_typex_hook32.dll ultra_taito_typex_hook64.dll
