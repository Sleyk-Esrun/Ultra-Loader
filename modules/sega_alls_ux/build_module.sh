#!/usr/bin/env bash
# Build the Sega ALLS UX per-board module DLL + its OWN independent hook DLL.
#
# Produces:
#   ultra_sega_alls_ux.dll               (spawner/injector, this module folder)
#   ultra_sega_alls_ux_hook{32,64}.dll    (this board's own hook, built by the
#                                          sibling build_allsux_hook.sh from
#                                          src/hook/ -- see that file for the
#                                          full source list)
#
# The spawner DLL exports `ul_sega_alls_ux_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega ALLS
# UX .exe game to the "sega_alls_ux" board profile. sega_alls_ux_main.c uses
# ONLY ultra_sega_alls_ux_hook{32,64}.dll (this board's OWN D3D11 + IO4-USB +
# keychip-presence hook), embedded as an RCDATA resource inside the module DLL
# and extracted to a temp file at injection time -- no shared-hook fallback
# exists (removed 2026-08-02/03; a prior version of this comment described
# that fallback, but sega_alls_ux_main.c's find_hook() never actually
# implemented one). Per the project owner's directive, this board's hook
# shares NO runtime binary with any other board and has NO dependency on
# modules/native_x86's compiled output.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_alls_ux && bash build_module.sh

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

# Build this board's OWN independent hook DLL FIRST (src/hook/ tree; no
# dependency on native_x86 or any other board's compiled output), so its
# bytes exist to embed.
echo "[sega_alls_ux] building board-native hook (build_allsux_hook.sh)"
bash "$HERE/build_allsux_hook.sh"

if [ ! -f "$HERE/ultra_sega_alls_ux_hook32.dll" ] || [ ! -f "$HERE/ultra_sega_alls_ux_hook64.dll" ]; then
    echo "[sega_alls_ux] ERROR: hook build did not produce ultra_sega_alls_ux_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_alls_ux] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_alls_ux.dll"
SRCS=("src/sega_alls_ux_main.c")

echo "[sega_alls_ux] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_alls_ux.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_alls_ux_run"; then
    echo "[sega_alls_ux] export OK: ul_sega_alls_ux_run"
else
    echo "[sega_alls_ux] ERROR: ul_sega_alls_ux_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\sega_alls_ux\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir sega_alls_ux)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module sega_alls_ux "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. build_allsux_hook.sh still installs its own copies to
# INSTALL_DIR -- remove them here so only the one embedded-hook DLL ships.
rm -f "$INSTALL_DIR/ultra_sega_alls_ux_hook32.dll" "$INSTALL_DIR/ultra_sega_alls_ux_hook64.dll"

echo "[sega_alls_ux] done"
