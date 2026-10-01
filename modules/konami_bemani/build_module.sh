#!/usr/bin/env bash
# Build the Konami Bemani PC per-board module: the launcher DLL + this board's
# OWN independent hook DLL (both bitnesses).
#
# Produces (in this module folder):
#   ultra_konami_bemani.dll           -- launcher (spawn + inject)
#   ultra_konami_bemani_hook32.dll    -- this board's OWN 32-bit I/O hook
#   ultra_konami_bemani_hook64.dll    -- this board's OWN 64-bit I/O hook
#
# The launcher DLL exports `ul_konami_bemani_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Konami
# Bemani PC .exe game to the "konami_bemani" board profile. konami_bemani_main.c
# then sniffs the game's PE bitness, searches for ultra_konami_bemani_hook{32,64}.dll
# (falling back to the shared Type X hook ONLY if this board's own hook is not
# installed -- see find_hook() in that file), and injects it via
# QueueUserAPC(LoadLibraryA) (primary) / CreateRemoteThread(LoadLibraryA)
# (fallback).
#
# This board's hook DLL is a FULLY SELF-CONTAINED, board-own build: every
# source file lives under modules/konami_bemani/src/hook/ (jvs_core_bemani.c,
# konami_io_bemani.c, d3d9_throttle_bemani.c, dinput8_emu_bemani.c,
# audio_hook_bemani.c, hook_dllmain_bemani.c) and is compiled by
# build_bemani_hook.sh -- it does NOT compile, link, or depend at runtime on
# native_x86/src/hook/'s compiled output, or on any other board module
# (including konami_pc), per project policy that every board get its own
# independent hook DLL.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/konami_bemani && bash build_module.sh

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
# bytes exist to embed. See build_bemani_hook.sh for the full recipe / policy
# notes.
echo "[konami_bemani] building this board's OWN hook DLL (build_bemani_hook.sh)"
bash "$HERE/build_bemani_hook.sh"

if [ ! -f "$HERE/ultra_konami_bemani_hook32.dll" ] || [ ! -f "$HERE/ultra_konami_bemani_hook64.dll" ]; then
    echo "[konami_bemani] ERROR: hook build did not produce ultra_konami_bemani_hook{32,64}.dll" >&2
    exit 1
fi

# Build the DLL-game host EXE (bemani_dll_host.exe).  Konami Bemani PC games
# ship a modules/launch.dll as the only executable unit -- Windows cannot
# CreateProcessA a DLL, so this tiny console EXE acts as the host process.
# It is embedded as DLLHOST32 alongside the hook DLLs and extracted to temp
# at launch time by konami_bemani_main.c.
echo "[konami_bemani] building bemani_dll_host.exe"
HOST_CFLAGS=(-pipe -m32 -O2 -w -D_WIN32_WINNT=0x0601)
HOST_LDFLAGS=(-m32 -mconsole -static -lkernel32 -luser32)
TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${HOST_CFLAGS[@]}" \
    "src/bemani_dll_host.c" -o bemani_dll_host.exe "${HOST_LDFLAGS[@]}"
ls -la bemani_dll_host.exe

# Embed hooks AND the host EXE as RCDATA resources inside the main module DLL
# (hook_res.rc) instead of shipping them as standalone files -- one file to
# sign/verify per board. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[konami_bemani] embedding hooks + dll host as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_konami_bemani.dll"
SRCS=("src/konami_bemani_main.c")

echo "[konami_bemani] building $OUT (launcher: spawn + inject)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_konami_bemani.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_konami_bemani_run"; then
    echo "[konami_bemani] export OK: ul_konami_bemani_run"
else
    echo "[konami_bemani] ERROR: ul_konami_bemani_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\konami_bemani\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir konami_bemani)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module konami_bemani "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_konami_bemani_hook32.dll" "$INSTALL_DIR/ultra_konami_bemani_hook64.dll"

echo "[konami_bemani] done"
