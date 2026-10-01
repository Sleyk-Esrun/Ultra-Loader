#!/usr/bin/env bash
# =============================================================================
#  build_unico_hook.sh -- Build ultra_unico_pc_hook{32,64}.dll from this
#                         board's OWN hook sources (modules/unico_pc/src/hook/).
# =============================================================================
#
#  Every source compiled here lives ONLY in modules/unico_pc/src/hook/. This
#  DLL does NOT link against native_x86's compiled hook DLL/import lib, or any
#  other board's module output -- native_x86 was read only as reference for
#  public Win32/JVS/DirectX/DirectInput/DirectSound API shapes, and every line
#  was independently (re-)written into this board's own file set, per the
#  project directive that every board get a fully independent, self-contained
#  hook DLL.
#
#  Subsystems armed (per UNICO's board profile: BP_GFX_MIXED, BP_IO_JVS_SERIAL
#  | BP_IO_ANALOG_WHEEL, BP_PROT_USB_DONGLE, BP_HOOK_D3D9_AND_D3D11):
#    - clean-room JVS input struct + wire codec       (jvs_core_unico.c)
#    - JVS-over-serial COM hook + kernel32 IAT patcher
#      + light USB-dongle-device bypass (env-gated)    (jvs_serial_hook_unico.c)
#    - D3D9 present/throttle hook (safe default for a
#      mixed-graphics board; D3D11 sibling deferred
#      until a real dump exists to verify against)      (d3d9_throttle_unico.c)
#    - synthetic DirectInput8 device                    (dinput8_emu_unico.c)
#    - DirectSound + XAudio2 diagnostic hook             (audio_hook_unico.c)
#    - DllMain + input source + dialog auto-dismiss      (hook_dllmain_unico.c)
#    - Ultra Omni-Bridge: gamepad/lightgun -> synthesized
#      cursor+keys for raw-OS-input titles (opt-in,
#      ULTRA_BRIDGE_ENABLE defaults off)                 (omni_bridge.c)
#
#  Dual-build (32-bit AND 64-bit): the board profile's default bitness is 64
#  (Win7 Embedded), but unico_pc_main.c sniffs the game's own PE bitness and
#  looks for whichever hook matches, so we build both -- exactly like every
#  other per-board build in this project.
#
#  Output files:
#    1. modules/unico_pc/ultra_unico_pc_hook32.dll
#    2. modules/unico_pc/ultra_unico_pc_hook64.dll
#    3. Completed Ultra Loader Cores/native/Ultra UNICO PC/  -- BOTH DLLs
#
#  -static / -static-libgcc: the DLL must be self-contained (no libgcc_s /
#  libwinpthread dependency) since it loads inside the game process, which has
#  no mingw runtime of its own.
#  -pipe + inline TMP/TEMP/TMPDIR: MSYS strips TMP/TEMP from native children,
#  so cc1/as fall back to C:\WINDOWS (unwritable) without this.
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/unico_pc && bash build_unico_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[unico-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[unico-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[unico-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[unico-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
# work/include -- shared, header-only Omni-service config structs
# (omni_client.h / omni_client_read_bridge()) consumed by omni_bridge.c.
LOADER_INC="../../../../work/include"
SRCS="$SRC_DIR/hook_dllmain_unico.c \
      $SRC_DIR/jvs_core_unico.c \
      $SRC_DIR/jvs_serial_hook_unico.c \
      $SRC_DIR/d3d9_throttle_unico.c \
      $SRC_DIR/dinput8_emu_unico.c \
      $SRC_DIR/audio_hook_unico.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_unico.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# psapi (EnumProcessModules for the IAT walker), user32 (window class/style
# calls, GetAsyncKeyState, FindWindow/ShowWindow), gdi32 (GetStockObject for
# the present-hook's black background brush), winmm (timeBeginPeriod for the
# D3D9 frame limiter). No -ld3d9/-ldinput8/-ldsound/-ldxguid needed -- every
# COM interface is reached via manual vtable dispatch (COBJMACROS) against
# pointers resolved at runtime through GetProcAddress, never a static import.
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm"

OUT32="ultra_unico_pc_hook32.dll"
OUT64="ultra_unico_pc_hook64.dll"

echo "[unico-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -m32 -shared -o "$OUT32" $SRCS $LIBS \
    -Wl,--out-implib,ultra_unico_pc_hook32.lib
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[unico-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[unico-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -m64 -shared -o "$OUT64" $SRCS $LIBS \
    -Wl,--out-implib,ultra_unico_pc_hook64.lib
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[unico-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify each DLL exports the id string (proves this board's hook, not some
# stale/other artifact, actually built and linked).
for OUT in "$OUT32" "$OUT64"; do
    BIT=32; OD="$M32/objdump.exe"
    if [ "$OUT" = "$OUT64" ]; then BIT=64; OD="$M64/objdump.exe"; fi
    if "$OD" -p "$OUT" | grep -q "ultra_unico_pc_hook_id"; then
        echo "[unico-hook-build] export OK ($BIT-bit): ultra_unico_pc_hook_id"
    else
        echo "[unico-hook-build] ERROR ($BIT-bit): ultra_unico_pc_hook_id not exported" >&2
        exit 1
    fi
done

# Drop into the release directory (both DLLs), matching every other board's
# build_hook.sh convention.
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra UNICO PC"
if [ -d "$(dirname "$REL_DIR")" ]; then
    mkdir -p "$REL_DIR"
    cp -f "$OUT32" "$REL_DIR/"
    cp -f "$OUT64" "$REL_DIR/"
    echo "[unico-hook-build] copied both DLLs to $REL_DIR/"
else
    echo "[unico-hook-build] NOTE: release parent dir does not exist -- skipped copy"
fi

echo "[unico-hook-build] DONE"
