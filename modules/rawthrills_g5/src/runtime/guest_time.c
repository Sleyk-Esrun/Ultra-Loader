/*
 * guest_time.c - a single VIRTUAL monotonic clock that all guest time sources
 * (clock_gettime, gettimeofday, GLUT_ELAPSED_TIME) derive from.
 *
 * Why: Snapshots roll the game's memory back to a saved frame, but real wall-clock
 * keeps advancing (and a disk save/load takes seconds). If the game computes
 * dt = now - last_time from REAL time, after a restore dt explodes (the rolled-back
 * last_time is seconds in the past) and the game's logic breaks/hangs. By feeding
 * all clocks from one virtual base we can REWIND time on restore to the saved value,
 * so dt stays a normal per-frame delta.
 *
 * Normal operation is unchanged: virtual time == real elapsed since process start
 * (the game only uses deltas, so the absolute base is irrelevant). Only restore
 * shifts the offset.
 */
#include <windows.h>
#include <stdint.h>
#include <time.h>

static LARGE_INTEGER g_freq;
static int64_t       g_offset;        /* virtual_ticks = QPC - g_offset */
static int64_t       g_base_unix;     /* unix seconds at init (for gettimeofday) */
static int           g_init = 0;

static void vt_init(void)
{
    if (g_init) return;
    QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    g_offset = c.QuadPart;            /* virtual time starts at 0 */
    g_base_unix = (int64_t)time(NULL);
    g_init = 1;
}

static int64_t vt_ticks(void)
{
    vt_init();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return c.QuadPart - g_offset;
}

uint64_t vtime_ns(void)
{
    vt_init();
    return (uint64_t)((double)vt_ticks() * (1000000000.0 / (double)g_freq.QuadPart));
}

/* Snapshot hooks: capture the virtual time, and on restore resume from it so the
 * game sees no time gap across the rolled-back frames + the disk I/O. */
uint64_t vtime_get(void) { return vtime_ns(); }
void vtime_set(uint64_t ns)
{
    vt_init();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    g_offset = c.QuadPart - (int64_t)((double)ns * ((double)g_freq.QuadPart / 1000000000.0));
}

/* Wall clock for gettimeofday: a plausible time-of-day that tracks virtual time
 * (so it also rewinds on restore). */
void vtime_wall(int32_t *sec, int32_t *usec)
{
    uint64_t ns = vtime_ns();
    if (sec)  *sec  = (int32_t)(g_base_unix + (int64_t)(ns / 1000000000ULL));
    if (usec) *usec = (int32_t)((ns % 1000000000ULL) / 1000ULL);
}
