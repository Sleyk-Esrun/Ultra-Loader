#!/usr/bin/env bash
# Build the konami_pc module DLL (ultra_konami_pc.dll) and deploy it next to the
# 64-bit module host so the loader's .exe routing picks up the CURRENT build.
#
# The module is loaded by the 64-bit ultra-module-host.exe, so it MUST be 64-bit.
# It launches the (usually 32-bit) Type X game suspended and injects the I/O+D3D8
# hook -- same-bitness in-process, or via the cross-bitness ultra-inject32.exe
# helper when the game is 32-bit and the host is 64-bit (the common case).
#
# 2026-08-03: the hook (ultra_typex_hook{32,64}.dll) is no longer a standalone
# file staged beside the module -- it's built by src/hook/build_hook.sh (called
# below) and embedded as an RCDATA resource inside ultra_konami_pc.dll itself
# (hook_res.rc), extracted to a temp file at injection time by
# extract_hook_to_temp() in konami_pc_module.cpp. One file to sign/verify, and
# the old "keep this in sync with the hook build" staleness risk (a prior
# version of this comment warned about it) is now structurally impossible --
# the hook build is a hard prerequisite of this script, not a separate manual
# step. ultra-inject{32,64}.exe stay as standalone files: they're generic,
# board-agnostic cross-bitness LoadLibrary helpers, not per-board hook logic.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
M64=/c/msys64/mingw64/bin
M32=/c/msys64/mingw32/bin
INC="$HERE/../../include"
SRC="src/module/konami_pc_module.cpp"
# The GHL replacement-DLL swap is its own translation unit.
SRC2="src/module/crk_swap.cpp"
OUT="ultra_konami_pc.dll"

# MSYS strips TMP/TEMP from native children, so gcc/cc1/as fall back to
# C:\WINDOWS (unwritable) and abort. Hand every compile a writable temp INLINE
# (the loader Makefile RUNENV / build_hook.sh pattern). Prefix every gcc/g++.
mkdir -p /c/msys64/tmp 2>/dev/null || true
T="TMP=C:/msys64/tmp TEMP=C:/msys64/tmp TMPDIR=C:/msys64/tmp"

# Build this board's OWN hook (both bitnesses) from source FIRST -- its bytes
# are needed for the embed step below.
echo "[module] building I/O hook (src/hook/build_hook.sh)"
bash "$HERE/src/hook/build_hook.sh"

HOOK32_BUILT="$HERE/src/hook/ultra_typex_hook32.dll"
HOOK64_BUILT="$HERE/src/hook/ultra_typex_hook64.dll"
if [ ! -f "$HOOK32_BUILT" ] || [ ! -f "$HOOK64_BUILT" ]; then
    echo "[module] ERROR: build_hook.sh did not produce both hook DLLs" >&2
    exit 1
fi

# Embed both as RCDATA resources inside the main module DLL (hook_res.rc)
# instead of shipping them as standalone files. See
# docs/PROJECT_LICENSING_AND_SIGNING.md.
echo "[module] embedding hook as RCDATA resources"
( cd "$HERE/src/module" && PATH="$M32:$PATH" TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" windres -O coff \
    --include-dir . -o hook_res.res.o hook_res.rc )
RES_OBJ="src/module/hook_res.res.o"

echo "[module] building $OUT (mingw32, static C++ runtime)"
# The unified board-profile framework (board_profile.c) + the Sega detail layer
# (sega_board.c) compile into the module so select_board() resolves a profile and
# arms the ULTRA_* env knobs the injected hook reads. They are C (not C++), so
# compile them with gcc to a .o and link -- the module declares their symbols
# extern "C", so C linkage on both sides matches.
HOOKDIR="src/hook"
echo "[module] compiling board_profile.c + sega_board.c (C linkage)"
PATH="$M32:$PATH" env $T gcc -std=c11 -O2 -c -I "$HOOKDIR" "$HOOKDIR/board_profile.c" -o board_profile.o
PATH="$M32:$PATH" env $T gcc -std=c11 -O2 -c -I "$HOOKDIR" "$HOOKDIR/sega_board.c"    -o sega_board.o

# UNIFIED boot console + system/title catalog (shared with the 32-bit loader). The
# module shows the SAME Ultra Loader boot console (board + game title) before it
# spawns+injects a konami_pc game, so every launch has one boot screen. These are
# the loader's own C sources (work/src), compiled here for the 32-bit module.
# boot_console.c also needs work/include (assets.h, the disk-first/embedded-
# fallback asset loader added after this script was first written -- this
# compile line never carried -I work/include, so it's been broken since that
# header landed; nothing had run this build end-to-end since to notice) and
# two symbols (asset_register_font, g_ul_audio_gated) that live in assets.c /
# splash.c -- the full loader's own asset subsystem, not appropriate to drag
# into this lightweight module. inject_bc_stubs.c (src/hook/) already solves
# this exact problem for the standalone ultra-inject32/64.exe injectors
# (disk-only font load, audio gate always off); reuse it here too.
LOADER_SRC="../../../../work/src"
LOADER_INC="../../../../work/include"
echo "[module] compiling boot_console.c + ultra_catalog.c (shared, C linkage)"
PATH="$M32:$PATH" env $T gcc -std=c11 -O2 -c -I "$LOADER_SRC" -I "$LOADER_INC" "$LOADER_SRC/boot_console.c"  -o boot_console.o
PATH="$M32:$PATH" env $T gcc -std=c11 -O2 -c -I "$LOADER_SRC" -I "$LOADER_INC" "$LOADER_SRC/ultra_catalog.c" -o ultra_catalog.o
PATH="$M32:$PATH" env $T gcc -std=c11 -O2 -c "$HOOKDIR/inject_bc_stubs.c" -o inject_bc_stubs.o

PATH="$M32:$PATH" env $T g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
    -I "$INC" -I "$HOOKDIR" -I "$LOADER_SRC" -I "$LOADER_INC" -o "$OUT" "$SRC" "$SRC2" "$RES_OBJ" \
    board_profile.o sega_board.o boot_console.o ultra_catalog.o inject_bc_stubs.o \
    -lkernel32 -luser32 -lgdi32 -lwinmm
rm -f board_profile.o sega_board.o boot_console.o ultra_catalog.o inject_bc_stubs.o

# Verify the export is by name (the host does GetProcAddress("ultra_module_create")).
if ! PATH="$M32:$PATH" objdump -p "$OUT" | grep -q "ultra_module_create"; then
    echo "ERROR: $OUT does not export ultra_module_create by name" >&2; exit 1
fi
echo "[module] OK: exports ultra_module_create"

# Deploy beside the host. The loader's ul_find_near() looks in <exe_dir> then
# <exe_dir>/modtest, so the project-root modtest/ is the deploy dir. Only the
# generic cross-bitness injectors are staged alongside now -- the I/O hook is
# embedded in $OUT itself (see hook_res.rc above).
DEPLOY="$HERE/../../../../modtest"
if [ -d "$DEPLOY" ]; then
    cp -f "$OUT" "$DEPLOY/"
    HOOK="$HERE/src/hook"
    for f in ultra-inject32.exe ultra-inject64.exe; do
        [ -f "$HOOK/$f" ] && cp -f "$HOOK/$f" "$DEPLOY/"
    done
    # Legacy artifact cleanup: the hook is embedded in $OUT now, never shipped
    # standalone. Remove any stale loose copy from a build predating this change.
    rm -f "$DEPLOY/ultra_typex_hook32.dll" "$DEPLOY/ultra_typex_hook64.dll"
    echo "[module] deployed $OUT (+ generic injectors) to $DEPLOY"
else
    echo "[module] NOTE: deploy dir $DEPLOY not found; built $OUT in place only"
fi
echo "[module] done"

# ============================================================================
#  THE FIVE CRK REPLACEMENT DLLs
#
#  Konami's real shaiolib.dll refuses to initialize without its GHL board: a
#  bare LoadLibrary of the stock file returns ERROR_DLL_INIT_FAILED (1114), and
#  because KSHG.exe imports it statically the Windows loader kills the process
#  with 0xC0000142 before the game's first instruction. These replacements are
#  therefore not optional polish - without shaiolib.CRK.dll this board has no
#  runnable title at all.
#
#  Carried forward from the pre-rebuild module, with ONE behavioral change:
#  shaiolib_crk.c's Nt*-wait-primitive layer (ULTRA_KONAMI_DEEP_HOOKS) now
#  DEFAULTS OFF. It used to default ON while the build script claimed otherwise,
#  and it is what drove ntdll's critical-section deadlock diagnostic into
#  dereferencing a NULL timeout.
# ============================================================================
# Where the module (and therefore its CRK siblings) must end up: the loader
# loads from %LOCALAPPDATA%, not Ship/, and the module copies the CRK files
# into the game folder from beside itself at launch.
#
# This board is deliberately left alone apart from how the path is resolved. It
# does NOT install its own module dll here, only the five CRK DLLs that are
# Silent Hill's runtime, and that stays true. What changes is that an unset
# LOCALAPPDATA now stops the script instead of writing those five DLLs to
# /Ultra Loader/modules/konami_pc, a directory nothing has ever read from. The
# resolver lives in one place for every board; see modules/_install_module.sh.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir konami_pc)"
mkdir -p "$INSTALL_DIR" 2>/dev/null || true

CRK_CFLAGS=(-pipe -m32 -O2 -w -fcommon -fno-strict-aliasing -D_WIN32_WINNT=0x0601)
CRK_CC=i686-w64-mingw32-gcc

crk_build() {   # $1 = source, $2 = output, rest = libs
    local src="$1" out="$2"; shift 2
    echo "[konami_pc] building $out"
    PATH="$M32:$PATH" TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" \
        "$CRK_CC" "${CRK_CFLAGS[@]}" "$src" -o "$out" -m32 -static -shared "$@"
    if [ ! -f "$out" ]; then echo "[konami_pc] FAILED to build $out" >&2; exit 1; fi
}

# ---------------------------------------------------------------------------
#  SKIP THE CRK REBUILD UNLESS IT IS ACTUALLY WANTED.
#
#  These five DLLs are Silent Hill's runtime, not this module's. They were being
#  rebuilt from source and copied over the working copies on EVERY konami_pc
#  build -- including builds done for a completely unrelated reason, like a
#  one-word label change in the shared pause menu. If src/*_crk.c is ahead of the
#  binaries and unplaytested, that silently swaps a locked game's runtime for
#  untested code, and the .CRK.dll files are in NO container, so the firmware
#  system does not insulate them the way it does modules.
#
#  Default is now SKIP. Set ULTRA_BUILD_CRK=1 when you actually intend to
#  rebuild Silent Hill's runtime and are prepared to playtest it.
# ---------------------------------------------------------------------------
if [ "${ULTRA_BUILD_CRK:-0}" = "1" ]; then
    crk_build src/shaiolib_crk.c shaiolib.CRK.dll -lwinmm -luser32 -lkernel32 -lgdi32
    crk_build src/libbms_crk.c   libbms.CRK.dll   -lkernel32
    crk_build src/libutil_crk.c  libutil.CRK.dll  -lkernel32 -luser32
    crk_build src/shaerror_crk.c shaerror.CRK.dll -lkernel32
    # -Wl,--kill-at is NOT optional here. Without it mingw exports the stdcall
    # decorated name DirectInput8Create@20, the game imports the undecorated
    # DirectInput8Create, and Windows puts up 'the procedure entry point could
    # not be located' before the game starts. Exactly that dialog appeared on
    # 2026-09-16 from a third-party dinput8 stub with the same defect.
    crk_build src/dinput8_crk.c  dinput8.CRK.dll  -ldinput8 -lkernel32 -luser32 -luuid -Wl,--kill-at
else
    echo "[konami_pc] CRK DLLs: SKIPPED (set ULTRA_BUILD_CRK=1 to rebuild them)."
    echo "[konami_pc]   They are Silent Hill's runtime, not this module's, and they"
    echo "[konami_pc]   live in no container. Rebuilding them needs a playtest."
fi

# The exports the GAME imports must exist, or the swap turns one failure mode
# into another - an "Entry Point Not Found" dialog instead of a boot.
for sym in shaiolib_initialize shaiolib_update shaiolib_COIN_count \
           shaiolib_BUTTON_status shaiolib_GUN_status shaiolib_SECURE_check; do
    PATH="$M32:$PATH" objdump -p shaiolib.CRK.dll | grep -q "$sym" \
        || { echo "[konami_pc] MISSING export: $sym" >&2; exit 1; }
done
echo "[konami_pc] shaiolib.CRK.dll exports OK"

# dinput8 must export the UNDECORATED name: the game imports DirectInput8Create,
# and a build that leaves the stdcall decoration on produces exactly the
# "procedure entry point could not be located" dialog seen on 2026-09-16.
PATH="$M32:$PATH" objdump -p dinput8.CRK.dll | grep -qE '[0-9]+[[:space:]]+DirectInput8Create$' \
    || echo "[konami_pc] WARNING: dinput8.CRK.dll may export a DECORATED DirectInput8Create" >&2

for f in shaiolib.CRK.dll libbms.CRK.dll libutil.CRK.dll shaerror.CRK.dll dinput8.CRK.dll; do
    cp -f "$f" "$INSTALL_DIR/$f"
    echo "[konami_pc] installed -> $INSTALL_DIR/$f"
done
