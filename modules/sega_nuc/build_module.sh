#!/usr/bin/env bash
# Build the Sega NUC per-board module DLL + its own injected hook DLLs.
#
# Produces (in this module folder):
#   ultra_sega_nuc.dll                                -- the module (loader-facing)
#   ultra_sega_nuc_hook32.dll / ultra_sega_nuc_hook64.dll -- the injected hook
#     (built by build_nuc_hook.sh from src/hook/*_nuc.c -- this board's OWN,
#     fully self-contained JVS engine + IO4-USB I/O + presence-only keychip
#     shim + D3D11 present hook + audio hook + DllMain; no shared runtime
#     binary with any other board, no dependency on native_x86/src/hook's
#     compiled output)
#
# The module DLL exports `ul_sega_nuc_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega NUC
# .exe game to the "sega_nuc" board profile. sega_nuc_main.c then spawns the
# game suspended and injects whichever hook DLL matches the game's bitness
# (see find_hook() in src/sega_nuc_main.c) -- this build now produces and
# stages that board-native hook instead of falling back to the shared Type X
# hook / modtest.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_nuc && bash build_module.sh

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

# Build this board's OWN injected hook DLLs (both bitnesses) FIRST, so their
# bytes exist to embed. build_nuc_hook.sh is fully self-contained
# (src/hook/*_nuc.c only) -- no dependency on native_x86/src/hook's compiled
# output.
echo "[sega_nuc] building board-native hook DLLs"
bash "$HERE/build_nuc_hook.sh"

HOOK32="ultra_sega_nuc_hook32.dll"
HOOK64="ultra_sega_nuc_hook64.dll"
if [ ! -f "$HERE/$HOOK32" ] || [ ! -f "$HERE/$HOOK64" ]; then
    echo "[sega_nuc] ERROR: hook build did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_nuc] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_nuc.dll"
SRCS=("src/sega_nuc_main.c")

echo "[sega_nuc] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_nuc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_nuc_run"; then
    echo "[sega_nuc] export OK: ul_sega_nuc_run"
else
    echo "[sega_nuc] ERROR: ul_sega_nuc_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\sega_nuc\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_nuc)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_nuc "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64"

echo "[sega_nuc] done"
