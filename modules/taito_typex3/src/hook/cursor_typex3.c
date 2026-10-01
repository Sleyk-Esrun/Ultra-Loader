/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  cursor_typex3.c -- keep the mouse pointer visible on an arcade touch title
 * =============================================================================
 *
 *  Theatrhythm's menus are driven by the TOUCH PANEL, and on the real cabinet
 *  the player's finger is the pointer, so the game hides the Windows cursor
 *  and never puts it back. On a PC that leaves the player aiming an invisible
 *  pointer at a touch menu: the input works, but nothing shows where it is.
 *
 *  WHY A WINDOW SUBCLASS AND NOT JUST ShowCursor(TRUE).
 *
 *  Two different mechanisms hide a cursor and they have to be answered
 *  separately:
 *
 *    1. The display COUNT. ShowCursor(FALSE) decrements a per-input-queue
 *       counter and the cursor is drawn only while it is >= 0. Unity drops it
 *       once at startup, so pushing it back up once is enough -- but it can be
 *       dropped again on a mode change, so it is re-checked.
 *
 *    2. WM_SETCURSOR. Every time the pointer moves over the client area
 *       Windows asks the window which cursor to draw, and Unity answers by
 *       calling SetCursor(NULL) -- no cursor. That happens on every mouse
 *       move, so a periodic SetCursor from another thread loses the race
 *       roughly always. The only reliable answer is to handle the message:
 *       we set the arrow and return TRUE, which stops the game's own handler
 *       from unsetting it.
 *
 *  Everything else is passed straight through to the original window
 *  procedure, so this changes nothing but which cursor is drawn.
 *
 *  DELIBERATELY NOT DONE HERE: moving the pointer. ultra_touch.c injects touch
 *  at the cursor's CURRENT position, and anything that also warps the cursor
 *  turns that into a feedback loop. This file only ever decides how the
 *  pointer is DRAWN.
 *
 *  ULTRA_CURSOR=0 disables.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "taito_typex3_hook.h"

/* Always log somewhere. The first version only wrote when ULTRA_JVS_LOG was
 * set, so when this file silently did nothing there was no way to tell
 * whether it had failed or had never run -- which cost a whole test cycle.
 * Default to %TEMP% alongside the other stand-in logs. */
static void cursor_log(const char* fmt, ...)
{
    static char path[MAX_PATH];
    const char* p = getenv("ULTRA_JVS_LOG");
    FILE* f;
    va_list ap;
    if (!p || !*p) {
        if (!path[0]) {
            DWORD n = GetTempPathA(sizeof path, path);
            if (!n || n >= sizeof path) return;
            strncat(path, "ultra_cursor.log", sizeof path - strlen(path) - 1);
        }
        p = path;
    }
    f = fopen(p, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static HCURSOR  g_arrow = NULL;
static volatile LONG g_stop = 0;

/* THE ORIGINAL WNDPROC IS STORED ON THE WINDOW, NOT IN A GLOBAL.
 *
 * The first version kept one global g_prev. That is correct only while exactly
 * one window is ever subclassed, and this process has more than one: the log
 * from a real run showed two attachments, the game's and one of Ultra Loader's
 * own. The second overwrote g_prev, so messages arriving at the FIRST window
 * were then forwarded to the SECOND window's procedure -- a different window's
 * code, run against the wrong window, for every message the game received.
 *
 * A window property is the right home for it: it belongs to the window, it
 * cannot be clobbered by attaching to a second one, and it lets the procedure
 * work out where to forward from the handle it is given. It also removes the
 * startup race the global had, because the property is published BEFORE the
 * procedure is installed rather than after. */
static const char PROP_PREV[] = "UltraCursorPrevProc";

static LRESULT CALLBACK cursor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC prev = (WNDPROC)GetPropA(h, PROP_PREV);
    int uni = IsWindowUnicode(h) ? 1 : 0;

    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        /* The pointer is over the game's own client area, which is the only
         * place the game hides it. Answer the message ourselves: set the
         * arrow and report handled, so the game's handler never runs and
         * cannot call SetCursor(NULL) behind us. Outside the client area
         * (title bar, borders, resize edges) fall through -- those cursors
         * belong to the window manager and are already correct. */
        SetCursor(g_arrow);
        return TRUE;
    }

    if (msg == WM_NCDESTROY) {
        /* Last message a window ever gets. Put its own procedure back and drop
         * the property, so a window that is destroyed and recreated (Unity
         * does this on a resolution or fullscreen change) does not leave a
         * dangling subclass behind for the next one to inherit. */
        if (prev) {
            if (uni) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)prev);
            else     SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)prev);
        }
        RemovePropA(h, PROP_PREV);
    }

    if (!prev)
        return uni ? DefWindowProcW(h, msg, wp, lp)
                   : DefWindowProcA(h, msg, wp, lp);
    return uni ? CallWindowProcW(prev, h, msg, wp, lp)
               : CallWindowProcA(prev, h, msg, wp, lp);
}

/* SUBCLASS A WINDOW WITH THE VARIANT IT WAS REGISTERED WITH.
 *
 * Every window is either ANSI or Unicode, decided by whether its class went in
 * through RegisterClassA or RegisterClassW, and SetWindowLongPtrA/W does more
 * than pick a name: calling the A variant on a Unicode window CONVERTS it to
 * ANSI, and from then on Windows transcodes every text-bearing message across
 * the boundary. Unity registers its window as Unicode. An early version of
 * this file used the A variants unconditionally, which silently re-labeled
 * the game's own window and corrupted message parameters passed through it.
 *
 * IsWindowUnicode() asks the window itself rather than assuming, so this is
 * correct for any host, and the matching CallWindowProcW/A keeps the chain
 * consistent on the way back out. */
static void attach(HWND w)
{
    WNDPROC prev;
    int uni;
    char cls[64];

    if (!w) return;
    if (GetPropA(w, PROP_PREV)) return;          /* already ours */

    uni = IsWindowUnicode(w) ? 1 : 0;
    prev = uni ? (WNDPROC)GetWindowLongPtrW(w, GWLP_WNDPROC)
               : (WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC);
    if (!prev) {
        cursor_log("CURSOR: cannot read wndproc of %p (err %lu)", (void*)w,
                   (unsigned long)GetLastError());
        return;
    }
    /* Publish the forwarding target BEFORE the procedure goes live, so a
     * message dispatched the instant after the swap already has somewhere to
     * go. The old ordering left a window in which cursor_proc was installed
     * and the pointer to the original was still unset. */
    if (!SetPropA(w, PROP_PREV, (HANDLE)prev)) {
        cursor_log("CURSOR: SetProp failed on %p (err %lu)", (void*)w,
                   (unsigned long)GetLastError());
        return;
    }
    SetLastError(0);
    if (uni) SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)cursor_proc);
    else     SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)cursor_proc);
    if (GetLastError() != 0) {
        RemovePropA(w, PROP_PREV);
        cursor_log("CURSOR: subclass failed on %p (err %lu)", (void*)w,
                   (unsigned long)GetLastError());
        return;
    }
    if (!GetClassNameA(w, cls, sizeof cls)) cls[0] = 0;
    cursor_log("CURSOR: visible pointer installed on window %p (%s, class '%s')",
               (void*)w, uni ? "unicode" : "ansi", cls);
}

/* FIND THE GAME'S WINDOW WITHOUT ASKING THE RENDERER.
 *
 * This used to call t3_d3d9_get_window(), which is filled in when the game
 * creates a Direct3D 9 device. This game imports OPENGL32.dll -- it never
 * creates one, so that getter returned NULL forever and this whole file did
 * nothing at all while appearing to be installed.
 *
 * A window does not need the renderer to be found: enumerate the top-level
 * windows this process owns and take the biggest visible one. That is the game
 * window under D3D9, D3D11, OpenGL or anything else, which is the point --
 * a cursor fix should not depend on which graphics API a title happens to use.
 */
typedef struct { HWND best; LONG area; } wndpick;

static BOOL CALLBACK pick_window(HWND h, LPARAM lp)
{
    wndpick* w = (wndpick*)lp;
    DWORD pid = 0;
    RECT r;
    LONG a;
    char cls[64];

    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;      /* tool/dialog, not the main window */

    /* SKIP ULTRA LOADER'S OWN WINDOWS.
     *
     * The hook runs INSIDE the game process, and so do several of the loader's
     * own windows -- the in-game overlay, the boot console, the credit pill.
     * "Biggest visible top-level window this process owns" therefore describes
     * them too, and a real run picked the overlay and subclassed it alongside
     * the game. Every class this project registers is named "Ultra<something>",
     * so the family name is the filter. */
    if (GetClassNameA(h, cls, sizeof cls) && !strncmp(cls, "Ultra", 5))
        return TRUE;

    if (!GetWindowRect(h, &r)) return TRUE;
    a = (r.right - r.left) * (r.bottom - r.top);
    if (a > w->area) { w->area = a; w->best = h; }
    return TRUE;
}

static HWND find_game_window(void)
{
    wndpick w;
    w.best = NULL;
    w.area = 0;
    EnumWindows(pick_window, (LPARAM)&w);
    return w.best;
}

static DWORD WINAPI cursor_thread(LPVOID arg)
{
    int guard;
    int waited = 0;
    (void)arg;
    g_arrow = LoadCursorA(NULL, IDC_ARROW);
    cursor_log("CURSOR: watcher started (looking for this process's own window)");
    while (!g_stop) {
        HWND w = find_game_window();
        if (!w && waited >= 0 && ++waited == 40)
            cursor_log("CURSOR: still no window after 10s -- nothing to attach to");
        if (w && waited >= 0) waited = -1;
        attach(w);
        /* Push the display count non-negative. ShowCursor returns the new
         * count, so this stops as soon as the cursor is showable; the guard
         * is only there so a pathological count can never spin forever. */
        for (guard = 0; guard < 16 && ShowCursor(TRUE) < 0; ++guard) { }
        Sleep(250);
    }
    return 0;
}

void ultra_cursor_install(void)
{
    char v[8];
    static int once = 0;
    if (once) return;
    once = 1;
    if (GetEnvironmentVariableA("ULTRA_CURSOR", v, sizeof v) && v[0] == (char)48) {
        cursor_log("CURSOR: disabled (ULTRA_CURSOR=0)");
        return;
    }
    /* A thread, because the window does not exist yet at DLL_PROCESS_ATTACH
     * and nothing may block under the loader lock. */
    CloseHandle(CreateThread(NULL, 0, cursor_thread, NULL, 0, NULL));
}

void ultra_cursor_stop(void)
{
    InterlockedExchange(&g_stop, 1);
}

#endif /* _WIN32 */
