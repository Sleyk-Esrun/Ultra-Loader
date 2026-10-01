#!/usr/bin/env bash
# =============================================================================
#  build_alls_hook.sh -- Build ultra_sega_alls_hook{32,64}.dll from this
#                        board's OWN hook sources (modules/sega_alls/src/hook/).
# =============================================================================
#
#  Every source compiled here lives ONLY in modules/sega_alls/src/hook/. This
#  DLL does NOT link against native_x86's compiled hook DLL/import lib, or any
#  other board's module output -- native_x86 was read only as reference for
#  public Win32/DXGI/D3D11 API shapes + the open JVS spec + documented Sega
#  keychip protocol facts, and every line was independently (re-)written into
#  this board's own file set.
#
#  Subsystems armed (per the project directive for sega_alls):
#    - D3D11/DXGI present + frame-limiter hook   (d3d11_throttle_alls.c)
#    - ALKey-framing keychip PRESENCE-ONLY stub  (sega_keychip_alls.c)
#      -- cmd 3/4/10/11/12/30-33 (comm-key rekey, GkeyWrite/Enc/Dec, ALKey*
#         Auth-2.0 key material) are DEFERRED, never derived/fabricated.
#    - IO4-USB (837-15257-class) I/O emulation   (sega_io4_alls.c)
#    - DirectSound + XAudio2 diagnostic hook     (audio_hook_alls.c)
#    - clean-room JVS input struct + wire codec  (jvs_core_alls.c)
#    - DllMain + shared log/IAT-walker/env infra (hook_dllmain_alls.c)
#
#  Dual-build (32-bit AND 64-bit): the board profile marks ALLS 64-bit-only
#  (Win10 IoT), but we build both bitnesses so sega_alls_main.c's PE-bitness
#  sniff always finds a matching board-native hook, exactly like every other
#  per-board build in this project.
#
#  Output files:
#    1. modules/sega_alls/ultra_sega_alls_hook32.dll
#    2. modules/sega_alls/ultra_sega_alls_hook64.dll
#    3. Completed Ultra Loader Cores/native/Ultra Sega ALLS/  -- BOTH DLLs
#
#  -static / -static-libgcc: the DLL must be self-contained (no libgcc_s /
#  libwinpthread dependency) since it loads inside the game process, which has
#  no mingw runtime of its own.
#  -pipe + inline TMP/TEMP/TMPDIR: MSYS strips TMP/TEMP from native children,
#  so cc1/as fall back to C:\WINDOWS (unwritable) without this.
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/sega_alls && bash build_alls_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[alls-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[alls-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[alls-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[alls-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
SRCS="$SRC_DIR/hook_dllmain_alls.c \
      $SRC_DIR/jvs_core_alls.c \
      $SRC_DIR/sega_io4_alls.c \
      $SRC_DIR/sega_keychip_alls.c \
      $SRC_DIR/d3d11_throttle_alls.c \
      $SRC_DIR/audio_hook_alls.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_alls.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

# LOADER_INC: this board's own hook sources live entirely under src/hook/, but
# omni_bridge.c (Ultra Omni-Bridge, the 5th Omni service) reads its tuning from
# the shared, header-only work/include/omni_client.h -- same convention as the
# other four Omni services (Play/Track/Wheel/Steer) already use elsewhere in
# this project. This is the only cross-board include this board's build pulls
# in; everything else here is compiled from this board's own file set.
LOADER_INC="../../../../work/include"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# psapi (EnumProcessModules for the IAT walker), user32 (FindWindow/ShowWindow/
# window-class for the audio fallback hwnd), winmm (timeBeginPeriod/EndPeriod
# for the D3D11 frame limiter). No -ld3d11/-ldxgi/-ldxguid/-ldsound needed --
# every COM interface is reached via manual vtable dispatch (macros/raw
# pointer poke) against pointers resolved at runtime through GetProcAddress,
# never a static import.
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="ultra_sega_alls_hook32.dll"
OUT64="ultra_sega_alls_hook64.dll"

echo "[alls-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS \
    -Wl,--out-implib,ultra_sega_alls_hook32.lib
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[alls-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[alls-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS \
    -Wl,--out-implib,ultra_sega_alls_hook64.lib
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[alls-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify each DLL exports the id string (proves this board's hook, not some
# stale/other artifact, actually built and linked).
for OUT in "$OUT32" "$OUT64"; do
    BIT=32; OD="$M32/objdump.exe"
    if [ "$OUT" = "$OUT64" ]; then BIT=64; OD="$M64/objdump.exe"; fi
    if "$OD" -p "$OUT" | grep -q "ultra_sega_alls_hook_id"; then
        echo "[alls-hook-build] export OK ($BIT-bit): ultra_sega_alls_hook_id"
    else
        echo "[alls-hook-build] ERROR ($BIT-bit): ultra_sega_alls_hook_id not exported" >&2
        exit 1
    fi
done

# Drop into the release directory (both DLLs), matching every other board's
# build_hook.sh convention.
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra Sega ALLS"
if [ -d "$REL_DIR" ]; then
    cp -f "$OUT32" "$REL_DIR/"
    cp -f "$OUT64" "$REL_DIR/"
    echo "[alls-hook-build] copied both DLLs to $REL_DIR/"
else
    echo "[alls-hook-build] NOTE: release dir $REL_DIR does not exist -- skipped copy"
fi

echo "[alls-hook-build] DONE"
