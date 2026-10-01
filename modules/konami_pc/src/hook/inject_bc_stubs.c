/* Stubs for boot_console.c symbols not needed in the standalone injector.
 * The injector doesn't use embedded resources or the audio gate; these no-ops
 * satisfy the linker without pulling in the full loader asset subsystem. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Attempt disk font load only (no embedded fallback in the injector). */
int asset_register_font(const char *rel_disk_path, const char *resource_name,
                        char *out_disk, size_t disk_cap, HANDLE *out_mem)
{
    (void)resource_name;
    if (out_disk && disk_cap > 0 && rel_disk_path) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *sl = strrchr(dir, '\\'); if (sl) *sl = 0;
        int n = snprintf(out_disk, disk_cap, "%s\\%s", dir, rel_disk_path);
        if (n > 0 && (size_t)n < disk_cap) {
            if (GetFileAttributesA(out_disk) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    if (out_disk && disk_cap > 0) out_disk[0] = 0;
    if (out_mem) *out_mem = NULL;
    return 0;
}

/* Audio gate: the injector has no game audio to gate. */
volatile int g_ul_audio_gated = 0;
