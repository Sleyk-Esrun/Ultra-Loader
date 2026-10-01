#!/usr/bin/env bash
# Build the Namco ES2 Plus per-board module DLL + its OWN board-native hook.
#
# Produces:
#   ultra_namco_es2plus.dll          -- the spawner/injector (in this module folder)
#   ultra_namco_es2plus_hook32.dll   -- this board's OWN, fully independent
#   ultra_namco_es2plus_hook64.dll      injected I/O hook (built by
#                                        build_es2plus_hook.sh from src/hook/,
#                                        which links NOTHING from native_x86 or
#                                        any other board's module)
#
# The hook DLL is staged into the module's install dir alongside the spawner
# so namco_es2plus_main.c's find_hook_named() picks the board-native hook
# first (it always searches there before it ever falls back to a shared
# modtest hook).

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

mkdir -p /c/msys64/tmp 2>/dev/null || true

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
        -D_WIN32_WINNT=0x0601
        "-I$HERE/include")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ldxgi)

# --- Build this board's OWN independent hook FIRST (build_es2plus_hook.sh) --
# Fully self-contained: every source lives under src/hook/ in THIS module
# folder; it does not compile, include, or link anything from
# modules/native_x86/src/hook/ or any other board's module.
echo "[namco_es2plus] building board-native hook (build_es2plus_hook.sh)"
bash "$HERE/build_es2plus_hook.sh"

if [ ! -f "$HERE/ultra_namco_es2plus_hook32.dll" ] || [ ! -f "$HERE/ultra_namco_es2plus_hook64.dll" ]; then
    echo "[namco_es2plus] ERROR: hook build did not produce ultra_namco_es2plus_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[namco_es2plus] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_namco_es2plus.dll"
SRCS=("src/namco_es2plus_main.c")

echo "[namco_es2plus] building $OUT (skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_namco_es2plus.lib

ls -la "$OUT"

if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_namco_es2plus_run"; then
    echo "[namco_es2plus] export OK: ul_namco_es2plus_run"
else
    echo "[namco_es2plus] ERROR: ul_namco_es2plus_run not exported" >&2
    exit 1
fi

# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir namco_es2plus)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module namco_es2plus "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_namco_es2plus_hook32.dll" "$INSTALL_DIR/ultra_namco_es2plus_hook64.dll"

echo "[namco_es2plus] done"
