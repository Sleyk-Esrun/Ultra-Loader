#!/usr/bin/env bash
# Build the generic_pc board's OWN, fully independent I/O hook DLL (both
# bitnesses): ultra_generic_pc_hook32.dll / ultra_generic_pc_hook64.dll.
#
# This is the board-native hook src/generic_pc_main.c prefers over the shared
# Type X fallback hook (see HOOK_DLL_PREF_32/64 in src/generic_pc_main.c). It
# is compiled ONLY from files in src/hook/ of THIS module -- no #include, no
# link, and no runtime dependency on native_x86/src/hook/'s compiled output or
# any other board module.
#
# Because generic_pc is the deliberate catch-all for an UNRECOGNISED PC arcade
# dump, the hook does not assume a graphics API: it arms JVS-over-serial I/O
# plus all four candidate present-hook chains (D3D8, D3D9, D3D11/DXGI, OpenGL)
# so whichever one the game's real imports resolve to is the one that fires.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/generic_pc && bash build_generic_hook.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/src/hook"

M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

# MSYS strips TMP/TEMP from native children; give gcc a writable temp inline
# (same pattern as build_module.sh / native_x86's build_hook.sh).
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# This board's OWN independent source tree (no files from any other module).
# omni_bridge.c is this board's OWN copy of the Ultra Omni-Bridge reference
# (modules/native_x86/src/hook/omni_bridge.c, adapted only via the -I below --
# see omni_bridge.c's file comment: copied, not linked, per board independence).
HOOK_SRC="jvs_core_generic.c jvs_serial_hook_generic.c hook_dllmain_generic.c \
d3d8_throttle_generic.c d3d9_throttle_generic.c d3d11_throttle_generic.c \
gl_present_generic.c dinput8_emu_generic.c audio_hook_generic.c omni_bridge.c ../../../../../../work/src/ultra_transport.c"

# omni_bridge.c #includes the shared, header-only omni_client.h (all 5 Omni
# services' env-var-backed config structs) from the loader's own work/include.
# Same relative depth as native_x86/src/hook (modules/<board>/src/hook), so the
# same "../../../../../../work/include" reaches "I:/Ultra Loader/work/include".
LOADER_INC="../../../../../../work/include"

# -static/-static-libgcc: self-contained DLL (no libgcc_s/libwinpthread dep),
# so the injected hook loads inside the game process which has no mingw
# runtime. -pipe: MSYS strips TMP/TEMP from native children so cc1's temp .s
# would otherwise land in an unwritable dir; -pipe removes that temp file.
CFLAGS="-std=c11 -O2 -w -pipe -static -static-libgcc -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601"

# -lpsapi: EnumProcessModules (IAT patcher walks every loaded module).
# -luser32/-lgdi32/-lwinmm: window/GDI/timer calls used across the hook set.
# -lopengl32: the GL present-hook path (gl_present_generic.c) resolves its
# swap/read-back entry points dynamically at runtime, but linking the import
# lib keeps the GL-side Win32 types/decls consistent across toolchain updates
# and matches this board's stated 4-API auto-detect contract explicitly.
LDLIBS="-lpsapi -luser32 -lwinmm -lgdi32 -lopengl32"

echo "[generic_pc] [32] building ultra_generic_pc_hook32.dll"
PATH="$M32:$PATH" env $T i686-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -shared \
    -o ultra_generic_pc_hook32.dll $HOOK_SRC $LDLIBS \
    -Wl,--out-implib,ultra_generic_pc_hook32.lib

echo "[generic_pc] [64] building ultra_generic_pc_hook64.dll"
PATH="$M64:$PATH" env $T x86_64-w64-mingw32-gcc $CFLAGS -I"$LOADER_INC" -shared \
    -o ultra_generic_pc_hook64.dll $HOOK_SRC $LDLIBS \
    -Wl,--out-implib,ultra_generic_pc_hook64.lib

# Stage the built DLLs at the module root (where generic_pc_main.c's
# find_hook_named() also checks: loader_dir directly).
cp -f ultra_generic_pc_hook32.dll ultra_generic_pc_hook64.dll "$HERE/"

echo "[generic_pc] built:"
ls -la "$HERE/ultra_generic_pc_hook32.dll" "$HERE/ultra_generic_pc_hook64.dll"

# Verify exports.
for BITS in 32 64; do
    DLL="$HERE/ultra_generic_pc_hook${BITS}.dll"
    OBJDUMP="/c/msys64/mingw${BITS}/bin/objdump.exe"
    if "$OBJDUMP" -p "$DLL" | grep -q "ultra_generic_pc_hook_id"; then
        echo "[generic_pc] [$BITS] export OK: ultra_generic_pc_hook_id"
    else
        echo "[generic_pc] [$BITS] ERROR: ultra_generic_pc_hook_id not exported" >&2
        exit 1
    fi
done

# Auto-install to %LOCALAPPDATA%\Ultra Loader\modules\generic_pc\ -- the FIRST
# path src/generic_pc_main.c's find_hook_named() checks, so the board-native
# hook is preferred over the shared Type X fallback.
INSTALL_DIR="$USERPROFILE/AppData/Local/Ultra Loader/modules/generic_pc"
INSTALL_DIR="$(cygpath -u "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")"
mkdir -p "$INSTALL_DIR"
cp -f "$HERE/ultra_generic_pc_hook32.dll" "$HERE/ultra_generic_pc_hook64.dll" "$INSTALL_DIR/"
echo "[generic_pc] installed -> $INSTALL_DIR/ultra_generic_pc_hook{32,64}.dll"

echo "[generic_pc] hook build done"
