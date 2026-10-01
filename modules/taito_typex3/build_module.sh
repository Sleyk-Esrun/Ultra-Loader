#!/usr/bin/env bash
# Build the Taito Type X3 per-board module DLL + its own independent hook.
#
# Produces (in this module folder):
#   ultra_taito_typex3.dll   -- the spawner/injector module DLL
#   ultra_typex_hook32.dll   -- this board's OWN 32-bit I/O+render+audio hook
#   ultra_typex_hook64.dll   -- this board's OWN 64-bit I/O+render+audio hook
#
# The module DLL exports `ul_taito_typex3_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after detecting a Type X3
# .exe game and resolving its board to "taito_typex3" via the unified board
# profile.
#
# The two hook DLLs are built ENTIRELY from this board's own
# src/hook/ tree (src/hook/build_typex3_hook.sh) -- self-contained, no shared
# source file or compiled binary with modules/native_x86/src/hook/'s
# multi-board hook or with I:\Ultra Loader\modtest\. This replaces the earlier
# skeleton phase, which exposed only the module export with no real hook
# artifact (taito_typex3_main.c's find_hook() would fall through to
# modtest\ at runtime until this board built its own).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typex3 && bash build_module.sh

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

# Build this board's OWN independent hook DLLs (32-bit + 64-bit) FIRST, from
# src/hook/ -- self-contained, no dependency on native_x86/src/hook or
# modtest\ at build or run time. Produces ultra_taito_typex3_hook{32,64}.dll
# directly in $HERE.
#
# NOTE: this step previously looked for ultra_typex_hook{32,64}.dll -- the
# OLD shared-hook filename -- which build_typex3_hook.sh has not produced
# since this board got its own independent hook (it outputs
# ultra_taito_typex3_hook{32,64}.dll, matching HOOK32/HOOK64 in
# taito_typex3_main.c). That stale reference meant this existence check
# could never actually pass; fixed here as part of the embedding rework.
echo "[taito_typex3] building this board's own independent hook (src/hook/build_typex3_hook.sh)"
bash "$HERE/src/hook/build_typex3_hook.sh"

HOOK32="$HERE/ultra_taito_typex3_hook32.dll"
HOOK64="$HERE/ultra_taito_typex3_hook64.dll"
if [ ! -f "$HOOK32" ] || [ ! -f "$HOOK64" ]; then
    echo "[taito_typex3] ERROR: independent hook build did not produce both DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typex3] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_typex3.dll"
SRCS=("src/taito_typex3_main.c")

echo "[taito_typex3] building $OUT (32-bit spawner/injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typex3.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_typex3_run"; then
    echo "[taito_typex3] export OK: ul_taito_typex3_run"
else
    echo "[taito_typex3] ERROR: ul_taito_typex3_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\taito_typex3\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir taito_typex3)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module taito_typex3 "$OUT"

# ----------------------------------------------------------------------------
#  iDmacDrv64.dll -- the driver Theatrhythm's dump is missing.
#
#  game_Data/Plugins/ismACIO.dll binds four functions from iDmacDrv64.dll BY
#  ORDINAL (#1 #2 #5 #6), and that DLL ships with neither the game nor
#  Windows, so the Unity plugin cannot LOAD at all (LoadLibrary -> 126,
#  ERROR_MOD_NOT_FOUND) and the game runs with no I/O whatsoever.
#
#  src/idmacdrv64.def is what makes this work: ismACIO imports NO NAMES, so
#  the ordinal numbers are the entire contract.
#
#  64-bit to match ismACIO.dll, and built with its own toolchain because this
#  module DLL itself is 32-bit. Installed beside the module DLL;
#  taito_typex3_main.c copies it next to game.exe at launch.
# ----------------------------------------------------------------------------
echo "[taito_typex3] building iDmacDrv64.dll (stand-in for the missing driver)"
PATH="/c/msys64/mingw64/bin:$PATH" TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -pipe -static -shared -o iDmacDrv64.dll src/idmacdrv64_stub.c src/idmacdrv64.def || { echo "[taito_typex3] ERROR: iDmacDrv64.dll failed to build" >&2; exit 1; }
ls -la iDmacDrv64.dll
if /c/msys64/mingw64/bin/objdump.exe -p iDmacDrv64.dll | grep -q "iDmacDrvRead"; then
    echo "[taito_typex3] iDmacDrv64.dll exports OK"
else
    echo "[taito_typex3] ERROR: iDmacDrv64.dll has no exports" >&2; exit 1
fi
cp -f iDmacDrv64.dll "$INSTALL_DIR/iDmacDrv64.dll"
echo "[taito_typex3] installed -> $INSTALL_DIR/iDmacDrv64.dll"

# ----------------------------------------------------------------------------
#  NESiCAReader.dll -- stand-in for the card reader.
#
#  The real plugin talks to a reader on COM2. With no reader attached it
#  reports XioStatErrorConnect (-3), and Siva.ErrorChecker turns that into
#  operator error 13003 - the red NESiCA reader error screen. Ours reports an
#  idle, healthy reader with no card, which is what an untouched cabinet
#  reports anyway. 64-bit, to match the game.
# ----------------------------------------------------------------------------
echo "[taito_typex3] building NESiCAReader.dll (card-reader stand-in)"
PATH="/c/msys64/mingw64/bin:$PATH" TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -pipe -static -shared -o NESiCAReader.dll src/nesicareader_stub.c || { echo "[taito_typex3] ERROR: NESiCAReader.dll failed to build" >&2; exit 1; }
ls -la NESiCAReader.dll
if /c/msys64/mingw64/bin/objdump.exe -p NESiCAReader.dll | grep -q "NESiCAReaderGetXioStatus"; then
    echo "[taito_typex3] NESiCAReader.dll exports OK"
else
    echo "[taito_typex3] ERROR: NESiCAReader.dll has no exports" >&2; exit 1
fi
cp -f NESiCAReader.dll "$INSTALL_DIR/NESiCAReader.dll"
echo "[taito_typex3] installed -> $INSTALL_DIR/NESiCAReader.dll"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change
# (both the correct board-named files and any leftover old shared-hook-named
# files from before the filename fix above).
rm -f "$INSTALL_DIR/ultra_taito_typex3_hook32.dll" "$INSTALL_DIR/ultra_taito_typex3_hook64.dll" \
      "$INSTALL_DIR/ultra_typex_hook32.dll" "$INSTALL_DIR/ultra_typex_hook64.dll" \
      "$HOOK32" "$HOOK64"

echo "[taito_typex3] done"
