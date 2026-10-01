#!/usr/bin/env bash
# =============================================================================
#  build_bemani_hook.sh -- Build ultra_konami_bemani_hook{32,64}.dll from this
#                          board's OWN 5-file hook tree (src/hook/).
# =============================================================================
#
#  Compiles the Konami Bemani PC arcade I/O hook DLL that konami_bemani_main.c
#  (this module's launcher) injects into the game process via
#  QueueUserAPC(LoadLibraryA) (primary) / CreateRemoteThread(LoadLibraryA)
#  (fallback). Produces BOTH bitnesses since Konami Bemani PC hardware spans
#  32-bit (older cabinets) and 64-bit (newer cabinets) Windows builds; the
#  launcher sniffs the game .exe's PE header and picks the matching DLL.
#
#  This board's hook tree is FULLY SELF-CONTAINED: every source file lives
#  under modules/konami_bemani/src/hook/ and was written as this board's OWN
#  independent copy (native_x86/src/hook/*.c was read only as reference). This
#  script does NOT compile, link, or copy anything from native_x86/src/hook/
#  or from any other board module -- no shared runtime binary, per project
#  policy.
#
#  Recipe mirrors native_x86/src/hook/build_hook.sh:
#    -static / -static-libgcc  -- self-contained, no libgcc_s/libwinpthread
#                                 dep once injected into the game process
#    -pipe                     -- avoid MSYS TMP-stripping issue (cc1 would
#                                 otherwise write its temp .s into an
#                                 unwritable C:\WINDOWS under some shells)
#    TMP/TEMP/TMPDIR inline    -- force a writable temp for the toolchain
#
#  Usage:
#    cd modules_pkg/ultra-loader-modules/modules/konami_bemani && bash build_bemani_hook.sh
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin
if [ ! -x "$M32/gcc.exe" ]; then
    echo "[kb-hook-build] ERROR: mingw32 gcc not found at $M32" >&2
    exit 2
fi
if [ ! -x "$M64/gcc.exe" ]; then
    echo "[kb-hook-build] ERROR: mingw64 gcc not found at $M64" >&2
    exit 2
fi

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the loader Makefile's RUNENV pattern) so the build works regardless of shell.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRC_DIR="src/hook"
SRCS="$SRC_DIR/jvs_core_bemani.c \
      $SRC_DIR/konami_io_bemani.c \
      $SRC_DIR/d3d9_throttle_bemani.c \
      $SRC_DIR/dinput8_emu_bemani.c \
      $SRC_DIR/audio_hook_bemani.c \
      $SRC_DIR/omni_bridge_bemani.c \
      $SRC_DIR/pause_menu_wire_bemani.c \
      ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c \
      $SRC_DIR/hook_dllmain_bemani.c ../../../../work/src/ultra_transport.c"

# work/include: this board's own copy of the Ultra Omni-Bridge (omni_bridge_
# bemani.c) reads its tuning from the shared work/include/omni_client.h
# (omni_client_read_bridge()), same header every other Omni-* service's
# per-board config struct lives in. This is the ONLY reason this board's hook
# build needs an -I outside its own src/hook/ tree; nothing here links against
# or copies any other file from work/.
LOADER_INC="../../../../work/include"

# -static / -static-libgcc: the injected DLL runs inside the GAME's process,
# which has no mingw runtime -- must carry its own. -pipe: see banner above.
CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -pipe -static -static-libgcc -I$SRC_DIR -I$LOADER_INC"

# psapi: EnumProcessModules (the IAT walker). user32/gdi32/winmm: general Win32
# surface (window subclass, GetStockObject, timeBeginPeriod). advapi32/ole32/
# dxguid/dinput8: header-level GUID/COM surface used by dinput.h/d3d9.h decls
# (mirrors every other board module's own hook build in this project).
LIBS="-lpsapi -luser32 -lgdi32 -lwinmm -ladvapi32 -lole32 -ldxguid -ldinput8"

echo "[kb-hook-build] [32] ultra_konami_bemani_hook32.dll"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o ultra_konami_bemani_hook32.dll $SRCS $LIBS

echo "[kb-hook-build] [64] ultra_konami_bemani_hook64.dll"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o ultra_konami_bemani_hook64.dll $SRCS $LIBS

echo "[kb-hook-build] OK:"
ls -la ultra_konami_bemani_hook32.dll ultra_konami_bemani_hook64.dll

# Verify the diagnostic export landed in both (each bitness needs its OWN
# objdump -- the 32-bit objdump cannot parse a PE32+ 64-bit image and vice versa).
if "$M32/objdump.exe" -p ultra_konami_bemani_hook32.dll | grep -q "ultra_konami_bemani_hook_id"; then
    echo "[kb-hook-build] export OK: ultra_konami_bemani_hook_id (ultra_konami_bemani_hook32.dll)"
else
    echo "[kb-hook-build] ERROR: ultra_konami_bemani_hook_id not exported in ultra_konami_bemani_hook32.dll" >&2
    exit 1
fi
if "$M64/objdump.exe" -p ultra_konami_bemani_hook64.dll | grep -q "ultra_konami_bemani_hook_id"; then
    echo "[kb-hook-build] export OK: ultra_konami_bemani_hook_id (ultra_konami_bemani_hook64.dll)"
else
    echo "[kb-hook-build] ERROR: ultra_konami_bemani_hook_id not exported in ultra_konami_bemani_hook64.dll" >&2
    exit 1
fi

echo "[kb-hook-build] DONE"
