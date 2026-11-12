/* Image IO: decode any Windows-imaging format, encode PNG/JPEG, resize,
 * thumbnails and the clipboard. Backed by WIC (Windows Imaging Component), so
 * WebP/AVIF/TIFF/HEIF come free with the OS codecs; stb_image is not involved.
 *
 * Rules:
 *   - every function takes UTF-8 paths and converts to UTF-16 internally;
 *   - pixels are straight (non-premultiplied) RGBA8, top-down, tightly packed;
 *   - the caller owns img_pixels.rgba and releases it with img_free.
 */
#ifndef IMG_H
#define IMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct img_pixels {
    uint8_t *rgba;   /* w*h*4, straight alpha, row 0 at the top */
    int w, h;
} img_pixels;

typedef enum img_format {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_PNG,
    IMG_FMT_JPEG,
    IMG_FMT_WEBP,
    IMG_FMT_AVIF,
    IMG_FMT_GIF,
    IMG_FMT_BMP,
    IMG_FMT_TIFF
} img_format;

/* COM + WIC factories. Idempotent; call once at startup on the main thread
 * (the worker threads never decode). */
bool img_init(void);
void img_shutdown(void);

/* Decode a file. Applies EXIF orientation. Returns false and leaves *out
 * zeroed when the file cannot be read. */
bool img_decode_file(const char *utf8_path, img_pixels *out);

/* Same, plus the source's embedded ICC profile (matching the reference
 * denoise.py open_image): `*icc`/`icc_size` get a malloc'ed copy of the
 * profile bytes when -- and only when -- the source is an RGB-family format
 * (RGB/RGBA/RGBX/palette); grey, CMYK and profile-less sources give NULL/0.
 * Free with img_free_icc (NULL-safe). 16-bit-per-channel sources are scaled
 * v/257+0.5 like PIL instead of WIC's high-byte clip; CMYK frames go through
 * IWICColorTransform with the embedded profile when one exists. */
bool img_decode_file_ex(const char *utf8_path, img_pixels *out,
                        unsigned char **icc, size_t *icc_size);

/* Same, from a memory buffer (clipboard PNG, test fixtures). */
bool img_decode_memory(const void *data, size_t size, img_pixels *out);

/* Memory variant of img_decode_file_ex (CLI stdin path). */
bool img_decode_memory_ex(const void *data, size_t size, img_pixels *out,
                          unsigned char **icc, size_t *icc_size);

void img_free(img_pixels *p);
void img_free_icc(unsigned char *icc);

/* Encode. `quality` is 0..100 and only affects JPEG. */
bool img_encode_png(const char *utf8_path, const img_pixels *p);
bool img_encode_jpeg(const char *utf8_path, const img_pixels *p, int quality);

/* PNG encode that embeds `icc` (SetColorContexts -> iCCP chunk), so the
 * colour profile survives the round trip like the reference's save_png.
 * NULL icc / 0 size behaves exactly like img_encode_png. */
bool img_encode_png_icc(const char *utf8_path, const img_pixels *p,
                        const unsigned char *icc, size_t icc_size);
bool img_encode_png_memory_icc(const img_pixels *p, const unsigned char *icc,
                               size_t icc_size, void **out, size_t *size);

/* Encode into a malloc'ed buffer instead of a file (caller free()s). Used for
 * the clipboard's CF_PNG. */
bool img_encode_png_memory(const img_pixels *p, void **out, size_t *size);
bool img_encode_jpeg_memory(const img_pixels *p, int quality, void **out, size_t *size);

/* High-quality resize (WIC Fant / cubic) to an exact size; `out` must be
 * empty. Returns false on bad input. */
bool img_resize(const img_pixels *src, int w, int h, img_pixels *out);

/* Proportional fit inside max_side x max_side. Never upscales. */
bool img_thumbnail(const img_pixels *src, int max_side, img_pixels *out);

/* True when any pixel has alpha < 255. */
bool img_has_alpha(const img_pixels *p);

/* Blends a denoised RGB result back with the original RGBA under an 8-bit
 * weight map (0 = denoised, 255 = original), the exact formula the reference
 * worker used:
 *
 *   out = (den * 255 + (src - den) * weight + 127) / 255   per channel
 *
 * Alpha passes through from `orig`. `den_rgb` is tightly packed RGB8 (the
 * engine's eng_image buffer), `weight` is w*h bytes (mask_feather's output).
 * `orig` must be w x h. Allocates `out`; free with img_free. */
bool img_composite_masked(img_pixels *out, const img_pixels *orig,
                          const uint8_t *den_rgb, const uint8_t *weight,
                          int w, int h);

/* Clipboard. img_clipboard_get accepts CF_DIBV5, CF_DIB (incl. BI_BITFIELDS
 * and top-down bitmaps) and CF_PNG; img_clipboard_set writes PNG and a
 * bottom-up CF_DIBV5 so both old and new consumers work. */
bool img_clipboard_has_image(void);
bool img_clipboard_get(img_pixels *out);
bool img_clipboard_set(const img_pixels *p);

/* Extension whitelist used by the drop zone, the file dialog and argv. */
bool img_supported_path(const char *utf8_path);

img_format img_format_of(const char *utf8_path);

#endif /* IMG_H */
