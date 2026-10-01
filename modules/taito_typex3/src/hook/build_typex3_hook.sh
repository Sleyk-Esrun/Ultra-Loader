#!/usr/bin/env bash
# =============================================================================
#  build_typex3_hook.sh -- build Taito Type X3's OWN, independent I/O hook DLL.
# =============================================================================
#
#  Produces (in the module root, modules/taito_typex3/):
#    ultra_taito_typex3_hook32.dll   -- 32-bit hook (matches HOOK32 in taito_typex3_main.c)
#    ultra_taito_typex3_hook64.dll   -- 64-bit hook (matches HOOK64 in taito_typex3_main.c)
#
#  Compiles ONLY the sources in THIS directory (src/hook/). No file here
#  #includes, links against, or is copied at runtime from
#  modules/native_x86/src/hook/ or from I:\Ultra Loader\modtest\ -- every
#  Type X3 board hook is fully self-contained, per the project owner's
#  per-board directive (every modern arcade board gets its own independent
#  hook DLL; no shared runtime binary with any other board).
#
#  CFLAGS/LDFLAGS/TMP-override pattern modeled on
#  modules/native_x86/src/hook/build_hook.sh (read there ONLY as a working
#  mingw-invocation reference -- this script shares no source or binary with
#  that build).
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/taito_typex3/src/hook && bash build_typex3_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
MODROOT="$(cd "$HERE/../.." && pwd)"   # modules/taito_typex3

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[t3-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[t3-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    exit 2
fi

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_typex3.c taito_fastio_typex3.c d3d9_throttle_typex3.c audio_hook_typex3.c omni_bridge.c cursor_typex3.c pause_menu_wire_typex3.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_typex3.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c ../../../../../../work/src/ultra_touch.c"

# omni_bridge.c's #include "omni_client.h" resolves against the loader's
# shared work/include/ (the same header-only, no-alloc, env-var-backed config
# struct every other Omni service client uses) -- NOT a copy, NOT a shared
# compiled binary, just the one shared config header. Path modeled on
# modules/native_x86/src/hook/build_hook.sh's LOADER_INC (same tree depth:
# modules/taito_typex3/src/hook -> ../../../../../../work/include).
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no libgcc_s/libwinpthread dependency
# once injected into the game process (which has no mingw runtime of its own).
# -pipe: avoids the MSYS TMP-stripping cc1/as temp-file failure.
CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter -pipe -static -static-libgcc"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="$MODROOT/ultra_taito_typex3_hook32.dll"
OUT64="$MODROOT/ultra_taito_typex3_hook64.dll"

echo "[t3-hook-build] compiling ultra_taito_typex3_hook32.dll (32-bit)"
echo "  sources: $SRCS"
PATH="$M32:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -m32 -shared -o "$OUT32" $SRCS $LIBS
ls -la "$OUT32"

echo "[t3-hook-build] compiling ultra_taito_typex3_hook64.dll (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -m64 -shared -o "$OUT64" $SRCS $LIBS
ls -la "$OUT64"

# Verify the identity export on each artifact (confirms the board's OWN build
# was linked in, not a placeholder).
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT32" | grep -q "ultra_taito_typex3_hook_id"; then
    echo "[t3-hook-build] export OK (32-bit): ultra_taito_typex3_hook_id"
else
    echo "[t3-hook-build] ERROR: ultra_taito_typex3_hook_id not exported (32-bit)" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p "$OUT64" | grep -q "ultra_taito_typex3_hook_id"; then
    echo "[t3-hook-build] export OK (64-bit): ultra_taito_typex3_hook_id"
else
    echo "[t3-hook-build] ERROR: ultra_taito_typex3_hook_id not exported (64-bit)" >&2
    exit 1
fi

echo "[t3-hook-build] DONE -- artifacts: $OUT32 , $OUT64"
