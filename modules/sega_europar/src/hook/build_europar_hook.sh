#!/usr/bin/env bash
# Build Sega Europa-R's OWN, fully self-contained hook artifacts. Modeled on
# modules/native_x86/src/hook/build_hook.sh's structure/CFLAGS/LDFLAGS/TMP-
# override pattern (read as reference only -- this script does not invoke
# that one, share its object files, or link against native_x86's output).
#
# Produces, in THIS directory (modules/sega_europar/src/hook/):
#   ultra_sega_europar_hook32.dll  -- the INJECTED rendering/JVS/audio/DRM hook
#                                     (HOOK_DLL_NAME in sega_europar_main.c)
#   dinput8.CRK.dll                 -- the FILE-SWAPPED DirectInput8 controls
#                                     proxy (DINPUT8_CRK_NAME in
#                                     sega_europar_main.c), built from the
#                                     existing dinput8_europar.c alone
#
# Both artifacts are 32-bit only: sega_europar_main.c's find_module_file()
# only ever searches for "ultra_sega_europar_hook32.dll" (no "...hook64.dll"
# name appears anywhere in that file), and Europa-R's board profile entry
# (board_profile.c) pins bitness=32 -- this is a WOW64 board, matching Ford
# Racing's own 32-bit PE header. There is no 64-bit variant to produce.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_europar/src/hook && bash build_europar_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
CC="$M32/gcc.exe"
OBJDUMP="$M32/objdump.exe"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (same workaround this module's own build_module.sh and every other Ultra
# Loader mingw build script uses).
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# -static -static-libgcc: the injected hook loads inside the game process,
# which has no mingw runtime -- it must be fully self-contained.
# -pipe: MSYS strips TMP/TEMP from native children so cc1's temp .s would
# otherwise land in C:\WINDOWS (unwritable); -pipe removes that temp file.
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -m32 -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601"

HOOK_SRC="jvs_core_europar.c jvs_serial_hook_europar.c d3d9_throttle_europar.c audio_hook_europar.c sega_keychip_europar.c omni_bridge_europar.c pause_menu_wire_europar.c happio_europar.c gearpad_europar.c panic_europar.c patch_file_europar.c hook_dllmain_europar.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"
HOOK_OUT="ultra_sega_europar_hook32.dll"

# pause_menu_wire_europar.c links the loader's OWN shared pause/settings-menu
# service (work/src/pause_menu_service.c) via a relative path, same convention
# as omni_bridge_europar.c's own omni_client.h include. pause_menu_service.c
# ITSELF uses bare #include "loader.h"/"settings.h" (it's compiled from many
# different boards' build scripts, most of which pass -I work/include) so,
# unlike this board's other files, it needs an explicit -I here too.
PAUSE_SRC="../../../../../../work/src/pause_menu_service.c"
PAUSE_SRC_IO="../../../../../../work/src/pause_menu_settings_io.c"
LOADER_INC="../../../../../../work/include"

echo "[sega_europar] building $HOOK_OUT (own independent hook: JVS-over-serial + D3D9 present/throttle + audio + keychip/inpout32/dialogs + Omni-Bridge + pause menu)"
echo "  sources: $HOOK_SRC $PAUSE_SRC"
PATH="$M32:$PATH" env $T "$CC" $CFLAGS -I"$LOADER_INC" -shared -o "$HOOK_OUT" $HOOK_SRC "$PAUSE_SRC" "$PAUSE_SRC_IO" \
    -lpsapi -luser32 -lgdi32 -lwinmm -lsetupapi -lhid -lxinput9_1_0 \
    -Wl,--out-implib,ultra_sega_europar_hook32.lib

ls -la "$HOOK_OUT"

if "$OBJDUMP" -p "$HOOK_OUT" | grep -qi "DllMain\|DLL_PROCESS_ATTACH" 2>/dev/null; then :; fi
# DllMain isn't exported by name (it's the loader entry, not an export table
# entry) -- verify the DLL is well-formed by confirming it's a valid PE and
# exports at least nothing-required; a link failure above would already have
# aborted (set -e). Confirm it's a real PE32 DLL for i386.
"$OBJDUMP" -f "$HOOK_OUT" | grep -q "file format pei-i386" \
    && echo "[sega_europar] $HOOK_OUT is a valid 32-bit PE DLL" \
    || { echo "[sega_europar] ERROR: $HOOK_OUT is not a valid 32-bit PE DLL" >&2; exit 1; }

# ---------------------------------------------------------------------------
DI8_SRC="dinput8_europar.c"
DI8_OUT="dinput8.CRK.dll"

echo "[sega_europar] building $DI8_OUT (dedicated DirectInput8 controls proxy)"
# --kill-at (2026-08-04 fix, real live bug -- confirmed by crash, not
# theoretical): DirectInput8Create is declared WINAPI (__stdcall). Without
# this flag, MinGW's linker exports it decorated as "DirectInput8Create@20",
# not the plain "DirectInput8Create" the real system dinput8.dll exports and
# that Ford Racing's own GetProcAddress(hDll, "DirectInput8Create") looks up.
# The build's own verification below only ever did a SUBSTRING grep, which
# matches the decorated name too, so this passed silently. The game itself
# never checked the resulting NULL function pointer before calling through
# it -- confirmed via a live crash: Application Error faulting module
# "unknown", offset 0x00000000, the classic call-through-NULL signature, and
# this proxy's own DirectInput8Create() log line never printed (the function
# was never reached at all). konami_pc/build_module.sh's dinput8_crk.c build
# already carries this exact flag for this exact reason; ported here.
PATH="$M32:$PATH" env $T "$CC" $CFLAGS -shared -o "$DI8_OUT" $DI8_SRC \
    -luser32 -lxinput9_1_0 -ldxguid \
    -Wl,--out-implib,dinput8_europar.lib \
    -Wl,--kill-at

ls -la "$DI8_OUT"

if "$OBJDUMP" -p "$DI8_OUT" | grep -qE "[[:space:]]DirectInput8Create$"; then
    echo "[sega_europar] export OK: DirectInput8Create (undecorated)"
else
    echo "[sega_europar] ERROR: DirectInput8Create not exported UNDECORATED from $DI8_OUT" >&2
    echo "[sega_europar]        (a substring match on a decorated name like DirectInput8Create@20" >&2
    echo "[sega_europar]        would wrongly pass here -- the game's GetProcAddress needs the exact," >&2
    echo "[sega_europar]        undecorated name; see the --kill-at comment above this build step)" >&2
    exit 1
fi

echo "[sega_europar] OK:"; ls -la "$HOOK_OUT" "$DI8_OUT"
