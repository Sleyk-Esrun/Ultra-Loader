/*
 * audio_engine.c - Ultra Loader software mixer + waveOut sink.
 *
 * Float mixer: up to MAX_VOICES voices, each resampled (linear) from its source
 * rate to the 48 kHz stereo mix, format-converted (u8/s16), gained/panned, and
 * accumulated; the master mix is clamped and written as 16-bit to waveOut on a
 * dedicated thread (event-driven, NUM_BLK blocks of BLK_FRAMES). Voices read PCM
 * directly from the game's memory; positions are byte-accurate so the game's
 * SEGAAPI streaming logic stays in sync.
 *
 * Optional ULTRA_AUDIO_WAV=<path> taps the final mix to a 16-bit WAV (for
 * objective verification - RMS/spectrum - since the build can't "listen").
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "audio_engine.h"

#define MIX_RATE    48000
#define MIX_CH      2
#define BLK_FRAMES  480            /* 10 ms */
#define NUM_BLK     8              /* ~80 ms buffer: rides out frame hitches on
                                    * heavy events (explosion/crash) without an
                                    * underrun. The TIME_CRITICAL/MMCSS audio
                                    * thread keeps this topped up; the depth is
                                    * insurance for the worst spikes. */
#define MAX_VOICES  64

typedef struct {
    volatile LONG used;
    volatile LONG status;          /* 0 stop, 1 active, 2 pause */
    unsigned char *pcm;            /* PCM source (game memory or engine-owned) */
    int          owned;            /* engine allocated pcm -> free on destroy */
    unsigned int size;             /* bytes */
    unsigned int rate;             /* source Hz */
    unsigned int fmt;              /* AUDIO_FMT_U8 / AUDIO_FMT_S16 */
    unsigned int chans;            /* 1 or 2 */
    unsigned int bpf;              /* bytes per frame = chans * (s16?2:1) */
    double       cursor;           /* source frame position (fractional) */
    int          loop;
    unsigned int startLoop, endLoop, endOffset;   /* bytes */
    float        gainL, gainR;
    float        env;             /* declick fade-in (0..1); reset to 0 on play */
    void        *userdata;
    unsigned     gen;              /* bumped on create; encoded in the handle (ABA guard) */
    volatile LONG posBytes;        /* lock-free playback position (bytes) */
    /* One-pole LP state for anti-aliasing on downsampling paths (step>1.0):
     * filters the source-rate stream before the polynomial interp, killing
     * the harsh "buzz" aliasing on bright SFX (explosions, torpedo lock beep). */
    float        aaL, aaR;
} Voice;

static Voice            g_voices[MAX_VOICES];
static CRITICAL_SECTION g_lock;
static HWAVEOUT         g_hwo;
static WAVEHDR          g_hdr[NUM_BLK];
static short            g_blk[NUM_BLK][BLK_FRAMES * MIX_CH];
static HANDLE           g_evt, g_thread;
static volatile LONG    g_run;
static int              g_on;
/* Master 1.0 (was 0.8): the headroom comes from the new dB-domain limiter +
 * soft knee, so the bus no longer needs to be pre-attenuated to avoid clip. */
static float            g_master = 1.0f;
static volatile LONG    g_audio_paused = 0;   /* 1 = pause-menu silence: emit zeros, freeze voice cursors */
static FILE            *g_wav; static unsigned g_wav_bytes;

/* Opaque voice handle = tag | index | generation. The generation guards against
 * ABA: a stale handle held past DestroyBuffer won't match a recycled slot. */
static void *vh_make(int idx, unsigned gen){
    return (void*)(uintptr_t)(0x5A000000u | ((unsigned)(idx & 0x3F) << 16) | (gen & 0xFFFF));
}
/* Resolve a handle to its live voice. Call ONLY while holding g_lock (reads
 * used/gen). Returns NULL for a stale/invalid handle. */
static Voice *vh_get(void *h){
    uintptr_t id = (uintptr_t)h;
    if ((id & 0xFF000000u) != 0x5A000000u) return NULL;
    int idx = (int)((id >> 16) & 0x3F);
    unsigned gen = (unsigned)(id & 0xFFFF);
    if (idx < 0 || idx >= MAX_VOICES) return NULL;
    Voice *V = &g_voices[idx];
    return (V->used && V->gen == gen) ? V : NULL;
}

/* ---- mixer ----------------------------------------------------------------*/
static inline void read_frame(Voice *V, unsigned f, int s16, int st, float *L, float *R){
    const unsigned char *p = V->pcm + (size_t)f * V->bpf;
    if (s16) {
        const int16_t *sp = (const int16_t*)p;
        float a = sp[0] * (1.0f/32768.0f);
        *L = a; *R = st ? sp[1] * (1.0f/32768.0f) : a;
    } else {
        float a = (p[0] - 128) * (1.0f/128.0f);
        *L = a; *R = st ? (p[1] - 128) * (1.0f/128.0f) : a;
    }
}

/* Hermite (Catmull-Rom) 4-tap interpolation. Continuous + 1st-derivative
 * continuous across sample boundaries -> no audible "stair-step" alias the
 * linear lerp left on rate-mismatched SFX (22 kHz -> 48 kHz mix). The 4-tap
 * footprint reads idx-1..idx+2; near loop ends we fall back to the linear
 * 2-tap path so the loop seam stays smooth. ~4x the arithmetic of linear but
 * trivially below mixer budget at 480 frames * 64 voices. */
static inline float hermite4(float xm1, float x0, float x1, float x2, float t){
    /* Catmull-Rom Hermite (the classic 1/2 form). t in [0,1]. */
    float c0 = x0;
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

static void mix_block(short *out, int frames){
    static float acc[BLK_FRAMES * MIX_CH];
    if (InterlockedCompareExchange(&g_audio_paused, 0, 0)) {
        /* Pause menu is up: emit silence and DON'T advance any voice cursor or
         * posBytes - so playback (and the game's SEGAAPI playback-position reads)
         * resume exactly where they paused. No skip, no click, no streaming desync.
         * Lock-free check so the TIME_CRITICAL mix thread never blocks on it. */
        memset(out, 0, (size_t)frames * MIX_CH * sizeof(short));
        return;
    }
    memset(acc, 0, sizeof(float) * frames * MIX_CH);

    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *V = &g_voices[vi];
        if (!V->used || V->status != 1 || !V->pcm || V->bpf == 0) continue;

        const double step = (double)V->rate / (double)MIX_RATE;
        const int    s16  = (V->fmt == AUDIO_FMT_S16);
        const int    st   = (V->chans >= 2);
        const unsigned maxF = V->size / V->bpf;
        unsigned endB = V->loop ? V->endLoop : V->endOffset;
        if (endB == 0 || endB > V->size) endB = V->size;
        unsigned endF   = endB / V->bpf;            if (endF > maxF) endF = maxF;
        unsigned loopF  = V->startLoop / V->bpf;     if (loopF >= endF) loopF = 0;
        if (endF == 0) continue;
        const float gL = V->gainL * g_master, gR = V->gainR * g_master;  /* mono pans via both ch gains (default 1/1 = centered) */
        float env = V->env; const float envStep = 1.0f/64.0f;   /* ~1.3ms fade-in -> no click on (re)trigger */

        /* Anti-aliasing low-pass coefficient: only engages when downsampling
         * (step > 1.0). A one-pole shaped so the cutoff sits around (Nyquist /
         * step); coef = 1 / step keeps the 22kHz SFX-into-48kHz mix path clean.
         * step < 1 (upsampling) leaves the signal untouched. */
        const float aa_c = (step > 1.0001) ? (float)(1.0 / step) : 1.0f;
        float aaL = V->aaL, aaR = V->aaR;

        double cur = V->cursor;
        for (int i = 0; i < frames; i++) {
            if (cur >= (double)endF) {
                if (V->loop) {
                    double span = (double)(endF - loopF);
                    if (span > 0.0) { int g=0; while (cur >= (double)endF && g++ < 8) cur -= span;
                                      if (cur >= (double)endF) cur = (double)loopF; }
                    else cur = loopF;
                } else {
                    V->status = 0;                   /* one-shot finished */
                    break;
                }
            }
            unsigned idx = (unsigned)cur;
            float frac = (float)(cur - idx);

            float L, R;
            /* 4-tap hermite when there's safe headroom on BOTH sides of idx;
             * else fall back to linear (loop seam / start-of-buffer). */
            int can_hermite = (idx >= 1) && (idx + 2 < endF);
            if (can_hermite) {
                float lm1,rm1, l0,r0, l1,r1, l2,r2;
                read_frame(V, idx - 1, s16, st, &lm1, &rm1);
                read_frame(V, idx,     s16, st, &l0,  &r0);
                read_frame(V, idx + 1, s16, st, &l1,  &r1);
                read_frame(V, idx + 2, s16, st, &l2,  &r2);
                L = hermite4(lm1, l0, l1, l2, frac);
                R = hermite4(rm1, r0, r1, r2, frac);
            } else {
                unsigned idx2 = idx + 1;
                if (idx2 >= endF) idx2 = V->loop ? loopF : idx;
                float l0,r0,l1,r1;
                read_frame(V, idx,  s16, st, &l0, &r0);
                read_frame(V, idx2, s16, st, &l1, &r1);
                L = l0 + (l1 - l0) * frac;
                R = r0 + (r1 - r0) * frac;
            }

            /* Anti-aliasing LPF on downsample (no-op when step<=1.0): smooths
             * the high-frequency residue the polynomial interp can't kill. */
            if (aa_c < 1.0f) {
                aaL += aa_c * (L - aaL); L = aaL;
                aaR += aa_c * (R - aaR); R = aaR;
            }

            acc[i*2+0] += L * gL * env;
            acc[i*2+1] += R * gR * env;
            if (env < 1.0f) { env += envStep; if (env > 1.0f) env = 1.0f; }
            cur += step;
        }
        V->cursor = cur; V->env = env;
        V->aaL = aaL; V->aaR = aaR;
        unsigned pb = (unsigned)(cur * V->bpf);
        if (pb > V->size) pb = V->size;
        InterlockedExchange(&V->posBytes, (LONG)pb);
    }
    LeaveCriticalSection(&g_lock);

    /* MASTER LIMITER - dB-domain release, ~250 ms time constant. The old path
     * released ~80 ms (0.08 per 10 ms block, linear) which "pumped" audibly on
     * bass-heavy music beds when explosions ducked the bus. Slower release =
     * the recovery is subliminal; you hear the loudness ceiling, not the
     * limiter breathing. Ceiling lowered to 0.90 to leave the soft-knee
     * saturator below clear headroom (master is now 1.0 vs the old 0.8). */
    static float limGain = 1.0f;
    float pk = 0.0f;
    for (int i = 0; i < frames * MIX_CH; i++) { float a = acc[i] < 0 ? -acc[i] : acc[i]; if (a > pk) pk = a; }
    if (pk > 0.0001f) { float need = (pk > 0.90f) ? 0.90f / pk : 1.0f; if (need < limGain) limGain = need; } /* instant catch */
    /* Release: per-10ms-block step toward unity such that the time constant is
     * ~250 ms. 1 - exp(-10/250) ~= 0.039. */
    limGain += (1.0f - limGain) * 0.039f;
    if (limGain > 1.0f) limGain = 1.0f;
    if (limGain < 0.999f) for (int i = 0; i < frames * MIX_CH; i++) acc[i] *= limGain;

    /* Soft-knee limiter: transparent below 0.75, smoothly saturates 0.75..1.0,
     * hard caps at 1.0 - clean peaks (no harsh hard-clip) without a libm dep. */
    for (int i = 0; i < frames * MIX_CH; i++) {
        float s = acc[i];
        float a = s < 0.0f ? -s : s, sign = s < 0.0f ? -1.0f : 1.0f;
        if (a > 0.75f) {
            if (a >= 1.25f) a = 1.0f;
            else { float k = (a - 0.75f) * 2.0f; a = 0.75f + 0.25f * (k * (2.0f - k)); }
            s = sign * a;
        }
        out[i] = (short)(s * 32767.0f);
    }
}

/* ---- waveOut sink ---------------------------------------------------------*/
/* Audio gate (set by the shell EXE before parallel dispatch, cleared by the
 * boot console when its fade-out starts). When set, the game's PCM is
 * silently discarded -- the host receives zero-fill samples so no game
 * audio leaks while the boot console is up. Defined in splash.c. */
extern volatile int g_ul_audio_gated;
static void fill_block(int b){
    mix_block(g_blk[b], BLK_FRAMES);
    if (g_ul_audio_gated) {
        memset(g_blk[b], 0, BLK_FRAMES * MIX_CH * sizeof(short));
    }
    g_hdr[b].dwBufferLength = BLK_FRAMES * MIX_CH * sizeof(short);
    waveOutWrite(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
    if (g_wav) { fwrite(g_blk[b], 1, BLK_FRAMES * MIX_CH * sizeof(short), g_wav);
                 g_wav_bytes += BLK_FRAMES * MIX_CH * sizeof(short); fflush(g_wav); }
}

static DWORD WINAPI audio_thread(LPVOID arg){
    (void)arg;
    /* The audio thread MUST stay scheduled through game CPU spikes (explosions,
     * crashes) or the buffer underruns and the sound stutters/freezes. Raise to
     * TIME_CRITICAL and, when available, register with MMCSS "Pro Audio" so the
     * OS scheduler favors it under load. The thread sleeps on g_evt between tiny
     * mixes, so this is safe (it does not busy-spin or starve other work). */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMODULE avrt = LoadLibraryA("avrt.dll");
    HANDLE (WINAPI *AvSet)(LPCWSTR, LPDWORD) = avrt ?
        (HANDLE (WINAPI*)(LPCWSTR,LPDWORD))GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
    DWORD mmidx = 0; HANDLE mm = AvSet ? AvSet(L"Pro Audio", &mmidx) : NULL;
    if (mm) loader_log(LOG_WARN, "[AUDIO] mix thread: TIME_CRITICAL + MMCSS Pro Audio");
    else    loader_log(LOG_WARN, "[AUDIO] mix thread: TIME_CRITICAL (MMCSS unavailable)");

    for (int b = 0; b < NUM_BLK; b++) fill_block(b);     /* prime all blocks */
    unsigned under = 0;
    while (InterlockedCompareExchange(&g_run, 1, 1)) {
        WaitForSingleObject(g_evt, 100);
        int done = 0;
        for (int b = 0; b < NUM_BLK; b++)
            if (g_hdr[b].dwFlags & WHDR_DONE) { fill_block(b); done++; }
        /* Every block already drained when we woke = the whole buffer ran dry
         * during a spike (a true underrun). Log sparsely; with the deeper buffer
         * + priority this should be rare-to-never. */
        if (done >= NUM_BLK && (under++ % 32) == 0)
            loader_log(LOG_WARN, "[AUDIO] underrun: buffer drained during a CPU spike (x%u)", under);
    }
    if (mm && avrt) { void (WINAPI *AvRev)(HANDLE) =
        (void (WINAPI*)(HANDLE))GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"); if (AvRev) AvRev(mm); }
    return 0;
}

static void wav_open(void){
    const char *p = getenv("ULTRA_AUDIO_WAV");
    if (!p || !*p) return;
    g_wav = fopen(p, "wb");
    if (!g_wav) return;
    /* 44-byte canonical WAV header; sizes patched on shutdown (PCM is still
     * readable from offset 44 even if the process is killed before patch). */
    unsigned dr = MIX_RATE, br = MIX_RATE * MIX_CH * 2; unsigned short ch = MIX_CH, ba = MIX_CH*2, bps = 16, fmt = 1;
    fwrite("RIFF", 1, 4, g_wav); unsigned z = 0xFFFFFFFF; fwrite(&z,4,1,g_wav);
    fwrite("WAVEfmt ", 1, 8, g_wav); unsigned sixteen = 16; fwrite(&sixteen,4,1,g_wav);
    fwrite(&fmt,2,1,g_wav); fwrite(&ch,2,1,g_wav); fwrite(&dr,4,1,g_wav);
    fwrite(&br,4,1,g_wav); fwrite(&ba,2,1,g_wav); fwrite(&bps,2,1,g_wav);
    fwrite("data",1,4,g_wav); fwrite(&z,4,1,g_wav);
    loader_log(LOG_WARN, "[AUDIO] capturing mix to %s", p);
}

int audio_init(void){
    if (g_on) return 1;
    InitializeCriticalSection(&g_lock);
    { const char *e = getenv("ULTRA_AUDIO_VOL"); if (e && *e) audio_set_master_volume(atoi(e)); }

    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = MIX_CH; wf.nSamplesPerSec = MIX_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = MIX_CH * 2; wf.nAvgBytesPerSec = MIX_RATE * MIX_CH * 2;

    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    MMRESULT r = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, (DWORD_PTR)g_evt, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { loader_log(LOG_WARN, "[AUDIO] waveOutOpen failed (%u), silent", r); return 0; }

    for (int b = 0; b < NUM_BLK; b++) {
        memset(&g_hdr[b], 0, sizeof(WAVEHDR));
        g_hdr[b].lpData = (LPSTR)g_blk[b];
        g_hdr[b].dwBufferLength = sizeof(g_blk[b]);
        waveOutPrepareHeader(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
        g_hdr[b].dwFlags |= WHDR_DONE;
    }
    wav_open();
    g_run = 1; g_on = 1;
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    loader_log(LOG_WARN, "[AUDIO] engine started: 48kHz stereo, waveOut, %d voices", MAX_VOICES);
    return 1;
}

void audio_shutdown(void){
    if (!g_on) return;
    InterlockedExchange(&g_run, 0);
    if (g_evt) SetEvent(g_evt);                 /* wake the thread to observe g_run */
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
    waveOutReset(g_hwo);                         /* thread is gone -> safe to tear down */
    for (int b = 0; b < NUM_BLK; b++) waveOutUnprepareHeader(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
    waveOutClose(g_hwo);
    if (g_wav) {
        unsigned data = g_wav_bytes, riff = data + 36;
        fseek(g_wav, 4, SEEK_SET);  fwrite(&riff, 4, 1, g_wav);
        fseek(g_wav, 40, SEEK_SET); fwrite(&data, 4, 1, g_wav);
        fclose(g_wav); g_wav = NULL;
    }
    g_on = 0;
}

int  audio_is_on(void){ return g_on; }
void audio_set_master_volume(int v){ if (v<0)v=0; if (v>100)v=100; g_master = v / 100.0f; }
void audio_set_pause(int pause){ InterlockedExchange(&g_audio_paused, pause ? 1 : 0); }

/* ---- voice API ------------------------------------------------------------*/
/* All accessors take g_lock and resolve the handle via vh_get() (generation-
 * checked) - the mix thread holds g_lock while reading these same fields, and
 * destroy frees under it, so there are no torn reads, OOB indexing, or
 * use-after-free / ABA races. */
void* audio_voice_create(void* pcm, unsigned size, unsigned rate,
                         unsigned fmt, unsigned chans, void* ud){
    if (!g_on || size == 0) return NULL;
    EnterCriticalSection(&g_lock);
    void *handle = NULL; int idx = -1;
    for (int i = 0; i < MAX_VOICES; i++) if (!g_voices[i].used) { idx = i; break; }
    if (idx < 0) {
        /* VOICE STEALING: a burst of SFX (explosion/crash) can exhaust all 64
         * voices. Rather than silently drop the new sound, reclaim a finished/
         * stopped one-shot first; if all are still playing, steal the furthest-
         * along non-looping one-shot. Never steal looping music. */
        for (int i = 0; i < MAX_VOICES; i++) if (g_voices[i].status == 0) { idx = i; break; }
        if (idx < 0) {
            double best = -1.0;
            for (int i = 0; i < MAX_VOICES; i++) {
                Voice *v = &g_voices[i];
                if (!v->loop && v->bpf && v->size) {
                    double prog = (v->cursor * v->bpf) / (double)v->size;
                    if (prog > best) { best = prog; idx = i; }
                }
            }
        }
    }
    if (idx >= 0) {
        Voice *V = &g_voices[idx];
        if (V->owned && V->pcm) free(V->pcm);   /* reclaiming a stolen slot: don't leak engine-owned PCM */
        unsigned gen = (V->gen + 1) & 0xFFFF; if (gen == 0) gen = 1;
        memset(V, 0, sizeof *V);
        V->gen = gen; V->used = 1; V->status = 0;
        V->size = size; V->rate = rate ? rate : MIX_RATE;
        if (fmt != AUDIO_FMT_U8 && fmt != AUDIO_FMT_S16)
            loader_log(LOG_WARN, "[AUDIO] unknown sample format 0x%x -> assuming s16", fmt);
        V->fmt = (fmt == AUDIO_FMT_U8 || fmt == AUDIO_FMT_S16) ? fmt : AUDIO_FMT_S16;
        V->chans = chans ? chans : 1;
        V->bpf = V->chans * ((V->fmt == AUDIO_FMT_S16) ? 2 : 1);
        if (pcm) { V->pcm = (unsigned char*)pcm; V->owned = 0; }
        else     { V->pcm = (unsigned char*)calloc(1, size); V->owned = 1; }
        V->startLoop = 0; V->endLoop = size; V->endOffset = size; V->loop = 0;
        V->gainL = 1.0f; V->gainR = 1.0f; V->userdata = ud;
        if (!V->pcm) V->used = 0; else handle = vh_make(idx, gen);
    }
    LeaveCriticalSection(&g_lock);
    return handle;
}
#define LK   EnterCriticalSection(&g_lock)
#define UNLK LeaveCriticalSection(&g_lock)
void* audio_voice_pcm(void* h){ LK; Voice*V=vh_get(h); void*p=V?V->pcm:NULL; UNLK; return p; }
void  audio_voice_destroy(void* h){ LK; Voice*V=vh_get(h); if(V){ V->status=0; if(V->owned&&V->pcm) free(V->pcm); V->pcm=NULL; V->used=0; } UNLK; }
void  audio_voice_play(void* h){ LK; Voice*V=vh_get(h); if(V){ if(!V->loop && V->cursor*V->bpf >= (double)V->endOffset){ V->cursor=0; InterlockedExchange(&V->posBytes,0);} V->env=0.0f; V->aaL=0.0f; V->aaR=0.0f; V->status=1; } UNLK; }
void  audio_voice_stop(void* h){ LK; Voice*V=vh_get(h); if(V){ V->status=0; V->cursor=0; InterlockedExchange(&V->posBytes,0);} UNLK; }

/* Snapshot support: serialize the mix thread around an in-place guest-RAM restore
 * (Voice.pcm aliases guest RAM, so a memcpy over it must not race a mixer read),
 * then drop all voice cursors so the game re-issues SEGAAPI playback cleanly. */
void  audio_lock(void){ EnterCriticalSection(&g_lock); }
void  audio_unlock(void){ LeaveCriticalSection(&g_lock); }
void  audio_reset_all_voices(void){ LK; for(int i=0;i<MAX_VOICES;i++){ Voice*V=&g_voices[i]; if(V->used){ V->status=0; V->cursor=0; InterlockedExchange(&V->posBytes,0);} } UNLK; }

/* Snapshot audio state: the whole voice table IS the engine's playback state
 * (status/cursor/posBytes that the game reads back via SEGAAPI to pace itself).
 * Capturing+restoring it rolls the audio engine back in lockstep with the game's
 * memory, so the game's free-buffer math stays sane after a restore (otherwise
 * the playback position is live/drained and the sample count goes garbage).
 * pcm pointers alias guest memory, which is restored to the same addresses. */
int  audio_state_size(void){ return (int)sizeof(g_voices); }
void audio_state_save(void* dst){ if(!dst) return; LK; memcpy(dst, g_voices, sizeof g_voices); UNLK; }
void audio_state_restore(const void* src){ if(!src) return; LK; memcpy(g_voices, src, sizeof g_voices); UNLK; }
void  audio_voice_pause(void* h, int p){ LK; Voice*V=vh_get(h); if(V){ if(p){ if(V->status==1) V->status=2; } else { if(V->status==2) V->status=1; } } UNLK; }  /* never resurrect a finished voice */
int   audio_voice_status(void* h){ LK; Voice*V=vh_get(h); int s=V?(int)V->status:-1; UNLK; return s; }  /* -1 = invalid handle */
unsigned audio_voice_get_pos(void* h){ LK; Voice*V=vh_get(h); unsigned p=V?(unsigned)V->posBytes:0; UNLK; return p; }
void  audio_voice_set_pos(void* h, unsigned b){ LK; Voice*V=vh_get(h); if(V){ if(b>V->size)b=V->size; if(V->bpf){ V->cursor=(double)b/V->bpf; InterlockedExchange(&V->posBytes,(LONG)b);} } UNLK; }
void  audio_voice_set_loop(void* h, int l){ LK; Voice*V=vh_get(h); if(V) V->loop=l; UNLK; }
void  audio_voice_set_start_loop(void* h, unsigned b){ LK; Voice*V=vh_get(h); if(V){ if(V->bpf) b-=b%V->bpf; V->startLoop=b; } UNLK; }
void  audio_voice_set_end_loop(void* h, unsigned b){ LK; Voice*V=vh_get(h); if(V){ if(V->bpf) b-=b%V->bpf; V->endLoop=b; } UNLK; }
void  audio_voice_set_end_offset(void* h, unsigned b){ LK; Voice*V=vh_get(h); if(V){ if(V->bpf) b-=b%V->bpf; V->endOffset=b; } UNLK; }
void  audio_voice_set_rate(void* h, unsigned r){ LK; Voice*V=vh_get(h); if(V&&r) V->rate=r; UNLK; }
void  audio_voice_set_channel_volume(void* h, unsigned ch, unsigned vol){ LK; Voice*V=vh_get(h); if(V){ float g=(float)((double)vol/(double)0xFFFFFFFFu); if(ch==0)V->gainL=g; else if(ch==1)V->gainR=g; } UNLK; }
void  audio_voice_set_userdata(void* h, void* ud){ LK; Voice*V=vh_get(h); if(V) V->userdata=ud; UNLK; }
void* audio_voice_get_userdata(void* h){ LK; Voice*V=vh_get(h); void*u=V?V->userdata:NULL; UNLK; return u; }
