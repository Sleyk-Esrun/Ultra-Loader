#!/usr/bin/env bash
# Build the Andamiro PC Plus per-board module DLL.
#
# Produces: ultra_unis.dll (in this module folder)
#
# The DLL exports `ul_unis_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after resolving an Andamiro PC Plus .exe game
# to the "unis" board profile.
#
# This is a 32-bit skeleton module: only src/unis_main.c is compiled. Real
# launcher logic (keychip / TrueCrypt / present hook) lands beside this file
# later -- the export already exists so the loader's GetProcAddress succeeds.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/unis && bash build_module.sh

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

# Build this board's OWN independent hook DLLs FIRST (both bitnesses -- see
# src/hook/build_unis_hook.sh), so their bytes exist to embed. This step was
# previously missing from this script entirely (the hook only ever got built
# by running that script directly) -- added here so a plain `bash
# build_module.sh` produces a fully working module, hook included.
echo "[unis] building board-native hook (src/hook/build_unis_hook.sh)"
( cd "$HERE/src/hook" && bash build_unis_hook.sh )

if [ ! -f "$HERE/src/hook/ultra_unis_hook32.dll" ] || [ ! -f "$HERE/src/hook/ultra_unis_hook64.dll" ]; then
    echo "[unis] ERROR: hook build did not produce ultra_unis_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[unis] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_unis.dll"
SRCS=("src/unis_main.c")

echo "[unis] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_unis.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_unis_run"; then
    echo "[unis] export OK: ul_unis_run"
else
    echo "[unis] ERROR: ul_unis_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\unis\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir unis)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module unis "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. build_unis_hook.sh still copies its own copies to the board
# root -- remove them (and any old install-dir copies) so only the one
# embedded-hook DLL ships.
rm -f "$INSTALL_DIR/ultra_unis_hook32.dll" "$INSTALL_DIR/ultra_unis_hook64.dll" \
      "$HERE/ultra_unis_hook32.dll" "$HERE/ultra_unis_hook64.dll"

echo "[unis] done"
