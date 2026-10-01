/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  gearpad_europar.c -- Sega Europa-R: XInput gamepad -> keyboard gear shift.
 * =============================================================================
 *  Ford Racing (Full Blown) reads STEER/PEDALS from the emulated HAPP wheel,
 *  but its GEAR UP / GEAR DOWN come from the KEYBOARD (a separate input path,
 *  verified live: the wheel report carries no shift channel). This maps the
 *  pad's shoulder buttons to the game's keyboard shift keys via SendInput --
 *  the same real-OS-keyboard mechanism the Ultra Omni-Bridge uses -- so the
 *  keystroke reaches the game exactly as a physical keypress would.
 *
 *  Keys are configurable at runtime (no rebuild):
 *     ULTRA_EUROPAR_GEARUP_VK    virtual-key sent by RB   (default 0 = off)
 *     ULTRA_EUROPAR_GEARDOWN_VK  virtual-key sent by LB   (default 0 = off)
 *
 *  Because the arcade build's exact shift keys aren't documented, a built-in
 *  finder pins them down in one manual race with zero guessing:
 *     ULTRA_EUROPAR_GEARSCAN=1
 *  In scan mode, each RB press sends the NEXT candidate key (and logs it to
 *  ULTRA_JVS_LOG); LB resets to the start. Hold a manual race, tap RB until the
 *  gear shifts, and the last logged key is your GEAR UP. Set that VK in
 *  ULTRA_EUROPAR_GEARUP_VK (and the neighbor for GEAR DOWN) and you're done.
 * ============================================================================= */
#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static void gplog(const char *fmt, ...) {
    const char *p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE *f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* SendInput a full key press+release (VK + its scan code), like the Omni-Bridge. */
static void gp_key_tap(int vk) {
    if (vk <= 0) return;
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    INPUT in[2]; ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = (WORD)sc; in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(2, in, sizeof(INPUT));
    Sleep(40);
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk; in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = (WORD)sc; in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

typedef BOOL (WINAPI *pfnXIGetState)(DWORD, void*);
static pfnXIGetState s_xi;
#pragma pack(push,1)
typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } gp_pad;
typedef struct { DWORD pkt; gp_pad g; } gp_state;
#pragma pack(pop)
#define GP_LB 0x0100
#define GP_RB 0x0200

/* Candidate keys tried by the scanner, ordered most-likely-first for an arcade
 * sequential shifter. VK codes with human labels for the log. */
static const struct { int vk; const char *name; } g_scan[] = {
    {0xBE,"."},{0xBC,","},{0xDB,"["},{0xDD,"]"},{0xBF,"/"},{0xDC,"\\"},{0xBA,";"},{0xDE,"'"},
    {0xBD,"-"},{0xBB,"="},{0xC0,"` (backtick)"},{0x09,"Tab"},
    {0x51,"Q"},{0x45,"E"},{0x52,"R"},{0x54,"T"},{0x59,"Y"},{0x55,"U"},{0x49,"I"},{0x4F,"O"},{0x50,"P"},
    {0x46,"F"},{0x47,"G"},{0x48,"H"},{0x4A,"J"},{0x4B,"K"},{0x4C,"L"},
    {0x43,"C"},{0x56,"V"},{0x42,"B"},{0x4E,"N"},{0x4D,"M"},
    {0x31,"1"},{0x32,"2"},{0x33,"3"},{0x34,"4"},{0x30,"0"},
    {0x21,"PageUp"},{0x22,"PageDown"},{0x24,"Home"},{0x23,"End"},{0x2D,"Insert"},{0x2E,"Delete"},
    {0xA1,"RShift"},{0xA0,"LShift"},{0xA2,"LCtrl"},
    {0x6B,"Numpad+"},{0x6D,"Numpad-"},{0x60,"Numpad0"},{0x61,"Numpad1"},{0x62,"Numpad2"},
    {0x70,"F1"},{0x71,"F2"},{0x72,"F3"},{0x73,"F4"},
};
#define GP_SCAN_N ((int)(sizeof(g_scan)/sizeof(g_scan[0])))

static volatile LONG g_gp_stop = 0;

static DWORD WINAPI gearpad_thread(LPVOID arg) {
    (void)arg;
    const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    for (int i = 0; dlls[i] && !s_xi; ++i) { HMODULE h = LoadLibraryA(dlls[i]); if (h) s_xi = (pfnXIGetState)(void*)GetProcAddress(h, "XInputGetState"); }
    if (!s_xi) { gplog("GEARPAD: XInput unavailable -- gear-pad inert"); return 0; }

    const char *eu = getenv("ULTRA_EUROPAR_GEARUP_VK");
    const char *ed = getenv("ULTRA_EUROPAR_GEARDOWN_VK");
    int gup = eu ? (int)strtol(eu, NULL, 0) : 0;
    int gdn = ed ? (int)strtol(ed, NULL, 0) : 0;
    const char *sc = getenv("ULTRA_EUROPAR_GEARSCAN");
    /* Explicit 1 = scan; explicit 0 = never scan; unset = scan only until keys
     * are configured (so it "just works" out of the box as a finder). */
    int scan = sc ? (sc[0]=='1') : (gup<=0 && gdn<=0);
    gplog("GEARPAD: active (scan=%d gearup_vk=0x%02X geardown_vk=0x%02X); RB=up LB=down",
          scan, gup, gdn);

    int lb_prev = 0, rb_prev = 0, idx = 0, announced = 0;
    while (!g_gp_stop) {
        gp_state st; ZeroMemory(&st, sizeof st);
        if (s_xi(0, &st) == 0) {
            int lb = (st.g.b & GP_LB) != 0, rb = (st.g.b & GP_RB) != 0;
            if (scan) {
                if (!announced) { announced = 1; gplog("GEARPAD SCAN ready: tap RB to try the next key, LB to restart. First key: %s (VK 0x%02X)", g_scan[0].name, g_scan[0].vk); }
                if (rb && !rb_prev) {
                    gplog("GEARPAD SCAN -> sending key [%d/%d] %s (VK 0x%02X)  <-- if the gear moved, THIS is it",
                          idx+1, GP_SCAN_N, g_scan[idx].name, g_scan[idx].vk);
                    gp_key_tap(g_scan[idx].vk);
                    idx = (idx + 1) % GP_SCAN_N;
                }
                if (lb && !lb_prev) { idx = 0; gplog("GEARPAD SCAN: reset to first key (%s)", g_scan[0].name); }
            } else {
                if (rb && !rb_prev && gup > 0) gp_key_tap(gup);
                if (lb && !lb_prev && gdn > 0) gp_key_tap(gdn);
            }
            lb_prev = lb; rb_prev = rb;
        }
        Sleep(15);
    }
    return 0;
}

void gearpad_europar_install(void) {
    HANDLE h = CreateThread(NULL, 0, gearpad_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
void gearpad_europar_stop(void) { InterlockedExchange(&g_gp_stop, 1); }
#endif /* _WIN32 */
