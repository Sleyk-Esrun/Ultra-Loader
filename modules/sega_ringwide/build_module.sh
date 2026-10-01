#!/usr/bin/env bash
# Build the Sega RingWide per-board module DLL + its own injected hook DLLs.
#
# Produces (in this module folder):
#   ultra_sega_ringwide.dll        -- the module dispatcher (spawner/injector)
#   ultra_sega_ringwide_hook32.dll -- this board's OWN 32-bit injected hook
#   ultra_sega_ringwide_hook64.dll -- this board's OWN 64-bit injected hook
#
# The dispatcher DLL exports `ul_sega_ringwide_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega
# RingWide .exe game to the "sega_ringwide" board profile.
#
# The hook DLLs are built from src/hook/ (see build_ringwide_hook.sh in that
# folder) -- a fully self-contained JVS engine + JVS-serial transport +
# presence-only mxparallel keychip shim + D3D9 present hook + audio hook, with
# NO dependency on modules/native_x86 or modtest at build or run time. Once
# these are staged, src/sega_ringwide_main.c's board-native search
# (ultra_sega_ringwide_hook{32,64}.dll) finds them and the old Type X shared-
# hook fallback never triggers.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_ringwide && bash build_module.sh

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

# Build this board's OWN, self-contained injected hook DLLs (both bitnesses)
# FIRST, so their bytes exist to embed. See src/hook/build_ringwide_hook.sh
# for what's inside; it has zero build- or run-time dependency on
# modules/native_x86 or modtest.
echo "[sega_ringwide] building board-native hook DLLs (src/hook/)"
bash "$HERE/src/hook/build_ringwide_hook.sh"

if [ ! -f "$HERE/src/hook/ultra_sega_ringwide_hook32.dll" ] || [ ! -f "$HERE/src/hook/ultra_sega_ringwide_hook64.dll" ]; then
    echo "[sega_ringwide] ERROR: hook build did not produce ultra_sega_ringwide_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_ringwide] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_ringwide.dll"
SRCS=("src/sega_ringwide_main.c")

echo "[sega_ringwide] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_ringwide.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_ringwide_run"; then
    echo "[sega_ringwide] export OK: ul_sega_ringwide_run"
else
    echo "[sega_ringwide] ERROR: ul_sega_ringwide_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\sega_ringwide\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_ringwide)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_ringwide "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_sega_ringwide_hook32.dll" "$INSTALL_DIR/ultra_sega_ringwide_hook64.dll" \
      "$HERE/ultra_sega_ringwide_hook32.dll" "$HERE/ultra_sega_ringwide_hook64.dll"

echo "[sega_ringwide] done"
