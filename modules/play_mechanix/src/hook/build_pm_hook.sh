#!/usr/bin/env bash
# Build Ultra Loader's Play-Mechanix-ONLY hook DLLs (both bitnesses).
#   ultra_play_mechanix_hook32.dll / ultra_play_mechanix_hook64.dll
#   -- the injected I/O hook for the Play Mechanix / Raw Thrills (Chicago PC)
#      lightgun family (Big Buck Hunter and kin).
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board hook: no shared source file, no shared compiled binary (see
# pm_hook.h for why -- per the project owner's standing per-board-
# independence directive).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/play_mechanix/src/hook && bash build_pm_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="pm_plumbing.c jvs_core_pm.c jvs_serial_hook_pm.c pm_gun_bridge.c d3d9_throttle_pm.c audio_hook_pm.c omni_bridge.c pause_menu_wire_pm.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_pm.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as every other board's hook build).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc"

# omni_bridge.c is this board's own copy of the Ultra Omni-Bridge reference
# (5th Omni service, see omni_bridge.c's header) -- it #includes the shared,
# header-only work/include/omni_client.h for its env-var-backed config, so
# (only) this one -I is needed; every other source here stays independent of
# work/include per this board's per-board-independence directive.
LOADER_INC="../../../../../../work/include"

OUT32="ultra_play_mechanix_hook32.dll"
OUT64="ultra_play_mechanix_hook64.dll"

echo "[play_mechanix hook] building $OUT32 (32-bit)"
echo "  sources: $SRCS"
PATH="$M32:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -m32 -shared -o "$OUT32" $SRCS -lpsapi -luser32 -lwinmm -lgdi32

echo "[play_mechanix hook] building $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -I"$LOADER_INC" -shared -o "$OUT64" $SRCS -lpsapi -luser32 -lwinmm -lgdi32

ls -la "$OUT32" "$OUT64"

# Verify the identity export on both (each bitness's own objdump -- mingw32's
# objdump cannot parse a PE32+ (64-bit) image and vice versa).
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT32" | grep -q "ultra_play_mechanix_hook_id"; then
    echo "[play_mechanix hook] export OK: ultra_play_mechanix_hook_id ($OUT32)"
else
    echo "[play_mechanix hook] ERROR: ultra_play_mechanix_hook_id not exported in $OUT32" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p "$OUT64" | grep -q "ultra_play_mechanix_hook_id"; then
    echo "[play_mechanix hook] export OK: ultra_play_mechanix_hook_id ($OUT64)"
else
    echo "[play_mechanix hook] ERROR: ultra_play_mechanix_hook_id not exported in $OUT64" >&2
    exit 1
fi

# Stage next to the module DLL (play_mechanix_main.c's find_hook_named() looks
# in %LOCALAPPDATA%\Ultra Loader\modules\play_mechanix\, the loader dir, and
# loader dir\modtest\ -- the module's own build_module.sh copies these there).
PROJ="$HERE/../../../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Play Mechanix"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT32" "$COMPLETED_DIR/$OUT32"
cp -f "$OUT64" "$COMPLETED_DIR/$OUT64"
echo "[play_mechanix hook] artifacts -> $COMPLETED_DIR/"

echo "[play_mechanix hook] done"
