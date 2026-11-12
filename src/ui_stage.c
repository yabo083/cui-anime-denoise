/* ui_stage.c -- the canvas: the only always-on surface of the Fluent layout
 * (docs/fluent-redesign.md). Draws the image, the compare split, the mask
 * overlay, the brush cursor and the small corner chips; the lasso lives in
 * ui_lasso.c, the floating pill/tray/toolbars in ui_float.c. Input keeps the
 * web UI's feel: wheel zooms at the cursor, space/middle pan, the split is a
 * 2px line that snaps to an edge within 24px. */

#include "app.h"
#include "chrome.h"
#include "icons.h"
#include "ui_fluent.h"
#include "ui_tokens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stage geometry                                                           */
/* ------------------------------------------------------------------------ */

#define STAGE_FIT_PAD       (UI_S24 * 1.0f)  /* breathing room around a fitted image */
#define STAGE_SPLIT_HIT      8.0f            /* half-width of the divider grab band (spec §7) */
#define STAGE_SPLIT_SNAP    24.0f            /* release within this of an edge snaps to it */
#define STAGE_HANDLE_R      14.0f            /* divider handle radius, idle (28px circle) */
#define STAGE_HANDLE_R_HOT  15.4f            /* +10% while hot */
#define STAGE_BRUSH_MIN       3.0f           /* brush cursor radius floor, screen px */
#define STAGE_BRUSH_HINT_ZOOM 0.2f           /* below this the cursor is not the coverage */
#define STAGE_ZOOM_MIN        0.02f          /* view zoom range, screen points per image px */
#define STAGE_ZOOM_MAX        16.0f

/* The stage measures in image pixels, the space the mask, the edge map, the
 * brush and the engine all use. Above DISPLAY_MAX the picture we draw is only
 * a downscaled texture, so its size is NOT that space: measuring the brush,
 * the lasso and the view in display pixels put a stroke's mask pixels a factor
 * (mask/display) away from the cursor for every large image. */
float stage_image_w(const job *j) {
    if (j->m.w > 0) return (float)j->m.w;
    if (j->orig.w > 0) return (float)j->orig.w;
    return (float)j->disp_w;
}
float stage_image_h(const job *j) {
    if (j->m.h > 0) return (float)j->m.h;
    if (j->orig.h > 0) return (float)j->orig.h;
    return (float)j->disp_h;
}

void stage_fit(job *j, cui_rect r) {
    float iw = stage_image_w(j), ih = stage_image_h(j);
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
void stage_img_to_screen(const job *j, cui_rect r, float ix, float iy, float *sx, float *sy) {
    float z = j->view.zoom;
    *sx = r.x + r.w * 0.5f + (ix - j->view.cx) * z;
    *sy = r.y + r.h * 0.5f + (iy - j->view.cy) * z;
}

void stage_screen_to_img(const job *j, cui_rect r, float sx, float sy, float *ix, float *iy) {
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
void stage_zoom_at(job *j, cui_rect r, float sx, float sy, float factor) {
    float z = j->view.zoom;
    if (z <= 0.0f || !(factor > 0.0f)) return;
    float nz = z * factor;
    if (nz < STAGE_ZOOM_MIN) nz = STAGE_ZOOM_MIN;
    if (nz > STAGE_ZOOM_MAX) nz = STAGE_ZOOM_MAX;
    if (nz == z) return;
    float ix, iy;
    stage_screen_to_img(j, r, sx, sy, &ix, &iy);
    j->view.zoom = nz;
    j->view.cx = ix - (sx - (r.x + r.w * 0.5f)) / nz;
    j->view.cy = iy - (sy - (r.y + r.h * 0.5f)) / nz;
    j->view_fitted = false;
}

void stage_zoom_center(app_state *st, job *j, float factor) {
    if (!j || j->view.zoom <= 0.0f) return;
    cui_rect r = st->stage;
    stage_zoom_at(j, r, r.x + r.w * 0.5f, r.y + r.h * 0.5f, factor);
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

/* A modal gates stage_handle_input, but these edge statics must still track
 * the button, else the press that opened the modal leaves them armed and the
 * first canvas gesture after it closes loses its press edge (the compare
 * divider pans instead of grabbing, a brush dot lands on release). */
void stage_track_buttons(cui_ctx *ui) {
    (void)mouse_pressed(ui, CUI_MOUSE_LEFT);
    (void)mouse_pressed(ui, CUI_MOUSE_RIGHT);
    (void)mouse_pressed(ui, CUI_MOUSE_MIDDLE);
}

void mask_note(app_state *st, job *j) {
    j->mask_active = mask_any(&j->m);
    j->mask_tex_dirty = true;
    j->composite_dirty = true;
    j->dirty_at = now_seconds();
    j->mask_fade_from = j->dirty_at;
    (void)st;
}

/* ------------------------------------------------------------------------ */
/* Stage input                                                              */
/* ------------------------------------------------------------------------ */

void stage_handle_input(app_state *st, job *j) {
    cui_ctx *ui = st->ui;
    cui_rect r = st->stage;
    cui_vec2 m = cui_mouse_pos(ui);
    bool inside = m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;

    /* The wheel is the canvas' own input: the notches are taken here, on every
     * frame the canvas is up, so a scroll outside it is dropped rather than
     * waiting for the next picture. Over the filmstrip it pages the tray. */
    float wheel = chrome_take_wheel();
    if (wheel != 0.0f && ui_hit(st->r_tray, m)) {
        ui_tray_page(st, wheel > 0.0f ? -1 : +1);
        wheel = 0.0f;
    }

    /* [ shrinks, ] grows the brush. Read before the canvas test so the keys
     * work wherever the pointer is, as they do in the web UI. */
    if (st->protect) {
        if (cui_key_pressed(ui, CUI_KEY_LEFT_BRACKET)) brush_step(st, BRUSH_STEP);
        if (cui_key_pressed(ui, CUI_KEY_RIGHT_BRACKET)) brush_step(st, 1.0f / BRUSH_STEP);
    }

    if (!inside) {
        if (st->painting) {
            mask_stroke_end(&j->m, j->hist);
            st->painting = false;
            st->stroke_open = false;
            j->composite_pending = true;
        }
        if (!cui_mouse_down(ui, CUI_MOUSE_LEFT)) st->dragging_split = false;
        return;
    }

    /* a divider drag lives only as long as the button: releasing it ends the
     * drag, whatever tool the user switches to meanwhile; near an edge the
     * line snaps to it instead of leaving a 1% sliver (spec §7) */
    if (!cui_mouse_down(ui, CUI_MOUSE_LEFT) && st->dragging_split) {
        st->dragging_split = false;
        float px = j->split * r.w;
        if (px < STAGE_SPLIT_SNAP) j->split = 0.0f;
        else if (px > r.w - STAGE_SPLIT_SNAP) j->split = 1.0f;
    }

    /* wheel zooms around the image point under the cursor (Ctrl optional);
     * in protection mode space+wheel sizes the brush instead (spec §8) */
    if (wheel != 0.0f) {
        if (st->protect && st->space_down) {
            brush_step(st, wheel > 0.0f ? 1.0f / BRUSH_STEP : BRUSH_STEP);
        } else {
            stage_zoom_at(j, r, m.x, m.y, powf(1.1f, wheel));
        }
    }

    float ix = 0.0f, iy = 0.0f;
    stage_screen_to_img(j, r, m.x, m.y, &ix, &iy);

    bool tool_paints = st->protect && (st->tool == TOOL_BRUSH || st->tool == TOOL_ERASE);
    bool tool_lasso = st->protect && st->tool == TOOL_LASSO;

    /* space + left drag pans in every mode, even while a paint tool is on */
    bool space_pan = st->space_down && cui_mouse_down(ui, CUI_MOUSE_LEFT);

    if (tool_paints && !space_pan) {
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
            stroke_erase = st->tool == TOOL_ERASE || right_press;
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
            j->mask_fade_from = now_seconds();
        }
        return;
    }

    if (tool_lasso && !space_pan) {
        bool left_press = mouse_pressed(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
        bool right_press = mouse_pressed(ui, CUI_MOUSE_RIGHT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT);
        lasso_handle_input(st, j, ix, iy, left_press, right_press);
        /* pan still works with the middle button while the lasso owns left */
        if (!cui_mouse_down(ui, CUI_MOUSE_MIDDLE)) return;
    }

    /* view/pan: any plain left drag pans, space and middle always do; a grab
     * on the split band is the compare divider, not a pan */
    static float last_x = 0.0f, last_y = 0.0f;
    static bool panning = false;
    bool press = mouse_pressed(ui, CUI_MOUSE_LEFT) || mouse_pressed(ui, CUI_MOUSE_MIDDLE) ||
                 (st->space_down && cui_mouse_down(ui, CUI_MOUSE_LEFT));
    if (press) {
        float split_x = r.x + j->split * r.w;
        st->dragging_split = st->view == VIEW_COMPARE && j->has_result &&
                             fabsf(m.x - split_x) <= STAGE_SPLIT_HIT && !st->space_down;
    }
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

/* One separable box pass over RGBA pixels: horizontal into tmp, vertical back.
 * Edge samples clamp, so the window width is always 2r+1. */
static void bg_box_pass(unsigned char *px, unsigned char *tmp, int w, int h, int r) {
    const int n = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        unsigned char *row = px + (size_t)y * w * 4, *dst = tmp + (size_t)y * w * 4;
        int a = 0, rr = 0, g = 0, b = 0;
        for (int x = -r; x <= r; x++) {
            int c = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
            rr += row[c*4]; g += row[c*4+1]; b += row[c*4+2]; a += row[c*4+3];
        }
        for (int x = 0; x < w; x++) {
            dst[x*4] = (unsigned char)(rr / n); dst[x*4+1] = (unsigned char)(g / n);
            dst[x*4+2] = (unsigned char)(b / n); dst[x*4+3] = (unsigned char)(a / n);
            int sub = x - r < 0 ? 0 : x - r, add = x + r + 1 > w - 1 ? w - 1 : x + r + 1;
            rr += row[add*4] - row[sub*4]; g += row[add*4+1] - row[sub*4+1];
            b += row[add*4+2] - row[sub*4+2]; a += row[add*4+3] - row[sub*4+3];
        }
    }
    for (int x = 0; x < w; x++) {
        int a = 0, rr = 0, g = 0, b = 0;
        for (int y = -r; y <= r; y++) {
            int c = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
            const unsigned char *s = tmp + ((size_t)c * w + x) * 4;
            rr += s[0]; g += s[1]; b += s[2]; a += s[3];
        }
        for (int y = 0; y < h; y++) {
            unsigned char *d = px + ((size_t)y * w + x) * 4;
            d[0] = (unsigned char)(rr / n); d[1] = (unsigned char)(g / n);
            d[2] = (unsigned char)(b / n); d[3] = (unsigned char)(a / n);
            int sub = y - r < 0 ? 0 : y - r, add = y + r + 1 > h - 1 ? h - 1 : y + r + 1;
            const unsigned char *sa = tmp + ((size_t)add * w + x) * 4;
            const unsigned char *ss = tmp + ((size_t)sub * w + x) * 4;
            rr += sa[0] - ss[0]; g += sa[1] - ss[1]; b += sa[2] - ss[2]; a += sa[3] - ss[3];
        }
    }
}

/* The acrylic floor texture: the result point-sampled to ~160px and softened
 * by six box passes (≈gaussian). Built once per result; the few thousand
 * reads stay well inside a frame budget. */
static void bg_build(cui_ctx *ui, job *j) {
    if (j->bg_tex) { cui_image_destroy(ui, j->bg_tex); j->bg_tex = 0; }
    j->bg_dirty = false;
    if (!j->res.rgb || j->res.w < 2 || j->res.h < 2) return;
    int sw = j->res.w > 160 ? 160 : j->res.w;
    int sh = (int)((long long)j->res.h * sw / j->res.w);
    if (sh < 1) sh = 1;
    unsigned char *px = (unsigned char *)malloc((size_t)sw * sh * 4);
    unsigned char *tmp = (unsigned char *)malloc((size_t)sw * sh * 4);
    if (!px || !tmp) { free(px); free(tmp); return; }
    for (int y = 0; y < sh; y++) {
        const uint8_t *src = j->res.rgb + ((long long)y * j->res.h / sh) * (size_t)j->res.w * 3;
        unsigned char *dst = px + (size_t)y * sw * 4;
        for (int x = 0; x < sw; x++) {
            const uint8_t *s = src + ((long long)x * j->res.w / sw) * 3;
            dst[x*4] = s[0]; dst[x*4+1] = s[1]; dst[x*4+2] = s[2]; dst[x*4+3] = 255;
        }
    }
    for (int i = 0; i < 6; i++) bg_box_pass(px, tmp, sw, sh, 4);
    j->bg_tex = cui_image_create(ui, sw, sh, px);
    if (j->bg_tex) { j->bg_w = sw; j->bg_h = sh; }
    free(px); free(tmp);
}

/* The canvas floor. Acrylic mode (the default) stretches a heavily blurred
 * copy of the most recent output under a theme-tinted veil — the WinUI mica
 * idea — and falls back to the solid token colour with a barely-there
 * checker so transparency is readable (spec: 对比 ≤4%). */
static void draw_canvas_floor(cui_ctx *ui, app_state *st, job *j, cui_rect r) {
    bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
    if (st->set.canvas_bg) {
        job *src = (j && j->res.rgb) ? j : NULL;
        if (!src) {
            for (int i = st->count - 1; i >= 0; i--) {
                if (st->jobs[i].has_result && st->jobs[i].res.rgb) { src = &st->jobs[i]; break; }
            }
        }
        if (src) {
            if (src->bg_dirty || !src->bg_tex) bg_build(ui, src);
            if (src->bg_tex) {
                float ia = (float)src->bg_w / (float)src->bg_h, ra = r.w / r.h;
                float uw = 1.0f, uh = 1.0f;
                if (ia > ra) uw = ra / ia; else uh = ia / ra;
                cui_rect uv = { (1.0f - uw) * 0.5f, (1.0f - uh) * 0.5f, uw, uh };
                cui_draw_image(ui, src->bg_tex, r, uv, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
                cui_draw_rect(ui, r, CUI_CORNERS(0),
                              dark ? CUI_RGBA(0x181818cc) : CUI_RGBA(0xf0f0f0cc));
                return;
            }
        }
    }
    cui_draw_rect(ui, r, CUI_CORNERS(0), dark ? CUI_RGBA(0x181818ff) : CUI_RGBA(0xE8E8E8ff));
    const float cell = 24.0f;
    cui_color check = dark ? CUI_RGBA(0xffffff08) : CUI_RGBA(0x00000007);
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

/* Small chip: black 55%, white 11px text, 6px radius (the compare labels and
 * the info chips in the corners, spec §2.3). */
static void draw_chip(cui_ctx *ui, float x, float y, const char *text) {
    cui_text_metrics m = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION, text, -1);
    float w = m.width + 14.0f, h = 20.0f;
    cui_draw_rect(ui, (cui_rect){ x, y, w, h }, CUI_CORNERS(UI_R_MD), CUI_RGBA(0x0000008c));
    cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION,
                  (cui_vec2){ x + 7.0f, y + (h - m.height) * 0.5f },
                  CUI_RGBA(0xffffffff), text, -1);
}

/* The texture that should appear on the "result" side right now: the masked
 * composite normally, the unprotected render when the protect bar asks for it,
 * with a 120ms cross-fade between replacements (spec §6). */
static void draw_result_tex(cui_ctx *ui, app_state *st, job *j, cui_rect img) {
    cui_texture tex = st->view_unprotected && j->raw_tex ? j->raw_tex : j->res_tex;
    double now = now_seconds();
    float ft = (float)((now - j->fade_at) / UI_T_RESULT);
    if (j->fade_tex && ft >= 0.0f && ft < 1.0f && !st->view_unprotected) {
        cui_draw_image(ui, j->fade_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0),
                       cui_alpha(CUI_RGBA(0xffffffff), 1.0f - ft));
        cui_draw_image(ui, tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0),
                       cui_alpha(CUI_RGBA(0xffffffff), ft));
        cui_request_redraw(ui);
        return;
    }
    if (tex) cui_draw_image(ui, tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
    else if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
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
    draw_canvas_floor(ui, st, j, r);

    if (!j) {
        ui_empty_paint(ui, st, r);
        ui_float_paint(ui, st, r);   /* banner still floats over the empty state */
        return;
    }
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

    /* the unprotected preview is built lazily, once, at display size */
    if (st->view_unprotected && j->has_result && !j->raw_tex) {
        img_pixels px = {0};
        if (job_raw_result_px(j, &px)) {
            img_pixels prev = {0};
            if (img_thumbnail(&px, DISPLAY_MAX, &prev)) {
                j->raw_tex = cui_image_create(ui, prev.w, prev.h, prev.rgba);
                j->raw_w = prev.w;
                j->raw_h = prev.h;
                img_free(&prev);
            }
            img_free(&px);
        }
    }

    float z = j->view.zoom;
    float iw = stage_image_w(j), ih = stage_image_h(j);
    float x0, y0;
    stage_img_to_screen(j, r, 0.0f, 0.0f, &x0, &y0);
    cui_rect img = { x0, y0, iw * z, ih * z };

    cui_push_clip(ui, r);

    /* one shadow, only under the artwork: it is the only "floating" object */
    {
        bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
        cui_draw_shadow(ui, img, CUI_CORNERS(2.0f), 18.0f, 0.0f, (cui_vec2){ 0.0f, 4.0f },
                        dark ? CUI_RGBA(0x000000a0) : CUI_RGBA(0x00000026));
    }

    view_mode mode = st->view;
    if (st->peek_orig) mode = VIEW_ORIGINAL;
    if (!j->has_result && mode != VIEW_ORIGINAL) mode = VIEW_ORIGINAL;

    if (mode == VIEW_ORIGINAL) {
        if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
    } else if (mode == VIEW_RESULT) {
        draw_result_tex(ui, st, j, img);
    } else {
        /* compare: original left, result right */
        if (j->orig_tex) cui_draw_image(ui, j->orig_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), CUI_RGBA(0xffffffff));
        if (j->has_result) {
            float split_x = r.x + j->split * r.w;
            if (split_x < r.x + r.w) {
                cui_rect clip = { split_x, r.y, r.x + r.w - split_x, r.h };
                cui_push_clip(ui, clip);
                draw_result_tex(ui, st, j, img);
                cui_pop_clip(ui);
            }
            /* divider: 2px white at .85 with a soft shadow on both sides and a
             * 28px circular handle that grows 10% while hot (spec §2.3) */
            cui_vec2 hc = { split_x, r.y + r.h * 0.5f };
            cui_vec2 mp = cui_mouse_pos(ui);
            bool over = mp.x >= r.x && mp.x < r.x + r.w && mp.y >= r.y && mp.y < r.y + r.h;
            bool hot = st->dragging_split || (over && fabsf(mp.x - split_x) <= STAGE_SPLIT_HIT);
            cui_draw_shadow(ui, (cui_rect){ split_x - 2.0f, r.y, 4.0f, r.h }, CUI_CORNERS(0),
                            6.0f, 0.0f, (cui_vec2){ 0, 0 }, CUI_RGBA(0x00000040));
            cui_draw_rect(ui, (cui_rect){ split_x - 1.0f, r.y, 2.0f, r.h }, CUI_CORNERS(0), CUI_RGBA(0xffffffd9));
            float hr = hot ? STAGE_HANDLE_R_HOT : STAGE_HANDLE_R;
            cui_draw_shadow(ui, (cui_rect){ hc.x - hr, hc.y - hr, hr * 2, hr * 2 }, CUI_CORNERS(hr),
                            10.0f, 0.0f, (cui_vec2){ 0, 2.0f }, CUI_RGBA(0x00000059));
            cui_draw_circle(ui, hc, hr, CUI_RGBA(0xffffffff));
            cui_draw_icon(ui, ICON_ARROW_LEFT_RIGHT,
                          (cui_rect){ hc.x - 8, hc.y - 8, 16, 16 }, 16.0f, CUI_RGBA(0x5D5D5Dff));
            /* corner labels */
            draw_chip(ui, r.x + UI_S12, r.y + UI_S12, "原图");
            {
                cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION,
                                                     st->view_unprotected ? "未保护结果" : "结果", -1);
                draw_chip(ui, r.x + r.w - UI_S12 - (tm.width + 14.0f), r.y + UI_S12,
                          st->view_unprotected ? "未保护结果" : "结果");
            }
        }
    }

    /* protection overlay: pinned on by the eye toggle or an M hold, shown at
     * full strength while a stroke runs, then it fades over 0.7 s (spec §3).
     * Outside protection mode it stays hidden unless peeked. */
    if (j->mask_tex) {
        float alpha = 0.0f;
        if (st->mask_eye || st->peek_mask) alpha = 1.0f;
        else if (st->protect) {
            if (st->painting || st->lasso_active) alpha = 1.0f;
            else if (j->mask_fade_from > 0.0) {
                alpha = 1.0f - (float)((now_seconds() - j->mask_fade_from) / UI_T_MASKFADE);
                if (alpha < 0.0f) alpha = 0.0f;
                else cui_request_redraw(ui);   /* the fade keeps its 0.7 s cadence */
            }
        }
        if (alpha > 0.0f)
            cui_draw_image(ui, j->mask_tex, img, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0),
                           cui_alpha(CUI_RGBA(0xffffffff), alpha));
    }

    lasso_paint(ui, st, j, r);

    /* brush cursor: the ring is the real coverage, with a 1px centre cross;
     * never smaller than a grabbable dot (spec §7) */
    if (st->protect && (st->tool == TOOL_BRUSH || st->tool == TOOL_ERASE)) {
        cui_vec2 m = cui_mouse_pos(ui);
        if (m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h &&
            !ui_hit(st->r_stage_buttons, m)) {
            float rad = st->brush_size * z * 0.5f;
            if (rad < STAGE_BRUSH_MIN) rad = STAGE_BRUSH_MIN;
            bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
            cui_color ring = st->tool == TOOL_ERASE ? CUI_RGBA(0x00000066) : CUI_RGBA(0x00000059);
            cui_draw_circle(ui, m, rad, ring);
            cui_draw_circle(ui, m, rad - 1.0f,
                            st->tool == TOOL_ERASE ? CUI_RGBA(0xFF8080e6) : CUI_RGBA(0xffffffe6));
            cui_draw_rect(ui, (cui_rect){ floorf(m.x), floorf(m.y), 1.0f, 1.0f }, CUI_CORNERS(0),
                          cui_token_color(ui, CUI_TOKEN_ACCENT));
            if (z < STAGE_BRUSH_HINT_ZOOM) {
                const char *hint = "缩放以精确涂抹";
                cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, hint, -1);
                float tx = m.x + rad + UI_S8;
                if (tx + tm.width > r.x + r.w - UI_S8) tx = m.x - rad - UI_S8 - tm.width;
                if (tx < r.x + UI_S8) tx = r.x + UI_S8;
                cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                              (cui_vec2){ tx, m.y - tm.line_height * 0.5f },
                              dark ? CUI_RGBA(0xffffff99) : CUI_RGBA(0x00000099), hint, -1);
            }
        }
    }

    /* corner info chips: size + elapsed + params (spec §1, optional line) */
    {
        char buf[128];
        snprintf(buf, sizeof buf, "%d × %d", j->orig.w, j->orig.h);
        draw_chip(ui, r.x + UI_S12, r.y + r.h - UI_S12 - 20.0f, buf);
        if (j->status == JOB_DONE) {
            char when[24], l0[16], info[160];
            fmt_ms(j->ms, when, sizeof when);
            fmt_l0(j->res_l0, l0, sizeof l0);
            snprintf(info, sizeof info, "%s · 预缩 %d%% · L0 %s%s", when, j->res_down_pct, l0,
                     j->sr_cached ? " · 复用超分" : "");
            cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_CAPTION, info, -1);
            draw_chip(ui, r.x + r.w - UI_S12 - (tm.width + 14.0f), r.y + r.h - UI_S12 - 20.0f, info);
        }
    }

    /* processing state: a first run shows the centred ring + stage text; a
     * re-run keeps the previous result and only a thin bar rides the pill */
    if (j->status == JOB_RUNNING && !j->has_result) {
        cui_draw_rect(ui, r, CUI_CORNERS(0), CUI_RGBA(cui_get_theme_mode(ui) == CUI_THEME_DARK
                                                    ? 0x00000066 : 0xffffff66));
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f - 20.0f;
        fl_spinner(ui, (cui_vec2){ cx, cy }, 24.0f, 3.5f, cui_token_color(ui, CUI_TOKEN_ACCENT));
        const char *phase = j->phase[0] ? j->phase : "处理中";
        if (strcmp(phase, "upscale") == 0) phase = "超分辨率重建…";
        else if (strcmp(phase, "smooth") == 0) phase = "L0 平滑…";
        else if (strcmp(phase, "decode") == 0) phase = "解码…";
        else if (strcmp(phase, "prepare") == 0) phase = "准备模型…";
        else if (strcmp(phase, "compose") == 0) phase = "合成保护区域…";
        char label[96];
        if (j->progress > 0.02f && j->progress < 1.0f)
            snprintf(label, sizeof label, "%s %d%%", phase, (int)(j->progress * 100.0f + 0.5f));
        else snprintf(label, sizeof label, "%s", phase);
        cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_SUB, label, -1);
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_SUB,
                      (cui_vec2){ cx - tm.width * 0.5f, cy + 36.0f },
                      cui_token_color(ui, CUI_TOKEN_FOREGROUND), label, -1);
    }

    cui_pop_clip(ui);

    /* floating layer 1 + 2: banner, tray, pill, protect bar, tune card */
    ui_float_paint(ui, st, r);
}

void draw_stage_area(cui_ctx *ui, app_state *st, float height) {
    (void)height;
    stage_ctx sc = { st };
    cui_canvas_result res = cui_canvas(ui, "stage", CUI_FILL, CUI_FILL, stage_paint, &sc, sizeof sc);
    if (res.rect.w > 1.0f && res.rect.h > 1.0f) {
        st->stage = res.rect;
        st->stage_valid = true;
    }
}
