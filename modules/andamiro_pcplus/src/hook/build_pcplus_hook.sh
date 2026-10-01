#!/usr/bin/env bash
# =============================================================================
# Build the andamiro_pcplus board's OWN, fully independent I/O hook DLL, both
# bitnesses:
#   ultra_andamiro_pcplus_hook32.dll   (i686-w64-mingw32,   32-bit games)
#   ultra_andamiro_pcplus_hook64.dll   (x86_64-w64-mingw32, 64-bit games)
#
# This tree (modules/andamiro_pcplus/src/hook/) is self-contained: it has its
# own copies of the JVS engine, the JVS-over-serial transport hook, the D3D9
# present hook, the DirectInput8 emulation, the DirectSound/XAudio2 diagnostic
# hook, and the Ultra Omni-Bridge gamepad-to-mouse/keyboard synthesis hook
# (omni_bridge.c, adapted from native_x86's reference copy per the project's
# per-board independence rule; config comes from the shared
# work/include/omni_client.h, hence the -I below). It does NOT #include, link
# against, or depend on the compiled output of native_x86/src/hook/ (that tree
# was read only as reference) or of any other board module.
#
# andamiro_pcplus_main.c (../andamiro_pcplus_main.c) searches for this DLL by
# exact name (HOOK_DLL_PREF_32/64) before falling back to the shared Type X
# hook, so the output names below are load-bearing.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/andamiro_pcplus/src/hook
#   bash build_pcplus_hook.sh
# =============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children so cc1/as fall back to an
# unwritable C:\WINDOWS temp dir; give gcc a writable temp inline (the
# project-wide Makefile/build_hook.sh pattern). -pipe also avoids the
# intermediate .s file entirely.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC="jvs_core_pcplus.c jvs_serial_hook_pcplus.c dinput8_emu_pcplus.c d3d9_throttle_pcplus.c audio_hook_pcplus.c omni_bridge.c pause_menu_wire_pcplus.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_pcplus.c ../../../../../../work/src/ultra_transport.c"

# Ultra Omni-Bridge (omni_bridge.c) reads its tuning from the shared
# work/include/omni_client.h (omni_client_read_bridge()) -- same loader
# headers dir every other board's build points at (see native_x86's
# build_hook.sh LOADER_INC). Header-only, no-alloc: no new LIBS needed.
LOADER_INC="../../../../../../work/include"

# -static(-libgcc): self-contained DLL, no libgcc_s/libwinpthread dependency --
# the injected hook loads inside the game process, which has no mingw runtime.
# -pipe: see the TMP note above.
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc"
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm"

OUT32="ultra_andamiro_pcplus_hook32.dll"
OUT64="ultra_andamiro_pcplus_hook64.dll"

echo "[32] $OUT32"
PATH="$M32:$PATH" env $T i686-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -m32 -shared -o "$OUT32" $SRC $LIBS \
    -Wl,--out-implib,ultra_andamiro_pcplus_hook32.lib

echo "[64] $OUT64"
PATH="$M64:$PATH" env $T x86_64-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -m64 -shared -o "$OUT64" $SRC $LIBS \
    -Wl,--out-implib,ultra_andamiro_pcplus_hook64.lib

echo "OK:"; ls -la "$OUT32" "$OUT64"

# Verify the diagnostic export both DLLs must carry.
for out in "$OUT32" "$OUT64"; do
    bits=32; objdump=/c/msys64/mingw32/bin/objdump.exe
    if [ "$out" = "$OUT64" ]; then bits=64; objdump=/c/msys64/mingw64/bin/objdump.exe; fi
    if "$objdump" -p "$out" | grep -q "ultra_andamiro_pcplus_hook_id"; then
        echo "[andamiro_pcplus] $bits-bit export OK: ultra_andamiro_pcplus_hook_id"
    else
        echo "[andamiro_pcplus] ERROR: ultra_andamiro_pcplus_hook_id not exported ($bits-bit)" >&2
        exit 1
    fi
done
