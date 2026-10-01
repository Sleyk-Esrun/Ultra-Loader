/* SPDX-License-Identifier: MIT
 * board_io_stub.c -- baseboard/EEPROM device stubs, System N2 edition.
 *
 * Lindbergh's vfs.c routes /dev/lbb and /dev/i2c/0 opens to lindbergh_boards.c
 * (real Sega baseboard + EEPROM emulation) -- that's genuinely Sega-Lindbergh
 * hardware, not System N2's. This board has no real baseboard/EEPROM protocol
 * reversed yet (no dump on hand), so these are honest not-supported stubs:
 * the guest's ioctl/read/write on these paths fails cleanly instead of
 * silently returning fabricated Sega-shaped data.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "omniwheel_service.h"

/* audio_engine.c checks this to gate output while the (not-yet-built) pause
 * menu is open; stubs_glibc.c reads this as the (not-yet-built) snapshots
 * feature's enabled flag. Both honest zero/off until those features exist
 * for this board. */
volatile int g_ul_audio_gated = 0;
int          g_ul_snapshots   = 0;

/* Ultra Omni-Wheel: not wired for this board yet (no real wheel-shaped N2
 * title identified). Honest "not present" so ftdi_shim.c's wheel-forwarding
 * path stays inert. */
int omniwheel_get_state(unsigned player, omniwheel_state_t *out)
{
    (void)player;
    if (out) { memset(out, 0, sizeof *out); }
    return 0;
}

int64_t eeprom_ioctl(uint64_t request, void *argp)
{
    (void)request; (void)argp;
    return -1;
}

int64_t lbb_read(void *buf, size_t count)
{
    (void)buf; (void)count;
    return -1;
}

int64_t lbb_write(const void *buf, size_t count)
{
    (void)buf; (void)count;
    return -1;
}

int64_t lbb_ioctl(uint64_t request, void *argp)
{
    (void)request; (void)argp;
    return -1;
}

/* JVS I/O: real N2 JVS protocol not reversed yet (no dump on hand). Same
 * honest not-supported stub as the baseboard/EEPROM paths above. */
int64_t jvs_read(void *buf, size_t count)
{
    (void)buf; (void)count;
    return -1;
}

int64_t jvs_write(const void *buf, size_t count)
{
    (void)buf; (void)count;
    return -1;
}

/* OpenGL: real N2 render pipeline not built yet (rendering is future work,
 * once real I/O exists to test against). "Not a GL library" so the guest's
 * dlopen falls through to the normal path instead of getting a fake handle
 * with nothing behind it. */
bool libgl_is_gl_library(const char *filename)
{
    (void)filename;
    return false;
}

void *libgl_dlopen(const char *filename)
{
    (void)filename;
    return NULL;
}

void *libgl_dlsym(void *handle, const char *symbol)
{
    (void)handle; (void)symbol;
    return NULL;
}

void *libgl_resolve(const char *name)
{
    (void)name;
    return NULL;
}
