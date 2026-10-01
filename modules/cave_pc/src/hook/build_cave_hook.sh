#!/usr/bin/env bash
# Build Ultra Loader's Cave-PC-ONLY hook DLL.
#   ultra_cave_pc_hook32.dll -- the injected I/O hook for Cave PC (Ver.A/B) games
#
# 2026-08-02 REBUILD: this board's hook was rebuilt a THIRD time today. Timeline:
#   1. Pre-2026-07-23: shared modules/native_x86/src/hook/'s multi-board
#      ultra_typex_hook32.dll, with Deathsmiles II's I/O isolated into
#      game_dsii.c (a board_io_ops.h per-game callback module). Project memory
#      records this combination as having gotten DSII's "crash+loadtime+gate
#      FIXED, general input confirmed".
#   2. 2026-07-23..2026-08-02 morning: forked into a from-scratch independent
#      rewrite (cave_plumbing.c/cave_io.c/cave_render9.c/cave_dialog.c) to stop
#      a fix for one board on the shared binary from regressing another. Real,
#      renders video, but reproducibly crashed (STATUS_INTEGER_DIVIDE_BY_ZERO)
#      on most live runs -- not confirmed to be a hook regression (the crash
#      site is deep in game.exe's own resource-loading code) but never cleanly
#      re-characterized against the proven architecture either, until now.
#   3. THIS REBUILD (owner's explicit direction): swap back to the PROVEN
#      SOURCE (game_dsii.c + the native_x86 board_io_ops-based hook engine),
#      ported into this board's OWN independent src/hook/ tree -- same pattern
#      already used for taito_typex/taito_typex2 earlier the same day (copy the
#      proven source wholesale into the board's own tree; no shared compiled
#      binary, ever). Every .c/.h in this directory's SRCS list below except
#      omni_bridge.c/.h is an unmodified copy of modules/native_x86/src/hook/'s
#      2026-07-31 tree (the exact file set native_x86/src/hook/build_hook.sh's
#      HOOK_SRC compiles, minus game_chaosbreaker.c/game_wackyrace.c -- other
#      boards' per-game hacks, deliberately not linked here per board_io_ops.h's
#      own per-game isolation principle). hook_dllmain.c carries two small,
#      clearly-commented edits on top of the verbatim port: Ultra Omni-Bridge
#      wiring, and a crash handler that logs every fatal exception code (not
#      just ACCESS_VIOLATION) -- see that file's own header comment.
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook32.dll: this board's own source tree, own build
# script, own compiled binary. A rebuild of the shared native_x86 hook, or of
# any OTHER board's own hook, can never again change what Deathsmiles II (or
# any future Cave PC dump) renders, sounds like, or is controlled with.
#
# 32-bit only -- the whole Cave PC mainline catalog (Deathsmiles II, DoDonPachi
# SaiDaiOuJou original, Akai Katana Shin, Espgaluda II, Ketsui, Muchi Muchi
# Pork) ships 32-bit; only SDOJ "Peak" (the D3D11 remaster) is 64-bit and is
# out of scope until a real dump is tested.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/cave_pc/src/hook && bash build_cave_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# Board-agnostic engine (ported verbatim from native_x86/src/hook/, see this
# script's header) + this board's own game_dsii.c + omni_bridge.c (this
# board's own copy of the reference implementation, per-board independence
# rule -- see omni_bridge.h's file header). Deliberately EXCLUDES
# game_chaosbreaker.c and game_wackyrace.c (other boards' per-game hacks).
SRCS="jvs_core.c jvs_serial_hook.c dongle_bypass.c namco_dongle.c rawthrills_io.c hook_dllmain.c dinput8_emu.c gamestate_probe.c d3d8_throttle.c d3d8_sharpen.c d3d9_throttle.c typex2_gun.c sega_board.c d3d11_throttle.c gl_present.c sega_keychip.c board_profile.c taito_fastio.c card_reader.c sega_io4.c konami_io.c namco_amdaemon.c namco_io.c controls.c input_profiles.c input_src_keyboard.c input_src_xinput.c input_src_dinput.c input_src_mouse.c input_src_gun.c input_autopilot.c pause_menu_wire.c board_io_ops.c audio_hook.c game_dsii.c game_cave_omatsuri.c omni_bridge.c ../../../../../../work/src/ultra_tracer.c"

# pause_menu_wire.c links the loader's OWN shared pause/settings-menu service
# (work/src/pause_menu_service.c) -- same source of truth every other board
# that ports this file set uses, not a copy (native_x86/src/hook/build_hook.sh
# does the exact same thing at the exact same relative depth). omni_bridge.c
# reads its tuning from the same loader's shared, header-only
# work/include/omni_client.h.
LOADER_INC="../../../../../../work/include"
LOADER_SRC_DIR="../../../../../../work/src"
PAUSE_SRC="$LOADER_SRC_DIR/pause_menu_service.c"
PAUSE_SRC_IO="$LOADER_SRC_DIR/pause_menu_settings_io.c"
# Universal arcade I/O transport layer (pacing + calibrated ranges + codecs).
# Shared SOURCE per the per-board independence rule -- same pattern as above.
UTX_SRC="$LOADER_SRC_DIR/ultra_transport.c"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as the shared hook's build).
CFLAGS="-std=c11 -O2 -Wall -w -pipe -static -static-libgcc -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601 -m32"
OUT="ultra_cave_pc_hook32.dll"

echo "[cave_pc hook] building $OUT (ported board_io_ops/game_dsii.c architecture)"
echo "  sources: $SRCS $PAUSE_SRC"
# -ldinput8 -ldxguid -lole32: the DirectInput8 READER source (input_src_dinput.c)
# links the real DirectInput8Create + IID_*/c_dfDIJoystick GUID objects
# (distinct from dinput8_emu.c, which only IAT-hooks the game's import).
env $T "$CC" $CFLAGS -I. -I"$LOADER_INC" -shared -o "$OUT" $SRCS "$PAUSE_SRC" "$PAUSE_SRC_IO" "$UTX_SRC" \
    -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi

ls -la "$OUT"

# Verify the identity export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ultra_cave_pc_hook_id"; then
    echo "[cave_pc hook] export OK: ultra_cave_pc_hook_id"
else
    echo "[cave_pc hook] ERROR: ultra_cave_pc_hook_id not exported" >&2
    exit 1
fi

# Stage next to the module DLL in this project's completed-cores folder
# (mirrors cave_pc/build_module.sh's own artifact drop).
PROJ="$HERE/../../../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Cave PC"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[cave_pc hook] artifact -> $COMPLETED_DIR/$OUT"

echo "[cave_pc hook] done"
