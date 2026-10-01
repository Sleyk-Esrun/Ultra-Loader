#!/usr/bin/env bash
# Build the Taito Type X2 module DLL.
#
# Produces: ultra_taito_typex2.dll (in this module folder)
#
# The DLL exports `ul_taito_typex2_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this once it has identified the launch
# target as a Type X2 board (D3D9 + Taito JVS + dongle profile). The shim does
# the spawn+inject itself, in-process: CreateProcess(game.exe,
# CREATE_SUSPENDED), VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
# (LoadLibraryA on the extracted hook), ResumeThread, WaitForSingleObject --
# no ultra-module-host.exe / ultra-inject32.exe involved (stale claim in a
# prior version of this comment; see taito_typex2_main.c for the real flow).
#
# 2026-08-01/02: this board is real, verified-playable (Haunted Museum, full
# credit loop; Wacky Race not yet re-verified against this exact rebuild).
# Initially tried pinning a frozen prebuilt hook binary (see git history of
# this file / prebuilt/ if it's still present) after the from-source build's
# D3D9 device came up with a solid BLACK backbuffer on live playtest -- ROOT
# CAUSE FOUND: the loader Settings dialog defaults Upscaler=FSR, which
# silently forces the D3D hook's upscale/sharpen path, and THAT path is what
# produced the black screen, independent of which hook binary was used (see
# taito_typex2_main.c's ULTRA_TYPEX_UPSCALE comment for the actual fix). Once
# that was disabled by default, the from-source build (src/build_hook.sh,
# compiling this board's own copied source tree plus dinput8_emu.c/
# d3d8_throttle.c/d3d8_sharpen.c/audio_hook.c) renders correctly -- confirmed
# live. The pinned-binary approach was ABANDONED once source access was
# needed anyway: dinput8_emu.c's DirectInput8 shim (only relevant on a
# from-source build) broke this game's raw mouse/keyboard reading, and
# fixing that -- plus adding the Omni-Track lightgun + XInput gamepad bridges
# this board needed -- requires editing typex2_gun.c, which a frozen binary
# can never reflect. This board therefore builds from source like every other
# independent board in this project (no dependency on modtest/ or
# native_x86/src/hook/'s compiled output).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typex2 && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

PROJ="$HERE/../../../.."
SRC_DIR="src"

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601
        "-I$HERE/include" "-I$SRC_DIR"
        "-I$PROJ/work/include" "-I$PROJ/work/src")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ld3d9 -ld3d11 -ldxgi -lshell32)

OUT="ultra_taito_typex2.dll"

mkdir -p /c/msys64/tmp 2>/dev/null || true

# Build this board's OWN independent hook DLL (both bitnesses) from source
# FIRST -- its bytes are needed for the embed step below. No dependency on
# modtest/ or native_x86/src/hook/'s compiled output.
echo "[taito_typex2] building independent hook (src/build_hook.sh)"
bash "$HERE/src/build_hook.sh"

HOOK32_BUILT="$SRC_DIR/ultra_taito_typex2_hook32.dll"
if [ ! -f "$HOOK32_BUILT" ]; then
    echo "[taito_typex2] ERROR: $HOOK32_BUILT was not produced by build_hook.sh" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typex2] embedding hook as RCDATA resource"
( cd "$SRC_DIR" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="$SRC_DIR/hook_res.res.o"

echo "[taito_typex2] building $OUT (Type X2 module shim, 32-bit)"

# Source files: just our thin entry-point stub, plus the embedded hook resource.
SRCS=("$SRC_DIR/taito_typex2_main.c")

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" TMPDIR="C:/msys64/tmp" \
    "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typex2.lib

ls -la "$OUT"

# Verify export
if "$M32/objdump.exe" -p "$OUT" | grep -q "ul_taito_typex2_run"; then
    echo "[taito_typex2] export OK: ul_taito_typex2_run"
else
    echo "[taito_typex2] ERROR: ul_taito_typex2_run not exported" >&2
    exit 1
fi

# Drop artifacts in the project's completed-cores folder for the user to
# install via the loader's "Install Module" UI. No auto-install.
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Type X2"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[taito_typex2] artifact -> $COMPLETED_DIR/$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change (both
# this board's own old standalone-hook names and the even older shared-hook
# name it used to fall back to).
rm -f "$COMPLETED_DIR/ultra_taito_typex2_hook32.dll" "$COMPLETED_DIR/ultra_taito_typex2_hook64.dll" \
      "$COMPLETED_DIR/ultra_typex_hook32.dll" "$COMPLETED_DIR/ultra_typex_hook64.dll"

echo "[taito_typex2] done -- install via loader's Settings -> Install Module when ready"
