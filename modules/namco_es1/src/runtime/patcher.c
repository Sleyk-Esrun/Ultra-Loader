/* SPDX-License-Identifier: MIT
 * patcher.c -- per-title binary patch pipeline, System N2 edition.
 *
 * Lindbergh's own patcher.c hardcodes dongle-bypass/community-patch logic
 * for specific Lindbergh titles (After Burner Climax, Ghost Squad Evolution,
 * Hummer Extreme Edition, ...) -- none of that applies here. No System N2
 * title has a known patch requirement yet (no dump on hand to reverse one
 * against), so this is an honest no-op: reports success with zero patches
 * applied. Extend this file (not lindbergh's) the same way Lindbergh's own
 * patcher.c grew, once a real N2 title needs a specific byte patch.
 */
#include <string.h>
#include "patcher.h"

PatchResult patcher_run(const char *elf_path, uint8_t *image, size_t image_size,
                         PatchOptions opts)
{
    (void)elf_path; (void)image; (void)image_size; (void)opts;
    PatchResult result;
    memset(&result, 0, sizeof result);
    result.success = true;
    result.patches_applied = 0;
    return result;
}

bool patcher_restore(const char *elf_path)
{
    (void)elf_path;
    return true;
}

void patcher_dry_run(const char *elf_path)
{
    (void)elf_path;
}
