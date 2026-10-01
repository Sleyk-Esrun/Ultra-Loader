/*
 * zlib_shim.c
 * Minimal zlib inflate bridge for guest ELFs that link libz.
 *
 * Ghost Squad Evolution (GSEVO, "vsg") gzip-compresses some of its data assets
 * (e.g. the caption/title tables loaded by decode_gzip_data / ev_caption_ctrl)
 * and decompresses them at runtime via three zlib entry points imported from
 * libz.so.1: inflateInit2_, inflate, inflateEnd. The loader never provided
 * these, so their PLT/GOT slots resolved to NULL and the first gzip asset load
 * called through 0 -> ACCESS_VIOLATION (EIP=0) inside decode_gzip_data_open
 * (the inflateInit2_(strm,-15,"1.1.4",0x38) call site).
 *
 * The guest's z_stream is the standard 32-bit zlib z_stream (56 bytes: next_in,
 * avail_in, total_in, next_out, avail_out, total_out, msg, state, zalloc, zfree,
 * opaque, data_type, adler, reserved) - byte-identical to the host mingw zlib's
 * z_stream, and the guest passes stream_size=0x38 (==sizeof) and version "1.1.4"
 * (first char '1' matches the host's "1.3.1", which is all inflateInit2_ checks).
 * The guest zeroes zalloc/zfree/opaque, so host zlib uses its own allocator and
 * stores its private `state` pointer back into the guest struct. So we can pass
 * the guest z_stream straight through to the statically-linked host zlib - no
 * struct translation needed.
 *
 * CLEAN-ROOM: this uses the standard, public zlib library (the host's mingw
 * libz.a) through its documented public API. zlib is the canonical reference
 * implementation of the gzip/DEFLATE formats RFC 1950/1951/1952 - not third-
 * party emulator/SDK source. The shim only adapts the loader's symbol-resolution
 * to the real zlib; no GSEVO-specific decompression logic lives here.
 *
 * ABC SAFETY: After Burner Climax imports no zlib symbols, so registering these
 * is purely additive - the names never appear in ABC's relocation table, and
 * the loader only binds a name if the guest references it. Verified: ABC's
 * .rel.plt has no inflate or deflate entries.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "loader.h"

/* Pull in the host zlib prototypes (mingw32 libz). We rename the host symbols
 * out of the way is unnecessary: we expose our OWN ul_z* wrappers to the guest
 * via sym_register, and call the host inflate*() directly here. */
#include <zlib.h>

/* The guest calls these cdecl (32-bit mingw default). The host zlib functions
 * are also cdecl, so a straight forward call is ABI-correct. We keep thin
 * wrappers (rather than registering the host symbols directly) so we can add a
 * one-time diagnostic and a version-string guard without touching host zlib. */

static int g_zlog = 4;   /* throttle the first few init logs */

int ul_inflateInit2_(z_streamp strm, int windowBits,
                     const char *version, int stream_size)
{
    /* The host inflateInit2_ checks version[0]==ZLIB_VERSION[0] ('1') and
     * stream_size==sizeof(z_stream). The guest's "1.1.4"/0x38 satisfy both
     * against the host 1.3.1 / 56-byte z_stream, so forward as-is. Force the
     * host's own version string so a future host-zlib major bump can't reject
     * the guest's stale literal. */
    int r = inflateInit2_(strm, windowBits, ZLIB_VERSION, (int)sizeof(z_stream));
    if (g_zlog > 0) {
        g_zlog--;
        loader_log(LOG_INFO, "[ZLIB] inflateInit2_(wbits=%d guest_ver=%s sz=%d) -> %d",
                   windowBits, version ? version : "(null)", stream_size, r);
    }
    return r;
}

int ul_inflate(z_streamp strm, int flush)
{
    return inflate(strm, flush);
}

int ul_inflateEnd(z_streamp strm)
{
    return inflateEnd(strm);
}

/* Register the three zlib symbols the loader's relocation pass needs to bind.
 * Called from the glibc-stub registration so the GOT/PLT slots resolve to our
 * wrappers instead of NULL. Inert for titles that don't import them. */
void zlib_shim_register(LoaderCtx *ctx)
{
    /* sym_register(LoaderCtx*, const char*, uintptr_t) declared in loader.h */
    sym_register(ctx, "inflateInit2_", (uintptr_t)ul_inflateInit2_);
    sym_register(ctx, "inflate",       (uintptr_t)ul_inflate);
    sym_register(ctx, "inflateEnd",    (uintptr_t)ul_inflateEnd);
    loader_log(LOG_INFO, "[ZLIB] inflate/inflateEnd/inflateInit2_ registered (host zlib %s)", ZLIB_VERSION);
}
