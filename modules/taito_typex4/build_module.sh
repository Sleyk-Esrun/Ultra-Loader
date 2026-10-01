#!/usr/bin/env bash
# Build the Taito Type X4 per-board module DLL + its OWN independent hook DLL.
#
# Produces (in this module folder):
#   ultra_taito_typex4.dll          -- the spawner (exports ul_taito_typex4_run)
#   ultra_taito_typex4_hook32.dll   -- this board's OWN hook (32-bit)
#   ultra_taito_typex4_hook64.dll   -- this board's OWN hook (64-bit)
#
# The spawner DLL exports `ul_taito_typex4_run(int argc, char **argv)`. The
# loader's dispatcher (work/src/main.c) calls this after detecting a Type X4
# .exe game and resolving its board to "taito_typex4" via the unified board
# profile.
#
# INDEPENDENCE: the hook DLLs are built ENTIRELY from src/hook/*.c -- this
# board's own clean-room JVS engine, Fast-I/O DMAC transport, D3D11 present
# hook, and DirectSound/XAudio2 hook (src/hook/build_typex4_hook.sh). No file
# from modules/native_x86/src/hook/ or modtest/ is #included, linked, or
# copied at build or run time. Compare the old skeleton behavior (and the
# still-unfixed pattern in modules/taito_typex/build_module.sh /
# modules/taito_typex2/build_module.sh), which staged a hook DLL copied
# straight from modtest/ -- that dependency is what this build now removes
# for Type X4.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/taito_typex4 && bash build_module.sh

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
         -lole32 -ldxguid -ldinput8 -ldxgi)

# ---------------------------------------------------------------------------
# Build THIS board's own, fully independent hook DLL (32 + 64-bit) FIRST, so
# its bytes exist to embed. See src/hook/build_typex4_hook.sh for the full
# rationale -- it compiles only the 5 source files in src/hook/, with no
# dependency on native_x86 or modtest.
# ---------------------------------------------------------------------------
echo "[taito_typex4] building independent hook DLLs (src/hook/build_typex4_hook.sh)"
bash "$HERE/src/hook/build_typex4_hook.sh"

HOOK32="$HERE/src/hook/ultra_taito_typex4_hook32.dll"
HOOK64="$HERE/src/hook/ultra_taito_typex4_hook64.dll"

# Verify the hook DLLs export their diagnostic id (confirms the right,
# independently-built DLL, not a stray copy).
for h in "$HOOK32" "$HOOK64"; do
    OBJDUMP=/c/msys64/mingw32/bin/objdump.exe
    [ "$h" = "$HOOK64" ] && OBJDUMP=/c/msys64/mingw64/bin/objdump.exe
    if [ ! -f "$h" ]; then
        echo "[taito_typex4] ERROR: $h not produced by build_typex4_hook.sh" >&2
        exit 1
    fi
    if "$OBJDUMP" -p "$h" | grep -q "ultra_taito_typex4_hook_id"; then
        echo "[taito_typex4] export OK: ultra_taito_typex4_hook_id ($h)"
    else
        echo "[taito_typex4] ERROR: ultra_taito_typex4_hook_id not exported ($h)" >&2
        exit 1
    fi
done

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[taito_typex4] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_taito_typex4.dll"
SRCS=("src/taito_typex4_main.c")

echo "[taito_typex4] building $OUT (32-bit skeleton)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_taito_typex4.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_taito_typex4_run"; then
    echo "[taito_typex4] export OK: ul_taito_typex4_run"
else
    echo "[taito_typex4] ERROR: ul_taito_typex4_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\taito_typex4\ -- one
# file, hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir taito_typex4)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module taito_typex4 "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_taito_typex4_hook32.dll" "$INSTALL_DIR/ultra_taito_typex4_hook64.dll" \
      "$HERE/ultra_taito_typex4_hook32.dll" "$HERE/ultra_taito_typex4_hook64.dll"

echo "[taito_typex4] done"
