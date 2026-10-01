#!/usr/bin/env bash
# Build the Taito Type Zero per-board module DLL + its own independent hook.
#
# Produces (both in this module folder):
#   ultra_taito_typezero.dll        -- the launcher/spawner DLL (src/
#                                      taito_typezero_main.c)
#   ultra_taito_typezero_hook32.dll -- the injected I/O hook, 32-bit
#   ultra_taito_typezero_hook64.dll -- the injected I/O hook, 64-bit
#
# The launcher DLL exports `ul_taito_typezero_run(int argc, char **argv)`.
# The loader's dispatcher (work/src/main.c) calls this after detecting a Type
# Zero .exe game and resolving its board to "taito_typezero" via the unified
# board profile.
#
# FIX (this rebuild pass): this script used to build ONLY the launcher DLL
# and leave the hook DLL to be found at runtime by taito_typezero_main.c's
# find_hook() search, which (until this pass) fell all the way back to
# modtest\ultra_typex_hook32.dll -- the SHARED multi-board hook -- if nothing
# board-specific was staged. That runtime dependency on modtest (and,
# incidentally, on the wrong hook entirely) is now closed: this script also
# builds src/hook/build_typexzero_hook.sh's fully independent
# ultra_taito_typezero_hook{32,64}.dll and stages it right here alongside the
# launcher DLL, so find_hook()'s (now modtest-free) search always finds this
# board's own hook without ever touching modtest or another board's folder.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typezero && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# Build this board's own independent hook DLLs first (src/hook/). Fully
# self-contained: no shared source file, no shared compiled binary, no
# modules/native_x86/src/hook/ or modtest/ dependency at build or run time.
bash "$HERE/src/hook/build_typexzero_hook.sh"

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

if [ ! -f "$HERE/ultra_taito_typezero_hook32.dll" ] || [ ! -f "$HERE/ultra_taito_typezero_hook64.dll" ]; then
    echo "[taito_typezero] ERROR: build_typexzero_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typezero] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_typezero.dll"
SRCS=("src/taito_typezero_main.c")

echo "[taito_typezero] building $OUT (32-bit skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typezero.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_typezero_run"; then
    echo "[taito_typezero] export OK: ul_taito_typezero_run"
else
    echo "[taito_typezero] ERROR: ul_taito_typezero_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\taito_typezero\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir taito_typezero)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module taito_typezero "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_taito_typezero_hook32.dll" "$INSTALL_DIR/ultra_taito_typezero_hook64.dll" \
      "$HERE/ultra_taito_typezero_hook32.dll" "$HERE/ultra_taito_typezero_hook64.dll"

echo "[taito_typezero] done"
