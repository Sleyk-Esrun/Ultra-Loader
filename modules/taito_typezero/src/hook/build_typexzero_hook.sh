#!/usr/bin/env bash
# Build Ultra Loader's Taito-Type-Zero-ONLY hook DLLs.
#   ultra_taito_typezero_hook32.dll -- the injected I/O hook for 32-bit games
#   ultra_taito_typezero_hook64.dll -- the injected I/O hook for 64-bit games
#                                      (defensive only -- the real board is
#                                      32-bit; kept so taito_typezero_main.c's
#                                      existing dual-bitness contract is fully
#                                      satisfied)
#
# Fully independent of modules/native_x86/src/hook/build_hook.sh's shared
# multi-board ultra_typex_hook{32,64}.dll: no shared source file, no shared
# compiled binary, no dependency on modtest/ at build or run time. Mirrors the
# fork already done for Cave PC (modules/cave_pc/src/hook/build_cave_hook.sh).
#
# Sources (all in this folder, this board's own independent copies -- logic
# carried over from native_x86/src/hook/{jvs_core,jvs_serial_hook,
# d3d9_throttle,audio_hook}.c as REFERENCE ONLY per the project owner's
# explicit instruction, every non-static symbol renamed ttz_/TTZ_):
#   jvs_core_typexzero.c        -- clean-room JVS I/O engine
#   jvs_serial_hook_typexzero.c -- JVS-over-serial COM transport + IAT patcher
#   d3d9_throttle_typexzero.c   -- D3D9 present hook (frame limiter, force-
#                                  windowed, optional upscale/rotate/sharpen/
#                                  dump)
#   audio_hook_typexzero.c      -- DirectSound + XAudio2 diagnostic hook
#   hook_dllmain_typexzero.c    -- DLL entry point, JVS node owner, local
#                                  XInput gamepad source
#   omni_bridge.c               -- Ultra Omni-Bridge (5th Omni service): this
#                                  board's own copy of the reference at
#                                  native_x86/src/hook/omni_bridge.c, unchanged
#                                  (its ultra_omni_bridge_install/stop API is
#                                  shared across boards on purpose). Opt-in via
#                                  ULTRA_BRIDGE_ENABLE (default OFF); config
#                                  comes from work/include/omni_client.h, hence
#                                  the -I"$LOADER_INC" added below.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typezero/src/hook && bash build_typexzero_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (the project's established workaround -- see memory: "Ultra Loader build
# env", and native_x86/src/hook/build_hook.sh's own TMP/TEMP/TMPDIR pattern).
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

SRCS="jvs_core_typexzero.c jvs_serial_hook_typexzero.c d3d9_throttle_typexzero.c audio_hook_typexzero.c hook_dllmain_typexzero.c omni_bridge.c pause_menu_wire_typexzero.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c ../../../../../../work/src/ultra_transport.c ../../../../../../work/src/ultra_tracer.c"

# Loader's shared, header-only include dir (work/include/omni_client.h) --
# needed only by omni_bridge.c above; same relative depth (modules/<board>/
# src/hook -> ../../../../../../work/include) as native_x86/src/hook/
# build_hook.sh's own LOADER_INC. Header-only: does not create a compiled/
# linked dependency on any other board or on native_x86.
LOADER_INC="../../../../../../work/include"

# -static -static-libgcc: self-contained, no mingw runtime dependency inside
# the injected game process (same rationale as every other hook DLL in this
# project). No -ld3d9/-ldsound: Direct3DCreate9/DirectSoundCreate(8)/
# XAudio2Create are all resolved dynamically at runtime (LoadLibrary+
# GetProcAddress), not linked statically -- matches native_x86's own hook,
# which does not link those import libs either. omni_bridge.c's XInput calls
# are the same: dynamically loaded (xinput1_4.dll etc. via LoadLibraryA), so
# no -lxinput and no new import-table dependency.
CFLAGS="-std=c11 -O2 -Wall -pipe -static -static-libgcc"
LIBS="-lpsapi -luser32 -lwinmm -lgdi32"

echo "[32] taito_typezero hook dll"
PATH="$M32:$PATH" env $T i686-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -m32 -shared -o ultra_taito_typezero_hook32.dll $SRCS $LIBS

echo "[64] taito_typezero hook dll"
PATH="$M64:$PATH" env $T x86_64-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -m64 -shared -o ultra_taito_typezero_hook64.dll $SRCS $LIBS

ls -la ultra_taito_typezero_hook32.dll ultra_taito_typezero_hook64.dll

# Verify the identity exports.
if /c/msys64/mingw32/bin/objdump.exe -p ultra_taito_typezero_hook32.dll | grep -q "ultra_taito_typezero_hook_id"; then
    echo "[taito_typezero hook] 32-bit export OK: ultra_taito_typezero_hook_id"
else
    echo "[taito_typezero hook] ERROR: 32-bit ultra_taito_typezero_hook_id not exported" >&2
    exit 1
fi
if /c/msys64/mingw64/bin/objdump.exe -p ultra_taito_typezero_hook64.dll | grep -q "ultra_taito_typezero_hook_id"; then
    echo "[taito_typezero hook] 64-bit export OK: ultra_taito_typezero_hook_id"
else
    echo "[taito_typezero hook] ERROR: 64-bit ultra_taito_typezero_hook_id not exported" >&2
    exit 1
fi

# Stage next to this board's own module DLL (taito_typezero_main.c's
# find_hook() searches the loader dir + %LOCALAPPDATA%\Ultra Loader\modules\
# taito_typezero\ -- build_module.sh copies these on from here).
cp -f ultra_taito_typezero_hook32.dll ultra_taito_typezero_hook64.dll "$HERE/../.."

echo "[taito_typezero hook] done"
