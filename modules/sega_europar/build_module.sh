#!/usr/bin/env bash
# Build the Sega Europa-R per-board module: the spawner/injector DLL AND this
# board's own, fully self-contained hook + controls-proxy DLLs.
#
# Produces (in this module folder / src/hook/):
#   ultra_sega_europar.dll          -- the spawner + injector (this script)
#   ultra_sega_europar_hook32.dll   -- the injected rendering/JVS/audio/DRM
#                                      hook, THIS BOARD'S OWN independent
#                                      build (src/hook/build_europar_hook.sh)
#   dinput8.CRK.dll                  -- the file-swapped DirectInput8 controls
#                                      proxy (src/hook/dinput8_europar.c),
#                                      also this board's own build
#
# The spawner DLL exports `ul_sega_europar_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after resolving a Sega
# Europa-R .exe game to the "sega_europar" board profile.
#
# NEITHER of the two hook artifacts above is shared with any other board.
# ultra_sega_europar_hook32.dll used to be a stale copy of
# modtest/ultra_typex_hook32.dll (Taito Type X's shared hook, WRONG name, not
# even the name sega_europar_main.c's HOOK_DLL_NAME looks for) -- that copy
# is no longer staged. src/hook/build_europar_hook.sh compiles this board's
# own jvs_core_europar.c / jvs_serial_hook_europar.c / d3d9_throttle_europar.c
# / audio_hook_europar.c / sega_keychip_europar.c / omni_bridge_europar.c
# (Ultra Omni-Bridge, opt-in via ULTRA_BRIDGE_ENABLE, default off) /
# hook_dllmain_europar.c into ultra_sega_europar_hook32.dll, and
# dinput8_europar.c (kept, now
# actually compiled) into dinput8.CRK.dll. Neither includes, links against,
# nor copies at runtime any file from modules/native_x86/src/hook/ or from
# modtest/.
#
# Artifact destination: Completed Ultra Loader Cores/native/Ultra Europa-R/
#   - ultra_sega_europar.dll        (spawner/injector)
#   - ultra_sega_europar_hook32.dll (this board's own hook, HOOK_DLL_NAME)
#   - dinput8.CRK.dll                (this board's own controls proxy,
#                                     DINPUT8_CRK_NAME -- file-swapped into
#                                     the GAME's own folder as "dinput8.dll"
#                                     by sega_europar_main.c at launch time,
#                                     not staged next to ultra-loader.exe)
# No %LOCALAPPDATA% auto-install.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_europar && bash build_module.sh

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

# Build this board's OWN hook + controls-proxy DLLs FIRST
# (src/hook/build_europar_hook.sh), so the hook's bytes exist to embed.
# Fully independent of native_x86/src/hook/ and of modtest/ -- see that
# script's header and sega_europar_main.c's header for the full contract.
echo "[sega_europar] building own hook + controls-proxy DLLs"
( cd "$HERE/src/hook" && bash build_europar_hook.sh )

HOOK_NAME="ultra_sega_europar_hook32.dll"
DI8_NAME="dinput8.CRK.dll"
HOOK_BUILT="$HERE/src/hook/$HOOK_NAME"
DI8_BUILT="$HERE/src/hook/$DI8_NAME"

if [ ! -f "$HOOK_BUILT" ]; then
    echo "[sega_europar] ERROR: $HOOK_BUILT was not produced by build_europar_hook.sh" >&2
    exit 1
fi
if [ ! -f "$DI8_BUILT" ]; then
    echo "[sega_europar] ERROR: $DI8_BUILT was not produced by build_europar_hook.sh" >&2
    exit 1
fi

# Embed the RENDERING hook as an RCDATA resource inside the main module DLL
# (hook_res.rc) instead of shipping it as a standalone file -- one file to
# sign/verify for the injected part of this board, and no separate hook file
# sits on disk between launches for anything to tamper with. The controls
# proxy (dinput8.CRK.dll) is a DIFFERENT mechanism (file-swapped into the
# GAME's own folder, never injected) and keeps shipping as its own file --
# see docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_europar] embedding rendering hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_europar.dll"
SRCS=("src/sega_europar_main.c")

echo "[sega_europar] building $OUT (32-bit spawner + injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_europar.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_europar_run"; then
    echo "[sega_europar] export OK: ul_sega_europar_run"
else
    echo "[sega_europar] ERROR: ul_sega_europar_run not exported" >&2
    exit 1
fi

# Drop the artifacts in the Completed Ultra Loader Cores tree (no
# %LOCALAPPDATA% auto-install per user spec) so the user can copy the whole
# folder next to ultra-loader.exe and run. The rendering hook is embedded in
# $OUT now; only the controls proxy still ships as its own file (it has to --
# it's copied into the GAME's own folder at launch, a real on-disk file is
# the entire point of that mechanism).
DEST_DIR="/i/Ultra Loader/Completed Ultra Loader Cores/native/Ultra Europa-R"

mkdir -p "$DEST_DIR"
cp -f "$OUT" "$DEST_DIR/$OUT"
echo "[sega_europar] artifact -> $DEST_DIR/$OUT"

cp -f "$DI8_BUILT" "$DEST_DIR/$DI8_NAME"
echo "[sega_europar] controls -> $DEST_DIR/$DI8_NAME (own independent build, matches DINPUT8_CRK_NAME; "
echo "                            sega_europar_main.c file-swaps this into the GAME's own folder as"
echo "                            \"dinput8.dll\" at launch time -- it is not loaded from next to"
echo "                            ultra-loader.exe)"

# Legacy artifact cleanup: the rendering hook is embedded in $OUT now, never
# shipped standalone. Remove any stale copy from a build predating this change.
rm -f "$DEST_DIR/$HOOK_NAME"

echo "[sega_europar] done"
