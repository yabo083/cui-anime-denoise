/* img_posix.c -- the POSIX image backend: stb_image + stb_image_write where
 * img.c uses WIC. Same img.h contract: straight RGBA8, top-down, tightly
 * packed; EXIF orientation applied on JPEG decode; RGB-family ICC profiles
 * preserved through decode -> PNG re-encode (iCCP chunk).
 *
 * Format coverage vs the WIC backend:
 *   decode: PNG, JPEG, GIF, BMP, TGA, PNM -- everything stb_image reads.
 *           No WebP / AVIF / TIFF / HEIF (stb_image does not read them);
 *           those inputs fail with "cannot decode". See docs/cli-spec.md.
 *   encode: PNG (with iCCP when a profile was decoded), JPEG.
 *
 * 16-bit-per-channel sources are scaled v/257+0.5 like PIL -- the same rule
 * the WIC backend documents -- because stb_image's own 16->8 path is a
 * high-byte clip, so 16-bit sources go through stbi_load_16_from_memory
 * and the rounding happens here.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../img.h"
#include "../img_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The implementations live in this one TU; every other file sees headers
 * only. The headers are the copies CUI already vendors -- one stb per repo,
 * not a second divergent set. stb is upstream code: its -Wall/-Wextra noise
 * is silenced here (the Makefile does the same for the other vendored TUs)
 * so our own warnings stay visible. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/CUI/third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/CUI/third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

bool img_init(void) { return true; }
void img_shutdown(void) {}

/* ------------------------------------------------------------------------- */
/* Little parsers                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int is_png_sig(const unsigned char *d, size_t n)
{
    static const unsigned char sig[8] =
        { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return n >= 8 && memcmp(d, sig, 8) == 0;
}

static int is_jpeg_sig(const unsigned char *d, size_t n)
{
    return n >= 4 && d[0] == 0xFF && d[1] == 0xD8;
}

/* PNG IHDR color type: 2 (RGB), 3 (palette) and 6 (RGBA) are the RGB
 * family the img.h contract preserves ICC for; grey (0, 4) is not. */
static int png_rgb_family(const unsigned char *d, size_t n)
{
    if (!is_png_sig(d, n) || n < 26) return 0;
    if (memcmp(d + 12, "IHDR", 4) != 0) return 0;
    /* IHDR data at offset 16: width(4) height(4) bitdepth(1) colortype(1) */
    {
        int ct = d[25];
        return ct == 2 || ct == 3 || ct == 6;
    }
}

/* Extracts the iCCP chunk (zlib-compressed profile) from PNG bytes.
 * Returns a malloc'ed buffer + size, NULL when absent/corrupt. */
static unsigned char *png_icc(const unsigned char *d, size_t n,
                              size_t *out_size)
{
    size_t pos = 8;
    *out_size = 0;
    if (!is_png_sig(d, n)) return NULL;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const unsigned char *type = d + pos + 4;
        const unsigned char *payload = d + pos + 8;
        if ((size_t)len > n || pos + 12 + (size_t)len > n) return NULL;
        if (memcmp(type, "iCCP", 4) == 0) {
            const unsigned char *z;
            int outlen = 0;
            char *raw;
            /* payload = profile name (Latin-1) + NUL + method(0) + zlib */
            const unsigned char *end = payload + len;
            const unsigned char *sep =
                (const unsigned char *)memchr(payload, 0, len);
            if (!sep || sep + 1 >= end || sep[1] != 0) return NULL;
            z = sep + 2;
            raw = stbi_zlib_decode_malloc_guesssize(
                (const char *)z, (int)(end - z), (int)(len * 4 + 4096),
                &outlen);
            if (!raw || outlen <= 0) {
                if (raw) STBI_FREE(raw);
                return NULL;
            }
            *out_size = (size_t)outlen;
            return (unsigned char *)raw;   /* stbi malloc == free-able */
        }
        pos += 12 + len;
        if (memcmp(type, "IDAT", 4) == 0) return NULL;   /* iCCP precedes IDAT */
    }
    return NULL;
}

/* Concatenates JPEG APP2 ICC_PROFILE segments in sequence order. */
static unsigned char *jpeg_icc(const unsigned char *d, size_t n,
                               size_t *out_size)
{
    unsigned char *icc = NULL;
    unsigned char seq_total = 0, seq_seen = 0;
    unsigned char seq_seen_map[256] = {0};
    size_t icc_len = 0, pos;
    *out_size = 0;
    if (!is_jpeg_sig(d, n)) return NULL;
    pos = 2;
    while (pos + 4 <= n) {
        unsigned marker, seglen;
        const unsigned char *seg;
        if (d[pos] != 0xFF) break;
        marker = d[pos + 1];
        if (marker == 0xFF) {   /* fill byte: JPEG allows padding before a marker */
            pos++;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        seglen = be16(d + pos + 2);
        if (seglen < 2 || pos + 2 + seglen > n) break;
        seg = d + pos + 4;
        if (marker == 0xE2 && seglen >= 16 &&
            memcmp(seg, "ICC_PROFILE\0", 12) == 0) {
            unsigned seq_no = seg[12], seq_cnt = seg[13];
            size_t plen = seglen - 2 - 14;
            if (seq_cnt && seq_no >= 1 && seq_no <= seq_cnt &&
                !seq_seen_map[seq_no]) {
                unsigned char *nb;
                if (!seq_total) seq_total = seq_cnt;
                seq_seen_map[seq_no] = 1;
                nb = (unsigned char *)realloc(icc, icc_len + plen);
                if (nb) {
                    icc = nb;
                    memcpy(icc + icc_len, seg + 14, plen);
                    icc_len += plen;
                    seq_seen++;
                    if (seq_seen >= seq_total) {
                        *out_size = icc_len;
                        return icc;
                    }
                }
            }
        }
        if (marker == 0xDA) break;   /* SOS: metadata is over */
        pos += 2 + seglen;
    }
    if (icc && seq_total && seq_seen < seq_total) {
        /* incomplete sequence: an ICC profile we cannot trust */
        free(icc);
        icc = NULL;
    }
    *out_size = 0;
    return icc;
}

/* JPEG APP1 EXIF: returns TIFF tag 0x0112 (orientation), 1 when absent. */
static int jpeg_exif_orientation(const unsigned char *d, size_t n)
{
    size_t pos;
    if (!is_jpeg_sig(d, n)) return 1;
    pos = 2;
    while (pos + 4 <= n) {
        unsigned marker, seglen;
        const unsigned char *seg;
        if (d[pos] != 0xFF) break;
        marker = d[pos + 1];
        if (marker == 0xFF) {   /* fill byte */
            pos++;
            continue;
        }
        if (marker == 0xDA) break;
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        seglen = be16(d + pos + 2);
        if (seglen < 2 || pos + 2 + seglen > n) break;
        seg = d + pos + 4;
        if (marker == 0xE1 && seglen >= 8 &&
            memcmp(seg, "Exif\0\0", 6) == 0) {
            const unsigned char *t = seg + 6;
            size_t tlen = seglen - 2 - 6;
            int le;
            if (tlen < 8) return 1;
            if (t[0] == 'I' && t[1] == 'I') le = 1;
            else if (t[0] == 'M' && t[1] == 'M') le = 0;
            else return 1;
            /* TIFF header: magic 42, then the IFD0 offset */
            {
                uint32_t ifd_off = le
                    ? (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                      ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24)
                    : be32(t + 4);
                const unsigned char *ifd_p;
                uint16_t cnt;
                if (ifd_off > tlen - 2) return 1;
                ifd_p = t + ifd_off;
                cnt = le ? (uint16_t)(ifd_p[0] | (ifd_p[1] << 8))
                         : be16(ifd_p);
                for (uint16_t k = 0; k < cnt; k++) {
                    const unsigned char *e = ifd_p + 2 + (size_t)k * 12;
                    uint16_t tag, type;
                    uint32_t count;
                    if (e + 12 > t + tlen) break;
                    tag = le ? (uint16_t)(e[0] | (e[1] << 8)) : be16(e);
                    type = le ? (uint16_t)(e[2] | (e[3] << 8)) : be16(e + 2);
                    count = le ? (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
                                 ((uint32_t)e[6] << 16) |
                                 ((uint32_t)e[7] << 24)
                               : be32(e + 4);
                    if (tag == 0x0112 && type == 3 && count == 1) {
                        int v = le ? (e[8] | (e[9] << 8)) : be16(e + 8);
                        return (v >= 1 && v <= 8) ? v : 1;
                    }
                }
            }
            return 1;
        }
        pos += 2 + seglen;
    }
    return 1;
}

/* Applies an EXIF orientation in-place (realloc-safe: allocates a rotated
 * copy then swaps). Orientations 5-8 swap w/h. */
static void apply_orientation(img_pixels *im, int orient)
{
    int W = im->w, H = im->h, nw, nh;
    uint8_t *px, *np;
    if (orient == 1 || !im->rgba) return;
    nw = (orient >= 5) ? H : W;
    nh = (orient >= 5) ? W : H;
    np = (uint8_t *)malloc((size_t)nw * nh * 4);
    if (!np) return;
    px = im->rgba;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            int sx, sy;
            switch (orient) {
                case 2:  sx = W - 1 - x;         sy = y;             break;
                case 3:  sx = W - 1 - x;         sy = H - 1 - y;     break;
                case 4:  sx = x;                 sy = H - 1 - y;     break;
                case 5:  sx = y;                 sy = x;             break;
                case 6:  sx = y;                 sy = H - 1 - x;     break;
                case 7:  sx = W - 1 - y;         sy = H - 1 - x;     break;
                case 8:  sx = H - 1 - y;         sy = x;             break;
                default: sx = x;                 sy = y;             break;
            }
            memcpy(np + ((size_t)y * nw + x) * 4,
                   px + ((size_t)sy * W + sx) * 4, 4);
        }
    }
    free(px);
    im->rgba = np;
    im->w = nw;
    im->h = nh;
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                     */
/* ------------------------------------------------------------------------- */

static bool decode_mem(const void *data, size_t size, img_pixels *out,
                       unsigned char **icc, size_t *icc_size)
{
    const unsigned char *d = (const unsigned char *)data;
    int w = 0, h = 0, comp = 0;
    int png, jpg, orient = 1;

    if (out) {
        out->rgba = NULL;
        out->w = out->h = 0;
    }
    if (icc) {
        *icc = NULL;
        if (icc_size) *icc_size = 0;
    }
    if (!d || size < 16 || !out) return false;

    png = is_png_sig(d, size);
    jpg = is_jpeg_sig(d, size);
    if (jpg) orient = jpeg_exif_orientation(d, size);

    if (stbi_is_16_bit_from_memory(d, (int)size)) {
        /* PIL-style v/257+0.5 scaling instead of stb's high-byte clip */
        stbi_us *p16 = stbi_load_16_from_memory(d, (int)size, &w, &h,
                                                &comp, 4);
        size_t n;
        unsigned char *px;
        if (!p16 || w <= 0 || h <= 0) {
            if (p16) STBI_FREE(p16);
            return false;
        }
        n = (size_t)w * (size_t)h * 4;
        px = (unsigned char *)malloc(n);
        if (!px) {
            STBI_FREE(p16);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            px[i] = (unsigned char)((p16[i] + 128) / 257);
        }
        STBI_FREE(p16);
        out->rgba = px;
    } else {
        out->rgba = stbi_load_from_memory(d, (int)size, &w, &h, &comp, 4);
        if (!out->rgba) return false;
        if (jpg && comp == 4) {
            /* 4-component JPEG = CMYK: stb returns raw CMYK channels. Use the
             * same naive conversion img.c falls back to without a colour
             * profile: R = 255 - min(255, C + K), alpha opaque. */
            size_t i, np = (size_t)w * (size_t)h;
            for (i = 0; i < np; i++) {
                unsigned char *px = out->rgba + i * 4;
                unsigned c = px[0], m = px[1], y = px[2], k = px[3];
                px[0] = (unsigned char)(255 - (c + k > 255 ? 255 : c + k));
                px[1] = (unsigned char)(255 - (m + k > 255 ? 255 : m + k));
                px[2] = (unsigned char)(255 - (y + k > 255 ? 255 : y + k));
                px[3] = 255;
            }
        }
    }
    if (w <= 0 || h <= 0 ||
        (uint64_t)w * (uint64_t)h * 4u > IMG_MAX_BYTES) {
        free(out->rgba);
        out->rgba = NULL;
        return false;
    }
    out->w = w;
    out->h = h;

    /* ICC only for RGB-family sources (img.h contract): PNG by color type,
     * JPEG by source channel count (4ch = CMYK, 1ch = grey). */
    if (icc) {
        if (png && png_rgb_family(d, size)) {
            *icc = png_icc(d, size, icc_size);
        } else if (jpg && comp == 3) {
            *icc = jpeg_icc(d, size, icc_size);
        }
    }
    if (orient != 1) apply_orientation(out, orient);
    return true;
}

bool img_decode_memory_ex(const void *data, size_t size, img_pixels *out,
                          unsigned char **icc, size_t *icc_size)
{
    return decode_mem(data, size, out, icc, icc_size);
}

bool img_decode_memory(const void *data, size_t size, img_pixels *out)
{
    return decode_mem(data, size, out, NULL, NULL);
}

bool img_decode_file_ex(const char *utf8_path, img_pixels *out,
                        unsigned char **icc, size_t *icc_size)
{
    FILE *f;
    unsigned char *buf;
    size_t cap = 1 << 20, len = 0;
    bool ok;

    if (out) {
        out->rgba = NULL;
        out->w = out->h = 0;
    }
    if (icc) {
        *icc = NULL;
        if (icc_size) *icc_size = 0;
    }
    if (!utf8_path || !out) return false;
    f = fopen(utf8_path, "rb");
    if (!f) return false;
    buf = (unsigned char *)malloc(cap);
    if (!buf) {
        fclose(f);
        return false;
    }
    for (;;) {
        size_t got;
        if (len + 65536 > cap) {
            unsigned char *nb;
            if (cap > IMG_MAX_BYTES) break;
            cap *= 2;
            nb = (unsigned char *)realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        got = fread(buf + len, 1, 65536, f);
        len += got;
        if (got < 65536) break;
    }
    fclose(f);
    if (!len || len > 0x7fffffffU) {   /* stb takes an int length */
        free(buf);
        return false;
    }
    ok = decode_mem(buf, len, out, icc, icc_size);
    free(buf);
    return ok;
}

bool img_decode_file(const char *utf8_path, img_pixels *out)
{
    return img_decode_file_ex(utf8_path, out, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* Encode                                                                     */
/* ------------------------------------------------------------------------- */

/* PNG CRC-32 (poly 0xEDB88320), needed when the iCCP chunk is grafted into
 * stb's finished byte stream. */
static uint32_t png_crc(const unsigned char *data, size_t len)
{
    static uint32_t table[256];
    static int ready;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!ready) {
        for (i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i;
            int k;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }
    for (i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* Inserts an iCCP chunk before the first IDAT of a complete PNG stream.
 * Returns a new malloc'ed buffer; NULL keeps the caller on the un-profiled
 * stream. */
static unsigned char *png_with_icc(const unsigned char *png, size_t png_len,
                                   const unsigned char *icc, size_t icc_len,
                                   size_t *out_len)
{
    static const char name[] = "ICC profile";
    unsigned char *z = NULL, *out, *chunk;
    size_t pos = 8, idat = 0, chunk_len, o = 0;
    int zlen = 0;
    uint32_t crc;

    if (!icc || !icc_len || !is_png_sig(png, png_len)) return NULL;
    while (pos + 12 <= png_len) {
        uint32_t len = be32(png + pos);
        if ((size_t)len > png_len || pos + 12 + (size_t)len > png_len)
            return NULL;
        if (memcmp(png + pos + 4, "IDAT", 4) == 0) {
            idat = pos;
            break;
        }
        pos += 12 + len;
    }
    if (!idat) return NULL;

    z = stbi_zlib_compress((unsigned char *)icc, (int)icc_len, &zlen, 8);
    if (!z || zlen <= 0) {
        if (z) STBIW_FREE(z);
        return NULL;
    }
    /* chunk data = name + NUL + method(0) + compressed profile */
    chunk_len = strlen(name) + 1 + 1 + (size_t)zlen;
    out = (unsigned char *)malloc(png_len + 12 + chunk_len);
    if (!out) {
        STBIW_FREE(z);
        return NULL;
    }
    memcpy(out, png, idat);
    o = idat;
    chunk = out + o;
    chunk[0] = (unsigned char)(chunk_len >> 24);
    chunk[1] = (unsigned char)(chunk_len >> 16);
    chunk[2] = (unsigned char)(chunk_len >> 8);
    chunk[3] = (unsigned char)chunk_len;
    memcpy(chunk + 4, "iCCP", 4);
    memcpy(chunk + 8, name, strlen(name) + 1);
    chunk[8 + strlen(name) + 1] = 0;          /* compression method: zlib */
    memcpy(chunk + 8 + strlen(name) + 2, z, (size_t)zlen);
    crc = png_crc(chunk + 4, 4 + chunk_len);
    chunk[8 + chunk_len + 0] = (unsigned char)(crc >> 24);
    chunk[8 + chunk_len + 1] = (unsigned char)(crc >> 16);
    chunk[8 + chunk_len + 2] = (unsigned char)(crc >> 8);
    chunk[8 + chunk_len + 3] = (unsigned char)crc;
    o += 12 + chunk_len;
    memcpy(out + o, png + idat, png_len - idat);
    STBIW_FREE(z);
    *out_len = png_len + 12 + chunk_len;
    return out;
}

/* Grows on demand for stbi_write_*_to_func. */
typedef struct {
    unsigned char *buf;
    size_t len, cap;
    int failed;
} mem_sink;

static void sink_put(void *ctx, void *data, int size)
{
    mem_sink *s = (mem_sink *)ctx;
    if (s->len + (size_t)size > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1 << 16;
        unsigned char *nb;
        while (nc < s->len + (size_t)size) nc *= 2;
        nb = (unsigned char *)realloc(s->buf, nc);
        if (!nb) {
            s->failed = 1;
            return;
        }
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, data, (size_t)size);
    s->len += (size_t)size;
}

/* Core: RGBA -> PNG bytes (with optional iCCP graft). */
static unsigned char *encode_png_mem(const img_pixels *p,
                                     const unsigned char *icc,
                                     size_t icc_size, size_t *out_len)
{
    unsigned char *png, *with;
    int len = 0;
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return NULL;
    png = stbi_write_png_to_mem(p->rgba, p->w * 4, p->w, p->h, 4, &len);
    if (!png || len <= 0) {
        if (png) STBIW_FREE(png);
        return NULL;
    }
    with = png_with_icc(png, (size_t)len, icc, icc_size, out_len);
    if (with) {
        STBIW_FREE(png);
        return with;   /* malloc'ed, caller frees */
    }
    /* no profile or graft failed: return stb's stream as a free-able buf */
    {
        unsigned char *copy = (unsigned char *)malloc((size_t)len);
        if (!copy) {
            STBIW_FREE(png);
            return NULL;
        }
        memcpy(copy, png, (size_t)len);
        STBIW_FREE(png);
        *out_len = (size_t)len;
        return copy;
    }
}

static bool write_file(const char *utf8_path, const unsigned char *data,
                       size_t len)
{
    FILE *f;
    bool ok;
    if (!utf8_path) return false;
    f = fopen(utf8_path, "wb");
    if (!f) return false;
    ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool img_encode_png_icc(const char *utf8_path, const img_pixels *p,
                        const unsigned char *icc, size_t icc_size)
{
    unsigned char *png;
    size_t len = 0;
    bool ok;
    png = encode_png_mem(p, icc, icc_size, &len);
    if (!png) return false;
    ok = write_file(utf8_path, png, len);
    free(png);
    return ok;
}

bool img_encode_png(const char *utf8_path, const img_pixels *p)
{
    return img_encode_png_icc(utf8_path, p, NULL, 0);
}

bool img_encode_png_memory_icc(const img_pixels *p, const unsigned char *icc,
                               size_t icc_size, void **out, size_t *size)
{
    unsigned char *png;
    if (!out || !size) return false;
    *out = NULL;
    *size = 0;
    png = encode_png_mem(p, icc, icc_size, size);
    if (!png) return false;
    *out = png;
    return true;
}

bool img_encode_png_memory(const img_pixels *p, void **out, size_t *size)
{
    return img_encode_png_memory_icc(p, NULL, 0, out, size);
}

bool img_encode_jpeg(const char *utf8_path, const img_pixels *p, int quality)
{
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0 || !utf8_path) return false;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    return stbi_write_jpg(utf8_path, p->w, p->h, 4, p->rgba, quality) != 0;
}

bool img_encode_jpeg_memory(const img_pixels *p, int quality, void **out,
                            size_t *size)
{
    mem_sink s;
    if (!out || !size || !p || !p->rgba || p->w <= 0 || p->h <= 0)
        return false;
    *out = NULL;
    *size = 0;
    memset(&s, 0, sizeof s);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    if (!stbi_write_jpg_to_func(sink_put, &s, p->w, p->h, 4, p->rgba,
                                quality) ||
        s.failed || !s.len) {
        free(s.buf);
        return false;
    }
    *out = s.buf;
    *size = s.len;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Resize                                                                     */
/* ------------------------------------------------------------------------- */

/* Separable triangle (bilinear) resampler, straight alpha. The filter width
 * scales with 1/scale, so large downscales degenerate to area-averaging --
 * the same "good enough for thumbnails" role WIC Fant fills on Windows.
 * The CLI never resizes photos through this path; eng_resample (the
 * Pillow-exact port) does the model pre-scale.
 *
 * One row in, one row out: `sc` interleaved channels per sample. */
static int resize_axis(const uint8_t *src, int sw, int sc,
                       uint8_t *dst, int dw)
{
    int i;
    double scale = (double)dw / sw;
    double fwidth = scale >= 1.0 ? 1.0 : 1.0 / scale;
    double *line = (double *)malloc((size_t)dw * sc * sizeof(double));
    if (!line) return 0;
    memset(line, 0, (size_t)dw * sc * sizeof(double));
    for (i = 0; i < dw; i++) {
        double center = (i + 0.5) / scale - 0.5;
        int lo = (int)(center - fwidth + 1);
        int hi = (int)(center + fwidth);
        double wsum = 0;
        if (lo < 0) lo = 0;
        if (hi > sw - 1) hi = sw - 1;
        for (int x = lo; x <= hi; x++) {
            double d = (x - center) / fwidth;
            double wgt = d < 0 ? -d : d;
            wgt = wgt < 1.0 ? 1.0 - wgt : 0.0;
            if (wgt == 0.0) continue;
            wsum += wgt;
            for (int c = 0; c < sc; c++)
                line[(size_t)i * sc + c] += src[(size_t)x * sc + c] * wgt;
        }
        if (wsum == 0) wsum = 1;
        for (int c = 0; c < sc; c++) {
            double v = line[(size_t)i * sc + c] / wsum;
            int iv = (int)(v + 0.5);
            dst[(size_t)i * sc + c] =
                (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
        }
    }
    free(line);
    return 1;
}

bool img_resize(const img_pixels *src, int w, int h, img_pixels *out)
{
    uint8_t *tmp;
    int ok;
    if (!out) return false;
    out->rgba = NULL;
    out->w = out->h = 0;
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0 || w <= 0 ||
        h <= 0)
        return false;
    if ((uint64_t)w * (uint64_t)h * 4u > IMG_MAX_BYTES) return false;

    /* horizontal pass: W x H -> w x H; vertical pass: w x H -> w x h.
     * resize_axis treats each row as `sc`-channel samples. */
    tmp = (uint8_t *)malloc((size_t)w * src->h * 4);
    if (!tmp) return false;
    for (int y = 0; y < src->h; y++) {
        if (!resize_axis(src->rgba + (size_t)y * src->w * 4, src->w,
                         4, tmp + (size_t)y * w * 4, w)) {
            free(tmp);
            return false;
        }
    }
    out->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out->rgba) {
        free(tmp);
        return false;
    }
    /* transpose for the vertical pass: columns become rows */
    {
        uint8_t *t_in = (uint8_t *)malloc((size_t)src->h * w * 4);
        uint8_t *t_out = (uint8_t *)malloc((size_t)h * w * 4);
        if (!t_in || !t_out) {
            free(t_in);
            free(t_out);
            free(tmp);
            free(out->rgba);
            out->rgba = NULL;
            return false;
        }
        for (int x = 0; x < w; x++)
            for (int y = 0; y < src->h; y++)
                memcpy(t_in + ((size_t)x * src->h + y) * 4,
                       tmp + ((size_t)y * w + x) * 4, 4);
        ok = 1;
        for (int x = 0; x < w && ok; x++)
            ok = resize_axis(t_in + (size_t)x * src->h * 4, src->h,
                             4, t_out + (size_t)x * h * 4, h);
        if (ok)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    memcpy(out->rgba + ((size_t)y * w + x) * 4,
                           t_out + ((size_t)x * h + y) * 4, 4);
        free(t_in);
        free(t_out);
    }
    free(tmp);
    if (!ok) {
        free(out->rgba);
        out->rgba = NULL;
        return false;
    }
    out->w = w;
    out->h = h;
    return true;
}
