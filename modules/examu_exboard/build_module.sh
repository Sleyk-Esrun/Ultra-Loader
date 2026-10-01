#!/usr/bin/env bash
# Build the Examu eX-BOARD per-board module DLL.
#
# Produces: ultra_examu_exboard.dll (in this module folder)
#
# The DLL exports `ul_examu_exboard_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting an eX-BOARD .exe
# game with ULTRA_MODULE=examu_exboard (or auto-detected from the board
# profile). NOTE: as of this rebuild, board_profile.c/.h contain NO "examu"
# or "exboard" resolution logic whatsoever (verified by grep) -- that phrase
# in the header comment above described intent, not an implemented lookup;
# flagging here so a future reader doesn't go looking for it.
#
# Examu eX-BOARD now has its OWN fully independent, self-contained hook DLL:
# ultra_examu_exboard_hook32.dll, built from src/hook/*_examu.c (own JVS
# engine, own IAT patcher, own D3D9/DirectInput8/audio hooks, own
# dialog-dismiss + byte-patch seam). It shares NO source file and NO compiled
# binary with modules/native_x86/src/hook/'s ultra_typex_hook32.dll (that
# tree was read as reference only) or with any other board's own hook. See
# src/hook/build_examu_hook.sh for the build and src/hook/*_examu.c headers
# for the per-file rationale.
#
# This module does the spawn+inject in-process: CreateProcess(game.exe,
# CREATE_SUSPENDED), VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
# (LoadLibraryA, ultra_examu_exboard_hook32.dll), ResumeThread,
# WaitForSingleObject.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/examu_exboard && bash build_module.sh

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

# Build this board's OWN independent hook DLL FIRST (src/hook/*_examu.c), so
# its bytes exist to embed. No shared runtime binary with modtest or any
# other board. build_examu_hook.sh also drops its own artifact into
# COMPLETED_DIR -- cleaned up below since it's embedded now, not shipped
# standalone.
HOOK_BUILD="$HERE/src/hook/build_examu_hook.sh"
if [ -f "$HOOK_BUILD" ]; then
    bash "$HOOK_BUILD"
else
    echo "[examu_exboard] ERROR: $HOOK_BUILD not found -- cannot build the independent hook" >&2
    exit 1
fi

if [ ! -f "$HERE/src/hook/ultra_examu_exboard_hook32.dll" ]; then
    echo "[examu_exboard] ERROR: hook build did not produce ultra_examu_exboard_hook32.dll" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[examu_exboard] embedding hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_examu_exboard.dll"
SRCS=("src/examu_exboard_main.c")

echo "[examu_exboard] building $OUT (spawn+inject for 32-bit eX-BOARD games)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_examu_exboard.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_examu_exboard_run"; then
    echo "[examu_exboard] export OK: ul_examu_exboard_run"
else
    echo "[examu_exboard] ERROR: ul_examu_exboard_run not exported" >&2
    exit 1
fi

# Drop the built artifact (hook embedded inside it) in the project's
# completed-cores folder so the user can install it through the loader's
# "Install Module" UI when ready. No auto-install to %LOCALAPPDATA% -- that's
# the user's decision, not ours.
PROJ="$HERE/../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra EX-Board"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[examu_exboard] artifact -> $COMPLETED_DIR/$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$COMPLETED_DIR/ultra_examu_exboard_hook32.dll" "$COMPLETED_DIR/ultra_typex_hook32.dll"

echo "[examu_exboard] done -- install via loader's Settings -> Install Module when ready"
