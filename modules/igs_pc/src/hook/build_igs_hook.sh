#!/usr/bin/env bash
# Build Ultra Loader's IGS-PC-ONLY hook DLLs.
#   ultra_igs_pc_hook32.dll -- the injected I/O hook for 32-bit IGS PC games
#   ultra_igs_pc_hook64.dll -- the injected I/O hook for 64-bit IGS PC games
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook{32,64}.dll: no shared source file, no shared
# compiled binary (see igs_hook.h for why). Same isolation pattern Cave PC's
# hook fork established (modules/cave_pc/src/hook/build_cave_hook.sh).
#
# IGS PC (generic JVS I/O family, distinct from IGS's PGM3/KOV3 HD board) has
# no known game dump yet (framework-only per docs/STOCK_TAKE.md), so both
# bitnesses are built -- igs_pc_main.c already sniffs the game's PE header and
# picks whichever this produces.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/igs_pc/src/hook && bash build_igs_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="igs_plumbing.c jvs_core_igs.c jvs_serial_hook_igs.c dinput8_emu_igs.c d3d9_throttle_igs.c audio_hook_igs.c igs_dialog.c omni_bridge.c pause_menu_wire_igs.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_igs.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as every other hook in this
# project -- the game process has no mingw runtime of its own).
# -I../../../../../../work/include: resolves omni_bridge.c's #include
# "omni_client.h" (the shared, header-only Omni-* config struct every board's
# hook reads its own copy of omni_bridge.c against -- see igs_hook.h/omni_bridge.h).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -I../../../../../../work/include"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

echo "[igs_pc hook] building ultra_igs_pc_hook32.dll (32-bit)"
echo "  sources: $SRCS"
PATH="$M32:$PATH" env $T gcc $CFLAGS -m32 -shared -o ultra_igs_pc_hook32.dll $SRCS $LIBS
ls -la ultra_igs_pc_hook32.dll

echo "[igs_pc hook] building ultra_igs_pc_hook64.dll (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -m64 -shared -o ultra_igs_pc_hook64.dll $SRCS $LIBS
ls -la ultra_igs_pc_hook64.dll

# Verify the identity export on both.
if /c/msys64/mingw32/bin/objdump.exe -p ultra_igs_pc_hook32.dll | grep -q "ultra_igs_pc_hook_id"; then
    echo "[igs_pc hook] export OK (32): ultra_igs_pc_hook_id"
else
    echo "[igs_pc hook] ERROR: ultra_igs_pc_hook_id not exported (32)" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p ultra_igs_pc_hook64.dll | grep -q "ultra_igs_pc_hook_id"; then
    echo "[igs_pc hook] export OK (64): ultra_igs_pc_hook_id"
else
    echo "[igs_pc hook] ERROR: ultra_igs_pc_hook_id not exported (64)" >&2
    exit 1
fi

# Stage next to the module DLL in this project's completed-cores folder
# (mirrors igs_pc/build_module.sh's own artifact drop).
PROJ="$HERE/../../../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra IGS PC"
mkdir -p "$COMPLETED_DIR"
cp -f ultra_igs_pc_hook32.dll "$COMPLETED_DIR/ultra_igs_pc_hook32.dll"
cp -f ultra_igs_pc_hook64.dll "$COMPLETED_DIR/ultra_igs_pc_hook64.dll"
echo "[igs_pc hook] artifacts -> $COMPLETED_DIR/ultra_igs_pc_hook{32,64}.dll"

echo "[igs_pc hook] done"
