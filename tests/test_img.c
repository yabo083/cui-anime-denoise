/* Driver for src/img.c + src/img_clip.c (WIC image IO). Build and run from
 * the repo root:
 *
 *   pwsh -NoProfile -Command ". .\toolchain.ps1; cl /nologo /utf-8 /W4
 *       /Fe:tmp\test_img.exe tests\test_img.c src\img.c src\img_clip.c
 *       /link ole32.lib oleaut32.lib windowscodecs.lib shlwapi.lib user32.lib"
 *   tmp\test_img.exe
 *
 * Checks: WebP decode and size, thumbnail geometry, byte-exact PNG file round
 * trip, PNG memory round trip, alpha probing, clipboard round trip (SKIP when
 * another process owns the clipboard) and graceful failure on a text file.
 */
#include "../src/img.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define SRC_IMAGE "E:\\AI\\gpt-anime-denoise\\examples\\furina_before_after.webp"
#define TMP_PNG "E:\\Codes\\AI\\lab\\cui-anime-denoise\\tmp\\t.png"
#define TMP_JPG "E:\\Codes\\AI\\lab\\cui-anime-denoise\\tmp\\t.jpg"
#define NOT_AN_IMAGE "E:\\Codes\\AI\\lab\\cui-anime-denoise\\README.md"
#define SRC_IMAGE_W 2096
#define SRC_IMAGE_H 1586

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static unsigned long long now_ms(void) { return GetTickCount64(); }

/* Index of the first differing byte pair, or -1 when the images are identical
 * in size and pixels. */
static long long first_diff(const img_pixels *a, const img_pixels *b)
{
    if (a->w != b->w || a->h != b->h) return -2;
    size_t bytes = (size_t)a->w * (size_t)a->h * 4u;
    return memcmp(a->rgba, b->rgba, bytes) == 0 ? -1 : (long long)0;
}

/* PNG data must end exactly on its IEND chunk (length 0, "IEND", CRC). */
static int ends_with_iend(const void *png, size_t size)
{
    const unsigned char *b = (const unsigned char *)png;
    static const unsigned char crc[4] = { 0xAE, 0x42, 0x60, 0x82 };
    if (size < 12) return 0;
    return b[size - 12] == 0 && b[size - 11] == 0 && b[size - 10] == 0 && b[size - 9] == 0 &&
           memcmp(b + size - 8, "IEND", 4) == 0 && memcmp(b + size - 4, crc, 4) == 0;
}

static void report(const char *tag, const img_pixels *p)
{
    printf("     %s: %dx%d\n", tag, p ? p->w : 0, p ? p->h : 0);
}

/* ------------------------------------------------------------------------- */

static void test_decode(void)
{
    img_pixels image = { NULL, 0, 0 };
    unsigned long long t0 = now_ms();
    bool ok = img_decode_file(SRC_IMAGE, &image);
    unsigned long long dt = now_ms() - t0;

    printf("decode %s\n", SRC_IMAGE);
    check(ok, "img_decode_file(webp) succeeded");
    if (!ok) return;
    printf("     %dx%d in %llu ms, %zu bytes of RGBA\n", image.w, image.h, dt,
           (size_t)image.w * (size_t)image.h * 4u);
    check(image.w == SRC_IMAGE_W && image.h == SRC_IMAGE_H, "decoded size is 2096x1586");
    check(image.rgba != NULL, "pixel buffer is present");

    /* A photo with no transparency: every alpha byte must be 255. */
    check(!img_has_alpha(&image), "img_has_alpha() is false for an opaque image");

    /* Thumbnail: long side capped at 256, aspect ratio preserved. */
    img_pixels thumb = { NULL, 0, 0 };
    t0 = now_ms();
    bool thumb_ok = img_thumbnail(&image, 256, &thumb);
    dt = now_ms() - t0;
    check(thumb_ok, "img_thumbnail(256) succeeded");
    if (thumb_ok) {
        printf("     thumbnail %dx%d in %llu ms\n", thumb.w, thumb.h, dt);
        int long_side = thumb.w > thumb.h ? thumb.w : thumb.h;
        check(long_side == 256, "thumbnail long side is 256");
        /* 1586 * 256 / 2096 = 193.75 -> 194 */
        check(thumb.w == 256 && thumb.h == 194, "thumbnail is 256x194");

        double want = (double)SRC_IMAGE_W / (double)SRC_IMAGE_H;
        double got = (double)thumb.w / (double)thumb.h;
        check(got > want - 0.02 && got < want + 0.02, "thumbnail aspect ratio preserved");

        /* No upscale: a small max_side returns the original pixels untouched. */
        img_pixels small = { NULL, 0, 0 };
        bool small_ok = img_thumbnail(&thumb, 4096, &small);
        check(small_ok, "img_thumbnail never upscales");
        if (small_ok) {
            check(small.w == thumb.w && small.h == thumb.h &&
                      memcmp(small.rgba, thumb.rgba, (size_t)thumb.w * (size_t)thumb.h * 4u) == 0,
                  "no-upscale copy is pixel identical");
            img_free(&small);
        }
    }
    img_free(&thumb);

    /* PNG file round trip: encode, decode, compare every RGBA byte. */
    t0 = now_ms();
    bool enc_ok = img_encode_png(TMP_PNG, &image);
    dt = now_ms() - t0;
    check(enc_ok, "img_encode_png(file) succeeded");
    if (enc_ok) {
        printf("     png written in %llu ms (%s)\n", dt, TMP_PNG);
        img_pixels back = { NULL, 0, 0 };
        check(img_decode_file(TMP_PNG, &back), "img_decode_file(png) succeeded");
        if (back.rgba) {
            report("decoded png", &back);
            check(first_diff(&image, &back) == -1, "PNG file round trip is RGBA exact");
            check(!img_has_alpha(&back), "round-tripped PNG still reports no alpha");
            img_free(&back);
        }
    }

    /* PNG memory round trip (the path the clipboard and the mask upload use). */
    void *png = NULL;
    size_t png_size = 0;
    t0 = now_ms();
    bool mem_ok = img_encode_png_memory(&image, &png, &png_size);
    dt = now_ms() - t0;
    check(mem_ok && png && png_size > 0, "img_encode_png_memory produced bytes");
    if (mem_ok && png) {
        printf("     png in memory: %zu bytes in %llu ms\n", png_size, dt);
        check(ends_with_iend(png, png_size), "memory PNG ends exactly at IEND");
        img_pixels back = { NULL, 0, 0 };
        check(img_decode_memory(png, png_size, &back), "img_decode_memory(png) succeeded");
        if (back.rgba) {
            report("decoded memory png", &back);
            check(first_diff(&image, &back) == -1, "PNG memory round trip is RGBA exact");
            img_free(&back);
        }
        free(png);
    }

    /* JPEG encode: no alpha channel, so this only has to succeed and decode. */
    check(img_encode_jpeg(TMP_JPG, &image, 90), "img_encode_jpeg(file) succeeded");
    img_pixels jpg = { NULL, 0, 0 };
    if (img_decode_file(TMP_JPG, &jpg)) {
        report("decoded jpg", &jpg);
        check(jpg.w == image.w && jpg.h == image.h, "JPEG keeps the dimensions");
        img_free(&jpg);
    } else {
        check(0, "img_decode_file(jpg) succeeded");
    }

    /* JPEG to memory (the worker upload path for lossy previews). */
    jpg.rgba = NULL;
    jpg.w = jpg.h = 0;
    void *jpeg_bytes = NULL;
    size_t jpeg_size = 0;
    bool jpeg_mem_ok = img_encode_jpeg_memory(&image, 80, &jpeg_bytes, &jpeg_size);
    check(jpeg_mem_ok && jpeg_bytes && jpeg_size > 0, "img_encode_jpeg_memory produced bytes");
    if (jpeg_mem_ok && jpeg_bytes) {
        printf("     jpeg in memory: %zu bytes\n", jpeg_size);
        check(img_decode_memory(jpeg_bytes, jpeg_size, &jpg), "img_decode_memory(jpeg) succeeded");
        if (jpg.rgba) {
            check(jpg.w == image.w && jpg.h == image.h, "memory JPEG keeps the dimensions");
            img_free(&jpg);
        }
        free(jpeg_bytes);
    }

    img_free(&image);
}

static void test_clipboard(void)
{
    printf("clipboard round trip\n");
    if (!OpenClipboard(NULL)) {
        printf("SKIP clipboard is held by another process\n");
        return;
    }
    CloseClipboard();

    img_pixels src = { NULL, 0, 0 };
    if (!img_decode_file(SRC_IMAGE, &src)) {
        check(0, "clipboard source image decoded");
        return;
    }
    img_pixels small = { NULL, 0, 0 };
    if (!img_thumbnail(&src, 256, &small)) {
        check(0, "clipboard source thumbnail built");
        img_free(&src);
        return;
    }
    img_free(&src);

    check(img_clipboard_set(&small), "img_clipboard_set succeeded");
    check(img_clipboard_has_image(), "img_clipboard_has_image is true");

    img_pixels back = { NULL, 0, 0 };
    bool got = img_clipboard_get(&back);
    check(got, "img_clipboard_get succeeded");
    if (got) {
        report("clipboard image", &back);
        check(first_diff(&small, &back) == -1, "clipboard round trip is RGBA exact");
        img_free(&back);
    }
    img_free(&small);
}

/* --- raw DIB variants (a paste source that is not our own PNG) ----------- */

static bool put_clipboard_block(UINT format, const void *data, size_t size)
{
    bool ok = false;
    if (!OpenClipboard(NULL)) return false;
    if (EmptyClipboard()) {
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, size);
        if (block) {
            void *dst = GlobalLock(block);
            if (dst) {
                memcpy(dst, data, size);
                GlobalUnlock(block);
                if (SetClipboardData(format, block)) ok = true;
                else GlobalFree(block);
            } else {
                GlobalFree(block);
            }
        }
    }
    CloseClipboard();
    return ok;
}

static void fill_probe_image(img_pixels *p, int w, int h, bool opaque)
{
    p->rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    p->w = w;
    p->h = h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *d = p->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            d[0] = (uint8_t)(x * 31 + 3);
            d[1] = (uint8_t)(y * 61 + 7);
            d[2] = (uint8_t)(x * 11 + y * 17 + 23);
            d[3] = opaque ? 255u : (uint8_t)(40 + x * 20 + y * 7);
        }
    }
}

static void test_clipboard_dib(void)
{
    const int W = 5, H = 3;
    img_pixels plain, alpha;
    fill_probe_image(&plain, W, H, true);
    fill_probe_image(&alpha, W, H, false);

    /* 1. CF_DIBV5, BI_BITFIELDS, 32bpp, top-down (negative height). */
    size_t header = sizeof(BITMAPV5HEADER);
    size_t bytes = header + (size_t)W * (size_t)H * 4u;
    uint8_t *dib = (uint8_t *)calloc(bytes, 1);
    BITMAPV5HEADER *v5 = (BITMAPV5HEADER *)dib;
    v5->bV5Size = (DWORD)header;
    v5->bV5Width = W;
    v5->bV5Height = -H;   /* top-down */
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5RedMask = 0x00FF0000u;
    v5->bV5GreenMask = 0x0000FF00u;
    v5->bV5BlueMask = 0x000000FFu;
    v5->bV5AlphaMask = 0xFF000000u;
    v5->bV5CSType = LCS_sRGB;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *s = alpha.rgba + ((size_t)y * W + x) * 4u;
            uint8_t *d = dib + header + ((size_t)y * W + x) * 4u;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
    check(put_clipboard_block(CF_DIBV5, dib, bytes), "put a top-down CF_DIBV5 on the clipboard");
    img_pixels got = { NULL, 0, 0 };
    bool ok = img_clipboard_get(&got);
    check(ok, "img_clipboard_get(CF_DIBV5) succeeded");
    if (ok) {
        report("dibv5", &got);
        check(first_diff(&alpha, &got) == -1, "CF_DIBV5 top-down + alpha masks decode exactly");
        img_free(&got);
    }
    free(dib);

    /* 2. CF_DIB, BITMAPINFOHEADER, 24bpp, bottom-up (BI_RGB). */
    size_t stride24 = (((size_t)W * 24u + 31u) / 32u) * 4u;
    bytes = sizeof(BITMAPINFOHEADER) + stride24 * (size_t)H;
    dib = (uint8_t *)calloc(bytes, 1);
    BITMAPINFOHEADER *ih = (BITMAPINFOHEADER *)dib;
    ih->biSize = sizeof(BITMAPINFOHEADER);
    ih->biWidth = W;
    ih->biHeight = H;   /* bottom-up */
    ih->biPlanes = 1;
    ih->biBitCount = 24;
    ih->biCompression = BI_RGB;
    for (int y = 0; y < H; y++) {
        uint8_t *row = dib + sizeof(BITMAPINFOHEADER) + stride24 * (size_t)(H - 1 - y);
        for (int x = 0; x < W; x++) {
            const uint8_t *s = plain.rgba + ((size_t)y * W + x) * 4u;
            row[x * 3 + 0] = s[2];
            row[x * 3 + 1] = s[1];
            row[x * 3 + 2] = s[0];
        }
    }
    check(put_clipboard_block(CF_DIB, dib, bytes), "put a 24bpp bottom-up CF_DIB on the clipboard");
    got.rgba = NULL;
    got.w = got.h = 0;
    ok = img_clipboard_get(&got);
    check(ok, "img_clipboard_get(CF_DIB 24bpp) succeeded");
    if (ok) {
        report("dib24", &got);
        check(first_diff(&plain, &got) == -1, "24bpp bottom-up CF_DIB decodes exactly");
        check(got.rgba[3] == 255, "24bpp CF_DIB is opaque");
        img_free(&got);
    }
    free(dib);

    /* 3. CF_DIB, 32bpp BI_RGB, bottom-up: byte 3 is padding, so alpha=255. */
    bytes = sizeof(BITMAPINFOHEADER) + (size_t)W * (size_t)H * 4u;
    dib = (uint8_t *)calloc(bytes, 1);
    ih = (BITMAPINFOHEADER *)dib;
    ih->biSize = sizeof(BITMAPINFOHEADER);
    ih->biWidth = W;
    ih->biHeight = H;
    ih->biPlanes = 1;
    ih->biBitCount = 32;
    ih->biCompression = BI_RGB;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *s = alpha.rgba + ((size_t)y * W + x) * 4u;
            uint8_t *d = dib + sizeof(BITMAPINFOHEADER) + ((size_t)(H - 1 - y) * W + x) * 4u;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 0x00;   /* padding, must not become transparent */
        }
    }
    check(put_clipboard_block(CF_DIB, dib, bytes), "put a 32bpp BI_RGB CF_DIB on the clipboard");
    got.rgba = NULL;
    got.w = got.h = 0;
    ok = img_clipboard_get(&got);
    check(ok, "img_clipboard_get(CF_DIB 32bpp BI_RGB) succeeded");
    if (ok) {
        report("dib32", &got);
        check(first_diff(&plain, &got) == -1, "32bpp BI_RGB CF_DIB colors decode exactly");
        check(got.rgba[3] == 255, "32bpp BI_RGB padding is not used as alpha");
        img_free(&got);
    }
    free(dib);

    img_free(&plain);
    img_free(&alpha);
}

static void test_failures(void)
{
    printf("robustness\n");
    img_pixels px = { (uint8_t *)1, 7, 7 };
    check(!img_decode_file(NOT_AN_IMAGE, &px), "text file is rejected");
    check(px.rgba == NULL && px.w == 0 && px.h == 0, "failed decode zeroes the output");

    check(!img_decode_file("E:\\Codes\\AI\\lab\\cui-anime-denoise\\tmp\\does-not-exist.png", &px),
          "missing file is rejected");
    check(!img_decode_memory("not an image", 12, &px), "garbage memory is rejected");
    check(!img_encode_png_memory(NULL, NULL, NULL), "null arguments are rejected");

    check(img_supported_path(SRC_IMAGE), "webp is a supported path");
    check(img_supported_path("C:\\x\\UPPER.JPG"), "extension matching ignores case");
    check(img_supported_path("C:\\x\\photo.heic"), "heic is a supported path");
    check(!img_supported_path(NOT_AN_IMAGE), "README.md is not a supported path");
    check(img_format_of("a.webp") == IMG_FMT_WEBP, "format of .webp is IMG_FMT_WEBP");
    check(img_format_of("a.JPEG") == IMG_FMT_JPEG, "format of .JPEG is IMG_FMT_JPEG");
    check(img_format_of("a.tif") == IMG_FMT_TIFF, "format of .tif is IMG_FMT_TIFF");
    check(img_format_of("a.bin") == IMG_FMT_UNKNOWN, "format of .bin is IMG_FMT_UNKNOWN");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("img_init: %s\n", img_init() ? "ok" : "FAILED");
    if (!img_init()) {
        printf("\nRESULT: 1 FAILURE(S)\n");
        return 1;
    }

    test_decode();
    test_clipboard();
    test_clipboard_dib();
    test_failures();

    img_shutdown();

    if (failures) printf("\nRESULT: %d FAILURE(S)\n", failures);
    else printf("\nRESULT: PASS\n");
    return failures ? 1 : 0;
}
