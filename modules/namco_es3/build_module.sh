#!/usr/bin/env bash
# Build the Namco ES3 per-board module + board-named hook DLLs.
#
# Produces (in this module folder):
#   ultra_namco_es3.dll           - 32-bit spawner/injector module
#   ultra_namco_es3_hook32.dll    - 32-bit hook (MKDX, WMMT5, TC5, DE4D)
#   ultra_namco_es3_hook64.dll    - 64-bit hook (Tekken 7, Pokken)
#
# The hook source is namco_es3's OWN copy, compiled fresh every build from
# src/hook/ IN THIS MODULE FOLDER (see the per-project directive: every board
# gets its own fully independent, self-contained hook DLL - no shared runtime
# binary with any other board, no dependency on native_x86/src/hook's compiled
# output, not even at build time). The DLLs are named for THIS board so every
# board's folder is self-explanatory - no cross-board names.
#
# Artifact destination: <this folder>/  (build in place)
#   The loader searches the module's own folder first when resolving hooks.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/namco_es3 && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC32=i686-w64-mingw32-gcc
CC64=x86_64-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
M64=/c/msys64/mingw64/bin

mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# ── 1. Hook DLLs FIRST (board-named, from namco_es3's OWN local hook source) ──
#
# HOOK_DIR now points ONLY at this module's own src/hook/ copy - every file
# below was cloned from native_x86/src/hook/ into THIS folder (with board-
# suffixed _es3 names) and stands alone: no #include reaches outside src/hook/
# except standard Windows headers, and no build step reaches into
# native_x86/ at all anymore. board_io_ops_es3.c (board_io_ops.h's
# registration/forwarding plumbing, required by hook_dllmain_es3.c and
# jvs_serial_hook_es3.c) and audio_hook_es3.c (DirectSound + XAudio2
# diagnostic hook, a capability this board never had before) are included -
# both are genuinely required for hook_dllmain_es3.c to link (it calls
# ultra_board_ops_get() and ultra_audio_install() unconditionally).
HOOK_DIR="$HERE/src/hook"
# Relative paths from HOOK_DIR to the LOADER's own shared public headers +
# shared pause-menu service (work/include, work/src) - this is the loader's
# cross-module public API every board module already links against (same as
# the module DLL above), NOT native_x86/src/hook, so it stays referenced here.
# HOOK_DIR keeps the same depth-from-root as before (modules/namco_es3/src/hook
# vs. modules/native_x86/src/hook), so the relative path is unchanged.
LOADER_INC_REL="../../../../../../work/include"
PAUSE_SRC_REL="../../../../../../work/src/pause_menu_service.c ../../../../../../work/src/pause_menu_settings_io.c"
# omni_bridge.c (Ultra Omni-Bridge -- opt-in raw keyboard/mouse-emulation
# bridge, ULTRA_BRIDGE_ENABLE, default OFF) is this board's OWN copy of the
# native_x86/src/hook/omni_bridge.c reference (per the per-board independence
# rule above); its one #include "omni_client.h" resolves against the same
# LOADER_INC_REL (work/include) every other file here already uses -- no new
# include path, no dependency on native_x86 at build time.

HOOK_SRC="jvs_core_es3.c jvs_serial_hook_es3.c dongle_bypass_es3.c namco_dongle_es3.c rawthrills_io_es3.c hook_dllmain_es3.c dinput8_emu_es3.c gamestate_probe_es3.c d3d8_throttle_es3.c d3d8_sharpen_es3.c d3d9_throttle_es3.c typex2_gun_es3.c sega_board_es3.c d3d11_throttle_es3.c gl_present_es3.c sega_keychip_es3.c board_profile_es3.c board_io_ops_es3.c audio_hook_es3.c taito_fastio_es3.c card_reader_es3.c sega_io4_es3.c konami_io_es3.c namco_amdaemon_es3.c namco_io_es3.c controls_es3.c input_profiles_es3.c input_src_keyboard_es3.c input_src_xinput_es3.c input_src_dinput_es3.c input_src_mouse_es3.c input_src_gun_es3.c input_autopilot_es3.c pause_menu_wire_es3.c omni_bridge.c game_mkdx_bananapass.c game_mkdx_iauth.c game_mkdx_banapassport_http.c game_swbp_dongle.c ../../../../../../work/src/ultra_tracer.c"
CFLAGS_HOOK="-std=c11 -O2 -Wall -pipe -static -static-libgcc"

echo "[namco_es3] building ultra_namco_es3_hook32.dll"
(cd "$HOOK_DIR" && PATH="$M32:$PATH" env $T $CC32 $CFLAGS_HOOK -I$LOADER_INC_REL -shared \
    -o "$HERE/ultra_namco_es3_hook32.dll" \
    $HOOK_SRC $PAUSE_SRC_REL \
    -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi -lws2_32 -lwinhttp)

echo "[namco_es3] building ultra_namco_es3_hook64.dll"
(cd "$HOOK_DIR" && PATH="$M64:$PATH" env $T $CC64 $CFLAGS_HOOK -I$LOADER_INC_REL -shared \
    -o "$HERE/ultra_namco_es3_hook64.dll" \
    $HOOK_SRC $PAUSE_SRC_REL \
    -lpsapi -luser32 -lwinmm -lgdi32 -ldinput8 -ldxguid -lole32 -lsetupapi -lws2_32 -lwinhttp)

if [ ! -f "$HERE/ultra_namco_es3_hook32.dll" ] || [ ! -f "$HERE/ultra_namco_es3_hook64.dll" ]; then
    echo "[namco_es3] ERROR: hook build did not produce both hook DLLs" >&2
    exit 1
fi

# ── 2. Embed both hooks as RCDATA resources (hook_res.rc) ───────────────
# One file to sign/verify per board, and no separate hook file sits on disk
# between launches for anything to tamper with. See
# docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[namco_es3] embedding hooks as RCDATA resources"
(cd "$HERE/src" && PATH="$M32:$PATH" env $T windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc)
RES_OBJ="src/hook_res.res.o"

# ── 3. Module DLL (32-bit, the loader is 32-bit), hooks embedded inside ──

CFLAGS_MOD=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing
            -D_WIN32_WINNT=0x0601
            "-I$HERE/include")
LDFLAGS_MOD=(-m32 -static -shared
             -lwinmm -lws2_32 -ladvapi32 -lgdi32 -luser32 -lkernel32
             -lole32 -ldxguid -ldinput8 -ldxgi -lshell32)

OUT_MOD="ultra_namco_es3.dll"
echo "[namco_es3] building $OUT_MOD (32-bit module)"
echo "  resources: $RES_OBJ"

PATH="$M32:$PATH" env $T $CC32 "${CFLAGS_MOD[@]}" src/namco_es3_main.c "$RES_OBJ" \
    -o "$OUT_MOD" "${LDFLAGS_MOD[@]}" \
    -Wl,--out-implib,ultra_namco_es3.lib

if /c/msys64/mingw32/bin/objdump.exe -p "$OUT_MOD" | grep -q "ul_namco_es3_run"; then
    echo "[namco_es3] export OK: ul_namco_es3_run"
else
    echo "[namco_es3] ERROR: ul_namco_es3_run not exported" >&2
    exit 1
fi

# ── 4. Summary ─────────────────────────────────────────────────────────
# The hooks are embedded in $OUT_MOD now, never shipped standalone. Remove
# any stale copy from a build predating this change.
rm -f "$HERE/ultra_namco_es3_hook32.dll" "$HERE/ultra_namco_es3_hook64.dll"

echo ""
echo "[namco_es3] artifacts:"
ls -la "$HERE/ultra_namco_es3.dll"
echo "[namco_es3] done"
