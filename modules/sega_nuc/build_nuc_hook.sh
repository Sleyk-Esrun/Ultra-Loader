#!/usr/bin/env bash
# Build the Sega Nu Compact board's OWN, fully self-contained injected hook DLL.
#   ultra_sega_nuc_hook32.dll / ultra_sega_nuc_hook64.dll
#
# This hook is injected (by sega_nuc_main.c) into a suspended Sega Nu Compact
# game process. It implements, entirely within src/hook/ of THIS board's
# folder: the JVS engine (jvs_core_nuc.c), IO4-USB I/O (sega_io4_nuc.c), the
# presence-only Sega keychip shim (sega_keychip_nuc.c), the D3D11 present hook
# (d3d11_throttle_nuc.c), a DirectSound/XAudio2 diagnostic audio hook
# (audio_hook_nuc.c), and its own DllMain + IAT patcher (hook_dllmain_nuc.c).
#
# NO shared runtime binary with any other board, NO dependency on
# native_x86/src/hook's compiled output, NOT dependent on sega_nu / sega_nu2
# (separate boards handled elsewhere). Modeled on native_x86/src/hook/
# build_hook.sh's CFLAGS/LDFLAGS/TMP pattern (read for REFERENCE ONLY).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_nuc && bash build_nuc_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children so cc1/as fall back to C:\WINDOWS
# (unwritable). Give every gcc invocation a writable temp inline (the loader
# Makefile's RUNENV pattern) so the build works regardless of the shell.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# -static -static-libgcc: fully self-contained (no libgcc_s/libwinpthread
# dependency), so the injected hook loads inside the game process, which has
# no mingw runtime of its own. -pipe: drops the .s temp file entirely (belt+
# suspenders alongside the TMP override above).
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -D_WIN32_WINNT=0x0601"

HOOK_SRC="src/hook/jvs_core_nuc.c src/hook/sega_io4_nuc.c src/hook/sega_keychip_nuc.c src/hook/d3d11_throttle_nuc.c src/hook/audio_hook_nuc.c src/hook/omni_bridge.c src/hook/pause_menu_wire_nuc.c ../../../../work/src/pause_menu_service.c ../../../../work/src/pause_menu_settings_io.c src/hook/hook_dllmain_nuc.c ../../../../work/src/ultra_transport.c"

# omni_bridge.c/.h are this board's OWN copy of the Ultra Omni-Bridge (5th
# Omni service) reference implementation at native_x86/src/hook/omni_bridge.c
# -- unchanged except living in this board's own src/hook/ (per this
# project's per-board independence rule, no board links a shared compiled
# copy). It only needs the shared, header-only work/include/omni_client.h for
# its config struct, so this is the only new -I this build needs.
LOADER_INC="../../../../work/include"

# -lpsapi: EnumProcessModules (the IAT patcher walks every loaded module).
# -luser32: window/keyboard calls (GetAsyncKeyState, FindWindowA, ShowWindow,
#   RegisterClassA/CreateWindowExA/DefWindowProcA for the audio fallback hwnd).
# -lwinmm: timeBeginPeriod/timeEndPeriod (D3D11 frame limiter). No -ld3d11 /
#   -ldxgi / -ldxguid / -ldsound / -lxinput* needed: every one of those APIs
#   is resolved dynamically (LoadLibrary+GetProcAddress) at runtime, and the
#   three D3D11/DXGI GUIDs this DLL needs are locally DEFINE_GUID'd (see
#   d3d11_throttle_nuc.c), so nothing here links against an import lib for
#   any of them.
LDFLAGS="-lpsapi -luser32 -lwinmm -lgdi32"

OUT32="ultra_sega_nuc_hook32.dll"
OUT64="ultra_sega_nuc_hook64.dll"

echo "[sega_nuc] building $OUT32 (32-bit hook)"
PATH="$M32:$PATH" env $T gcc $CFLAGS -Isrc/hook -I"$LOADER_INC" -shared -o "$OUT32" $HOOK_SRC $LDFLAGS \
    -Wl,--out-implib,ultra_sega_nuc_hook32.lib

echo "[sega_nuc] building $OUT64 (64-bit hook)"
PATH="$M64:$PATH" env $T gcc $CFLAGS -Isrc/hook -I"$LOADER_INC" -shared -o "$OUT64" $HOOK_SRC $LDFLAGS \
    -Wl,--out-implib,ultra_sega_nuc_hook64.lib

echo "[sega_nuc] verifying exports"
for OUT in "$OUT32" "$OUT64"; do
    BITS=32; OBJDUMP=/c/msys64/mingw32/bin/objdump.exe
    if [ "$OUT" = "$OUT64" ]; then BITS=64; OBJDUMP=/c/msys64/mingw64/bin/objdump.exe; fi
    if "$OBJDUMP" -p "$OUT" | grep -q "DllMain\|EntryPoint"; then
        echo "[sega_nuc] $OUT ($BITS-bit): looks like a valid PE DLL"
    fi
done

ls -la "$OUT32" "$OUT64"
echo "[sega_nuc] done"
