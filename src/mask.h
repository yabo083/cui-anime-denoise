/* Protection mask: an 8-bit grey layer at image resolution.
 *
 *   0   = use the denoised pixel
 *   255 = use the original pixel
 *
 * Strokes are rasterised straight into the layer (round caps, replacing the
 * old value - they do not accumulate), and undo keeps a tile journal so a
 * stroke costs only the 64x64 tiles it actually touched.
 *
 * The feathering pipeline matches the web UI: the raw mask is dilated by
 * feather/2 with a disc, blurred with sigma = feather/4, then max()ed back
 * with the raw mask, so a stroke keeps ~98% of its strength at its own edge
 * and fades to 0 about `feather` pixels outside it.
 */
#ifndef MASK_H
#define MASK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct mask {
    uint8_t *px;      /* w*h, 0..255 */
    int w, h;
} mask;

typedef struct mask_hist mask_hist;

#define MASK_TILE 64
#define MASK_VALUE_BRUSH 230      /* round(90 * 2.55) for the default 90% mix */

/* ---- layer ---- */
bool mask_init(mask *m, int w, int h);
void mask_free(mask *m);
void mask_clear(mask *m);                 /* everything back to 0 */
bool mask_any(const mask *m);             /* any pixel > 0 */
bool mask_save(const mask *m, const char *utf8_path);   /* 8-bit grey PNG */
bool mask_load(mask *m, const char *utf8_path);         /* false if absent/unreadable */

/* ---- undo/redo ---- */
/* One history, one document. `budget_bytes` caps total journal memory
 * (default 192 MiB when 0); the oldest entries are dropped first. */
mask_hist *mask_hist_new(size_t budget_bytes);
void mask_hist_free(mask_hist *h);
void mask_hist_reset(mask_hist *h);
bool mask_can_undo(const mask_hist *h);
bool mask_can_redo(const mask_hist *h);
bool mask_undo(mask *m, mask_hist *h);
bool mask_redo(mask *m, mask_hist *h);

/* ---- painting (image pixel coordinates) ---- */
typedef struct mask_brush {
    float diameter;    /* px, >= 1 */
    int value;         /* 0..255 */
} mask_brush;

void mask_stroke_begin(mask *m, mask_hist *h);
void mask_stroke_dot(mask *m, mask_hist *h, float x, float y, const mask_brush *b);
void mask_stroke_segment(mask *m, mask_hist *h, float x0, float y0, float x1, float y1,
                         const mask_brush *b);
void mask_stroke_end(mask *m, mask_hist *h);

/* Polygon fill. Nonzero winding, pixel centres at +0.5, and the outline is
 * stroked one pixel wide so boundary pixels are included. `value` is written
 * verbatim (0 = subtract). */
void mask_fill_polygon(mask *m, mask_hist *h, const float *xy, int n, int value);

/* ---- feathering ---- */
/* Weights at the mask's own resolution; `w` is malloc'ed and owned by the
 * caller. Returns false only on bad input. */
bool mask_feather(const mask *m, int feather_px, uint8_t **w);

/* ---- magnetic lasso (live-wire) ---- */
typedef struct mask_edges {
    uint8_t *e;        /* 0..255 edge strength */
    int w, h;
} mask_edges;

/* Edge map from an RGBA image (the denoised result when there is one): luma
 * premultiplied by alpha, 3x3 box blur, Sobel L1 on luma and on alpha, max,
 * /4. */
bool mask_build_edges(const uint8_t *rgba, int w, int h, mask_edges *out);
void mask_edges_free(mask_edges *e);

/* Cheapest path between two points on the edge map (local Dijkstra, window
 * margin 24 px). Writes up to `cap` points as x,y pairs including both ends;
 * returns the point count, or 0 when it gives up (huge window) - then the
 * caller should just draw a straight line. */
int mask_live_wire(const mask_edges *e, int ax, int ay, int bx, int by, int *out_xy, int cap);

/* Nearest strong edge within `radius` px of (x,y); returns false when the
 * best score is too weak, i.e. the caller should keep the raw cursor point. */
bool mask_snap(const mask_edges *e, int x, int y, int radius, int *out_x, int *out_y);

#endif /* MASK_H */
