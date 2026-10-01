/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  audio_hook_unico.c -- UNICO PC board's OWN DirectSound + XAudio2
 *                         diagnostic-passthrough audio hook.
 * =============================================================================
 *
 *  Genuinely NEW capability for this board: no audio instrumentation existed
 *  anywhere in unico_pc's module before this file (the per-game complete rule
 *  -- render + SOUND + controls, all three -- needs one). Adapted from the
 *  PATTERN in modules/native_x86/src/hook/audio_hook.c (read as reference
 *  only -- no #include, no link, no runtime dependency on that file).
 *
 *  Mechanism: reuse THIS board's own IAT-patch machinery
 *  (ultra_iat_patch_all_unico, owned by jvs_serial_hook_unico.c) -- no new
 *  hooking technique introduced. Default behavior is diagnostic passthrough
 *  (log the call + HRESULT via ULTRA_JVS_LOG so a silent-audio report has real
 *  data instead of guesses).
 *
 *  ONE concrete, generic fix carried over: IDirectSound(8)'s
 *  SetCooperativeLevel legitimately fails when the HWND a game passes isn't
 *  in a state Windows accepts for DSSCL_EXCLUSIVE/DSSCL_PRIORITY (common
 *  under this board's spawn-suspended + APC-inject launch, where the game's
 *  own window may not be focused/visible yet the instant it calls this). On
 *  a FAILED real call, retry once with our own always-valid top-level window
 *  + DSSCL_PRIORITY -- broadly compatible, only engages when the game's own
 *  attempt already failed (never overrides a working level).
 *
 *  Clean-room: Ultra Loader's own code over the public DirectSound/XAudio2
 *  Win32 API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* owned by jvs_serial_hook_unico.c */
void ultra_iat_patch_all_unico(const char* impdll, const char* impfn, void* newfn);
extern void ultra_unico_log(const char* fmt, ...);
#define alog ultra_unico_log

/* ---- a fallback top-level window SetCooperativeLevel can always use ------- */
static HWND fallback_hwnd_unico(void) {
    static HWND h;
    static int tried;
    if (h || tried) return h;
    tried = 1;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = (HINSTANCE)GetModuleHandleA(NULL);
    wc.lpszClassName = "UltraAudioFallbackUnico";
    RegisterClassA(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */
    h = CreateWindowExA(0, "UltraAudioFallbackUnico", NULL, WS_POPUP,
                         0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    return h;
}

/* ---- IDirectSound(8) vtable wrap: only SetCooperativeLevel (slot 6) ------- */
typedef HRESULT (STDMETHODCALLTYPE *pfnSetCoopLevel)(IDirectSound*, HWND, DWORD);
static pfnSetCoopLevel s_real_setcooplevel[8];
static void*           s_real_setcooplevel_obj[8];
static int             s_scl_n;

static HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel_Unico(IDirectSound* self, HWND hwnd, DWORD level) {
    pfnSetCoopLevel real = NULL;
    for (int i = 0; i < s_scl_n; i++)
        if (s_real_setcooplevel_obj[i] == (void*)self) { real = s_real_setcooplevel[i]; break; }
    if (!real) return DSERR_GENERIC;

    HRESULT hr = real(self, hwnd, level);
    alog("audio(unico): SetCooperativeLevel(hwnd=%p level=0x%lX) -> 0x%08lX", (void*)hwnd,
         (unsigned long)level, (unsigned long)hr);
    if (FAILED(hr)) {
        HWND fb = fallback_hwnd_unico();
        if (fb) {
            HRESULT hr2 = real(self, fb, DSSCL_PRIORITY);
            alog("audio(unico): SetCooperativeLevel retry (fallback hwnd=%p, DSSCL_PRIORITY) -> 0x%08lX",
                 (void*)fb, (unsigned long)hr2);
            if (SUCCEEDED(hr2)) return hr2;
        }
    }
    return hr;
}

static void wrap_directsound_unico(IDirectSound* ds) {
    if (!ds || s_scl_n >= 8) return;
    void*** vtbl = (void***)(void*)ds;
    DWORD old;
    if (VirtualProtect(*vtbl, sizeof(void*) * 11, PAGE_EXECUTE_READWRITE, &old)) {
        s_real_setcooplevel_obj[s_scl_n] = (void*)ds;
        s_real_setcooplevel[s_scl_n]     = (pfnSetCoopLevel)(*vtbl)[6];
        (*vtbl)[6] = (void*)Hook_SetCooperativeLevel_Unico;
        s_scl_n++;
        VirtualProtect(*vtbl, sizeof(void*) * 11, old, &old);
    }
}

/* ---- DirectSoundCreate / DirectSoundCreate8 IAT hooks --------------------- */
typedef HRESULT (WINAPI *pDirectSoundCreate)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
typedef HRESULT (WINAPI *pDirectSoundCreate8)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
static pDirectSoundCreate  Real_DirectSoundCreate_Unico;
static pDirectSoundCreate8 Real_DirectSoundCreate8_Unico;

static HRESULT WINAPI Hook_DirectSoundCreate_Unico(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    HRESULT hr = Real_DirectSoundCreate_Unico ? Real_DirectSoundCreate_Unico(guid, out, outer) : DSERR_NODRIVER;
    alog("audio(unico): DirectSoundCreate -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound_unico((IDirectSound*)*out);
    return hr;
}
static HRESULT WINAPI Hook_DirectSoundCreate8_Unico(LPCGUID guid, LPDIRECTSOUND8* out, LPUNKNOWN outer) {
    HRESULT hr = Real_DirectSoundCreate8_Unico ? Real_DirectSoundCreate8_Unico(guid, out, outer) : DSERR_NODRIVER;
    alog("audio(unico): DirectSoundCreate8 -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound_unico((IDirectSound*)*out);
    return hr;
}

/* ---- XAudio2Create: log-only (WASAPI-backed) ------------------------------ */
typedef HRESULT (WINAPI *pXAudio2Create)(void**, UINT32, UINT32);
static pXAudio2Create Real_XAudio2Create_Unico;

static HRESULT WINAPI Hook_XAudio2Create_Unico(void** ppXAudio2, UINT32 flags, UINT32 proc) {
    HRESULT hr = Real_XAudio2Create_Unico ? Real_XAudio2Create_Unico(ppXAudio2, flags, proc) : E_NOTIMPL;
    alog("audio(unico): XAudio2Create(flags=0x%lX) -> 0x%08lX (obj=%p)",
         (unsigned long)flags, (unsigned long)hr, ppXAudio2 ? *ppXAudio2 : NULL);
    return hr;
}

/* -------------------------------------------------------------------------- */
static int s_installed;
void ultra_audio_install_unico(void) {
    if (s_installed) return;
    s_installed = 1;

    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (!ds) ds = LoadLibraryA("dsound.dll");
    if (ds) {
        Real_DirectSoundCreate_Unico  = (pDirectSoundCreate)(void*)GetProcAddress(ds, "DirectSoundCreate");
        Real_DirectSoundCreate8_Unico = (pDirectSoundCreate8)(void*)GetProcAddress(ds, "DirectSoundCreate8");
        ultra_iat_patch_all_unico("dsound.dll", "DirectSoundCreate",  (void*)Hook_DirectSoundCreate_Unico);
        ultra_iat_patch_all_unico("dsound.dll", "DirectSoundCreate8", (void*)Hook_DirectSoundCreate8_Unico);
        alog("audio(unico): dsound.dll present, DirectSoundCreate/8 armed");
    }

    static const char* xa2_versions[] = {
        "xaudio2_9.dll", "xaudio2_8.dll", "xaudio2_7.dll",
        "xaudio2_6.dll", "xaudio2_5.dll", "xaudio2_4.dll",
        "xaudio2_3.dll", "xaudio2_2.dll", "xaudio2_1.dll",
        "xaudio2_0.dll", "xaudio2.dll",
    };
    for (size_t i = 0; i < sizeof(xa2_versions)/sizeof(xa2_versions[0]); ++i) {
        HMODULE xa = GetModuleHandleA(xa2_versions[i]);
        if (!xa) continue;   /* only arm for a version this process actually loaded */
        if (!Real_XAudio2Create_Unico)
            Real_XAudio2Create_Unico = (pXAudio2Create)(void*)GetProcAddress(xa, "XAudio2Create");
        ultra_iat_patch_all_unico(xa2_versions[i], "XAudio2Create", (void*)Hook_XAudio2Create_Unico);
        alog("audio(unico): %s present, XAudio2Create armed", xa2_versions[i]);
    }
}

#endif /* _WIN32 */
