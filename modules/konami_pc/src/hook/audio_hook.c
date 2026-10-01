/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  audio_hook.c -- generic DirectSound + XAudio2 interception for native_x86
 * =============================================================================
 *
 *  Every present-hook (D3D8/9/11/GL) this project has already has a matching
 *  install/log/diagnose story; audio had NONE -- no DirectSoundCreate or
 *  XAudio2Create interception existed anywhere in this hook before this file.
 *  That's a real, previously-unaddressed gap (per-game "render works, controls
 *  half-work, audio never even gets instrumented" reports).
 *
 *  Mechanism: reuse the SAME IAT-patch machinery every other hook in this file
 *  uses (ultra_iat_patch_all, from jvs_serial_hook.c) -- no new hooking
 *  technique introduced. Default behavior is diagnostic passthrough (log the
 *  call + HRESULT so a silent-audio report has real data instead of guesses).
 *
 *  ONE concrete, generic, known-class fix is included: IDirectSound(8)'s
 *  SetCooperativeLevel legitimately fails when the HWND a game passes isn't
 *  in a state Windows will accept for DSSCL_EXCLUSIVE/DSSCL_PRIORITY (common
 *  under an injected/automated launch where the game's own window isn't
 *  focused/visible yet the instant it calls this). On a FAILED real call, we
 *  retry once with our own always-valid top-level window + DSSCL_PRIORITY --
 *  broadly compatible, and only ever engages when the game's own attempt
 *  already failed (never overrides a working level). No per-game data.
 *
 *  This is a genuinely-new capability, not a per-game hack -- it belongs in
 *  the shared generic core, unlike the board/game-specific pieces this same
 *  refactor pass is moving OUT of the shared files (see board_io_ops.h).
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* shared IAT patcher (jvs_serial_hook.c) */
void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

static void alog(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG");
    if (!p || !*p) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

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
    wc.lpszClassName = "UltraAudioFallback";
    RegisterClassA(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */
    h = CreateWindowExA(0, "UltraAudioFallback", NULL, WS_POPUP,
                         0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    return h;
}

/* ---- IDirectSound(8) vtable wrap: only SetCooperativeLevel (slot 6) ------- *
 * IDirectSound/IDirectSound8 share the same vtable layout:
 *   0 QueryInterface 1 AddRef 2 Release 3 CreateSoundBuffer 4 GetCaps
 *   5 DuplicateSoundBuffer 6 SetCooperativeLevel 7 Compact
 *   8 GetSpeakerConfig 9 SetSpeakerConfig 10 Initialize
 * We only ever patch slot 6; every other call goes through the game's own
 * unmodified vtable entries -- this hook cannot affect anything else IDirectSound
 * does. */
typedef HRESULT (STDMETHODCALLTYPE *pfnSetCoopLevel)(IDirectSound*, HWND, DWORD);
static pfnSetCoopLevel s_real_setcooplevel[8];   /* small ring; enough for one JVS node's audio device(s) */
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
 * The same trace exists in konami_audio.c, and it never fired: BOTH files are
 * compiled into this board's hook and both IAT-patch DirectSoundCreate8, so they
 * race and only one wins. The log settles which -- konami_audio.c prefixes its
 * lines "[konami] audio:", and the DirectSoundCreate8 result line has no prefix,
 * so THIS file is the one that actually receives the object. The trace has to
 * live here to see anything.
 *
 * Log-only, straight pass-through, and every line resolves its CALLER to
 * module+offset so the log says whether KSHG.exe asked for the sound or one of
 * our own layers did. */
extern void ultra_boot_trace_where(void*, char*, unsigned);

/* IDirectSoundBuffer vtable: 0 QI, 1 AddRef, 2 Release, 3 GetCaps,
 * 4 GetCurrentPosition, 5 GetFormat, 6 GetVolume, 7 GetPan, 8 GetFrequency,
 * 9 GetStatus, 10 Initialize, 11 Lock, 12 Play, ... 20 Restore. */
#define AH_DSB_LOCK   11   /* IDirectSoundBuffer::Lock */
#define AH_DSB_PLAY   12
#define AH_DSB_COUNT  21
#define AH_LOOPING    0x00000001u

typedef HRESULT (STDMETHODCALLTYPE *ah_pfnPlay)(void*, DWORD, DWORD, DWORD);
static ah_pfnPlay s_ah_real_play;

static HRESULT STDMETHODCALLTYPE AH_Hook_Play(void* self, DWORD r1, DWORD pri, DWORD flags) {
    static unsigned n;
    char w[320];
    ++n;
    if (n <= 16 || (n % 20) == 0) {
        ultra_boot_trace_where(__builtin_return_address(0), w, (unsigned)sizeof w);
        alog("SOUND: Play #%u buf=%p flags=0x%lX%s  <- called from %s",
             n, self, (unsigned long)flags, (flags & AH_LOOPING) ? " LOOPING" : "", w);
    }
    return s_ah_real_play ? s_ah_real_play(self, r1, pri, flags) : 0;
}

/* IS AUDIO DATA FLOWING AT ALL?
 *
 * The game never calls Play, so it mixes everything into one already-looping
 * streaming buffer. That makes Play useless as an activity signal -- but every
 * time the game writes PCM into that stream it must Lock the buffer first. A
 * steady stream of Lock calls means audio IS being produced and any silence is
 * in the content; no Lock calls at all means the audio engine is dead, which
 * would point straight at the EaxManagerCreate init we deliberately fail.
 * Log-only, heavily rate-limited: this fires at audio-buffer rate. */
typedef HRESULT (STDMETHODCALLTYPE *ah_pfnLock)(void*, DWORD, DWORD, void**, DWORD*,
                                                void**, DWORD*, DWORD);
static ah_pfnLock s_ah_real_lock;

static HRESULT STDMETHODCALLTYPE AH_Hook_Lock(void* self, DWORD off, DWORD bytes,
                                              void** p1, DWORD* b1,
                                              void** p2, DWORD* b2, DWORD flags) {
    static unsigned n;
    static DWORD t0;
    HRESULT hr;
    if (!t0) t0 = GetTickCount();
    ++n;
    if (n == 1 || (n % 300) == 0)
        alog("SOUND: Lock #%u buf=%p bytes=%lu  (audio data IS flowing, %lu locks in %lums)",
             n, self, (unsigned long)bytes, (unsigned long)n,
             (unsigned long)(GetTickCount() - t0));
    hr = s_ah_real_lock ? s_ah_real_lock(self, off, bytes, p1, b1, p2, b2, flags)
                        : E_FAIL;
    return hr;
}

/* IDirectSound::CreateSoundBuffer is slot 3. All buffers from one dsound.dll
 * share a vtable, so patching Play on the first buffer catches every sound the
 * game plays, and we never patch a vtable twice. */
typedef HRESULT (STDMETHODCALLTYPE *ah_pfnCSB)(IDirectSound*, const void*, void**, void*);
static ah_pfnCSB s_ah_real_csb;

static HRESULT STDMETHODCALLTYPE AH_Hook_CreateSoundBuffer(IDirectSound* self, const void* desc,
                                                           void** out, void* outer) {
    HRESULT hr = s_ah_real_csb ? s_ah_real_csb(self, desc, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && !s_ah_real_play) {
        void*** v = (void***)*out;
        DWORD old;
        if (VirtualProtect(*v, sizeof(void*) * AH_DSB_COUNT, PAGE_EXECUTE_READWRITE, &old)) {
            s_ah_real_play = (ah_pfnPlay)(*v)[AH_DSB_PLAY];
            (*v)[AH_DSB_PLAY] = (void*)AH_Hook_Play;
            s_ah_real_lock = (ah_pfnLock)(*v)[AH_DSB_LOCK];
            (*v)[AH_DSB_LOCK] = (void*)AH_Hook_Lock;
            VirtualProtect(*v, sizeof(void*) * AH_DSB_COUNT, old, &old);
            alog("SOUND: IDirectSoundBuffer::Play hooked on the shared vtable (real=%p)",
                 (void*)s_ah_real_play);
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
        /* slot 3 = CreateSoundBuffer: the only route to the buffers, and so the
         * only way to observe what the game actually plays. */
        if (!s_ah_real_csb) {
            s_ah_real_csb = (ah_pfnCSB)(*vtbl)[3];
            (*vtbl)[3] = (void*)AH_Hook_CreateSoundBuffer;
            alog("SOUND: CreateSoundBuffer hooked -- sound trace live");
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

/* ---- XAudio2Create: log-only (WASAPI-backed; no cooperative-level class of
 * bug to defend against). Resolved dynamically by version -- games link
 * against ONE specific xaudio2_*.dll; ultra_iat_patch_all no-ops cleanly on
 * any name a given process doesn't actually import, so it's safe to attempt
 * every known version. */
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
void ultra_audio_install(void) {
    if (s_installed) return;   /* idempotent, matches every other *_install() here */
    s_installed = 1;

    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (!ds) ds = LoadLibraryA("dsound.dll");
    if (ds) {
        Real_DirectSoundCreate  = (pDirectSoundCreate)(void*)GetProcAddress(ds, "DirectSoundCreate");
        Real_DirectSoundCreate8 = (pDirectSoundCreate8)(void*)GetProcAddress(ds, "DirectSoundCreate8");
        ultra_iat_patch_all("dsound.dll", "DirectSoundCreate",  (void*)Hook_DirectSoundCreate);
        ultra_iat_patch_all("dsound.dll", "DirectSoundCreate8", (void*)Hook_DirectSoundCreate8);
        alog("audio: dsound.dll present, DirectSoundCreate/8 armed");
    }

    /* XAudio2: try every version the last ~15 years of Windows/DirectX shipped.
     * Each ultra_iat_patch_all call is a clean no-op if that exact DLL name
     * isn't in this process's import table. */
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
        ultra_iat_patch_all(xa2_versions[i], "XAudio2Create", (void*)Hook_XAudio2Create);
        alog("audio: %s present, XAudio2Create armed", xa2_versions[i]);
    }
}

#endif /* _WIN32 */
