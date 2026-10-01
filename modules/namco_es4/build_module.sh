#!/usr/bin/env bash
# Build the Namco ES4 per-board module DLL + this board's OWN fully
# independent, self-contained I/O hook DLL.
#
# Produces:
#   ultra_namco_es4.dll                              (launcher, in this folder)
#   src/hook/ultra_namco_es4_hook{32,64}.dll          (built by src/hook/build_es4_hook.sh)
#
# The hook is namco_es4's OWN tree (src/hook/*_es4.c) -- it does not link
# against, or depend on the compiled output of, native_x86/src/hook/ or any
# other board's module folder. Both DLLs are staged to this board's install
# dir so namco_es4_main.c's find_hook_named() picks up the board-native hook
# first, no modtest fallback needed.

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

# Build this board's OWN fully independent, self-contained I/O hook FIRST
# (ultra_namco_es4_hook{32,64}.dll, from src/hook/*_es4.c -- no shared runtime
# binary with native_x86 or any other board), so its bytes exist to embed.
HOOK_DIR="$HERE/src/hook"
echo "[namco_es4] building board-native hook (src/hook/build_es4_hook.sh)"
( cd "$HOOK_DIR" && bash build_es4_hook.sh )

for hook in ultra_namco_es4_hook32.dll ultra_namco_es4_hook64.dll; do
    if [ ! -f "$HOOK_DIR/$hook" ]; then
        echo "[namco_es4] ERROR: $hook was not produced by build_es4_hook.sh" >&2
        exit 1
    fi
done

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[namco_es4] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_namco_es4.dll"
SRCS=("src/namco_es4_main.c")

echo "[namco_es4] building $OUT (skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_namco_es4.lib

ls -la "$OUT"

if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_namco_es4_run"; then
    echo "[namco_es4] export OK: ul_namco_es4_run"
else
    echo "[namco_es4] ERROR: ul_namco_es4_run not exported" >&2
    exit 1
fi

# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir namco_es4)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module namco_es4 "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_namco_es4_hook32.dll" "$INSTALL_DIR/ultra_namco_es4_hook64.dll"

echo "[namco_es4] done"
