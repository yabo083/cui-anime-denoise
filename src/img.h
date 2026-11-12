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

/* Same, from a memory buffer (worker previews, clipboard PNG). */
bool img_decode_memory(const void *data, size_t size, img_pixels *out);

void img_free(img_pixels *p);

/* Encode. `quality` is 0..100 and only affects JPEG. */
bool img_encode_png(const char *utf8_path, const img_pixels *p);
bool img_encode_jpeg(const char *utf8_path, const img_pixels *p, int quality);

/* Encode into a malloc'ed buffer instead of a file (caller free()s). Used for
 * the clipboard's CF_PNG and for shipping masks to the worker without disk. */
bool img_encode_png_memory(const img_pixels *p, void **out, size_t *size);
bool img_encode_jpeg_memory(const img_pixels *p, int quality, void **out, size_t *size);

/* High-quality resize (WIC Fant / cubic) to an exact size; `out` must be
 * empty. Returns false on bad input. */
bool img_resize(const img_pixels *src, int w, int h, img_pixels *out);

/* Proportional fit inside max_side x max_side. Never upscales. */
bool img_thumbnail(const img_pixels *src, int max_side, img_pixels *out);

/* True when any pixel has alpha < 255. */
bool img_has_alpha(const img_pixels *p);

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
