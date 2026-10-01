#!/usr/bin/env bash
# Build the Adrenaline Amusements per-board module: the spawner/injector DLL AND this
# board's OWN fully independent hook DLL.
#
# Produces (in this module folder):
#   ultra_adrenaline_aa.dll          -- spawner+injector (src/adrenaline_aa_main.c)
#   ultra_adrenaline_aa_hook32.dll   -- board-native 32-bit I/O hook
#   ultra_adrenaline_aa_hook64.dll   -- board-native 64-bit I/O hook
#
# The module DLL exports `ul_adrenaline_aa_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Adrenaline
# ADR .exe game to the "adrenaline_aa" board profile. adrenaline_aa_main.c then
# picks the matching hook DLL by the game's PE bitness and injects it.
#
# 2026-08-01 UPDATE: this board now builds its own hook from src/hook/ (see
# src/hook/build_adr_hook.sh) instead of relying on the runtime fallback to
# the shared Type X hook (ultra_typex_hook{32,64}.dll from
# modules/native_x86/src/hook/). Per the project owner's directive, every
# board owns its hook outright: no shared source file, no shared compiled
# binary, no dependency on native_x86/src/hook/'s build output. The
# adrenaline_aa_main.c fallback-to-shared-hook path still exists as a defensive
# last resort (a user who deletes the board-native DLL isn't left with a dead
# board), but this script always builds and stages the board-native one.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/adrenaline_aa && bash build_module.sh

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

# Build this board's OWN fully independent hook DLL (both bitnesses) FIRST,
# so its bytes exist to embed. See src/hook/build_adr_hook.sh /
# src/hook/hook_adr.h for the full rationale -- no shared source, no shared
# binary with any other board.
echo "[adrenaline_aa] building board-native hook (src/hook/build_adr_hook.sh)"
bash "$HERE/src/hook/build_adr_hook.sh"

for f in ultra_adrenaline_aa_hook32.dll ultra_adrenaline_aa_hook64.dll; do
    if [ ! -f "$HERE/$f" ]; then
        echo "[adrenaline_aa] ERROR: $f missing after build_adr_hook.sh" >&2
        exit 1
    fi
done

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[adrenaline_aa] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_adrenaline_aa.dll"
SRCS=("src/adrenaline_aa_main.c")

echo "[adrenaline_aa] building $OUT (32-bit spawner+injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_adrenaline_aa.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_adrenaline_aa_run"; then
    echo "[adrenaline_aa] export OK: ul_adrenaline_aa_run"
else
    echo "[adrenaline_aa] ERROR: ul_adrenaline_aa_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\adrenaline_aa\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir adrenaline_aa)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module adrenaline_aa "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_adrenaline_aa_hook32.dll" "$INSTALL_DIR/ultra_adrenaline_aa_hook64.dll" \
      "$HERE/ultra_adrenaline_aa_hook32.dll" "$HERE/ultra_adrenaline_aa_hook64.dll"

echo "[adrenaline_aa] done"
