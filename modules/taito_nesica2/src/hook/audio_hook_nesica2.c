/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  audio_hook_nesica2.c -- DirectSound + XAudio2 interception, NESiCAxLive2's
 *                          own independent copy.
 * =============================================================================
 *
 *  Every present-hook this project ships has a matching install/log/diagnose
 *  story; audio needs one too so a "render works, controls half-work, audio
 *  never even gets instrumented" report has real data instead of guesses.
 *
 *  Mechanism: the SAME IAT-patch machinery every other TU in this DLL uses
 *  (tn2_iat_patch_all, hook_dllmain_nesica2.c) -- no new hooking technique
 *  introduced. Default behavior is diagnostic passthrough (log the call +
 *  HRESULT).
 *
 *  ONE concrete, generic, known-class fix: IDirectSound(8)'s
 *  SetCooperativeLevel legitimately fails when the HWND a game passes isn't
 *  in a state Windows will accept for DSSCL_EXCLUSIVE/DSSCL_PRIORITY (common
 *  under an injected/automated launch where the game's own window isn't
 *  focused/visible yet the instant it calls this). On a FAILED real call, we
 *  retry once with our own always-valid top-level window + DSSCL_PRIORITY --
 *  broadly compatible, and only ever engages when the game's own attempt
 *  already failed (never overrides a working level). No per-game data.
 *
 *  INDEPENDENCE: fresh code over the public DirectSound/XAudio2 API, reusing
 *  only this board's own tn2_iat_patch_all/tn2_log (adapted from the
 *  taito_typex3 independent build's own copy, itself read from
 *  native_x86/src/hook/audio_hook.c as a working-pattern reference per the
 *  project owner's explicit instruction). Not linked to, nor sharing a
 *  compiled binary with, any other board's audio hook in this project.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "taito_nesica2_hook.h"

/* ---- a fallback top-level window SetCooperativeLevel can always use ------- */
static HWND fallback_hwnd(void) {
    static HWND h;
    static int tried;
    if (h || tried) return h;
    tried = 1;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = (HINSTANCE)GetModuleHandleA(NULL);
    wc.lpszClassName = "UltraNesica2AudioFallback";
    RegisterClassA(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */
    h = CreateWindowExA(0, "UltraNesica2AudioFallback", NULL, WS_POPUP,
                         0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    return h;
}

/* ---- IDirectSound(8) vtable wrap: only SetCooperativeLevel (slot 6) ------- */
typedef HRESULT (STDMETHODCALLTYPE *pfnSetCoopLevel)(IDirectSound*, HWND, DWORD);
static pfnSetCoopLevel s_real_setcooplevel[8];
static void*           s_real_setcooplevel_obj[8];
static int              s_scl_n;

static HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel(IDirectSound* self, HWND hwnd, DWORD level) {
    pfnSetCoopLevel real = NULL;
    for (int i = 0; i < s_scl_n; i++)
        if (s_real_setcooplevel_obj[i] == (void*)self) { real = s_real_setcooplevel[i]; break; }
    if (!real) return DSERR_GENERIC;

    HRESULT hr = real(self, hwnd, level);
    tn2_log("audio: SetCooperativeLevel(hwnd=%p level=0x%lX) -> 0x%08lX", (void*)hwnd,
         (unsigned long)level, (unsigned long)hr);
    if (FAILED(hr)) {
        HWND fb = fallback_hwnd();
        if (fb) {
            HRESULT hr2 = real(self, fb, DSSCL_PRIORITY);
            tn2_log("audio: SetCooperativeLevel retry (fallback hwnd=%p, DSSCL_PRIORITY) -> 0x%08lX",
                 (void*)fb, (unsigned long)hr2);
            if (SUCCEEDED(hr2)) return hr2;
        }
    }
    return hr;
}

static void wrap_directsound(IDirectSound* ds) {
    if (!ds || s_scl_n >= 8) return;
    void*** vtbl = (void***)(void*)ds;
    DWORD old;
    if (VirtualProtect(*vtbl, sizeof(void*) * 11, PAGE_EXECUTE_READWRITE, &old)) {
        s_real_setcooplevel_obj[s_scl_n] = (void*)ds;
        s_real_setcooplevel[s_scl_n]     = (pfnSetCoopLevel)(*vtbl)[6];
        (*vtbl)[6] = (void*)Hook_SetCooperativeLevel;
        s_scl_n++;
        VirtualProtect(*vtbl, sizeof(void*) * 11, old, &old);
    }
}

/* ---- DirectSoundCreate / DirectSoundCreate8 IAT hooks --------------------- */
typedef HRESULT (WINAPI *pDirectSoundCreate)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
typedef HRESULT (WINAPI *pDirectSoundCreate8)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
static pDirectSoundCreate  Real_DirectSoundCreate;
static pDirectSoundCreate8 Real_DirectSoundCreate8;

static HRESULT WINAPI Hook_DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    HRESULT hr = Real_DirectSoundCreate ? Real_DirectSoundCreate(guid, out, outer) : DSERR_NODRIVER;
    tn2_log("audio: DirectSoundCreate -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound((IDirectSound*)*out);
    return hr;
}
static HRESULT WINAPI Hook_DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* out, LPUNKNOWN outer) {
    HRESULT hr = Real_DirectSoundCreate8 ? Real_DirectSoundCreate8(guid, out, outer) : DSERR_NODRIVER;
    tn2_log("audio: DirectSoundCreate8 -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound((IDirectSound*)*out);
    return hr;
}

/* ---- XAudio2Create: log-only (WASAPI-backed; no cooperative-level class of
 * bug to defend against). Resolved dynamically by version; the IAT patch
 * no-ops cleanly on any name a given process doesn't actually import. */
typedef HRESULT (WINAPI *pXAudio2Create)(void**, UINT32, UINT32);
static pXAudio2Create Real_XAudio2Create;

static HRESULT WINAPI Hook_XAudio2Create(void** ppXAudio2, UINT32 flags, UINT32 proc) {
    HRESULT hr = Real_XAudio2Create ? Real_XAudio2Create(ppXAudio2, flags, proc) : E_NOTIMPL;
    tn2_log("audio: XAudio2Create(flags=0x%lX) -> 0x%08lX (obj=%p)",
         (unsigned long)flags, (unsigned long)hr, ppXAudio2 ? *ppXAudio2 : NULL);
    return hr;
}

/* -------------------------------------------------------------------------- */
static int s_installed;
void tn2_audio_install(void) {
    if (s_installed) return;
    s_installed = 1;

    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (!ds) ds = LoadLibraryA("dsound.dll");
    if (ds) {
        Real_DirectSoundCreate  = (pDirectSoundCreate)(void*)GetProcAddress(ds, "DirectSoundCreate");
        Real_DirectSoundCreate8 = (pDirectSoundCreate8)(void*)GetProcAddress(ds, "DirectSoundCreate8");
        tn2_iat_patch_all("dsound.dll", "DirectSoundCreate",  (void*)Hook_DirectSoundCreate);
        tn2_iat_patch_all("dsound.dll", "DirectSoundCreate8", (void*)Hook_DirectSoundCreate8);
        tn2_log("audio: dsound.dll present, DirectSoundCreate/8 armed");
    }

    static const char* xa2_versions[] = {
        "xaudio2_9.dll", "xaudio2_8.dll", "xaudio2_7.dll",
        "xaudio2_6.dll", "xaudio2_5.dll", "xaudio2_4.dll",
        "xaudio2_3.dll", "xaudio2_2.dll", "xaudio2_1.dll",
        "xaudio2_0.dll", "xaudio2.dll",
    };
    for (size_t i = 0; i < sizeof(xa2_versions)/sizeof(xa2_versions[0]); ++i) {
        HMODULE xa = GetModuleHandleA(xa2_versions[i]);
        if (!xa) continue;   /* not loaded by this process -- don't force-load a version it never asked for */
        if (!Real_XAudio2Create)
            Real_XAudio2Create = (pXAudio2Create)(void*)GetProcAddress(xa, "XAudio2Create");
        tn2_iat_patch_all(xa2_versions[i], "XAudio2Create", (void*)Hook_XAudio2Create);
        tn2_log("audio: %s present, XAudio2Create armed", xa2_versions[i]);
    }
}

#endif /* _WIN32 */
