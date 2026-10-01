#!/usr/bin/env bash
# =============================================================================
#  build_ringedge_hook.sh -- Build ultra_sega_ringedge_hook{32,64}.dll from
#                             THIS BOARD'S OWN 6 source files.
# =============================================================================
#
#  Compiles the Sega RingEdge hook DLL that sega_ringedge_main.c injects into
#  the game process (find_hook() searches for ultra_sega_ringedge_hook<bits>.dll
#  FIRST, before falling back to the shared Type X hook). Dual-build: 32-bit AND
#  64-bit, even though RingEdge itself is a 32-bit board (BP boards vary; a
#  matching pair keeps the module folder self-contained the same way every
#  other native module ships both bitnesses).
#
#  Source files (ALL under src/hook/, ALL this board's own independent copies --
#  none of them #include, link against, or are copied at runtime from the
#  native_x86 module or modtest/):
#    jvs_core_ringedge.c        -- clean-room JVS I/O engine
#    jvs_serial_hook_ringedge.c -- JVS-over-serial (COM2/3) transport + IAT walker
#    sega_keychip_ringedge.c    -- mxparallel keychip shim (presence-only, env-gated)
#    d3d9_throttle_ringedge.c   -- D3D9 present hook (frame limiter + force-windowed)
#    audio_hook_ringedge.c      -- DirectSound/XAudio2 diagnostic hook
#    omni_bridge.c              -- Ultra Omni-Bridge (KBM emulation from gamepad/
#                                   lightgun); board-own copy of the reference at
#                                   native_x86/src/hook/omni_bridge.c, opt-in via
#                                   ULTRA_BRIDGE_ENABLE (default OFF)
#    hook_dllmain_ringedge.c    -- DllMain + shared glue (log, JVS node, gamepad fallback)
#
#  Follows the same recipe as modules/sega_nu/build_hook.sh and
#  modules/native_x86/src/hook/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  Output: ultra_sega_ringedge_hook32.dll / ultra_sega_ringedge_hook64.dll,
#  placed in this module folder (where sega_ringedge_main.c's find_hook_named()
#  looks first, alongside %LOCALAPPDATA%\Ultra Loader\modules\sega_ringedge\).
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/sega_ringedge && bash build_ringedge_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[re-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[re-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[re-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[re-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
# work/include -- shared, header-only Omni-client config (omni_client.h); read
# by omni_bridge.c only. Same relative-path recipe as every other module's own
# build script that needs it (see native_x86/src/hook/build_hook.sh's LOADER_INC).
LOADER_INC="../../../../work/include"
SRCS="$SRC_DIR/jvs_core_ringedge.c \
      $SRC_DIR/jvs_serial_hook_ringedge.c \
      $SRC_DIR/sega_keychip_ringedge.c \
      $SRC_DIR/d3d9_throttle_ringedge.c \
      $SRC_DIR/audio_hook_ringedge.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_ringedge.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c \
      $SRC_DIR/hook_dllmain_ringedge.c ../../../../work/src/ultra_transport.c"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32 (windows/
# events/RegisterClass for the audio fallback hwnd + splash dismiss), winmm
# (timeBeginPeriod/timeEndPeriod for the D3D9 frame limiter). No -ld3d9/-ldsound
# needed: every D3D9/DirectSound entry point is resolved dynamically via
# GetModuleHandle/LoadLibrary+GetProcAddress, and COM calls go through each
# object's own vtable pointer (COBJMACROS), not an imported symbol.
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="ultra_sega_ringedge_hook32.dll"
OUT64="ultra_sega_ringedge_hook64.dll"

echo "[re-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[re-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[re-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[re-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify the diagnostic export landed in both.
for out in "$OUT32" "$OUT64"; do
    bits=32; obj="$M32/objdump.exe"
    [ "$out" = "$OUT64" ] && { bits=64; obj="$M64/objdump.exe"; }
    if "$obj" -p "$out" | grep -q "ultra_sega_ringedge_hook_id"; then
        echo "[re-hook-build] export OK ($bits-bit): ultra_sega_ringedge_hook_id"
    else
        echo "[re-hook-build] ERROR: ultra_sega_ringedge_hook_id not exported in $out" >&2
        exit 1
    fi
done

echo "[re-hook-build] DONE"
ls -la "$OUT32" "$OUT64"
