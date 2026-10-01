#!/usr/bin/env bash
# Build the Play Mechanix / Raw Thrills (Chicago PC) per-board module DLL.
#
# Produces: ultra_play_mechanix.dll (in this module folder)
#
# The DLL exports `ul_play_mechanix_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after resolving a Play Mechanix / Raw Thrills (Chicago PC) .exe game
# to the "play_mechanix" board profile.
#
# This is a 32-bit skeleton module: only src/play_mechanix_main.c is compiled. Real
# launcher logic (keychip / TrueCrypt / present hook) lands beside this file
# later -- the export already exists so the loader's GetProcAddress succeeds.
#
# 2026-08-01 UPDATE: this board now ALSO builds its own fully independent
# hook DLL from src/hook/ (build_pm_hook.sh) -- ultra_play_mechanix_hook{32,64}.dll,
# a clean-room JVS-over-serial engine + lightgun screen-position channel +
# D3D9 present hook + audio diagnostic hook, sharing NO source file and NO
# compiled binary with modules/native_x86/src/hook/ or any other board's hook
# tree (per the project owner's per-board-independence directive). Both DLLs
# are staged into the SAME install dir as the launcher skeleton below, so
# play_mechanix_main.c's find_hook_named() finds the board-native hook FIRST
# and the "fall back to the shared ultra_typex_hook{32,64}.dll" branch in
# play_mechanix_main.c is no longer exercised on an install that ran this
# script (the fallback code itself is untouched -- it stays as a defensive
# last resort if a user's install is ever missing the board-native DLL).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/play_mechanix && bash build_module.sh

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

# Build this board's OWN independent hook DLL (both bitnesses) FIRST, so its
# bytes exist to embed. src/hook/build_pm_hook.sh is self-contained (own JVS
# engine, own D3D9/audio hooks, own IAT-patch plumbing -- see
# src/hook/pm_hook.h).
echo "[play_mechanix] building board-native hook (src/hook/build_pm_hook.sh)"
( cd "$HERE/src/hook" && bash build_pm_hook.sh )

HOOK32="$HERE/src/hook/ultra_play_mechanix_hook32.dll"
HOOK64="$HERE/src/hook/ultra_play_mechanix_hook64.dll"
if [ ! -f "$HOOK32" ] || [ ! -f "$HOOK64" ]; then
    echo "[play_mechanix] ERROR: board-native hook build did not produce both DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[play_mechanix] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_play_mechanix.dll"
SRCS=("src/play_mechanix_main.c")

echo "[play_mechanix] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_play_mechanix.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_play_mechanix_run"; then
    echo "[play_mechanix] export OK: ul_play_mechanix_run"
else
    echo "[play_mechanix] ERROR: ul_play_mechanix_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\play_mechanix\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir play_mechanix)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module play_mechanix "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_play_mechanix_hook32.dll" "$INSTALL_DIR/ultra_play_mechanix_hook64.dll"

echo "[play_mechanix] done"
