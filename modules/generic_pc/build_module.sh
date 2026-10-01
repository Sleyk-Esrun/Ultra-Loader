#!/usr/bin/env bash
# Build the Generic PC fallback per-board module DLL.
#
# Produces: ultra_generic_pc.dll (in this module folder)
#
# The DLL exports `ul_generic_pc_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this when a PC-arcade title resolves to
# the "generic_pc" fallback profile (no specific vendor board matched).
#
# This launcher spawns the game suspended and injects a hook DLL. It now
# builds+stages this board's OWN independent hook set (see build_generic_hook.sh
# and src/hook/) instead of relying only on the runtime shared-Type-X-hook
# fallback: ultra_generic_pc_hook{32,64}.dll -- self-contained JVS-serial I/O +
# auto-detected D3D8/D3D9/D3D11/GL present hooks, no dependency on
# native_x86/src/hook/'s compiled output. The shared Type X hook remains only
# as generic_pc_main.c's last-resort fallback if this board-native hook is
# ever missing from the install (see HOOK_DLL_FALLBACK_32/64 there).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/generic_pc && bash build_module.sh

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

# Build this board's OWN independent hook DLLs FIRST (see build_generic_hook.sh
# and src/hook/), so their bytes exist to embed into the main module DLL below.
echo "[generic_pc] building board-native hook (ultra_generic_pc_hook32/64.dll)"
bash "$HERE/build_generic_hook.sh"

HOOK32="ultra_generic_pc_hook32.dll"
HOOK64="ultra_generic_pc_hook64.dll"
if [ ! -f "$HERE/$HOOK32" ] || [ ! -f "$HERE/$HOOK64" ]; then
    echo "[generic_pc] ERROR: hook build did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[generic_pc] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_generic_pc.dll"
SRCS=("src/generic_pc_main.c")

echo "[generic_pc] building $OUT (32-bit skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_generic_pc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_generic_pc_run"; then
    echo "[generic_pc] export OK: ul_generic_pc_run"
else
    echo "[generic_pc] ERROR: ul_generic_pc_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\generic_pc\ultra_generic_pc.dll
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir generic_pc)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module generic_pc "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. build_generic_hook.sh still copies them to INSTALL_DIR itself
# (untouched, to avoid disturbing its own internal logic) -- remove those
# copies here so only the one embedded-hook DLL actually ships.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64"

echo "[generic_pc] done"
