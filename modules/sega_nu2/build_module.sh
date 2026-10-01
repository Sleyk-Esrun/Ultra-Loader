#!/usr/bin/env bash
# Build the Sega Nu2 per-board module DLL.
#
# Produces: ultra_sega_nu2.dll (in this module folder)
#
# The DLL exports `ul_sega_nu2_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a Sega Nu2 .exe game
# and resolving its board to "sega_nu2" via the unified board profile.
#
# Nu2 games are 64-bit D3D11 with the "N2 / L3.1" keychip variant of the Sega
# DRM stack plus a BitLocker+TPM-sealed container. The module is a 32-bit
# spawner + injector: it PE-sniffs the game, spawns it suspended, and injects
# the matching-bitness hook (ultra_sega_nu2_hook64.dll for the normal 64-bit
# case via ultra-inject64.exe; ultra_sega_nu2_hook32.dll in-process for
# oddball 32-bit repacks). Env vars ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID /
# ULTRA_D3D11 / ULTRA_HOOK_D3D11 / ULTRA_IO4 / ULTRA_SEGA_KEYCHIP /
# ULTRA_SEGA_TRUECRYPT / ULTRA_SEGA_AMAUTH / ULTRA_SEGA_ALLNET /
# ULTRA_SEGA_AMDAEMON / ULTRA_SEGA_NETWORK / ULTRA_CONTROLS /
# ULTRA_JVS_ANALOG=3 / ULTRA_SEGA_WINDOWED / ULTRA_INPOUT32_BYPASS /
# ULTRA_DISMISS_DIALOGS / ULTRA_FPS_CAP=60 drive the hook's per-board
# branching -- see src/sega_nu2_main.c header for the full rationale.
#
# THIS BOARD'S HOOK IS FULLY INDEPENDENT: ultra_sega_nu2_hook{32,64}.dll is
# built from modules/sega_nu2/src/hook/ (9 fresh TUs, see build_nu2_hook.sh)
# -- it is NOT a copy of, and does not link against, sega_nu's hook or the
# shared native_x86 hook. This script builds that hook as part of the module
# build (step 2 below) so a single `bash build_module.sh` produces and stages
# everything this board needs. The generic ultra_typex_hook{32,64}.dll (a
# board-agnostic present/IAT hook used across many boards) is staged too, but
# ONLY as a last-resort fallback the spawner reaches for if the dedicated
# hook is somehow missing -- see sega_nu2_main.c's find_hook() search order.
#
# Artifact destination: Completed Ultra Loader Cores/native/Ultra Nu2/
#   - ultra_sega_nu2.dll          (this module -- 32-bit spawner/injector)
#   - ultra_sega_nu2_hook32.dll   (THIS board's own hook, built by
#                                  build_nu2_hook.sh -- used by 32-bit repacks)
#   - ultra_sega_nu2_hook64.dll   (THIS board's own hook -- used by 64-bit
#                                  Nu2 games, the normal case)
#   - ultra_typex_hook32.dll      (generic fallback, staged from modtest/)
#   - ultra_typex_hook64.dll      (generic fallback, staged from modtest/)
#   - ultra-inject64.exe          (staged from modtest/; generic cross-bitness
#                                  LoadLibrary helper the module shells out to
#                                  for 64-bit inject -- carries no board logic)
# NO %LOCALAPPDATA% auto-install per user spec.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/sega_nu2 && bash build_module.sh

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

# -----------------------------------------------------------------------------
# Build THIS board's own, fully independent hook DLL (both bitnesses) FIRST,
# so its bytes exist to embed. From modules/sega_nu2/src/hook/ -- see
# build_nu2_hook.sh. Not a copy of sega_nu's hook or the shared native_x86
# hook; shares no runtime binary with any other board.
# -----------------------------------------------------------------------------
echo "[sega_nu2] building this board's own hook (ultra_sega_nu2_hook32/64.dll)"
bash "$HERE/build_nu2_hook.sh"

if [ ! -f "$HERE/ultra_sega_nu2_hook32.dll" ] || [ ! -f "$HERE/ultra_sega_nu2_hook64.dll" ]; then
    echo "[sega_nu2] ERROR: build_nu2_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files. This also removes the old
# shared-typex-hook fallback entirely (see sega_nu2_main.c) -- there is no
# fallback resource, only this board's own embedded hook, so the generic
# ultra_typex_hook{32,64}.dll no longer needs staging at all. One file to
# sign/verify per board, and no separate hook file sits on disk between
# launches for anything to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[sega_nu2] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_sega_nu2.dll"
SRCS=("src/sega_nu2_main.c")

echo "[sega_nu2] building $OUT (32-bit spawner + injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_sega_nu2.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_sega_nu2_run"; then
    echo "[sega_nu2] export OK: ul_sega_nu2_run"
else
    echo "[sega_nu2] ERROR: ul_sega_nu2_run not exported" >&2
    exit 1
fi

# Drop the artifact (hooks embedded inside it) in the Completed Ultra Loader
# Cores tree (no %LOCALAPPDATA% auto-install per user spec). Still stage
# ultra-inject64.exe -- a generic, board-agnostic cross-bitness LoadLibrary
# helper (carries no board-specific logic, same tool every 64-bit board
# shells out to), not a per-board hook, so it's out of scope for embedding.
DEST_DIR="/i/Ultra Loader/Completed Ultra Loader Cores/native/Ultra Nu2"
INJECT64_SRC="/i/Ultra Loader/modtest/ultra-inject64.exe"

mkdir -p "$DEST_DIR"
cp -f "$OUT" "$DEST_DIR/$OUT"
echo "[sega_nu2] artifact -> $DEST_DIR/$OUT"

if [ -f "$INJECT64_SRC" ]; then
    cp -f "$INJECT64_SRC" "$DEST_DIR/ultra-inject64.exe"
    echo "[sega_nu2] inject64 -> $DEST_DIR/ultra-inject64.exe"
else
    echo "[sega_nu2] WARNING: inject64 not found at $INJECT64_SRC -- not staged" >&2
fi

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$DEST_DIR/ultra_sega_nu2_hook32.dll" "$DEST_DIR/ultra_sega_nu2_hook64.dll" \
      "$DEST_DIR/ultra_typex_hook32.dll" "$DEST_DIR/ultra_typex_hook64.dll" \
      "$HERE/ultra_sega_nu2_hook32.dll" "$HERE/ultra_sega_nu2_hook64.dll"

echo "[sega_nu2] done -- copy \"$DEST_DIR\" contents next to ultra-loader.exe to install"
