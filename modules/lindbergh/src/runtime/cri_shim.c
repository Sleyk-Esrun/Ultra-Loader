/*
 * cri_shim.c
 * Clean-room shims for the Sega/CRI sound + movie middleware that Ghost Squad
 * Evolution (GSEVO, "vsg") loads via dlopen("./snd_util.so") /
 * dlopen("libkswapapi.so") and resolves by dlsym(), caching the returned
 * pointers and CALLING them unconditionally.
 *
 * The middleware families (all dlsym'd by GSEVO, verified from the vsg ELF
 * .symtab + the live dlsym log):
 *   - CRI ADXM   (so_ADXM_*)      ADX framework setup / per-frame exec / errcb
 *   - CRI ADXLINUX (so_ADXLINUX_*) Linux ADX sound/file subsystem bring-up
 *   - CRI ADXT   (so_ADXT_*)      streaming ADX "talker" voices (play/stop/stat)
 *   - CRI Sofdec movie (so_mwPly*) MPEG/Sofdec movie player
 *   - sndoutput* (snd_util.so)    raw sound-output ring (init/exec/destroy)
 *   - kswap_collect (libkswapapi) frame-swap sync collector
 *
 * CLEAN-ROOM: these shims re-implement only the PUBLIC API CONTRACT of the
 * CRI ADX/ADXT/ADXM/Sofdec + sndoutput interfaces as documented, plus the
 * OBSERVED usage in the vsg ELF (disassembled: adx_interface_init caches the
 * pointers; adx_init/movie_init/sfd_interface_init call them; adx_get_stat
 * interprets ADXT GetStat where ==3 means PLAYEND and otherwise the raw value
 * is returned, so 0=ADXT_STAT_STOP marks a voice as idle/done). NO CRI SDK or
 * competitor source was used - same clean-room policy as the SEGAAPI work.
 *
 * STRATEGY (this push): SURVIVE init + reach a frame with SAFE SILENCE. Calls
 * that the game treats as returning a HANDLE return a non-NULL pointer into a
 * large, zero-initialized dummy struct (so any field the game later
 * dereferences reads as 0). Status queries return the "idle/finished" value so
 * the game's wait loops complete instantly. Everything else is a success
 * no-op. No actual ADX/PCM decode happens here yet -> AUDIO IS SILENT.
 *
 * ABC SAFETY: this dispatch is keyed on symbol names that After Burner Climax
 * never dlsym's (ABC uses SEGAAPI directly, not snd_util.so), so registering it
 * is purely additive - stub_dlsym_v2 only reaches cri_shim_lookup() after libGL
 * and the loader symbol table both miss, which never happens for ABC's symbols.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "loader.h"

/* ------------------------------------------------------------------ */
/* Dummy handles                                                       */
/* ------------------------------------------------------------------ */
/* The game derefs handles returned by Create/SetupFramework/Open. Give each
 * handle class its own big zeroed block so reads at any plausible offset land
 * on readable zero memory (no fault) and any function-pointer the game might
 * fetch FROM the handle reads as 0 (it never calls through a 0 because the
 * middleware code that would is in the stubbed .so, which the game does not
 * execute - it only calls the dlsym'd entry points, which are OUR shims).
 *
 * Size: 64 KiB per class is far larger than any real CRI handle struct, so a
 * deref at any field offset the game uses is in-bounds. Static + zero-init. */
#define CRI_HANDLE_BYTES (64 * 1024)

static uint8_t g_adxm_framework[CRI_HANDLE_BYTES];   /* so_ADXM_SetupFramework */
static uint8_t g_adxt_voice[CRI_HANDLE_BYTES];        /* so_ADXT_Create        */
static uint8_t g_mwply_movie[CRI_HANDLE_BYTES];       /* so_mwPlyCreateSofdec  */
static uint8_t g_sndout_handle[CRI_HANDLE_BYTES];     /* sndoutputInit         */
static uint8_t g_generic_handle[CRI_HANDLE_BYTES];    /* any other "creator"   */

/* ADXT_STAT_* (public CRI ADXT contract):
 *   STOP=0  DECINFO=1  PLAYING=2  PLAYEND=3  ERROR=4
 * adx_get_stat() treats !=3 as the literal status and 3 (PLAYEND) as a cue to
 * read GetStatPause. Returning STOP(0) means "voice idle/finished" -> the
 * game's "wait until the SE/voice stops" loops complete immediately. */
#define ADXT_STAT_STOP   0
#define ADXT_STAT_PLAYING 2   /* CRI: PREP=1, PLAYING=2, PLAYEND=3 */

/* mwPLY status. GSEVO's movie state machine (RE'd from movie_ctrl @0x080f719a,
 * is_movie_finished @0x080f7a2e, start_movie @0x080f7826) does NOT use the raw
 * CRI Sofdec enum directly - it keeps its OWN movie state in [0x86604a4]
 * (0=idle, 1=starting, 2=playing, 3=finished) and, while playing (state 2),
 * polls so_mwPlyGetStat (slot [0x8660478]) each frame:
 *      eax = GetStat();  cmp eax,3;  je <set state=3 / finished>
 * i.e. the game advances PAST the movie ONLY when GetStat returns exactly 3.
 * If GetStat never returns 3 the movie state stays at 2 forever, is_movie_
 * finished() is never true, and the title sits on a BLACK SCREEN waiting for a
 * cutscene that never ends (the exact post-tutorial hang we observed).
 *
 * Since these shims do NO real Sofdec decode, we make every movie "complete
 * instantly": report PLAYING (2) for a couple of polls so any per-frame movie
 * housekeeping runs at least once, then PLAYEND (3) so the game's wait loop
 * exits and the next scene starts. The play counter is (re)armed by
 * so_mwPlyStartFname so each new movie gets the same brief play->end arc.
 *
 * Enum values used here match the game's own check (3 == its "ended" sentinel,
 * which equals the CRI Sofdec MWE_MV_PLY_STAT_PLAYEND class the game maps to). */
#define MWPLY_STAT_PLAYING  2
#define MWPLY_STAT_PLAYEND  3

/* Per-movie play-progress counter. start_movie -> StartFname arms it to N; each
 * GetStat poll decrements; while >0 report PLAYING(2), at 0 report PLAYEND(3).
 * Small (a handful of frames) so the movie ends fast but not on the very first
 * poll, letting movie_ctrl's per-frame path execute at least once. */
static volatile int g_mwply_play_ctr = 0;
#define MWPLY_PLAY_FRAMES  3

/* ------------------------------------------------------------------ */
/* Shim implementations (all cdecl - the game calls via call [slot] with        */
/* cdecl arg passing; cdecl is the 32-bit mingw default, caller cleans stack).  */
/* ------------------------------------------------------------------ */

/* ---- CRI ADXM (framework) ---- */
static void *cri_ADXM_SetupFramework(void *cfg, int mode) { (void)cfg;(void)mode; return g_adxm_framework; }
static int   cri_ADXM_SetCbErr(void *cb, void *obj)       { (void)cb;(void)obj; return 0; }
static int   cri_ADXM_ExecMain(void)                      { return 0; }
static int   cri_ADXM_ShutdownFramework(void)             { return 0; }

/* ---- CRI ADXLINUX (Linux sound/file subsystem) ---- */
extern int  audio_init(void);
extern void audio_shutdown(void);
extern void audio_voice_stop(void *h);
extern void audio_voice_destroy(void *h);
extern int  audio_voice_status(void *h);
extern void *ul_adx_play_file(const char *path);

static void *cri_ADXLINUX_SetupSound(void *arg)           {
    (void)arg;
    /* Open the waveOut device so the ADX voices we'll create actually play. */
    audio_init();
    return g_generic_handle;
}
static int   cri_ADXLINUX_SetupFileSystem(void *arg)      { (void)arg; return 0; }
static int   cri_ADXLINUX_ShutdownSound(void)             { audio_shutdown(); return 0; }
static int   cri_ADXLINUX_ShutdownAnsiFs(void)            { return 0; }

/* ---- CRI ADXT (streaming voices) ---- */
/* We map ADXT voice handles to audio_engine voice handles. The game's Create()
 * gets a stable opaque pointer (g_adxt_voice slot). StartFname() loads + decodes
 * the file and remembers the audio_voice for status queries. */
static void *s_adxt_audio_voice = NULL;   /* current audio_engine voice */
static int   cri_ADXT_Init(void)                          { return 0; }
static int   cri_ADXT_SetDefSvrFreq(int hz)               { (void)hz; return 0; }
static int   cri_ADXT_Finish(void)                        { return 0; }
static void *cri_ADXT_Create(int maxch, void *buf, int bytes) { (void)maxch;(void)buf;(void)bytes; return g_adxt_voice; }
static int   cri_ADXT_Destroy(void *v)                    {
    (void)v;
    if (s_adxt_audio_voice) { audio_voice_destroy(s_adxt_audio_voice); s_adxt_audio_voice = NULL; }
    return 0;
}
static int   cri_ADXT_StartFname(void *v, const char *fn) {
    (void)v;
    if (!fn) return 0;
    /* Stop + free any prior voice (CRI semantics: StartFname pre-empts current). */
    if (s_adxt_audio_voice) { audio_voice_destroy(s_adxt_audio_voice); s_adxt_audio_voice = NULL; }
    s_adxt_audio_voice = ul_adx_play_file(fn);
    return s_adxt_audio_voice ? 0 : 0;   /* CRI returns 0 = success even on failure */
}
static int   cri_ADXT_Stop(void *v)                       {
    (void)v;
    if (s_adxt_audio_voice) audio_voice_stop(s_adxt_audio_voice);
    return 0;
}
static int   cri_ADXT_Pause(void *v, int sw)              { (void)v;(void)sw; return 0; }
static int   cri_ADXT_SetOutVol(void *v, int vol)         { (void)v;(void)vol; return 0; }
static int   cri_ADXT_GetOutVol(void *v)                  { (void)v; return 0; }
static int   cri_ADXT_GetStat(void *v)                    {
    (void)v;
    if (s_adxt_audio_voice && audio_voice_status(s_adxt_audio_voice) == 1)
        return ADXT_STAT_PLAYING;
    return ADXT_STAT_STOP;
}
static int   cri_ADXT_GetStatPause(void *v)               { (void)v; return 0; }

/* ---- CRI Sofdec movie player (so_mwPly*) ---- */
static void *cri_mwPlyCreateSofdec(void *cfg)             { (void)cfg; return g_mwply_movie; }
static int   cri_mwPlyDestroy(void *m)                    { (void)m; return 0; }
static int   cri_mwPlyInitSfdFx(void *cfg)               { (void)cfg; return 0; }
static int   cri_mwPlyFinishSfdFx(void)                   { return 0; }
static int   cri_mwPlyStartFname(void *m, const char *fn) {
    (void)m;
    /* New movie => arm the brief play->end arc so GetStat reports PLAYING for a
     * few polls then PLAYEND(3), letting the game advance past the cutscene. */
    g_mwply_play_ctr = MWPLY_PLAY_FRAMES;
    loader_log(LOG_INFO, "[CRI] mwPlyStartFname(%s) -> play %d frames then PLAYEND",
               fn ? fn : "(null)", MWPLY_PLAY_FRAMES);
    return 0;
}
static int   cri_mwPlyPause(void *m, int sw)              { (void)m;(void)sw; return 0; }
static int   cri_mwPlyGetStat(void *m)                    {
    (void)m;
    /* ISOLATION TEST (ULTRA_GS_MOVIE_STOP=1): revert to the pre-fix STOP(0)
     * behavior so a movie never reports ended (game hangs black on cutscenes).
     * Used only to bisect the early-exit regression; default = play->end fix. */
    static int s_stop = -1;
    if (s_stop < 0) { const char *e = getenv("ULTRA_GS_MOVIE_STOP"); s_stop = (e && atoi(e)) ? 1 : 0; }
    if (s_stop) return 0;
    /* Report PLAYING until the per-movie counter drains, then PLAYEND(3) so
     * GSEVO's movie_ctrl sets its state to "finished" and the next scene runs.
     * Unconditional PLAYEND once armed-out is safe: the game only consults
     * GetStat while its own state var says "playing". */
    if (g_mwply_play_ctr > 0) { g_mwply_play_ctr--; return MWPLY_STAT_PLAYING; }
    return MWPLY_STAT_PLAYEND;
}
static int   cri_mwPlyGetTime(void *m, void *a, void *b)  { (void)m;(void)a;(void)b; return 0; }
static int   cri_mwPlyUpdateTime(void *m, int t)          { (void)m;(void)t; return 0; }
static int   cri_mwPlySetUpdateTimeAuto(void *m)          { (void)m; return 0; }
static int   cri_mwPlySetOutVol(void *m, int v)          { (void)m;(void)v; return 0; }
static int   cri_mwPlyGetCurFrm(void *m, void *out)       { (void)m;(void)out; return 0; } /* 0 = no frame ready */
static int   cri_mwPlyRelCurFrm(void *m)                  { (void)m; return 0; }
static int   cri_mwPlyCalcWorkCprmSfd(void *a, void *b)   { (void)a;(void)b; return 0; }
static int   cri_mwPlyFxSetOutBufPitchHeight(void *m, int p, int h) { (void)m;(void)p;(void)h; return 0; }
static int   cri_mwPlyFxCnvFrmRgba8888(void *m, void *dst) { (void)m;(void)dst; return 0; }

/* ---- raw sound output (snd_util.so) ---- */
static void *cri_sndoutputInit(void *cfg)                 { (void)cfg; return g_sndout_handle; }
static int   cri_sndoutputExec(void)                     { return 0; }
static int   cri_sndoutputDestroy(void)                  { return 0; }

/* ---- libkswapapi (frame-swap sync) ---- */
static int   cri_kswap_collect(void)                     { return 0; }

/* ------------------------------------------------------------------ */
/* Name-keyed dispatch table                                           */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; void *fn; } CriEntry;

static const CriEntry g_cri_table[] = {
    /* ADXM */
    { "so_ADXM_SetupFramework",        (void*)cri_ADXM_SetupFramework },
    { "so_ADXM_SetCbErr",              (void*)cri_ADXM_SetCbErr },
    { "so_ADXM_ExecMain",              (void*)cri_ADXM_ExecMain },
    { "so_ADXM_ShutdownFramework",     (void*)cri_ADXM_ShutdownFramework },
    /* ADXLINUX */
    { "so_ADXLINUX_SetupSound",        (void*)cri_ADXLINUX_SetupSound },
    { "so_ADXLINUX_SetupFileSystem",   (void*)cri_ADXLINUX_SetupFileSystem },
    { "so_ADXLINUX_ShutdownSound",     (void*)cri_ADXLINUX_ShutdownSound },
    { "so_ADXLINUX_ShutdownAnsiFs",    (void*)cri_ADXLINUX_ShutdownAnsiFs },
    /* ADXT */
    { "so_ADXT_Init",                  (void*)cri_ADXT_Init },
    { "so_ADXT_SetDefSvrFreq",         (void*)cri_ADXT_SetDefSvrFreq },
    { "so_ADXT_Finish",                (void*)cri_ADXT_Finish },
    { "so_ADXT_Create",                (void*)cri_ADXT_Create },
    { "so_ADXT_Destroy",               (void*)cri_ADXT_Destroy },
    { "so_ADXT_StartFname",            (void*)cri_ADXT_StartFname },
    { "so_ADXT_Stop",                  (void*)cri_ADXT_Stop },
    { "so_ADXT_Pause",                 (void*)cri_ADXT_Pause },
    { "so_ADXT_SetOutVol",             (void*)cri_ADXT_SetOutVol },
    { "so_ADXT_GetOutVol",             (void*)cri_ADXT_GetOutVol },
    { "so_ADXT_GetStat",               (void*)cri_ADXT_GetStat },
    { "so_ADXT_GetStatPause",          (void*)cri_ADXT_GetStatPause },
    /* Sofdec movie */
    { "so_mwPlyCreateSofdec",          (void*)cri_mwPlyCreateSofdec },
    { "so_mwPlyDestroy",               (void*)cri_mwPlyDestroy },
    { "so_mwPlyInitSfdFx",             (void*)cri_mwPlyInitSfdFx },
    { "so_mwPlyFinishSfdFx",           (void*)cri_mwPlyFinishSfdFx },
    { "so_mwPlyStartFname",            (void*)cri_mwPlyStartFname },
    { "so_mwPlyPause",                 (void*)cri_mwPlyPause },
    { "so_mwPlyGetStat",               (void*)cri_mwPlyGetStat },
    { "so_mwPlyGetTime",               (void*)cri_mwPlyGetTime },
    { "so_mwPlyUpdateTime",            (void*)cri_mwPlyUpdateTime },
    { "so_mwPlySetUpdateTimeAuto",     (void*)cri_mwPlySetUpdateTimeAuto },
    { "so_mwPlySetOutVol",             (void*)cri_mwPlySetOutVol },
    { "so_mwPlyGetCurFrm",             (void*)cri_mwPlyGetCurFrm },
    { "so_mwPlyRelCurFrm",             (void*)cri_mwPlyRelCurFrm },
    { "so_mwPlyCalcWorkCprmSfd",       (void*)cri_mwPlyCalcWorkCprmSfd },
    { "so_mwPlyFxSetOutBufPitchHeight",(void*)cri_mwPlyFxSetOutBufPitchHeight },
    { "so_mwPlyFxCnvFrmRgba8888",      (void*)cri_mwPlyFxCnvFrmRgba8888 },
    /* sndoutput */
    { "sndoutputInit",                 (void*)cri_sndoutputInit },
    { "sndoutputExec",                 (void*)cri_sndoutputExec },
    { "sndoutputDestroy",              (void*)cri_sndoutputDestroy },
    /* kswap */
    { "kswap_collect",                 (void*)cri_kswap_collect },
};

/* Public: stub_dlsym_v2 calls this for any symbol that libGL + the loader
 * symbol table both missed. Returns a dedicated per-function shim for a known
 * middleware symbol, or NULL so the caller falls back to its safe no-op. */
void *cri_shim_lookup(const char *symbol)
{
    if (!symbol) return NULL;
    for (size_t i = 0; i < sizeof(g_cri_table)/sizeof(g_cri_table[0]); i++) {
        if (strcmp(symbol, g_cri_table[i].name) == 0) {
            loader_log(LOG_INFO, "[CRI] dlsym(%s) -> shim %p", symbol, g_cri_table[i].fn);
            return g_cri_table[i].fn;
        }
    }
    return NULL;
}
