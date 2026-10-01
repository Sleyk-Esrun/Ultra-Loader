#!/usr/bin/env bash
# Build the Sega ALLS per-board module: the spawner/injector DLL AND this
# board's OWN, fully self-contained hook DLLs.
#
# Produces (both staged into this module folder + %LOCALAPPDATA% install dir):
#   1. ultra_sega_alls.dll          -- the spawner/injector (this script)
#   2. ultra_sega_alls_hook32.dll   -- board-native hook, 32-bit  (build_alls_hook.sh)
#   3. ultra_sega_alls_hook64.dll   -- board-native hook, 64-bit  (build_alls_hook.sh)
#
# The spawner DLL exports `ul_sega_alls_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after detecting a Sega ALLS
# title and resolving its board to "sega_alls" via the unified board profile.
# ul_sega_alls_run() (src/sega_alls_main.c) then searches for
# ultra_sega_alls_hook{32,64}.dll (this board's own hook, built from
# src/hook/) and injects it into the game; it falls back to the shared
# ultra_typex_hook{32,64}.dll ONLY if the board-native hook is missing from
# every search path. Building + staging the board-native hook here (instead of
# leaving it absent) is what makes sega_alls run on ITS OWN hook rather than
# silently falling back to modtest's shared Type X hook / doing nothing.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_alls && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

mkdir -p /c/msys64/tmp 2>/dev/null || true

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
        -D_WIN32_WINNT=0x0601
        "-I$HERE/include")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ldxgi)

# ---------------------------------------------------------------------------
# Build THIS board's own, fully self-contained hook DLLs (both bitnesses)
# FIRST, so their bytes exist to embed. build_alls_hook.sh compiles ONLY
# sources under src/hook/ -- no dependency on native_x86's compiled hook
# DLL/import lib.
# ---------------------------------------------------------------------------
echo "[sega_alls] building board-native hook (ultra_sega_alls_hook32/64.dll)"
bash "$HERE/build_alls_hook.sh"

if [ ! -f "$HERE/ultra_sega_alls_hook32.dll" ] || [ ! -f "$HERE/ultra_sega_alls_hook64.dll" ]; then
    echo "[sega_alls] ERROR: hook build did not produce ultra_sega_alls_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_alls] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_alls.dll"
SRCS=("src/sega_alls_main.c")

echo "[sega_alls] building $OUT (skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_alls.lib

ls -la "$OUT"

if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_alls_run"; then
    echo "[sega_alls] export OK: ul_sega_alls_run"
else
    echo "[sega_alls] ERROR: ul_sega_alls_run not exported" >&2
    exit 1
fi

# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_alls)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_alls "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_sega_alls_hook32.dll" "$INSTALL_DIR/ultra_sega_alls_hook64.dll"

echo "[sega_alls] done"
