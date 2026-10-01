#!/usr/bin/env bash
# Build Ultra Loader's UNIS-Arcade-ONLY hook DLL (both bitnesses).
#   ultra_unis_hook32.dll / ultra_unis_hook64.dll
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook{32,64}.dll: no shared source file, no shared
# compiled binary, no board_io_ops per-game indirection, no other board's
# protection scaffolding. UNIS Arcade is a generic-JVS board with no dump on
# hand (see ../../UNTESTED.txt), so this hook implements the plain public
# JVS / D3D9 / DirectInput8 / DirectSound+XAudio2 contracts only.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/unis/src/hook && bash build_unis_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_unis.c jvs_serial_hook_unis.c d3d9_throttle_unis.c dinput8_emu_unis.c audio_hook_unis.c omni_bridge.c pause_menu_wire_unis.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_unis.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# work/include -- shared, header-only Omni-client config (omni_client.h); read
# by omni_bridge.c only (this board's own copy of the Ultra Omni-Bridge
# reference at native_x86/src/hook/omni_bridge.c). Same relative-path recipe
# as native_x86/src/hook/build_hook.sh's LOADER_INC.
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as every other board's own hook).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -I$LOADER_INC"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

echo "[unis hook] building ultra_unis_hook32.dll"
echo "  sources: $SRCS"
PATH="$M32:$PATH" env $T i686-w64-mingw32-gcc $CFLAGS -m32 -shared \
    -o ultra_unis_hook32.dll $SRCS $LIBS

echo "[unis hook] building ultra_unis_hook64.dll"
PATH="$M64:$PATH" env $T x86_64-w64-mingw32-gcc $CFLAGS -shared \
    -o ultra_unis_hook64.dll $SRCS $LIBS

ls -la ultra_unis_hook32.dll ultra_unis_hook64.dll

# Verify the identity export on both (each bitness's own objdump -- the 32-bit
# objdump cannot parse a PE32+ file and vice versa).
check_export() {
    local file="$1" objdump="$2"
    if "$objdump" -p "$file" | grep -q "ultra_unis_hook_id"; then
        echo "[unis hook] export OK: ultra_unis_hook_id ($file)"
    else
        echo "[unis hook] ERROR: ultra_unis_hook_id not exported ($file)" >&2
        exit 1
    fi
}
check_export ultra_unis_hook32.dll "$M32/objdump.exe"
check_export ultra_unis_hook64.dll "$M64/objdump.exe"

# Stage next to the module DLL (mirrors every other board's own build_*_hook.sh).
MODULE_DIR="$HERE/../.."
cp -f ultra_unis_hook32.dll "$MODULE_DIR/ultra_unis_hook32.dll"
cp -f ultra_unis_hook64.dll "$MODULE_DIR/ultra_unis_hook64.dll"
echo "[unis hook] staged -> $MODULE_DIR/ultra_unis_hook{32,64}.dll"

echo "[unis hook] done"
