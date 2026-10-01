#!/usr/bin/env bash
# =============================================================================
#  build_hook.sh -- Build ultra_rawthrills_hook32.dll from the 9 stub TUs.
# =============================================================================
#
#  Compiles the Raw Thrills PC arcade hook DLL (32-bit only -- sdaemon.exe and
#  every RawApp binary in the wild are 32-bit) that rawthrills_pc_main.c
#  injects into the game process. The DLL is dropped into two locations:
#
#    1. modules/rawthrills_pc/                                (module dir)
#    2. Completed Ultra Loader Cores/native/Ultra Raw Thrills (release dir)
#
#  The module's spawner searches: %LOCALAPPDATA%\Ultra Loader\modules\rawthrills_pc\
#  -> loader-exe-dir -> loader-exe-dir\modtest\. In practice #1 is what the
#  packaged loader ships; #2 is the manual test drop.
#
#  Follows the exact same recipe as the Type X hook's build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s/libwinpthread
#                                 dep once injected into the game process
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  NO auto-install. The build produces the DLL and copies it to the two
#  locations above; the user is responsible for installing it if they want it
#  in %LOCALAPPDATA%.
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[rt-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[rt-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
SRCS="$SRC_DIR/rawthrills_hook_main.c \
      $SRC_DIR/jammausb_stub.c \
      $SRC_DIR/usbwd_stub.c \
      $SRC_DIR/porttalk_bypass.c \
      $SRC_DIR/ewf_stub.c \
      $SRC_DIR/rawart_redirect.c \
      $SRC_DIR/d3d8_throttle_rawthrills.c \
      $SRC_DIR/d3d9_throttle_rawthrills.c \
      $SRC_DIR/d3d11_throttle_rawthrills.c \
      $SRC_DIR/audio_hook_rawthrills.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_rawthrills.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c"

# LOADER_INC: this board's own copy of omni_bridge.c (added 2026-08-02, ported
# from modules/native_x86/src/hook/omni_bridge.c) includes the shared
# work/include/omni_client.h for its ULTRA_BRIDGE_* config struct -- same
# header every other board/module reads for the other four Omni services'
# config. Relative path: rawthrills_pc/ -> modules/ -> ultra-loader-modules/
# -> modules_pkg/ -> Ultra Loader/ -> work/include.
LOADER_INC="../../../../work/include"

# -Wno-unused-function silences the extended EWF exports that are unused
# unless the game imports them (we compile them in for future use as a
# drop-in ewfapi.dll shim).
CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -pipe -static -static-libgcc \
        -I$SRC_DIR -I$LOADER_INC"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32/gdi32/winmm
# for the general Win32 surface, advapi32 for SCM intercepts. hid.dll,
# d3d8/d3d9/d3d11/dxgi.dll and dsound.dll/xaudio2_*.dll are all loaded
# dynamically via LoadLibrary in their respective stub -- no -ld3d8/-ld3d9/
# -ld3d11/-ldxgi/-ldxguid/-ldsound on the linker line (avoids a hard dep on
# those import libs being present in the sysroot; the D3D11 file's
# initguid.h + DEFINE_GUID block is what makes -ldxguid unnecessary).
LIBS="-lpsapi -luser32 -ladvapi32 -lwinmm -lgdi32"

OUT="ultra_rawthrills_hook32.dll"

echo "[rt-hook-build] compiling $OUT (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT" $SRCS $LIBS

echo "[rt-hook-build] built: $(ls -la $OUT | awk '{print $5,$9}')"

# Drop into the release directory too so the loader-dir search order finds it.
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra Raw Thrills"
if [ -d "$REL_DIR" ]; then
    cp "$OUT" "$REL_DIR/"
    echo "[rt-hook-build] copied to $REL_DIR/"
else
    echo "[rt-hook-build] NOTE: release dir $REL_DIR does not exist -- skipped copy"
fi

echo "[rt-hook-build] DONE"
