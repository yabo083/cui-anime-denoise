/* ui_stage.c -- see AGENTS.md for the routing table and docs/design.md for the rules. */

#include "app.h"
#include "chrome.h"
#include "icons.h"
#include "ui_tokens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stage geometry                                                           */
/* ------------------------------------------------------------------------ */

/* Sizes below are screen points derived from the visual tokens; the ones the
 * ticket fixes by hand (divider handle, brush cursor) have no token role. */
#define STAGE_FIT_PAD       (UI_PAD * 2.0f)  /* breathing room around a fitted image */
#define STAGE_SPLIT_HIT      14.0f           /* half-width of the divider grab band */
#define STAGE_HANDLE_R       17.0f           /* divider handle radius, idle */
#define STAGE_HANDLE_R_HOT   21.0f           /* divider handle radius, hover/drag */
#define STAGE_HANDLE_RING     2.0f           /* divider handle outline, idle */
#define STAGE_HANDLE_RING_HOT 4.0f           /* divider handle outline, hover/drag */
#define STAGE_BRUSH_MIN       3.0f           /* brush cursor radius floor, screen px */
#define STAGE_BRUSH_HINT_ZOOM 0.2f           /* below this the cursor is not the coverage */
#define STAGE_ZOOM_MIN        0.02f          /* view zoom range, screen points per image px */
#define STAGE_ZOOM_MAX        16.0f

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

/* Brush size keys step proportionally, so 2 -> 300 takes the same number of
 * taps at either end of the range. */
#define BRUSH_STEP             0.85f
#define BRUSH_KEY_MIN          2.0f
#define BRUSH_KEY_MAX        300.0f

/* The stage measures in image pixels, the space the mask, the edge map, the
 * brush and the worker all use. Above DISPLAY_MAX the picture we draw is only
 * a downscaled texture, so its size is NOT that space: measuring the brush,
 * the lasso and the view in display pixels put a stroke's mask pixels a factor
 * (mask/display) away from the cursor for every large image. */
static float image_w(const job *j) {
    if (j->m.w > 0) return (float)j->m.w;
    if (j->orig.w > 0) return (float)j->orig.w;
    return (float)j->disp_w;
}
static float image_h(const job *j) {
    if (j->m.h > 0) return (float)j->m.h;
    if (j->orig.h > 0) return (float)j->orig.h;
    return (float)j->disp_h;
}

void stage_fit(job *j, cui_rect r) {
    float iw = image_w(j), ih = image_h(j);
    if (iw <= 0.0f || ih <= 0.0f) {
        /* no texture yet: stay "unfitted" so the next frame fits for real
         * instead of leaving a 1:1 view centred on the image origin */
        j->view = (view2d){ 0.0f, 0.0f, 0.0f };
        j->view_fitted = false;
        return;
    }
    const float pad = STAGE_FIT_PAD;
    float sx = (r.w - 2 * pad) / iw;
    float sy = (r.h - 2 * pad) / ih;
    float s = sx < sy ? sx : sy;
    if (s <= 0.0f) s = 0.001f;
    j->view.zoom = s;
    j->view.cx = iw * 0.5f;
    j->view.cy = ih * 0.5f;
    j->view_fitted = true;
    j->fit_w = r.w;
    j->fit_h = r.h;
}

static void stage_ensure_view(job *j, cui_rect r) {
    if (j->view.zoom <= 0.0f) { stage_fit(j, r); return; }
    /* a fitted image follows the canvas when the window resizes */
    if (j->view_fitted && (fabsf(r.w - j->fit_w) > 1.0f || fabsf(r.h - j->fit_h) > 1.0f))
        stage_fit(j, r);
}

/* Image pixel <-> screen point. `zoom` is screen points per image pixel and
 * (cx, cy) is the image point that sits at the canvas centre. */
static void img_to_screen(const job *j, cui_rect r, float ix, float iy, float *sx, float *sy) {
    float z = j->view.zoom;
    *sx = r.x + r.w * 0.5f + (ix - j->view.cx) * z;
    *sy = r.y + r.h * 0.5f + (iy - j->view.cy) * z;
}

static void screen_to_img(const job *j, cui_rect r, float sx, float sy, float *ix, float *iy) {
    float z = j->view.zoom;
    if (z <= 0.0f) z = 1.0f;
    *ix = j->view.cx + (sx - (r.x + r.w * 0.5f)) / z;
    *iy = j->view.cy + (sy - (r.y + r.h * 0.5f)) / z;
}

/* Zooms by `factor` with the image point under (sx, sy) held in place, so the
 * picture grows out from under the pointer instead of from the canvas centre.
 * Solving screen = centre + (image - c) * z for the new centre:
 *   c' = image - (screen - centre) / z'
 * The clamp is applied to z' before the centre is solved, which keeps the
 * anchor exact at the ends of the range too. */
static void stage_zoom_at(job *j, cui_rect r, float sx, float sy, float factor) {
    float z = j->view.zoom;
    if (z <= 0.0f || !(factor > 0.0f)) return;
    float nz = z * factor;
    if (nz < STAGE_ZOOM_MIN) nz = STAGE_ZOOM_MIN;
    if (nz > STAGE_ZOOM_MAX) nz = STAGE_ZOOM_MAX;
    if (nz == z) return;
    float ix, iy;
    screen_to_img(j, r, sx, sy, &ix, &iy);
    j->view.zoom = nz;
    j->view.cx = ix - (sx - (r.x + r.w * 0.5f)) / nz;
    j->view.cy = iy - (sy - (r.y + r.h * 0.5f)) / nz;
    j->view_fitted = false;
}

/* Zeroes on mouse buttons: true on the frame a press starts. */
static bool mouse_pressed(cui_ctx *ui, cui_mouse_button b) {
    static bool was_left, was_right, was_middle;
    bool down = cui_mouse_down(ui, b);
    bool *was = b == CUI_MOUSE_LEFT ? &was_left : (b == CUI_MOUSE_RIGHT ? &was_right : &was_middle);
    bool hit = down && !*was;
    *was = down;
    return hit;
}

void mask_note(app_state *st, job *j) {
    j->mask_active = mask_any(&j->m);
    j->mask_tex_dirty = true;
    j->composite_dirty = true;
    j->dirty_at = now_seconds();
    (void)st;
}

/* ------------------------------------------------------------------------ */
/* Stage input                                                              */
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
    float w = image_w(j), h = image_h(j);
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

/* Brush size from the keyboard: proportional, so the step feels the same at a
 * 3 px brush and at a 300 px one. */
static void brush_step(app_state *st, float factor) {
    float s = st->brush_size > 0.0f ? st->brush_size * factor : BRUSH_KEY_MIN;
    if (s < BRUSH_KEY_MIN) s = BRUSH_KEY_MIN;
    if (s > BRUSH_KEY_MAX) s = BRUSH_KEY_MAX;
    if (s == st->brush_size) return;
    st->brush_size = s;
    settings_from_state(st);
}

void stage_handle_input(app_state *st, job *j) {
    cui_ctx *ui = st->ui;
    cui_rect r = st->stage;
    cui_vec2 m = cui_mouse_pos(ui);
    bool inside = m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;

    /* The wheel is the canvas' own input: the notches are taken here, on every
     * frame the canvas is up, so a scroll outside it is dropped rather than
     * waiting for the next picture. */
    float wheel = chrome_take_wheel();

    /* Brush size: [ shrinks, ] grows. Read before the canvas test so the keys
     * work wherever the pointer is, as they do in the web UI. */
    if (cui_key_pressed(ui, CUI_KEY_LEFT_BRACKET)) brush_step(st, BRUSH_STEP);
    if (cui_key_pressed(ui, CUI_KEY_RIGHT_BRACKET)) brush_step(st, 1.0f / BRUSH_STEP);

    if (!inside) {
        if (st->painting) {
            mask_stroke_end(&j->m, j->hist);
            st->painting = false;
            request_composite(st, j);
        }
        if (!cui_mouse_down(ui, CUI_MOUSE_LEFT)) st->dragging_split = false;
        return;
    }

    /* a divider drag lives only as long as the button: releasing it ends the
     * drag, whatever tool the user switches to meanwhile */
    if (!cui_mouse_down(ui, CUI_MOUSE_LEFT)) st->dragging_split = false;

    /* Ctrl+wheel zooms around the image point under the cursor (a plain wheel
     * keeps its usual meaning: nothing on the stage scrolls) */
    if (wheel != 0.0f && (cui_key_mods(ui) & CUI_MOD_CTRL))
        stage_zoom_at(j, r, m.x, m.y, powf(1.1f, wheel));

    float ix = 0.0f, iy = 0.0f;
    screen_to_img(j, r, m.x, m.y, &ix, &iy);

    bool tool_eraser = st->tool == TOOL_ERASE;

    if (st->tool == TOOL_BRUSH || st->tool == TOOL_ERASE) {
        mask_brush b;
        b.diameter = st->brush_size;
        int paint_value = (int)(st->brush_mix * 2.55f + 0.5f);

        /* the erase flag belongs to the stroke, not to the frame: a right-button
         * drag stays an eraser for its whole length even though only the press
         * frame sees the button go down */
        static bool stroke_erase;

        /* a fast click can go down and up inside one frame, so also accept the
         * click event CUI already tracks */
        bool left_press = mouse_pressed(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
        bool right_press = mouse_pressed(ui, CUI_MOUSE_RIGHT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT);
        if (left_press || right_press) {
            stroke_erase = tool_eraser || right_press;
            b.value = stroke_erase ? 0 : paint_value;
            mask_stroke_begin(&j->m, j->hist);
            mask_stroke_dot(&j->m, j->hist, ix, iy, &b);
            st->painting = true;
            st->stroke_open = true;
            st->paint_x = ix;
            st->paint_y = iy;
            mask_note(st, j);
        } else if (st->painting && (cui_mouse_down(ui, CUI_MOUSE_LEFT) || cui_mouse_down(ui, CUI_MOUSE_RIGHT))) {
            b.value = stroke_erase ? 0 : paint_value;
            if (fabsf(ix - st->paint_x) + fabsf(iy - st->paint_y) > 0.5f) {
                mask_stroke_segment(&j->m, j->hist, st->paint_x, st->paint_y, ix, iy, &b);
                st->paint_x = ix;
                st->paint_y = iy;
                mask_note(st, j);
            }
        } else if (st->painting) {
            mask_stroke_end(&j->m, j->hist);
            st->painting = false;
            st->stroke_open = false;
            j->composite_pending = true;
        }
        return;
    }

    if (st->tool == TOOL_LASSO) {
        ensure_edges(j);

        bool alt = (cui_key_mods(ui) & CUI_MOD_ALT) != 0;
        bool left_press = mouse_pressed(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
        bool right_press = mouse_pressed(ui, CUI_MOUSE_RIGHT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT);
        bool near = lasso_near_start(st, j, ix, iy);

        /* two clicks in the same spot in quick succession are the other way to
         * say "close it" */
        static double prev_click_at;
        static float prev_click_x, prev_click_y;
        bool dbl = false;
        if (left_press) {
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
            lasso_close_now(st, j, alt);
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
        if (!near && cui_mouse_down(ui, CUI_MOUSE_LEFT) && hypotf(sx - lx, sy - ly) * z >= LASSO_ANCHOR_GAP) {
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
        return;
    }

    /* view tool: pan with any drag */
    if (mouse_pressed(ui, CUI_MOUSE_LEFT) || mouse_pressed(ui, CUI_MOUSE_MIDDLE) ||
        (st->space_down && cui_mouse_down(ui, CUI_MOUSE_LEFT))) {
        st->dragging_split = false;
        float split_x = r.x + j->split * r.w;
        if (fabsf(m.x - split_x) <= STAGE_SPLIT_HIT && st->view == VIEW_COMPARE) {
            st->dragging_split = true;
        }
    }
    static float last_x = 0.0f, last_y = 0.0f;
    static bool panning = false;
    if ((cui_mouse_down(ui, CUI_MOUSE_LEFT) && !st->dragging_split) || cui_mouse_down(ui, CUI_MOUSE_MIDDLE)) {
        if (panning) {
            float dx = m.x - last_x, dy = m.y - last_y;
            j->view_fitted = false;
            if (j->view.zoom > 0.0f) {
                j->view.cx -= dx / j->view.zoom;
                j->view.cy -= dy / j->view.zoom;
            }
        }
        panning = true;
        last_x = m.x;
        last_y = m.y;
    } else {
        panning = false;
    }
    if (st->dragging_split) {
        float t = (m.x - r.x) / (r.w > 1.0f ? r.w : 1.0f);
        j->split = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
}

/* ------------------------------------------------------------------------ */
/* Stage drawing                                                            */
/* ------------------------------------------------------------------------ */

/* The canvas floor: a quiet surface with a barely-there grid. The artwork must
 * be the strongest contrast on screen (docs/visual-design.md §6.3). */
/* Small pill used for the before/after labels and the zoom hint. */
static void draw_chip(cui_ctx *ui, float x, float y, const char *text) {
    cui_text_metrics m = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION, text, -1);
    float w = m.width + 16.0f, h = 20.0f;
    cui_draw_rect(ui, (cui_rect){ x, y, w, h }, CUI_CORNERS(6.0f), CUI_RGBA(0x161412a6));
    cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION,
                  (cui_vec2){ x + 8.0f, y + (h - m.height) * 0.5f + m.ascent * 0.0f },
                  CUI_RGBA(0xffffffff), text, -1);
}

static void draw_canvas_floor(cui_ctx *ui, cui_rect r) {
    cui_draw_rect(ui, r, CUI_CORNERS(UI_R_TILE), CUI_COLOR_SURFACE_MUTED);
    const float cell = 24.0f;
    bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
    cui_color check = dark ? CUI_RGBA(0xffffff0a) : CUI_RGBA(0x00000008);
    for (float y = r.y, row = 0; y < r.y + r.h; y += cell, row++) {
        for (float x = r.x + ((int)row & 1 ? cell : 0.0f); x < r.x + r.w; x += cell * 2) {
            float w = cell, h = cell;
            if (x + w > r.x + r.w) w = r.x + r.w - x;
            if (y + h > r.y + r.h) h = r.y + r.h - y;
            if (w > 0.0f && h > 0.0f)
                cui_draw_rect(ui, (cui_rect){ x, y, w, h }, CUI_CORNERS(0), check);
        }
    }
}

typedef struct stage_ctx { app_state *st; } stage_ctx;

static void stage_paint(cui_ctx *ui, cui_rect r, void *user) {
    app_state *st = ((stage_ctx *)user)->st;
    /* the paint pass knows the final rect: input uses it on the next frame */
    if (r.w > 1.0f && r.h > 1.0f) {
        st->stage = r;
        st->stage_valid = true;
    }
    job *j = job_selected(st);
    if (!j) return;
    /* A new picture starts from a fitted view: the zoom and split of the
     * previous one (possibly fitted to another canvas) mean nothing here. */
    static char painted_path[PATH_CAP];
    if (strcmp(painted_path, j->path) != 0) {
        snprintf(painted_path, sizeof painted_path, "%s", j->path);
        j->view = (view2d){ 0.0f, 0.0f, 0.0f };
        j->split = 0.5f;
    }
    /* fits a never-shown image and re-fits a fitted view when the canvas
     * changed size; running it here (not just on selection) keeps a window
     * resize from leaving the image at the old zoom */
    stage_ensure_view(j, r);

    draw_canvas_floor(ui, r);

    float z = j->view.zoom;
    float iw = image_w(j), ih = image_h(j);
    float x0, y0;
    img_to_screen(j, r, 0.0f, 0.0f, &x0, &y0);
    cui_rect img = { x0, y0, iw * z, ih * z };

    cui_push_clip(ui, r);

    /* one shadow, only under the artwork: it is the only "floating" object */
    {
        bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
        cui_draw_shadow(ui, img, CUI_CORNERS(2.0f), 18.0f, 0.0f, (cui_vec2){ 0.0f, 4.0f },
                        dark ? CUI_RGBA(0x000000a0) : CUI_RGBA(0x1d1b1826));
    }

    view_mode mode = st->view;
    if (st->peek_orig) mode = VIEW_ORIGINAL;

    if (mode == VIEW_ORIGINAL || !j->has_result) {
        if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
    } else if (mode == VIEW_RESULT) {
        if (j->res_tex) cui_draw_image(ui, j->res_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
        else if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
    } else {
        /* compare: original left, result right */
        if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
        if (j->res_tex) {
            float split_x = r.x + j->split * r.w;
            if (split_x < r.x + r.w) {
                cui_rect clip = { split_x, r.y, r.x + r.w - split_x, r.h };
                cui_push_clip(ui, clip);
                cui_draw_image(ui, j->res_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
                cui_pop_clip(ui);
            }
            /* divider: the handle grows and thickens while the band is hot or
             * being dragged, and a full-height accent line marks an active drag */
            float line_x = split_x;
            cui_vec2 hc = { line_x, r.y + r.h * 0.5f };
            cui_vec2 mp = cui_mouse_pos(ui);
            bool over = mp.x >= r.x && mp.x < r.x + r.w && mp.y >= r.y && mp.y < r.y + r.h;
            bool hot = st->dragging_split || (over && fabsf(mp.x - line_x) <= STAGE_SPLIT_HIT);
            if (st->dragging_split) {
                cui_draw_rect(ui, (cui_rect){ line_x - 1.5f, r.y, 3.0f, r.h }, CUI_CORNERS(0),
                              cui_alpha(cui_token_color(ui, CUI_TOKEN_ACCENT), 0.55f));
            }
            cui_draw_rect(ui, (cui_rect){ line_x - 1.0f, r.y, 2.0f, r.h }, CUI_CORNERS(0), CUI_RGBA(0xfffffff2));
            float hr = hot ? STAGE_HANDLE_R_HOT : STAGE_HANDLE_R;
            float hring = hot ? STAGE_HANDLE_RING_HOT : STAGE_HANDLE_RING;
            cui_draw_circle(ui, hc, hr, CUI_RGBA(0xfffffffa));
            cui_draw_circle(ui, hc, hr - hring, CUI_RGBA(0x1d1b1814));
        }
    }

    /* protection overlay */
    if ((st->peek_mask || st->tool != TOOL_VIEW) && j->mask_tex) {
        cui_draw_image(ui, j->mask_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0),
                       CUI_RGBA(0xffffffff));
    }

    /* lasso preview */
    if (st->lasso_active && st->lasso_n >= 2) {
        for (int i = 0; i + 3 < st->lasso_n; i += 2) {
            float ax, ay, bx, by;
            img_to_screen(j, r, st->lasso[i], st->lasso[i + 1], &ax, &ay);
            img_to_screen(j, r, st->lasso[i + 2], st->lasso[i + 3], &bx, &by);
            cui_draw_line(ui, (cui_vec2){ ax, ay }, (cui_vec2){ bx, by }, 3.0f, CUI_RGBA(0x00000066));
            cui_draw_line(ui, (cui_vec2){ ax, ay }, (cui_vec2){ bx, by }, 1.5f, CUI_RGBA(0xfffffff2));
        }
        for (int i = 2; i + 1 < st->lasso_n; i += 2) {
            float ax, ay;
            img_to_screen(j, r, st->lasso[i], st->lasso[i + 1], &ax, &ay);
            cui_draw_circle(ui, (cui_vec2){ ax, ay }, 4.5f, CUI_RGBA(0x00000066));
            cui_draw_circle(ui, (cui_vec2){ ax, ay }, 3.0f, CUI_RGBA(0xffffffff));
        }
        /* the first vertex is also the click target that closes the outline, so
         * it gets a bigger ring - and a thicker, coral one while a click there
         * would actually close */
        {
            cui_vec2 mp = cui_mouse_pos(ui);
            float hx, hy;
            screen_to_img(j, r, mp.x, mp.y, &hx, &hy);
            bool closable = lasso_near_start(st, j, hx, hy);
            float fx, fy;
            img_to_screen(j, r, st->lasso[0], st->lasso[1], &fx, &fy);
            float ring = closable ? 8.0f : 6.0f;
            cui_draw_circle(ui, (cui_vec2){ fx, fy }, ring, CUI_RGBA(0x00000080));
            cui_draw_circle(ui, (cui_vec2){ fx, fy }, ring - (closable ? 3.0f : 2.0f),
                            closable ? CUI_RGBA(0xe07a5fff) : CUI_RGBA(0xffffffff));
        }
        float ax, ay;
        img_to_screen(j, r, st->lasso[st->lasso_n - 2], st->lasso[st->lasso_n - 1], &ax, &ay);
        if (st->lasso_live_n > 1) {
            for (int i = 0; i + 3 < st->lasso_live_n * 2; i += 2) {
                float bx, by, cx2, cy2;
                img_to_screen(j, r, (float)st->lasso_live[i], (float)st->lasso_live[i + 1], &bx, &by);
                img_to_screen(j, r, (float)st->lasso_live[i + 2], (float)st->lasso_live[i + 3], &cx2, &cy2);
                cui_draw_line(ui, (cui_vec2){ bx, by }, (cui_vec2){ cx2, cy2 }, 3.0f, CUI_RGBA(0x00000066));
                cui_draw_line(ui, (cui_vec2){ bx, by }, (cui_vec2){ cx2, cy2 }, 1.4f, CUI_RGBA(0xe07a5ff2));
            }
        } else {
            img_to_screen(j, r, st->lasso_hover_x, st->lasso_hover_y, &ax, &ay);
            float lx, ly;
            img_to_screen(j, r, st->lasso[st->lasso_n - 2], st->lasso[st->lasso_n - 1], &lx, &ly);
            cui_draw_line(ui, (cui_vec2){ lx, ly }, (cui_vec2){ ax, ay }, 1.2f, CUI_RGBA(0xe07a5fe6));
        }
    }

    /* brush cursor */
    if ((st->tool == TOOL_BRUSH || st->tool == TOOL_ERASE)) {
        cui_vec2 m = cui_mouse_pos(ui);
        if (m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h) {
            /* the ring is the real coverage, brush_size * zoom / 2, but never
             * smaller than a grabbable dot: a 6 px brush at 10 % zoom would
             * otherwise be a 0.3 px speck */
            float rad = st->brush_size * z * 0.5f;
            if (rad < STAGE_BRUSH_MIN) rad = STAGE_BRUSH_MIN;
            cui_draw_circle(ui, m, rad, CUI_RGBA(0x00000059));
            cui_draw_circle(ui, m, rad - 1.0f, CUI_RGBA(0xffffffe6));
            /* one screen pixel: where the click actually lands */
            cui_draw_rect(ui, (cui_rect){ floorf(m.x), floorf(m.y), 1.0f, 1.0f }, CUI_CORNERS(0),
                          cui_token_color(ui, CUI_TOKEN_ACCENT));
            if (z < STAGE_BRUSH_HINT_ZOOM) {
                const char *hint = "缩放以精确涂抹";
                cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, hint, -1);
                float tx = m.x + rad + UI_GAP_SM;
                if (tx + tm.width > r.x + r.w - UI_GAP) tx = m.x - rad - UI_GAP_SM - tm.width;
                if (tx < r.x + UI_GAP) tx = r.x + UI_GAP;
                cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                              (cui_vec2){ tx, m.y - tm.line_height * 0.5f },
                              cui_token_color(ui, CUI_TOKEN_TEXT_MUTED), hint, -1);
            }
        }
    }

    cui_pop_clip(ui);
}

void ui_runtime_panel(cui_ctx *ui, app_state *st, float height) {
    float h = height < 400.0f ? height : 400.0f;
    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(height), .align = CUI_ALIGN_CENTER,
            .justify = CUI_JUSTIFY_CENTER) {
        cui_card_opts co;
        memset(&co, 0, sizeof co);
        co.id = "runtime_card";
        co.width = CUI_PX(760);
        co.height = CUI_PX(h);
        co.pad = UI_PAD;
        co.gap = UI_GAP;
        co.align = CUI_ALIGN_CENTER;
        co.dir = CUI_COLUMN;
        cui_card_begin(ui, &co);
        cui_icon(ui, ICON_HARD_DRIVE, UI_ICON_LG, CUI_COLOR_ACCENT);
        cui_text(ui, "首次使用：安装 AI 运行环境", .font = CUI_FONT_DISPLAY, .size = UI_FS_BRAND);
        cui_text(ui, "去噪需要 PyTorch 与模型权重，安装包本身只有几 MB，这里按需下载"
                     "（约 2 GB，只需一次）。",
                 .size = UI_FS_BODY, .color = CUI_COLOR_TEXT_SECONDARY, .align = CUI_TEXT_CENTER);
        cui_text(ui, st->rt.device_note, .size = UI_FS_BODY, .color = CUI_COLOR_TEXT_MUTED);

        if (st->rt.state == RT_INSTALLING) {
            char pct[48];
            snprintf(pct, sizeof pct, "%s  %d%%", st->rt.stage, (int)(st->rt.progress * 100.0f + 0.5f));
            cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
                cui_spinner(ui, UI_ICON_SM);
                cui_text(ui, pct, .size = UI_FS_BODY);
            }
            cui_progress(ui, st->rt.progress, .width = CUI_FILL, .height = 6, .tone = CUI_TONE_ACCENT);
            if (st->rt.log_tail[0]) {
                cui_scroll(ui, .id = "rt_log", .height = CUI_PX(96), .width = CUI_FILL) {
                    cui_code(ui, st->rt.log_tail);
                }
            }
            if (cui_button(ui, "取消安装", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) {
                rt_cancel();
            }
        } else if (st->rt.state == RT_FAILED) {
            cui_text(ui, st->rt.error, .size = UI_FS_BODY, .color = CUI_COLOR_DANGER);
            if (st->rt.log_tail[0]) {
                cui_scroll(ui, .id = "rt_log", .height = CUI_PX(120), .width = CUI_FILL) {
                    cui_code(ui, st->rt.log_tail);
                }
            }
            if (cui_button(ui, "重试安装", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_DOWNLOAD)) {
                rt_install();
            }
        } else {
            if (cui_button(ui, "开始安装", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_DOWNLOAD)) {
                rt_install();
            }
            cui_text(ui, "安装位置：" APP_ID " 的数据目录，可随时在设置里查看。",
                     .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_card_end(ui);
    }
}

void draw_stage_area(cui_ctx *ui, app_state *st, float height) {
    job *j = job_selected(st);
    bool panel = !st->bench && (st->rt.state == RT_INSTALLING || st->rt.state == RT_FAILED ||
                 (st->rt.state == RT_MISSING && !st->worker_up));
    /* Nothing on the stage can zoom this frame (the installer panel or the drop
     * zone is showing), so the wheel's notches are dropped here rather than
     * landing on the next picture the user opens. */
    if (panel || !j || !st->stage_valid) (void)chrome_take_wheel();

    if (panel) {
        ui_runtime_panel(ui, st, height);
        return;
    }
    if (!j) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(height), .align = CUI_ALIGN_CENTER,
                .justify = CUI_JUSTIFY_CENTER) {
            cui_column(ui, .width = CUI_FILL, .max_width = 760, .gap = UI_GAP) {
                int r = cui_dropzone(ui, "main_drop",
                                     .title = "把图片拖到这里",
                                     .description = "或点击选择文件 · PNG JPG WebP BMP TIFF AVIF · 也可以 Ctrl+V 粘贴",
                                     .accept = IMAGE_FILTER,
                                     .icon = CUI_ICON_IMAGE,
                                     .height = height - 40.0f > 160.0f ? height - 40.0f : 160.0f);
                if (r < 0) st->want_browse = true;
            }
        }
        return;
    }

    stage_ctx sc = { st };
    cui_canvas_result res = cui_canvas(ui, "stage", CUI_FILL, CUI_PX(height), stage_paint, &sc, sizeof sc);
    if (res.rect.w > 1.0f && res.rect.h > 1.0f) {
        st->stage = res.rect;
        st->stage_valid = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Toolbar above the canvas                                                 */
/* ------------------------------------------------------------------------ */

void ui_stage_toolbar(cui_ctx *ui, app_state *st, job *j) {
    cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER, .height = CUI_PX(UI_CONTROL_H)) {
        static const char *views[3] = { "对比", "原图", "结果" };
        int v = (int)st->view;
        if (cui_segmented(ui, "view", views, 3, &v, .size = CUI_SIZE_SM)) st->view = (view_mode)v;
        st->r_view = cui_last_rect(ui);

        cui_spacer(ui);

        static const char *tools[3] = { "笔刷", "橡皮", "套索" };
        int t = (int)st->tool - 1;   /* -1 when TOOL_VIEW */
        for (int i = 0; i < 3; i++) {
            const char *icon = i == 0 ? ICON_BRUSH : (i == 1 ? ICON_ERASER : ICON_LASSO);
            bool on = st->tool == (tool_mode)(i + 1);
            bool hit_tool = cui_button(ui, tools[i], .variant = on ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST,
                                       .size = CUI_SIZE_SM, .icon = icon,
                                       .shortcut = i == 0 ? "B" : (i == 1 ? "E" : "L"));
            st->r_tool[i] = cui_last_rect(ui);
            if (hit_tool) {
                st->tool = on ? TOOL_VIEW : (tool_mode)(i + 1);
                if (st->tool == TOOL_LASSO) ensure_edges(j);
                if (st->tool == TOOL_VIEW) lasso_reset(st);
            }
        }

        cui_separator(ui);

        bool hit_undo = cui_icon_button(ui, CUI_ICON_UNDO_2, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "撤销（Ctrl+Z）", .disabled = !mask_can_undo(j->hist));
        st->r_undo = cui_last_rect(ui);
        if (hit_undo) {
            if (mask_undo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
        }
        bool hit_redo = cui_icon_button(ui, CUI_ICON_REDO_2, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "重做（Ctrl+Shift+Z）", .disabled = !mask_can_redo(j->hist));
        st->r_redo = cui_last_rect(ui);
        if (hit_redo) {
            if (mask_redo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
        }
        if (cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "清除全部保护", .disabled = !j->mask_active)) {
            mask_stroke_begin(&j->m, j->hist);
            mask_clear(&j->m);
            mask_stroke_end(&j->m, j->hist);
            mask_note(st, j);
            j->composite_pending = true;
        }
        (void)t;

        cui_spacer(ui);

        char zoom_label[32];
        snprintf(zoom_label, sizeof zoom_label, "%d%%", (int)(j->view.zoom * 100.0f + 0.5f));
        if (cui_icon_button(ui, CUI_ICON_ZOOM_OUT, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "缩小（Ctrl+-）")) {
            j->view_fitted = false;
            j->view.zoom = j->view.zoom / 1.25f < STAGE_ZOOM_MIN ? STAGE_ZOOM_MIN : j->view.zoom / 1.25f;
        }
        cui_text(ui, zoom_label, .size = UI_FS_BODY, .nowrap = true, .tabular = true, .color = CUI_COLOR_TEXT_SECONDARY);
        if (cui_icon_button(ui, CUI_ICON_ZOOM_IN, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "放大（Ctrl+=）")) {
            j->view_fitted = false;
            j->view.zoom = j->view.zoom * 1.25f > STAGE_ZOOM_MAX ? STAGE_ZOOM_MAX : j->view.zoom * 1.25f;
        }
        if (cui_icon_button(ui, CUI_ICON_MAXIMIZE_2, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "适合窗口（F）")) {
            stage_fit(j, st->stage);
        }
        if (cui_button(ui, "1:1", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM, .shortcut = "0")) {
            j->view.zoom = 1.0f;
            j->view.cx = image_w(j) * 0.5f;
            j->view.cy = image_h(j) * 0.5f;
            j->view_fitted = false;
        }
    }
}

/* ------------------------------------------------------------------------ */
