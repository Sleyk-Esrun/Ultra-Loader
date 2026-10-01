#!/usr/bin/env bash
# Build the Unico PC per-board module DLL + its own independent I/O hook.
#
# Produces:
#   1. ultra_unico_pc.dll                       (this module folder)
#      Exports `ul_unico_pc_run(int argc, char **argv)`. The loader's
#      dispatcher (work/src/main.c) calls this after resolving a Unico PC-
#      arcade title to board "unico_pc" via the unified board profile.
#      ul_unico_pc_run() spawns the game suspended and injects a hook DLL --
#      see src/unico_pc_main.c's header for the exact search order.
#   2. ultra_unico_pc_hook{32,64}.dll           (via build_unico_hook.sh)
#      This board's OWN fully independent, self-contained I/O hook -- JVS-
#      over-serial + D3D9 present/throttle + synthetic DirectInput8 + audio
#      diagnostics + dialog auto-dismiss. Built from modules/unico_pc/src/
#      hook/ ONLY; no shared source or compiled output with native_x86's hook
#      or any other board's module. See that script's own header for the
#      full subsystem list.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/unico_pc && bash build_module.sh

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

# -----------------------------------------------------------------------
# Build this board's OWN independent I/O hook FIRST (JVS-over-serial + D3D9
# + DirectInput8 + audio + dialog-dismiss), so its bytes exist to embed.
# -----------------------------------------------------------------------
echo "[unico_pc] building own independent hook (ultra_unico_pc_hook{32,64}.dll)"
bash "$HERE/build_unico_hook.sh"

if [ ! -f "$HERE/ultra_unico_pc_hook32.dll" ] || [ ! -f "$HERE/ultra_unico_pc_hook64.dll" ]; then
    echo "[unico_pc] ERROR: hook build did not produce ultra_unico_pc_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[unico_pc] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_unico_pc.dll"
SRCS=("src/unico_pc_main.c")

echo "[unico_pc] building $OUT (32-bit skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_unico_pc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_unico_pc_run"; then
    echo "[unico_pc] export OK: ul_unico_pc_run"
else
    echo "[unico_pc] ERROR: ul_unico_pc_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\unico_pc\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir unico_pc)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module unico_pc "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_unico_pc_hook32.dll" "$INSTALL_DIR/ultra_unico_pc_hook64.dll"

echo "[unico_pc] done"
