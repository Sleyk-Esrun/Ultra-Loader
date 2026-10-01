#!/usr/bin/env bash
# =============================================================================
#  build_gvr_hook.sh -- Build ultra_global_vr_hook{32,64}.dll from the 7 TUs in
#                       src/hook/.
# =============================================================================
#
#  Compiles the Global VR board-native hook DLL that global_vr_main.c injects
#  into the game process (QueueUserAPC(LoadLibraryA) primary, CreateRemoteThread
#  fallback -- see src/global_vr_main.c). This DLL is FULLY INDEPENDENT: it
#  shares no compiled object code with native_x86/src/hook/ or with any other
#  board's module -- every source file lives only in this module's own
#  src/hook/ tree (jvs_core_gvr.c, jvs_serial_hook_gvr.c, dinput8_emu_gvr.c,
#  d3d9_throttle_gvr.c, audio_hook_gvr.c, iat_patch_gvr.c, hook_dllmain_gvr.c).
#
#  Dual-build: 32-bit AND 64-bit -- global_vr_main.c sniffs the game .exe's PE
#  bitness at CreateProcess time and injects whichever hook matches.
#
#  Output files:
#    1. modules/global_vr/ultra_global_vr_hook32.dll   (module dir, staged by
#    2. modules/global_vr/ultra_global_vr_hook64.dll    build_module.sh)
#    3. Completed Ultra Loader Cores/native/Ultra Global VR/  -- BOTH DLLs
#
#  Follows the same recipe as modules/sega_nu/build_hook.sh and
#  modules/rawthrills_pc/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/global_vr && bash build_gvr_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[gvr-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[gvr-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[gvr-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[gvr-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
# Ultra Omni-Bridge (5th Omni service) reads its tuning from the loader's
# shared work/include/omni_client.h -- same -I recipe modules/native_x86/src/
# hook/build_hook.sh already uses for that header (LOADER_INC).
LOADER_INC="../../../../work/include"
SRCS="$SRC_DIR/hook_dllmain_gvr.c \
      $SRC_DIR/iat_patch_gvr.c \
      $SRC_DIR/jvs_core_gvr.c \
      $SRC_DIR/jvs_serial_hook_gvr.c \
      $SRC_DIR/dinput8_emu_gvr.c \
      $SRC_DIR/d3d9_throttle_gvr.c \
      $SRC_DIR/gl_throttle_gvr.c \
      $SRC_DIR/audio_hook_gvr.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_gvr.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32/gdi32/winmm
# for the Win32 window + frame-timing surface the D3D9/audio hooks touch,
# advapi32 rounds out the general surface. No -ldinput8/-ld3d9/-ldsound: every
# entry point on those APIs is resolved dynamically via GetProcAddress, never
# linked directly, so this DLL has no hard dependency on those import libs
# being present in the mingw sysroot.
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm -ladvapi32"

OUT32="ultra_global_vr_hook32.dll"
OUT64="ultra_global_vr_hook64.dll"

echo "[gvr-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS \
    -Wl,--out-implib,ultra_global_vr_hook32.lib
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[gvr-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[gvr-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS \
    -Wl,--out-implib,ultra_global_vr_hook64.lib
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[gvr-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify the export both objdumps agree exists (sanity: confirms DllMain's
# module actually linked, not just that gcc didn't error).
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT32" | grep -q "ultra_global_vr_hook_id"; then
    echo "[gvr-hook-build] export OK (32): ultra_global_vr_hook_id"
else
    echo "[gvr-hook-build] ERROR: ultra_global_vr_hook_id not exported in $OUT32" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p "$OUT64" | grep -q "ultra_global_vr_hook_id"; then
    echo "[gvr-hook-build] export OK (64): ultra_global_vr_hook_id"
else
    echo "[gvr-hook-build] ERROR: ultra_global_vr_hook_id not exported in $OUT64" >&2
    exit 1
fi

# Drop into the release directory too (both DLLs), mirroring every other
# board's build_hook.sh.
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra Global VR"
if [ -d "$REL_DIR" ]; then
    cp -f "$OUT32" "$REL_DIR/"
    cp -f "$OUT64" "$REL_DIR/"
    echo "[gvr-hook-build] copied both DLLs to $REL_DIR/"
else
    echo "[gvr-hook-build] NOTE: release dir $REL_DIR does not exist -- skipped copy"
fi

echo "[gvr-hook-build] DONE"
