/* img_common.c -- the platform-free half of img.h: pixel allocation and
 * teardown, the extension whitelist, PNG tail trimming, alpha probing, the
 * masked composite formula and the thumbnail size maths.
 *
 * These used to live inside img.c (the WIC backend). The POSIX CLI build
 * (src/posix/img_posix.c, stb-backed) needs the same helpers, so they sit in
 * their own unit here -- compiled identically on both platforms. Everything
 * that touches WIC, COM or the OS clipboard stays in img.c / img_clip.c.
 *
 * Case folding below is deliberately ASCII-only: the whitelist is ASCII and
 * _stricmp agrees with tolower for these bytes, so the behaviour is the same
 * on both platforms without dragging the CRT locale along.
 */
#include "img.h"
#include "img_priv.h"

#include <stdlib.h>
#include <string.h>

/* ASCII-only case-insensitive equality (extensions are ASCII by contract). */
static int ext_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned ca = (unsigned char)*a;
        unsigned cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* Allocates a zeroed img_pixels (caller frees with img_free). Clears *out on
 * failure. */
bool img__pixels_alloc(int w, int h, img_pixels *out)
{
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (w <= 0 || h <= 0) return false;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (bytes > IMG_MAX_BYTES) return false;
    uint8_t *px = (uint8_t *)malloc((size_t)bytes);
    if (!px) return false;
    out->rgba = px;
    out->w = w;
    out->h = h;
    return true;
}

void img_free(img_pixels *p)
{
    if (!p) return;
    free(p->rgba);
    p->rgba = NULL;
    p->w = 0;
    p->h = 0;
}

void img_free_icc(unsigned char *icc)
{
    free(icc);
}

/* Pointer to the extension of the last path component (after the final '.').
 * Byte-wise, because the extension is always ASCII even when the directory is
 * not. Returns NULL when there is no usable extension. */
static const char *path_ext(const char *path)
{
    const char *ext = NULL;
    if (!path) return NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') ext = NULL;
        else if (*p == '.') ext = p + 1;
    }
    return (ext && *ext) ? ext : NULL;
}

typedef struct img_ext_entry {
    const char *ext;
    img_format fmt;
} img_ext_entry;

/* One table for both img_supported_path and img_format_of. HEIC/HEIF decode
 * through the OS HEIF codec but img_format has no value for them, so they map
 * to IMG_FMT_UNKNOWN while still counting as supported. */
static const img_ext_entry k_exts[] = {
    { "png", IMG_FMT_PNG },
    { "jpg", IMG_FMT_JPEG },
    { "jpeg", IMG_FMT_JPEG },
    { "webp", IMG_FMT_WEBP },
    { "avif", IMG_FMT_AVIF },
    { "gif", IMG_FMT_GIF },
    { "bmp", IMG_FMT_BMP },
    { "tif", IMG_FMT_TIFF },
    { "tiff", IMG_FMT_TIFF },
    { "heic", IMG_FMT_UNKNOWN },
    { "heif", IMG_FMT_UNKNOWN },
};

bool img_supported_path(const char *utf8_path)
{
    const char *ext = path_ext(utf8_path);
    if (!ext) return false;
    for (size_t i = 0; i < sizeof k_exts / sizeof k_exts[0]; i++) {
        if (ext_ieq(ext, k_exts[i].ext)) return true;
    }
    return false;
}

img_format img_format_of(const char *utf8_path)
{
    const char *ext = path_ext(utf8_path);
    if (!ext) return IMG_FMT_UNKNOWN;
    for (size_t i = 0; i < sizeof k_exts / sizeof k_exts[0]; i++) {
        if (ext_ieq(ext, k_exts[i].ext)) return k_exts[i].fmt;
    }
    return IMG_FMT_UNKNOWN;
}

/* PNG carriers (HGLOBAL on the clipboard, HGLOBAL-backed encoder streams)
 * round their size up past the IEND chunk. Cut the buffer at IEND so strict
 * decoders never see trailing bytes. Anything that is not a PNG header is
 * passed through untouched and judged by the decoder. */
size_t img__png_trim_length(const uint8_t *data, size_t size)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t offset = 8;

    if (size < 8 || memcmp(data, signature, sizeof signature) != 0) return size;
    while (offset + 8 <= size) {
        uint32_t length = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                          ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];
        const uint8_t *type = data + offset + 4;
        if ((size_t)length > size || offset + 12 + (size_t)length > size) return size;
        offset += 12 + (size_t)length;
        if (memcmp(type, "IEND", 4) == 0) return offset;
    }
    return size;
}

/* ------------------------------------------------------------------------- */
/* Alpha                                                                      */
/* ------------------------------------------------------------------------- */

bool img_has_alpha(const img_pixels *p)
{
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    size_t count = (size_t)p->w * (size_t)p->h;
    for (size_t i = 0; i < count; i++) {
        if (p->rgba[i * 4 + 3] != 255) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Composite                                                                  */
/* ------------------------------------------------------------------------- */

bool img_composite_masked(img_pixels *out, const img_pixels *orig,
                          const uint8_t *den_rgb, const uint8_t *weight,
                          int w, int h)
{
    if (!out || !orig || !orig->rgba || !den_rgb || !weight) return false;
    if (orig->w != w || orig->h != h || w <= 0 || h <= 0) return false;
    if (!img__pixels_alloc(w, h, out)) return false;

    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++) {
        /* worker.py: (den*255 + (src-den)*m + 127) // 255 per channel. The
         * numerator can never go negative (src-den >= -den), so the integer
         * division rounds exactly like numpy's floor here. */
        int m = weight[i];
        const uint8_t *s = orig->rgba + i * 4;
        const uint8_t *d = den_rgb + i * 3;
        uint8_t *o = out->rgba + i * 4;
        o[0] = (uint8_t)((d[0] * 255 + (s[0] - d[0]) * m + 127) / 255);
        o[1] = (uint8_t)((d[1] * 255 + (s[1] - d[1]) * m + 127) / 255);
        o[2] = (uint8_t)((d[2] * 255 + (s[2] - d[2]) * m + 127) / 255);
        o[3] = s[3];
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Thumbnail                                                                  */
/* ------------------------------------------------------------------------- */

bool img_thumbnail(const img_pixels *src, int max_side, img_pixels *out)
{
    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0 || max_side <= 0) return false;

    if (src->w <= max_side && src->h <= max_side) {
        /* Never upscale: hand back a copy and let the caller treat it alike. */
        img_pixels copy;
        if (!img__pixels_alloc(src->w, src->h, &copy)) return false;
        memcpy(copy.rgba, src->rgba, (size_t)src->w * (size_t)src->h * 4u);
        *out = copy;
        return true;
    }

    int wide = src->w >= src->h ? src->w : src->h;
    double scale = (double)max_side / (double)wide;
    int w = (int)((double)src->w * scale + 0.5);
    int h = (int)((double)src->h * scale + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return img_resize(src, w, h, out);
}
