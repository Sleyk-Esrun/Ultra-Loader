#!/usr/bin/env bash
# =============================================================================
#  build_ringedge2_hook.sh -- Build ultra_sega_ringedge2_hook{32,64}.dll, the
#                             Sega RingEdge 2 board's OWN fully independent I/O
#                             + present hook.
# =============================================================================
#
#  Modeled on modules/native_x86/src/hook/build_hook.sh, but this tree links
#  ONLY the files in THIS directory -- no file from modules/native_x86/src/
#  hook/ or modtest/ is #included, linked, or copied at build or run time.
#  Everything this board needs (JVS engine, JVS-serial transport, mxparallel
#  presence-only keychip shim, D3D9 AND D3D11 present hooks, audio hook,
#  DllMain) is its own compiled copy under this board's own symbol names.
#
#  Output: ultra_sega_ringedge2_hook32.dll / ultra_sega_ringedge2_hook64.dll,
#  written into THIS directory (src/hook/), then build_module.sh copies both
#  up into the module root + the installed module folder, exactly like it
#  already does for the ultra_sega_ringedge2.dll skeleton launcher.
#
#  -static / -static-libgcc  -- self-contained, no libgcc_s/libwinpthread dep
#                                once injected into the game process (which has
#                                no mingw runtime of its own).
#  -pipe                     -- MSYS strips TMP/TEMP from native children, so
#                                cc1 would otherwise write its temp .s file into
#                                C:\WINDOWS (unwritable) and `as` fails; -pipe
#                                removes the temp file entirely.
#  TMP/TEMP/TMPDIR inline    -- belt-and-suspenders: force a writable temp for
#                                the whole toolchain regardless of shell.
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/sega_ringedge2/src/hook &&
#      bash build_ringedge2_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[re2-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[re2-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[re2-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[re2-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="ringedge2_hook_main.c \
      pcp_daemon_ringedge2.c \
      watchdog_ringedge2.c \
      jvs_core_ringedge2.c \
      jvs_serial_ringedge2.c \
      amcredit_ringedge2.c \
      keychip_ringedge2.c \
      d3d9_present_ringedge2.c \
      d3d11_present_ringedge2.c \
      audio_ringedge2.c \
      omni_bridge.c \
      pause_menu_wire_ringedge2.c \
      ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c ../../../../../../work/src/ultra_tracer.c"

# LOADER_INC: omni_bridge.c #include "omni_client.h" -- the shared, header-
# only, no-alloc Omni config struct at work/include/omni_client.h (also used
# by Omni-Play/Omni-Track/Omni-Wheel/Omni-Steer). Same depth/path as every
# other board's own hook tree (modules/<board>/src/hook/ -> repo root is 6
# levels up), see modules/sega_aurora/src/hook/build_aurora_hook.sh for the
# identical pattern.
LOADER_INC="../../../../../../work/include"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I. -I$LOADER_INC"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32/gdi32 (the
# present-hook output-window resize + splash dismiss), winmm (frame-limiter
# timeBeginPeriod/timeEndPeriod). No -ld3d9/-ld3d11/-ldxgi/-ldxguid: every D3D9/
# D3D11/DXGI call in this tree goes through a resolved function pointer or a
# COBJMACROS vtable call, and the DXGI IIDs are defined locally (see
# d3d11_present_ringedge2.c) -- so nothing here is a link-time import.
# ws2_32: pcp_daemon_ringedge2.c serves the local Sega daemon protocol on
# loopback. This board cannot reach a frame without it -- the game blocks its
# main thread on a 60s receive timeout waiting for one.
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm -lws2_32"

OUT32="ultra_sega_ringedge2_hook32.dll"
OUT64="ultra_sega_ringedge2_hook64.dll"

echo "[re2-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS \
    -Wl,--out-implib,ultra_sega_ringedge2_hook32.lib
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[re2-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[re2-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS \
    -Wl,--out-implib,ultra_sega_ringedge2_hook64.lib
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[re2-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify the confirmability export landed in both.
for dll in "$OUT32" "$OUT64"; do
    bits=32; obj="$M32/objdump.exe"
    if [ "$dll" = "$OUT64" ]; then bits=64; obj="$M64/objdump.exe"; fi
    if "$obj" -p "$dll" | grep -q "ultra_sega_ringedge2_hook_id"; then
        echo "[re2-hook-build] export OK ($bits-bit): ultra_sega_ringedge2_hook_id"
    else
        echo "[re2-hook-build] ERROR ($bits-bit): ultra_sega_ringedge2_hook_id not exported" >&2
        exit 1
    fi
done

# Stage a copy up in the module root (alongside ultra_sega_ringedge2.dll) so a
# dev run from the module folder finds it without a separate install step.
MODULE_ROOT="../.."
if [ -d "$MODULE_ROOT" ]; then
    cp -f "$OUT32" "$OUT64" "$MODULE_ROOT/"
    echo "[re2-hook-build] copied both DLLs to $MODULE_ROOT/"
fi

echo "[re2-hook-build] DONE"
ls -la "$OUT32" "$OUT64"
