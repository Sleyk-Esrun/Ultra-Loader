#!/usr/bin/env bash
# =============================================================================
#  build_allsux_hook.sh -- Build ultra_sega_alls_ux_hook{32,64}.dll from this
#                          board's OWN, fully independent src/hook/ tree.
# =============================================================================
#
#  Per the project owner's directive: every modern arcade board gets its own
#  self-contained hook DLL. This board (Sega ALLS UX -- same ALLS family as
#  sega_alls, a SEPARATE board handled elsewhere) shares NO runtime binary
#  with sega_alls or any other board, and has NO dependency on
#  modules/native_x86/src/hook's compiled output. Reference logic in that
#  folder was READ ONLY (JVS engine, Sega IO4-USB, keychip presence/DEFER
#  framing, D3D11 present hook, DirectInput8 emulation, audio diagnostic) and
#  reimplemented from scratch into this module's own tree.
#
#  Sources (src/hook/, all new, all this board's own):
#    hook_dllmain_allsux.c   -- DllMain, log/env helpers, IAT patcher, XInput
#                               cabinet-input poll thread
#    jvs_core_allsux.c        -- clean-room JVS input struct + wire engine
#    sega_io4_allsux.c        -- Sega IO4-USB (837-15257) cabinet I/O emulation
#    sega_keychip_allsux.c    -- USB N2 keychip presence shim; ALKey (cmd 30-33)
#                               + GkeyEnc/Dec-class commands stay DEFERRED
#    d3d11_throttle_allsux.c  -- DXGI IDXGISwapChain::Present frame limiter +
#                               capture + force-windowed + device-window getter
#    dinput8_emu_allsux.c     -- synthetic DirectInput8 device
#    audio_hook_allsux.c      -- DirectSound/XAudio2 diagnostic hook
#    omni_bridge.c            -- Ultra Omni-Bridge (5th Omni service): raw
#                               keyboard/mouse (SendInput) synthesis from a
#                               gamepad, for a game that reads OS input
#                               directly. Ported unchanged from the reference
#                               at native_x86/src/hook/omni_bridge.c per the
#                               per-board independence rule; config comes from
#                               work/include/omni_client.h (opt-in, off by
#                               default -- see hook_dllmain_allsux.c).
#
#  Output files:
#    1. modules/sega_alls_ux/ultra_sega_alls_ux_hook32.dll
#    2. modules/sega_alls_ux/ultra_sega_alls_ux_hook64.dll
#    3. %LOCALAPPDATA%\Ultra Loader\modules\sega_alls_ux\  -- BOTH DLLs, the
#       single supported install location sega_alls_ux_main.c searches first
#       (matches this module's existing build_module.sh convention).
#
#  Follows the same proven recipe as modules/sega_nu/build_hook.sh and
#  modules/rawthrills_pc/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#    -pipe                     -- avoid the MSYS TMP-stripping issue
#    TMP/TEMP/TMPDIR inline    -- force a writable temp for the toolchain
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/sega_alls_ux && bash build_allsux_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[allsux-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[allsux-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[allsux-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    echo "[allsux-hook-build]        install msys2 + mingw-w64-x86_64-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
# work/include -- omni_client.h + shared Omni-service config structs (header-
# only, no-alloc, env-var-backed). Same relative depth as vita/build_module.sh
# and every other in-tree consumer of this header (modules/<board> -> ../../../../work/include).
# NOTE: kept RELATIVE (not $HERE-prefixed) on purpose -- this project's path
# ("I:/Ultra Loader/...") contains a space, and CFLAGS is expanded unquoted
# below ($CFLAGS), so an absolute path here would word-split into two bogus
# -I args. We've already cd'd into $HERE, so the relative form is safe.
WORK_INC="../../../../work/include"
SRCS="$SRC_DIR/hook_dllmain_allsux.c \
      $SRC_DIR/jvs_core_allsux.c \
      $SRC_DIR/sega_io4_allsux.c \
      $SRC_DIR/sega_keychip_allsux.c \
      $SRC_DIR/d3d11_throttle_allsux.c \
      $SRC_DIR/dinput8_emu_allsux.c \
      $SRC_DIR/audio_hook_allsux.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_allsux.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wno-unused-variable -pipe -static -static-libgcc \
        -D_WIN32_WINNT=0x0601 -I$SRC_DIR -I$WORK_INC"

# psapi: EnumProcessModules (the IAT walker). user32: window/input-state calls
# (FindWindowA/ShowWindow/CreateWindowExA/RegisterClassA/GetAsyncKeyState).
# winmm: timeBeginPeriod/timeEndPeriod (the D3D11 frame limiter). Nothing else
# is linked: D3D11/DXGI/DirectInput8/DirectSound/XAudio2 real entry points are
# all resolved at runtime via GetModuleHandle/LoadLibrary+GetProcAddress (see
# each TU), so this hook carries no import-lib dependency on any of them.
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="ultra_sega_alls_ux_hook32.dll"
OUT64="ultra_sega_alls_ux_hook64.dll"

echo "[allsux-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -m32 -shared -o "$OUT32" $SRCS $LIBS
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[allsux-hook-build] built: $OUT32 ($sz32 bytes)"

echo "[allsux-hook-build] compiling $OUT64 (64-bit)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -m64 -shared -o "$OUT64" $SRCS $LIBS
sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
echo "[allsux-hook-build] built: $OUT64 ($sz64 bytes)"

# Verify both export the DllMain id string (confirms this is a real, linked
# hook DLL and not an empty/broken shared object).
for out in "$OUT32" "$OUT64"; do
    tool="$M32/objdump.exe"; [ "$out" = "$OUT64" ] && tool="$M64/objdump.exe"
    if "$tool" -p "$out" | grep -qi "ultra_sega_alls_ux_hook_id"; then
        echo "[allsux-hook-build] export OK ($out): ultra_sega_alls_ux_hook_id"
    else
        echo "[allsux-hook-build] ERROR: ultra_sega_alls_ux_hook_id not exported in $out" >&2
        exit 1
    fi
done

# Auto-install to the SINGLE supported install location sega_alls_ux_main.c's
# find_hook_named() searches FIRST (matches this module's existing
# build_module.sh convention for the spawner DLL).
INSTALL_DIR="$USERPROFILE/AppData/Local/Ultra Loader/modules/sega_alls_ux"
INSTALL_DIR="$(cygpath -u "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")"
mkdir -p "$INSTALL_DIR"
cp -f "$OUT32" "$INSTALL_DIR/$OUT32"
cp -f "$OUT64" "$INSTALL_DIR/$OUT64"
echo "[allsux-hook-build] installed -> $INSTALL_DIR/{$OUT32,$OUT64}"

echo "[allsux-hook-build] DONE"
