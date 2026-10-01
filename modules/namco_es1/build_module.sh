#!/usr/bin/env bash
# Build the Namco System N2 module DLL.
#
# Produces: ultra_namco_es1.dll (in this module folder)
#
# System N2 is a real Linux PC board (Arcade Linux, Debian 4.0-based) -- this
# module uses the same ELF32 + dynlink guest-Linux pipeline as Lindbergh, its
# OWN independent copy (ported from modules/lindbergh/src/runtime/'s generic,
# non-board-specific files only -- see namco_es1_main.c's header). No shared
# compiled binary with lindbergh or any other module.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/namco_es1 && bash build_module.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

PROJ="$HERE/../../../.."
RUNTIME_SRC="src/runtime"

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fpermissive -fno-strict-aliasing -masm=intel -D_WIN32_WINNT=0x0601 "-I$PROJ/work/include" "-I$RUNTIME_SRC")
LDFLAGS=(-m32 -static -shared -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32 -lole32 -lz -lgcc_eh)

OUT="ultra_namco_es1.dll"

mkdir -p /c/msys64/tmp 2>/dev/null || true

echo "[namco_es1] building $OUT (Linux/ELF32 guest engine, own independent copy)"
echo "  runtime src: $RUNTIME_SRC ($(ls "$RUNTIME_SRC"/*.c 2>/dev/null | wc -l) files)"

SRCS=("$RUNTIME_SRC"/*.c)

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_namco_es1.lib

ls -la "$OUT"
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_namco_es1_run"; then
    echo "[namco_es1] export OK: ul_namco_es1_run"
else
    echo "[namco_es1] ERROR: ul_namco_es1_run not exported" >&2
    exit 1
fi

# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir namco_es1)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module namco_es1 "$OUT"

echo "[namco_es1] done"
