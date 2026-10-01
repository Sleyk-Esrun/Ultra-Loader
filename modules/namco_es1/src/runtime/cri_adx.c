/*
 * cri_adx.c - minimal ADX decoder + CSB stream finder.
 *
 * GSEVO (and many Sega Lindbergh titles) ship their audio in CRI's ADX format,
 * wrapped in CRI's CSB (Sound Bank) containers. The container is built on
 * @UTF tables. This file provides:
 *
 *   1. ADX format header parse + linear-predictive 4-bit ADPCM decoder
 *   2. Tiny "find ADX inside a file" helper (locates the 0x80 0x00 sync)
 *   3. ul_adx_play_file(path, voice_handle): load -> decode -> audio_voice
 *
 * The decoder is faithful to the standard ADX type-3 spec (the one CRI's own
 * adxenc emits). Variants we DON'T yet handle: ADX type-4 (AHX), encrypted
 * ADX (mode 0x08/0x09 - needs the per-game key), and looped ADX (we play
 * once and stop). Most GSEVO SFX use type-3 unencrypted - good enough to
 * actually hear audio.
 *
 * Once we hear audio at all, follow-up work: properly parse the CSB @UTF
 * table so multiple streams indexed by name (snd_se_jungle.csb has dozens)
 * can be selected, and hook the "stream" family (so_ADXT_StartFname) to
 * play the named cue.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "loader.h"
#include "audio_engine.h"

/* ── ADX header layout (big-endian on disk) ────────────────────────────── */
typedef struct {
    unsigned short sync;            /* 0x8000 */
    unsigned short copy_offset;     /* where the "(c)CRI" string ends; ADX data starts at copy_offset + 4 */
    unsigned char  enc_type;        /* 0x03 = standard 4-bit ADPCM */
    unsigned char  block_size;      /* 18 */
    unsigned char  sample_bits;     /* 4 */
    unsigned char  channels;        /* 1 = mono, 2 = stereo */
    unsigned int   sample_rate;     /* e.g. 22050, 32000, 44100 */
    unsigned int   total_samples;   /* across all frames */
    unsigned short highpass_hz;     /* used to derive predictor coeffs */
    unsigned char  version;
    unsigned char  flags;
} AdxHdr;

/* Read big-endian helpers. */
static unsigned short be16(const unsigned char *p) { return (unsigned short)((p[0]<<8) | p[1]); }
static unsigned int   be32(const unsigned char *p) { return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }

/* Compute the ADX predictor coefficients from highpass freq + sample rate.
 * Formula straight from CRI's spec (sometimes called the "Sonic the Hedgehog
 * formula" since most homebrew references use it). Returns a,b as Q12 ints. */
static void adx_coeffs(int hz_hp, int rate, int *out_a, int *out_b)
{
    double sqr2  = 1.41421356237309504880;
    double a_d   = sqr2 - cos(2.0 * 3.14159265358979323846 * (double)hz_hp / (double)rate);
    double b_d   = sqr2 - 1.0;
    double inner = (a_d + b_d) * (a_d - b_d);
    if (inner < 0.0) inner = 0.0;
    double c_d   = (a_d - sqrt(inner)) / b_d;
    *out_a = (int)floor(4096.0 *  2.0 * c_d + 0.5);
    *out_b = (int)floor(4096.0 * -1.0 * c_d * c_d + 0.5);
}

/* Decode one 18-byte ADX frame into 32 int16 samples (per channel).
 * Returns the new (prev1, prev2) state via in/out params. */
static void adx_decode_frame(const unsigned char *src, short *dst, int *prev1, int *prev2, int a, int b)
{
    short scale = (short)be16(src);
    int p1 = *prev1, p2 = *prev2;
    for (int i = 0; i < 32; i++) {
        int byte_idx = 2 + (i >> 1);
        int nibble = (i & 1) ? (src[byte_idx] & 0x0F) : ((src[byte_idx] >> 4) & 0x0F);
        if (nibble & 0x08) nibble |= ~0x0F;   /* sign-extend 4-bit */
        int pred = (a * p1 + b * p2) >> 12;
        int s = pred + (int)scale * nibble;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        dst[i] = (short)s;
        p2 = p1; p1 = s;
    }
    *prev1 = p1; *prev2 = p2;
}

/* Parse an ADX header from a raw buffer. Returns 1 on success, 0 on bad/short. */
static int adx_parse(const unsigned char *p, size_t n, AdxHdr *h)
{
    if (n < 0x14 || p[0] != 0x80 || p[1] != 0x00) return 0;
    h->sync         = be16(p + 0x00);
    h->copy_offset  = be16(p + 0x02);
    h->enc_type     = p[0x04];
    h->block_size   = p[0x05];
    h->sample_bits  = p[0x06];
    h->channels     = p[0x07];
    h->sample_rate  = be32(p + 0x08);
    h->total_samples= be32(p + 0x0C);
    h->highpass_hz  = be16(p + 0x10);
    h->version      = p[0x12];
    h->flags        = p[0x13];
    if (h->enc_type != 0x03) return 0;     /* only type-3 for now */
    if (h->block_size != 18) return 0;
    if (h->sample_bits != 4) return 0;
    if (h->channels < 1 || h->channels > 2) return 0;
    if (h->sample_rate < 8000 || h->sample_rate > 96000) return 0;
    return 1;
}

/* Search a buffer for the ADX sync byte 0x80 0x00 (CSB containers wrap one or
 * more ADX streams in a @UTF table - we just find the first ADX). Returns
 * offset on success, -1 if not found. */
static long find_adx_sync(const unsigned char *buf, size_t n)
{
    for (size_t i = 0; i + 0x20 < n; i++) {
        if (buf[i] == 0x80 && buf[i+1] == 0x00) {
            AdxHdr h;
            if (adx_parse(buf + i, n - i, &h)) return (long)i;
        }
    }
    return -1;
}

/* Decode an entire ADX stream to interleaved S16 PCM. Caller frees pcm via
 * free(). Returns 1 + fills *pcm/*size/*rate/*chans on success. */
static int adx_decode_all(const unsigned char *buf, size_t n,
                          short **pcm, unsigned *bytes,
                          unsigned *rate, unsigned *chans)
{
    AdxHdr h;
    if (!adx_parse(buf, n, &h)) return 0;
    size_t data_off = (size_t)h.copy_offset + 4;       /* per spec */
    if (data_off >= n) return 0;

    int a, b; adx_coeffs(h.highpass_hz, h.sample_rate, &a, &b);

    /* Total samples × channels × 2 bytes for s16. Cap to prevent runaway. */
    if (h.total_samples == 0 || h.total_samples > 16000000) return 0;
    size_t out_bytes = (size_t)h.total_samples * h.channels * sizeof(short);
    short *out = (short*)malloc(out_bytes);
    if (!out) return 0;

    int prev1[2] = {0,0}, prev2[2] = {0,0};
    size_t src = data_off;
    size_t out_idx = 0;
    int frame_size_per_ch = 18;
    int frame_size = frame_size_per_ch * h.channels;

    while (out_idx + 32 * h.channels <= h.total_samples * (size_t)h.channels && src + frame_size <= n) {
        short tmp[2][32];
        for (int c = 0; c < h.channels; c++)
            adx_decode_frame(buf + src + c * frame_size_per_ch, tmp[c], &prev1[c], &prev2[c], a, b);
        /* Interleave. */
        for (int i = 0; i < 32; i++) {
            for (int c = 0; c < h.channels; c++) {
                if (out_idx < h.total_samples * h.channels)
                    out[out_idx++] = tmp[c][i];
            }
        }
        src += frame_size;
    }

    *pcm   = out;
    *bytes = (unsigned)(out_idx * sizeof(short));
    *rate  = h.sample_rate;
    *chans = h.channels;
    return 1;
}

/* Public: load a CSB or raw ADX file, decode the first stream, and return a
 * playing audio voice handle (or NULL on failure). The voice owns the PCM
 * memory and frees it on destroy. */
void* ul_adx_play_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }

    long off = (buf[0] == 0x80 && buf[1] == 0x00) ? 0 : find_adx_sync(buf, rd);
    if (off < 0) { free(buf); return NULL; }

    short *pcm = NULL;
    unsigned bytes = 0, rate = 0, chans = 0;
    int ok = adx_decode_all(buf + off, rd - off, &pcm, &bytes, &rate, &chans);
    free(buf);
    if (!ok || !pcm || bytes == 0) { if (pcm) free(pcm); return NULL; }

    /* Hand the PCM to the audio engine. Engine owns the memory now (it copies
     * via voice_create with pcm != NULL - see audio_voice_create). */
    void *v = audio_voice_create(pcm, bytes, rate, AUDIO_FMT_S16, chans, NULL);
    if (!v) { free(pcm); return NULL; }
    audio_voice_play(v);

    loader_log(LOG_INFO, "[CRI/ADX] %s: %u Hz %u ch %u samples played",
               path, rate, chans, bytes / (chans * 2));
    return v;
}
