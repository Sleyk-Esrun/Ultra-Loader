#!/usr/bin/env bash
# Build the Taito NESiCAxLive (gen 1) per-board module DLL.
#
# Produces: ultra_taito_nesica.dll (in this module folder)
#
# The DLL exports `ul_taito_nesica_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting a NESiCAxLive .exe
# game and resolving its board to "taito_nesica" via the unified board
# profile.
#
# NLX1 games are 32-bit D3D9 (Chaos Breaker Plus, Melty Blood AA, Blade Arcus,
# etc.) with occasional D3D11 refreshes. The module is a 32-bit spawner +
# injector: it PE-sniffs the game and injects the matching-bitness hook
# (ultra_taito_nesica_hook32.dll in-process for the normal 32-bit case; the
# 64-bit hook via ultra-inject64.exe for the rare 64-bit repack).
#
# The hook DLL is THIS BOARD'S OWN independent build -- built from
# src/hook/*.c by build_nesica_hook.sh (invoked below), sharing no source,
# object file, or compiled binary with native_x86/src/hook/'s legacy shared
# ultra_typex_hook{32,64}.dll (modtest/) or any other board module. Per the
# project directive: every board gets its own fully independent, self-
# contained hook DLL.
#
# Env vars ULTRA_BOARD_HINT / ULTRA_SYSTEM_ID / ULTRA_NESICA_GEN=1 /
# ULTRA_D3D9 / ULTRA_D3D11 / ULTRA_CONTROLS / ULTRA_JVS_ANALOG=2 /
# ULTRA_SEGA_WINDOWED / ULTRA_DONGLE_BYPASS / ULTRA_NESICA_OFFLINE /
# ULTRA_DISMISS_DIALOGS / ULTRA_FPS_CAP=60 drive the hook's per-board
# branching -- see src/taito_nesica_main.c header for the full rationale.
#
# Artifact destination: Completed Ultra Loader Cores/native/Ultra NesicaLiveX/
#   - ultra_taito_nesica.dll        (this module -- spawner + injector)
#   - ultra_taito_nesica_hook32.dll (this board's OWN independent hook build)
#   - ultra_taito_nesica_hook64.dll (this board's OWN independent hook build)
#   - ultra-inject64.exe            (staged from modtest/; generic bitness-
#                                    crossing inject helper -- contains no
#                                    board-specific I/O/JVS logic, so it is
#                                    not part of the "hook DLL" independence
#                                    requirement; every module in this project
#                                    reuses this same OS-level utility)
# NO %LOCALAPPDATA% auto-install per user spec.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_nesica && bash build_module.sh

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

# Build this board's OWN independent hook DLL (both bitnesses) FIRST, from
# src/hook/*.c, so its bytes exist to embed.
echo "[taito_nesica] building independent hook DLL (build_nesica_hook.sh)"
bash "$HERE/build_nesica_hook.sh"
HOOK32_SRC="$HERE/ultra_taito_nesica_hook32.dll"
HOOK64_SRC="$HERE/ultra_taito_nesica_hook64.dll"
if [ ! -f "$HOOK32_SRC" ] || [ ! -f "$HOOK64_SRC" ]; then
    echo "[taito_nesica] ERROR: build_nesica_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_nesica] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_nesica.dll"
SRCS=("src/taito_nesica_main.c")

echo "[taito_nesica] building $OUT (32-bit spawner + injector module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_nesica.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_nesica_run"; then
    echo "[taito_nesica] export OK: ul_taito_nesica_run"
else
    echo "[taito_nesica] ERROR: ul_taito_nesica_run not exported" >&2
    exit 1
fi

# Drop the artifact (hooks embedded inside it) in the Completed Ultra Loader
# Cores tree (no %LOCALAPPDATA% auto-install per user spec). Still stage
# ultra-inject64.exe -- a generic, board-agnostic cross-bitness helper (no
# board-specific I/O/JVS logic, reused as-is by every module), so it's out of
# scope for embedding.
DEST_DIR="/i/Ultra Loader/Completed Ultra Loader Cores/native/Ultra NesicaLiveX"
INJECT64_SRC="/i/Ultra Loader/modtest/ultra-inject64.exe"

mkdir -p "$DEST_DIR"
cp -f "$OUT" "$DEST_DIR/$OUT"
echo "[taito_nesica] artifact -> $DEST_DIR/$OUT"

if [ -f "$INJECT64_SRC" ]; then
    cp -f "$INJECT64_SRC" "$DEST_DIR/ultra-inject64.exe"
    echo "[taito_nesica] inject64 -> $DEST_DIR/ultra-inject64.exe (generic bitness-crossing helper, not board-specific)"
else
    echo "[taito_nesica] WARNING: inject64 not found at $INJECT64_SRC -- not staged" >&2
fi

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$DEST_DIR/ultra_taito_nesica_hook32.dll" "$DEST_DIR/ultra_taito_nesica_hook64.dll" \
      "$HOOK32_SRC" "$HOOK64_SRC"

echo "[taito_nesica] done -- copy \"$DEST_DIR\" contents next to ultra-loader.exe to install"
