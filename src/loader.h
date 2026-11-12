/* Background image loading: one decode thread plus a bounded hand-off queue.
 *
 * Decoding a 1600x1200 PNG costs ~36 ms, which is four frames of stutter if it
 * happens inside the frame. The UI therefore only pushes paths here and pops
 * finished results once per frame; everything expensive (decode, display-size
 * downscale, thumbnail) happens on the loader thread.
 *
 * Rules:
 *   - no CUI headers and no texture work in here: textures belong to the UI
 *     thread, so a result carries plain pixels and the UI uploads them;
 *   - loader_push copies the path, the caller keeps ownership of its own copy;
 *   - loader_pop hands the three pixel buffers over: the caller img_free()s
 *     full, display and thumb (display is empty when no downscale was needed);
 *   - every entry point is safe before loader_init() and after
 *     loader_shutdown(): they return 0/false and touch nothing.
 */
#ifndef LOADER_H
#define LOADER_H

#include <stdbool.h>

#include "img.h"

#define LOADER_QUEUE_CAP   64     /* queued paths, and finished results, each */
#define LOADER_DISPLAY_MAX 2560   /* textures above this are downscaled for display */
#define LOADER_THUMB_SIDE  128    /* square tile thumbnail */

typedef struct loader_result {
    int    id;                 /* returned by loader_push */
    char   path[1024];         /* UTF-8, verbatim */
    bool   ok;
    char   error[192];         /* human readable reason when ok == false */
    img_pixels full;           /* original RGBA, straight alpha; owned by the caller */
    img_pixels display;        /* long side <= LOADER_DISPLAY_MAX; empty when full is small enough */
    img_pixels thumb;          /* LOADER_THUMB_SIDE^2 centre crop for the tile strip */
    bool   has_alpha;
    int    src_w, src_h;       /* original size, valid even after full is released */
} loader_result;

/* Starts the decode thread (also bringing img_init() up if nobody has). */
void loader_init(void);

/* Drops unprocessed paths, waits up to 3 s for the image being decoded and
 * joins the thread. */
void loader_shutdown(void);

/* Queues a path. Returns the item id (> 0), or 0 when the queue is full or the
 * loader is not running. */
int  loader_push(const char *utf8_path);

/* Takes one finished result without blocking. Returns false when there is
 * nothing ready. */
bool loader_pop(loader_result *out);

/* Items pushed but not yet finished (queued plus the one being decoded). */
int  loader_pending(void);

#endif /* LOADER_H */
