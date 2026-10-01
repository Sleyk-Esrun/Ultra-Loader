#!/usr/bin/env bash
# Build the Cave PC per-board module DLL.
#
# Produces: ultra_cave_pc.dll (in this module folder)
#
# The DLL exports `ul_cave_pc_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a Cave PC .exe
# game and resolving its board to "cave_pc" via the unified board profile.
#
# This module does the spawn+inject in-process: CreateProcess(game.exe,
# CREATE_SUSPENDED), VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
# (LoadLibraryA, ultra_typex_hook32.dll), ResumeThread, WaitForSingleObject.
#
# 2026-08-02 REBUILD (3rd architecture this same day -- see src/hook/
# build_cave_hook.sh's header for the full timeline): this board's hook was
# rebuilt AGAIN, this time by PORTING the proven game_dsii.c + native_x86
# board_io_ops-based hook engine into this board's own independent
# src/hook/ tree -- the source combination project memory records as having
# gotten Deathsmiles II's "crash+loadtime+gate FIXED, general input
# confirmed", before this morning's from-scratch cave_plumbing.c/cave_io.c/
# cave_render9.c/cave_dialog.c rewrite briefly replaced it. This is a full
# swap, not a merge: cave_io.c/cave_render9.c/cave_dialog.c/cave_plumbing.c
# are gone from src/hook/, replaced by an unmodified copy of native_x86/src/
# hook/'s 2026-07-31 tree (jvs_core.c, jvs_serial_hook.c, hook_dllmain.c,
# board_io_ops.c/.h, controls.c, input_profiles.c, d3d9_throttle.c,
# dinput8_emu.c, board_profile.c, audio_hook.c, and the rest of that proven
# file set) plus this board's own game_dsii.c (Deathsmiles II's per-game
# board_io_ops callbacks: CVKEY.222 bypass, COM3 protocol responder,
# direct-write input thread, coin/credit CVKEY reference-key fix) and
# omni_bridge.c (Ultra Omni-Bridge, wired via hook_dllmain.c). No shared
# compiled binary with native_x86/src/hook/ or any other board, ever --
# only the proven SOURCE was ported, following the same pattern already
# used for taito_typex/taito_typex2 earlier the same day. See src/hook/
# build_cave_hook.sh for the exact file list and full rationale.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/cave_pc && bash build_module.sh

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

# Build this board's OWN independent hook DLL from source FIRST, so its
# bytes exist to embed. No dependency on modtest/ or native_x86/src/hook/'s
# compiled output.
echo "[cave_pc] building independent hook (src/hook/build_cave_hook.sh)"
bash "$HERE/src/hook/build_cave_hook.sh"

HOOK_OUT="$HERE/src/hook/ultra_cave_pc_hook32.dll"
if [ ! -f "$HOOK_OUT" ]; then
    echo "[cave_pc] ERROR: $HOOK_OUT not built" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[cave_pc] embedding hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_cave_pc.dll"
SRCS=("src/cave_pc_main.c")

echo "[cave_pc] building $OUT (spawn+inject for 32-bit Cave PC games)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_cave_pc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_cave_pc_run"; then
    echo "[cave_pc] export OK: ul_cave_pc_run"
else
    echo "[cave_pc] ERROR: ul_cave_pc_run not exported" >&2
    exit 1
fi

# Drop the artifact (hook embedded inside it) in the project's completed-
# cores folder. No auto-install to %LOCALAPPDATA% -- the user installs via
# the loader's "Install Module" UI when ready.
PROJ="$HERE/../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Cave PC"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[cave_pc] artifact -> $COMPLETED_DIR/$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$COMPLETED_DIR/ultra_cave_pc_hook32.dll" \
      "$COMPLETED_DIR/ultra_typex_hook32.dll" "$COMPLETED_DIR/ultra_typex_hook64.dll"

echo "[cave_pc] done -- install via loader's Settings -> Install Module when ready"
