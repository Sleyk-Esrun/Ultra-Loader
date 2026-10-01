/*
 * ossl_stub.c
 *
 * OpenSSL EVP + friends stubs for Hummer Extreme Edition's dongle-signature
 * verification path. Hummer's .dynsym references a small OpenSSL surface:
 *   EVP_sha1, EVP_DigestInit/Update/Final, EVP_MD_CTX_cleanup,
 *   EVP_VerifyFinal, EVP_PKEY_assign, EVP_cleanup,
 *   d2i_RSA_PUBKEY, RSA_size, RSA_free,
 *   ERR_free_strings, ERR_remove_state
 * This is the dongle-signature-verify path, NOT a cipher path
 * (no EVP_CIPHER_*, no EncryptInit/DecryptInit).
 *
 * SEMANTICS: return-1-on-success matches OpenSSL convention so the game's
 *   if (EVP_VerifyFinal(...) != 1) abort();
 * dongle check silently passes without needing a wrapper-level bypass patch.
 *
 * BYTE-SAFETY: additive. ABC does not import any EVP_* / RSA_* / ERR_* symbol
 * (verified: not in ABC .dynsym), so this file is dead code for ABC.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "loader.h"

/* Dummy context objects. We hand out pointers to these statics; the game
 * treats them as opaque handles, so any non-NULL address is fine. */
static struct { int dummy; } g_evp_md,   g_evp_pkey, g_rsa;

/* EVP message-digest API -------------------------------------------------- */
static void *stub_EVP_sha1(void)              { return &g_evp_md; }
static void *stub_EVP_sha256(void)            { return &g_evp_md; }
static void *stub_EVP_md5(void)               { return &g_evp_md; }

static int   stub_EVP_DigestInit  (void *ctx, const void *md)                 { (void)ctx;(void)md;              return 1; }
static int   stub_EVP_DigestInit_ex(void *ctx, const void *md, void *engine)  { (void)ctx;(void)md;(void)engine; return 1; }
static int   stub_EVP_DigestUpdate(void *ctx, const void *d, unsigned n)      { (void)ctx;(void)d;(void)n;       return 1; }
static int   stub_EVP_DigestFinal (void *ctx, unsigned char *md, unsigned *s) {
    (void)ctx;
    if (md) memset(md, 0, 20);   /* SHA-1 size */
    if (s)  *s = 20;
    return 1;
}
static int   stub_EVP_DigestFinal_ex(void *ctx, unsigned char *md, unsigned *s) {
    return stub_EVP_DigestFinal(ctx, md, s);
}
static int   stub_EVP_MD_CTX_init   (void *ctx)                                { (void)ctx; return 1; }
static int   stub_EVP_MD_CTX_cleanup(void *ctx)                                { (void)ctx; return 1; }
static void *stub_EVP_MD_CTX_create (void)                                     { static struct { int dummy; } c; return &c; }
static void  stub_EVP_MD_CTX_destroy(void *ctx)                                { (void)ctx; }

/* EVP verify - the actual dongle-signature check. Return 1 = "valid". */
static int   stub_EVP_VerifyFinal(void *ctx, const void *sig, unsigned sl, void *pk) {
    (void)ctx;(void)sig;(void)sl;(void)pk;
    loader_log(LOG_WARN, "EVP_VerifyFinal stub, returning success (dongle-bypass)");
    return 1;
}
static int   stub_EVP_VerifyInit  (void *ctx, const void *md)                 { (void)ctx;(void)md; return 1; }
static int   stub_EVP_VerifyUpdate(void *ctx, const void *d, unsigned n)      { (void)ctx;(void)d;(void)n; return 1; }

/* EVP PKEY helpers */
static void *stub_EVP_PKEY_new(void)                                          { return &g_evp_pkey; }
static void  stub_EVP_PKEY_free(void *pk)                                     { (void)pk; }
static int   stub_EVP_PKEY_assign(void *pk, int type, void *k)                { (void)pk;(void)type;(void)k; return 1; }
static int   stub_EVP_PKEY_set1_RSA(void *pk, void *rsa)                      { (void)pk;(void)rsa; return 1; }

static void  stub_EVP_cleanup(void)                                           {}

/* RSA API - d2i_ = "DER to internal", returns an opaque RSA*. */
static void *stub_d2i_RSA_PUBKEY(void **a, const unsigned char **in, long len) {
    (void)a;(void)in;(void)len;
    return &g_rsa;
}
static void *stub_d2i_RSAPublicKey(void **a, const unsigned char **in, long len) {
    (void)a;(void)in;(void)len;
    return &g_rsa;
}
static void *stub_d2i_RSAPrivateKey(void **a, const unsigned char **in, long len) {
    (void)a;(void)in;(void)len;
    return &g_rsa;
}
static int   stub_RSA_size(void *r)             { (void)r; return 256; } /* 2048-bit key */
static void  stub_RSA_free(void *r)             { (void)r; }
static void *stub_RSA_new(void)                 { return &g_rsa; }

/* ERR bookkeeping - always successful no-ops */
static void  stub_ERR_free_strings(void)                                       {}
static void  stub_ERR_remove_state(unsigned long tid)                          { (void)tid; }
static void  stub_ERR_load_crypto_strings(void)                                {}
static unsigned long stub_ERR_get_error(void)                                  { return 0; }
static void  stub_ERR_clear_error(void)                                        {}

/* OPENSSL_add_all_algorithms - no-op */
static void  stub_OpenSSL_add_all_algorithms(void)                             {}
static void  stub_OpenSSL_add_all_digests(void)                                {}
static void  stub_OpenSSL_add_all_ciphers(void)                                {}

/* ---- registration ----------------------------------------------------- */
void ossl_stub_register(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering OpenSSL stubs (EVP/RSA/ERR, dongle-verify path)");
    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
    /* digest */
    R(EVP_sha1); R(EVP_sha256); R(EVP_md5);
    R(EVP_DigestInit); R(EVP_DigestInit_ex);
    R(EVP_DigestUpdate);
    R(EVP_DigestFinal); R(EVP_DigestFinal_ex);
    R(EVP_MD_CTX_init); R(EVP_MD_CTX_cleanup);
    R(EVP_MD_CTX_create); R(EVP_MD_CTX_destroy);
    /* verify (dongle) */
    R(EVP_VerifyFinal); R(EVP_VerifyInit); R(EVP_VerifyUpdate);
    /* PKEY */
    R(EVP_PKEY_new); R(EVP_PKEY_free);
    R(EVP_PKEY_assign); R(EVP_PKEY_set1_RSA);
    /* misc */
    R(EVP_cleanup);
    /* RSA */
    R(d2i_RSA_PUBKEY); R(d2i_RSAPublicKey); R(d2i_RSAPrivateKey);
    R(RSA_size); R(RSA_free); R(RSA_new);
    /* ERR */
    R(ERR_free_strings); R(ERR_remove_state); R(ERR_load_crypto_strings);
    R(ERR_get_error); R(ERR_clear_error);
    /* init helpers */
    R(OpenSSL_add_all_algorithms);
    R(OpenSSL_add_all_digests);
    R(OpenSSL_add_all_ciphers);
    #undef R
}
