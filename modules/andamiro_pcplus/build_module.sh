#!/usr/bin/env bash
# Build the Andamiro PC Plus per-board module DLL + its OWN I/O hook DLLs.
#
# Produces (in this module folder):
#   ultra_andamiro_pcplus.dll          -- the spawner/injector module (32-bit)
#   ultra_andamiro_pcplus_hook32.dll   -- this board's OWN I/O hook (32-bit games)
#   ultra_andamiro_pcplus_hook64.dll   -- this board's OWN I/O hook (64-bit games)
#
# The module DLL exports `ul_andamiro_pcplus_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving an Andamiro
# PC Plus .exe game to the "andamiro_pcplus" board profile; that function then
# spawns the game suspended and injects one of the two hook DLLs above
# (picked by the game's own PE bitness -- see find_hook()/game_pe_bits() in
# src/andamiro_pcplus_main.c).
#
# The hook DLLs are built from src/hook/ -- a tree fully independent of
# native_x86/src/hook/ and of every other board module (own JVS engine, own
# JVS-serial transport hook, own D3D9 present hook, own DirectInput8
# emulation, own DirectSound/XAudio2 diagnostic hook; see src/hook/build_pcplus_hook.sh
# for the full source list). This replaces the earlier runtime-only fallback,
# where no board-native hook existed yet and andamiro_pcplus_main.c could only
# fall back to the shared Type X hook (ultra_typex_hook{32,64}.dll) -- that
# fallback path still exists in andamiro_pcplus_main.c as a safety net, but is
# no longer needed at runtime now that this board has its own hook.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/andamiro_pcplus && bash build_module.sh

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

# ---------------------------------------------------------------------------
# This board's OWN, fully independent I/O hook DLLs (both bitnesses), built
# FIRST so their bytes exist to embed. No shared runtime binary and no
# dependency on native_x86/src/hook/'s compiled output or on any other board
# module.
echo "[andamiro_pcplus] building board-native I/O hook (src/hook/build_pcplus_hook.sh)"
bash "$HERE/src/hook/build_pcplus_hook.sh"

HOOK32="ultra_andamiro_pcplus_hook32.dll"
HOOK64="ultra_andamiro_pcplus_hook64.dll"
if [ ! -f "$HERE/src/hook/$HOOK32" ] || [ ! -f "$HERE/src/hook/$HOOK64" ]; then
    echo "[andamiro_pcplus] ERROR: hook build did not produce $HOOK32/$HOOK64" >&2
    exit 1
fi
ls -la "$HERE/src/hook/$HOOK32" "$HERE/src/hook/$HOOK64"

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[andamiro_pcplus] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_andamiro_pcplus.dll"
SRCS=("src/andamiro_pcplus_main.c")

echo "[andamiro_pcplus] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_andamiro_pcplus.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_andamiro_pcplus_run"; then
    echo "[andamiro_pcplus] export OK: ul_andamiro_pcplus_run"
else
    echo "[andamiro_pcplus] ERROR: ul_andamiro_pcplus_run not exported" >&2
    exit 1
fi

# Auto-install to the SINGLE supported install location per user spec:
#   %LOCALAPPDATA%\Ultra Loader\modules\andamiro_pcplus\
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir andamiro_pcplus)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module andamiro_pcplus "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/$HOOK32" "$INSTALL_DIR/$HOOK64" "$HERE/$HOOK32" "$HERE/$HOOK64"

echo "[andamiro_pcplus] done"
