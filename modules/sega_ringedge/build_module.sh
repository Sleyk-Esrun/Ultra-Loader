#!/usr/bin/env bash
# Build the Sega RingEdge per-board module DLL + its OWN independent hook DLL.
#
# Produces (in this module folder):
#   ultra_sega_ringedge.dll          -- the spawner/injector module
#   ultra_sega_ringedge_hook32.dll   -- this board's OWN JVS/D3D9/audio/keychip
#   ultra_sega_ringedge_hook64.dll      hook (built by build_ringedge_hook.sh
#                                       from src/hook/*_ringedge.c -- NOT the
#                                       shared native_x86 hook, no dependency
#                                       on native_x86 or modtest/ at build OR
#                                       run time)
#
# The module DLL exports `ul_sega_ringedge_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega
# RingEdge .exe game to the "sega_ringedge" board profile. sega_ringedge_main.c
# then searches for ultra_sega_ringedge_hook{32,64}.dll FIRST (see
# HOOK_DLL_PREF_32/64 in src/sega_ringedge_main.c) and only falls back to the
# shared Type X hook if this board-native one isn't installed -- so building +
# staging it here (this script) is what makes that primary path real instead
# of a fallback-only skeleton.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_ringedge && bash build_module.sh

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

# -----------------------------------------------------------------------------
# Build this board's OWN independent hook DLL (both bitnesses) FIRST, so its
# bytes exist to embed. This is a SEPARATE compile of THIS module's
# src/hook/*_ringedge.c files only -- no #include, link, or runtime copy from
# native_x86/src/hook/ or modtest/.
# -----------------------------------------------------------------------------
echo "[sega_ringedge] building board-native hook (ultra_sega_ringedge_hook{32,64}.dll)"
bash "$HERE/build_ringedge_hook.sh"

HOOK32="ultra_sega_ringedge_hook32.dll"
HOOK64="ultra_sega_ringedge_hook64.dll"
if [ ! -f "$HOOK32" ] || [ ! -f "$HOOK64" ]; then
    echo "[sega_ringedge] ERROR: board-native hook build did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_ringedge] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_ringedge.dll"
SRCS=("src/sega_ringedge_main.c")

echo "[sega_ringedge] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_ringedge.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_ringedge_run"; then
    echo "[sega_ringedge] export OK: ul_sega_ringedge_run"
else
    echo "[sega_ringedge] ERROR: ul_sega_ringedge_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\sega_ringedge\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_ringedge)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_ringedge "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64"

echo "[sega_ringedge] done"
