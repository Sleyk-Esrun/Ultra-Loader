#!/usr/bin/env bash
# Build taito_nesica's OWN independent I/O hook DLL (both bitnesses):
#   ultra_taito_nesica_hook32.dll  -- injected into 32-bit NLX1 games (the norm)
#   ultra_taito_nesica_hook64.dll  -- injected into a rare 64-bit NLX1 repack
#
# This DLL provides the clean-room JVS-over-serial engine, the DirectInput8
# emulation, the D3D9 frame-limiter/present hook, the DirectSound/XAudio2
# diagnostic hook, the (env-gated, OFF by default) NESiCA/Aime card-reader
# presence stub, and Ultra Omni-Bridge (5th Omni service -- gamepad/lightgun
# -> synthesized OS cursor/click/key input, opt-in via ULTRA_BRIDGE_ENABLE,
# default OFF). It is compiled ONLY from this board's own src/hook/ tree
# (jvs_core_nesica.c, jvs_serial_hook_nesica.c, card_reader_nesica.c,
# dinput8_emu_nesica.c, d3d9_throttle_nesica.c, audio_hook_nesica.c,
# hook_dllmain_nesica.c, omni_bridge.c) -- it shares no source file, object
# file, or link step with native_x86/src/hook/'s compiled output (the legacy
# shared ultra_typex_hook{32,64}.dll built in modtest/) or with any other
# board module. omni_bridge.c is this board's own copy of the reference at
# native_x86/src/hook/omni_bridge.c, unchanged; its config comes from the
# shared header-only work/include/omni_client.h, hence the extra -I below.
# Per the project directive: every board gets its own fully independent,
# self-contained hook DLL.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_nesica && bash build_nesica_hook.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
SRC_DIR="src/hook"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

HOOK_SRC="$SRC_DIR/jvs_core_nesica.c $SRC_DIR/jvs_serial_hook_nesica.c $SRC_DIR/card_reader_nesica.c $SRC_DIR/dinput8_emu_nesica.c $SRC_DIR/d3d9_throttle_nesica.c $SRC_DIR/audio_hook_nesica.c $SRC_DIR/omni_bridge.c $SRC_DIR/pause_menu_wire_nesica.c ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c $SRC_DIR/hook_dllmain_nesica.c ../../../../work/src/ultra_transport.c"

# Loader's shared, header-only include dir (work/include/omni_client.h) --
# needed only by omni_bridge.c above. This script runs from the board root
# (unlike some boards' build_*_hook.sh which live inside src/hook/), so it is
# 4 levels up to I:/Ultra Loader: taito_nesica -> modules ->
# ultra-loader-modules -> modules_pkg -> (repo root) -> work/include.
# Header-only: does not create a compiled/linked dependency on any other
# board or on native_x86.
LOADER_INC="../../../../work/include"

# -static/-static-libgcc: the DLL must be self-contained (no libgcc_s/
# libwinpthread dependency) so it loads inside the injected game process,
# which has no mingw runtime of its own. -pipe avoids a temp .s file (MSYS
# strips TMP/TEMP from native children so cc1 would otherwise try to write
# into the unwritable C:\WINDOWS).
CFLAGS=(-O2 -w -fcommon -fno-strict-aliasing -pipe -static -static-libgcc
        -D_WIN32_WINNT=0x0601 "-I$SRC_DIR" "-I$LOADER_INC")
LDFLAGS=(-shared -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32)

build_one() {
    local BITS="$1" CC="$2" BINDIR="$3" OUT="$4"
    echo "[taito_nesica] building $OUT ($BITS-bit independent I/O hook)"
    PATH="$BINDIR:$PATH" env $T "$CC" -m"$BITS" "${CFLAGS[@]}" $HOOK_SRC \
        -o "$OUT" "${LDFLAGS[@]}" -Wl,--out-implib,"${OUT%.dll}.lib"
    ls -la "$OUT"
    if "$BINDIR/objdump.exe" -p "$OUT" | grep -q "ultra_taito_nesica_hook_id"; then
        echo "[taito_nesica] export OK: ultra_taito_nesica_hook_id ($OUT)"
    else
        echo "[taito_nesica] ERROR: ultra_taito_nesica_hook_id not exported ($OUT)" >&2
        exit 1
    fi
}

build_one 32 i686-w64-mingw32-gcc   "$M32" "ultra_taito_nesica_hook32.dll"
build_one 64 x86_64-w64-mingw32-gcc "$M64" "ultra_taito_nesica_hook64.dll"

echo "[taito_nesica] done -- ultra_taito_nesica_hook32.dll + ultra_taito_nesica_hook64.dll built independently"
