#include <stdlib.h>
/*
 * lindbergh_preinit.c - Sega Lindbergh boot sequence emulation
 *
 * Ported from Lindbergh Launcher (SDL3 loader) to Ultra Loader.
 * Adapted: loader_ctx_t removed (no dependency), log_warn -> loader_log.
 *
 * Runs BEFORE __libc_start_main to put game BSS/memory into the state
 * it expects after a real Lindbergh hardware boot.
 *
 * Real Lindbergh startup order:
 *   1. MontaVista Linux boots
 *   2. Baseboard driver loads (/dev/lbb)
 *   3. ssound kernel modules load (Creative SB audio)
 *   4. libsegaapi.so constructors run -> audio context initialized
 *   5. amLib initializes -> DIP switch context, dongle context set
 *   6. xinit starts X server
 *   7. Game launches via 'xinit abc -- X -s 0 -nolisten tcp'
 *
 * BSS range for After Burner Climax (Rev A):
 *   seg[3] va=0x82e6000 sz=0x1ddcf80 -> end=0x0a0c2f80   (data + BSS)
 *
 * NOTE: DIPSWITCH_CONTEXT_ADDR (0x0a0ca168) is 0x71e8 bytes PAST the end
 * of the mapped segment. Do NOT write to it - it is not mapped.
 * The SDL3 loader used a broader BSS estimate (0x0a0ca3c0) that doesn't
 * match this ELF's actual segment layout.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "loader.h"
#include "lindbergh_boards.h"

/* Set an env var through the Win32 env, our CRT, AND ucrtbase's CRT (Mesa reads
 * getenv against ucrtbase's snapshot, which SetEnvironmentVariableA misses). */
static void ul_preinit_set_env(const char *name, const char *val)
{
    char buf[192];
    snprintf(buf, sizeof buf, "%s=%s", name, val);
    SetEnvironmentVariableA(name, val);
    _putenv(buf);
    HMODULE uc = GetModuleHandleA("ucrtbase.dll");
    if (!uc) uc = LoadLibraryA("ucrtbase.dll");
    if (uc) {
        int (*uc_putenv)(const char*) = (int(*)(const char*))GetProcAddress(uc, "_putenv");
        if (uc_putenv) uc_putenv(buf);
        int (*uc_putenv_s)(const char*,const char*) = (int(*)(const char*,const char*))GetProcAddress(uc, "_putenv_s");
        if (uc_putenv_s) uc_putenv_s(name, val);
    }
}

/* ── After Burner Climax (Rev A) segment map ─────────────────────────────
 * seg[3] va=0x82e6000, sz=0x1ddcf80, end=0x0a0c2f80  rw- (data + BSS)
 * Actual mapped range - anything outside this is unmapped and will fault.
 */
#define BSS_LO  0x082e6000U
#define BSS_HI  0x0a0c2f80U

/* Hardware-ready flags - within seg[3], set by patcher and confirmed here */
#define HWREADY_FLAGS_ADDR  0x09571594U

/* ── BSS write helpers ───────────────────────────────────────────────────*/
static void bss_write8(uint32_t va, uint8_t val)
{
    if (va < BSS_LO || va >= BSS_HI) {
        loader_log(LOG_WARN, "[PREINIT] bss_write8: 0x%08x outside mapped segment [0x%08x-0x%08x], skipping",
                   va, BSS_LO, BSS_HI);
        return;
    }
    /* BSS_LO/HI are ABC's data+BSS extents. Other Lindbergh titles (e.g. Let's
     * Go Jungle) have a DIFFERENT layout, so an ABC-specific poke address can
     * fall inside [BSS_LO,BSS_HI] yet land on a page this title never committed
     * -> a raw write faults in loader code (crash at RIP in lindbergh_preinit
     * accessing the bogus ELF addr). Guard the write: only poke COMMITTED,
     * writable guest memory; skip (don't crash) otherwise. Harmless for ABC
     * (its flag page IS committed). */
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)va, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        loader_log(LOG_WARN, "[PREINIT] bss_write8: 0x%08x not committed for this title, skipping (ABC-specific poke)", va);
        return;
    }
    DWORD old = 0;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        VirtualProtect((void*)(uintptr_t)va, 1, PAGE_READWRITE, &old);
    *(volatile uint8_t*)(uintptr_t)va = val;
    if (old) VirtualProtect((void*)(uintptr_t)va, 1, old, &old);
}

/* ── Public entry point ──────────────────────────────────────────────────*/
void lindbergh_preinit(void)
{
    loader_log(LOG_INFO, "[PREINIT] === Lindbergh pre-init ===");
    loader_log(LOG_INFO, "[PREINIT] BSS range: 0x%08x - 0x%08x", BSS_LO, BSS_HI);

    /* Hardware-ready flags (0x09571594-0x09571596) - within seg[3] ✓
     * Written by patcher (dongle bypass), confirmed here.
     * The game display callback gates on these being non-zero. */
    bss_write8(HWREADY_FLAGS_ADDR,     1);
    bss_write8(HWREADY_FLAGS_ADDR + 1, 1);
    bss_write8(HWREADY_FLAGS_ADDR + 2, 1);
    loader_log(LOG_INFO, "[PREINIT] Hardware-ready flags set @ 0x%08x", HWREADY_FLAGS_ADDR);

    /* The game does NOT expect its DIP/hardware state pre-written into BSS.
     * It reads that state at runtime by polling port 0x38 (IN instruction) and
     * via ioctl on /dev/lbb and /dev/i2c/0. Install the board emulators that
     * answer those, instead of poking unmapped BSS (the old 0x0a0ca168 write).
     * This is the mechanism lindbergh-loader uses on Linux. */
    lindbergh_boards_install();
    loader_log(LOG_INFO, "[PREINIT] Lindbergh boards installed (security/baseboard/eeprom)");

    /* Shader directory check */
    {
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        loader_log(LOG_INFO, "[PREINIT] Working dir: %s", cwd);

        char shader_dir[MAX_PATH];
        snprintf(shader_dir, sizeof(shader_dir), "%s\\shader\\asm", cwd);
        DWORD attr = GetFileAttributesA(shader_dir);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            loader_log(LOG_INFO, "[PREINIT] Shader dir OK: %s", shader_dir);
        } else {
            loader_log(LOG_WARN, "[PREINIT] WARNING: Shader dir missing: %s", shader_dir);
            loader_log(LOG_WARN, "[PREINIT] Fix: mklink /J disk0\\shader disk1\\shader");
        }
    }

    /* ROM directory check */
    {
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        char rom_dir[MAX_PATH];
        snprintf(rom_dir, sizeof(rom_dir), "%s\\rom", cwd);
        DWORD attr = GetFileAttributesA(rom_dir);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            loader_log(LOG_INFO, "[PREINIT] ROM dir OK: %s", rom_dir);
        } else {
            loader_log(LOG_WARN, "[PREINIT] WARNING: ROM dir missing: %s", rom_dir);
        }
    }

    /* Temp directories */
    {
        char tmp[MAX_PATH];
        GetTempPathA(sizeof(tmp), tmp);
        char lindy_tmp[MAX_PATH], segaboot[MAX_PATH];
        snprintf(lindy_tmp, sizeof(lindy_tmp), "%slindbergh", tmp);
        snprintf(segaboot,  sizeof(segaboot),  "%slindbergh\\segaboot", tmp);
        CreateDirectoryA(lindy_tmp, NULL);
        CreateDirectoryA(segaboot,  NULL);
        loader_log(LOG_INFO, "[PREINIT] Temp dirs: %s", lindy_tmp);
    }

    /* DISPLAY env var for fake X11 server */
    SetEnvironmentVariableA("DISPLAY", "127.0.0.1:0");
    loader_log(LOG_INFO, "[PREINIT] DISPLAY=127.0.0.1:0");

    /* Re-assert the GL driver selection (main.c chose it first; mirror here
     * before the GL context is created). Default llvmpipe (software);
     * ULTRA_GL_DRIVER=zink = hardware GL-on-Vulkan (AMD/Nvidia/Intel); =d3d12 =
     * DirectX12 backend (deadlocks on this game). Explicit GALLIUM_DRIVER wins.
     * Mesa reads getenv against ucrtbase's CRT snapshot, so poke that too. For
     * hardware backends disable the glthread worker (thread-affine WGL hazard). */
    {
        char cur[64];
        const char *drv;
        if (GetEnvironmentVariableA("GALLIUM_DRIVER", cur, sizeof cur) > 0) {
            drv = cur;
        } else {
            drv = getenv("ULTRA_GL_DRIVER");
            if (!drv || !*drv) drv = "llvmpipe";
            ul_preinit_set_env("GALLIUM_DRIVER", drv);
        }
        int is_hw = (!_stricmp(drv, "zink") || !_stricmp(drv, "d3d12"));
        if (is_hw) {
            ul_preinit_set_env("mesa_glthread", "false");
            ul_preinit_set_env("MESA_GLTHREAD",  "false");
            if (!_stricmp(drv, "d3d12")) ul_preinit_set_env("GALLIUM_THREAD", "0");
        }
        loader_log(LOG_INFO, "[PREINIT] GALLIUM_DRIVER=%s%s", drv, is_hw ? " (hardware; glthread off)" : "");
    }

    loader_log(LOG_INFO, "[PREINIT] === Pre-init complete ===");
}
