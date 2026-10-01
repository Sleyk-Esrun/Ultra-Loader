#!/usr/bin/env bash
# =============================================================================
#  build_typex4_hook.sh -- Build ultra_taito_typex4_hook{32,64}.dll from THIS
#                          board's own hook sources (this folder only).
# =============================================================================
#
#  Compiles the Taito Type X4 hook DLL that ../taito_typex4_main.c (this
#  board's spawner) injects into the game process. Dual-build: 32-bit AND
#  64-bit -- Type X4 titles are 64-bit, but the 32-bit artifact is built too
#  for parity with the spawner's PE-bitness dispatch (and any 32-bit Type X4
#  tooling) so the module folder is self-contained either way.
#
#  SELF-CONTAINED: every source file compiled here lives in this same folder
#  (modules/taito_typex4/src/hook/). Nothing is #included, linked, or copied
#  at runtime from modules/native_x86/src/hook/ or from modtest/ -- this is a
#  fully independent per-board hook, per the project owner's standing
#  directive that every modern arcade board gets its own self-contained hook
#  DLL with no shared runtime binary and no build/run dependency on the
#  native_x86 substrate.
#
#  Output files (this folder):
#    1. ultra_taito_typex4_hook32.dll
#    2. ultra_taito_typex4_hook64.dll
#  ../build_module.sh copies both into the module's install directory
#  alongside ultra_taito_typex4.dll (the spawner), matching the hook-name +
#  search-path contract in ../src/taito_typex4_main.c's find_hook().
#
#  Follows the same recipe as modules/sega_nu/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  NO auto-install here. build_module.sh (one level up) does the staging.
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[tx4-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[tx4-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[tx4-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[tx4-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_typex4.c \
      taito_fastio_typex4.c \
      d3d11_throttle_typex4.c \
      audio_hook_typex4.c \
      felica_spoof_typex4.c \
      omni_bridge.c \
      pause_menu_wire_typex4.c \
      ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c \
      hook_dllmain_typex4.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# LOADER_INC: Ultra Omni-Bridge's config (omni_bridge.c #includes omni_client.h)
# lives in the loader's shared work/include/, same tree every Omni service's
# client-side config header ships from -- NOT a cross-board dependency (it's
# loader-shared infra, same class of thing as work/include/inject.h that this
# board's own spawner already depends on). omni_bridge.c itself is this
# board's OWN copy (see the file header), not a link against native_x86.
LOADER_INC="../../../../../../work/include"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I. -I$LOADER_INC"

# psapi (EnumProcessModules for the IAT walker), user32 (window/keyboard/
# GetAsyncKeyState for the standalone input capture + audio fallback window),
# winmm (timeBeginPeriod/timeEndPeriod for the D3D11 frame limiter). No
# -ld3d11/-ldxgi/-ldsound: every D3D11/DXGI/DirectSound entry point this hook
# calls is resolved dynamically via GetProcAddress or reached through a COM
# vtable pointer already handed back by the game, so no import library is
# needed for any of them (same design as the project's other D3D11/audio
# hooks).
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="ultra_taito_typex4_hook32.dll"
OUT64="ultra_taito_typex4_hook64.dll"

echo "[tx4-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[tx4-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[tx4-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[tx4-hook-build] built: $OUT64 ($sz64 bytes)"

echo "[tx4-hook-build] DONE"
ls -la "$OUT32" "$OUT64"
