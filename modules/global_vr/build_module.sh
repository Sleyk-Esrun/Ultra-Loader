#!/usr/bin/env bash
# Build the Global VR (PC Arcade) per-board module: the spawner/injector DLL
# PLUS its own board-native hook DLL (no more runtime-only fallback to the
# shared Type X hook).
#
# Produces (in this module folder):
#   ultra_global_vr.dll          -- spawner/injector (exports ul_global_vr_run)
#   ultra_global_vr_hook32.dll   -- board-native I/O hook, 32-bit
#   ultra_global_vr_hook64.dll   -- board-native I/O hook, 64-bit
#
# The spawner DLL exports `ul_global_vr_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Global VR
# (PC Arcade) .exe game to the "global_vr" board profile. global_vr_main.c
# (compiled into ultra_global_vr.dll) then searches for
# ultra_global_vr_hook{32,64}.dll (this board's OWN hook, built by
# build_gvr_hook.sh below) and only falls back to the shared
# ultra_typex_hook{32,64}.dll if the board-native hook isn't installed.
#
# The hook DLL is a FULLY INDEPENDENT tree (src/hook/*_gvr.c) with no compiled
# object code shared with native_x86/src/hook/ or any other board's module --
# see src/hook/global_vr_hook.h for the file list and build_gvr_hook.sh for the
# dual-bitness (32/64) build recipe.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/global_vr && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline.
mkdir -p /c/msys64/tmp 2>/dev/null || true

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
        -D_WIN32_WINNT=0x0601
        "-I$HERE/include")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ldxgi)

# Build this board's OWN hook DLL FIRST, so its bytes exist to embed.
# build_gvr_hook.sh compiles both bitnesses from src/hook/*_gvr.c.
echo "[global_vr] building board-native hook (ultra_global_vr_hook{32,64}.dll)"
bash "$HERE/build_gvr_hook.sh"

HOOK32="ultra_global_vr_hook32.dll"
HOOK64="ultra_global_vr_hook64.dll"
if [ ! -f "$HOOK32" ] || [ ! -f "$HOOK64" ]; then
    echo "[global_vr] ERROR: build_gvr_hook.sh did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[global_vr] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_global_vr.dll"
SRCS=("src/global_vr_main.c")

echo "[global_vr] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_global_vr.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_global_vr_run"; then
    echo "[global_vr] export OK: ul_global_vr_run"
else
    echo "[global_vr] ERROR: ul_global_vr_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\global_vr\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir global_vr)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module global_vr "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64"

echo "[global_vr] done"
