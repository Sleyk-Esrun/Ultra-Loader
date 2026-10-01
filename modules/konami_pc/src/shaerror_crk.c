/* shaerror_crk.c -- silent stub for Konami's shaerror.dll (Silent Hill Arcade)
 *
 * shaerror.dll is the SHA (Silent Hill Arcade) error/I/O check library.
 * It exports ONE function: shaerror_get_errdata(void *buf, const char *key).
 *
 * The real DLL enumerates the GHL I/O board (VID 06D9 / PID 0220) via SetupAPI
 * dynamically (LoadLibraryA + GetProcAddress), fills an internal table, and
 * when called from KSHG.exe:
 *   - returns -1 if the board is found (copies 0x4B8-byte device state to buf)
 *   - returns  0 if no board found → KSHG.exe enters error mode, which shows
 *     ALL inputs as stuck ON (DOWN + TRIGGER permanently held), making the
 *     gray I/O-precheck screen display stuck inputs and causing the service
 *     menu to auto-scroll indefinitely.
 *
 * Since the real shaerror.dll does its own SetupAPI board detection via its
 * own IAT (not KSHG.exe's IAT), our IAT hooks in shaiolib_crk.c do NOT reach
 * it.  The only clean fix is to replace shaerror.dll with this stub.
 *
 * We return -1 (board found) with a zero-filled buf (all inputs not pressed).
 * KSHG.exe sees a connected board with clean I/O state and proceeds normally.  */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v)
{ (void)h; (void)r; (void)v; return TRUE; }

/* THIS STUB HAS BEEN SILENT SINCE IT WAS WRITTEN, AND THAT IS A PROBLEM.
 *
 * Its header describes the exact failure the owner is hearing -- "ALL inputs as
 * stuck ON (DOWN + TRIGGER permanently held) ... causing the service menu to
 * auto-scroll indefinitely" -- but nothing here has ever recorded whether the
 * game calls this function, how often, with which key, or what we answer. A
 * whole session was spent guessing at a repeating input while the one function
 * documented to cause exactly that symptom logged nothing at all.
 *
 * Log-only. Behavior is unchanged: still memset + return -1. */
static void she_log(const char *fmt, ...)
{
    static FILE *f = (FILE *)-1;          /* -1 = not yet resolved */
    va_list ap;
    if (f == (FILE *)-1) {
        char path[MAX_PATH] = {0};
        const char *p = getenv("ULTRA_JVS_LOG");
        if (p && *p) {
            _snprintf(path, MAX_PATH - 1, "%s", p);
        } else {
            char local[MAX_PATH] = {0};
            DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local);
            if (got && got < sizeof local)
                _snprintf(path, MAX_PATH - 1, "%s\\Ultra Loader\\shaerror.log", local);
        }
        f = path[0] ? fopen(path, "a") : NULL;
    }
    if (!f) return;
    fprintf(f, "[%6lu] shaerror: ", (unsigned long)GetTickCount());
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/* Size of the device-state struct the real shaerror.dll copies into buf.
 * Determined from the body at 0x10001000: rep movsl with ecx=0x12e dwords. */
#define SHAERR_DEVSTATE_SIZE  0x4B8  /* 1208 bytes */

__declspec(dllexport)
int __cdecl shaerror_get_errdata(void *buf, const char *key)
{
    static unsigned long n = 0;
    ++n;
    /* Log the first few in full, then every 30th, so a repeating call shows its
     * cadence without flooding. The KEY matters: the real library answers
     * different keys differently, and this stub answers every one identically --
     * if the game asks something we should not be saying -1 to, this names it. */
    if (n <= 8 || (n % 30) == 0)
        she_log("get_errdata #%lu key=\"%s\" buf=%p -> -1 (board present, inputs zeroed)",
                n, key ? key : "(null)", buf);

    /* Zero-fill: all input bits 0 = no buttons pressed, axes neutral.
     * Return -1 to tell KSHG.exe "board present, data valid" so it does
     * NOT enter error mode.  Returning 0 means "no board" -> error mode. */
    if (buf) memset(buf, 0, SHAERR_DEVSTATE_SIZE);
    return -1;
}
