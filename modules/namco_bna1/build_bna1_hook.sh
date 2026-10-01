#!/usr/bin/env bash

# --- Ultra Loader universal I/O transport layer (shared source) ---
UTX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../../.."
UTX_SRC="$UTX_ROOT/work/src/ultra_transport.c"
UTX_INC="$UTX_ROOT/work/include"
# Build the Namco System BNA1 board-NATIVE hook DLL (both bitnesses).
#
# Produces (in this module's src/hook/ dir, then copied up to the module root):
#   ultra_namco_bna1_hook32.dll
#   ultra_namco_bna1_hook64.dll
#
# This is the DLL namco_bna1_main.c's find_hook() prefers over the shared Type
# X fallback (ultra_typex_hook{32,64}.dll). It is fully SELF-CONTAINED: every
# source file it links lives in modules/namco_bna1/src/hook/ (its own
# independent copy of the JVS engine, IAT patcher, JVS-over-USB + amauthd
# transport, D3D11 present throttle, DirectInput8 emulation, and DirectSound/
# XAudio2 diagnostic hook) -- ZERO link dependency on native_x86's compiled
# output or any other board module.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/namco_bna1 && bash build_bna1_hook.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/src/hook"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (same pattern as build_module.sh / the reference build_hook.sh).
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# -static makes the DLL self-contained (no libgcc_s/libwinpthread dependency),
# so it loads inside the game process which has no mingw runtime of its own.
# -pipe drops the .s temp (see the TMP note above). -fcommon/-fno-strict-aliasing
# match this project's other hook builds (tolerant of the C sources' style).
CFLAGS=(-std=c11 -O2 -Wall -pipe -static -static-libgcc -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601 -I"$UTX_INC")

SRCS=(jvs_core_bna1.c iat_patch_bna1.c namco_io_bna1.c d3d11_throttle_bna1.c
      dinput8_emu_bna1.c audio_hook_bna1.c omni_bridge.c pause_menu_wire_bna1.c ../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c hook_dllmain_bna1.c "$UTX_SRC")

# omni_bridge.c's #include "omni_client.h" resolves against the loader's
# shared work/include/ (the same header-only, no-alloc, env-var-backed config
# struct every other Omni service client uses) -- NOT a copy, NOT a shared
# compiled binary, just the one shared config header. Path modeled on
# modules/native_x86/src/hook/build_hook.sh's LOADER_INC (same tree depth:
# modules/namco_bna1/src/hook -> ../../../../../../work/include).
LOADER_INC="../../../../../../work/include"

# -lpsapi: EnumProcessModules (iat_patch_bna1.c's IAT walk).
# -luser32: window/message + GetAsyncKeyState calls (dinput8_emu/audio_hook/d3d11 splash-dismiss;
#           also SendInput/SetCursorPos/MapVirtualKeyA for omni_bridge.c).
# -lwinmm: timeBeginPeriod/timeEndPeriod (frame limiter).
# -lgdi32: pulled in transitively by some mingw d3d11.h/dxgi.h inline helpers.
# xinput1_4.dll/xinput1_3.dll/xinput9_1_0.dll (omni_bridge.c's gamepad thread) are
# resolved dynamically at runtime via LoadLibraryA/GetProcAddress -- no -lxinput* dep.
LIBS=(-lpsapi -luser32 -lwinmm -lgdi32)

build_one() {
  local bits="$1"
  local cc="$2"
  local mpath="$3"
  local out="ultra_namco_bna1_hook${bits}.dll"
  echo "[namco_bna1] building $out ($bits-bit board-native hook)"
  PATH="$mpath:$PATH" env $T "$cc" "${CFLAGS[@]}" -I. -I"$LOADER_INC" -shared -o "$out" "${SRCS[@]}" \
      -Wl,--out-implib,"ultra_namco_bna1_hook${bits}.lib" "${LIBS[@]}"
  # Verify DllMain + the id export landed.
  if "$mpath/objdump.exe" -p "$out" | grep -q "ultra_namco_bna1_hook_id"; then
      echo "[namco_bna1] export OK: ultra_namco_bna1_hook_id ($out)"
  else
      echo "[namco_bna1] ERROR: ultra_namco_bna1_hook_id not exported in $out" >&2
      exit 1
  fi
}

build_one 32 i686-w64-mingw32-gcc   "$M32"
build_one 64 x86_64-w64-mingw32-gcc "$M64"

# Stage a copy at the module root (beside namco_bna1_main.c's build output) and
# auto-install to %LOCALAPPDATA%\Ultra Loader\modules\namco_bna1\ -- the FIRST
# place namco_bna1_main.c's find_hook_named() looks, same convention as
# build_module.sh uses for the spawner DLL.
cp -f ultra_namco_bna1_hook32.dll ultra_namco_bna1_hook64.dll "$HERE/"

INSTALL_DIR="$USERPROFILE/AppData/Local/Ultra Loader/modules/namco_bna1"
INSTALL_DIR="$(cygpath -u "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")"
mkdir -p "$INSTALL_DIR"
cp -f ultra_namco_bna1_hook32.dll ultra_namco_bna1_hook64.dll "$INSTALL_DIR/"
echo "[namco_bna1] installed hooks -> $INSTALL_DIR/"

echo "[namco_bna1] OK:"
ls -la "$HERE/src/hook/ultra_namco_bna1_hook32.dll" "$HERE/src/hook/ultra_namco_bna1_hook64.dll" \
       "$HERE/ultra_namco_bna1_hook32.dll" "$HERE/ultra_namco_bna1_hook64.dll"
