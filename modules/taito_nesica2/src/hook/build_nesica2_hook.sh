#!/usr/bin/env bash
# =============================================================================
#  build_nesica2_hook.sh -- build Taito NESiCAxLive2's OWN, independent I/O
#                           hook DLL.
# =============================================================================
#
#  Produces (in the module root, modules/taito_nesica2/):
#    ultra_taito_nesica2_hook32.dll   -- 32-bit hook (rare NLX2 D3D9 repack)
#    ultra_taito_nesica2_hook64.dll   -- 64-bit hook (the normal NLX2 case;
#                                        matches HOOK32/HOOK64 in
#                                        taito_nesica2_main.c)
#
#  Compiles ONLY the sources in THIS directory (src/hook/). No file here
#  #includes, links against, or is copied at runtime from
#  modules/native_x86/src/hook/, I:\Ultra Loader\modtest\, or taito_nesica's
#  own module folder (a SEPARATE board, handled elsewhere) -- every
#  NESiCAxLive2 board hook is fully self-contained, per the project owner's
#  per-board directive (every modern arcade board gets its own independent
#  hook DLL; no shared runtime binary with any other board).
#
#  CFLAGS/LDFLAGS/TMP-override pattern modeled on
#  modules/native_x86/src/hook/build_hook.sh and the already-independent
#  modules/taito_typex3/src/hook/build_typex3_hook.sh (read there ONLY as a
#  working mingw-invocation reference -- this script shares no source or
#  binary with either).
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/taito_nesica2/src/hook && bash build_nesica2_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
MODROOT="$(cd "$HERE/../.." && pwd)"   # modules/taito_nesica2

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[nesica2-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[nesica2-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    exit 2
fi

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_nesica2.c jvs_serial_hook_nesica2.c d3d9_throttle_nesica2.c d3d11_throttle_nesica2.c audio_hook_nesica2.c card_reader_nesica2.c omni_bridge.c pause_menu_wire_nesica2.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_nesica2.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# LOADER_INC: omni_bridge.c #includes the shared, header-only, no-alloc
# work/include/omni_client.h (also used by Omni-Play/Omni-Track/Omni-Wheel/
# Omni-Steer's own config structs). This script runs from THIS directory
# (src/hook/), so it is 6 levels up to I:/Ultra Loader: hook -> src ->
# taito_nesica2 -> modules -> ultra-loader-modules -> modules_pkg -> (repo
# root) -> work/include. Same depth/pattern as every other board's own
# hook tree that lives at src/hook/ (see modules/taito_typex4/src/hook/
# build_typex4_hook.sh, modules/sega_ringedge2/src/hook/
# build_ringedge2_hook.sh for the identical convention). Header-only: does
# not create a compiled/linked dependency on native_x86 or any other board.
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no libgcc_s/libwinpthread dependency
# once injected into the game process (which has no mingw runtime of its own).
# -pipe: avoids the MSYS TMP-stripping cc1/as temp-file failure.
CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter -pipe -static -static-libgcc -I. -I$LOADER_INC"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="$MODROOT/ultra_taito_nesica2_hook32.dll"
OUT64="$MODROOT/ultra_taito_nesica2_hook64.dll"

echo "[nesica2-hook-build] compiling ultra_taito_nesica2_hook32.dll (32-bit)"
echo "  sources: $SRCS"
PATH="$M32:$PATH" env $T gcc $CFLAGS -m32 -shared -o "$OUT32" $SRCS $LIBS
ls -la "$OUT32"

echo "[nesica2-hook-build] compiling ultra_taito_nesica2_hook64.dll (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -m64 -shared -o "$OUT64" $SRCS $LIBS
ls -la "$OUT64"

# Verify the identity export on each artifact (confirms the board's OWN build
# was linked in, not a placeholder / the old copied modtest DLL).
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT32" | grep -q "ultra_taito_nesica2_hook_id"; then
    echo "[nesica2-hook-build] export OK (32-bit): ultra_taito_nesica2_hook_id"
else
    echo "[nesica2-hook-build] ERROR: ultra_taito_nesica2_hook_id not exported (32-bit)" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p "$OUT64" | grep -q "ultra_taito_nesica2_hook_id"; then
    echo "[nesica2-hook-build] export OK (64-bit): ultra_taito_nesica2_hook_id"
else
    echo "[nesica2-hook-build] ERROR: ultra_taito_nesica2_hook_id not exported (64-bit)" >&2
    exit 1
fi

echo "[nesica2-hook-build] DONE -- artifacts: $OUT32 , $OUT64"
