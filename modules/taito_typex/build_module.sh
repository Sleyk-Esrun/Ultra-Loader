#!/usr/bin/env bash
# Build the Taito Type X per-board module DLL.
#
# Produces: ultra_taito_typex.dll (in this module folder)
#
# The DLL exports `ul_taito_typex_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a Type X .exe game
# and resolving its board to "taito_typex" via the unified board profile.
#
# This module does the spawn+inject in-process: CreateProcess(game.exe,
# CREATE_SUSPENDED), VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
# (LoadLibraryA, ultra_taito_typex_hook32.dll), ResumeThread, WaitForSingleObject.
# The hook DLL (ultra_taito_typex_hook32.dll) provides DRM bypass, I/O emulation,
# and D3D wrapper -- it's installed alongside this module DLL.
#
# 2026-09-27: UNFROZEN. From 2026-08-01 through today this board embedded a
# pinned prebuilt hook binary (still kept at prebuilt/ultra_taito_typex_hook32.dll
# for reference/rollback) because a from-source rebuild once produced a
# stable-but-black D3D8 backbuffer on live playtest. ROOT CAUSE (see
# taito_typex_main.c's long ULTRA_TYPEX_UPSCALE comment): the loader's
# Settings dialog defaulted Upscaler=FSR, which silently forced the D3D8
# hook's upscale/sharpen path, and THAT path produced the black screen --
# independent of which hook binary was used. taito_typex_main.c already
# forces native/no-upscale by default unless a user explicitly opts in
# (ULTRA_TYPEX_UPSCALE), so the from-source hook is not actually at fault and
# was never re-tried after that safeguard landed.
#
# Unfreezing now because this board's shared hook was missing real, already-
# written capability (Omni-Bridge, the audio hook, the pause-menu overlay --
# all added to src/ on 2026-08-02, one day after the freeze, and never
# actually shipped) that later titles on this board need and cannot get from
# a static binary. Chaos Breaker -- the one title proven against the OLD
# frozen hook -- is NOT put at risk by this: it has its own signed container
# (firmware/taito_typex/Chaos Breaker.ufw, built 2026-09-20) that embeds its
# OWN staged copy of the module, and ufw_launch_for_game() makes the loader
# prefer a container's own module over this shared installed one (see
# work/src/main.c's ULTRA_MODULE_PATH comment) -- so launching Chaos Breaker
# through that .ufw is unaffected by anything this script does. A snapshot of
# the pre-unfreeze frozen build also sits at
# _baseline/taito_typex_frozen_hook_pre_unfreeze_2026-09-27/ as a rollback.
#
# The hook is now built FROM src/build_hook.sh (this board's own independent
# source tree -- no dependency on modtest/ or native_x86/src/hook/, same as
# every other board) and embedded fresh on every build, rather than pointing
# at the frozen prebuilt/ copy.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typex && bash build_module.sh

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

# Build this board's own independent hook FROM SOURCE (src/build_hook.sh),
# same pattern every other board follows. Produces src/ultra_taito_typex_hook32.dll
# (and a 64-bit sibling that main.c never looks for). Only the 32-bit one is used.
echo "[taito_typex] building hook from source (src/build_hook.sh)"
bash "$HERE/src/build_hook.sh"
HOOK32="$HERE/src/ultra_taito_typex_hook32.dll"
if [ ! -f "$HOOK32" ]; then
    echo "[taito_typex] ERROR: $HOOK32 not produced by build_hook.sh" >&2
    exit 1
fi
if /c/msys64/mingw32/bin/objdump.exe -p "$HOOK32" | grep -q "ultra_typex_hook_id"; then
    echo "[taito_typex] hook export OK: ultra_typex_hook_id"
else
    echo "[taito_typex] ERROR: ultra_typex_hook_id not exported from freshly-built hook" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. hook_res.rc now points at the freshly-built hook above, not
# the frozen prebuilt/ copy. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typex] embedding freshly-built hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_typex.dll"
SRCS=("src/taito_typex_main.c")

echo "[taito_typex] building $OUT (spawn+inject for 32-bit Type X games)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typex.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_typex_run"; then
    echo "[taito_typex] export OK: ul_taito_typex_run"
else
    echo "[taito_typex] ERROR: ul_taito_typex_run not exported" >&2
    exit 1
fi

# Drop the built artifact (hook embedded inside it) in the project's
# completed-cores folder so the user can install it through the loader's
# "Install Module" UI when ready. No auto-install to %LOCALAPPDATA% -- that's
# the user's decision, not ours.
PROJ="$HERE/../../../.."
COMPLETED_DIR="$PROJ/Completed Ultra Loader Cores/native/Ultra Type X"
mkdir -p "$COMPLETED_DIR"
cp -f "$OUT" "$COMPLETED_DIR/$OUT"
echo "[taito_typex] artifact -> $COMPLETED_DIR/$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change
# (both the pinned-prebuilt-named files and the even older shared-hook name).
rm -f "$COMPLETED_DIR/ultra_taito_typex_hook32.dll" "$COMPLETED_DIR/ultra_taito_typex_hook64.dll" \
      "$COMPLETED_DIR/ultra_typex_hook32.dll" "$COMPLETED_DIR/ultra_typex_hook64.dll"

echo "[taito_typex] done -- install via loader's Settings -> Install Module when ready"
