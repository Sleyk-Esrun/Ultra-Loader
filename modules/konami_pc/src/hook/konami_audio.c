/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  konami_audio.c -- DirectSound + XAudio2 interception for the Konami-PC-only hook DLL
 * =============================================================================
 *
 *  PROVENANCE: ported from modules/native_x86/src/hook/audio_hook.c's proven
 *  pattern, as this board's own independent copy (own symbols, own build --
 *  no shared source file, no shared compiled binary).
 *
 *  SAFE TO INSTALL, UNLIKE A D3D9 HOOK: shaiolib_crk.c never touches
 *  DirectSoundCreate, DirectSoundCreate8, IDirectSound::SetCooperativeLevel,
 *  or XAudio2Create anywhere (confirmed by inspection -- it only walks
 *  EaxMan.dll/dsound.dll/d3d9.dll/dinput8.dll's OWN import tables to cap their
 *  internal Sleep/WaitForSingleObject/WaitForMultipleObjects/LoadLibrary
 *  calls, a completely different (dll,function) target set, and separately
 *  hooks EaxMan.dll's EaxManagerCreate specifically -- never dsound.dll's own
 *  exports). So, unlike Direct3DCreate9 (see konami_render9.c's header for why
 *  that one is NOT touched here), there is no known hook this DLL could
 *  install-race against for audio. KSHG.exe statically imports dsound.dll
 *  (confirmed by shaiolib_crk.c's own DllMain comment: "EaxMan.dll, dsound.dll,
 *  d3d9.dll -- statically imported by KSHG.exe"), so a KSHG.exe IAT patch on
 *  ("dsound.dll","DirectSoundCreate"/"DirectSoundCreate8") reaches it cleanly.
 *
 *  ONE concrete, generic, known-class fix carried over from the reference:
 *  IDirectSound(8)'s SetCooperativeLevel legitimately fails when the HWND a
 *  game passes isn't in a state Windows will accept for DSSCL_EXCLUSIVE/
 *  DSSCL_PRIORITY -- common under an injected/automated launch where the
 *  game's own window isn't focused/visible the instant it calls this. On a
 *  FAILED real call we retry once with our own always-valid top-level window
 *  + DSSCL_PRIORITY -- broadly compatible, and only ever engages when the
 *  game's own attempt already failed (never overrides a working level).
 *
 *  XAudio2Create is log-only passthrough: no evidence KSHG.exe uses XAudio2
 *  at all (this is a DirectSound-era Konami title); the hook is a clean no-op
 *  if the game never imports any xaudio2_*.dll (konami_iat_patch_all only
 *  patches DLLs actually present in the process import list).
 *
 *  Clean-room: Ultra Loader's own code over the public DirectSound API.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "konami_hook.h"

#define alog konami_log

/* ---- a fallback top-level window SetCooperativeLevel can always use -------
 * Created lazily, once, only if a game's own SetCooperativeLevel call fails. */
static HWND fallback_hwnd(void) {
    static HWND h;
    static int tried;
    if (h || tried) return h;
    tried = 1;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = (HINSTANCE)GetModuleHandleA(NULL);
    wc.lpszClassName = "UltraKonamiAudioFallback";
    RegisterClassA(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */
    h = CreateWindowExA(0, "UltraKonamiAudioFallback", NULL, WS_POPUP,
                         0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    return h;
}

/* ---- IDirectSound(8) vtable wrap: only SetCooperativeLevel (slot 6) ------- *
 * IDirectSound/IDirectSound8 share the same vtable layout:
 *   0 QueryInterface 1 AddRef 2 Release 3 CreateSoundBuffer 4 GetCaps
 *   5 DuplicateSoundBuffer 6 SetCooperativeLevel 7 Compact
 *   8 GetSpeakerConfig 9 SetSpeakerConfig 10 Initialize
 * We only ever patch slot 6; every other call goes through the game's own
 * unmodified vtable entries -- this hook cannot affect anything else
 * IDirectSound does. */
typedef HRESULT (STDMETHODCALLTYPE *pfnSetCoopLevel)(IDirectSound*, HWND, DWORD);
static pfnSetCoopLevel s_real_setcooplevel[8];
static void*           s_real_setcooplevel_obj[8];
static int              s_scl_n;

static HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel(IDirectSound* self, HWND hwnd, DWORD level) {
    pfnSetCoopLevel real = NULL;
    for (int i = 0; i < s_scl_n; i++)
        if (s_real_setcooplevel_obj[i] == (void*)self) { real = s_real_setcooplevel[i]; break; }
    if (!real) return DSERR_GENERIC;   /* should never happen: only called via our own patched slot */

    HRESULT hr = real(self, hwnd, level);
    alog("audio: SetCooperativeLevel(hwnd=%p level=0x%lX) -> 0x%08lX", (void*)hwnd,
         (unsigned long)level, (unsigned long)hr);
    if (FAILED(hr)) {
        HWND fb = fallback_hwnd();
        if (fb) {
            HRESULT hr2 = real(self, fb, DSSCL_PRIORITY);
            alog("audio: SetCooperativeLevel retry (fallback hwnd=%p, DSSCL_PRIORITY) -> 0x%08lX",
                 (void*)fb, (unsigned long)hr2);
            if (SUCCEEDED(hr2)) return hr2;
        }
    }
    return hr;
}

/* ---- WHO IS MAKING THE NOISE ----------------------------------------------
 *
 * An entire session went into guessing which input was repeating, when the thing
 * we actually wanted to know was who asks for the sound. Every probe below is
 * LOG-ONLY and passes straight through; each one resolves its CALLER to
 * module+offset, so the log says whether the request came from KSHG.exe (the
 * game genuinely playing something), from shaiolib.dll (our own layer), or from
 * Windows on someone's behalf.
 *
 * Covered: IDirectSoundBuffer::Play (the game's own effects), plus the four
 * Win32 routes that make a noise without DirectSound at all -- MessageBeep,
 * kernel32 Beep, and PlaySound/sndPlaySound. A MessageBox with an icon calls
 * MessageBeep every time it is shown, so a raise/dismiss loop lands here too. */
extern void ultra_boot_trace_where(void*, char*, unsigned);

#define SND_CALLER(w) ultra_boot_trace_where(__builtin_return_address(0), (w), (unsigned)sizeof (w))

/* IDirectSoundBuffer vtable: 0 QI, 1 AddRef, 2 Release, 3 GetCaps,
 * 4 GetCurrentPosition, 5 GetFormat, 6 GetVolume, 7 GetPan, 8 GetFrequency,
 * 9 GetStatus, 10 Initialize, 11 Lock, 12 Play, 13 SetCurrentPosition,
 * 14 SetFormat, 15 SetVolume, 16 SetPan, 17 SetFrequency, 18 Stop, 19 Unlock,
 * 20 Restore.  Play is slot 12. */
#define DSB_VT_PLAY   12
#define DSB_VT_COUNT  21
#define DSBPLAY_LOOPING_BIT 0x00000001u

typedef HRESULT (STDMETHODCALLTYPE *pfnDSBPlay)(void*, DWORD, DWORD, DWORD);
static pfnDSBPlay s_real_dsb_play;

static HRESULT STDMETHODCALLTYPE Hook_DSB_Play(void* self, DWORD res1, DWORD pri, DWORD flags) {
    static unsigned n;
    char w[320];
    ++n;
    if (n <= 16 || (n % 20) == 0) {
        SND_CALLER(w);
        alog("SOUND: Play #%u buf=%p flags=0x%lX%s  <- called from %s",
             n, self, (unsigned long)flags,
             (flags & DSBPLAY_LOOPING_BIT) ? " LOOPING" : "", w);
    }
    return s_real_dsb_play ? s_real_dsb_play(self, res1, pri, flags) : 0;
}

/* IDirectSound::CreateSoundBuffer is slot 3. Every buffer handed out by one
 * dsound.dll shares a vtable, so patching Play on the FIRST buffer catches all
 * of them and we never touch a vtable twice. */
typedef HRESULT (STDMETHODCALLTYPE *pfnCreateSoundBuffer)(IDirectSound*, const void*, void**, void*);
static pfnCreateSoundBuffer s_real_csb;

static HRESULT STDMETHODCALLTYPE Hook_CreateSoundBuffer(IDirectSound* self, const void* desc,
                                                        void** out, void* outer) {
    HRESULT hr = s_real_csb ? s_real_csb(self, desc, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && !s_real_dsb_play) {
        void*** v = (void***)*out;
        DWORD old;
        if (VirtualProtect(*v, sizeof(void*) * DSB_VT_COUNT, PAGE_EXECUTE_READWRITE, &old)) {
            s_real_dsb_play = (pfnDSBPlay)(*v)[DSB_VT_PLAY];
            (*v)[DSB_VT_PLAY] = (void*)Hook_DSB_Play;
            VirtualProtect(*v, sizeof(void*) * DSB_VT_COUNT, old, &old);
            alog("SOUND: IDirectSoundBuffer::Play hooked on the shared vtable (real=%p)",
                 (void*)s_real_dsb_play);
        }
    }
    return hr;
}

/* ---- the Win32 noise-makers, none of which involve DirectSound ------------- */
typedef BOOL (WINAPI *pfnMessageBeep)(UINT);
typedef BOOL (WINAPI *pfnBeep)(DWORD, DWORD);
typedef BOOL (WINAPI *pfnPlaySoundA)(LPCSTR, HMODULE, DWORD);
typedef BOOL (WINAPI *pfnPlaySoundW)(LPCWSTR, HMODULE, DWORD);
static pfnMessageBeep s_real_messagebeep;
static pfnBeep        s_real_beep;
static pfnPlaySoundA  s_real_playsounda;
static pfnPlaySoundW  s_real_playsoundw;

static BOOL WINAPI Hook_MessageBeep(UINT type) {
    static unsigned n; char w[320]; ++n;
    if (n <= 16 || (n % 20) == 0) {
        SND_CALLER(w);
        alog("SOUND: MessageBeep #%u type=0x%X  <- called from %s", n, type, w);
    }
    return s_real_messagebeep ? s_real_messagebeep(type) : TRUE;
}
static BOOL WINAPI Hook_Beep(DWORD freq, DWORD dur) {
    static unsigned n; char w[320]; ++n;
    if (n <= 16 || (n % 20) == 0) {
        SND_CALLER(w);
        alog("SOUND: Beep #%u %luHz %lums  <- called from %s",
             n, (unsigned long)freq, (unsigned long)dur, w);
    }
    return s_real_beep ? s_real_beep(freq, dur) : TRUE;
}
static BOOL WINAPI Hook_PlaySoundA(LPCSTR s, HMODULE m, DWORD f) {
    static unsigned n; char w[320]; ++n;
    if (n <= 16 || (n % 20) == 0) {
        SND_CALLER(w);
        alog("SOUND: PlaySoundA #%u \"%s\" flags=0x%lX  <- called from %s",
             n, s ? s : "(null)", (unsigned long)f, w);
    }
    return s_real_playsounda ? s_real_playsounda(s, m, f) : TRUE;
}
static BOOL WINAPI Hook_PlaySoundW(LPCWSTR s, HMODULE m, DWORD f) {
    static unsigned n; char w[320]; ++n;
    if (n <= 16 || (n % 20) == 0) {
        SND_CALLER(w);
        alog("SOUND: PlaySoundW #%u flags=0x%lX  <- called from %s", n, (unsigned long)f, w);
    }
    return s_real_playsoundw ? s_real_playsoundw(s, m, f) : TRUE;
}

static void wrap_directsound(IDirectSound* ds) {
    if (!ds || s_scl_n >= 8) return;
    void*** vtbl = (void***)(void*)ds;
    DWORD old;
    if (VirtualProtect(*vtbl, sizeof(void*) * 11, PAGE_EXECUTE_READWRITE, &old)) {
        s_real_setcooplevel_obj[s_scl_n] = (void*)ds;
        s_real_setcooplevel[s_scl_n]     = (pfnSetCoopLevel)(*vtbl)[6];
        (*vtbl)[6] = (void*)Hook_SetCooperativeLevel;
        /* slot 3 = CreateSoundBuffer: the only way to reach the buffers, and
         * therefore the only way to see what the game actually plays. */
        if (!s_real_csb) {
            s_real_csb = (pfnCreateSoundBuffer)(*vtbl)[3];
            (*vtbl)[3] = (void*)Hook_CreateSoundBuffer;
            alog("audio: CreateSoundBuffer hooked -- sound trace armed");
        }
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
    alog("audio: DirectSoundCreate -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound((IDirectSound*)*out);
    return hr;
}
static HRESULT WINAPI Hook_DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* out, LPUNKNOWN outer) {
    HRESULT hr = Real_DirectSoundCreate8 ? Real_DirectSoundCreate8(guid, out, outer) : DSERR_NODRIVER;
    alog("audio: DirectSoundCreate8 -> 0x%08lX (obj=%p)", (unsigned long)hr, out ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) wrap_directsound((IDirectSound*)*out);
    return hr;
}

/* ---- XAudio2Create: log-only. See file header -- no evidence this game
 * imports it; konami_iat_patch_all no-ops cleanly on any name the process
 * doesn't actually import, so it is safe to attempt every known version. */
typedef HRESULT (WINAPI *pXAudio2Create)(void**, UINT32, UINT32);
static pXAudio2Create Real_XAudio2Create;

static HRESULT WINAPI Hook_XAudio2Create(void** ppXAudio2, UINT32 flags, UINT32 proc) {
    HRESULT hr = Real_XAudio2Create ? Real_XAudio2Create(ppXAudio2, flags, proc) : E_NOTIMPL;
    alog("audio: XAudio2Create(flags=0x%lX) -> 0x%08lX (obj=%p)",
         (unsigned long)flags, (unsigned long)hr, ppXAudio2 ? *ppXAudio2 : NULL);
    return hr;
}

/* -------------------------------------------------------------------------- */
static int s_installed;
void konami_audio_install(void) {
    if (s_installed) return;   /* idempotent, matches every other *_install() in this DLL */
    s_installed = 1;

    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (ds) {
        Real_DirectSoundCreate  = (pDirectSoundCreate)(void*)GetProcAddress(ds, "DirectSoundCreate");
        Real_DirectSoundCreate8 = (pDirectSoundCreate8)(void*)GetProcAddress(ds, "DirectSoundCreate8");
        konami_iat_patch_all("dsound.dll", "DirectSoundCreate",  (void*)Hook_DirectSoundCreate);
        konami_iat_patch_all("dsound.dll", "DirectSoundCreate8", (void*)Hook_DirectSoundCreate8);
        alog("audio: dsound.dll present, DirectSoundCreate/8 armed");
    } else {
        alog("audio: dsound.dll not loaded yet -- worker thread will retry");
    }

    /* The Win32 noise-makers, independent of dsound entirely -- so this block
     * sits OUTSIDE the dsound branch. A beep that never touches DirectSound is
     * precisely the case we have failed to rule out, so it must be armed whether
     * or not dsound is up yet. Real pointers come from the real modules, so the
     * pass-through is exact; a module that is absent simply gets no probe. */
    {
        HMODULE u  = GetModuleHandleA("user32.dll");
        HMODULE k  = GetModuleHandleA("kernel32.dll");
        HMODULE wm = GetModuleHandleA("winmm.dll");
        if (u)  s_real_messagebeep = (pfnMessageBeep)(void*)GetProcAddress(u,  "MessageBeep");
        if (k)  s_real_beep        = (pfnBeep)(void*)GetProcAddress(k,  "Beep");
        if (wm) s_real_playsounda  = (pfnPlaySoundA)(void*)GetProcAddress(wm, "PlaySoundA");
        if (wm) s_real_playsoundw  = (pfnPlaySoundW)(void*)GetProcAddress(wm, "PlaySoundW");
        konami_iat_patch_all("user32.dll",   "MessageBeep", (void*)Hook_MessageBeep);
        konami_iat_patch_all("kernel32.dll", "Beep",        (void*)Hook_Beep);
        konami_iat_patch_all("winmm.dll",    "PlaySoundA",  (void*)Hook_PlaySoundA);
        konami_iat_patch_all("winmm.dll",    "PlaySoundW",  (void*)Hook_PlaySoundW);
        alog("audio: sound trace armed (DSB::Play + MessageBeep/Beep/PlaySound)");
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
        konami_iat_patch_all(xa2_versions[i], "XAudio2Create", (void*)Hook_XAudio2Create);
        alog("audio: %s present, XAudio2Create armed", xa2_versions[i]);
    }
    if (ds) s_installed = 1; else s_installed = 0;   /* allow retry until dsound.dll is loaded */
}

#endif /* _WIN32 */
