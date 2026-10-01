#!/usr/bin/env bash
# Build the Taito NESiCAxLive2 (gen 2) per-board module DLL.
#
# Produces: ultra_taito_nesica2.dll (in this module folder)
#
# The DLL exports `ul_taito_nesica2_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a NESiCAxLive2 .exe
# game and resolving its board to "taito_nesica2" via the unified board
# profile.
#
# NLX2 games are 64-bit D3D11 fighters/shooters (BlazBlue CP Extend, KOF XIII
# Climax, Persona 4 Arena Ultimax NLX2, Under Night In-Birth, Nitroplus
# Blasterz IH, Chaos Code NSC). The module is a 32-bit spawner + injector:
# it PE-sniffs the game and injects the matching-bitness hook
# (ultra_taito_nesica2_hook64.dll via ultra-inject64.exe for the normal
# 64-bit case; ultra_taito_nesica2_hook32.dll in-process for the rare
# 32-bit repack).
#
# INDEPENDENCE (2026-08-01 rebuild): this script used to just COPY the
# shared modtest/ultra_typex_hook{32,64}.dll into the artifact folder. Per
# the project owner's per-board directive (every modern arcade board gets
# its own fully independent, self-contained hook DLL -- no shared runtime
# binary with any other board), it now BUILDS this board's own hook from
# src/hook/ (via src/hook/build_nesica2_hook.sh) and stages THAT instead.
# ultra-inject64.exe (a generic, board-agnostic cross-bitness DLL-injection
# helper -- not board hook logic) is still staged from modtest/, matching
# the precedent already set by the taito_typex3 / taito_typezero independent
# rebuilds.
#
# Env vars ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID / ULTRA_NESICA_GEN=2 /
# ULTRA_D3D11 / ULTRA_D3D9 / ULTRA_CONTROLS / ULTRA_JVS_ANALOG=2 /
# ULTRA_SEGA_WINDOWED / ULTRA_DONGLE_BYPASS / ULTRA_NESICA_OFFLINE /
# ULTRA_CARD_READER / ULTRA_DISMISS_DIALOGS / ULTRA_FPS_CAP=60 drive the
# hook's per-board branching -- see src/taito_nesica2_main.c header and
# src/hook/taito_nesica2_hook.h for the full rationale.
#
# Artifact destination: Completed Ultra Loader Cores/native/Ultra NesicaLiveX 2/
#   - ultra_taito_nesica2.dll        (this module -- 32-bit spawner/injector)
#   - ultra_taito_nesica2_hook32.dll (THIS board's own independent hook; 32-bit repacks)
#   - ultra_taito_nesica2_hook64.dll (THIS board's own independent hook; normal 64-bit NLX2)
#   - ultra-inject64.exe             (staged from modtest/; generic cross-bitness inject helper)
# NO %LOCALAPPDATA% auto-install per user spec.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_nesica2 && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# Build this board's OWN independent hook DLLs first (src/hook/). Fully
# self-contained: no source or binary shared with native_x86/src/hook,
# modtest/, or any other board's module folder.
echo "[taito_nesica2] building this board's OWN independent hook (src/hook/build_nesica2_hook.sh)"
bash "$HERE/src/hook/build_nesica2_hook.sh"

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

if [ ! -f "$HERE/ultra_taito_nesica2_hook32.dll" ] || [ ! -f "$HERE/ultra_taito_nesica2_hook64.dll" ]; then
    echo "[taito_nesica2] ERROR: build_nesica2_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_nesica2] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_nesica2.dll"
SRCS=("src/taito_nesica2_main.c")

echo "[taito_nesica2] building $OUT (32-bit spawner + injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_nesica2.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_nesica2_run"; then
    echo "[taito_nesica2] export OK: ul_taito_nesica2_run"
else
    echo "[taito_nesica2] ERROR: ul_taito_nesica2_run not exported" >&2
    exit 1
fi

# Drop the artifact (hooks embedded inside it) in the Completed Ultra Loader
# Cores tree (no %LOCALAPPDATA% auto-install per user spec). Still stage the
# generic cross-bitness inject64 helper (sourced from modtest/ -- a
# board-agnostic OS-level tool, not board hook logic), so it's out of scope
# for embedding.
DEST_DIR="/i/Ultra Loader/Completed Ultra Loader Cores/native/Ultra NesicaLiveX 2"
INJECT64_SRC="/i/Ultra Loader/modtest/ultra-inject64.exe"

mkdir -p "$DEST_DIR"
cp -f "$OUT" "$DEST_DIR/$OUT"
echo "[taito_nesica2] artifact -> $DEST_DIR/$OUT"

if [ -f "$INJECT64_SRC" ]; then
    cp -f "$INJECT64_SRC" "$DEST_DIR/ultra-inject64.exe"
    echo "[taito_nesica2] inject64 -> $DEST_DIR/ultra-inject64.exe"
else
    echo "[taito_nesica2] WARNING: inject64 not found at $INJECT64_SRC -- not staged" >&2
fi

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change
# (including the even older shared-hook copy step).
rm -f "$DEST_DIR/ultra_typex_hook32.dll" "$DEST_DIR/ultra_typex_hook64.dll" \
      "$DEST_DIR/ultra_taito_nesica2_hook32.dll" "$DEST_DIR/ultra_taito_nesica2_hook64.dll" \
      "$HERE/ultra_taito_nesica2_hook32.dll" "$HERE/ultra_taito_nesica2_hook64.dll"

echo "[taito_nesica2] done -- copy \"$DEST_DIR\" contents next to ultra-loader.exe to install"
