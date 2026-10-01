/*
 * ftdi_shim.h
 * Ultra Loader - FTDI FT4232H virtual-serial shim for Cruis'n Blast.
 *
 * Cruis'n Blast (Raw Thrills G5, Lindbergh-adjacent Linux ELF32 title) reaches
 * its wheel / pedal / gearbox and its coin / start / service boards through an
 * FT4232H USB-to-quad-serial bridge (VID:0403 PID:6015). The Linux driver
 * exposes the two "arcade I/O channels" as /dev/ttyUSB*, but the game's
 * bootstrap doesn't use raw ttyUSB names - it uses libudev to enumerate the
 * "tty" subsystem, matches on the FT4232H product string, then rewrites those
 * device nodes to symlinks named /dev/rio2.<SERIAL> and /dev/uhb.<SERIAL> via
 * a udev rules file that ships with the cabinet image. The game then opens
 * those symlinks, calls termios (TCGETS/TCSETS), and starts framing.
 *
 *   RIO2 = Raw thrills I/O 2 - steering-wheel encoder, brake/gas pedals,
 *          gear paddles, force-feedback commands, view/horn buttons.
 *   UHB  = Universal Harness Board - coin acceptor, start button, service
 *          switch, test switch, ticket dispenser status.
 *
 * This shim materialises both channels as pure software:
 *
 *   1. libudev is intercepted at dlopen()/dlsym() (via stubs_glibc.c's
 *      dispatcher). udev_enumerate_add_match_subsystem("tty") returns exactly
 *      two synthetic entries - rio2 + uhb - with product string, serial,
 *      VID/PID, and a synthetic sysfs devnode ("/dev/rio2.<SERIAL>" and
 *      "/dev/uhb.<SERIAL>").
 *
 *   2. open("/dev/rio2.*") and open("/dev/uhb.*") allocate a synthetic FD
 *      backed by a Win32 socketpair (a pair of localhost anonymous sockets
 *      wrapped as an FD). A background thread produces protocol frames onto
 *      the "device" side; the game reads from the "host" side.
 *
 *   3. ioctl(TCGETS/TCSETS/TCSETSW/TIOCMGET/…) on the synthetic FD returns
 *      success no-ops. Termios settings are honored only symbolically -
 *      the protocol framer runs at a fixed 100 Hz regardless of the
 *      baud the game requests.
 *
 *   4. The RIO2 producer thread reads the loader's OmniWheel service state
 *      via omni_client.h and emits per-frame wheel-position / pedal /
 *      gear frames in Raw Thrills' RIO2 v1 protocol. If OmniWheel reports
 *      no wheel present, it emits centered/null frames.
 *
 *   5. The UHB producer thread emits status heartbeats - "coin OK, service
 *      OK, ticket dispenser present, everything nominal". The loader's
 *      overlay layer (`ULTRA_FREEPLAY` etc) synthesises coin drops.
 *
 * PUBLIC API
 * ----------
 *   ftdi_shim_init()             - one-shot startup, seeds the socketpairs,
 *                                  spawns producer threads, registers the
 *                                  device paths so vfs.c's open() dispatcher
 *                                  can route to us.
 *   ftdi_shim_shutdown()         - teardown.
 *   ftdi_shim_dlopen(name, flags)- called from stubs_glibc.c when a game
 *                                  dlopen()s libftd2xx.so / libftdi1.so /
 *                                  libudev.so; returns a fake non-NULL
 *                                  handle the game caches.
 *   ftdi_shim_lookup(sym)        - called from stubs_glibc.c's dlsym; returns
 *                                  a pointer to a static shim implementation
 *                                  of the requested symbol (or NULL to fall
 *                                  through to the loader's symbol table).
 *
 * PATH ROUTING (vfs.c integration)
 * ---------------------------------
 *   ftdi_shim_owns_path(path)    - true for "/dev/rio2.*" or "/dev/uhb.*".
 *   ftdi_shim_open(path, flags)  - allocate a synthetic FD; returns >=0 on
 *                                  success or -1 on failure.
 *   ftdi_shim_is_fd(fd)          - true if fd was minted by us.
 *   ftdi_shim_read/write/close/ioctl - synthetic FD ops.
 *
 * All identifiers are prefixed ftdi_shim_ so we don't collide with the
 * real libftd2xx D2XX or libudev symbol names, which are added to the
 * dlsym table under their canonical names by dispatch_lookup().
 */

#ifndef ULTRA_FTDI_SHIM_H
#define ULTRA_FTDI_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle -------------------------------------------------------------- */
void  ftdi_shim_init(void);
void  ftdi_shim_shutdown(void);

/* dlopen / dlsym dispatcher hooks (called from stubs_glibc.c) ------------ */
int   ftdi_shim_is_ftdi_library(const char *name);  /* libftd2xx / libftdi */
int   ftdi_shim_is_udev_library(const char *name);  /* libudev            */
void *ftdi_shim_dlopen(const char *name, int flags);
void *ftdi_shim_lookup(const char *symbol);

/* Path-routing hooks (called from vfs.c) --------------------------------- */
int   ftdi_shim_owns_path(const char *path);
int   ftdi_shim_open(const char *path, int flags);
int   ftdi_shim_is_fd(int fd);
long  ftdi_shim_read(int fd, void *buf, size_t count);
long  ftdi_shim_write(int fd, const void *buf, size_t count);
int   ftdi_shim_close(int fd);
long  ftdi_shim_ioctl(int fd, unsigned long req, void *arg);

/* Introspection (used by rt_g5 detection printouts + tests) -------------- */
int   ftdi_shim_channel_count(void);        /* always 2 (RIO2 + UHB) */
const char *ftdi_shim_channel_devnode(int idx);
const char *ftdi_shim_channel_serial (int idx);
const char *ftdi_shim_channel_product(int idx);

/* FT4232H USB IDs - the constants Cruis'n Blast matches against. Exposed for
 * unit tests + the udev enumeration synthesizer. */
#define FTDI_SHIM_VID          0x0403
#define FTDI_SHIM_PID_FT4232H  0x6011  /* stock FT4232H */
#define FTDI_SHIM_PID_RT       0x6015  /* Raw Thrills reflashed EEPROM PID */

/* Synthetic device serial numbers. Deterministic + logged at boot so a
 * user comparing against a real cabinet sees the same names. */
#define FTDI_SHIM_SERIAL_RIO2  "RIO2ULTRA0001"
#define FTDI_SHIM_SERIAL_UHB   "UHB0ULTRA0001"

#ifdef __cplusplus
}
#endif

#endif /* ULTRA_FTDI_SHIM_H */
