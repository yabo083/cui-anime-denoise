/* img_clip.c -- the clipboard half of the WIC image module (split out of
 * img.c so the decode/encode file stays under its line budget).
 *
 * Accepts CF_DIBV5, CF_DIB (incl. BI_BITFIELDS and top-down bitmaps) and
 * CF_PNG on get; writes PNG plus a bottom-up CF_DIBV5 on set, so both old
 * and new consumers work. Same house rules as img.c: straight RGBA8,
 * top-down, caller owns the result.
 */
#include "img.h"
#include "img_priv.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>

/* BITMAPINFOHEADER compression value that asks for a fourth (alpha) mask.
 * wingdi.h only defines it when WINVER >= 0x0601, so keep our own name. */
#define IMG_BI_ALPHABITFIELDS 6

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

    if (!img__pixels_alloc(w, h, &px)) return false;

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
    length = img__png_trim_length(base, (size_t)size);
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
