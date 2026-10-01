#!/usr/bin/env bash
# Build the Raw Thrills PC per-board module DLL.
#
# Produces: ultra_rawthrills_pc.dll (in this module folder)
#
# The DLL exports `ul_rawthrills_pc_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a Raw Thrills PC
# arcade .exe game (Fast & Furious Super Bikes, Big Buck Hunter, etc.) and
# resolving its board to "rawthrills_pc" via the unified board profile.
#
# This module does the spawn+inject in-process: CreateProcess(game.exe,
# CREATE_SUSPENDED), VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
# (LoadLibraryA, ultra_rawthrills_hook32.dll), ResumeThread,
# WaitForSingleObject. The hook DLL (ultra_rawthrills_hook32.dll) provides
# D3D8/D3D9/D3D11 present interception, DirectInput8 emu, JammaUSB transport
# stub, USBWD watchdog stub, PortTalk.SYS bypass, EWF stub, and lightgun
# bridge -- it's staged alongside this module DLL.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/rawthrills_pc && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline.
mkdir -p /c/msys64/tmp 2>/dev/null || true

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
        -D_WIN32_WINNT=0x0601
        "-I$HERE/include")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ldxgi -lshell32)

# Build this board's OWN fully independent, self-contained hook fork FIRST
# (9 TUs: jammausb/usbwd/porttalk/ewf/rawart + d3d8/d3d9/d3d11 present hooks +
# DirectSound/XAudio2 diagnostic hook -- see build_hook.sh), so its bytes
# exist to embed. This step was previously NOT called by this script at all
# (only checked whether a prior manual `bash build_hook.sh` run had left an
# output file behind, else fell back to the shared taito_typex hook) -- added
# here so a plain `bash build_module.sh` produces a fully working, genuinely
# independent module, no shared-hook fallback needed or possible anymore.
echo "[rawthrills_pc] building board-native hook (build_hook.sh)"
bash "$HERE/build_hook.sh"

if [ ! -f "$HERE/ultra_rawthrills_hook32.dll" ]; then
    echo "[rawthrills_pc] ERROR: build_hook.sh did not produce ultra_rawthrills_hook32.dll" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[rawthrills_pc] embedding hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_rawthrills_pc.dll"
SRCS=("src/rawthrills_pc_main.c")

echo "[rawthrills_pc] building $OUT (spawn+inject for 32-bit Raw Thrills RawApp games)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_rawthrills_pc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_rawthrills_pc_run"; then
    echo "[rawthrills_pc] export OK: ul_rawthrills_pc_run"
else
    echo "[rawthrills_pc] ERROR: ul_rawthrills_pc_run not exported" >&2
    exit 1
fi

# Drop the built artifact (hook embedded inside it) in the project's
# completed-cores folder so the user can install it through the loader's
# "Install Module" UI when ready. No auto-install to %LOCALAPPDATA% -- that's
# the user's decision, not ours.
PROJ="$HERE/../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Raw Thrills"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[rawthrills_pc] artifact -> $COMPLETED_DIR/$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$COMPLETED_DIR/ultra_rawthrills_hook32.dll" "$HERE/ultra_rawthrills_hook32.dll"

echo "[rawthrills_pc] done -- install via loader's Settings -> Install Module when ready"
