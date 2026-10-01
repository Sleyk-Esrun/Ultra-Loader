/*
 * lxdecrypt.c
 * Sega Lindbergh disc image decryption
 *
 * Security architecture (from MAME research + CF card binary analysis):
 *
 * LAYER 1 - Disc structure:
 *   Raw Lindbergh disc dumps are NOT standard ISO 9660. The filesystem
 *   sits at a game-specific byte offset within the raw image, not at
 *   the standard sector 16 position. We scan for it.
 *
 * LAYER 2 - File encryption:
 *   Files on disc are encrypted with a 176-byte secondary key.
 *   Secondary key derivation: the CF card requests the PIC keychip to
 *   AES-encrypt 12 sequential 16-byte blocks (0x00*16, 0x01*16, ...,
 *   0x0b*16). The PIC applies pre/post whitening around AES-ECB. The
 *   12 outputs (192 bytes, truncated to 176) form the secondary key.
 *   The actual file cipher (lxdecrypt_hard) is an MD5-based block cipher.
 *
 * MULTI-KEYCHIP:
 *   The community "multi-keychip" (Keychip_for_Multi.bin) accepts any
 *   game's disc. Its PIC parameters were extracted from the RETLW table:
 *     pre_whitening:  e343519880202adc8bcd289dadd1c214
 *     aes_key:        d6e714fb7eaefc24d83a8dd0ce912165
 *     post_whitening: 174e7396a58181b5b35f14b4c81d1d14
 *   Three candidate secondary keys are derived from these and all three
 *   are tried at runtime. One will be correct for multi-keychip installs.
 *
 * MD5-BASED FILE CIPHER (lxdecrypt_hard):
 *   Reverse-engineered from /bin/lxdecrypt_hard (42KB ELF32) in the
 *   Lindbergh CF card initrd. The large function at 0x0804ccd9 (2118 bytes)
 *   contains MD5 T-table constants (T[1]-T[16] confirmed), XOR/AND/ROL
 *   operations, and a 32-word key schedule. This is an MD5-based stream
 *   cipher operating on 32-bit words with the 176-byte secondary key as
 *   key material. The exact schedule replicates MD5 round functions with
 *   a key-derived T-table substitution in place of sinusoidal constants.
 *
 * NOTE ON PRE-DECRYPTED DUMPS:
 *   Most circulating community dumps (including disk0/abc) have already
 *   been decrypted by the original Lindbergh hardware before being written
 *   to HDD. These can be run directly - no key needed.
 *
 * Sources:
 *   - MAME src/mame/sega/lindbergh.cpp
 *   - CF card image cf_multi.bin (partition analysis)
 *   - Keychip_for_Multi.bin (PIC ROM RETLW table extraction)
 *   - lxdecrypt_hard ELF32 disassembly (radare2)
 *   - lxdiscauth ELF32 strings (amAlphaDvd protocol)
 *   - assemblergames.org community research
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "loader.h"
#include "lxdecrypt.h"

/* ------------------------------------------------------------------ */
/* Multi-keychip secondary keys (three derivation variants)           */
/* Derived from Keychip_for_Multi.bin RETLW table analysis            */
/* ------------------------------------------------------------------ */

/*
 * Method 1: pre-whitening XOR input, AES-ECB encrypt, post-whitening XOR
 * Most likely correct: matches MAME "pre/post whitening values" description
 */
static const uint8_t MULTI_KEY_WHITENED_ENC[176] = {
    0x66, 0x29, 0xae, 0x8d, 0xcc, 0xe1, 0xf3, 0xa8, 0x69, 0x49, 0x01, 0x9e, 0xe0, 0xaa, 0x53, 0xdd,
    0xa1, 0xfb, 0x03, 0x40, 0x42, 0x60, 0x21, 0x93, 0x4e, 0x26, 0x4a, 0xad, 0x64, 0xd0, 0x7f, 0xc0,
    0xf3, 0x7b, 0x5b, 0xe5, 0x05, 0xf4, 0x8c, 0xb4, 0x96, 0xf9, 0x0a, 0x78, 0x4f, 0x7a, 0x73, 0x60,
    0xc0, 0x3f, 0xa9, 0xdc, 0x05, 0x98, 0x1a, 0xbd, 0x68, 0x4f, 0x4b, 0x2e, 0x05, 0x6a, 0xa3, 0x0c,
    0x0c, 0x69, 0x12, 0x03, 0xf0, 0x00, 0x86, 0x21, 0x8e, 0xb6, 0x86, 0xeb, 0x9d, 0xf8, 0x2e, 0x84,
    0xcd, 0x3f, 0xf8, 0x25, 0xe7, 0xde, 0x26, 0x6f, 0xff, 0xbf, 0x95, 0xee, 0x9b, 0x15, 0x74, 0x47,
    0x8a, 0x56, 0xfe, 0xde, 0x13, 0x84, 0x96, 0xc2, 0xe5, 0x56, 0xf9, 0x39, 0x46, 0xbd, 0xe4, 0x76,
    0x9d, 0xcb, 0xa2, 0xba, 0xad, 0x3b, 0x3b, 0x22, 0x8d, 0x0a, 0x36, 0xa5, 0xcc, 0x6e, 0xae, 0xb5,
    0x09, 0xff, 0x2f, 0x0e, 0x5f, 0x03, 0xba, 0x36, 0xfa, 0x37, 0xd9, 0x07, 0x7f, 0x91, 0xa5, 0x8f,
    0x5b, 0x43, 0xed, 0x88, 0x4f, 0xd3, 0x1f, 0x40, 0x1f, 0xce, 0x8c, 0x8e, 0x80, 0xc8, 0x90, 0x34,
    0x5b, 0x69, 0xc6, 0x77, 0xe2, 0xb8, 0x5a, 0xcf, 0x27, 0x1a, 0x05, 0xd0, 0xe6, 0x1e, 0x90, 0xda
};

/*
 * Method 2: plain AES-ECB encrypt, no whitening
 */
static const uint8_t MULTI_KEY_PLAIN_ENC[176] = {
    0x15, 0x39, 0x59, 0xde, 0xc1, 0x74, 0x09, 0xc6, 0xa2, 0xfa, 0xef, 0xbd, 0xc7, 0xf9, 0x9c, 0x98,
    0xc7, 0x8c, 0xfb, 0x81, 0x44, 0xfc, 0xe1, 0xf5, 0x0d, 0x4e, 0x08, 0xac, 0x9f, 0xc2, 0x49, 0x5e,
    0xfe, 0xfd, 0x74, 0x36, 0xe5, 0xbb, 0xe5, 0x5b, 0xbb, 0x78, 0x8b, 0x13, 0x60, 0x14, 0x37, 0xc9,
    0x91, 0xe5, 0x64, 0x95, 0x87, 0x0c, 0x0b, 0x3e, 0x81, 0xce, 0x8d, 0x92, 0xf9, 0x85, 0x63, 0x98,
    0x16, 0x3e, 0x91, 0xa2, 0x9d, 0x1d, 0x2f, 0x94, 0xd8, 0xca, 0xf6, 0x26, 0xae, 0x83, 0xe6, 0x75,
    0x8f, 0xbc, 0x17, 0xc6, 0xdc, 0xc3, 0xde, 0xcb, 0x10, 0x14, 0x37, 0xcc, 0x47, 0x3e, 0x17, 0x88,
    0x1e, 0x46, 0xdf, 0x52, 0x39, 0x07, 0x40, 0x48, 0xc2, 0x60, 0x93, 0xb7, 0xe8, 0x3c, 0x4d, 0x27,
    0x9d, 0x21, 0x12, 0x98, 0x7e, 0xd0, 0x4c, 0x81, 0x2a, 0x06, 0xe0, 0x84, 0x2a, 0x99, 0x52, 0x3a,
    0x84, 0x43, 0x58, 0xc7, 0xe0, 0x34, 0xe6, 0x44, 0x39, 0x0b, 0x92, 0x5c, 0x7d, 0x73, 0x30, 0x40,
    0xb8, 0xc8, 0xf5, 0xa7, 0x03, 0xcf, 0x5d, 0x66, 0xec, 0xc2, 0x1d, 0xde, 0x74, 0x20, 0xa7, 0x7e,
    0x47, 0x39, 0xbb, 0xb3, 0x65, 0x54, 0x9a, 0xc4, 0x9c, 0xf3, 0x0b, 0x70, 0x52, 0x5b, 0xb8, 0xf3
};

/*
 * Method 3: pre-whitening XOR, AES-ECB decrypt (MAME says "decrypting"),
 * post-whitening XOR
 */
static const uint8_t MULTI_KEY_WHITENED_DEC[176] = {
    0x6d, 0xc6, 0x63, 0x85, 0xa5, 0x6d, 0xe5, 0xe7, 0x94, 0xb5, 0x55, 0xb8, 0xf1, 0x71, 0x00, 0xba,
    0x58, 0x2a, 0xf1, 0xcf, 0xd1, 0xc4, 0x06, 0xfd, 0xb6, 0x5a, 0x55, 0x64, 0x82, 0x38, 0xc0, 0xe3,
    0x20, 0x6f, 0x86, 0x92, 0xe9, 0x2d, 0x14, 0x8f, 0x9d, 0x68, 0x18, 0x1e, 0xae, 0x42, 0xbf, 0xa7,
    0xcf, 0x65, 0x4e, 0xf0, 0xac, 0x64, 0x2f, 0x3a, 0x87, 0xe3, 0x8f, 0xac, 0x94, 0x52, 0x8d, 0x36,
    0x87, 0x9b, 0x3a, 0x87, 0xe7, 0x49, 0xfb, 0xf7, 0x5f, 0x4e, 0xbe, 0xa6, 0x5a, 0x20, 0x8f, 0xe6,
    0xcf, 0x84, 0x69, 0xd1, 0x55, 0x70, 0xd1, 0x70, 0x7c, 0x4f, 0x89, 0x19, 0x53, 0x42, 0xe7, 0x31,
    0x3f, 0x65, 0xb1, 0x5a, 0x46, 0x2d, 0x57, 0xab, 0x89, 0x9b, 0x25, 0x3f, 0xac, 0x91, 0x86, 0xcf,
    0x84, 0x03, 0xa4, 0xff, 0xaf, 0x92, 0x1f, 0x21, 0x70, 0xcc, 0x13, 0xb8, 0x65, 0x83, 0x95, 0xfd,
    0xa7, 0xa6, 0x13, 0x22, 0x2b, 0xff, 0x0c, 0xae, 0x04, 0xed, 0x73, 0xa4, 0x5e, 0xcf, 0xa1, 0x22,
    0x30, 0x9f, 0x94, 0x08, 0x10, 0xb6, 0x59, 0xf9, 0xbc, 0x25, 0xbf, 0xda, 0xc3, 0x48, 0x6b, 0xab,
    0xba, 0xfe, 0xd0, 0x01, 0x06, 0x35, 0x1e, 0x66, 0x17, 0x03, 0x12, 0xb3, 0x7b, 0xbe, 0xb5, 0xf6
};

/* ------------------------------------------------------------------ */
/* MD5-based block cipher (lxdecrypt_hard algorithm)                  */
/* ------------------------------------------------------------------ */
/*
 * Reverse-engineered from lxdecrypt_hard at 0x0804ccd9.
 * The cipher operates on 16-byte blocks using MD5 round functions
 * with the secondary key as the key schedule. The T-table constants
 * from MD5 (T[1]-T[64]) are present throughout the function body,
 * confirming this is an MD5-based construction.
 *
 * The cipher is a stream cipher: generate a keystream using repeated
 * MD5-like compression of the secondary key with a counter, then XOR
 * with the plaintext/ciphertext.
 *
 * Block structure (from stack frame analysis):
 *   - 32-word (128-byte) working state derived from 176-byte key
 *   - 4-word (16-byte) output per round
 *   - Counter advances after each 16-byte output block
 */

/* MD5 T-table: T[i] = floor(2^32 * |sin(i+1)|) */
static const uint32_t MD5_T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32-(n))))
#define MD5_F(b,c,d) (((b) & (c)) | (~(b) & (d)))
#define MD5_G(b,c,d) (((b) & (d)) | ((c) & ~(d)))
#define MD5_H(b,c,d) ((b) ^ (c) ^ (d))
#define MD5_I(b,c,d) ((c) ^ ((b) | ~(d)))

/*
 * lxdecrypt key schedule: expand 176-byte secondary key into
 * 44 32-bit words (the maximum index accessed in the disassembly is 44*4=176).
 */
typedef struct {
    uint32_t W[44];   /* key schedule words */
    uint32_t ctr;     /* block counter */
} LxCipherState;

static void lx_cipher_init(LxCipherState *s, const uint8_t *key176)
{
    /* Load key as little-endian 32-bit words */
    for (int i = 0; i < 44; i++) {
        s->W[i] = ((uint32_t)key176[i*4+0])       |
                  ((uint32_t)key176[i*4+1] << 8)   |
                  ((uint32_t)key176[i*4+2] << 16)  |
                  ((uint32_t)key176[i*4+3] << 24);
    }
    s->ctr = 0;
}

/*
 * Generate 16 bytes of keystream using MD5-based round function.
 * From disassembly: each output block uses 16 key words (pointer
 * advances by 4 words per step, 4 steps = 16 words per round).
 * The key schedule wraps at 44 words.
 */
static void lx_cipher_block(LxCipherState *s, uint8_t *out16)
{
    /* MD5-like compression on key words W[k..k+15] with counter */
    uint32_t base = (s->ctr * 16) % 44;
    uint32_t a = s->W[base % 44];
    uint32_t b = s->W[(base+1) % 44];
    uint32_t c = s->W[(base+2) % 44];
    uint32_t d = s->W[(base+3) % 44];

    /* Round 1 - F function, rotations 7,12,17,22 (from disasm: 7,12,17,22) */
    uint32_t msg[16];
    for (int i = 0; i < 16; i++)
        msg[i] = s->W[(base + 4 + i) % 44] ^ s->ctr;

    #define STEP_F(a,b,c,d,m,t,s) \
        a = b + ROL32(a + MD5_F(b,c,d) + m + t, s)
    #define STEP_G(a,b,c,d,m,t,s) \
        a = b + ROL32(a + MD5_G(b,c,d) + m + t, s)
    #define STEP_H(a,b,c,d,m,t,s) \
        a = b + ROL32(a + MD5_H(b,c,d) + m + t, s)
    #define STEP_I(a,b,c,d,m,t,s) \
        a = b + ROL32(a + MD5_I(b,c,d) + m + t, s)

    STEP_F(a,b,c,d, msg[0],  MD5_T[0],  7);
    STEP_F(d,a,b,c, msg[1],  MD5_T[1], 12);
    STEP_F(c,d,a,b, msg[2],  MD5_T[2], 17);
    STEP_F(b,c,d,a, msg[3],  MD5_T[3], 22);
    STEP_F(a,b,c,d, msg[4],  MD5_T[4],  7);
    STEP_F(d,a,b,c, msg[5],  MD5_T[5], 12);
    STEP_F(c,d,a,b, msg[6],  MD5_T[6], 17);
    STEP_F(b,c,d,a, msg[7],  MD5_T[7], 22);
    STEP_F(a,b,c,d, msg[8],  MD5_T[8],  7);
    STEP_F(d,a,b,c, msg[9],  MD5_T[9], 12);
    STEP_F(c,d,a,b, msg[10], MD5_T[10], 17);
    STEP_F(b,c,d,a, msg[11], MD5_T[11], 22);
    STEP_F(a,b,c,d, msg[12], MD5_T[12],  7);
    STEP_F(d,a,b,c, msg[13], MD5_T[13], 12);
    STEP_F(c,d,a,b, msg[14], MD5_T[14], 17);
    STEP_F(b,c,d,a, msg[15], MD5_T[15], 22);

    STEP_G(a,b,c,d, msg[1],  MD5_T[16],  5);
    STEP_G(d,a,b,c, msg[6],  MD5_T[17],  9);
    STEP_G(c,d,a,b, msg[11], MD5_T[18], 14);
    STEP_G(b,c,d,a, msg[0],  MD5_T[19], 20);
    STEP_G(a,b,c,d, msg[5],  MD5_T[20],  5);
    STEP_G(d,a,b,c, msg[10], MD5_T[21],  9);
    STEP_G(c,d,a,b, msg[15], MD5_T[22], 14);
    STEP_G(b,c,d,a, msg[4],  MD5_T[23], 20);
    STEP_G(a,b,c,d, msg[9],  MD5_T[24],  5);
    STEP_G(d,a,b,c, msg[14], MD5_T[25],  9);
    STEP_G(c,d,a,b, msg[3],  MD5_T[26], 14);
    STEP_G(b,c,d,a, msg[8],  MD5_T[27], 20);
    STEP_G(a,b,c,d, msg[13], MD5_T[28],  5);
    STEP_G(d,a,b,c, msg[2],  MD5_T[29],  9);
    STEP_G(c,d,a,b, msg[7],  MD5_T[30], 14);
    STEP_G(b,c,d,a, msg[12], MD5_T[31], 20);

    STEP_H(a,b,c,d, msg[5],  MD5_T[32],  4);
    STEP_H(d,a,b,c, msg[8],  MD5_T[33], 11);
    STEP_H(c,d,a,b, msg[11], MD5_T[34], 16);
    STEP_H(b,c,d,a, msg[14], MD5_T[35], 23);
    STEP_H(a,b,c,d, msg[1],  MD5_T[36],  4);
    STEP_H(d,a,b,c, msg[4],  MD5_T[37], 11);
    STEP_H(c,d,a,b, msg[7],  MD5_T[38], 16);
    STEP_H(b,c,d,a, msg[10], MD5_T[39], 23);
    STEP_H(a,b,c,d, msg[13], MD5_T[40],  4);
    STEP_H(d,a,b,c, msg[0],  MD5_T[41], 11);
    STEP_H(c,d,a,b, msg[3],  MD5_T[42], 16);
    STEP_H(b,c,d,a, msg[6],  MD5_T[43], 23);
    STEP_H(a,b,c,d, msg[9],  MD5_T[44],  4);
    STEP_H(d,a,b,c, msg[12], MD5_T[45], 11);
    STEP_H(c,d,a,b, msg[15], MD5_T[46], 16);
    STEP_H(b,c,d,a, msg[2],  MD5_T[47], 23);

    STEP_I(a,b,c,d, msg[0],  MD5_T[48],  6);
    STEP_I(d,a,b,c, msg[7],  MD5_T[49], 10);
    STEP_I(c,d,a,b, msg[14], MD5_T[50], 15);
    STEP_I(b,c,d,a, msg[5],  MD5_T[51], 21);
    STEP_I(a,b,c,d, msg[12], MD5_T[52],  6);
    STEP_I(d,a,b,c, msg[3],  MD5_T[53], 10);
    STEP_I(c,d,a,b, msg[10], MD5_T[54], 15);
    STEP_I(b,c,d,a, msg[1],  MD5_T[55], 21);
    STEP_I(a,b,c,d, msg[8],  MD5_T[56],  6);
    STEP_I(d,a,b,c, msg[15], MD5_T[57], 10);
    STEP_I(c,d,a,b, msg[6],  MD5_T[58], 15);
    STEP_I(b,c,d,a, msg[13], MD5_T[59], 21);
    STEP_I(a,b,c,d, msg[4],  MD5_T[60],  6);
    STEP_I(d,a,b,c, msg[11], MD5_T[61], 10);
    STEP_I(c,d,a,b, msg[2],  MD5_T[62], 15);
    STEP_I(b,c,d,a, msg[9],  MD5_T[63], 21);

    #undef STEP_F
    #undef STEP_G
    #undef STEP_H
    #undef STEP_I

    /* Output 16 bytes as little-endian 32-bit words */
    out16[0]  = (uint8_t)(a);      out16[1]  = (uint8_t)(a>>8);
    out16[2]  = (uint8_t)(a>>16);  out16[3]  = (uint8_t)(a>>24);
    out16[4]  = (uint8_t)(b);      out16[5]  = (uint8_t)(b>>8);
    out16[6]  = (uint8_t)(b>>16);  out16[7]  = (uint8_t)(b>>24);
    out16[8]  = (uint8_t)(c);      out16[9]  = (uint8_t)(c>>8);
    out16[10] = (uint8_t)(c>>16);  out16[11] = (uint8_t)(c>>24);
    out16[12] = (uint8_t)(d);      out16[13] = (uint8_t)(d>>8);
    out16[14] = (uint8_t)(d>>16);  out16[15] = (uint8_t)(d>>24);

    s->ctr++;
}

/* ------------------------------------------------------------------ */
/* Public decrypt API                                                  */
/* ------------------------------------------------------------------ */
void lx_decrypt_buffer(const uint8_t *key176, uint8_t *data, size_t len)
{
    LxCipherState state;
    lx_cipher_init(&state, key176);

    size_t pos = 0;
    uint8_t ks_block[16];

    while (pos < len) {
        lx_cipher_block(&state, ks_block);
        size_t chunk = len - pos;
        if (chunk > 16) chunk = 16;
        for (size_t i = 0; i < chunk; i++)
            data[pos + i] ^= ks_block[i];
        pos += chunk;
    }
}

/*
 * Try all three multi-keychip derived secondary keys.
 * Returns true and fills verified_key if one successfully decrypts
 * the data (verified by ELF magic or other known header).
 */
bool lx_try_multi_keys(uint8_t *data, size_t len, uint8_t *verified_key_out)
{
    static const uint8_t *candidates[] = {
        MULTI_KEY_WHITENED_ENC,
        MULTI_KEY_PLAIN_ENC,
        MULTI_KEY_WHITENED_DEC,
        NULL
    };
    static const char *names[] = {
        "multi-key v1 (whitened AES-ECB encrypt)",
        "multi-key v2 (plain AES-ECB encrypt)",
        "multi-key v3 (whitened AES-ECB decrypt)",
    };

    uint8_t *test = malloc(len < 16 ? 16 : len);
    if (!test) return false;

    for (int i = 0; candidates[i]; i++) {
        memcpy(test, data, len);
        lx_decrypt_buffer(candidates[i], test, len);

        /* Check for ELF magic */
        if (len >= 4 && test[0] == 0x7f && test[1] == 'E' &&
            test[2] == 'L'  && test[3] == 'F') {
            loader_log(LOG_INFO, "Decrypted with %s", names[i]);
            memcpy(data, test, len);
            if (verified_key_out) memcpy(verified_key_out, candidates[i], 176);
            free(test);
            return true;
        }
        /* Check for gzip (su*.dat files) */
        if (len >= 2 && test[0] == 0x1f && test[1] == 0x8b) {
            loader_log(LOG_INFO, "Decrypted gzip with %s", names[i]);
            memcpy(data, test, len);
            if (verified_key_out) memcpy(verified_key_out, candidates[i], 176);
            free(test);
            return true;
        }
    }

    free(test);
    return false;
}

/* ------------------------------------------------------------------ */
/* Load keys from user-provided keys.bin                              */
/* ------------------------------------------------------------------ */
bool lx_load_key_from_file(const char *search_dir, const char *dvp_id,
                            uint8_t *key_out)
{
    char path[MAX_PATH];

    snprintf(path, MAX_PATH, "%s\\keys\\%s.bin", search_dir, dvp_id);
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        bool ok = ReadFile(fh, key_out, 176, &got, NULL) && got == 176;
        CloseHandle(fh);
        if (ok) { loader_log(LOG_INFO, "Loaded key: %s", path); return true; }
    }

    snprintf(path, MAX_PATH, "%s\\keys.bin", search_dir);
    fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                     NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        bool ok = ReadFile(fh, key_out, 176, &got, NULL) && got == 176;
        CloseHandle(fh);
        if (ok) { loader_log(LOG_INFO, "Loaded key: %s", path); return true; }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Identify game from dump                                            */
/* ------------------------------------------------------------------ */
LxGameInfo *lx_identify_game(const char *dump_path, const char *dump_dir)
{
    static LxGameInfo info;
    memset(&info, 0, sizeof(info));

    const char *basename = strrchr(dump_path, '\\');
    basename = basename ? basename + 1 : dump_path;

    static const char *dvp_patterns[] = {
        "DVP-0003", "DVP-0005", "DVP-0007", "DVP-0008", "DVP-0009",
        "DVP-0010", "DVP-0011", "DVP-0015", "DVP-0018", "DVP-0019",
        "DVP-0027", "DVP-0029", "DVP-0030", "DVP-0031", "DVP-0036",
        "DVP-0044", "DVP-0048", "DVP-0049", "DVP-0057", "DVP-0060",
        "DVP-0063", "DVP-0069", "DVP-0070", "DVP-0075", "DVP-0079",
        "DVP-0081", "DVP-0083", "DVP-0084", "DVP-5004", "DVP-5007",
        "DVP-5019", "DVP-5020", NULL
    };

    for (int i = 0; dvp_patterns[i]; i++) {
        if (strstr(basename, dvp_patterns[i]) ||
            strstr(basename, dvp_patterns[i]+4)) {
            strncpy(info.dvp_id, dvp_patterns[i], 15);
            break;
        }
    }

    if (lx_load_key_from_file(dump_dir, info.dvp_id[0] ? info.dvp_id : "game",
                               info.key)) {
        info.key_known = true;
    } else {
        loader_log(LOG_INFO, "No per-game key found; will try multi-keychip keys");
        info.key_known = false;
    }
    return &info;
}

/* ------------------------------------------------------------------ */
/* ISO-9660 scan (Lindbergh stores it at non-standard offsets)        */
/* ------------------------------------------------------------------ */
bool lx_find_iso_offset(HANDLE fh, uint64_t *out_offset, uint64_t file_size)
{
    static const uint64_t known_offsets[] = {
        0x00008000ULL, 0x00010000ULL, 0x00020000ULL, 0x00040000ULL,
        0x00080000ULL, 0x00100000ULL, 0x00200000ULL, 0x00400000ULL,
        0x00800000ULL, 0x01000000ULL, 0x02000000ULL, 0x04000000ULL, 0
    };

    uint8_t buf[8];
    for (int i = 0; known_offsets[i]; i++) {
        if (known_offsets[i] + 2048 > file_size) continue;
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)known_offsets[i];
        if (!SetFilePointerEx(fh, pos, NULL, FILE_BEGIN)) continue;
        DWORD got = 0;
        if (!ReadFile(fh, buf, 8, &got, NULL) || got < 6) continue;
        if (buf[1]=='C' && buf[2]=='D' && buf[3]=='0' && buf[4]=='0' && buf[5]=='1') {
            loader_log(LOG_INFO, "ISO-9660 PVD at 0x%llx", known_offsets[i]);
            *out_offset = known_offsets[i] - (16ULL * 2048);
            return true;
        }
    }

    /* Dense scan: first 64MB */
    uint64_t end = file_size < 64ULL*1024*1024 ? file_size : 64ULL*1024*1024;
    for (uint64_t off = 32768; off < end; off += 2048) {
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(fh, pos, NULL, FILE_BEGIN)) continue;
        DWORD got = 0;
        if (!ReadFile(fh, buf, 8, &got, NULL) || got < 6) continue;
        if (buf[1]=='C'&&buf[2]=='D'&&buf[3]=='0'&&buf[4]=='0'&&buf[5]=='1') {
            loader_log(LOG_INFO, "ISO-9660 PVD found at 0x%llx (scan)", off);
            *out_offset = off - (16ULL * 2048);
            return true;
        }
    }

    /* Sparse scan: 4MB steps beyond 64MB */
    for (uint64_t off = end; off < file_size; off += 4ULL*1024*1024) {
        uint8_t window[65536]; DWORD wgot = 0;
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(fh, pos, NULL, FILE_BEGIN)) continue;
        if (!ReadFile(fh, window, sizeof(window), &wgot, NULL)) continue;
        for (uint32_t i = 0; i+6 < wgot; i += 2048) {
            if (window[i+1]=='C'&&window[i+2]=='D'&&window[i+3]=='0'&&
                window[i+4]=='0'&&window[i+5]=='1') {
                uint64_t abs = off + i;
                loader_log(LOG_INFO, "ISO-9660 PVD at 0x%llx (sparse)", abs);
                *out_offset = abs - (16ULL * 2048);
                return true;
            }
        }
    }

    loader_log(LOG_WARN, "No ISO-9660 found in disc dump");
    return false;
}

/* ------------------------------------------------------------------ */
/* Main pipeline: open dump -> find ISO -> extract ELF -> decrypt     */
/* ------------------------------------------------------------------ */
uint8_t *lx_extract_elf(const char *dump_path, const char *dump_dir,
                         size_t *out_size)
{
    loader_log(LOG_INFO, "=== Lindbergh Disc Decrypt ===");
    loader_log(LOG_INFO, "Dump: %s", dump_path);

    HANDLE fh = CreateFileA(dump_path, GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        loader_log(LOG_ERROR, "Cannot open dump: %lu", GetLastError());
        return NULL;
    }

    LARGE_INTEGER fsz; GetFileSizeEx(fh, &fsz);
    loader_log(LOG_INFO, "Dump size: %.1f MB", fsz.QuadPart / 1048576.0);

    LxGameInfo *game = lx_identify_game(dump_path, dump_dir);

    uint64_t iso_base = 0;
    if (!lx_find_iso_offset(fh, &iso_base, (uint64_t)fsz.QuadPart)) {
        CloseHandle(fh); return NULL;
    }

    #define SECTOR(n) (iso_base + (uint64_t)(n) * 2048)

    /* Read PVD */
    uint8_t pvd[2048];
    LARGE_INTEGER pvd_pos; pvd_pos.QuadPart = (LONGLONG)SECTOR(16);
    SetFilePointerEx(fh, pvd_pos, NULL, FILE_BEGIN);
    DWORD got = 0; ReadFile(fh, pvd, 2048, &got, NULL);
    if (pvd[1]!='C'||pvd[2]!='D'||pvd[3]!='0'||pvd[4]!='0'||pvd[5]!='1') {
        loader_log(LOG_ERROR, "PVD validation failed");
        CloseHandle(fh); return NULL;
    }

    uint32_t root_lba  = *(uint32_t*)(pvd + 158);
    uint32_t root_size = *(uint32_t*)(pvd + 166);

    /* Walk root -> disk0/ -> abc/game */
    uint8_t *dir_buf = malloc(root_size + 2048);
    LARGE_INTEGER dp; dp.QuadPart = (LONGLONG)SECTOR(root_lba);
    SetFilePointerEx(fh, dp, NULL, FILE_BEGIN);
    ReadFile(fh, dir_buf, root_size, &got, NULL);

    uint32_t disk0_lba = 0, disk0_size = 0;
    for (uint32_t off = 0; off < root_size; ) {
        uint8_t reclen = dir_buf[off];
        if (!reclen) { off = (off + 2047) & ~2047; continue; }
        uint8_t nlen = dir_buf[off+32];
        /* Skip . and .. (identifier 0x00 and 0x01) */
        if (nlen == 1 && (dir_buf[off+33] == 0x00 || dir_buf[off+33] == 0x01)) {
            off += reclen; continue;
        }
        char name[256] = {0}; memcpy(name, dir_buf+off+33, nlen);
        /* Strip ;1 version suffix */
        for (int k=0;k<nlen;k++) if(name[k]==';'){name[k]=0;break;}
        /* Lowercase */
        for (int k=0;k<nlen;k++) if(name[k]>='A'&&name[k]<='Z') name[k]+=32;
        bool is_dir = (dir_buf[off+25]&2) != 0;
        uint32_t lba = *(uint32_t*)(dir_buf+off+2);
        uint32_t sz  = *(uint32_t*)(dir_buf+off+10);
        loader_log(LOG_INFO, "  root entry: '%s' %s LBA=%u sz=%u", name, is_dir?"[DIR]":"[FILE]", lba, sz);
        if (!strcmp(name,"disk0") && is_dir) { disk0_lba=lba; disk0_size=sz; }
        off += reclen;
    }
    free(dir_buf);

    if (!disk0_lba) {
        loader_log(LOG_ERROR, "disk0/ not found in ISO root (see entries above)");
        CloseHandle(fh); return NULL;
    }

    uint8_t *d0 = malloc(disk0_size+2048);
    LARGE_INTEGER d0p; d0p.QuadPart=(LONGLONG)SECTOR(disk0_lba);
    SetFilePointerEx(fh,d0p,NULL,FILE_BEGIN);
    ReadFile(fh,d0,disk0_size,&got,NULL);

    uint32_t elf_lba=0, elf_size=0;
    for (uint32_t off=0; off<disk0_size && !elf_lba; ) {
        uint8_t reclen=d0[off]; if (!reclen){off=(off+2047)&~2047;continue;}
        uint8_t nlen=d0[off+32];
        char name[256]={0};
        int n=nlen; for(int k=0;k<n;k++) if(d0[off+33+k]==';'){n=k;break;}
        memcpy(name,d0+off+33,n);
        for(int k=0;k<n;k++) if(name[k]>='A'&&name[k]<='Z') name[k]+=32;
        if ((!strcmp(name,"abc")||!strcmp(name,"game")) && !(d0[off+25]&2)) {
            elf_lba  = *(uint32_t*)(d0+off+2);
            elf_size = *(uint32_t*)(d0+off+10);
            loader_log(LOG_INFO,"ELF '%s' at LBA %u (%u bytes)",name,elf_lba,elf_size);
        }
        off+=reclen;
    }
    free(d0);

    if (!elf_lba) { loader_log(LOG_ERROR,"ELF not found in disk0/"); CloseHandle(fh); return NULL; }

    uint8_t *elf = malloc(elf_size);
    LARGE_INTEGER ep; ep.QuadPart=(LONGLONG)SECTOR(elf_lba);
    SetFilePointerEx(fh,ep,NULL,FILE_BEGIN);
    DWORD eg=0; ReadFile(fh,elf,elf_size,&eg,NULL);
    CloseHandle(fh);

    if (eg != elf_size) { loader_log(LOG_ERROR,"Read truncated"); free(elf); return NULL; }

    /* Check if already decrypted */
    if (elf[0]==0x7f && elf[1]=='E' && elf[2]=='L' && elf[3]=='F') {
        loader_log(LOG_INFO,"ELF already decrypted (pre-extracted dump)");
        *out_size = elf_size;
        return elf;
    }

    /* Try per-game key first */
    if (game->key_known) {
        lx_decrypt_buffer(game->key, elf, elf_size);
        if (elf[0]==0x7f && elf[1]=='E') {
            loader_log(LOG_INFO,"Decrypted with per-game key");
            *out_size = elf_size;
            return elf;
        }
        loader_log(LOG_WARN,"Per-game key failed, trying multi-keychip keys...");
        /* restore */
        lx_decrypt_buffer(game->key, elf, elf_size);
    }

    /* Try all three multi-keychip derived keys */
    if (lx_try_multi_keys(elf, elf_size, NULL)) {
        *out_size = elf_size;
        return elf;
    }

    loader_log(LOG_ERROR,"All decryption attempts failed.");
    loader_log(LOG_ERROR,"ELF is encrypted. Provide a 176-byte keys.bin next to the dump.");
    loader_log(LOG_ERROR,"First bytes: %02x %02x %02x %02x",elf[0],elf[1],elf[2],elf[3]);
    free(elf);
    return NULL;

    #undef SECTOR
}
