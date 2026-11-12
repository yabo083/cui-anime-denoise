/* Image IO on top of the Windows Imaging Component (WIC).
 *
 * Everything the header promises lives here: file and memory decode (first
 * frame only, EXIF orientation applied, embedded ICC profile on request,
 * 16-bit-per-channel and CMYK sources handled with PIL semantics), PNG/JPEG
 * encode to a file or to a buffer (ICC via frame colour contexts), high
 * quality resize, thumbnails, alpha probing. The clipboard half lives in
 * img_clip.c. WIC gives us WebP, AVIF, HEIF and TIFF for free through the
 * OS codecs, so no third-party decoder is involved.
 *
 * House rules, all of them enforced below:
 *   - pixels are straight (non-premultiplied) RGBA8, top-down, tight stride;
 *   - every failure leaves *out zeroed and releases every COM object on the
 *     way out (no spots, no leaks);
 *   - the factory is created once and used by the UI thread only, which is
 *     what the header promises.
 */
#include "img.h"
#include "img_priv.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <stdlib.h>
#include <string.h>

/* 8-bit-per-channel straight alpha, the format WIC decodes and scales in. */
#define IMG_PIXEL_FORMAT (&GUID_WICPixelFormat32bppRGBA)

static IWICImagingFactory *g_factory;
static bool g_com_owned;   /* true only when we called CoInitializeEx */

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

wchar_t *img__utf8_to_wide(const char *s)
{
    if (!s || !*s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
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
        if (_stricmp(ext, k_exts[i].ext) == 0) return true;
    }
    return false;
}

img_format img_format_of(const char *utf8_path)
{
    const char *ext = path_ext(utf8_path);
    if (!ext) return IMG_FMT_UNKNOWN;
    for (size_t i = 0; i < sizeof k_exts / sizeof k_exts[0]; i++) {
        if (_stricmp(ext, k_exts[i].ext) == 0) return k_exts[i].fmt;
    }
    return IMG_FMT_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

bool img_init(void)
{
    if (g_factory) return true;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        g_com_owned = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        /* Someone else picked a different apartment; use theirs, do not
         * unbalance it in img_shutdown. */
        g_com_owned = false;
    } else {
        return false;
    }

    IWICImagingFactory *factory = NULL;
#if defined(NTDDI_WIN8) && (NTDDI_VERSION >= NTDDI_WIN8)
    hr = CoCreateInstance(&CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) factory = NULL;
#endif
    if (!factory) {
        hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory, (void **)&factory);
        if (FAILED(hr)) factory = NULL;
    }
    if (!factory) {
        if (g_com_owned) CoUninitialize();
        g_com_owned = false;
        return false;
    }
    g_factory = factory;
    return true;
}

void img_shutdown(void)
{
    if (g_factory) {
        g_factory->lpVtbl->Release(g_factory);
        g_factory = NULL;
    }
    if (g_com_owned) {
        CoUninitialize();
        g_com_owned = false;
    }
}

void img_free(img_pixels *p)
{
    if (!p) return;
    free(p->rgba);
    p->rgba = NULL;
    p->w = 0;
    p->h = 0;
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                     */
/* ------------------------------------------------------------------------- */

/* EXIF orientation 1..8 as a WIC transform. WIC mirrors before it rotates, so
 * the flip+rotate pairs for 5 (transpose) and 7 (transverse) are the opposite
 * way round from a rotate-then-flip reading. Verified against a constructed
 * lossless TIFF for every value. */
static WICBitmapTransformOptions orientation_transform(UINT orientation)
{
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

/* Reads System.Photo.Orientation through the frame's metadata query reader.
 * Missing metadata is not an error: the picture is then upright. */
static void read_orientation(IWICBitmapFrameDecode *frame, UINT *out)
{
    static const wchar_t *const paths[] = {
        L"/app1/ifd/{ushort=274}",   /* EXIF IFD0: JPEG, PNG eXIf, WebP EXIF */
        L"/ifd/{ushort=274}",        /* TIFF-style containers without APP1 */
    };
    IWICMetadataQueryReader *reader = NULL;

    *out = 1;
    if (FAILED(frame->lpVtbl->GetMetadataQueryReader(frame, &reader)) || !reader) return;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->lpVtbl->GetMetadataByName(reader, paths[i], &value))) {
            UINT orientation = 0;
            switch (value.vt) {
            case VT_UI2: orientation = value.uiVal; break;
            case VT_I2: orientation = (UINT)(USHORT)value.iVal; break;
            case VT_UI4: orientation = value.ulVal; break;
            case VT_I4: orientation = (UINT)value.lVal; break;
            default: break;
            }
            PropVariantClear(&value);
            if (orientation >= 1 && orientation <= 8) {
                *out = orientation;
                break;
            }
        } else {
            PropVariantClear(&value);
        }
    }
    reader->lpVtbl->Release(reader);
}

/* How the frame's own pixel format reaches straight RGBA8. WICPixelFormatGUID
 * is checked by value (WIC substitutes e.g. BGRA for RGBA on some codecs). */
typedef enum img_src_kind {
    IMG_KIND_CONVERT = 0,   /* 8bpc family: IWICFormatConverter is fine */
    IMG_KIND_16,            /* 16-bit/channel: scale v/257 like PIL, never >>8 */
    IMG_KIND_CMYK           /* colour transform when profiled, else K-min */
} img_src_kind;

typedef struct src_format {
    const GUID *guid;
    uint8_t kind;
    uint8_t channels;   /* channel samples per pixel (gray 1, RGB 3, RGBA/CMYK 4, CMYKA 5) */
    uint8_t chan_bytes; /* bytes per channel sample (1 or 2) */
    uint8_t bgr;        /* channel order is BGR(A) instead of RGB(A) */
    uint8_t premul;     /* alpha is premultiplied */
} src_format;

/* The formats we take direct control of. Everything else lands in
 * IMG_KIND_CONVERT, including the 32bppGreyFloat and FixedPoint oddballs. */
static const src_format g_src_formats[] = {
    { &GUID_WICPixelFormat16bppGray,        IMG_KIND_16,   1, 2, 0, 0 },
    { &GUID_WICPixelFormat48bppRGB,         IMG_KIND_16,   3, 2, 0, 0 },
    { &GUID_WICPixelFormat48bppBGR,         IMG_KIND_16,   3, 2, 1, 0 },
    { &GUID_WICPixelFormat64bppRGBA,        IMG_KIND_16,   4, 2, 0, 0 },
    { &GUID_WICPixelFormat64bppBGRA,        IMG_KIND_16,   4, 2, 1, 0 },
    { &GUID_WICPixelFormat64bppPRGBA,       IMG_KIND_16,   4, 2, 0, 1 },
    { &GUID_WICPixelFormat64bppPBGRA,       IMG_KIND_16,   4, 2, 1, 1 },
    { &GUID_WICPixelFormat32bppCMYK,        IMG_KIND_CMYK, 4, 1, 0, 0 },
    { &GUID_WICPixelFormat40bppCMYKAlpha,   IMG_KIND_CMYK, 5, 1, 0, 0 },
    { &GUID_WICPixelFormat64bppCMYK,        IMG_KIND_CMYK, 4, 2, 0, 0 },
    { &GUID_WICPixelFormat80bppCMYKAlpha,   IMG_KIND_CMYK, 5, 2, 0, 0 },
};

static const src_format *find_src_format(const WICPixelFormatGUID *fmt)
{
    for (size_t i = 0; i < sizeof g_src_formats / sizeof g_src_formats[0]; i++)
        if (IsEqualGUID(fmt, g_src_formats[i].guid)) return &g_src_formats[i];
    return NULL;
}

/* RGB-family formats (plus indexed -> PIL 'P') are the only ones whose
 * embedded ICC the reference pipeline keeps; grey and CMYK drop it. */
static bool format_is_rgb_family(const WICPixelFormatGUID *fmt)
{
    static const GUID *const family[] = {
        &GUID_WICPixelFormat1bppIndexed,  &GUID_WICPixelFormat2bppIndexed,
        &GUID_WICPixelFormat4bppIndexed,  &GUID_WICPixelFormat8bppIndexed,
        &GUID_WICPixelFormat16bppBGR555,  &GUID_WICPixelFormat16bppBGR565,
        &GUID_WICPixelFormat16bppBGRA5551,
        &GUID_WICPixelFormat24bppRGB,     &GUID_WICPixelFormat24bppBGR,
        &GUID_WICPixelFormat32bppRGB,     &GUID_WICPixelFormat32bppRGBA,
        &GUID_WICPixelFormat32bppPRGBA,   &GUID_WICPixelFormat32bppBGR,
        &GUID_WICPixelFormat32bppBGRA,    &GUID_WICPixelFormat32bppPBGRA,
        &GUID_WICPixelFormat32bppBGR101010, &GUID_WICPixelFormat32bppRGBA1010102XR,
        &GUID_WICPixelFormat48bppRGB,     &GUID_WICPixelFormat48bppBGR,
        &GUID_WICPixelFormat48bppRGBFixedPoint,
        &GUID_WICPixelFormat64bppRGBA,    &GUID_WICPixelFormat64bppBGRA,
        &GUID_WICPixelFormat64bppPRGBA,   &GUID_WICPixelFormat64bppPBGRA,
        &GUID_WICPixelFormat64bppRGBFixedPoint, &GUID_WICPixelFormat64bppRGBAFixedPoint,
        &GUID_WICPixelFormat96bppRGBFloat,
        &GUID_WICPixelFormat128bppRGBFloat, &GUID_WICPixelFormat128bppRGBAFloat,
        &GUID_WICPixelFormat128bppPRGBAFloat,
    };
    for (size_t i = 0; i < sizeof family / sizeof family[0]; i++)
        if (IsEqualGUID(fmt, family[i])) return true;
    return false;
}

/* Pulls the frame's colour contexts. Returns the count found (the array holds
 * released-on-exit IWICColorContext pointers) and hands the first
 * WICColorContextProfile one back through *profile_ctx for ICC bytes /
 * CMYK transforms. */
static UINT frame_color_contexts(IWICBitmapFrameDecode *frame, IWICColorContext **ctxs,
                                 UINT capacity, IWICColorContext **profile_ctx)
{
    UINT want = 0, got = 0;

    *profile_ctx = NULL;
    if (FAILED(frame->lpVtbl->GetColorContexts(frame, 0, NULL, &want)) || !want)
        return 0;
    if (want > capacity) want = capacity;
    for (UINT i = 0; i < want; i++) {
        if (FAILED(g_factory->lpVtbl->CreateColorContext(g_factory, &ctxs[i])) || !ctxs[i])
            return got;
        got++;
    }
    if (FAILED(frame->lpVtbl->GetColorContexts(frame, got, ctxs, &got))) return got;
    for (UINT i = 0; i < got; i++) {
        WICColorContextType type;
        if (SUCCEEDED(ctxs[i]->lpVtbl->GetType(ctxs[i], &type)) &&
            type == WICColorContextProfile && !*profile_ctx)
            *profile_ctx = ctxs[i];
    }
    return got;
}

/* Copies the ICC profile bytes out of a WICColorContextProfile context. */
static unsigned char *context_profile_bytes(IWICColorContext *ctx, size_t *size)
{
    UINT bytes = 0;
    unsigned char *copy;

    *size = 0;
    if (FAILED(ctx->lpVtbl->GetProfileBytes(ctx, 0, NULL, &bytes)) || !bytes) return NULL;
    copy = (unsigned char *)malloc(bytes);
    if (!copy) return NULL;
    if (FAILED(ctx->lpVtbl->GetProfileBytes(ctx, bytes, copy, &bytes))) {
        free(copy);
        return NULL;
    }
    *size = bytes;
    return copy;
}

/* PIL's uint16 -> uint8: v / 257.0 + 0.5, clipped. The reference gates the
 * divide on the image actually reaching past 255 (I;16 containers can still
 * hold 8-bit-range values); `gated` switches it to a plain floor. */
static uint8_t scale_16_to_8(uint32_t v, bool gated)
{
    double d = gated ? (double)v + 0.5 : (double)v / 257.0 + 0.5;
    return (uint8_t)(d > 255.0 ? 255.0 : d);
}

/* 16-bit-per-channel sources, scaled the way the reference's open_image does
 * (Pillow divides by 257 and rounds; WIC's own converter just takes the high
 * byte, which is a floor - off by one on ~half the samples). Alpha is the
 * high byte like PIL's RGBA;16B unpacker. */
static bool decode_16bit(IWICBitmapSource *source, const src_format *spec, img_pixels *out)
{
    UINT w = 0, h = 0;
    uint8_t *buf = NULL;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;
    UINT stride;

    if (FAILED(source->lpVtbl->GetSize(source, &w, &h)) || w == 0 || h == 0) return false;
    if (!img__pixels_alloc((int)w, (int)h, &px)) return false;
    stride = w * (spec->chan_bytes * spec->channels);
    buf = (uint8_t *)malloc((size_t)stride * h);
    if (!buf) goto cleanup;
    if (FAILED(source->lpVtbl->CopyPixels(source, NULL, stride, stride * h, buf))) goto cleanup;

    /* PIL's I;16 rule: only divide by 257 when the data actually exceeds 255. */
    bool gated = false;
    if (spec->channels == 1) {
        uint16_t peak = 0;
        for (size_t i = 0, n = (size_t)w * h; i < n; i++)
            if (((const uint16_t *)buf)[i] > peak) peak = ((const uint16_t *)buf)[i];
        gated = peak <= 255;
    }

    for (UINT y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(buf + (size_t)y * stride);
        uint8_t *dst = px.rgba + (size_t)y * w * 4u;
        for (UINT x = 0; x < w; x++, dst += 4) {
            if (spec->channels == 1) {
                dst[0] = dst[1] = dst[2] = scale_16_to_8(row[x], gated);
                dst[3] = 255;
            } else {
                const uint16_t *s = row + (size_t)x * spec->channels;
                uint32_t r16 = s[spec->bgr ? 2u : 0u];
                uint32_t g16 = s[1];
                uint32_t b16 = s[spec->bgr ? 0u : 2u];
                uint32_t a16 = spec->channels >= 4 ? s[3] : 65535u;
                if (spec->premul && a16 && a16 < 65535u) {
                    r16 = r16 * 65535u / a16;
                    g16 = g16 * 65535u / a16;
                    b16 = b16 * 65535u / a16;
                }
                dst[0] = scale_16_to_8(r16, false);
                dst[1] = scale_16_to_8(g16, false);
                dst[2] = scale_16_to_8(b16, false);
                dst[3] = (uint8_t)(a16 >> 8);
            }
        }
    }
    *out = px;
    ok = true;

cleanup:
    free(buf);
    if (!ok) img_free(&px);
    return ok;
}

/* PIL's CMYK->RGB for untagged CMYK: R = 255 - min(255, C + K). The task only
 * guarantees parity for profiled CMYK; this fallback keeps random CMYK JPEGs
 * from failing outright. */
static bool decode_cmyk_naive(IWICBitmapSource *source, const src_format *spec, img_pixels *out)
{
    UINT w = 0, h = 0;
    uint8_t *buf = NULL;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;
    UINT bytes_pp = spec->chan_bytes * spec->channels;
    UINT stride;

    if (FAILED(source->lpVtbl->GetSize(source, &w, &h)) || w == 0 || h == 0) return false;
    if (!img__pixels_alloc((int)w, (int)h, &px)) return false;
    stride = w * bytes_pp;
    buf = (uint8_t *)malloc((size_t)stride * h);
    if (!buf) goto cleanup;
    if (FAILED(source->lpVtbl->CopyPixels(source, NULL, stride, stride * h, buf))) goto cleanup;

    for (UINT y = 0; y < h; y++) {
        const uint8_t *row = buf + (size_t)y * stride;
        uint8_t *dst = px.rgba + (size_t)y * w * 4u;
        for (UINT x = 0; x < w; x++, dst += 4, row += bytes_pp) {
            /* 16-bit CMYK: PIL keeps the high byte only (little-endian WORD). */
            unsigned c = spec->chan_bytes == 2 ? row[1] : row[0];
            unsigned m = spec->chan_bytes == 2 ? row[3] : row[1];
            unsigned yv = spec->chan_bytes == 2 ? row[5] : row[2];
            unsigned k = spec->chan_bytes == 2 ? row[7] : row[3];
            dst[0] = (uint8_t)(255 - (c + k > 255 ? 255 : c + k));
            dst[1] = (uint8_t)(255 - (m + k > 255 ? 255 : m + k));
            dst[2] = (uint8_t)(255 - (yv + k > 255 ? 255 : yv + k));
            dst[3] = spec->channels == 5 ? (spec->chan_bytes == 2 ? row[9] : row[4]) : 255;
        }
    }
    *out = px;
    ok = true;

cleanup:
    free(buf);
    if (!ok) img_free(&px);
    return ok;
}

/* Profiled CMYK -> sRGB through IWICColorTransform (the WIC analogue of the
 * reference's ImageCms.profileToProfile). Output arrives BGRA and is swapped
 * to RGBA afterwards. Returns false to let the caller fall back to naive. */
static bool decode_cmyk_transform(IWICBitmapSource *source, IWICColorContext *src_ctx,
                                  img_pixels *out)
{
    IWICColorContext *dst_ctx = NULL;
    IWICColorTransform *xform = NULL;
    uint8_t *buf = NULL;
    img_pixels px = { NULL, 0, 0 };
    UINT w = 0, h = 0;
    HRESULT hr;
    bool ok = false;

    if (FAILED(g_factory->lpVtbl->CreateColorContext(g_factory, &dst_ctx)) || !dst_ctx)
        goto cleanup;
    /* EXIF colour space 1 is sRGB. Prefer the OS's sRGB .icm when it exists:
     * a real profile file lets WCS match littleCMS's transform more closely
     * than the built-in canned sRGB context. */
    hr = dst_ctx->lpVtbl->InitializeFromFilename(
        dst_ctx, L"C:\\Windows\\System32\\spool\\drivers\\color\\sRGB Color Space Profile.icm");
    if (FAILED(hr))
        hr = dst_ctx->lpVtbl->InitializeFromExifColorSpace(dst_ctx, 1);
    if (FAILED(hr)) goto cleanup;
    if (FAILED(g_factory->lpVtbl->CreateColorTransformer(g_factory, &xform)) || !xform)
        goto cleanup;
    if (FAILED(xform->lpVtbl->Initialize(xform, source, src_ctx, dst_ctx,
                                         &GUID_WICPixelFormat32bppBGRA)))
        goto cleanup;
    if (FAILED(xform->lpVtbl->GetSize(xform, &w, &h)) || w == 0 || h == 0) goto cleanup;
    if (!img__pixels_alloc((int)w, (int)h, &px)) goto cleanup;

    UINT stride = w * 4u;
    buf = (uint8_t *)malloc((size_t)stride * h);
    if (!buf) goto cleanup;
    if (FAILED(xform->lpVtbl->CopyPixels(xform, NULL, stride, stride * h, buf))) goto cleanup;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        px.rgba[i * 4 + 0] = buf[i * 4 + 2];
        px.rgba[i * 4 + 1] = buf[i * 4 + 1];
        px.rgba[i * 4 + 2] = buf[i * 4 + 0];
        px.rgba[i * 4 + 3] = buf[i * 4 + 3];
    }
    *out = px;
    ok = true;

cleanup:
    free(buf);
    if (!ok) img_free(&px);
    if (xform) xform->lpVtbl->Release(xform);
    if (dst_ctx) dst_ctx->lpVtbl->Release(dst_ctx);
    return ok;
}

/* Shared tail of both decoders: first frame -> orientation -> RGBA (+ ICC). */
static bool decode_frame(IWICBitmapDecoder *decoder, img_pixels *out,
                         unsigned char **icc, size_t *icc_size)
{
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapSource *source;
    IWICColorContext *ctxs[8] = { NULL };
    IWICColorContext *profile_ctx = NULL;
    UINT context_count = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormatUndefined;
    const src_format *spec = NULL;
    UINT orientation = 1;
    UINT w = 0, h = 0;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;

    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) || !frame) goto cleanup;
    frame->lpVtbl->GetPixelFormat(frame, &fmt);
    spec = find_src_format(&fmt);

    read_orientation(frame, &orientation);
    source = (IWICBitmapSource *)frame;
    if (orientation > 1) {
        if (SUCCEEDED(g_factory->lpVtbl->CreateBitmapFlipRotator(g_factory, &rotator)) &&
            SUCCEEDED(rotator->lpVtbl->Initialize(rotator, source, orientation_transform(orientation)))) {
            source = (IWICBitmapSource *)rotator;
        }
    }

    /* Colour contexts are cheap to grab and needed for CMYK transforms as
     * well as for the ICC the caller may want back. */
    context_count = frame_color_contexts(frame, ctxs, 8, &profile_ctx);

    if (spec && spec->kind == IMG_KIND_16) {
        ok = decode_16bit(source, spec, &px);
    } else if (spec && spec->kind == IMG_KIND_CMYK) {
        ok = profile_ctx ? decode_cmyk_transform(source, profile_ctx, &px) : false;
        if (!ok) ok = decode_cmyk_naive(source, spec, &px);
    } else {
        if (FAILED(g_factory->lpVtbl->CreateFormatConverter(g_factory, &converter)) || !converter)
            goto cleanup;
        if (FAILED(converter->lpVtbl->Initialize(converter, source, IMG_PIXEL_FORMAT,
                                                 WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeCustom)))
            goto cleanup;
        if (FAILED(converter->lpVtbl->GetSize(converter, &w, &h))) goto cleanup;
        if (w == 0 || h == 0) goto cleanup;
        if (!img__pixels_alloc((int)w, (int)h, &px)) goto cleanup;
        UINT stride = w * 4u;
        if (FAILED(converter->lpVtbl->CopyPixels(converter, NULL, stride, stride * h, px.rgba)))
            goto cleanup;
        ok = true;
    }
    if (!ok) goto cleanup;

    if (icc && icc_size && profile_ctx && format_is_rgb_family(&fmt)) {
        *icc = context_profile_bytes(profile_ctx, icc_size);
        if (!*icc) *icc_size = 0;
    }
    *out = px;
    px.rgba = NULL;
    ok = true;

cleanup:
    img_free(&px);
    for (UINT i = 0; i < context_count; i++) ctxs[i]->lpVtbl->Release(ctxs[i]);
    if (converter) converter->lpVtbl->Release(converter);
    if (rotator) rotator->lpVtbl->Release(rotator);
    if (frame) frame->lpVtbl->Release(frame);
    return ok;
}

static bool decode_file_common(const char *utf8_path, img_pixels *out,
                               unsigned char **icc, size_t *icc_size)
{
    IWICBitmapDecoder *decoder = NULL;
    wchar_t *path;
    bool ok;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (icc) *icc = NULL;
    if (icc_size) *icc_size = 0;
    if (!utf8_path || !*utf8_path || !img_init()) return false;

    path = img__utf8_to_wide(utf8_path);
    if (!path) return false;
    HRESULT hr = g_factory->lpVtbl->CreateDecoderFromFilename(
        g_factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    free(path);
    if (FAILED(hr) || !decoder) return false;

    ok = decode_frame(decoder, out, icc, icc_size);
    decoder->lpVtbl->Release(decoder);
    return ok;
}

static bool decode_memory_common(const void *data, size_t size, img_pixels *out,
                                 unsigned char **icc, size_t *icc_size)
{
    IWICBitmapDecoder *decoder = NULL;
    IStream *stream;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (icc) *icc = NULL;
    if (icc_size) *icc_size = 0;
    if (!data || size < 8 || size > (size_t)UINT_MAX || !img_init()) return false;

    stream = SHCreateMemStream((const BYTE *)data, (UINT)size);
    if (!stream) return false;

    HRESULT hr = g_factory->lpVtbl->CreateDecoderFromStream(
        g_factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
    stream->lpVtbl->Release(stream);

    if (FAILED(hr) || !decoder) return false;
    bool ok = decode_frame(decoder, out, icc, icc_size);
    decoder->lpVtbl->Release(decoder);
    return ok;
}

bool img_decode_file(const char *utf8_path, img_pixels *out)
{
    return decode_file_common(utf8_path, out, NULL, NULL);
}

bool img_decode_file_ex(const char *utf8_path, img_pixels *out,
                        unsigned char **icc, size_t *icc_size)
{
    return decode_file_common(utf8_path, out, icc, icc_size);
}

bool img_decode_memory(const void *data, size_t size, img_pixels *out)
{
    return decode_memory_common(data, size, out, NULL, NULL);
}

bool img_decode_memory_ex(const void *data, size_t size, img_pixels *out,
                          unsigned char **icc, size_t *icc_size)
{
    return decode_memory_common(data, size, out, icc, icc_size);
}

void img_free_icc(unsigned char *icc)
{
    free(icc);
}

/* ------------------------------------------------------------------------- */
/* Encode                                                                     */
/* ------------------------------------------------------------------------- */

/* WIC's PNG encoder only really supports 32bppBGRA: asking for 32bppRGBA
 * succeeds but hands back the BGRA GUID, so the channel order is swapped by
 * hand when that happens. Alpha stays straight in both cases - no premultiply
 * and no colour management, so a round trip is bit exact. `icc` (when given)
 * lands in an iCCP chunk via a frame colour context, matching the reference's
 * save_png. A profile WIC refuses is skipped, never fatal. */
static bool png_write_frame(IWICBitmapEncoder *encoder, const img_pixels *p,
                            const unsigned char *icc, size_t icc_size)
{
    IWICBitmapFrameEncode *frame = NULL;
    IWICColorContext *ctx = NULL;
    IPropertyBag2 *options = NULL;
    uint8_t *bgra = NULL;
    const uint8_t *source;
    size_t count = (size_t)p->w * (size_t)p->h;
    bool ok = false;

    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options)) || !frame) goto cleanup;
    if (FAILED(frame->lpVtbl->Initialize(frame, options))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetSize(frame, (UINT)p->w, (UINT)p->h))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetResolution(frame, 96, 96))) goto cleanup;

    /* Fully opaque images go out as 24bpp RGB: Pillow did the same for RGB
     * sources and it keeps the output file ~25% smaller. Anything with a
     * real alpha channel keeps 32bpp. */
    bool opaque = true;
    for (size_t i = 0; i < count; i++) {
        if (p->rgba[i * 4 + 3] != 255) { opaque = false; break; }
    }
    GUID format = opaque ? GUID_WICPixelFormat24bppBGR
                         : GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &format))) goto cleanup;

    if (icc && icc_size && icc_size <= UINT_MAX &&
        SUCCEEDED(g_factory->lpVtbl->CreateColorContext(g_factory, &ctx)) && ctx &&
        SUCCEEDED(ctx->lpVtbl->InitializeFromMemory(ctx, (const BYTE *)icc, (UINT)icc_size))) {
        frame->lpVtbl->SetColorContexts(frame, 1, &ctx);   /* best effort */
    }

    UINT stride;
    if (IsEqualGUID(&format, &GUID_WICPixelFormat32bppRGBA)) {
        source = p->rgba;
        stride = (UINT)p->w * 4u;
    } else if (IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA)) {
        bgra = (uint8_t *)malloc(count * 4);
        if (!bgra) goto cleanup;
        for (size_t i = 0; i < count; i++) {
            bgra[i * 4 + 0] = p->rgba[i * 4 + 2];
            bgra[i * 4 + 1] = p->rgba[i * 4 + 1];
            bgra[i * 4 + 2] = p->rgba[i * 4 + 0];
            bgra[i * 4 + 3] = p->rgba[i * 4 + 3];
        }
        source = bgra;
        stride = (UINT)p->w * 4u;
    } else if (IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR)) {
        bgra = (uint8_t *)malloc(count * 3);
        if (!bgra) goto cleanup;
        for (size_t i = 0; i < count; i++) {
            bgra[i * 3 + 0] = p->rgba[i * 4 + 2];
            bgra[i * 3 + 1] = p->rgba[i * 4 + 1];
            bgra[i * 3 + 2] = p->rgba[i * 4 + 0];
        }
        source = bgra;
        stride = (UINT)p->w * 3u;
    } else {
        goto cleanup;   /* anything else would silently change the pixels */
    }

    if (FAILED(frame->lpVtbl->WritePixels(frame, (UINT)p->h, stride, stride * (UINT)p->h,
                                          (BYTE *)source)))
        goto cleanup;
    if (FAILED(frame->lpVtbl->Commit(frame))) goto cleanup;
    if (FAILED(encoder->lpVtbl->Commit(encoder))) goto cleanup;
    ok = true;

cleanup:
    free(bgra);
    if (ctx) ctx->lpVtbl->Release(ctx);
    if (options) options->lpVtbl->Release(options);
    if (frame) frame->lpVtbl->Release(frame);
    return ok;
}

/* JPEG has no alpha, so the image is composited over white into 24bppBGR.
 * The header allows either that or refusing alpha images; compositing keeps
 * "save as JPEG" working on any picture the app can hold. */
static bool jpeg_write_frame(IWICBitmapEncoder *encoder, const img_pixels *p, int quality)
{
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    uint8_t *bgr = NULL;
    size_t count = (size_t)p->w * (size_t)p->h;
    bool ok = false;

    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options)) || !frame) goto cleanup;
    if (options) {
        PROPBAG2 option;
        VARIANT value;
        memset(&option, 0, sizeof option);
        VariantInit(&value);
        option.pstrName = (LPOLESTR)L"ImageQuality";
        option.vt = VT_R4;
        value.vt = VT_R4;
        value.fltVal = (float)(quality < 0 ? 0 : (quality > 100 ? 100 : quality)) / 100.0f;
        options->lpVtbl->Write(options, 1, &option, &value);
    }
    if (FAILED(frame->lpVtbl->Initialize(frame, options))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetSize(frame, (UINT)p->w, (UINT)p->h))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetResolution(frame, 96, 96))) goto cleanup;

    GUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &format))) goto cleanup;
    if (!IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR)) goto cleanup;

    bgr = (uint8_t *)malloc(count * 3);
    if (!bgr) goto cleanup;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *s = p->rgba + i * 4;
        unsigned alpha = s[3];
        for (int c = 0; c < 3; c++) {
            /* c * a + 255 * (255 - a), rounded; c is R,G,B (stored B,G,R). */
            unsigned v = ((unsigned)s[2 - c] * alpha + 255u * (255u - alpha) + 127u) / 255u;
            bgr[i * 3 + c] = (uint8_t)v;
        }
    }

    UINT stride = (UINT)p->w * 3u;
    if (FAILED(frame->lpVtbl->WritePixels(frame, (UINT)p->h, stride, stride * (UINT)p->h,
                                          (BYTE *)bgr)))
        goto cleanup;
    if (FAILED(frame->lpVtbl->Commit(frame))) goto cleanup;
    if (FAILED(encoder->lpVtbl->Commit(encoder))) goto cleanup;
    ok = true;

cleanup:
    free(bgr);
    if (options) options->lpVtbl->Release(options);
    if (frame) frame->lpVtbl->Release(frame);
    return ok;
}

/* Opens an encoder on `stream` (file-backed or memory-backed, both are plain
 * IStreams) and writes one frame. `icc` is only honoured for PNG. */
static bool encode_stream(IStream *stream, const img_pixels *p, bool png, int quality,
                          const unsigned char *icc, size_t icc_size)
{
    IWICBitmapEncoder *encoder = NULL;
    REFGUID container = png ? &GUID_ContainerFormatPng : &GUID_ContainerFormatJpeg;
    LARGE_INTEGER start;
    bool ok = false;

    start.QuadPart = 0;
    stream->lpVtbl->Seek(stream, start, STREAM_SEEK_SET, NULL);

    if (FAILED(g_factory->lpVtbl->CreateEncoder(g_factory, container, NULL, &encoder)) || !encoder)
        return false;
    if (SUCCEEDED(encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache))) {
        ok = png ? png_write_frame(encoder, p, icc, icc_size)
                 : jpeg_write_frame(encoder, p, quality);
    }
    encoder->lpVtbl->Release(encoder);
    return ok;
}

/* Copies the bytes an encoder wrote into a fresh malloc buffer. */
static bool stream_to_buffer(IStream *stream, void **out, size_t *size)
{
    STATSTG stat;
    HGLOBAL block = NULL;
    const void *base;
    size_t bytes;
    uint8_t *copy;

    memset(&stat, 0, sizeof stat);
    if (FAILED(stream->lpVtbl->Stat(stream, &stat, STATFLAG_NONAME))) return false;
    if (stat.cbSize.QuadPart <= 0) return false;
    if ((ULONGLONG)stat.cbSize.QuadPart > (ULONGLONG)(size_t)-1) return false;
    if (FAILED(GetHGlobalFromStream(stream, &block)) || !block) return false;

    bytes = (size_t)stat.cbSize.QuadPart;
    base = GlobalLock(block);
    if (!base) return false;
    copy = (uint8_t *)malloc(bytes);
    if (copy) memcpy(copy, base, bytes);
    GlobalUnlock(block);
    if (!copy) return false;

    *out = copy;
    *size = bytes;
    return true;
}

static bool encode_file(const char *utf8_path, const img_pixels *p, bool png, int quality,
                        const unsigned char *icc, size_t icc_size)
{
    IWICStream *stream = NULL;
    wchar_t *path;
    bool created = false;
    bool ok = false;

    if (!utf8_path || !*utf8_path || !p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    if (!img_init()) return false;
    path = img__utf8_to_wide(utf8_path);
    if (!path) return false;

    if (SUCCEEDED(g_factory->lpVtbl->CreateStream(g_factory, &stream)) && stream) {
        if (SUCCEEDED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))) {
            created = true;
            ok = encode_stream((IStream *)stream, p, png, quality, icc, icc_size);
        }
    }
    if (stream) stream->lpVtbl->Release(stream);
    if (!ok && created) DeleteFileW(path);   /* never leave a half-written picture */
    free(path);
    return ok;
}

/* PNG carriers (HGLOBAL on the clipboard, HGLOBAL-backed encoder streams)
 * round their size up past the IEND chunk. Cut the buffer at IEND so strict
 * decoders never see trailing bytes. Anything that is not a PNG header is
 * passed through untouched and judged by WIC. */
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

static bool encode_memory(const img_pixels *p, bool png, int quality,
                          const unsigned char *icc, size_t icc_size,
                          void **out, size_t *size)
{
    IStream *stream = NULL;
    bool ok;

    if (!out || !size) return false;
    *out = NULL;
    *size = 0;
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    if (!img_init()) return false;

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !stream) return false;
    ok = encode_stream(stream, p, png, quality, icc, icc_size) &&
         stream_to_buffer(stream, out, size);
    stream->lpVtbl->Release(stream);
    if (ok && png) {
        /* The HGLOBAL behind the stream can be larger than the PNG itself. */
        size_t exact = img__png_trim_length((const uint8_t *)*out, *size);
        if (exact >= 8 && exact <= *size) *size = exact;
    }
    if (!ok) {
        free(*out);
        *out = NULL;
        *size = 0;
    }
    return ok;
}

bool img_encode_png(const char *utf8_path, const img_pixels *p)
{
    return encode_file(utf8_path, p, true, 0, NULL, 0);
}

bool img_encode_jpeg(const char *utf8_path, const img_pixels *p, int quality)
{
    return encode_file(utf8_path, p, false, quality, NULL, 0);
}

bool img_encode_png_icc(const char *utf8_path, const img_pixels *p,
                        const unsigned char *icc, size_t icc_size)
{
    return encode_file(utf8_path, p, true, 0, icc, icc_size);
}

bool img_encode_png_memory(const img_pixels *p, void **out, size_t *size)
{
    return encode_memory(p, true, 0, NULL, 0, out, size);
}

bool img_encode_png_memory_icc(const img_pixels *p, const unsigned char *icc,
                               size_t icc_size, void **out, size_t *size)
{
    return encode_memory(p, true, 0, icc, icc_size, out, size);
}

bool img_encode_jpeg_memory(const img_pixels *p, int quality, void **out, size_t *size)
{
    return encode_memory(p, false, quality, NULL, 0, out, size);
}

/* ------------------------------------------------------------------------- */
/* Resize                                                                     */
/* ------------------------------------------------------------------------- */

bool img_resize(const img_pixels *src, int w, int h, img_pixels *out)
{
    IWICBitmap *bitmap = NULL;
    IWICBitmapScaler *scaler = NULL;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0 || w <= 0 || h <= 0) return false;
    if (!img_init()) return false;

    UINT stride = (UINT)src->w * 4u;
    if (FAILED(g_factory->lpVtbl->CreateBitmapFromMemory(g_factory, (UINT)src->w, (UINT)src->h,
                                                        IMG_PIXEL_FORMAT, stride,
                                                        stride * (UINT)src->h, src->rgba, &bitmap)) ||
        !bitmap)
        return false;

    if (SUCCEEDED(g_factory->lpVtbl->CreateBitmapScaler(g_factory, &scaler)) && scaler) {
        /* Fant is the best resampler WIC has for downscales; fall back to the
         * cubic modes on builds that refuse it. */
        HRESULT hr = scaler->lpVtbl->Initialize(scaler, (IWICBitmapSource *)bitmap, (UINT)w, (UINT)h,
                                                WICBitmapInterpolationModeFant);
        if (FAILED(hr)) {
            hr = scaler->lpVtbl->Initialize(scaler, (IWICBitmapSource *)bitmap, (UINT)w, (UINT)h,
                                            WICBitmapInterpolationModeHighQualityCubic);
        }
        if (FAILED(hr)) {
            hr = scaler->lpVtbl->Initialize(scaler, (IWICBitmapSource *)bitmap, (UINT)w, (UINT)h,
                                            WICBitmapInterpolationModeCubic);
        }
        if (SUCCEEDED(hr) && img__pixels_alloc(w, h, &px)) {
            UINT out_stride = (UINT)w * 4u;
            if (SUCCEEDED(scaler->lpVtbl->CopyPixels(scaler, NULL, out_stride,
                                                     out_stride * (UINT)h, px.rgba))) {
                *out = px;
                ok = true;
            } else {
                img_free(&px);
            }
        }
    }

    if (scaler) scaler->lpVtbl->Release(scaler);
    bitmap->lpVtbl->Release(bitmap);
    return ok;
}

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

