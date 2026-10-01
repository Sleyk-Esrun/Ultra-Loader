#!/usr/bin/env bash
# Build the Sega Nu per-board module DLL.
#
# Produces: ultra_sega_nu.dll (in this module folder)
#
# The DLL exports `ul_sega_nu_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after resolving a Sega Nu .exe game
# to the "sega_nu" board profile.
#
# This is a 64-BIT spawner + injector module (Sega Nu is 64-bit-only, Win
# Embedded 8 + D3D11). It builds this board's OWN independent hook first
# (build_hook.sh, at this module's root -- produces ultra_sega_nu_hook32.dll
# + ultra_sega_nu_hook64.dll from src/hook/'s 13 source files: keychip/
# amauth/allnet/truecrypt/amdaemon stubs + D3D11 present + audio + IO4-USB +
# Omni-Bridge + pause menu), embeds both as RCDATA resources inside
# ultra_sega_nu.dll (hook_res.rc), then builds src/sega_nu_main.c, which
# extracts the matching-bitness hook to a temp file and injects it: queues an
# APC for LoadLibraryA on the main thread as primary inject path, resumes,
# then does a CreateRemoteThread(LoadLibraryA) fallback.
#
# 2026-08-03: this script previously never called build_hook.sh at all, and
# instead staged the old shared ultra_typex_hook{32,64}.dll from modtest/ --
# dead weight, since sega_nu_main.c's hook lookup never actually searched for
# that filename. This board's own hook + build_hook.sh already existed
# (someone had built and verified them separately); this script just wasn't
# wired to use them. Fixed: real hook build + embed, shared-hook staging
# removed entirely -- this board has zero build-or-run dependency on
# modtest/ or native_x86/src/hook/'s compiled output now.
#
# Artifact destination: Completed Ultra Loader Cores/native/Ultra Nu/
#   - ultra_sega_nu.dll         (this module; both hook bitnesses embedded)
#   - ultra-inject64.exe        (staged from modtest/, a generic stand-alone
#                                injector the user or a helper script can
#                                drive if the in-module inject_dll_* paths
#                                ever need to be bypassed for diagnosis --
#                                not used by this module's own code path)
# NO %LOCALAPPDATA% auto-install per user spec.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_nu && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# MODULE must be 32-bit: ultra-loader.exe is 32-bit and LoadLibrary of a
# 64-bit DLL fails with 193 ERROR_BAD_EXE_FORMAT. 64-bit GAMES are still
# supported -- injection is delegated to ultra-inject64.exe (see main.c).
CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline.
mkdir -p /c/msys64/tmp 2>/dev/null || true

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
        -D_WIN32_WINNT=0x0603
        "-I$HERE/include")
LDFLAGS=(-m32 -static -shared
         -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
         -lole32 -ldxguid -ldinput8 -ldxgi -lshell32)

OUT="ultra_sega_nu.dll"
SRCS=("src/sega_nu_main.c")

# Build this board's OWN independent hook (both bitnesses) from source FIRST
# -- its bytes are needed for the embed step below.
echo "[sega_nu] building independent hook (build_hook.sh)"
bash "$HERE/build_hook.sh"

HOOK32_BUILT="$HERE/ultra_sega_nu_hook32.dll"
HOOK64_BUILT="$HERE/ultra_sega_nu_hook64.dll"
if [ ! -f "$HOOK32_BUILT" ] || [ ! -f "$HOOK64_BUILT" ]; then
    echo "[sega_nu] ERROR: build_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify
# per board, and no separate hook file sits on disk between launches for
# anything to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_nu] embedding hook as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

echo "[sega_nu] building $OUT (64-bit spawner + injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_nu.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw64/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_nu_run"; then
    echo "[sega_nu] export OK: ul_sega_nu_run"
else
    echo "[sega_nu] ERROR: ul_sega_nu_run not exported" >&2
    exit 1
fi

# Drop the artifact in the Completed Ultra Loader Cores tree (no %LOCALAPPDATA%
# auto-install per user spec). Stage the generic stand-alone injector too (not
# used by this module's own code path, but kept available for diagnosis).
DEST_DIR="/i/Ultra Loader/Completed Ultra Loader Cores/native/Ultra Nu"
INJ64_SRC="/i/Ultra Loader/modtest/ultra-inject64.exe"

mkdir -p "$DEST_DIR"
cp -f "$OUT" "$DEST_DIR/$OUT"
echo "[sega_nu] artifact -> $DEST_DIR/$OUT"

if [ -f "$INJ64_SRC" ]; then
    cp -f "$INJ64_SRC" "$DEST_DIR/ultra-inject64.exe"
    echo "[sega_nu] inject64 -> $DEST_DIR/ultra-inject64.exe"
else
    echo "[sega_nu] NOTE: ultra-inject64.exe not found at $INJ64_SRC -- not staged" >&2
fi

# Legacy artifact cleanup: both hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change (both
# this board's own old standalone-hook names and the even older shared-hook
# name it used to stage from modtest/).
rm -f "$DEST_DIR/ultra_sega_nu_hook32.dll" "$DEST_DIR/ultra_sega_nu_hook64.dll" \
      "$DEST_DIR/ultra_typex_hook32.dll" "$DEST_DIR/ultra_typex_hook64.dll"

echo "[sega_nu] done"
