#!/usr/bin/env bash
# Build the Taito Type X+ per-board module: the spawner/injector DLL AND this
# board's OWN fully independent hook DLL.
#
# Produces (in this module folder):
#   ultra_taito_typexplus.dll          -- spawner+injector (src/taito_typexplus_main.c)
#   ultra_taito_typexplus_hook32.dll   -- board-native 32-bit I/O hook
#   ultra_taito_typexplus_hook64.dll   -- board-native 64-bit I/O hook
#
# The module DLL exports `ul_taito_typexplus_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Taito
# TXP .exe game to the "taito_typexplus" board profile. taito_typexplus_main.c then
# picks the matching hook DLL by the game's PE bitness and injects it.
#
# 2026-08-01 UPDATE: this board now builds its own hook from src/hook/ (see
# src/hook/build_txp_hook.sh) instead of relying on the runtime fallback to
# the shared Type X hook (ultra_typex_hook{32,64}.dll from
# modules/native_x86/src/hook/). Per the project owner's directive, every
# board owns its hook outright: no shared source file, no shared compiled
# binary, no dependency on native_x86/src/hook/'s build output. The
# taito_typexplus_main.c fallback-to-shared-hook path still exists as a defensive
# last resort (a user who deletes the board-native DLL isn't left with a dead
# board), but this script always builds and stages the board-native one.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typexplus && bash build_module.sh

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
# so its bytes exist to embed. See src/hook/build_txp_hook.sh /
# src/hook/hook_txp.h for the full rationale -- no shared source, no shared
# binary with any other board.
echo "[taito_typexplus] building board-native hook (src/hook/build_txp_hook.sh)"
bash "$HERE/src/hook/build_txp_hook.sh"

for f in ultra_taito_typexplus_hook32.dll ultra_taito_typexplus_hook64.dll; do
    if [ ! -f "$HERE/$f" ]; then
        echo "[taito_typexplus] ERROR: $f missing after build_txp_hook.sh" >&2
        exit 1
    fi
done

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typexplus] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_typexplus.dll"
SRCS=("src/taito_typexplus_main.c")

echo "[taito_typexplus] building $OUT (32-bit spawner+injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typexplus.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_typexplus_run"; then
    echo "[taito_typexplus] export OK: ul_taito_typexplus_run"
else
    echo "[taito_typexplus] ERROR: ul_taito_typexplus_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\taito_typexplus\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir taito_typexplus)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module taito_typexplus "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_taito_typexplus_hook32.dll" "$INSTALL_DIR/ultra_taito_typexplus_hook64.dll" \
      "$HERE/ultra_taito_typexplus_hook32.dll" "$HERE/ultra_taito_typexplus_hook64.dll"

echo "[taito_typexplus] done"
