#!/usr/bin/env bash
# Build the IGS PC per-board module DLL + its OWN independent hook DLLs.
#
# Produces:
#   ultra_igs_pc.dll           -- the module (spawner+injector, in this folder)
#   ultra_igs_pc_hook32.dll    -- the injected I/O hook, 32-bit games
#   ultra_igs_pc_hook64.dll    -- the injected I/O hook, 64-bit games
#
# The module DLL exports `ul_igs_pc_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after resolving an IGS PC .exe game
# to the "igs_pc" board profile (generic JVS I/O; distinct from IGS's PGM3/
# KOV3 HD board, which is its own separate module/core).
#
# 2026-08-01 UPDATE: this used to build ONLY src/igs_pc_main.c -- a
# spawn+inject skeleton with nothing to actually inject (the hook DLL it looks
# for, ultra_igs_pc_hook{32,64}.dll, was never built). The hook now exists at
# src/hook/ as a FULLY SELF-CONTAINED tree (jvs_core_igs.c, jvs_serial_hook_igs.c,
# dinput8_emu_igs.c, d3d9_throttle_igs.c, audio_hook_igs.c, igs_dialog.c,
# igs_plumbing.c, hook_dllmain_igs.c) -- no shared source file and no shared
# compiled binary with modules/native_x86/src/hook/ (the multi-board "Type X
# hook") or any other board's module. See src/hook/igs_hook.h for the full
# rationale. This script now builds BOTH the module and its own hook, and
# stages the hook DLLs to the SAME single supported install location the
# module DLL already uses.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/igs_pc && bash build_module.sh

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

# Build IGS PC's OWN independent hook DLLs FIRST (both bitnesses), so their
# bytes exist to embed. Fully self-contained -- see src/hook/igs_hook.h and
# src/hook/build_igs_hook.sh.
echo "[igs_pc] building own hook DLLs (src/hook/build_igs_hook.sh)"
bash "$HERE/src/hook/build_igs_hook.sh"

if [ ! -f "$HERE/src/hook/ultra_igs_pc_hook32.dll" ] || [ ! -f "$HERE/src/hook/ultra_igs_pc_hook64.dll" ]; then
    echo "[igs_pc] ERROR: hook build did not produce ultra_igs_pc_hook{32,64}.dll" >&2
    exit 1
fi

# Embed both hooks as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[igs_pc] embedding hooks as RCDATA resources"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_igs_pc.dll"
SRCS=("src/igs_pc_main.c")

echo "[igs_pc] building $OUT (spawn+inject module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_igs_pc.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_igs_pc_run"; then
    echo "[igs_pc] export OK: ul_igs_pc_run"
else
    echo "[igs_pc] ERROR: ul_igs_pc_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\igs_pc\ -- one file,
# hooks embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir igs_pc)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module igs_pc "$OUT"

# Legacy artifact cleanup: the hooks are embedded in $OUT now, never shipped
# standalone. Remove any stale copy from a build predating this change.
rm -f "$INSTALL_DIR/ultra_igs_pc_hook32.dll" "$INSTALL_DIR/ultra_igs_pc_hook64.dll" \
      "$HERE/ultra_igs_pc_hook32.dll" "$HERE/ultra_igs_pc_hook64.dll"

echo "[igs_pc] done"
