#!/usr/bin/env bash
# Build Ultra Loader's Examu eX-BOARD-ONLY hook DLL.
#   ultra_examu_exboard_hook32.dll -- the injected I/O hook for eX-BOARD games
#   (Arcana Heart / Arcana Heart 2 / Arcana Heart 3 / LOVEMAX)
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook32.dll: no shared source file, no shared
# compiled binary. Every symbol in this board's own src/hook/*_examu.c files
# is board-pinned (own JVS engine, own IAT patcher, own D3D9/audio/DirectInput
# hooks) -- a rebuild of the shared native_x86 hook, or of any OTHER board's
# own hook, can never again change what an eX-BOARD title renders, sounds
# like, or is controlled with. This satisfies the project owner's directive:
# every board gets its own fully independent, self-contained hook DLL.
#
# 32-bit only -- eX-BOARD's whole known catalog (Arcana Heart family) ships
# 32-bit, matching examu_exboard_main.c's unconditional 32-bit hook choice.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/examu_exboard/src/hook && bash build_examu_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_examu.c jvs_serial_hook_examu.c d3d9_throttle_examu.c dinput8_emu_examu.c audio_hook_examu.c dongle_bypass_examu.c omni_bridge.c pause_menu_wire_examu.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_examu.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# omni_bridge.c reads its tuning from the loader's shared, header-only
# work/include/omni_client.h (same include every other board's Omni-Bridge
# copy uses) -- same relative depth as native_x86/src/hook's build_hook.sh
# LOADER_INC (modules/<board>/src/hook is the same depth for every board).
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as the shared hook's build).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -m32"
OUT="ultra_examu_exboard_hook32.dll"

echo "[examu_exboard hook] building $OUT"
echo "  sources: $SRCS"
env $T "$CC" $CFLAGS -I. -I"$LOADER_INC" -shared -o "$OUT" $SRCS \
    -lpsapi -luser32 -lwinmm -lgdi32

ls -la "$OUT"

# Verify the identity export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ultra_examu_exboard_hook_id"; then
    echo "[examu_exboard hook] export OK: ultra_examu_exboard_hook_id"
else
    echo "[examu_exboard hook] ERROR: ultra_examu_exboard_hook_id not exported" >&2
    exit 1
fi

# Stage next to the module DLL in this project's completed-cores folder
# (mirrors examu_exboard/build_module.sh's own artifact drop).
PROJ="$HERE/../../../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra EX-Board"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[examu_exboard hook] artifact -> $COMPLETED_DIR/$OUT"

echo "[examu_exboard hook] done"
