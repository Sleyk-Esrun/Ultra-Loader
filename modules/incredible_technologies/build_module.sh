#!/usr/bin/env bash
# Build the Incredible Technologies (Golden Tee PC "nowboard") per-board
# module DLL + its OWN independent injected hook DLL.
#
# Produces:
#   ultra_incredible_technologies.dll        (spawner/injector, this folder)
#   src/hook/ultra_incredible_technologies_hook32.dll  (the real I/O/render/
#                                              audio hook -- see src/hook/build_it_hook.sh)
#
# The module DLL exports `ul_incredible_technologies_run(int argc, char **argv)`.
# The loader's dispatcher (work/src/main.c) calls this after resolving an
# Incredible Technologies .exe game to the "incredible_technologies" board
# profile. incredible_technologies_main.c then spawns the game suspended and
# injects whichever hook find_hook() locates -- preferring this board's own
# ultra_incredible_technologies_hook32.dll over the shared Type X fallback.
#
# src/hook/ is a fully self-contained tree (own JVS engine, COM-port
# interception, D3D9 present throttle, DirectSound/XAudio2 diagnostic,
# DllMain) -- no shared runtime binary with any other board, no dependency
# on modules/native_x86/src/hook's compiled output. See src/hook/it_hook.h
# for the design notes (trackball-as-mouse routing, JVS ROTARY wiring, etc).
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/incredible_technologies && bash build_module.sh

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

# This board's OWN self-contained hook DLL (src/hook/), built FIRST so its
# bytes exist to embed. Own JVS engine (trackball-as-mouse ROTARY routing),
# D3D9 present throttle and DirectSound/XAudio2 diagnostic.
echo "[incredible_technologies] building board-native hook (src/hook/build_it_hook.sh)"
bash "$HERE/src/hook/build_it_hook.sh"

if [ ! -f "$HERE/src/hook/ultra_incredible_technologies_hook32.dll" ]; then
    echo "[incredible_technologies] ERROR: hook build did not produce ultra_incredible_technologies_hook32.dll" >&2
    exit 1
fi

# Embed the hook as an RCDATA resource inside the main module DLL (hook_res.rc)
# instead of shipping it as a standalone file -- one file to sign/verify per
# board, and no separate hook file sits on disk between launches for anything
# to tamper with. See docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[incredible_technologies] embedding hook as RCDATA resource"
( cd "$HERE/src" && TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/hook_res.res.o"

OUT="ultra_incredible_technologies.dll"
SRCS=("src/incredible_technologies_main.c")

echo "[incredible_technologies] building $OUT (32-bit skeleton module)"
echo "  sources: ${SRCS[*]}"
echo "  resources: $RES_OBJ"

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$RES_OBJ" \
    -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_incredible_technologies.lib

ls -la "$OUT"

# Verify export.
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_incredible_technologies_run"; then
    echo "[incredible_technologies] export OK: ul_incredible_technologies_run"
else
    echo "[incredible_technologies] ERROR: ul_incredible_technologies_run not exported" >&2
    exit 1
fi

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\incredible_technologies\
# -- one file, hook embedded inside it.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir incredible_technologies)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module incredible_technologies "$OUT"

# Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
# standalone. build_it_hook.sh still installs its own copy to INSTALL_DIR --
# remove it here so only the one embedded-hook DLL actually ships.
rm -f "$INSTALL_DIR/ultra_incredible_technologies_hook32.dll"

echo "[incredible_technologies] done"
