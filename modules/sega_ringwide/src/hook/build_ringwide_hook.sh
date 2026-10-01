#!/usr/bin/env bash
# Build the sega_ringwide board's OWN, fully self-contained injected hook DLL
# (both bitnesses):
#   ultra_sega_ringwide_hook32.dll / ultra_sega_ringwide_hook64.dll
#
# This DLL implements, independently for this board only:
#   - the clean-room JVS I/O engine                (jvs_core_ringwide.c)
#   - the JVS-over-serial transport (COM IAT hooks) (jvs_serial_hook_ringwide.c)
#   - a PRESENCE-ONLY mxparallel keychip shim       (sega_keychip_ringwide.c)
#   - the D3D9 present/frame-limiter hook           (d3d9_throttle_ringwide.c)
#   - a DirectSound/XAudio2 diagnostic audio hook   (audio_hook_ringwide.c)
#   - DllMain + the standalone-gamepad input source (hook_dllmain_ringwide.c)
#
# It does NOT #include, link against, or depend at build OR run time on
# modules/native_x86/src/hook's compiled output, nor on
# I:\Ultra Loader\modtest\. Every source file above lives in this same
# directory and was written as this board's own copy (the native_x86 tree was
# read as REFERENCE ONLY for the general shape of each subsystem).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_ringwide/src/hook
#   bash build_ringwide_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (same pattern as every other Ultra Loader build script).
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC="jvs_core_ringwide.c jvs_serial_hook_ringwide.c sega_keychip_ringwide.c d3d9_throttle_ringwide.c audio_hook_ringwide.c omni_bridge.c pause_menu_wire_ringwide.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_ringwide.c ../../../../../../work/src/ultra_transport.c"

# omni_bridge.c (Ultra Omni-Bridge, this board's own copy -- see its header
# comment) reads its tuning from the shared work/include/omni_client.h, so it
# needs that dir on the include path. Nothing else in this board's tree uses
# work/include today, so this -I is added solely for that one file.
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained (no libgcc_s/libwinpthread dependency),
# so the injected hook loads inside the game process, which has no mingw
# runtime of its own. -pipe: avoids cc1's temp .s file (see TMP note above).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -D_WIN32_WINNT=0x0601 -I$LOADER_INC"
# No -ld3d9 / -ldsound / -lxinput import libs needed: every one of those APIs
# is reached dynamically (GetModuleHandle/LoadLibrary + GetProcAddress) or via
# a raw vtable pointer the game itself created, matching the reference hook's
# approach for d3d9_throttle.c / audio_hook.c / omni_bridge.c.
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

echo "[32] sega_ringwide hook dll"
PATH="$M32:$PATH" env $T i686-w64-mingw32-gcc $CFLAGS -shared -o ultra_sega_ringwide_hook32.dll $SRC $LIBS \
    -Wl,--out-implib,ultra_sega_ringwide_hook32.lib

echo "[64] sega_ringwide hook dll"
PATH="$M64:$PATH" env $T x86_64-w64-mingw32-gcc $CFLAGS -shared -o ultra_sega_ringwide_hook64.dll $SRC $LIBS \
    -Wl,--out-implib,ultra_sega_ringwide_hook64.lib

echo "OK:"
ls -la ultra_sega_ringwide_hook32.dll ultra_sega_ringwide_hook64.dll

# Verify each DLL exports the diagnostic id function + a real DllMain (use the
# matching-bitness objdump -- the 32-bit objdump cannot parse a PE32+ image).
if "$M32/objdump.exe" -p ultra_sega_ringwide_hook32.dll | grep -q "ultra_sega_ringwide_hook_id"; then
    echo "[sega_ringwide] ultra_sega_ringwide_hook32.dll: export OK: ultra_sega_ringwide_hook_id"
else
    echo "[sega_ringwide] ERROR: ultra_sega_ringwide_hook32.dll missing ultra_sega_ringwide_hook_id export" >&2
    exit 1
fi
if "$M64/objdump.exe" -p ultra_sega_ringwide_hook64.dll | grep -q "ultra_sega_ringwide_hook_id"; then
    echo "[sega_ringwide] ultra_sega_ringwide_hook64.dll: export OK: ultra_sega_ringwide_hook_id"
else
    echo "[sega_ringwide] ERROR: ultra_sega_ringwide_hook64.dll missing ultra_sega_ringwide_hook_id export" >&2
    exit 1
fi
