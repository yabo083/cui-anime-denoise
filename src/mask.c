/* Protection mask: brush/lasso rasteriser, feathering, tile-journal undo and
 * the magnetic-lasso helpers (edge map, live wire, snap).
 *
 * Conventions from mask.h:
 *   - 0 = use the denoised pixel, 255 = keep the original pixel;
 *   - pixel (x, y) covers [x, x+1) x [y, y+1); its centre is (x+0.5, y+0.5);
 *   - a stroke overwrites, it never accumulates (erase = value 0).
 *
 * Edges are hard (no antialiasing). The web UI paints an exact grey on a
 * canvas and only the canvas' own 1 px smoothing softens the rim, so a
 * membership test at the pixel centre reproduces the painted shape without
 * inventing a second grey ramp that the feather pass would then widen. The
 * 1 px lasso outline is therefore stroked *inside* the polygon: it can close
 * gaps the scanline rule left, but it never grows the shape the user drew.
 */
#include "img.h"
#include "mask.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <emmintrin.h>
#define MASK_USE_SSE2 1
#endif

#define TILE_PX (MASK_TILE * MASK_TILE)
#define INF_F 1.0e30f

/* ---------------------------------------------------------------- history */

typedef struct hist_tile {
    struct hist_tile *next;
    int tx, ty;
    uint8_t px[TILE_PX];          /* pre-stroke content, tiles past the edge = 0 */
} hist_tile;

typedef struct hist_item {
    struct hist_item *prev, *next;
    hist_tile *tiles;
    int ntiles;
    size_t bytes;
} hist_item;

typedef struct journal {
    mask *m;                      /* document being recorded; NULL = idle */
    int depth;                    /* begin/end nesting (fill uses its own pair) */
    hist_item *rec;               /* tiles captured so far for this journal */
    uint8_t *seen;                /* one bit per tile: already copied? */
    int tw, th;
} journal;

struct mask_hist {
    size_t budget;
    size_t bytes;                 /* live payload of undo + redo entries */
    hist_item *u_head, *u_tail;   /* oldest .. newest undoable */
    hist_item *redo;              /* newest first */
    journal jr;
};

/* Payload accounting: the tile pixels plus the record itself. Used both for
 * the budget and by the tests, hence the extra accessor at the bottom. */
static size_t item_bytes(const hist_item *it) {
    return sizeof(hist_item) + (size_t)it->ntiles * sizeof(hist_tile);
}

static void item_free(hist_item *it) {
    if (!it) return;
    hist_tile *t = it->tiles;
    while (t) {
        hist_tile *n = t->next;
        free(t);
        t = n;
    }
    free(it);
}

static void redo_free(mask_hist *h) {
    hist_item *it = h->redo;
    while (it) {
        hist_item *n = it->next;
        h->bytes -= it->bytes;
        item_free(it);
        it = n;
    }
    h->redo = NULL;
}

static void undo_free(mask_hist *h) {
    hist_item *it = h->u_head;
    while (it) {
        hist_item *n = it->next;
        h->bytes -= it->bytes;
        item_free(it);
        it = n;
    }
    h->u_head = h->u_tail = NULL;
}

static void push_undo(mask_hist *h, hist_item *it) {
    it->prev = h->u_tail;
    it->next = NULL;
    if (h->u_tail) h->u_tail->next = it; else h->u_head = it;
    h->u_tail = it;
}

/* Drop the oldest entries. The newest entry always survives so a single huge
 * stroke is still one undo step even when it alone exceeds the budget. */
static void hist_trim(mask_hist *h) {
    while (h->bytes > h->budget && h->u_head && h->u_head != h->u_tail) {
        hist_item *it = h->u_head;
        h->u_head = it->next;
        if (h->u_head) h->u_head->prev = NULL; else h->u_tail = NULL;
        h->bytes -= it->bytes;
        item_free(it);
    }
}

/* ---------------------------------------------------------------- journal */

/* mask_clear() takes no mask_hist, yet the UI wraps it in
 * mask_stroke_begin/end so that "clear mask" is one undo step (the web UI
 * pushes undo before clearing too). mask.h says one history, one document, so
 * remembering the open journal here is enough to journal that call. */
static mask_hist *open_journal;

/* Copies the current content of one 64x64 tile, once per open journal. Must
 * run before the tile is written for the first time in this stroke. */
static void tile_capture(mask *m, mask_hist *h, int tx, int ty) {
    journal *j = &h->jr;
    if (!j->seen || !j->rec || j->m != m) return;
    size_t idx = (size_t)ty * (size_t)j->tw + (size_t)tx;
    uint8_t bit = (uint8_t)(1u << (idx & 7u));
    if (j->seen[idx >> 3] & bit) return;
    j->seen[idx >> 3] |= bit;

    hist_tile *t = malloc(sizeof *t);
    if (!t) return;
    t->tx = tx;
    t->ty = ty;
    memset(t->px, 0, sizeof t->px);

    int x0 = tx * MASK_TILE, y0 = ty * MASK_TILE;
    int cw = m->w - x0, ch = m->h - y0;
    if (cw > MASK_TILE) cw = MASK_TILE;
    if (ch > MASK_TILE) ch = MASK_TILE;
    for (int r = 0; r < ch; r++)
        memcpy(t->px + (size_t)r * MASK_TILE,
               m->px + (size_t)(y0 + r) * (size_t)m->w + (size_t)x0,
               (size_t)cw);

    t->next = j->rec->tiles;
    j->rec->tiles = t;
    j->rec->ntiles++;
}

static void tile_apply(mask *m, const hist_tile *t) {
    int x0 = t->tx * MASK_TILE, y0 = t->ty * MASK_TILE;
    int cw = m->w - x0, ch = m->h - y0;
    if (cw > MASK_TILE) cw = MASK_TILE;
    if (ch > MASK_TILE) ch = MASK_TILE;
    for (int r = 0; r < ch; r++)
        memcpy(m->px + (size_t)(y0 + r) * (size_t)m->w + (size_t)x0,
               t->px + (size_t)r * MASK_TILE,
               (size_t)cw);
}

static bool jr_begin(mask_hist *h, mask *m) {
    journal *j = &h->jr;
    int tw = (m->w + MASK_TILE - 1) / MASK_TILE;
    int th = (m->h + MASK_TILE - 1) / MASK_TILE;
    if (tw != j->tw || th != j->th) {
        uint8_t *seen = malloc((size_t)((tw * th + 7) / 8));
        if (!seen) return false;
        free(j->seen);
        j->seen = seen;
        j->tw = tw;
        j->th = th;
    }
    memset(j->seen, 0, (size_t)((j->tw * j->th + 7) / 8));
    j->rec = calloc(1, sizeof *j->rec);
    if (!j->rec) return false;
    j->m = m;
    open_journal = h;
    return true;
}

static void jr_end(mask_hist *h) {
    journal *j = &h->jr;
    hist_item *it = j->rec;
    j->rec = NULL;
    j->m = NULL;
    open_journal = NULL;
    if (!it) return;
    if (!it->tiles) {                 /* nothing changed: no undo entry */
        free(it);
        return;
    }
    it->bytes = item_bytes(it);
    redo_free(h);                     /* a new edit invalidates the redo path */
    push_undo(h, it);
    h->bytes += it->bytes;
    hist_trim(h);
}

/* ---------------------------------------------------------------- layer */

bool mask_init(mask *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return false;
    m->px = calloc((size_t)w * (size_t)h, 1);
    if (!m->px) return false;
    m->w = w;
    m->h = h;
    return true;
}

void mask_free(mask *m) {
    if (!m) return;
    free(m->px);
    m->px = NULL;
    m->w = m->h = 0;
}

void mask_clear(mask *m) {
    if (!m || !m->px) return;
    mask_hist *h = open_journal;
    if (h && h->jr.m == m && m->w > 0 && m->h > 0) {
        int tw = (m->w + MASK_TILE - 1) / MASK_TILE;
        int th = (m->h + MASK_TILE - 1) / MASK_TILE;
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) tile_capture(m, h, tx, ty);
    }
    memset(m->px, 0, (size_t)m->w * (size_t)m->h);
}

/* Every protected pixel becomes unprotected and back, preserving the
 * per-pixel strength the strokes carried (the web UI's invert keeps the
 * value, it does not flatten to 255). Journals the whole layer like
 * mask_clear() so the invert is one undo step. */
void mask_invert(mask *m) {
    if (!m || !m->px) return;
    mask_hist *h = open_journal;
    if (h && h->jr.m == m && m->w > 0 && m->h > 0) {
        int tw = (m->w + MASK_TILE - 1) / MASK_TILE;
        int th = (m->h + MASK_TILE - 1) / MASK_TILE;
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) tile_capture(m, h, tx, ty);
    }
    size_t n = (size_t)m->w * (size_t)m->h;
    for (size_t i = 0; i < n; i++) m->px[i] = (uint8_t)(255 - m->px[i]);
}

bool mask_any(const mask *m) {
    if (!m || !m->px) return false;
    size_t n = (size_t)m->w * (size_t)m->h;
    const uint8_t *p = m->px;
    size_t i = 0;
    /* word-wise first, then the tail: masks are mostly empty */
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, sizeof v);
        if (v) return true;
    }
    for (; i < n; i++)
        if (p[i]) return true;
    return false;
}

/* ---------------------------------------------------------------- save/load */

bool mask_save(const mask *m, const char *utf8_path) {
    if (!m || !m->px || !utf8_path || m->w <= 0 || m->h <= 0) return false;
    img_pixels p;
    p.rgba = malloc((size_t)m->w * (size_t)m->h * 4u);
    if (!p.rgba) return false;
    p.w = m->w;
    p.h = m->h;
    size_t n = (size_t)m->w * (size_t)m->h;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = m->px[i];
        p.rgba[i * 4 + 0] = v;
        p.rgba[i * 4 + 1] = v;
        p.rgba[i * 4 + 2] = v;
        p.rgba[i * 4 + 3] = 255;
    }
    bool ok = img_encode_png(utf8_path, &p);
    free(p.rgba);
    return ok;
}

bool mask_load(mask *m, const char *utf8_path) {
    if (!m || !m->px || m->w <= 0 || m->h <= 0 || !utf8_path) return false;
    img_pixels p;
    memset(&p, 0, sizeof p);
    if (!img_decode_file(utf8_path, &p) || !p.rgba || p.w <= 0 || p.h <= 0) {
        img_free(&p);
        return false;
    }
    size_t n = (size_t)m->w * (size_t)m->h;
    if (p.w == m->w && p.h == m->h) {
        for (size_t i = 0; i < n; i++) m->px[i] = p.rgba[i * 4];
    } else {
        /* A mask saved before the image was resized: nearest neighbour keeps
         * the stroke edges where the user put them. */
        for (int y = 0; y < m->h; y++) {
            int sy = (int)((long long)y * p.h / m->h);
            for (int x = 0; x < m->w; x++) {
                int sx = (int)((long long)x * p.w / m->w);
                m->px[(size_t)y * (size_t)m->w + (size_t)x] =
                    p.rgba[((size_t)sy * (size_t)p.w + (size_t)sx) * 4];
            }
        }
    }
    img_free(&p);
    return true;
}

/* ---------------------------------------------------------------- undo/redo */

mask_hist *mask_hist_new(size_t budget_bytes) {
    mask_hist *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->budget = budget_bytes ? budget_bytes : (size_t)192 * 1024 * 1024;
    return h;
}

void mask_hist_free(mask_hist *h) {
    if (!h) return;
    if (open_journal == h) open_journal = NULL;
    undo_free(h);
    redo_free(h);
    free(h->jr.seen);
    item_free(h->jr.rec);
    free(h);
}

void mask_hist_reset(mask_hist *h) {
    if (!h) return;
    if (open_journal == h) open_journal = NULL;
    undo_free(h);
    redo_free(h);
    item_free(h->jr.rec);
    h->jr.rec = NULL;
    h->jr.m = NULL;
    h->jr.depth = 0;
    h->bytes = 0;
}

bool mask_can_undo(const mask_hist *h) { return h && h->u_tail != NULL; }
bool mask_can_redo(const mask_hist *h) { return h && h->redo != NULL; }

/* Same tiles, current content: what a redo has to put back. */
static hist_item *item_current(mask *m, const hist_item *src) {
    hist_item *it = calloc(1, sizeof *it);
    if (!it) return NULL;
    for (const hist_tile *s = src->tiles; s; s = s->next) {
        hist_tile *t = malloc(sizeof *t);
        if (!t) break;
        t->tx = s->tx;
        t->ty = s->ty;
        memset(t->px, 0, sizeof t->px);
        int x0 = s->tx * MASK_TILE, y0 = s->ty * MASK_TILE;
        int cw = m->w - x0, ch = m->h - y0;
        if (cw > MASK_TILE) cw = MASK_TILE;
        if (ch > MASK_TILE) ch = MASK_TILE;
        for (int r = 0; r < ch; r++)
            memcpy(t->px + (size_t)r * MASK_TILE,
                   m->px + (size_t)(y0 + r) * (size_t)m->w + (size_t)x0,
                   (size_t)cw);
        t->next = it->tiles;
        it->tiles = t;
        it->ntiles++;
    }
    it->bytes = item_bytes(it);
    return it;
}

bool mask_undo(mask *m, mask_hist *h) {
    if (!m || !m->px || !h || h->jr.m) return false;   /* not mid-stroke */
    hist_item *it = h->u_tail;
    if (!it) return false;

    h->u_tail = it->prev;
    if (h->u_tail) h->u_tail->next = NULL; else h->u_head = NULL;
    h->bytes -= it->bytes;

    hist_item *back = item_current(m, it);
    if (back && back->tiles) {
        back->next = h->redo;
        if (h->redo) h->redo->prev = back;
        h->redo = back;
        h->bytes += back->bytes;
    } else {
        item_free(back);
    }
    for (const hist_tile *t = it->tiles; t; t = t->next) tile_apply(m, t);
    item_free(it);
    hist_trim(h);
    return true;
}

bool mask_redo(mask *m, mask_hist *h) {
    if (!m || !m->px || !h || h->jr.m) return false;
    hist_item *it = h->redo;
    if (!it) return false;

    h->redo = it->next;
    if (h->redo) h->redo->prev = NULL;
    h->bytes -= it->bytes;

    hist_item *fwd = item_current(m, it);
    if (fwd && fwd->tiles) {
        push_undo(h, fwd);
        h->bytes += fwd->bytes;
    } else {
        item_free(fwd);
    }
    for (const hist_tile *t = it->tiles; t; t = t->next) tile_apply(m, t);
    item_free(it);
    hist_trim(h);
    return true;
}

/* --------------------------------------------------------------- painting */

/* Writes `v` over one clipped row run, journalling every tile it touches
 * before the first byte lands in it. */
static void paint_run(mask *m, mask_hist *h, int y, int xa, int xb, uint8_t v) {
    if (xa < 0) xa = 0;
    if (xb > m->w - 1) xb = m->w - 1;
    if (xa > xb) return;
    if (h) {
        int tx0 = xa / MASK_TILE, tx1 = xb / MASK_TILE, ty = y / MASK_TILE;
        for (int tx = tx0; tx <= tx1; tx++) tile_capture(m, h, tx, ty);
    }
    memset(m->px + (size_t)y * (size_t)m->w + (size_t)xa, v, (size_t)(xb - xa + 1));
}

/* One linear condition c * px in [c_lo, c_hi] narrowed into [lo, hi]. */
static bool clip_linear(double c, double c_lo, double c_hi, double *lo, double *hi) {
    double a, b;
    if (c > 1e-12) {
        a = c_lo / c;
        b = c_hi / c;
    } else if (c < -1e-12) {
        a = c_hi / c;
        b = c_lo / c;
    } else {
        return c_lo <= 0.0 && 0.0 <= c_hi;
    }
    if (a > *lo) *lo = a;
    if (b < *hi) *hi = b;
    return true;
}

/* x range of the capsule (segment, radius r) on the line cy. The capsule is
 * the union of the two end discs and the slab body, all convex, so their
 * intervals merge into one - that is what makes a row scan exact in O(1). */
static bool span_row(double ax, double ay, double bx, double by, double r, double cy,
                     double *lo, double *hi) {
    double l = 1e30, h = -1e30;     /* min / max over the three convex parts */
    bool any = false;

    double d = r * r - (cy - ay) * (cy - ay);
    if (d >= 0.0) {
        double s = sqrt(d);
        if (ax - s < l) l = ax - s;
        if (ax + s > h) h = ax + s;
        any = true;
    }
    d = r * r - (cy - by) * (cy - by);
    if (d >= 0.0) {
        double s = sqrt(d);
        if (bx - s < l) l = bx - s;
        if (bx + s > h) h = bx + s;
        any = true;
    }

    double dx = bx - ax, dy = by - ay;
    double len = sqrt(dx * dx + dy * dy);
    if (len > 1e-6) {
        double ux = dx / len, uy = dy / len;
        double bl = -1e30, bh = 1e30;
        /* (p - a) . u in [0, len] */
        bool ok = clip_linear(ux, ax * ux - (cy - ay) * uy,
                              ax * ux + len - (cy - ay) * uy, &bl, &bh);
        /* |(p - a) . n| <= r, n = (-uy, ux) */
        if (ok)
            ok = clip_linear(-uy, ax * -uy - r - (cy - ay) * ux,
                             ax * -uy + r - (cy - ay) * ux, &bl, &bh);
        if (ok && bl <= bh) {
            if (bl < l) l = bl;
            if (bh > h) h = bh;
            any = true;
        }
    }
    if (!any || l > h) return false;
    *lo = l;
    *hi = h;
    return true;
}

/* First pixel whose centre is > x, and last pixel whose centre is < x. */
static int first_inside(double x) { return (int)floor(x - 0.5 + 1e-9) + 1; }
static int last_inside(double x) { return (int)ceil(x - 0.5 - 1e-9) - 1; }

static void stroke_capsule(mask *m, mask_hist *h, double ax, double ay, double bx,
                           double by, double r, uint8_t v) {
    if (r < 0.5) r = 0.5;
    double ty0 = (ay < by ? ay : by) - r, ty1 = (ay < by ? by : ay) + r;
    int y0 = (int)floor(ty0), y1 = (int)ceil(ty1);
    if (y0 < 0) y0 = 0;
    if (y1 > m->h - 1) y1 = m->h - 1;
    for (int y = y0; y <= y1; y++) {
        double lo, hi;
        if (!span_row(ax, ay, bx, by, r, y + 0.5, &lo, &hi)) continue;
        int xa = first_inside(lo), xb = last_inside(hi);
        if (xa < 0) xa = 0;
        if (xb > m->w - 1) xb = m->w - 1;
        if (xa <= xb) paint_run(m, h, y, xa, xb, v);
    }
}

void mask_stroke_begin(mask *m, mask_hist *h) {
    if (!h || !m || !m->px) return;
    if (h->jr.depth > 0) {
        h->jr.depth++;
        return;
    }
    if (jr_begin(h, m)) h->jr.depth = 1;
}

void mask_stroke_dot(mask *m, mask_hist *h, float x, float y, const mask_brush *b) {
    mask_stroke_segment(m, h, x, y, x, y, b);
}

void mask_stroke_segment(mask *m, mask_hist *h, float x0, float y0, float x1, float y1,
                         const mask_brush *b) {
    if (!m || !m->px || !b) return;
    int v = b->value;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    stroke_capsule(m, h, x0, y0, x1, y1, (double)b->diameter * 0.5, (uint8_t)v);
}

void mask_stroke_end(mask *m, mask_hist *h) {
    (void)m;
    if (!h || h->jr.depth <= 0) return;
    if (--h->jr.depth > 0) return;
    jr_end(h);
}

/* ---------------------------------------------------------- polygon fill */

static void sort_crossings(double *xs, int *dir, int n) {
    for (int i = 1; i < n; i++) {
        double x = xs[i];
        int d = dir[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > x) {
            xs[j + 1] = xs[j];
            dir[j + 1] = dir[j];
            j--;
        }
        xs[j + 1] = x;
        dir[j + 1] = d;
    }
}

void mask_fill_polygon(mask *m, mask_hist *h, const float *xy, int n, int value) {
    if (!m || !m->px || !xy || n < 3 || m->w <= 0 || m->h <= 0) return;

    double area2 = 0.0, ylo = xy[1], yhi = xy[1];
    for (int i = 0; i < n; i++) {
        int j = (i + 1 == n) ? 0 : i + 1;
        area2 += (double)xy[2 * i] * (double)xy[2 * j + 1] -
                 (double)xy[2 * j] * (double)xy[2 * i + 1];
        double yi = xy[2 * i + 1];
        if (yi < ylo) ylo = yi;
        if (yi > yhi) yhi = yi;
    }
    if (fabs(area2) * 0.5 < 4.0) return;   /* a speck, not a lasso */

    int v = value;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    uint8_t val = (uint8_t)v;

    int half = n / 2 + 1;
    double *xs = malloc((size_t)n * sizeof *xs);
    int *dir = malloc((size_t)n * sizeof *dir);
    double *ilo = malloc((size_t)half * sizeof *ilo);
    double *ihi = malloc((size_t)half * sizeof *ihi);
    if (!xs || !dir || !ilo || !ihi) {
        free(xs);
        free(dir);
        free(ilo);
        free(ihi);
        return;
    }

    bool own = (h != NULL);
    if (own) mask_stroke_begin(m, h);

    int y0 = (int)floor(ylo - 1.0), y1 = (int)ceil(yhi + 1.0);
    if (y0 < 0) y0 = 0;
    if (y1 > m->h - 1) y1 = m->h - 1;

    for (int y = y0; y <= y1; y++) {
        double cy = y + 0.5;

        int cnt = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1 == n) ? 0 : i + 1;
            double yi = xy[2 * i + 1], yj = xy[2 * j + 1];
            if (yi == yj) continue;
            double e0 = yi < yj ? yi : yj, e1 = yi < yj ? yj : yi;
            if (cy < e0 || cy >= e1) continue;
            double t = (cy - yi) / (yj - yi);
            xs[cnt] = (double)xy[2 * i] + t * ((double)xy[2 * j] - (double)xy[2 * i]);
            dir[cnt] = (yj > yi) ? 1 : -1;
            cnt++;
        }
        if (!cnt) continue;
        sort_crossings(xs, dir, cnt);

        int ni = 0, wind = 0;
        double start = 0.0;
        for (int k = 0; k < cnt; k++) {
            if (wind == 0) start = xs[k];
            wind += dir[k];
            if (wind == 0 && ni < half) {
                ilo[ni] = start;
                ihi[ni] = xs[k];
                ni++;
            }
        }

        for (int t = 0; t < ni; t++) {
            int xa = first_inside(ilo[t]), xb = last_inside(ihi[t]);
            if (xa < 0) xa = 0;
            if (xb > m->w - 1) xb = m->w - 1;
            if (xa <= xb) paint_run(m, h, y, xa, xb, val);
        }

        /* 1 px outline (radius 0.5 capsule per edge), clipped to the inside
         * spans so it only ever fills gaps. */
        for (int i = 0; i < n && ni; i++) {
            int j = (i + 1 == n) ? 0 : i + 1;
            double xi = xy[2 * i], yi = xy[2 * i + 1];
            double xj = xy[2 * j], yj = xy[2 * j + 1];
            if (cy < (yi < yj ? yi : yj) - 1.0 || cy > (yi < yj ? yj : yi) + 1.0) continue;
            double lo, hi;
            if (!span_row(xi, yi, xj, yj, 0.5, cy, &lo, &hi)) continue;
            int ea = first_inside(lo), eb = last_inside(hi);
            if (ea > eb) continue;
            for (int t = 0; t < ni; t++) {
                int xa = first_inside(ilo[t]), xb = last_inside(ihi[t]);
                if (xa < ea) xa = ea;
                if (xb > eb) xb = eb;
                if (xa < 0) xa = 0;
                if (xb > m->w - 1) xb = m->w - 1;
                if (xa <= xb) paint_run(m, h, y, xa, xb, val);
            }
        }
    }

    if (own) mask_stroke_end(m, h);
    free(xs);
    free(dir);
    free(ilo);
    free(ihi);
}

/* -------------------------------------------------------------- feather */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* dst = max(dst, src) over `cnt` bytes. */
static void max_into(uint8_t *dst, const uint8_t *src, int cnt) {
    int k = 0;
#if defined(MASK_USE_SSE2)
    for (; k + 16 <= cnt; k += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(src + k));
        __m128i c = _mm_loadu_si128((const __m128i *)(const void *)(dst + k));
        _mm_storeu_si128((__m128i *)(void *)(dst + k), _mm_max_epu8(v, c));
    }
#endif
    for (; k < cnt; k++) {
        uint8_t v = src[k], c = dst[k];
        dst[k] = v > c ? v : c;
    }
}

/* Integer box blur with clamped edges; `tab` is the division table built once
 * per call (per-pixel integer division is far too slow at 16 MP). */
static void box_h(const uint8_t *src, uint8_t *dst, int w, int h, int r,
                  const uint8_t *tab) {
    if (r > w) r = w;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * (size_t)w;
        uint8_t *d = dst + (size_t)y * (size_t)w;
        uint32_t sum = (uint32_t)s[0] * (uint32_t)r;
        for (int x = 0; x <= r && x < w; x++) sum += s[x];
        int lo = 0, hi = w - r;                 /* [lo, hi): both neighbours inside */
        if (hi < r) hi = r;
        for (; lo < r && lo < w; lo++) {
            d[lo] = tab[sum];
            sum -= s[0];
            sum += s[lo + r + 1 < w ? lo + r + 1 : w - 1];
        }
        for (; lo < hi; lo++) {
            d[lo] = tab[sum];
            sum -= s[lo - r];
            sum += s[lo + r + 1];
        }
        for (; lo < w; lo++) {
            d[lo] = tab[sum];
            sum -= s[lo - r < 0 ? 0 : lo - r];
            sum += s[w - 1];
        }
    }
}

static void box_v(const uint8_t *src, uint8_t *dst, int w, int h, int r,
                  const uint8_t *tab, uint16_t *acc) {
    memset(acc, 0, (size_t)w * sizeof *acc);
    for (int dy = -r; dy <= r; dy++) {
        const uint8_t *row = src + (size_t)clampi(dy, 0, h - 1) * (size_t)w;
        for (int x = 0; x < w; x++) acc[x] = (uint16_t)(acc[x] + row[x]);
    }
    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) d[x] = tab[acc[x]];
        const uint8_t *ro = src + (size_t)clampi(y - r, 0, h - 1) * (size_t)w;
        const uint8_t *ri = src + (size_t)clampi(y + r + 1, 0, h - 1) * (size_t)w;
        for (int x = 0; x < w; x++) {
            acc[x] = (uint16_t)(acc[x] - ro[x]);
            acc[x] = (uint16_t)(acc[x] + ri[x]);
        }
    }
}

bool mask_feather(const mask *m, int feather_px, uint8_t **w) {
    if (!m || !m->px || !w || m->w <= 0 || m->h <= 0) return false;
    *w = NULL;
    size_t n = (size_t)m->w * (size_t)m->h;

    uint8_t *out = malloc(n);
    if (!out) return false;
    if (feather_px <= 0) {
        memcpy(out, m->px, n);
        *w = out;
        return true;
    }

    /* content bounds: nothing outside them can survive the dilation */
    int minx = m->w, miny = m->h, maxx = -1, maxy = -1;
    for (int y = 0; y < m->h; y++) {
        const uint8_t *row = m->px + (size_t)y * (size_t)m->w;
        for (int x = 0; x < m->w; x++) {
            if (!row[x]) continue;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (maxx < 0) {                 /* empty mask: nothing to feather */
        memset(out, 0, n);
        *w = out;
        return true;
    }

    /* Everything the feather can touch lives inside the content bounds
     * grown by the dilation radius plus the support of the three box passes.
     * Working on that crop is exact (outside it the result is the raw mask,
     * which is 0 there) and cuts the blur work by the empty margin. */
    double r = feather_px * 0.5;
    double sigma = (double)feather_px * 0.25;
    int br = (int)lround(sigma * 1.5);
    if (br < 1) br = 1;
    if (br > 127) br = 127;                       /* keeps the sums inside 16 bit */
    int win = 2 * br + 1, half = win / 2;
    int reach = (int)ceil(r) + 3 * br;

    int rx0 = minx - reach, ry0 = miny - reach;
    int rx1 = maxx + reach, ry1 = maxy + reach;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > m->w - 1) rx1 = m->w - 1;
    if (ry1 > m->h - 1) ry1 = m->h - 1;
    int bw = rx1 - rx0 + 1, bh = ry1 - ry0 + 1;
    size_t bn = (size_t)bw * (size_t)bh;

    uint8_t *sub = malloc(bn);                    /* dilated, then blurred */
    uint8_t *tmp = malloc(bn);
    uint8_t *tab = malloc(65536);
    uint16_t *acc = malloc((size_t)bw * sizeof *acc);
    if (!sub || !tmp || !tab || !acc) {
        free(sub);
        free(tmp);
        free(tab);
        free(acc);
        free(out);
        return false;
    }
    for (int y = 0; y < bh; y++)
        memcpy(sub + (size_t)y * (size_t)bw,
               m->px + (size_t)(y + ry0) * (size_t)m->w + (size_t)rx0, (size_t)bw);
    memcpy(tmp, sub, bn);                         /* raw crop: the taps read this */

    /* Disc dilation, radius r. The web UI composites 8/12/16 copies on rings
     * at r/3, 2r/3 and r with a per-ring angle jitter; the same taps here.
     * Bands of rows keep the destination in L2 across all 36 taps. */
    static const double K[3] = { 1.0 / 3.0, 2.0 / 3.0, 1.0 };
    static const int NT[3] = { 8, 12, 16 };
    const int band = 64;
    for (int yb = 0; yb < bh; yb += band) {
        int ye = yb + band;
        if (ye > bh) ye = bh;
        for (int ring = 0; ring < 3; ring++) {
            double rr = r * K[ring];
            for (int i = 0; i < NT[ring]; i++) {
                double a = 2.0 * 3.14159265358979323846 * (double)i / (double)NT[ring] +
                           K[ring];
                int ox = (int)lround(cos(a) * rr);
                int oy = (int)lround(sin(a) * rr);
                if (!ox && !oy) continue;
                int lo = yb, hi = ye;
                if (oy > 0) { if (hi > bh - oy) hi = bh - oy; }
                else if (oy < 0) { if (lo < -oy) lo = -oy; }
                int sx0 = ox > 0 ? 0 : -ox, sx1 = ox > 0 ? bw - ox : bw;
                int cnt = sx1 - sx0;
                if (cnt <= 0) continue;
                for (int y = lo; y < hi; y++) {
                    const uint8_t *src = tmp + (size_t)(y + oy) * (size_t)bw +
                                         (size_t)(sx0 + ox);
                    uint8_t *dst = sub + (size_t)y * (size_t)bw + (size_t)sx0;
                    max_into(dst, src, cnt);
                }
            }
        }
    }

    /* Three box passes approximate the Gaussian with sigma = f/4. */
    for (int s = 0; s < 65536; s++) tab[s] = (uint8_t)((s + half) / win);
    for (int p = 0; p < 3; p++) {
        box_h(sub, tmp, bw, bh, br, tab);
        box_v(tmp, sub, bw, bh, br, tab, acc);
    }

    /* Raw mask everywhere, max() with the feathered crop on top. */
    memcpy(out, m->px, n);
    for (int y = 0; y < bh; y++) {
        const uint8_t *raw = m->px + (size_t)(y + ry0) * (size_t)m->w + (size_t)rx0;
        const uint8_t *f = sub + (size_t)y * (size_t)bw;
        uint8_t *o = out + (size_t)(y + ry0) * (size_t)m->w + (size_t)rx0;
        for (int x = 0; x < bw; x++) {
            uint8_t a = raw[x], b = f[x];
            o[x] = a > b ? a : b;
        }
    }

    free(sub);
    free(tmp);
    free(tab);
    free(acc);
    *w = out;
    return true;
}

/* ------------------------------------------------------------ edge map */

/* 3x3 box blur, integer /9, clamped edges (same as the web UI's blur3). */
static void blur3(const uint8_t *a, uint8_t *o, int w, int h) {
    uint16_t *t = malloc((size_t)w * (size_t)h * sizeof *t);
    if (!t) {
        memcpy(o, a, (size_t)w * (size_t)h);
        return;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = a + (size_t)y * (size_t)w;
        uint16_t *d = t + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++)
            d[x] = (uint16_t)(s[clampi(x - 1, 0, w - 1)] + s[x] +
                              s[clampi(x + 1, 0, w - 1)]);
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *u = t + (size_t)clampi(y - 1, 0, h - 1) * (size_t)w;
        const uint16_t *c = t + (size_t)y * (size_t)w;
        const uint16_t *d = t + (size_t)clampi(y + 1, 0, h - 1) * (size_t)w;
        uint8_t *o_row = o + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++)
            o_row[x] = (uint8_t)((u[x] + c[x] + d[x]) / 9u);
    }
    free(t);
}

bool mask_build_edges(const uint8_t *rgba, int w, int h, mask_edges *out) {
    if (!rgba || !out || w <= 0 || h <= 0) return false;
    out->e = NULL;
    out->w = out->h = 0;

    size_t n = (size_t)w * (size_t)h;
    uint8_t *lum = malloc(n);
    uint8_t *bl = malloc(n);
    uint8_t *e = calloc(n, 1);
    if (!lum || !bl || !e) {
        free(lum);
        free(bl);
        free(e);
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = rgba + i * 4;
        uint32_t y = (uint32_t)((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8);
        lum[i] = (uint8_t)(y * p[3] / 255u);
    }
    blur3(lum, bl, w, h);

    const int stride = w * 4;
    for (int y = 1; y < h - 1; y++) {
        const uint8_t *t = bl + (size_t)(y - 1) * (size_t)w;
        const uint8_t *c = bl + (size_t)y * (size_t)w;
        const uint8_t *b = bl + (size_t)(y + 1) * (size_t)w;
        const uint8_t *at = rgba + (size_t)(y - 1) * (size_t)stride;
        const uint8_t *ac = rgba + (size_t)y * (size_t)stride;
        const uint8_t *ab = rgba + (size_t)(y + 1) * (size_t)stride;
        for (int x = 1; x < w - 1; x++) {
            int gx = (int)t[x + 1] + 2 * (int)c[x + 1] + (int)b[x + 1] -
                     (int)t[x - 1] - 2 * (int)c[x - 1] - (int)b[x - 1];
            int gy = (int)b[x - 1] + 2 * (int)b[x] + (int)b[x + 1] -
                     (int)t[x - 1] - 2 * (int)t[x] - (int)t[x + 1];
            int sl = abs(gx) + abs(gy);

            int ai = x * 4 + 3;
            int ax0 = ai - 4, ax1 = ai + 4;
            gx = (int)at[ax1] + 2 * (int)ac[ax1] + (int)ab[ax1] -
                 (int)at[ax0] - 2 * (int)ac[ax0] - (int)ab[ax0];
            gy = (int)ab[ax0] + 2 * (int)ab[ai] + (int)ab[ax1] -
                 (int)at[ax0] - 2 * (int)at[ai] - (int)at[ax1];
            int sa = abs(gx) + abs(gy);

            int v = (sl > sa ? sl : sa) / 4;
            if (v > 255) v = 255;
            e[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)v;
        }
    }

    free(lum);
    free(bl);
    out->e = e;
    out->w = w;
    out->h = h;
    return true;
}

void mask_edges_free(mask_edges *e) {
    if (!e) return;
    free(e->e);
    e->e = NULL;
    e->w = e->h = 0;
}

/* ----------------------------------------------------------- live wire */

typedef struct heap {
    float *key;
    int *val;
    int n, cap;
} heap;

static void heap_push(heap *hp, float key, int val) {
    if (hp->n == hp->cap) {
        int cap = hp->cap ? hp->cap * 2 : 1024;
        float *k = realloc(hp->key, (size_t)cap * sizeof *k);
        int *v = realloc(hp->val, (size_t)cap * sizeof *v);
        if (!k || !v) {           /* out of memory: drop the entry, Dijkstra
                                   * still terminates, just with a worse path */
            if (k) hp->key = k;
            if (v) hp->val = v;
            return;
        }
        hp->key = k;
        hp->val = v;
        hp->cap = cap;
    }
    int i = hp->n++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (hp->key[p] <= key) break;
        hp->key[i] = hp->key[p];
        hp->val[i] = hp->val[p];
        i = p;
    }
    hp->key[i] = key;
    hp->val[i] = val;
}

static int heap_pop(heap *hp) {
    int top = hp->val[0];
    float key = hp->key[--hp->n];
    int val = hp->val[hp->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hp->n) break;
        if (c + 1 < hp->n && hp->key[c + 1] < hp->key[c]) c++;
        if (hp->key[c] >= key) break;
        hp->key[i] = hp->key[c];
        hp->val[i] = hp->val[c];
        i = c;
    }
    if (hp->n) {
        hp->key[i] = key;
        hp->val[i] = val;
    }
    return top;
}

int mask_live_wire(const mask_edges *e, int ax, int ay, int bx, int by, int *out_xy, int cap) {
    if (!e || !e->e || !out_xy || e->w <= 0 || e->h <= 0 || cap < 1) return 0;
    int w = e->w, h = e->h;
    ax = clampi(ax, 0, w - 1);
    ay = clampi(ay, 0, h - 1);
    bx = clampi(bx, 0, w - 1);
    by = clampi(by, 0, h - 1);
    if (ax == bx && ay == by) {
        if (cap < 1) return 0;
        out_xy[0] = ax;
        out_xy[1] = ay;
        return 1;
    }

    const int M = 24;
    int x0 = clampi((ax < bx ? ax : bx) - M, 0, w - 1);
    int y0 = clampi((ay < by ? ay : by) - M, 0, h - 1);
    int x1 = clampi((ax > bx ? ax : bx) + M, 0, w - 1);
    int y1 = clampi((ay > by ? ay : by) + M, 0, h - 1);
    int W = x1 - x0 + 1, H = y1 - y0 + 1;
    long N = (long)W * (long)H;
    if (N > 1500000) return 0;         /* a huge jump: caller draws a line */

    int mx = 1;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *row = e->e + (size_t)y * (size_t)w;
        for (int x = x0; x <= x1; x++)
            if (row[x] > mx) mx = row[x];
    }

    float *cost = malloc((size_t)N * sizeof *cost);
    float *dist = malloc((size_t)N * sizeof *dist);
    int *prev = malloc((size_t)N * sizeof *prev);
    uint8_t *done = calloc((size_t)N, 1);
    heap hp;
    memset(&hp, 0, sizeof hp);
    if (!cost || !dist || !prev || !done) {
        free(cost);
        free(dist);
        free(prev);
        free(done);
        return 0;
    }

    for (int y = 0; y < H; y++) {
        const uint8_t *row = e->e + (size_t)(y + y0) * (size_t)w + (size_t)x0;
        for (int x = 0; x < W; x++) {
            float f = 1.0f - (float)row[x] / (float)mx;
            cost[(size_t)y * (size_t)W + (size_t)x] = 0.1f + f * f;
            dist[(size_t)y * (size_t)W + (size_t)x] = INF_F;
            prev[(size_t)y * (size_t)W + (size_t)x] = -1;
        }
    }

    int s = (ay - y0) * W + (ax - x0);
    int t = (by - y0) * W + (bx - x0);
    dist[s] = 0.0f;
    heap_push(&hp, 0.0f, s);

    while (hp.n) {
        int u = heap_pop(&hp);
        if (done[u]) continue;
        done[u] = 1;
        if (u == t) break;
        int ux = u % W, uy = u / W;
        float du = dist[u];
        for (int dy = -1; dy <= 1; dy++) {
            int ny = uy + dy;
            if (ny < 0 || ny >= H) continue;
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = ux + dx;
                if (nx < 0 || nx >= W) continue;
                int v = ny * W + nx;
                if (done[v]) continue;
                float step = (dx && dy) ? 1.4142f : 1.0f;
                float nd = du + cost[v] * step;
                if (nd < dist[v]) {
                    dist[v] = nd;
                    prev[v] = u;
                    heap_push(&hp, nd, v);
                }
            }
        }
    }

    int count = 0;
    if (done[t]) {
        for (int v = t;; v = prev[v]) {
            count++;
            if (v == s) break;
            if (prev[v] < 0) { count = 0; break; }
        }
    }
    if (count < 1 || count > cap) {
        free(cost);
        free(dist);
        free(prev);
        free(done);
        free(hp.key);
        free(hp.val);
        return 0;
    }
    int i = count - 1;
    for (int v = t;; v = prev[v]) {
        out_xy[i * 2] = v % W + x0;
        out_xy[i * 2 + 1] = v / W + y0;
        i--;
        if (v == s) break;
    }

    free(cost);
    free(dist);
    free(prev);
    free(done);
    free(hp.key);
    free(hp.val);
    return count;
}

bool mask_snap(const mask_edges *e, int x, int y, int radius, int *out_x, int *out_y) {
    if (!e || !e->e || !out_x || !out_y || e->w <= 0 || e->h <= 0) return false;
    if (radius < 1) radius = 1;
    double best = -1.0e9;
    int bx = x, by = y;
    int r2 = radius * radius;
    int y0 = clampi(y - radius, 0, e->h - 1), y1 = clampi(y + radius, 0, e->h - 1);
    for (int py = y0; py <= y1; py++) {
        const uint8_t *row = e->e + (size_t)py * (size_t)e->w;
        for (int px = clampi(x - radius, 0, e->w - 1); px <= clampi(x + radius, 0, e->w - 1); px++) {
            int dx = px - x, dy = py - y;
            int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            double score = (double)row[px] - 16.0 * sqrt((double)d2) / (double)radius;
            if (score > best) {
                best = score;
                bx = px;
                by = py;
            }
        }
    }
    if (best < 20.0) return false;
    *out_x = bx;
    *out_y = by;
    return true;
}

/* Test-only accessor, declared by tests/test_mask.c: the live journal size.
 * Kept out of mask.h so the contract header stays untouched. */
size_t mask_hist_bytes_used(const mask_hist *h) { return h ? h->bytes : 0; }
