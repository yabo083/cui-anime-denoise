/* ui_lasso.c -- the magnetic lasso: input, live-wire preview and drawing.
 * Split out of ui_stage.c so the stage file stays under the 800-line cap;
 * it shares the stage's coordinate helpers (stage_image_w / stage_screen_to_img
 * and friends). The feel (anchor gap, snap radius, close gestures) is the web
 * UI's, see docs/fluent-redesign.md §8. */

#include "app.h"
#include "chrome.h"
#include "icons.h"
#include "ui_tokens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lasso. The outline is aimed by eye, so its feel must not change with zoom:
 * every distance below is screen points and is divided by the zoom where the
 * mask needs image pixels. The numbers are the web UI's. */
#define LASSO_ANCHOR_GAP      48.0f          /* travel between automatic vertices */
#define LASSO_CLOSE_R         10.0f          /* the first vertex is a click target */
#define LASSO_SNAP_R          10.0f          /* edge search radius */
#define LASSO_SNAP_MIN          2            /* image px */
#define LASSO_SNAP_MAX         24
#define LASSO_MIN_VERTS         3            /* an outline needs three corners */
#define LASSO_MIN_AREA         4.0f          /* image px²; below this it was a click */
#define LASSO_DBL_MS         400.0           /* a second click this soon... */
#define LASSO_DBL_PX           6.0f          /* ...and this close closes the outline */

/* ------------------------------------------------------------------------ */

static void lasso_push(app_state *st, float x, float y) {
    if (st->lasso_n + 2 > st->lasso_cap) {
        int cap = st->lasso_cap ? st->lasso_cap * 2 : 64;
        float *p = (float *)realloc(st->lasso, (size_t)cap * sizeof(float));
        if (!p) return;
        st->lasso = p;
        st->lasso_cap = cap;
    }
    st->lasso[st->lasso_n++] = x;
    st->lasso[st->lasso_n++] = y;
}

void ensure_edges(job *j) {
    if (j->edges_ready || !j->orig.rgba) return;
    if (mask_build_edges(j->orig.rgba, j->orig.w, j->orig.h, &j->edges)) j->edges_ready = true;
}

/* Recomputes the live-wire segment from the last vertex to the cursor. */
static void lasso_update_live(app_state *st, job *j, float hx, float hy) {
    st->lasso_hover_x = hx;
    st->lasso_hover_y = hy;
    if (!j->edges_ready || st->lasso_n < 2) return;
    int ax = (int)(st->lasso[st->lasso_n - 2] + 0.5f);
    int ay = (int)(st->lasso[st->lasso_n - 1] + 0.5f);
    int bx = (int)(hx + 0.5f), by = (int)(hy + 0.5f);
    int cap = 4096;
    free(st->lasso_live);
    st->lasso_live = (int *)malloc((size_t)cap * 2 * sizeof(int));
    if (!st->lasso_live) { st->lasso_live_n = 0; return; }
    st->lasso_live_n = mask_live_wire(&j->edges, ax, ay, bx, by, st->lasso_live, cap);
}

/* Radius of the edge search around the cursor, in image pixels: ten screen
 * points whatever the zoom, which is where the mask's own limit kicks in. */
static int lasso_snap_radius(const job *j) {
    float z = j->view.zoom;
    long r = lroundf(z > 0.0f ? LASSO_SNAP_R / z : LASSO_SNAP_R);
    if (r < LASSO_SNAP_MIN) r = LASSO_SNAP_MIN;
    if (r > LASSO_SNAP_MAX) r = LASSO_SNAP_MAX;
    return (int)r;
}

/* Pulls a cursor point onto the nearest strong edge. mask_snap() hands the
 * point back unchanged when nothing nearby scores 20, so the result is always
 * usable. The half pixel puts the vertex on that pixel's centre, the space the
 * brush and mask_fill_polygon() both measure in. */
static void lasso_snap_pt(const job *j, float x, float y, float *ox, float *oy) {
    *ox = x;
    *oy = y;
    if (!j->edges_ready) return;
    int sx = 0, sy = 0;
    if (!mask_snap(&j->edges, (int)lroundf(x), (int)lroundf(y), lasso_snap_radius(j), &sx, &sy)) return;
    *ox = (float)sx + 0.5f;
    *oy = (float)sy + 0.5f;
}

/* True when a click would close the outline: three corners, and the cursor
 * within LASSO_CLOSE_R screen points of the first one. */
static bool lasso_near_start(const app_state *st, const job *j, float ix, float iy) {
    if (!st->lasso_active || st->lasso_n < LASSO_MIN_VERTS * 2) return false;
    float z = j->view.zoom > 0.0f ? j->view.zoom : 1.0f;
    float dx = ix - st->lasso[0], dy = iy - st->lasso[1];
    return sqrtf(dx * dx + dy * dy) * z < LASSO_CLOSE_R;
}

/* Commits the cursor as the next vertex: clamped to the image, snapped to an
 * edge, with the live preview restarting from it. */
static void lasso_add_vertex(app_state *st, job *j, float ix, float iy) {
    float w = stage_image_w(j), h = stage_image_h(j);
    if (w > 0.0f) { ix = ix < 0.0f ? 0.0f : (ix > w - 1.0f ? w - 1.0f : ix); }
    if (h > 0.0f) { iy = iy < 0.0f ? 0.0f : (iy > h - 1.0f ? h - 1.0f : iy); }
    float sx, sy;
    lasso_snap_pt(j, ix, iy, &sx, &sy);
    lasso_push(st, sx, sy);
    lasso_update_live(st, j, sx, sy);
}

bool lasso_active(const app_state *st) {
    return st->lasso_active && st->lasso_n >= 2;
}

/* Fills the outline - or clears it when subtracting - and forgets it. A click
 * without area, or an outline with too few corners, is dropped silently: the
 * user was aiming, not committing. */
void lasso_close_now(app_state *st, job *j, bool subtract) {
    if (j && st->lasso_n >= LASSO_MIN_VERTS * 2) {
        int nv = st->lasso_n / 2;
        double area = 0.0;
        for (int i = 0; i < nv; i++) {
            const float *a = st->lasso + i * 2;
            const float *b = st->lasso + ((i + 1) % nv) * 2;
            area += (double)a[0] * (double)b[1] - (double)b[0] * (double)a[1];
        }
        area = fabs(area) * 0.5;
        if (area >= LASSO_MIN_AREA) {
            mask_stroke_begin(&j->m, j->hist);
            mask_fill_polygon(&j->m, j->hist, st->lasso, nv,
                              subtract ? 0 : (int)(st->brush_mix * 2.55f + 0.5f));
            mask_stroke_end(&j->m, j->hist);
            mask_note(st, j);
            j->composite_pending = true;   /* the pump composites after the debounce */
        }
    }
    lasso_reset(st);
}

/* Backspace: drops the last vertex, leaving the preview running from the one
 * before it. With fewer than two vertices left there is nothing to step back
 * to, so the outline goes away entirely. */
void lasso_drop_anchor(app_state *st) {
    if (!st->lasso_active || st->lasso_n < 4) { lasso_reset(st); return; }
    st->lasso_n -= 2;
    job *j = job_selected(st);
    if (j) lasso_update_live(st, j, st->lasso_hover_x, st->lasso_hover_y);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

/* Runs while the lasso tool owns the stage. The pointer is already known to
 * be inside the canvas; buttons are the frame-edge flags. */
void lasso_handle_input(app_state *st, job *j, float ix, float iy, bool left_press,
                        bool right_press) {
    ensure_edges(j);

    bool alt = (cui_key_mods(st->ui) & CUI_MOD_ALT) != 0;
    bool near = lasso_near_start(st, j, ix, iy);

    /* two clicks in the same spot in quick succession are the other way to
     * say "close it" */
    static double prev_click_at;
    static float prev_click_x, prev_click_y;
    bool dbl = false;
    if (left_press) {
        cui_vec2 m = cui_mouse_pos(st->ui);
        double now = now_seconds();
        dbl = prev_click_at > 0.0 && now - prev_click_at < LASSO_DBL_MS / 1000.0 &&
              hypotf(m.x - prev_click_x, m.y - prev_click_y) < LASSO_DBL_PX;
        prev_click_at = now;
        prev_click_x = m.x;
        prev_click_y = m.y;
    }

    if (!st->lasso_active) {
        if (left_press) {
            st->lasso_active = true;
            lasso_add_vertex(st, j, ix, iy);
        }
        return;
    }

    if (left_press) {
        /* back on the first vertex with three corners placed: this click
         * closes instead of adding another one */
        if ((near || dbl) && st->lasso_n >= LASSO_MIN_VERTS * 2) {
            lasso_close_now(st, j, alt);
            return;
        }
        lasso_add_vertex(st, j, ix, iy);
        return;
    }

    if (right_press) {
        lasso_close_now(st, j, true);   /* right-click closes subtractively */
        return;
    }

    /* Dragging lays an automatic vertex every LASSO_ANCHOR_GAP screen
     * points of travel and the segment from the last vertex to the cursor
     * stays a live-wire preview; releasing the button only ends the drag,
     * it does not close the outline. */
    if (st->lasso_n < 2) return;
    float sx, sy;
    lasso_snap_pt(j, ix, iy, &sx, &sy);
    float lx = st->lasso[st->lasso_n - 2], ly = st->lasso[st->lasso_n - 1];
    float z = j->view.zoom > 0.0f ? j->view.zoom : 1.0f;
    if (!near && cui_mouse_down(st->ui, CUI_MOUSE_LEFT) && hypotf(sx - lx, sy - ly) * z >= LASSO_ANCHOR_GAP) {
        lasso_push(st, sx, sy);
        /* the preview restarts here; the next move repaints it */
        st->lasso_hover_x = sx;
        st->lasso_hover_y = sy;
        st->lasso_live_n = 0;
        return;
    }
    /* the preview runs to the cursor, or back to the first vertex once a
     * click there would close the outline */
    float tx = near ? st->lasso[0] : sx, ty = near ? st->lasso[1] : sy;
    float dx = tx - st->lasso_hover_x, dy = ty - st->lasso_hover_y;
    if (dx * dx + dy * dy > 1.0f) lasso_update_live(st, j, tx, ty);
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

void lasso_paint(cui_ctx *ui, app_state *st, job *j, cui_rect r) {
    if (!st->lasso_active || st->lasso_n < 2) return;

    bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
    cui_color edge = CUI_RGBA(dark ? 0x4CC2FFF2 : 0x0067C0F2);
    cui_color edge_fill = CUI_RGBA(dark ? 0x4CC2FF40 : 0x0067C040);
    cui_color halo = CUI_RGBA(0x00000066);

    for (int i = 0; i + 3 < st->lasso_n; i += 2) {
        float ax, ay, bx, by;
        stage_img_to_screen(j, r, st->lasso[i], st->lasso[i + 1], &ax, &ay);
        stage_img_to_screen(j, r, st->lasso[i + 2], st->lasso[i + 3], &bx, &by);
        cui_draw_line(ui, (cui_vec2){ ax, ay }, (cui_vec2){ bx, by }, 3.0f, halo);
        cui_draw_line(ui, (cui_vec2){ ax, ay }, (cui_vec2){ bx, by }, 1.5f, CUI_RGBA(0xfffffff2));
    }
    for (int i = 2; i + 1 < st->lasso_n; i += 2) {
        float ax, ay;
        stage_img_to_screen(j, r, st->lasso[i], st->lasso[i + 1], &ax, &ay);
        cui_draw_circle(ui, (cui_vec2){ ax, ay }, 4.5f, halo);
        cui_draw_circle(ui, (cui_vec2){ ax, ay }, 3.0f, CUI_RGBA(0xffffffff));
    }
    /* the first vertex is also the click target that closes the outline, so
     * it gets a bigger ring - and a thicker, accent one while a click there
     * would actually close */
    {
        cui_vec2 mp = cui_mouse_pos(ui);
        float hx, hy;
        stage_screen_to_img(j, r, mp.x, mp.y, &hx, &hy);
        bool closable = lasso_near_start(st, j, hx, hy);
        float fx, fy;
        stage_img_to_screen(j, r, st->lasso[0], st->lasso[1], &fx, &fy);
        float ring = closable ? 8.0f : 6.0f;
        cui_draw_circle(ui, (cui_vec2){ fx, fy }, ring, CUI_RGBA(0x00000080));
        cui_draw_circle(ui, (cui_vec2){ fx, fy }, ring - (closable ? 3.0f : 2.0f),
                        closable ? edge : CUI_RGBA(0xffffffff));
    }
    float ax, ay;
    stage_img_to_screen(j, r, st->lasso[st->lasso_n - 2], st->lasso[st->lasso_n - 1], &ax, &ay);
    if (st->lasso_live_n > 1) {
        for (int i = 0; i + 3 < st->lasso_live_n * 2; i += 2) {
            float bx, by, cx2, cy2;
            stage_img_to_screen(j, r, (float)st->lasso_live[i], (float)st->lasso_live[i + 1], &bx, &by);
            stage_img_to_screen(j, r, (float)st->lasso_live[i + 2], (float)st->lasso_live[i + 3], &cx2, &cy2);
            cui_draw_line(ui, (cui_vec2){ bx, by }, (cui_vec2){ cx2, cy2 }, 3.0f, halo);
            cui_draw_line(ui, (cui_vec2){ bx, by }, (cui_vec2){ cx2, cy2 }, 1.4f, edge);
        }
    } else {
        stage_img_to_screen(j, r, st->lasso_hover_x, st->lasso_hover_y, &ax, &ay);
        float lx, ly;
        stage_img_to_screen(j, r, st->lasso[st->lasso_n - 2], st->lasso[st->lasso_n - 1], &lx, &ly);
        cui_draw_line(ui, (cui_vec2){ lx, ly }, (cui_vec2){ ax, ay }, 1.2f, CUI_RGBA(0xffffffff));
        cui_draw_line(ui, (cui_vec2){ lx, ly }, (cui_vec2){ ax, ay }, 1.0f, edge);
    }
    /* the closing preview: fill the would-be outline so the user sees what
     * they are about to protect (the web UI shades it the same accent) */
    if (st->lasso_n >= LASSO_MIN_VERTS * 2) {
        cui_vec2 mp = cui_mouse_pos(ui);
        float hx, hy;
        stage_screen_to_img(j, r, mp.x, mp.y, &hx, &hy);
        if (lasso_near_start(st, j, hx, hy)) {
            cui_vec2 *pts = malloc((size_t)(st->lasso_n / 2) * sizeof *pts);
            if (pts) {
                for (int i = 0; i + 1 < st->lasso_n; i += 2)
                    stage_img_to_screen(j, r, st->lasso[i], st->lasso[i + 1], &pts[i / 2].x, &pts[i / 2].y);
                cui_draw_polygon(ui, pts, st->lasso_n / 2, edge_fill);
                free(pts);
            }
        }
    }
}
