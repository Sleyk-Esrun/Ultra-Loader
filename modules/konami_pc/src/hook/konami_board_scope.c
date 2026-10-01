/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  konami_board_scope.c -- what this board deliberately does NOT carry
 * =============================================================================
 *
 *  konami_pc is DERIVED from native_x86, the same way every other board is: it
 *  takes the shared core it needs and leaves the rest behind. It is not a copy
 *  of native_x86 and it is not a rewrite.
 *
 *  WHAT IT TAKES
 *      jvs_core / jvs_serial_hook / board_io_ops / board_profile   JVS + serial
 *      konami_io                                                   Konami ACIO
 *      dinput8_emu                                                 DirectInput8
 *      controls / input_profiles / input_src_*                     input mapping
 *      input_autopilot                                             attract input
 *      d3d9_throttle                                               Konami PC is D3D9
 *      audio_hook / pause_menu_wire / omni_bridge / dongle_bypass
 *
 *  WHAT IT LEAVES BEHIND, and why
 *      sega_board / sega_io4 / sega_keychip     other vendor
 *      namco_io / namco_amdaemon / namco_dongle other vendor
 *      taito_fastio / typex2_gun                other vendor
 *      rawthrills_io                            other vendor
 *      card_reader                              NESiCA/Aime; Konami uses ICCA
 *                                               over ACIO instead
 *      d3d8_* / d3d11_* / gl_present            these titles are D3D9 only
 *      game_chaosbreaker / wackyrace / dsii     other boards' per-game files
 *      gamestate_probe                          Type X bring-up aid
 *
 *  The shared core still REFERENCES those vendors - that is what makes
 *  native_x86 the multi-board module - so a derived board has to answer for
 *  them. Answering "not on this board" here is the whole point of the
 *  derivation: the board carries its own decisions instead of eighteen files of
 *  another vendor's code that would never run.
 *
 *  Every one of these is inert on native_x86 too unless its env gate is set, so
 *  nothing observable changes; what changes is that this board no longer
 *  compiles, ships or has to be reasoned about with code it cannot use.
 *
 *  HISTORY: this board previously had NO input stack at all - 6 hook files,
 *  every input source dropped - which is why its games had no controls and why
 *  Silent Hill, which had run under native_x86, stopped working once it was
 *  routed here. Re-derived 2026-09-13.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- other vendors' I/O boards ------------------------------------------- */
void ultra_sega_io4_install(void)       { }
int  ultra_sega_io4_enabled(void)       { return 0; }
void ultra_sega_io4_insert_coin(int slot, int count) { (void)slot; (void)count; }
void ultra_sega_keychip_install(void)   { }
void ultra_taito_fastio_install(void)   { }
void ultra_rawthrills_io_install(void)  { }
void ultra_namco_io_install(void)       { }
void ultra_namco_amdaemon_install(void) { }

/* ---- Namco ES3 USB dongle: this board has its own protection DLLs --------- */
void    ultra_namco_dongle_install(void) { }
HANDLE  ultra_namco_dongle_open_device(void) { return INVALID_HANDLE_VALUE; }
int     ultra_namco_dongle_is_device_path(const char* path) { (void)path; return 0; }
int     ultra_namco_dongle_is_handle(HANDLE h) { (void)h; return 0; }
FARPROC ultra_namco_dongle_getproc(HMODULE mod, LPCSTR name) { (void)mod; (void)name; return NULL; }
BOOL    ultra_namco_dongle_ioctl(HANDLE h, DWORD code, LPVOID in_buf, DWORD in_sz,
                                 LPVOID out_buf, DWORD out_sz, LPDWORD bytes_ret,
                                 LPOVERLAPPED ov)
{
    (void)h; (void)code; (void)in_buf; (void)in_sz; (void)out_buf; (void)out_sz; (void)ov;
    if (bytes_ret) *bytes_ret = 0;
    return FALSE;
}

/* ---- card reader: Konami's is ICCA on the ACIO bus, not NESiCA/Aime ------- */
void ultra_card_reader_install(void) { }

/* ---- renderers this board does not use ----------------------------------- */
void ultra_d3d11_install(void)   { }
void ultra_d3d11_teardown(void)  { }
void ultra_d3d8_teardown(void)   { }
void ultra_d3d8_install_throttle(void* d3d8) { (void)d3d8; }
/* ---- the hook's diagnostic log ------------------------------------------
 *
 * Every file in this hook logs through ultra_d3d8_log (most of them via a
 * local `#define tlog ultra_d3d8_log`). On native_x86 the body lives in
 * d3d8_throttle.c, which this board does not link - so a no-op stub here
 * silently discarded the ENTIRE hook log: JVS, board profile, DirectInput8,
 * renderer, pause menu, all of it. That is why the only thing left after the
 * access violation was shaiolib_crk.c's own log, which reports on the
 * protection DLL and knows nothing about us.
 *
 * Same implementation as native_x86's: append a line per call, reopening each
 * time so a crash cannot lose buffered output. konami_pc_main.c now sets
 * ULTRA_JVS_LOG; with it unset this stays free. */
void ultra_d3d8_log(const char* fmt, ...)
{
    const char* p = getenv("ULTRA_JVS_LOG");
    FILE* f;
    va_list ap;
    if (!p || !*p) return;
    f = fopen(p, "a");
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc(10, f);
    fclose(f);
}

/* konami_render9.c and konami_audio.c were written against konami_plumbing.c,
 * which the derivation dropped in favor of the shared equivalents. Both names
 * simply forward - the behavior is identical, there is now one implementation
 * instead of two. */
void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);
void konami_iat_patch_all(const char* impdll, const char* impfn, void* newfn)
{
    ultra_iat_patch_all(impdll, impfn, newfn);
}
void konami_log(const char* fmt, ...)
{
    const char* p = getenv("ULTRA_JVS_LOG");
    FILE* f;
    va_list ap;
    if (!p || !*p) return;
    f = fopen(p, "a");
    if (!f) return;
    fputs("[konami] ", f);
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc(10, f);
    fclose(f);
}
void ultra_gl_install(void)      { }
void ultra_gl_teardown(void)     { }

/* ---- Type X2 lightgun bridge --------------------------------------------- */
void ultra_typex2_gun_install(void) { }
void ultra_typex2_gun_stop(void)    { }
void ultra_typex2_gun_set_window(HWND w) { (void)w; }
int  ultra_typex2_gun_pose(int* x, int* y, int* trig)
{
    (void)x; (void)y; (void)trig;
    return 0;                       /* no pose source on this board */
}
/* The pause menu and the Omni bridge ask for the render window through this
 * name on every board. konami_render9.c is the file that actually tracks it
 * (it polls for the game window and owns the upscale/window management), so
 * forward to it rather than guessing with GetActiveWindow - a guess returns
 * whatever window happens to be focused, including the loader's. */
HWND konami_render9_get_window(void);
HWND ultra_typex2_device_window(void)
{
    HWND h = konami_render9_get_window();
    if (!h) h = GetActiveWindow();
    if (!h) h = GetForegroundWindow();
    return h;
}

/* ---- Type X bring-up aid -------------------------------------------------- */
void ultra_gamestate_probe_start(void) { }


/* ---- renderer entry points the shared serial hook re-points ---------------
 * jvs_serial_hook installs these into the IAT for boards that throttle D3D11
 * or GL. This board is D3D9 only, so they must exist to link but must never
 * change behavior: each one forwards to the real export. */
HRESULT __stdcall ultra_Hook_D3D11CreateDeviceAndSwapChain(
        void* a, unsigned b, void* c, unsigned d, const void* e, unsigned f,
        unsigned g, const void* h, void** i, void** j, void* k, void** l)
{
    typedef HRESULT (__stdcall *fn_t)(void*, unsigned, void*, unsigned, const void*,
                                      unsigned, unsigned, const void*, void**, void**,
                                      void*, void**);
    HMODULE m = LoadLibraryA("d3d11.dll");
    fn_t real = m ? (fn_t)(void*)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!real) return (HRESULT)0x80004005L;   /* E_FAIL */
    return real(a, b, c, d, e, f, g, h, i, j, k, l);
}

HRESULT __stdcall ultra_Hook_CreateDXGIFactory(const void* riid, void** out)
{
    typedef HRESULT (__stdcall *fn_t)(const void*, void**);
    HMODULE m = LoadLibraryA("dxgi.dll");
    fn_t real = m ? (fn_t)(void*)GetProcAddress(m, "CreateDXGIFactory") : NULL;
    return real ? real(riid, out) : (HRESULT)0x80004005L;
}

HRESULT __stdcall ultra_Hook_CreateDXGIFactory1(const void* riid, void** out)
{
    typedef HRESULT (__stdcall *fn_t)(const void*, void**);
    HMODULE m = LoadLibraryA("dxgi.dll");
    fn_t real = m ? (fn_t)(void*)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    return real ? real(riid, out) : (HRESULT)0x80004005L;
}

BOOL __stdcall ultra_Hook_wglSwapBuffers(HDC dc)
{
    typedef BOOL (__stdcall *fn_t)(HDC);
    HMODULE m = LoadLibraryA("opengl32.dll");
    fn_t real = m ? (fn_t)(void*)GetProcAddress(m, "wglSwapBuffers") : NULL;
    return real ? real(dc) : FALSE;
}

BOOL __stdcall ultra_Hook_SwapBuffers(HDC dc)
{
    return SwapBuffers(dc);
}


/* ---- D3D9: TRANSPARENT, deliberately ------------------------------------
 *
 * native_x86 answers this name with d3d9_throttle.c, which hooks
 * IDirect3D9::CreateDevice and then patches the device's Present/Reset vtable
 * slots to add an fps limiter and upscaling.
 *
 * This board must NOT do that, and the reason is specific rather than
 * cautious: shaiolib_crk.c owns the entire D3D9 device lifecycle here. It
 * installs its own passive Direct3DCreate9 IAT hook, forces the game's render
 * gate, and drives presentation itself with Present(dev, NULL, NULL, NULL,
 * NULL). A second hook on the same create-fn hands the game a device whose
 * vtable has been rewritten underneath the code that believes it owns it, and
 * the game's only path to a device runs through that code. konami_render9.c
 * has carried that reasoning in its header since the board was written; the
 * re-derivation dropped the file and linked d3d9_throttle.c in its place,
 * which is what started the access violation.
 *
 * jvs_serial_hook.c patches this name into the IAT unconditionally and also
 * answers GetProcAddress("Direct3DCreate9") with it, so the symbol has to
 * exist. Forwarding to the real export makes both paths behaviorally
 * identical to not hooking at all, which is what this board needs.
 *
 * Window management and upscaling are not lost - konami_render9.c does them
 * without touching the device (NULL-rect Present auto-stretch). */
void* __stdcall ultra_Hook_Direct3DCreate9(UINT ver)
{
    typedef void* (__stdcall *fn_t)(UINT);
    static fn_t real = NULL;
    if (!real) {
        HMODULE m = LoadLibraryA("d3d9.dll");
        if (m) real = (fn_t)(void*)GetProcAddress(m, "Direct3DCreate9");
    }
    return real ? real(ver) : NULL;
}

/* d3d9_throttle.c's other export; nothing on this board calls it. */
void ultra_d3d9_install_throttle(void* d3d9) { (void)d3d9; }
void ultra_d3d9_teardown(void) { }

#endif /* _WIN32 */
