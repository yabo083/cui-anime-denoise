/* img_priv.h -- internals shared by img.c and img_clip.c. Not part of the
 * public surface: callers outside these two files keep including img.h only.
 */
#ifndef IMG_PRIV_H
#define IMG_PRIV_H

#include "img.h"

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Refuse to allocate anything bigger than this while decoding; 2 GiB is well
 * past any anime frame we would ever denoise (a 4096x4096 mask is 64 MiB). */
#define IMG_MAX_BYTES ((uint64_t)2048u * 1024u * 1024u)

/* UTF-8 -> newly malloc'ed UTF-16 (NULL on failure). */
wchar_t *img__utf8_to_wide(const char *s);

/* Zeroed img_pixels alloc; clears *out on failure. */
bool img__pixels_alloc(int w, int h, img_pixels *out);

/* PNG byte stream: cut at IEND so strict decoders never see the trailing
 * slack that HGLOBAL-backed streams leave. Non-PNG input is returned whole. */
size_t img__png_trim_length(const uint8_t *data, size_t size);

#endif /* IMG_PRIV_H */
