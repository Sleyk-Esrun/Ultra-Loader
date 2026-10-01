#!/usr/bin/env bash
# Build the Namco System BNA1 per-board module DLL + its board-native hook.
#
# Produces (in this module folder):
#   ultra_namco_bna1.dll          -- the spawner/injector (ul_namco_bna1_run)
#   ultra_namco_bna1_hook32.dll   -- board-native 32-bit I/O hook
#   ultra_namco_bna1_hook64.dll   -- board-native 64-bit I/O hook
#
# The spawner DLL exports `ul_namco_bna1_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after detecting a BNA1 .exe
# game and resolving its board to "namco_bna1" via the unified board profile.
# namco_bna1_main.c's find_hook() then prefers this board's OWN hook
# (ultra_namco_bna1_hook{32,64}.dll, built by build_bna1_hook.sh below) over
# the shared Type X fallback -- so a normal `bash build_module.sh` run now
# stages the real board-native hook instead of falling back to modtest.
#
# The hook DLL is fully self-contained: every source it links lives under
# src/hook/ in THIS module (its own independent copy of the JVS engine, IAT
# patcher, JVS-over-USB + amauthd transport, D3D11 present throttle,
# DirectInput8 emulation, and DirectSound/XAudio2 diagnostic hook) -- zero link
# dependency on native_x86's compiled output or any other board module.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/namco_bna1 && bash build_module.sh

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

# Build this board's OWN hook DLL (both bitnesses) FIRST, so its bytes exist
# to embed. build_bna1_hook.sh is self-contained (modules/namco_bna1/src/hook/
# *_bna1.c only).
echo "[namco_bna1] building board-native hook (ultra_namco_bna1_hook{32,64}.dll)"
bash "$HERE/build_bna1_hook.sh"

if [ ! -f "$HERE/ultra_namco_bna1_hook32.dll" ] || [ ! -f "$HERE/ultra_namco_bna1_hook64.dll" ]; then
    echo "[namco_bna1] ERROR: hook build did not produce ultra_namco_bna1_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[namco_bna1] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_namco_bna1.dll"
SRCS=("src/namco_bna1_main.c")

echo "[namco_bna1] building $OUT (32-bit skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_namco_bna1.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_namco_bna1_run"; then
    echo "[namco_bna1] export OK: ul_namco_bna1_run"
else
    echo "[namco_bna1] ERROR: ul_namco_bna1_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\namco_bna1\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir namco_bna1)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module namco_bna1 "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_namco_bna1_hook32.dll" "$INSTALL_DIR/ultra_namco_bna1_hook64.dll"

echo "[namco_bna1] done"
