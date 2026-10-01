#!/usr/bin/env bash
# =============================================================================
#  build_nu2_hook.sh -- Build ultra_sega_nu2_hook{32,64}.dll from the 10 TUs
#                       under src/hook/.
# =============================================================================
#
#  Compiles the Sega Nu2 hook DLL that sega_nu2_main.c injects into the game
#  process. Dual-build: 32-bit AND 64-bit -- Nu2 titles are 64-bit exclusively,
#  but a 32-bit build is produced too as a belt-and-braces fallback for
#  oddball 32-bit repacks (matches sega_nu2_main.c's pe_bitness() sniff +
#  inject_dll_32/inject_dll_64 split).
#
#  THIS HOOK IS INDEPENDENT: every source file under src/hook/ is a fresh,
#  standalone TU written for the sega_nu2 board module. None of them include
#  or link against anything in modules/sega_nu/ or modules/native_x86/ --
#  see the per-file header comments for the "independence note" on each. The
#  resulting DLL shares NO runtime binary with any other board, per the
#  project's per-board self-containment directive. The one exception is
#  omni_bridge.c's include of the shared, header-only work/include/
#  omni_client.h (env-var config reader for all 5 Omni services, no .c to
#  link) -- every other board's from-source hook that adopts Omni-Bridge
#  includes the same header the same way, per that file's own header comment.
#
#  Output files (dropped in THIS module's root, matching sega_nu/build_hook.sh):
#    1. modules/sega_nu2/ultra_sega_nu2_hook32.dll
#    2. modules/sega_nu2/ultra_sega_nu2_hook64.dll
#    3. Completed Ultra Loader Cores/native/Ultra Nu2/  -- both DLLs (release)
#
#  Follows the same recipe as modules/sega_nu/build_hook.sh /
#  modules/native_x86/src/hook/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force writable temp for the toolchain
#
#  NO auto-install. The user opts in via Settings -> Install Module (or by
#  copying the DLLs to %LOCALAPPDATA%\Ultra Loader\modules\sega_nu2\).
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/sega_nu2 && bash build_nu2_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[sn2-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[sn2-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[sn2-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[sn2-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
SRCS="$SRC_DIR/hook_dllmain_nu2.c \
      $SRC_DIR/sega_keychip_nu2.c \
      $SRC_DIR/amauth_stub_nu2.c \
      $SRC_DIR/allnet_stub_nu2.c \
      $SRC_DIR/truecrypt_bypass_nu2.c \
      $SRC_DIR/amdaemon_stub_nu2.c \
      $SRC_DIR/d3d11_throttle_nu2.c \
      $SRC_DIR/audio_hook_nu2.c \
      $SRC_DIR/sega_io4_nu2.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_nu2.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

# work/include holds the shared, header-only omni_client.h (Omni-Play/Track/
# Wheel/Steer/Bridge env-var config reader) that this board's own omni_bridge.c
# copy includes -- see that file's header comment. No other cross-tree include
# is added; omni_client.h is header-only (no .c to link).
# NOTE: relative, not $HERE-prefixed -- $HERE is an absolute path and this
# project's directory tree has a space in it ("Ultra Loader"), which would
# break word-splitting when CFLAGS is later expanded unquoted below. We've
# already cd'd into $HERE above, so the relative form is equivalent and safe.
WORK_INCLUDE="../../../../work/include"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc -I$SRC_DIR -I$WORK_INCLUDE"

# Libraries: psapi (EnumProcessModules for the IAT walker), user32/advapi32
# for MessageBox/Registry surfaces we don't call but that don't hurt to link,
# ws2_32 (allnet_stub_nu2 HTTP responder + gethostbyname/getaddrinfo hooks),
# gdi32 (loader-splash FindWindowA/ShowWindow in d3d11_throttle_nu2.c),
# winmm (timeBeginPeriod/timeEndPeriod, d3d11_throttle_nu2.c's frame limiter).
# d3d11.dll/dxgi.dll/dsound.dll/xinput*.dll are all resolved dynamically at
# runtime (GetModuleHandleA/LoadLibraryA + GetProcAddress) -- no -ld3d11
# / -ldxgi / -ldxguid / -ldsound / -lxinput* link-time dependency.
LIBS="-lpsapi -lws2_32 -luser32 -ladvapi32 -lgdi32 -lwinmm"

OUT32="ultra_sega_nu2_hook32.dll"
OUT64="ultra_sega_nu2_hook64.dll"

echo "[sn2-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[sn2-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[sn2-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[sn2-hook-build] built: $OUT64 ($sz64 bytes)"

# Drop into the release directory (both DLLs).
REL_DIR="../../../../Completed Ultra Loader Cores/native/Ultra Nu2"
if [ -d "$REL_DIR" ]; then
    cp -f "$OUT32" "$REL_DIR/"
    cp -f "$OUT64" "$REL_DIR/"
    echo "[sn2-hook-build] copied both DLLs to $REL_DIR/"
else
    echo "[sn2-hook-build] NOTE: release dir $REL_DIR does not exist -- skipped copy"
fi

echo "[sn2-hook-build] DONE"
