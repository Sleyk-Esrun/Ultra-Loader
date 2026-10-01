#!/usr/bin/env bash
# Build the Namco System ES4 board's OWN fully independent, self-contained I/O
# hook DLL (both bitnesses):
#   ultra_namco_es4_hook32.dll / ultra_namco_es4_hook64.dll
#
# This DLL links ONLY the sources in this folder. It does NOT link against, or
# depend on the compiled output of, modules/native_x86/src/hook/ or any other
# board's module folder -- per the project directive, every modern arcade
# board gets its own hook DLL with no shared runtime binary. The native_x86
# tree was read as REFERENCE ONLY; its logic was ported into the *_es4.c files
# in this folder as this board's own independent copy.
#
# Model: CFLAGS/LDFLAGS/TMP pattern from
# modules/native_x86/src/hook/build_hook.sh (same -static self-contained DLL
# story: the injected hook loads inside the game process, which has no mingw
# runtime, so -static avoids a libgcc_s/libwinpthread dependency).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# -pipe drops the .s temp; some shells (MSYS) strip TMP/TEMP from native
# children so `as` falls back to C:\WINDOWS (unwritable). Set a writable temp
# INLINE on every gcc invocation (the loader Makefile's RUNENV pattern) so the
# build works regardless of the shell.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# This board's own hook sources ONLY -- no native_x86 file is compiled here.
# omni_bridge.c is this board's own copy of the Ultra Omni-Bridge reference
# (modules/native_x86/src/hook/omni_bridge.c, read-only reference) -- ported
# unchanged except its omni_client.h #include path (see the file's banner).
HOOK_SRC="jvs_core_es4.c namco_io_es4.c dinput8_emu_es4.c d3d11_throttle_es4.c audio_hook_es4.c omni_bridge.c pause_menu_wire_es4.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_es4.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# -static -static-libgcc: self-contained (no libgcc_s/libwinpthread dependency)
# so the injected hook loads inside the game process, which has no mingw
# runtime. -lpsapi: EnumProcessModules (the IAT patcher walks all loaded
# modules). -luser32: window/keyboard calls (dinput8 emu, d3d11 splash-dismiss,
# audio fallback window, XInput fallback). -lwinmm: timeBeginPeriod/
# timeEndPeriod (the D3D11 frame limiter). No -ld3d11/-ldxgi/-ldinput8/
# -ldxguid/-ldsound: every one of those APIs is resolved dynamically at
# runtime via GetProcAddress/LoadLibraryA (see d3d11_throttle_es4.c,
# dinput8_emu_es4.c, audio_hook_es4.c) -- this hook has zero static import
# dependency on any of those system DLLs, so it loads cleanly even in a
# process that never links them itself.
LOADER_INC="../../../../../../work/include"
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc -I. -I$LOADER_INC"
LDFLAGS="-lpsapi -luser32 -lwinmm -lgdi32"

echo "[namco_es4] [32] hook dll"
PATH="$M32:$PATH" env $T gcc $CFLAGS -shared -o ultra_namco_es4_hook32.dll $HOOK_SRC $LDFLAGS \
    -Wl,--out-implib,ultra_namco_es4_hook32.lib

echo "[namco_es4] [64] hook dll"
PATH="$M64:$PATH" env $T gcc $CFLAGS -shared -o ultra_namco_es4_hook64.dll $HOOK_SRC $LDFLAGS \
    -Wl,--out-implib,ultra_namco_es4_hook64.lib

echo "[namco_es4] verifying exports"
for f in ultra_namco_es4_hook32.dll ultra_namco_es4_hook64.dll; do
    bits=32; OD="$M32/objdump.exe"
    if [[ "$f" == *64.dll ]]; then bits=64; OD="$M64/objdump.exe"; fi
    if "$OD" -p "$f" | grep -q "ultra_namco_es4_hook_id"; then
        echo "[namco_es4] [$bits] export OK: ultra_namco_es4_hook_id"
    else
        echo "[namco_es4] [$bits] ERROR: ultra_namco_es4_hook_id not exported" >&2
        exit 1
    fi
done

echo "OK:"; ls -la ultra_namco_es4_hook32.dll ultra_namco_es4_hook64.dll
