#!/usr/bin/env bash
# =============================================================================
#  build_hook.sh -- Build ultra_sega_nu_hook{32,64}.dll from the 5 stub TUs.
# =============================================================================
#
#  Compiles the Sega Nu hook DLL that sega_nu_main.c injects into the game
#  process. Dual-build: 32-bit AND 64-bit -- the Sega Nu / ALLS family spans
#  both bitnesses (Nu = 64-bit; earlier Nu-adjacent titles + amdaemon utility
#  binaries are 32-bit). The spawner picks the matching DLL from the game's
#  PE bitness at CreateProcess time.
#
#  Output files:
#    1. modules/sega_nu/ultra_sega_nu_hook32.dll   (module dir, packaged)
#    2. modules/sega_nu/ultra_sega_nu_hook64.dll
#    3. Completed Ultra Loader Cores/native/Ultra Nu/  -- BOTH DLLs (release)
#
#  Follows the same recipe as modules/rawthrills_pc/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  omni_bridge.c -- Ultra Omni-Bridge (KBM emulation from gamepad/lightgun
#                    for raw-input-only game dumps, the 5th Omni service).
#                    Reference: modules/native_x86/src/hook/omni_bridge.c,
#                    opt-in via ULTRA_BRIDGE_ENABLE (default OFF).
#
#  NO auto-install. The user opts in via Settings -> Install Module (or by
#  copying the DLLs to %LOCALAPPDATA%\Ultra Loader\modules\sega_nu\).
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[sn-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[sn-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[sn-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[sn-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"

# work/include -- shared, header-only Omni-client config (omni_client.h);
# read by omni_bridge.c only. Same relative-path recipe as every other
# module's own build script that needs it (see native_x86/src/hook/
# build_hook.sh's LOADER_INC).
LOADER_INC="../../../../work/include"

SRCS="$SRC_DIR/sega_nu_hook_main.c \
      $SRC_DIR/keychip_stub.c \
      $SRC_DIR/amauth_stub.c \
      $SRC_DIR/allnet_stub.c \
      $SRC_DIR/truecrypt_bypass.c \
      $SRC_DIR/amdaemon_stub.c \
      $SRC_DIR/d3d11_throttle_nu.c \
      $SRC_DIR/audio_hook_nu.c \
      $SRC_DIR/sega_io4_nu.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_nu.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32/advapi32
# for MessageBox/Registry surfaces we don't call but that don't hurt to link,
# ws2_32 (allnet_stub HTTP responder + gethostbyname/getaddrinfo hooks),
# gdi32 (loader-splash FindWindowA/ShowWindow in d3d11_throttle_nu.c),
# winmm (timeBeginPeriod/timeEndPeriod, d3d11_throttle_nu.c's frame limiter).
# d3d11.dll/dxgi.dll/dsound.dll/xinput*.dll are all resolved dynamically at
# runtime (GetModuleHandleA/LoadLibraryA + GetProcAddress) -- no -ld3d11
# / -ldxgi / -ldxguid / -ldsound / -lxinput* link-time dependency.
LIBS="-lpsapi -lws2_32 -luser32 -ladvapi32 -lgdi32 -lwinmm"

OUT32="ultra_sega_nu_hook32.dll"
OUT64="ultra_sega_nu_hook64.dll"

echo "[sn-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[sn-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[sn-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[sn-hook-build] built: $OUT64 ($sz64 bytes)"

# Drop into the release directory (both DLLs).
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra Nu"
if [ -d "$REL_DIR" ]; then
    cp "$OUT32" "$REL_DIR/"
    cp "$OUT64" "$REL_DIR/"
    echo "[sn-hook-build] copied both DLLs to $REL_DIR/"
else
    echo "[sn-hook-build] NOTE: release dir $REL_DIR does not exist -- skipped copy"
fi

echo "[sn-hook-build] DONE"
