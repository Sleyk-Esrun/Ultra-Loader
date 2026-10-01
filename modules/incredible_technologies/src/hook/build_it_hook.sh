#!/usr/bin/env bash
# Build Ultra Loader's Incredible Technologies-ONLY hook DLL.
#   ultra_incredible_technologies_hook32.dll -- the injected I/O hook for
#   IT "nowboard" Golden Tee-family games (Golden Tee Golf 2005/Live/
#   Unplugged/PGA, Silver Strike Bowling, Target Toss Pro, Big Buck Safari
#   IT variant, Powerputt, Shuffleboard).
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook32.dll: no shared source file, no shared
# compiled binary -- every TU in this folder (jvs_core_it.*, it_hook.h,
# hook_dllmain_it.c, jvs_serial_hook_it.c, d3d9_throttle_it.c, audio_hook_it.c)
# is this board's own independent copy. 32-bit only -- IT's Golden Tee-family
# PC catalog (2005-era through the "Unplugged"/PGA revisions) is Windows XP/
# Vista-era 32-bit; a 64-bit revision is out of scope until a real dump
# surfaces (same "32-bit until proven otherwise" scoping used for cave_pc).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/incredible_technologies/src/hook && bash build_it_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build env").
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_it.c jvs_serial_hook_it.c d3d9_throttle_it.c audio_hook_it.c omni_bridge.c pause_menu_wire_it.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_it.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# omni_bridge.c's #include "omni_client.h" resolves against the loader's
# shared work/include/ (the same header-only, no-alloc, env-var-backed config
# struct every other Omni service client uses) -- NOT a copy, NOT a shared
# compiled binary, just the one shared config header. Path modeled on
# modules/native_x86/src/hook/build_hook.sh's LOADER_INC (same tree depth:
# modules/incredible_technologies/src/hook -> ../../../../../../work/include).
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as every other board's own hook).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -m32 -D_WIN32_WINNT=0x0601"
OUT="ultra_incredible_technologies_hook32.dll"

echo "[incredible_technologies hook] building $OUT"
echo "  sources: $SRCS"
# omni_bridge.c's gamepad thread resolves xinput1_4.dll/xinput1_3.dll/
# xinput9_1_0.dll dynamically at runtime via LoadLibraryA/GetProcAddress --
# no -lxinput* import-library dependency, so the import table is unchanged
# beyond what user32 (SendInput/SetCursorPos/MapVirtualKeyA) already covers.
env $T "$CC" $CFLAGS -I. -I"$LOADER_INC" -shared -o "$OUT" $SRCS \
    -lpsapi -luser32 -lgdi32 -lwinmm -lkernel32 \
    -Wl,--out-implib,ultra_incredible_technologies_hook32.lib

ls -la "$OUT"

# Verify the identity export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ultra_incredible_technologies_hook_id"; then
    echo "[incredible_technologies hook] export OK: ultra_incredible_technologies_hook_id"
else
    echo "[incredible_technologies hook] ERROR: ultra_incredible_technologies_hook_id not exported" >&2
    exit 1
fi

# Stage next to the module DLL in this project's completed-cores folder
# (mirrors incredible_technologies/build_module.sh's own artifact drop).
PROJ="$HERE/../../../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Incredible Technologies"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[incredible_technologies hook] artifact -> $COMPLETED_DIR/$OUT"

# Also install to the SINGLE supported install location per user spec, next to
# ultra_incredible_technologies.dll (the module DLL built by build_module.sh),
# so incredible_technologies_main.c's find_hook_named() picks it up as the
# preferred board-native hook instead of falling back to ultra_typex_hook32.dll.
INSTALL_DIR="$USERPROFILE/AppData/Local/Ultra Loader/modules/incredible_technologies"
INSTALL_DIR="$(cygpath -u "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")"
mkdir -p "$INSTALL_DIR"
cp -f "$OUT" "$INSTALL_DIR/$OUT"
echo "[incredible_technologies hook] installed -> $INSTALL_DIR/$OUT"

echo "[incredible_technologies hook] done"
