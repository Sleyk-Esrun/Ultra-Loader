#!/usr/bin/env bash
# Build the Sega Lindbergh module DLL.
#
# Produces: ultra_lindbergh.dll (in this module folder)
#
# The DLL exports `ul_lindbergh_run(int argc, char **argv)`. The loader's
# dispatcher (work/src/main.c) calls this after detecting an ELF game file.
#
# Usage:
#   cd modules_pkg/ultra-loader-modules/modules/lindbergh && bash build_module.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CC=i686-w64-mingw32-gcc
M32=/c/msys64/mingw32/bin
PATH="$M32:$PATH"

PROJ="$HERE/../../../.."
RUNTIME_SRC="src/runtime"

CFLAGS=(-pipe -m32 -O2 -w -fcommon -fpermissive -fno-strict-aliasing -masm=intel -D_WIN32_WINNT=0x0601 "-I$PROJ/work/include" "-I$RUNTIME_SRC" "-I$PROJ/work/src")
LDFLAGS=(-m32 -static -shared -lwinmm -lws2_32 -ladvapi32 -lgdi32 -lmsimg32 -luser32 -lkernel32 -lcomdlg32 -lcomctl32 -lshell32 -lole32 -lz -ldxguid -ldinput8 -lxinput -lwinhttp -lgcc_eh)

OUT="ultra_lindbergh.dll"

echo "[lindbergh] building $OUT (Lindbergh runtime as DLL)"
echo "  runtime src: $RUNTIME_SRC ($(ls "$RUNTIME_SRC"/*.c 2>/dev/null | wc -l) files)"

# Source files
SRCS=("$RUNTIME_SRC"/*.c)
# Shared utilities used by both the loader shell AND this runtime DLL.
# (miniz=ZIP, game_probe=game ID, lightgun_service=lightgun glue)
# di_range.c: ultra_di_is_xinput_device / ultra_di_set_axis_range
# board_detect.c: ul_detect_native_board_from_path, called by settings.c
# (moved out of main.c, which never links into a module)
SRCS+=("$PROJ/work/src/shared/di_range.c"
       "$PROJ/work/src/shared/board_detect.c")
# THE ADVANCED-FEATURES SYMBOLS ARE STUBBED HERE, NOT LINKED FOR REAL.
#
# settings.c is compiled into this runtime DLL for the loader's settings UI, and
# when the Advanced Features panel was added it gained references to ULDP
# (ul_slotmap), ULCM (ul_mesh) and the Ultra UI kit (ul_ui). Linking the REAL
# implementations in made the module build -- and then crash on entry, every
# Lindbergh title, with an access violation inside that code before a single
# frame rendered (2026-09-19).
#
# None of it belongs in a game module: ULDP seating and the ULCM mesh are the
# LOADER's services and are driven from the loader's own process, and the UI kit
# paints the loader's settings window, which a module never shows. The module
# only needs the symbols to exist so settings.c resolves.
#
# So: stubs. If a future change makes a module genuinely need one of these, link
# the real TU for that one symbol family and re-test the module ON A GAME --
# linking is not running, which is exactly the mistake that caused the crash.
# (lindbergh_advanced_stubs.c needs no entry -- SRCS globs src/runtime/*.c above.)
# ufw.c (container reader) + monocypher (BLAKE2b / Ed25519) back the container
# and signing paths settings.c now touches. The loader itself links these via
# work/Makefile's include/crypto/*.c wildcard; this script lists them by name.
SRCS+=("$PROJ/work/src/shared/ufw.c"
       "$PROJ/work/include/crypto/monocypher.c"
       "$PROJ/work/include/crypto/monocypher-ed25519.c")
SRCS+=("$PROJ/work/src/ufw_launch.c"
       "$PROJ/work/src/shared/ultra_io_publish.c")
SRCS+=("$PROJ/work/src/shared/miniz.c"
       "$PROJ/work/src/shared/game_probe.c"
       "$PROJ/work/src/shared/lightgun_service.c"
       "$PROJ/work/src/shared/omniwheel_service.c"
       "$PROJ/work/src/shared/omnisteer_service.c"
       "$PROJ/work/src/shared/omniplay_service.c")
# Loader-shell files the runtime depends on (settings UI, splash, console, catalog).
# These will be split out in a future cleanup but live in work/src/ today.
# assets.c + mesa_fetch.c satisfy asset_*/mesa_fetch_* refs from settings/splash/
# boot_console (unblocked while adding the FTDI shim for Cruis'n Blast).
# report.c backs the Settings dialog's "Generate Report" button. settings.c
# calls report_count_runs()/report_generate() directly, so omitting this file
# does not merely disable the button -- it breaks the LINK, and a failed link
# here leaves NO ultra_lindbergh.dll at all. That is how this board silently
# stopped building: every other board compiles pause_menu_service.c but not
# settings.c, so lindbergh is the only one that needs report.c and the only one
# the omission could break.
# The Hypothesis Engine. settings.c calls hypothesis_dialog() directly, so
# leaving these out does not merely hide a button -- it BREAKS THE LINK, and a
# failed link here leaves no ultra_lindbergh.dll at all. This is the second time
# that exact trap has fired on this board: it is the only one that compiles
# settings.c, so it is the only one where a new loader feature can stop a MODULE
# from building. Anything settings.c gains, this list gains.
SRCS+=("$PROJ/work/src/settings.c"
       "$PROJ/work/src/report.c"
       "$PROJ/work/src/hypothesis.c"
       "$PROJ/work/src/hypothesis_rules.c"
       "$PROJ/work/src/hypothesis_boards.c"
       "$PROJ/work/src/hypothesis_session.c"
       "$PROJ/work/src/hypothesis_runner.c"
       "$PROJ/work/src/hypothesis_ui.c"
       "$PROJ/work/src/hypothesis_submit.c"
       "$PROJ/work/src/splash.c"
       "$PROJ/work/src/boot_console.c"
       "$PROJ/work/src/ultra_catalog.c"
       "$PROJ/work/src/playtime.c"
       "$PROJ/work/src/assets.c"
       "$PROJ/work/src/mesa_fetch.c")
# Shared in-game pause/settings menu service. Lifted from sdl_window.c so every
# module (Lindbergh, Type X / X2, Sega Europa-R, RawThrills PC, native libretro
# frontends) renders the same GDI layered-window overlay through one codebase.
SRCS+=("$PROJ/work/src/pause_menu_service.c"
       "$PROJ/work/src/pause_menu_settings_io.c"
       "$PROJ/work/src/credit_injector_service.c")
# Universal Ultra Omni-Track lightgun engine
SRCS+=("$PROJ/work/src/lightgun/ultra_lightgun.c"
       "$PROJ/work/src/lightgun/backends/ul_backend_rawinput_win32.c")

TMP="C:/msys64/tmp" TEMP="C:/msys64/tmp" "$CC" "${CFLAGS[@]}" "${SRCS[@]}" -o "$OUT" "${LDFLAGS[@]}" \
    -Wl,--out-implib,ultra_lindbergh.lib

ls -la "$OUT"
# Verify export
if /c/msys64/mingw32/bin/objdump.exe -p "$OUT" | grep -q "ul_lindbergh_run"; then
    echo "[lindbergh] export OK: ul_lindbergh_run"
else
    echo "[lindbergh] ERROR: ul_lindbergh_run not exported" >&2
    exit 1
fi

# Auto-install to the SINGLE supported install location per user spec:
#   %LOCALAPPDATA%\Ultra Loader\modules\lindbergh\ultra_lindbergh.dll
# Production users do this via Settings -> Install Module.
# Install through the one shared copy of this step, at
# modules/_install_module.sh. It resolves the path with guards, and it
# installs the dll and its signature together or installs neither.
. "$HERE/../_install_module.sh"
INSTALL_DIR="$(ul_module_install_dir lindbergh)"
# The mkdir stays here, not only inside the helper. Later lines in some
# of these scripts copy unsigned support DLLs into $INSTALL_DIR, and the
# helper creates the directory only on the runs where it actually
# installs, which is not every run.
mkdir -p "$INSTALL_DIR"
ul_install_module lindbergh "$OUT"

echo "[lindbergh] done"
