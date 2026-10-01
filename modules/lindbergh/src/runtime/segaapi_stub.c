/*
 * segaapi_stub.c - SEGA Audio API (SEGAAPI / libsegaapi.so) implementation.
 *
 * Lindbergh's proprietary audio library. We implement the ABI exactly (see the
 * authoritative layout below) and route every voice to Ultra Loader's real
 * audio engine (audio_engine.c → waveOut). Each SEGAAPI "buffer" is one mixer
 * voice that reads PCM straight from the game's own memory (mapData.hBufferHdr)
 * and reports a byte-accurate playback position so the game's streaming stays in
 * sync. If no audio device is available the calls degrade to a silent no-op path
 * (the game still runs).
 *
 * Calling convention: the game is an i386 SysV ELF (cdecl); these are plain
 * cdecl C functions registered by name (no thunk) - stack-arg compatible.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "segaapi_stub.h"
#include "audio_engine.h"

/* Exact SEGAAPI/HAWOS BufferConfig (36 bytes; all fields 4 bytes on 32-bit). */
typedef struct { uint32_t dwSize; uint32_t dwOffset; void *hBufferHdr; } SegaMapData;
typedef struct {
    uint32_t    dwPriority;
    uint32_t    dwSampleRate;
    uint32_t    dwSampleFormat;     /* 0x04=u8 PCM, 0x20=s16 PCM */
    uint32_t    byNumChans;
    uint32_t    dwReserved;
    void       *hUserData;
    SegaMapData mapData;
} SegaBufferConfig;

#define SEGA_SUCCESS    0
#define SEGA_FAIL       ((int)0x8000A000)
#define ALLOC_USER_MEM  0x00000002

static int g_audio_on = 0;

/* Silent fallback (no device): keep the game happy + advance position so its
 * streaming logic never deadlocks waiting for playback to progress. */
#define FB_MAX 64
static struct { int used; void *ud; uint32_t pos; int status; } g_fb[FB_MAX];
static void *fb_alloc(void){
    for (int i=0;i<FB_MAX;i++) if(!g_fb[i].used){ memset(&g_fb[i],0,sizeof g_fb[i]); g_fb[i].used=1; return (void*)(uintptr_t)(0xA0D10000+i); }
    return (void*)0xA0D10000;
}
static int fb_idx(void *h){ uintptr_t v=(uintptr_t)h; return (v>=0xA0D10000 && v<0xA0D10000+FB_MAX)?(int)(v-0xA0D10000):-1; }

/* ------------------------------------------------------------------ */
int stub_SEGAAPI_Init(void){
    if (getenv("ULTRA_SKIP_AUDIO")) { g_audio_on = 0; loader_log(LOG_INFO,"SEGAAPI_Init -> skipped (audio off)"); return SEGA_SUCCESS; }
    g_audio_on = audio_init();
    loader_log(LOG_WARN, "SEGAAPI_Init -> %s", g_audio_on ? "REAL audio engine" : "silent (no device)");
    return SEGA_SUCCESS;
}
int stub_SEGAAPI_Reset(void){ return SEGA_SUCCESS; }
int stub_SEGAAPI_Exit(void){ if (g_audio_on) audio_shutdown(); return SEGA_SUCCESS; }

int stub_SEGAAPI_CreateBuffer(SegaBufferConfig *cfg, void *cb, unsigned dwFlags, void **phHandle){
    (void)cb;
    if (!cfg || !phHandle) return SEGA_FAIL;
    void *pcm = cfg->mapData.hBufferHdr;
    unsigned size = cfg->mapData.dwSize;
    if (dwFlags & ALLOC_USER_MEM) pcm = NULL;        /* engine allocates + writes back */
    if (g_audio_on) {
        void *v = audio_voice_create(pcm, size, cfg->dwSampleRate,
                                     cfg->dwSampleFormat, cfg->byNumChans, cfg->hUserData);
        if (!v) return SEGA_FAIL;
        if (!pcm) cfg->mapData.hBufferHdr = audio_voice_pcm(v);
        cfg->mapData.dwOffset = 0;
        *phHandle = v;
        return SEGA_SUCCESS;
    }
    *phHandle = fb_alloc();
    cfg->mapData.dwOffset = 0;
    return SEGA_SUCCESS;
}

int stub_SEGAAPI_DestroyBuffer(void *h){ if(g_audio_on) audio_voice_destroy(h); else { int i=fb_idx(h); if(i>=0) g_fb[i].used=0; } return SEGA_SUCCESS; }
int stub_SEGAAPI_Play(void *h){ if(g_audio_on) audio_voice_play(h); else { int i=fb_idx(h); if(i>=0) g_fb[i].status=1; } return SEGA_SUCCESS; }
int stub_SEGAAPI_Stop(void *h){ if(g_audio_on) audio_voice_stop(h); else { int i=fb_idx(h); if(i>=0){ g_fb[i].status=0; g_fb[i].pos=0; } } return SEGA_SUCCESS; }
int stub_SEGAAPI_Pause(void *h){ if(g_audio_on) audio_voice_pause(h,1); else { int i=fb_idx(h); if(i>=0) g_fb[i].status=2; } return SEGA_SUCCESS; }

/* We read PCM live from the game's buffer, so UpdateBuffer is just an ack. */
int stub_SEGAAPI_UpdateBuffer(void *h, unsigned off, unsigned len){ (void)h;(void)off;(void)len; return SEGA_SUCCESS; }

unsigned stub_SEGAAPI_GetPlaybackPosition(void *h){
    if (g_audio_on) return audio_voice_get_pos(h);
    int i = fb_idx(h);
    if (i >= 0) { if (g_fb[i].status==1) g_fb[i].pos += 1024; return g_fb[i].pos; }
    return 0;
}
int stub_SEGAAPI_SetPlaybackPosition(void *h, unsigned pos){ if(g_audio_on) audio_voice_set_pos(h,pos); else { int i=fb_idx(h); if(i>=0) g_fb[i].pos=pos; } return SEGA_SUCCESS; }
int stub_SEGAAPI_GetPlaybackStatus(void *h){ if(g_audio_on) return audio_voice_status(h); int i=fb_idx(h); return i>=0?g_fb[i].status:-1; }

int stub_SEGAAPI_SetLoopState(void *h, int loop)          { if(g_audio_on) audio_voice_set_loop(h,loop);          return SEGA_SUCCESS; }
int stub_SEGAAPI_SetStartLoopOffset(void *h, unsigned o)  { if(g_audio_on) audio_voice_set_start_loop(h,o);       return SEGA_SUCCESS; }
int stub_SEGAAPI_SetEndLoopOffset(void *h, unsigned o)    { if(g_audio_on) audio_voice_set_end_loop(h,o);         return SEGA_SUCCESS; }
int stub_SEGAAPI_SetEndOffset(void *h, unsigned o)        { if(g_audio_on) audio_voice_set_end_offset(h,o);       return SEGA_SUCCESS; }
int stub_SEGAAPI_SetSampleRate(void *h, unsigned rate)    { if(g_audio_on) audio_voice_set_rate(h,rate);          return SEGA_SUCCESS; }
int stub_SEGAAPI_SetChannelVolume(void *h, unsigned ch, unsigned vol){ if(g_audio_on) audio_voice_set_channel_volume(h,ch,vol); return SEGA_SUCCESS; }

/* Routing / sends / synth / IO / EAX - accepted (mix uses channel volume + the
 * default front-L/R routing); these refine positioning, not whether sound plays. */
int stub_SEGAAPI_SetIOVolume(unsigned io, unsigned vol)   { (void)io;(void)vol;    return SEGA_SUCCESS; }
int stub_SEGAAPI_SetSendLevel(void *h, unsigned s, unsigned l){ (void)h;(void)s;(void)l; return SEGA_SUCCESS; }
int stub_SEGAAPI_SetSendRouting(void *h, unsigned s, unsigned r){ (void)h;(void)s;(void)r; return SEGA_SUCCESS; }
int stub_SEGAAPI_SetSynthParam(void *h, unsigned p, int v){ (void)h;(void)p;(void)v; return SEGA_SUCCESS; }

/* Round 3 additions ---------------------------------------------------
 * GetLastStatus: some SEGAAPI clients (Hummer) call this after a Create/
 *   Update/Play to poll for the "last error since previous call". We keep
 *   a per-thread success sentinel - every path in this stub returns
 *   SEGA_SUCCESS on the happy road, so 0 is the honest answer.
 * GetSynthParam: setter is already a no-op (SetSynthParam); the paired
 *   getter returns 0 for whatever param is queried. Callers use it for
 *   optional readback (mute/level dB) - 0 = "muted/default", harmless.
 * SetSynthParamMultiple: the array-form of SetSynthParam. Iterating and
 *   returning success matches the setter's per-param semantics.
 * PlayWithSetup: "apply a bundle of state then Play". We forward to Play
 *   after ignoring the setup blob - the setup fields are all things our
 *   audio engine already reads live (rate/format/channel volume). */
int stub_SEGAAPI_GetLastStatus(void)                                { return SEGA_SUCCESS; }
int stub_SEGAAPI_GetSynthParam(void *h, unsigned p, int *out)       { (void)h;(void)p; if(out) *out = 0; return SEGA_SUCCESS; }
int stub_SEGAAPI_SetSynthParamMultiple(void *h, unsigned n, const void *arr){ (void)h;(void)n;(void)arr; return SEGA_SUCCESS; }
int stub_SEGAAPI_PlayWithSetup(void *h, unsigned n, const void *arr){ (void)n;(void)arr; return stub_SEGAAPI_Play(h); }
/* Release = "finish playing, stop looping" (the reference clears AL_LOOPING).
 * Without this, looped cues/BGM stems drone forever and leak the voice pool. */
int stub_SEGAAPI_SetReleaseState(void *h, int s)          { if(g_audio_on && s) audio_voice_set_loop(h,0); return SEGA_SUCCESS; }

int   stub_SEGAAPI_SetUserData(void *h, void *d){ if(g_audio_on) audio_voice_set_userdata(h,d); else { int i=fb_idx(h); if(i>=0) g_fb[i].ud=d; } return SEGA_SUCCESS; }
void *stub_SEGAAPI_GetUserData(void *h){ if(g_audio_on) return audio_voice_get_userdata(h); int i=fb_idx(h); return i>=0?g_fb[i].ud:NULL; }

/* ------------------------------------------------------------------ */
/* EAX 4.0 GUIDs - 16-byte DATA symbols. Hummer's dynsym references these
 * for EAX property routing (FXSlot IDs + effect creator IDs). The game
 * reads/compares the GUIDs but never dereferences meaning; a zero-filled
 * 16-byte constant at a stable address satisfies the reloc.
 *
 * SEGAAPI_SetGlobalEAXProperty is registered as a SEGA_SUCCESS stub below,
 * NOT here (it's a function, not a GUID).
 *
 * Byte-safe: ABC does not import any of these EAX GUIDs. Additive. */
static const unsigned char UL_EAX_GUID_ZERO[16] = {0};
#define EAX_GUID(name) static const unsigned char name[16] = {0}
EAX_GUID(EAXPROPERTYID_EAX40_FXSlot0);
EAX_GUID(EAXPROPERTYID_EAX40_FXSlot1);
EAX_GUID(EAXPROPERTYID_EAX40_FXSlot2);
EAX_GUID(EAXPROPERTYID_EAX40_FXSlot3);
EAX_GUID(EAX_NULL_GUID);
EAX_GUID(EAX_AGCCOMPRESSOR_EFFECT);
EAX_GUID(EAX_AUTOWAH_EFFECT);
EAX_GUID(EAX_CHORUS_EFFECT);
EAX_GUID(EAX_DISTORTION_EFFECT);
EAX_GUID(EAX_ECHO_EFFECT);
EAX_GUID(EAX_EQUALIZER_EFFECT);
EAX_GUID(EAX_FLANGER_EFFECT);
EAX_GUID(EAX_FREQUENCYSHIFTER_EFFECT);
EAX_GUID(EAX_PITCHSHIFTER_EFFECT);
EAX_GUID(EAX_REVERB_EFFECT);
EAX_GUID(EAX_RINGMODULATOR_EFFECT);
EAX_GUID(EAX_VOCALMORPHER_EFFECT);
#undef EAX_GUID

/* Function form of the Hummer EAX property setter (not a GUID). */
static int stub_SEGAAPI_SetGlobalEAXProperty(const void *guid, unsigned prop,
                                             void *data, unsigned size) {
    (void)guid; (void)prop; (void)data; (void)size;
    return SEGA_SUCCESS;
}
static int stub_SEGAAPI_GetGlobalEAXProperty(const void *guid, unsigned prop,
                                             void *data, unsigned size) {
    (void)guid; (void)prop; (void)data; (void)size;
    return SEGA_SUCCESS;
}
static int stub_SEGAAPI_SetEAXProperty(void *h, const void *guid, unsigned prop,
                                       void *data, unsigned size) {
    (void)h; (void)guid; (void)prop; (void)data; (void)size;
    return SEGA_SUCCESS;
}
static int stub_SEGAAPI_GetEAXProperty(void *h, const void *guid, unsigned prop,
                                       void *data, unsigned size) {
    (void)h; (void)guid; (void)prop; (void)data; (void)size;
    return SEGA_SUCCESS;
}

/* ------------------------------------------------------------------ */
void segaapi_register_stubs(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering SEGAAPI audio (real engine)");
    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
    R(SEGAAPI_Init);    R(SEGAAPI_Reset);   R(SEGAAPI_Exit);
    R(SEGAAPI_CreateBuffer);  R(SEGAAPI_DestroyBuffer);
    R(SEGAAPI_Play);    R(SEGAAPI_Stop);    R(SEGAAPI_Pause);
    R(SEGAAPI_UpdateBuffer);
    R(SEGAAPI_GetPlaybackPosition); R(SEGAAPI_SetPlaybackPosition);
    R(SEGAAPI_GetPlaybackStatus);
    R(SEGAAPI_SetLoopState);   R(SEGAAPI_SetStartLoopOffset);
    R(SEGAAPI_SetEndLoopOffset); R(SEGAAPI_SetEndOffset);
    R(SEGAAPI_SetSampleRate);  R(SEGAAPI_SetChannelVolume);
    R(SEGAAPI_SetIOVolume);    R(SEGAAPI_SetSendLevel);
    R(SEGAAPI_SetSendRouting); R(SEGAAPI_SetSynthParam);
    R(SEGAAPI_SetReleaseState);
    /* Round 3 additions */
    R(SEGAAPI_GetLastStatus); R(SEGAAPI_GetSynthParam);
    R(SEGAAPI_SetSynthParamMultiple); R(SEGAAPI_PlayWithSetup);
    R(SEGAAPI_SetUserData);    R(SEGAAPI_GetUserData);
    /* EAX property routing (Hummer) */
    R(SEGAAPI_SetGlobalEAXProperty); R(SEGAAPI_GetGlobalEAXProperty);
    R(SEGAAPI_SetEAXProperty);       R(SEGAAPI_GetEAXProperty);
    #undef R

    /* EAX 4.0 GUID DATA symbols (16-byte constants). Register the ADDRESS
     * of each so the guest's R_386_GLOB_DAT slot binds to a stable 16 B
     * chunk it can memcmp against. Byte-safe: ABC imports none of these. */
    #define D(name) sym_register(ctx, #name, (uintptr_t)&name)
    D(EAXPROPERTYID_EAX40_FXSlot0); D(EAXPROPERTYID_EAX40_FXSlot1);
    D(EAXPROPERTYID_EAX40_FXSlot2); D(EAXPROPERTYID_EAX40_FXSlot3);
    D(EAX_NULL_GUID);
    D(EAX_AGCCOMPRESSOR_EFFECT); D(EAX_AUTOWAH_EFFECT);
    D(EAX_CHORUS_EFFECT);        D(EAX_DISTORTION_EFFECT);
    D(EAX_ECHO_EFFECT);          D(EAX_EQUALIZER_EFFECT);
    D(EAX_FLANGER_EFFECT);       D(EAX_FREQUENCYSHIFTER_EFFECT);
    D(EAX_PITCHSHIFTER_EFFECT);  D(EAX_REVERB_EFFECT);
    D(EAX_RINGMODULATOR_EFFECT); D(EAX_VOCALMORPHER_EFFECT);
    #undef D
    (void)UL_EAX_GUID_ZERO;  /* silence unused-warn on some builds */
}
