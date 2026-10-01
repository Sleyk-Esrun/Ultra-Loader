#!/usr/bin/env bash
# =============================================================================
#  build_es2plus_hook.sh -- Build ultra_namco_es2plus_hook{32,64}.dll from this
#                           board's OWN independent src/hook/ tree.
# =============================================================================
#
#  Namco System ES2+ (Windows-Embedded variant). D3D9 present hook + JVS-over-USB
#  (ESIF) I/O + amauthd LOCAL-presence-only handshake + clean-room DirectInput8
#  emulation + DirectSound/XAudio2 diagnostic hook.
#
#  This is a FULLY SELF-CONTAINED build: every source file lives under
#  src/hook/ in THIS module folder. It does NOT compile, include, or link
#  anything from modules/native_x86/src/hook/ or any other board's module --
#  no shared runtime binary with any other board (including namco_es1, a
#  separate board handled elsewhere).
#
#  Dual-build: 32-bit AND 64-bit, mirroring modules/sega_nu/build_hook.sh's
#  proven recipe:
#    -static / -static-libgcc  -- self-contained, no libgcc_s dep once injected
#                                 into a game process that ships no mingw runtime
#    -pipe                     -- avoid the MSYS TMP-stripping issue (cc1 would
#                                 otherwise try to write its temp .s into an
#                                 unwritable C:\WINDOWS)
#    TMP/TEMP/TMPDIR inline    -- force a writable temp for the toolchain
#
#  Output files (this module folder):
#    ultra_namco_es2plus_hook32.dll
#    ultra_namco_es2plus_hook64.dll
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/namco_es2plus && bash build_es2plus_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[es2p-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    echo "[es2p-hook-build]        install msys2 + mingw-w64-i686-toolchain" >&2
    exit 2
fi

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
SRCS="$SRC_DIR/hook_dllmain_es2plus.c \
      $SRC_DIR/jvs_core_es2plus.c \
      $SRC_DIR/namco_io_es2plus.c \
      $SRC_DIR/d3d9_throttle_es2plus.c \
      $SRC_DIR/dinput8_emu_es2plus.c \
      $SRC_DIR/audio_hook_es2plus.c \
      $SRC_DIR/omni_bridge.c \
      $SRC_DIR/pause_menu_wire_es2plus.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c ../../../../work/src/ultra_transport.c"

# LOADER_INC: this board's own copy of omni_bridge.c (ported from
# native_x86/src/hook/omni_bridge.c per the per-board independence rule --
# see that file's header) reads its tuning from the shared, header-only
# work/include/omni_client.h. omni_bridge.{c,h} themselves are unmodified;
# only this -I is board-specific (namco_es2plus/build_es2plus_hook.sh lives at
# the board root, one level shallower than native_x86/src/hook/build_hook.sh,
# hence 4 "../" here vs that file's 6).
LOADER_INC="../../../../work/include"

CFLAGS="-std=c11 -O2 -w -pipe -static -static-libgcc -fcommon -fno-strict-aliasing \
        -D_WIN32_WINNT=0x0601 -I$SRC_DIR -I$LOADER_INC"

# psapi: EnumProcessModules (this board's own IAT walker). user32/gdi32/winmm:
# window subclassing, black-brush, timeBeginPeriod/timeEndPeriod (D3D9 frame
# limiter). No -ld3d9/-ldinput8/-ldsound import libs needed: every D3D9/
# DirectInput8/DirectSound entry point this hook touches is resolved at
# runtime via GetModuleHandleA/LoadLibraryA + GetProcAddress (or reached
# purely through COM vtable pointers), never linked statically.
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm"

OUT32="ultra_namco_es2plus_hook32.dll"
OUT64="ultra_namco_es2plus_hook64.dll"

echo "[es2p-hook-build] compiling $OUT32 (32-bit)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o "$OUT32" $SRCS $LIBS \
    -Wl,--out-implib,"${OUT32%.dll}.lib"
sz32=$(stat -c%s "$OUT32" 2>/dev/null || stat -f%z "$OUT32" 2>/dev/null || echo "?")
echo "[es2p-hook-build] built: $OUT32 ($sz32 bytes)"

if [ -x "$M64/gcc.exe" ]; then
    echo "[es2p-hook-build] compiling $OUT64 (64-bit)"
    PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o "$OUT64" $SRCS $LIBS \
        -Wl,--out-implib,"${OUT64%.dll}.lib"
    sz64=$(stat -c%s "$OUT64" 2>/dev/null || stat -f%z "$OUT64" 2>/dev/null || echo "?")
    echo "[es2p-hook-build] built: $OUT64 ($sz64 bytes)"
else
    echo "[es2p-hook-build] WARN: mingw64 gcc not found at $M64 -- skipping 64-bit hook" >&2
fi

# Verify exports (both DLLs, when built) -- each bitness needs its OWN objdump;
# mingw32's objdump cannot parse a PE32+ (64-bit) image and vice versa.
verify_export() {
    local out="$1" objdump="$2"
    [ -f "$out" ] || return 0
    [ -x "$objdump" ] || { echo "[es2p-hook-build] WARN: objdump not found for $out -- skipping export check" >&2; return 0; }
    if "$objdump" -p "$out" | grep -q "ultra_namco_es2plus_hook_id"; then
        echo "[es2p-hook-build] export OK: ultra_namco_es2plus_hook_id ($out)"
    else
        echo "[es2p-hook-build] ERROR: ultra_namco_es2plus_hook_id not exported ($out)" >&2
        exit 1
    fi
}
verify_export "$OUT32" "$M32/objdump.exe"
verify_export "$OUT64" "$M64/objdump.exe"

echo "[es2p-hook-build] OK:"; ls -la "$OUT32" "$OUT64" 2>/dev/null || ls -la "$OUT32"
echo "[es2p-hook-build] DONE"
