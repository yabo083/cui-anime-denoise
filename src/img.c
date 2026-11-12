/* Image IO on top of the Windows Imaging Component (WIC).
 *
 * Everything the header promises lives here: file and memory decode (first
 * frame only, EXIF orientation applied), PNG/JPEG encode to a file or to a
 * buffer, high quality resize, thumbnails, alpha probing and the clipboard
 * (CF_DIBV5 / CF_DIB / CF_PNG). WIC gives us WebP, AVIF, HEIF and TIFF for
 * free through the OS codecs, so no third-party decoder is involved.
 *
 * House rules, all of them enforced below:
 *   - pixels are straight (non-premultiplied) RGBA8, top-down, tight stride;
 *   - every failure leaves *out zeroed and releases every COM object on the
 *     way out (no spots, no leaks);
 *   - the factory is created once and used by the UI thread only, which is
 *     what the header promises.
 */
#include "img.h"

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

/* Refuse to allocate anything bigger than this while decoding; 2 GiB is well
 * past any anime frame we would ever denoise (a 4096x4096 mask is 64 MiB). */
#define IMG_MAX_BYTES ((uint64_t)2048u * 1024u * 1024u)

/* BITMAPINFOHEADER compression value that asks for a fourth (alpha) mask.
 * wingdi.h only defines it when WINVER >= 0x0601, so keep our own name. */
#define IMG_BI_ALPHABITFIELDS 6

/* 8-bit-per-channel straight alpha, the format WIC decodes and scales in. */
#define IMG_PIXEL_FORMAT (&GUID_WICPixelFormat32bppRGBA)

static IWICImagingFactory *g_factory;
static bool g_com_owned;   /* true only when we called CoInitializeEx */

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static wchar_t *utf8_to_wide(const char *s)
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
static bool pixels_alloc(int w, int h, img_pixels *out)
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

/* Shared tail of both decoders: first frame -> orientation -> straight RGBA. */
static bool decode_frame(IWICBitmapDecoder *decoder, img_pixels *out)
{
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapSource *source;
    UINT orientation = 1;
    UINT w = 0, h = 0;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;

    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) || !frame) goto cleanup;

    read_orientation(frame, &orientation);
    source = (IWICBitmapSource *)frame;
    if (orientation > 1) {
        if (SUCCEEDED(g_factory->lpVtbl->CreateBitmapFlipRotator(g_factory, &rotator)) &&
            SUCCEEDED(rotator->lpVtbl->Initialize(rotator, source, orientation_transform(orientation)))) {
            source = (IWICBitmapSource *)rotator;
        }
    }

    if (FAILED(g_factory->lpVtbl->CreateFormatConverter(g_factory, &converter)) || !converter) goto cleanup;
    if (FAILED(converter->lpVtbl->Initialize(converter, source, IMG_PIXEL_FORMAT,
                                             WICBitmapDitherTypeNone, NULL, 0.0,
                                             WICBitmapPaletteTypeCustom)))
        goto cleanup;
    if (FAILED(converter->lpVtbl->GetSize(converter, &w, &h))) goto cleanup;
    if (w == 0 || h == 0) goto cleanup;
    if (!pixels_alloc((int)w, (int)h, &px)) goto cleanup;

    UINT stride = w * 4u;
    if (FAILED(converter->lpVtbl->CopyPixels(converter, NULL, stride, stride * h, px.rgba))) goto cleanup;

    *out = px;
    ok = true;

cleanup:
    if (!ok) img_free(&px);
    if (converter) converter->lpVtbl->Release(converter);
    if (rotator) rotator->lpVtbl->Release(rotator);
    if (frame) frame->lpVtbl->Release(frame);
    return ok;
}

bool img_decode_file(const char *utf8_path, img_pixels *out)
{
    IWICBitmapDecoder *decoder = NULL;
    wchar_t *path;
    bool ok;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (!utf8_path || !*utf8_path || !img_init()) return false;

    path = utf8_to_wide(utf8_path);
    if (!path) return false;
    HRESULT hr = g_factory->lpVtbl->CreateDecoderFromFilename(
        g_factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    free(path);
    if (FAILED(hr) || !decoder) return false;

    ok = decode_frame(decoder, out);
    decoder->lpVtbl->Release(decoder);
    return ok;
}

bool img_decode_memory(const void *data, size_t size, img_pixels *out)
{
    IWICBitmapDecoder *decoder = NULL;
    IStream *stream;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (!data || size < 8 || size > (size_t)UINT_MAX || !img_init()) return false;

    stream = SHCreateMemStream((const BYTE *)data, (UINT)size);
    if (!stream) return false;

    HRESULT hr = g_factory->lpVtbl->CreateDecoderFromStream(
        g_factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
    stream->lpVtbl->Release(stream);

    if (FAILED(hr) || !decoder) return false;
    bool ok = decode_frame(decoder, out);
    decoder->lpVtbl->Release(decoder);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Encode                                                                     */
/* ------------------------------------------------------------------------- */

/* WIC's PNG encoder only really supports 32bppBGRA: asking for 32bppRGBA
 * succeeds but hands back the BGRA GUID, so the channel order is swapped by
 * hand when that happens. Alpha stays straight in both cases - no premultiply
 * and no colour management, so a round trip is bit exact. */
static bool png_write_frame(IWICBitmapEncoder *encoder, const img_pixels *p)
{
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    uint8_t *bgra = NULL;
    const uint8_t *source;
    size_t count = (size_t)p->w * (size_t)p->h;
    bool ok = false;

    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options)) || !frame) goto cleanup;
    if (FAILED(frame->lpVtbl->Initialize(frame, options))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetSize(frame, (UINT)p->w, (UINT)p->h))) goto cleanup;
    if (FAILED(frame->lpVtbl->SetResolution(frame, 96, 96))) goto cleanup;

    GUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &format))) goto cleanup;

    if (IsEqualGUID(&format, &GUID_WICPixelFormat32bppRGBA)) {
        source = p->rgba;
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
    } else {
        goto cleanup;   /* anything else would silently change the pixels */
    }

    UINT stride = (UINT)p->w * 4u;
    if (FAILED(frame->lpVtbl->WritePixels(frame, (UINT)p->h, stride, stride * (UINT)p->h,
                                          (BYTE *)source)))
        goto cleanup;
    if (FAILED(frame->lpVtbl->Commit(frame))) goto cleanup;
    if (FAILED(encoder->lpVtbl->Commit(encoder))) goto cleanup;
    ok = true;

cleanup:
    free(bgra);
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
 * IStreams) and writes one frame. */
static bool encode_stream(IStream *stream, const img_pixels *p, bool png, int quality)
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
        ok = png ? png_write_frame(encoder, p) : jpeg_write_frame(encoder, p, quality);
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

static bool encode_file(const char *utf8_path, const img_pixels *p, bool png, int quality)
{
    IWICStream *stream = NULL;
    wchar_t *path;
    bool created = false;
    bool ok = false;

    if (!utf8_path || !*utf8_path || !p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    if (!img_init()) return false;
    path = utf8_to_wide(utf8_path);
    if (!path) return false;

    if (SUCCEEDED(g_factory->lpVtbl->CreateStream(g_factory, &stream)) && stream) {
        if (SUCCEEDED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))) {
            created = true;
            ok = encode_stream((IStream *)stream, p, png, quality);
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
static size_t png_trim_length(const uint8_t *data, size_t size)
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

static bool encode_memory(const img_pixels *p, bool png, int quality, void **out, size_t *size)
{
    IStream *stream = NULL;
    bool ok;

    if (!out || !size) return false;
    *out = NULL;
    *size = 0;
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    if (!img_init()) return false;

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !stream) return false;
    ok = encode_stream(stream, p, png, quality) && stream_to_buffer(stream, out, size);
    stream->lpVtbl->Release(stream);
    if (ok && png) {
        /* The HGLOBAL behind the stream can be larger than the PNG itself. */
        size_t exact = png_trim_length((const uint8_t *)*out, *size);
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
    return encode_file(utf8_path, p, true, 0);
}

bool img_encode_jpeg(const char *utf8_path, const img_pixels *p, int quality)
{
    return encode_file(utf8_path, p, false, quality);
}

bool img_encode_png_memory(const img_pixels *p, void **out, size_t *size)
{
    return encode_memory(p, true, 0, out, size);
}

bool img_encode_jpeg_memory(const img_pixels *p, int quality, void **out, size_t *size)
{
    return encode_memory(p, false, quality, out, size);
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
        if (SUCCEEDED(hr) && pixels_alloc(w, h, &px)) {
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
        if (!pixels_alloc(src->w, src->h, &copy)) return false;
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
/* Clipboard                                                                  */
/* ------------------------------------------------------------------------- */

/* Another process may hold the clipboard for a few milliseconds around a copy;
 * retry briefly instead of failing the user's paste/picture operation. */
static bool clipboard_open(void)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (OpenClipboard(NULL)) return true;
        Sleep(10);
    }
    return false;
}

typedef struct channel_mask {
    uint32_t mask;
    unsigned shift;
    unsigned bits;
} channel_mask;

static channel_mask channel_mask_make(uint32_t mask)
{
    channel_mask m;
    uint32_t bits;

    m.mask = mask;
    m.shift = 0;
    m.bits = 0;
    if (!mask) return m;
    while (m.shift < 32 && ((mask >> m.shift) & 1u) == 0) m.shift++;
    bits = mask >> m.shift;
    while ((bits & 1u) != 0) {
        m.bits++;
        bits >>= 1;
    }
    return m;
}

/* Extracts one channel from a little-endian pixel word and scales it to 8 bit
 * with rounding, so 16-bit or 5-6-5 DIBs survive too. */
static uint8_t channel_get(uint32_t pixel, channel_mask m)
{
    uint32_t value;
    uint32_t max_value;

    if (!m.mask || !m.bits) return 0;
    value = (pixel & m.mask) >> m.shift;
    if (m.bits == 8) return (uint8_t)value;
    max_value = m.bits >= 32 ? 0xFFFFFFFFu : ((1u << m.bits) - 1u);
    return (uint8_t)((value * 255u + max_value / 2u) / max_value);
}

/* Parses a packed DIB (BITMAPINFOHEADER and up). Supports 24/32bpp, BI_RGB,
 * BI_BITFIELDS and BI_ALPHABITFIELDS, bottom-up and top-down. */
static bool dib_parse(const uint8_t *data, size_t size, img_pixels *out)
{
    const BITMAPINFOHEADER *info = (const BITMAPINFOHEADER *)data;
    uint32_t red_mask = 0, green_mask = 0, blue_mask = 0, alpha_mask = 0;
    size_t header_size, pixels_offset, stride;
    int w, h;
    bool top_down;
    channel_mask red, green, blue, alpha;
    img_pixels px = { NULL, 0, 0 };
    bool ok = false;

    if (!data || size < sizeof(BITMAPINFOHEADER)) return false;
    if (info->biSize < sizeof(BITMAPINFOHEADER) || info->biSize > size) return false;
    if (info->biPlanes != 1) return false;

    w = (int)info->biWidth;
    h = (int)info->biHeight;
    top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0) return false;
    if (info->biBitCount != 24 && info->biBitCount != 32) return false;

    header_size = info->biSize;
    if (info->biCompression == BI_BITFIELDS || info->biCompression == IMG_BI_ALPHABITFIELDS) {
        if (info->biSize >= 108) {
            /* BITMAPV4/V5HEADER carries the masks itself. */
            const BITMAPV5HEADER *v5 = (const BITMAPV5HEADER *)data;
            red_mask = v5->bV5RedMask;
            green_mask = v5->bV5GreenMask;
            blue_mask = v5->bV5BlueMask;
            alpha_mask = v5->bV5AlphaMask;
        } else {
            size_t mask_bytes = (info->biBitCount == 32 && info->biCompression == IMG_BI_ALPHABITFIELDS)
                                    ? 16u : 12u;
            if (header_size + mask_bytes > size) return false;
            memcpy(&red_mask, data + header_size, 4);
            memcpy(&green_mask, data + header_size + 4, 4);
            memcpy(&blue_mask, data + header_size + 8, 4);
            if (mask_bytes == 16u) memcpy(&alpha_mask, data + header_size + 12, 4);
            header_size += mask_bytes;
        }
        if (!red_mask && !green_mask && !blue_mask) return false;
    } else if (info->biCompression != BI_RGB) {
        return false;   /* RLE/JPEG/PNG-embedded DIBs are not worth supporting */
    } else {
        /* BI_RGB declares no channel masks; the layout is fixed BGR(A). A
         * 32bpp BI_RGB byte 3 is padding, so it is not used as alpha. */
        red_mask = 0x00FF0000u;
        green_mask = 0x0000FF00u;
        blue_mask = 0x000000FFu;
    }

    pixels_offset = header_size;
    stride = (((size_t)w * info->biBitCount + 31u) / 32u) * 4u;
    if (pixels_offset + stride * (size_t)h > size) return false;

    if (!pixels_alloc(w, h, &px)) return false;

    red = channel_mask_make(red_mask);
    green = channel_mask_make(green_mask);
    blue = channel_mask_make(blue_mask);
    alpha = channel_mask_make(alpha_mask);

    for (int y = 0; y < h; y++) {
        int source_y = top_down ? y : (h - 1 - y);
        const uint8_t *row = data + pixels_offset + stride * (size_t)source_y;
        uint8_t *dst = px.rgba + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            uint32_t word = (uint32_t)row[0] | ((uint32_t)row[1] << 8) | ((uint32_t)row[2] << 16);
            uint8_t a = 255;
            if (info->biBitCount == 32) {
                word |= (uint32_t)row[3] << 24;
                if (alpha.mask) a = channel_get(word, alpha);
            }
            dst[0] = channel_get(word, red);
            dst[1] = channel_get(word, green);
            dst[2] = channel_get(word, blue);
            dst[3] = a;
            row += info->biBitCount / 8u;
            dst += 4;
        }
    }

    *out = px;
    ok = true;
    return ok;
}

static bool clipboard_get_png(img_pixels *out)
{
    UINT format = RegisterClipboardFormatW(L"PNG");
    HGLOBAL block;
    const uint8_t *base;
    SIZE_T size;
    size_t length;
    bool ok = false;

    if (!format) return false;
    block = (HGLOBAL)GetClipboardData(format);
    if (!block) return false;
    base = (const uint8_t *)GlobalLock(block);
    if (!base) return false;
    size = GlobalSize(block);
    length = png_trim_length(base, (size_t)size);
    if (length >= 8) ok = img_decode_memory(base, length, out);
    GlobalUnlock(block);
    return ok;
}

static bool clipboard_get_dib(UINT format, img_pixels *out)
{
    HGLOBAL block = (HGLOBAL)GetClipboardData(format);
    const uint8_t *base;
    SIZE_T size;
    bool ok;

    if (!block) return false;
    base = (const uint8_t *)GlobalLock(block);
    if (!base) return false;
    size = GlobalSize(block);
    ok = dib_parse(base, (size_t)size, out);
    GlobalUnlock(block);
    return ok;
}

bool img_clipboard_has_image(void)
{
    UINT png = RegisterClipboardFormatW(L"PNG");
    if (IsClipboardFormatAvailable(CF_DIBV5)) return true;
    if (IsClipboardFormatAvailable(CF_DIB)) return true;
    if (png && IsClipboardFormatAvailable(png)) return true;
    return false;
}

bool img_clipboard_get(img_pixels *out)
{
    bool ok;

    if (!out) return false;
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;
    if (!img_init()) return false;
    if (!clipboard_open()) return false;

    /* PNG first: it is the only lossless, alpha-carrying format here. */
    ok = clipboard_get_png(out);
    if (!ok) ok = clipboard_get_dib(CF_DIBV5, out);
    if (!ok) ok = clipboard_get_dib(CF_DIB, out);

    CloseClipboard();
    if (!ok) img_free(out);
    return ok;
}

/* Borrows the clipboard (already opened and emptied by the caller). */
static bool clipboard_put_dibv5(const img_pixels *p)
{
    size_t header_bytes = sizeof(BITMAPV5HEADER);
    size_t total = header_bytes + (size_t)p->w * (size_t)p->h * 4u;
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, total);
    uint8_t *base;
    BITMAPV5HEADER *info;
    uint8_t *dst;

    if (!block) return false;
    base = (uint8_t *)GlobalLock(block);
    if (!base) {
        GlobalFree(block);
        return false;
    }

    info = (BITMAPV5HEADER *)base;
    memset(info, 0, header_bytes);
    info->bV5Size = (DWORD)header_bytes;
    info->bV5Width = (LONG)p->w;
    info->bV5Height = (LONG)p->h;   /* positive: bottom-up, like GDI expects */
    info->bV5Planes = 1;
    info->bV5BitCount = 32;
    info->bV5Compression = BI_BITFIELDS;
    info->bV5SizeImage = (DWORD)((size_t)p->w * (size_t)p->h * 4u);
    info->bV5RedMask = 0x00FF0000u;
    info->bV5GreenMask = 0x0000FF00u;
    info->bV5BlueMask = 0x000000FFu;
    info->bV5AlphaMask = 0xFF000000u;
    info->bV5CSType = LCS_sRGB;

    dst = base + header_bytes;
    for (int y = 0; y < p->h; y++) {
        const uint8_t *src = p->rgba + (size_t)(p->h - 1 - y) * (size_t)p->w * 4u;
        for (int x = 0; x < p->w; x++) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            src += 4;
            dst += 4;
        }
    }
    GlobalUnlock(block);

    if (!SetClipboardData(CF_DIBV5, block)) {
        GlobalFree(block);
        return false;
    }
    return true;
}

static bool clipboard_put_png(const img_pixels *p)
{
    UINT format = RegisterClipboardFormatW(L"PNG");
    void *png = NULL;
    size_t size = 0;
    HGLOBAL block;
    void *base;

    if (!format || !img_encode_png_memory(p, &png, &size) || size == 0) {
        free(png);
        return false;
    }
    block = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!block) {
        free(png);
        return false;
    }
    base = GlobalLock(block);
    if (!base) {
        GlobalFree(block);
        free(png);
        return false;
    }
    memcpy(base, png, size);
    GlobalUnlock(block);
    free(png);

    if (!SetClipboardData(format, block)) {
        GlobalFree(block);
        return false;
    }
    return true;
}

bool img_clipboard_set(const img_pixels *p)
{
    bool ok = false;

    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return false;
    if ((uint64_t)p->w * (uint64_t)p->h * 4u > IMG_MAX_BYTES) return false;
    if (!img_init()) return false;
    if (!clipboard_open()) return false;

    if (EmptyClipboard()) {
        /* Both flavours: CF_DIBV5 for classic GDI consumers, CF_PNG for apps
         * that keep alpha and avoid a lossy 8-bit conversion. */
        bool dib_ok = clipboard_put_dibv5(p);
        bool png_ok = clipboard_put_png(p);
        ok = dib_ok || png_ok;
    }
    CloseClipboard();
    return ok;
}
