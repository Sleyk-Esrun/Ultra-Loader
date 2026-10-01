#!/usr/bin/env bash
# Build the Sega RingEdge 2 per-board module DLL + its own I/O hook.
#
# Produces:
#   ultra_sega_ringedge2.dll               (in this module folder -- launcher skeleton)
#   ultra_sega_ringedge2_hook32.dll / _hook64.dll  (this board's OWN independent
#                                                   JVS + presence-only keychip +
#                                                   D3D9/D3D11 present + audio hook,
#                                                   built from src/hook/ by
#                                                   src/hook/build_ringedge2_hook.sh)
#
# The launcher DLL exports `ul_sega_ringedge2_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega
# RingEdge 2 .exe game to the "sega_ringedge2" board profile. sega_ringedge2_
# main.c then looks for ultra_sega_ringedge2_hook{32,64}.dll (built here) and
# only falls back to the shared Type X hook / modtest if that board-native
# hook is somehow missing from the install.
#
# The hook DLL is this board's OWN fully independent tree (src/hook/): no file
# from modules/native_x86/src/hook/ or modtest/ is #included, linked, or
# copied into it at build or run time.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_ringedge2 && bash build_module.sh

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

# Build this board's OWN independent I/O hook (JVS + presence-only mxparallel
# keychip + D3D9/D3D11 present + audio) FIRST, so its bytes exist to embed.
# Self-contained under src/hook/ -- no dependency on modules/native_x86/src/hook/
# or modtest/ at build or run time.
HOOK32="ultra_sega_ringedge2_hook32.dll"
HOOK64="ultra_sega_ringedge2_hook64.dll"
echo "[sega_ringedge2] building $HOOK32 / $HOOK64 (board-native I/O hook)"
bash "$HERE/src/hook/build_ringedge2_hook.sh"

if [ ! -f "$HERE/src/hook/$HOOK32" ] || [ ! -f "$HERE/src/hook/$HOOK64" ]; then
    echo "[sega_ringedge2] ERROR: hook build did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi
ls -la "$HERE/src/hook/$HOOK32" "$HERE/src/hook/$HOOK64"

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_ringedge2] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_ringedge2.dll"
SRCS=("src/sega_ringedge2_main.c")

echo "[sega_ringedge2] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_ringedge2.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_ringedge2_run"; then
    echo "[sega_ringedge2] export OK: ul_sega_ringedge2_run"
else
    echo "[sega_ringedge2] ERROR: ul_sega_ringedge2_run not exported" >&2
    exit 1
fi

# Auto-install to the SINGLE supported install location per user spec:
#   %LOCALAPPDATA%\Ultra Loader\modules\sega_ringedge2\
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_ringedge2)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_ringedge2 "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64" "$HERE/$HOOK32" "$HERE/$HOOK64"

echo "[sega_ringedge2] done"
