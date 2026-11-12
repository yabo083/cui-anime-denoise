/* ui_float.c -- the floating layers of the Fluent layout (docs/fluent-redesign.md):
 * the control pill, the filmstrip tray, the immersive protection toolbar, the
 * tune card, the engine banner, the empty state and the quick-rerun bar.
 *
 * CUI has no absolute-position layer, so these are drawn inside the stage
 * canvas (z-order: tray < pill < protect < tune) with the ui_fluent.h
 * primitives, and hit-tested by hand: the paint pass publishes rects into
 * app_state, and ui_overlay_input() reads last frame's rects before the
 * stage sees the pointer.
 */

#include "app.h"
#include "chrome.h"
#include "icons.h"
#include "ui_fluent.h"
#include "ui_tokens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frame-edge press of the left button, tracked locally: the floaters' rects
 * live outside CUI's widget hit-testing, so they keep their own edge state.
 * Each file that needs one keeps its own copy -- a shared static would fire
 * at the first call site only. */
static bool ov_pressed(cui_ctx *ui) {
    static bool was;
    bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
    bool hit = down && !was;
    was = down;
    return hit;
}
static bool ov_down(cui_ctx *ui) { return cui_mouse_down(ui, CUI_MOUSE_LEFT); }

static cui_color text1(cui_ctx *ui) { return cui_token_color(ui, CUI_TOKEN_FOREGROUND); }
static cui_color accent(cui_ctx *ui) { return cui_token_color(ui, CUI_TOKEN_ACCENT); }
static int btn_state(cui_ctx *ui, cui_rect r) {
    cui_vec2 m = cui_mouse_pos(ui);
    if (!ui_hit(r, m)) return 0;
    return ov_down(ui) ? 2 : 1;
}

/* ------------------------------------------------------------------------ */
/* The main control pill                                                     */
/* ------------------------------------------------------------------------ */

static const char *PILL_PRESET[3] = { "轻柔", "推荐", "强力" };
static const char *PILL_VIEW[3] = { "对比", "原图", "结果" };

#define PILL_PRESET_W  156.0f
#define PILL_VIEW_W    138.0f
#define PILL_SAVE_W     96.0f
#define PILL_ADD_W     100.0f

static cui_rect pill_rect(app_state *st, cui_rect stage, job *j) {
    bool full = j && j->has_result;
    float w = UI_PILL_PAD * 2.0f + PILL_PRESET_W;
    if (full) w += UI_S8 + PILL_VIEW_W + UI_S8 + UI_ICONBTN + UI_S4 + UI_ICONBTN + UI_S8 + PILL_SAVE_W;
    else w += UI_S8 + PILL_ADD_W;
    (void)st;
    return (cui_rect){ stage.x + (stage.w - w) * 0.5f,
                       stage.y + stage.h - UI_PILL_LIFT - UI_PILL_H, w, UI_PILL_H };
}

static void pill_paint(cui_ctx *ui, app_state *st, cui_rect stage, job *j) {
    st->r_pill = st->r_preset = st->r_view = (cui_rect){ 0, 0, 0, 0 };
    st->r_pill_tune = st->r_pill_brush = st->r_pill_save = st->r_pill_add = (cui_rect){ 0, 0, 0, 0 };
    if (st->count == 0 || st->protect) return;

    bool full = j && j->has_result;
    float a = cui_animate(ui, cui_make_id(ui, "pill"), 1.0f);
    cui_rect pr = pill_rect(st, stage, j);
    pr.y += (1.0f - a) * 14.0f;
    st->r_pill = pr;
    if (a < 0.02f) return;

    cui_push_clip(ui, stage);
    fl_card_draw(ui, pr, UI_R_FULL);
    float x = pr.x + UI_PILL_PAD;
    float cy = pr.y + (UI_PILL_H - UI_SEG_H) * 0.5f;

    /* preset segmented */
    cui_rect seg = { x, cy, PILL_PRESET_W, UI_SEG_H };
    ui_seg_draw(ui, seg, PILL_PRESET, 3,
                st->preset <= 2 ? st->preset : DEFAULT_PRESET, cui_make_id(ui, "seg_preset"));
    st->r_preset = seg;
    x += seg.w + UI_S8;

    if (full) {
        cui_rect vseg = { x, cy, PILL_VIEW_W, UI_SEG_H };
        ui_seg_draw(ui, vseg, PILL_VIEW, 3, (int)st->view, cui_make_id(ui, "seg_view"));
        st->r_view = vseg;
        x += vseg.w + UI_S8;

        cui_rect b = { x, cy, UI_ICONBTN, UI_ICONBTN };
        ui_iconbtn_draw(ui, b, CUI_ICON_SLIDERS_HORIZONTAL, btn_state(ui, b), st->tune_open);
        st->r_pill_tune = b;
        x += b.w + UI_S4;

        b.x = x;
        ui_iconbtn_draw(ui, b, ICON_BRUSH, btn_state(ui, b), false);
        st->r_pill_brush = b;
        x += b.w + UI_S8;

        /* save: accent; a done auto-save turns it into a tick for a moment;
         * the wake-up makes the tick expire on time even while the app idles */
        double age = j->saved_at > 0.0 ? now_seconds() - j->saved_at : 99.0;
        bool tick = age < 2.5;
        if (tick) cui_request_wakeup(ui, (float)(2.5 - age));
        cui_rect sv = { x, cy, PILL_SAVE_W, UI_SEG_H };
        ui_button_draw(ui, sv, tick ? CUI_ICON_CHECK : CUI_ICON_SAVE,
                       tick ? "已保存" : "另存为", !tick, false, btn_state(ui, sv));
        if (tick)
            cui_draw_icon(ui, CUI_ICON_CHECK, (cui_rect){ sv.x + 10, sv.y + sv.h * 0.5f - 8, 16, 16 },
                          15.0f, cui_token_color(ui, CUI_TOKEN_SUCCESS));
        st->r_pill_save = sv;
    } else {
        cui_rect ad = { x, cy, PILL_ADD_W, UI_SEG_H };
        ui_button_draw(ui, ad, CUI_ICON_PLUS, "添加图片", false, false, btn_state(ui, ad));
        st->r_pill_add = ad;
    }

    /* the quick-rerun bar: a 2px accent segment sweeping the pill's top edge
     * while a parameter-triggered re-run is in flight (spec §2.2) */
    if (j && j->status == JOB_RUNNING && j->has_result) {
        float t = (float)fmod(now_seconds() * 0.9, 1.0);
        float segw = pr.w * 0.28f;
        float span = pr.w - segw;
        float p = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
        cui_draw_rect(ui, (cui_rect){ pr.x + span * p, pr.y - 3.0f, segw, 2.0f },
                      CUI_CORNERS(1.0f), accent(ui));
        cui_request_redraw(ui);
    }
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Filmstrip tray                                                            */
/* ------------------------------------------------------------------------ */

#define TRAY_PAGE_BTN 24.0f

/* Tiles that fit beside the add tile and (when needed) the pager buttons. */
static int tray_capacity(cui_rect stage) {
    float avail = stage.w - UI_S32 * 2.0f - TRAY_PAGE_BTN * 2.0f - UI_TILE - UI_TILE_GAP * 4.0f;
    int cap = (int)((avail + UI_TILE_GAP) / (UI_TILE + UI_TILE_GAP));
    return cap < 1 ? 1 : cap;
}

void ui_tray_page(app_state *st, int dir) {
    int cap = tray_capacity(st->stage);
    int pages = (st->count + cap - 1) / cap;
    if (pages < 1) pages = 1;
    st->tray_page += dir;
    if (st->tray_page < 0) st->tray_page = pages - 1;
    if (st->tray_page >= pages) st->tray_page = 0;
}

static void tray_paint(cui_ctx *ui, app_state *st, cui_rect stage) {
    st->r_tray = st->r_tray_add = st->r_tray_prev = st->r_tray_next = (cui_rect){ 0, 0, 0, 0 };
    for (int i = 0; i < MAX_JOBS; i++) {
        st->r_queue[i] = (cui_rect){ 0, 0, 0, 0 };
        st->r_tray_x[i] = (cui_rect){ 0, 0, 0, 0 };
    }
    if (st->count <= 1 || st->protect) return;

    int cap = tray_capacity(stage);
    int pages = (st->count + cap - 1) / cap;
    if (st->tray_page >= pages) st->tray_page = pages - 1;
    if (st->tray_page < 0) st->tray_page = 0;
    /* keep the selected tile on the visible page */
    if (st->selected >= 0 && st->selected / cap != st->tray_page && st->selected / cap < pages)
        st->tray_page = st->selected / cap;

    int first = st->tray_page * cap;
    int n = st->count - first;
    if (n > cap) n = cap;
    bool paged = st->count > cap;

    float items_w = (float)n * UI_TILE + (float)(n - 1) * UI_TILE_GAP + UI_TILE_GAP + UI_TILE;
    float w = items_w + UI_TILE_PAD * 2.0f + (paged ? TRAY_PAGE_BTN * 2.0f + UI_S8 : 0.0f);
    float a = cui_animate(ui, cui_make_id(ui, "tray"), 1.0f);
    cui_rect tr = { stage.x + (stage.w - w) * 0.5f,
                    stage.y + stage.h - UI_PILL_LIFT - UI_PILL_H - UI_TRAY_GAP - UI_TRAY_H,
                    w, UI_TRAY_H };
    tr.y += (1.0f - a) * 14.0f;
    st->r_tray = tr;
    if (a < 0.02f) return;

    cui_push_clip(ui, stage);
    fl_card_draw(ui, tr, UI_R_LG);

    float x = tr.x + UI_TILE_PAD;
    float ty = tr.y + (UI_TRAY_H - UI_TILE) * 0.5f;

    if (paged) {
        cui_rect pv = { x, tr.y + (UI_TRAY_H - 24.0f) * 0.5f, TRAY_PAGE_BTN, 24.0f };
        ui_iconbtn_draw(ui, pv, CUI_ICON_CHEVRON_LEFT, btn_state(ui, pv), false);
        st->r_tray_prev = pv;
        x += TRAY_PAGE_BTN + UI_S4;
    }

    cui_vec2 m = cui_mouse_pos(ui);
    for (int i = 0; i < n; i++) {
        int idx = first + i;
        job *jb = &st->jobs[idx];
        cui_rect t = { x, ty, UI_TILE, UI_TILE };
        st->r_queue[idx] = t;
        bool sel = idx == st->selected;
        bool hot = ui_hit(t, m);

        if (sel)
            cui_draw_rect(ui, (cui_rect){ t.x - 2, t.y - 2, t.w + 4, t.h + 4 },
                          CUI_CORNERS(UI_R_MD + 2), accent(ui));
        if (jb->thumb_tex)
            cui_draw_image(ui, jb->thumb_tex, t, (cui_rect){ 0, 0, 1, 1 },
                           CUI_CORNERS(UI_R_MD), CUI_RGBA(0xffffffff));
        else {
            cui_draw_rect(ui, t, CUI_CORNERS(UI_R_MD), fl_card_bg(ui));
            cui_draw_icon(ui, CUI_ICON_IMAGE, t, 18.0f, fl_text3(ui));
        }
        if (hot && !sel)
            cui_draw_rect_border(ui, t, CUI_CORNERS(UI_R_MD), CUI_TRANSPARENT, fl_border(ui), 1.0f);

        /* status badge, bottom-right */
        float bx = t.x + t.w - 13.0f, by = t.y + t.h - 13.0f;
        cui_vec2 bc = { bx + 6.5f, by + 6.5f };
        if (jb->status == JOB_RUNNING) {
            fl_spinner(ui, bc, 5.5f, 2.0f, accent(ui));
        } else if (jb->status == JOB_DONE) {
            cui_draw_circle(ui, bc, 7.0f, cui_token_color(ui, CUI_TOKEN_SUCCESS));
            cui_draw_icon(ui, CUI_ICON_CHECK, (cui_rect){ bx + 2, by + 2, 9, 9 }, 9.0f,
                          CUI_RGBA(0xffffffff));
        } else if (jb->status == JOB_ERROR) {
            cui_draw_circle(ui, bc, 7.0f, cui_token_color(ui, CUI_TOKEN_DANGER));
            cui_draw_icon(ui, CUI_ICON_X, (cui_rect){ bx + 2, by + 2, 9, 9 }, 9.0f,
                          CUI_RGBA(0xffffffff));
        } else if (jb->status == JOB_QUEUED) {
            cui_draw_circle(ui, bc, 4.5f, CUI_RGBA(0x8C8C8Cff));
        }

        /* hover remove ×, top-right */
        if (hot) {
            cui_rect xr = { t.x + t.w - 16.0f, t.y - 2.0f, 18.0f, 18.0f };
            st->r_tray_x[idx] = xr;
            cui_draw_circle(ui, (cui_vec2){ xr.x + 9, xr.y + 9 }, 8.0f,
                            ui_hit(xr, m) ? cui_token_color(ui, CUI_TOKEN_DANGER)
                                          : CUI_RGBA(0x00000099));
            cui_draw_icon(ui, CUI_ICON_X, (cui_rect){ xr.x + 4, xr.y + 4, 10, 10 }, 10.0f,
                          CUI_RGBA(0xffffffff));
        }
        x += UI_TILE + UI_TILE_GAP;
    }

    /* the add tile at the end */
    cui_rect add = { x, ty, UI_TILE, UI_TILE };
    st->r_tray_add = add;
    bool ahot = ui_hit(add, m);
    cui_draw_rect_border(ui, add, CUI_CORNERS(UI_R_MD), ahot ? fl_hover(ui) : CUI_TRANSPARENT,
                         fl_border(ui), 1.0f);
    cui_draw_icon(ui, CUI_ICON_PLUS, add, 18.0f, ahot ? accent(ui) : fl_text3(ui));

    if (paged) {
        cui_rect nx = { x + UI_TILE + UI_S4, tr.y + (UI_TRAY_H - 24.0f) * 0.5f,
                        TRAY_PAGE_BTN, 24.0f };
        ui_iconbtn_draw(ui, nx, CUI_ICON_CHEVRON_RIGHT, btn_state(ui, nx), false);
        st->r_tray_next = nx;
    }
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Protection toolbar                                                        */
/* ------------------------------------------------------------------------ */

static const char *PTOOL_ICON[3] = { ICON_BRUSH, ICON_ERASER, ICON_LASSO };

/* One x-walk that lays out the toolbar's contents; run once with paint=false
 * to measure the natural width and once with paint=true to draw it. The width
 * can never drift from the layout the way a hand-summed constant did. */
static float protect_lay(cui_ctx *ui, app_state *st, job *j, cui_rect pr, bool paint) {
    float x = pr.x + UI_PILL_PAD;
    float cy = pr.y + (UI_PROTECT_H - UI_ICONBTN) * 0.5f;
    float cyc = pr.y + (UI_PROTECT_H - UI_ICONBTN_SM) * 0.5f;

    for (int i = 0; i < 3; i++) {
        cui_rect b = { x, cy, UI_ICONBTN, UI_ICONBTN };
        if (paint) {
            ui_iconbtn_draw(ui, b, PTOOL_ICON[i], btn_state(ui, b), st->tool == (tool_mode)(i + 1));
            st->r_tool[i] = b;
        }
        x += UI_ICONBTN + UI_S4;
    }
    x += UI_S4;
    if (paint)
        cui_draw_rect(ui, (cui_rect){ x, pr.y + 10.0f, 1.0f, UI_PROTECT_H - 20.0f },
                      CUI_CORNERS(0), fl_border(ui));
    x += UI_S8;

    /* sliders: brush / 恢复比例 / 柔边, each with a value caption */
    float vals[3] = { st->brush_size, st->brush_mix, st->feather_px };
    const char *suffix[3] = { "", "%", "" };
    for (int i = 0; i < 3; i++) {
        cui_rect sr = { x, pr.y + 6.0f, 64.0f, UI_PROTECT_H - 12.0f };
        if (paint) {
            ui_slider_draw(ui, sr, vals[i],
                           i == 0 ? BRUSH_KEY_MIN : 0.0f,
                           i == 0 ? BRUSH_KEY_MAX : (i == 1 ? 100.0f : 150.0f),
                           st->drag_ctl == (int)(CTL_P_BRUSH + i));
            st->r_p_slider[i] = sr;
            char vb[16];
            snprintf(vb, sizeof vb, "%.0f", (double)vals[i]);
            if (suffix[i][0]) strncat(vb, suffix[i], sizeof vb - strlen(vb) - 1);
            cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, vb, -1);
            cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                          (cui_vec2){ x + 68.0f, pr.y + (UI_PROTECT_H - tm.height) * 0.5f },
                          fl_text3(ui), vb, -1);
        }
        x += 64.0f + 24.0f + UI_S4;
    }
    x += UI_S4;
    if (paint)
        cui_draw_rect(ui, (cui_rect){ x, pr.y + 10.0f, 1.0f, UI_PROTECT_H - 20.0f },
                      CUI_CORNERS(0), fl_border(ui));
    x += UI_S8;

    struct { cui_rect *r; const char *icon; bool on; bool disabled; } small[] = {
        { &st->r_undo,   CUI_ICON_UNDO_2, false, !mask_can_undo(j->hist) },
        { &st->r_redo,   CUI_ICON_REDO_2, false, !mask_can_redo(j->hist) },
        { &st->r_peye,   st->mask_eye ? CUI_ICON_EYE : CUI_ICON_EYE_OFF, st->mask_eye, false },
        { &st->r_pinvert, ICON_CONTRAST,  false, !j->mask_active },
        { &st->r_pclear, CUI_ICON_TRASH_2, false, !j->mask_active },
    };
    for (int i = 0; i < 5; i++) {
        cui_rect b = { x, cyc, UI_ICONBTN_SM, UI_ICONBTN_SM };
        if (paint) {
            int s = small[i].disabled ? 0 : btn_state(ui, b);
            ui_iconbtn_draw(ui, b, small[i].icon, s, small[i].on);
            if (small[i].disabled)
                cui_draw_rect(ui, b, CUI_CORNERS(UI_R_MD), CUI_RGBA(fl_dark(ui) ? 0x00000040 : 0xFFFFFF66));
            *small[i].r = b;
        }
        x += UI_ICONBTN_SM + UI_S4;
        if (i == 1) x += UI_S4;
    }
    x += UI_S4;

    /* 未保护结果 toggle */
    cui_rect up = { x, cyc, 76.0f, UI_ICONBTN_SM };
    if (paint) {
        ui_button_draw(ui, up, NULL, "未保护", false, !j->has_result, btn_state(ui, up));
        if (st->view_unprotected)
            cui_draw_rect_border(ui, up, CUI_CORNERS(UI_R_MD), CUI_TRANSPARENT, accent(ui), 1.5f);
        st->r_punprot = up;
    }
    x += up.w + UI_S8;

    cui_rect cc = { x, cyc, 56.0f, UI_ICONBTN_SM };
    if (paint) {
        ui_button_draw(ui, cc, NULL, "取消", false, false, btn_state(ui, cc));
        st->r_pcancel = cc;
    }
    x += cc.w + UI_S4;

    cui_rect dn = { x, cyc, 56.0f, UI_ICONBTN_SM };
    if (paint) {
        ui_button_draw(ui, dn, NULL, "完成", true, false, btn_state(ui, dn));
        st->r_pdone = dn;
    }
    return x + dn.w;
}

static void protect_paint(cui_ctx *ui, app_state *st, cui_rect stage, job *j) {
    st->r_protect = (cui_rect){ 0, 0, 0, 0 };
    for (int i = 0; i < 3; i++) { st->r_tool[i] = (cui_rect){0,0,0,0}; st->r_p_slider[i] = (cui_rect){0,0,0,0}; }
    st->r_undo = st->r_redo = st->r_peye = st->r_pinvert = st->r_pclear =
        st->r_punprot = st->r_pdone = st->r_pcancel = (cui_rect){ 0, 0, 0, 0 };
    if (!st->protect || !j) return;

    float a = cui_animate(ui, cui_make_id(ui, "protect"), 1.0f);
    cui_rect pr = { stage.x, stage.y + UI_S8, 0.0f, UI_PROTECT_H };
    float w = protect_lay(ui, st, j, pr, false) - pr.x + UI_PILL_PAD;
    pr.x = stage.x + (stage.w - w) * 0.5f;
    pr.w = w;
    pr.y -= (1.0f - a) * (UI_PROTECT_H + 8.0f);
    st->r_protect = pr;
    if (a < 0.02f) return;

    cui_push_clip(ui, stage);
    fl_card_draw(ui, pr, UI_R_FULL);
    protect_lay(ui, st, j, pr, true);
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Tune card                                                                 */
/* ------------------------------------------------------------------------ */

static void tune_paint(cui_ctx *ui, app_state *st, cui_rect stage) {
    st->r_tune = st->r_tune_reset = (cui_rect){ 0, 0, 0, 0 };
    st->r_tune_slider[0] = st->r_tune_slider[1] = (cui_rect){ 0, 0, 0, 0 };
    if (!st->tune_open || st->protect || st->count == 0) return;

    /* anchored above the tune button, clamped inside the stage */
    float ax = st->r_pill_tune.w > 0 ? st->r_pill_tune.x + st->r_pill_tune.w * 0.5f
                                   : stage.x + stage.w * 0.5f;
    float a = cui_animate(ui, cui_make_id(ui, "tune"), 1.0f);
    float h = 140.0f;
    cui_rect cr = { ax - UI_TUNE_W * 0.5f,
                    stage.y + stage.h - UI_PILL_LIFT - UI_PILL_H - UI_S8 - h,
                    UI_TUNE_W, h };
    if (cr.x < stage.x + UI_S8) cr.x = stage.x + UI_S8;
    if (cr.x + cr.w > stage.x + stage.w - UI_S8) cr.x = stage.x + stage.w - UI_S8 - cr.w;
    cr.y += (1.0f - a) * 8.0f;
    st->r_tune = cr;
    if (a < 0.02f) return;

    cui_push_clip(ui, stage);
    fl_card_draw(ui, cr, UI_R_LG);

    float x = cr.x + UI_S16;
    float y = cr.y + UI_S12;
    cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, (cui_vec2){ x, y }, text1(ui), "微调参数", -1);
    {
        cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, "松手即重跑", -1);
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                      (cui_vec2){ cr.x + cr.w - UI_S16 - tm.width, y + 3.0f },
                      fl_text3(ui), "松手即重跑", -1);
    }
    y += 26.0f;

    /* 预缩比例 30..100 */
    char vb[16];
    snprintf(vb, sizeof vb, "%d%%", st->down_pct);
    cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, (cui_vec2){ x, y }, fl_text2(ui), "预缩比例", -1);
    {
        cui_text_metrics vm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, vb, -1);
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                      (cui_vec2){ cr.x + cr.w - UI_S16 - vm.width, y }, text1(ui), vb, -1);
    }
    y += 14.0f;
    cui_rect s0 = { x, y, UI_TUNE_W - UI_S16 * 2.0f, 18.0f };
    ui_slider_draw(ui, s0, (float)st->down_pct, 30.0f, 100.0f, st->drag_ctl == CTL_TUNE_DOWN);
    st->r_tune_slider[0] = s0;
    y += 28.0f;

    /* L0 0..0.005 */
    char l0[16];
    fmt_l0(st->l0, l0, sizeof l0);
    cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, (cui_vec2){ x, y }, fl_text2(ui), "L0 平滑", -1);
    {
        cui_text_metrics vm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, l0, -1);
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                      (cui_vec2){ cr.x + cr.w - UI_S16 - vm.width, y }, text1(ui), l0, -1);
    }
    y += 14.0f;
    cui_rect s1 = { x, y, UI_TUNE_W - UI_S16 * 2.0f, 18.0f };
    ui_slider_draw(ui, s1, st->l0, 0.0f, 0.005f, st->drag_ctl == CTL_TUNE_L0);
    st->r_tune_slider[1] = s1;
    y += 26.0f;

    cui_rect rs = { cr.x + cr.w - UI_S16 - 72.0f, y - 4.0f, 72.0f, 24.0f };
    ui_button_draw(ui, rs, CUI_ICON_ROTATE_CCW, "重置", false, false, btn_state(ui, rs));
    st->r_tune_reset = rs;
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Engine banner                                                             */
/* ------------------------------------------------------------------------ */

static void banner_paint(cui_ctx *ui, app_state *st, cui_rect stage) {
    st->r_banner = st->r_banner_retry = (cui_rect){ 0, 0, 0, 0 };
    if (st->bench || (!st->booting && !st->engine_error[0])) return;

    bool err = !st->booting && st->engine_error[0];
    const char *line = st->booting ? "正在启动推理引擎…" : "推理引擎未就绪";
    const char *sub = st->booting
                      ? (st->boot_stage[0] ? st->boot_stage : "加载中")
                      : st->engine_error;

    cui_text_metrics t1 = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, line, -1);
    cui_text_metrics t2 = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, sub, -1);
    float text_w = t1.width > t2.width ? t1.width : t2.width;
    if (text_w > 380.0f) text_w = 380.0f;
    float w = 24.0f + text_w + (err ? 76.0f : 0.0f) + 16.0f;
    float a = cui_animate(ui, cui_make_id(ui, "banner"), 1.0f);
    cui_rect br = { stage.x + (stage.w - w) * 0.5f, stage.y + UI_S8, w, UI_BANNER_H };
    br.y -= (1.0f - a) * (UI_BANNER_H + 8.0f);
    st->r_banner = br;
    if (a < 0.02f) return;

    cui_push_clip(ui, stage);
    fl_card_draw(ui, br, UI_R_FULL);
    float x = br.x + UI_S16;
    if (st->booting) {
        fl_spinner(ui, (cui_vec2){ x + 7, br.y + br.h * 0.5f }, 7.0f, 2.0f, accent(ui));
    } else {
        cui_draw_icon(ui, CUI_ICON_CIRCLE_ALERT,
                      (cui_rect){ x, br.y + br.h * 0.5f - 8, 16, 16 }, 16.0f,
                      cui_token_color(ui, CUI_TOKEN_DANGER));
    }
    x += 22.0f;
    cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, (cui_vec2){ x, br.y + 5.0f }, text1(ui),
                  line, -1);
    if (sub && *sub)
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, (cui_vec2){ x, br.y + 20.0f },
                      fl_text2(ui), sub, -1);
    x += text_w + 8.0f;
    if (err) {
        cui_rect rb = { x, br.y + (UI_BANNER_H - UI_ICONBTN_SM) * 0.5f, 68.0f, UI_ICONBTN_SM };
        ui_button_draw(ui, rb, CUI_ICON_REFRESH_CW, "重试", false, false, btn_state(ui, rb));
        st->r_banner_retry = rb;
    }
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Empty state                                                               */
/* ------------------------------------------------------------------------ */

void ui_empty_paint(cui_ctx *ui, app_state *st, cui_rect r) {
    float cw = 380.0f, ch = 196.0f;
    cui_rect cr = { r.x + (r.w - cw) * 0.5f, r.y + (r.h - ch) * 0.5f - 20.0f, cw, ch };
    st->r_empty = cr;

    cui_draw_rect_border(ui, cr, CUI_CORNERS(UI_R_LG), fl_card_bg(ui), fl_border(ui), 1.0f);
    float cx = cr.x + cr.w * 0.5f;
    cui_draw_icon(ui, ICON_IMAGES, (cui_rect){ cx - 24, cr.y + 30, 48, 48 }, 48.0f, fl_text3(ui));

    const char *t1 = "拖入图片、点击浏览或 Ctrl+V 粘贴";
    const char *t2 = "支持批量队列";
    cui_text_metrics m1 = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_SUB, t1, -1);
    cui_draw_text(ui, CUI_FONT_BODY, UI_FS_SUB,
                  (cui_vec2){ cx - m1.width * 0.5f, cr.y + 96 }, text1(ui), t1, -1);
    cui_text_metrics m2 = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_BODY, t2, -1);
    cui_draw_text(ui, CUI_FONT_BODY, UI_FS_BODY,
                  (cui_vec2){ cx - m2.width * 0.5f, cr.y + 122 }, fl_text3(ui), t2, -1);

    cui_rect br = { cx - 52.0f, cr.y + 148, 104.0f, 30.0f };
    ui_button_draw(ui, br, CUI_ICON_FOLDER_OPEN, "浏览文件…", true, false, btn_state(ui, br));
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

/* True when the pointer is over any floating control this frame. Handles
 * every overlay click itself so the stage never sees a double gesture. */
bool ui_overlay_input(app_state *st, job *j) {
    cui_ctx *ui = st->ui;
    cui_vec2 m = cui_mouse_pos(ui);
    bool press = ov_pressed(ui);
    bool down = ov_down(ui);

    /* a captured slider keeps the pointer until release, wherever it goes */
    if (st->drag_ctl != CTL_NONE) {
        if (!down) {
            if (st->drag_ctl == CTL_TUNE_DOWN || st->drag_ctl == CTL_TUNE_L0) {
                st->preset = preset_of(st->down_pct, st->l0);
                settings_from_state(st);
                if (j) job_rerun(st, j);          /* 松手重跑 (spec §3) */
            } else if (st->drag_ctl == CTL_P_BRUSH || st->drag_ctl == CTL_P_MIX) {
                settings_from_state(st);
            }
            st->drag_ctl = CTL_NONE;
        } else if (st->drag_ctl == CTL_TUNE_DOWN) {
            st->down_pct = (int)(ui_slider_value(st->r_tune_slider[0], m.x, 30.0f, 100.0f, 5.0f) + 0.5f);
        } else if (st->drag_ctl == CTL_TUNE_L0) {
            st->l0 = ui_slider_value(st->r_tune_slider[1], m.x, 0.0f, 0.005f, 0.0001f);
        } else if (st->drag_ctl == CTL_P_BRUSH) {
            st->brush_size = ui_slider_value(st->r_p_slider[0], m.x, BRUSH_KEY_MIN, BRUSH_KEY_MAX, 1.0f);
        } else if (st->drag_ctl == CTL_P_MIX) {
            st->brush_mix = ui_slider_value(st->r_p_slider[1], m.x, 0.0f, 100.0f, 1.0f);
        } else if (st->drag_ctl == CTL_P_FEATHER) {
            st->feather_px = ui_slider_value(st->r_p_slider[2], m.x, 0.0f, 150.0f, 2.0f);
            if (j) {
                j->feather_px = (int)(st->feather_px + 0.5f);
                j->mask_tex_dirty = true;
                j->composite_pending = true;
            }
        }
        return true;
    }

    /* hit zones, topmost first */
    bool in_tune = st->tune_open && ui_hit(st->r_tune, m);
    bool in_protect = st->protect && ui_hit(st->r_protect, m);
    bool in_pill = ui_hit(st->r_pill, m);
    bool in_tray = ui_hit(st->r_tray, m);
    bool in_banner = ui_hit(st->r_banner, m);
    bool in_empty = st->count == 0 && ui_hit(st->r_empty, m);
    bool consumed = in_tune || in_protect || in_pill || in_tray || in_banner || in_empty;

    /* light-dismiss: a press outside the tune card closes it */
    if (st->tune_open && press && !in_tune) st->tune_open = false;

    if (!press) return consumed;

    /* --- tune card --- */
    if (in_tune) {
        if (ui_hit(st->r_tune_slider[0], m)) st->drag_ctl = CTL_TUNE_DOWN;
        else if (ui_hit(st->r_tune_slider[1], m)) st->drag_ctl = CTL_TUNE_L0;
        else if (ui_hit(st->r_tune_reset, m)) {
            st->down_pct = PRESETS[DEFAULT_PRESET].down_pct;
            st->l0 = PRESETS[DEFAULT_PRESET].l0;
            st->preset = DEFAULT_PRESET;
            settings_from_state(st);
            if (j) job_rerun(st, j);
        }
        return true;
    }

    /* --- protect bar --- */
    if (in_protect && j) {
        for (int i = 0; i < 3; i++) {
            if (ui_hit(st->r_tool[i], m)) {
                if (st->tool == (tool_mode)(i + 1)) st->tool = TOOL_BRUSH;
                else {
                    lasso_reset(st);
                    st->tool = (tool_mode)(i + 1);
                    if (st->tool == TOOL_LASSO) ensure_edges(j);
                }
                return true;
            }
        }
        if (ui_hit(st->r_p_slider[0], m)) { st->drag_ctl = CTL_P_BRUSH; return true; }
        if (ui_hit(st->r_p_slider[1], m)) { st->drag_ctl = CTL_P_MIX; return true; }
        if (ui_hit(st->r_p_slider[2], m)) { st->drag_ctl = CTL_P_FEATHER; return true; }
        if (ui_hit(st->r_undo, m) && mask_can_undo(j->hist)) {
            if (mask_undo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
            return true;
        }
        if (ui_hit(st->r_redo, m) && mask_can_redo(j->hist)) {
            if (mask_redo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
            return true;
        }
        if (ui_hit(st->r_peye, m)) { st->mask_eye = !st->mask_eye; return true; }
        if (ui_hit(st->r_pinvert, m) && j->mask_active) {
            if (st->lasso_active) lasso_close_now(st, j, false);
            mask_stroke_begin(&j->m, j->hist);
            mask_invert(&j->m);
            mask_stroke_end(&j->m, j->hist);
            mask_note(st, j);
            j->composite_pending = true;
            toast(st, "已反选（Ctrl+Z 可撤销）");
            return true;
        }
        if (ui_hit(st->r_pclear, m) && j->mask_active) {
            mask_stroke_begin(&j->m, j->hist);
            mask_clear(&j->m);
            mask_stroke_end(&j->m, j->hist);
            mask_note(st, j);
            j->composite_pending = true;
            return true;
        }
        if (ui_hit(st->r_punprot, m)) {
            if (j->has_result) st->view_unprotected = !st->view_unprotected;
            return true;
        }
        if (ui_hit(st->r_pcancel, m)) { protect_cancel(st); return true; }
        if (ui_hit(st->r_pdone, m)) { protect_done(st); return true; }
        return true;
    }

    /* --- banner --- */
    if (in_banner) {
        if (ui_hit(st->r_banner_retry, m)) st->want_restart = true;
        return true;
    }

    /* --- tray --- */
    if (in_tray) {
        for (int i = 0; i < st->count; i++) {
            if (st->r_tray_x[i].w > 0 && ui_hit(st->r_tray_x[i], m)) {
                job_remove(ui, st, i);
                return true;
            }
        }
        for (int i = 0; i < st->count; i++) {
            if (st->r_queue[i].w > 0 && ui_hit(st->r_queue[i], m)) {
                if (st->selected != i) job_focus(st, i);
                return true;
            }
        }
        if (ui_hit(st->r_tray_add, m)) { st->want_browse = true; return true; }
        if (ui_hit(st->r_tray_prev, m)) { ui_tray_page(st, -1); return true; }
        if (ui_hit(st->r_tray_next, m)) { ui_tray_page(st, +1); return true; }
        return true;
    }

    /* --- pill --- */
    if (in_pill) {
        int p = ui_seg_hit(st->r_preset, 3, m.x);
        if (p >= 0) {
            apply_preset(st, p);
            settings_from_state(st);
            if (j) job_rerun(st, j);            /* 改预设即重跑 (spec §9) */
            return true;
        }
        int v = ui_seg_hit(st->r_view, 3, m.x);
        if (v >= 0) { st->view = (view_mode)v; settings_from_state(st); return true; }
        if (ui_hit(st->r_pill_tune, m)) { st->tune_open = !st->tune_open; return true; }
        if (ui_hit(st->r_pill_brush, m)) { protect_enter(st); return true; }
        if (ui_hit(st->r_pill_save, m)) { st->want_save = true; return true; }
        if (ui_hit(st->r_pill_add, m)) { st->want_browse = true; return true; }
        return true;
    }

    /* --- empty state --- */
    if (in_empty) { st->want_browse = true; return true; }

    return consumed;
}

/* ------------------------------------------------------------------------ */
/* Paint entry                                                               */
/* ------------------------------------------------------------------------ */

void ui_float_paint(cui_ctx *ui, app_state *st, cui_rect r) {
    job *j = job_selected(st);
    banner_paint(ui, st, r);
    tray_paint(ui, st, r);
    pill_paint(ui, st, r, j);
    protect_paint(ui, st, r, j);
    tune_paint(ui, st, r);

    /* the union the brush cursor hides behind */
    cui_rect u = { 0, 0, 0, 0 };
    cui_rect rects[] = { st->r_pill, st->r_tray, st->r_protect, st->r_tune, st->r_banner };
    for (int i = 0; i < 5; i++) {
        cui_rect q = rects[i];
        if (q.w <= 0.0f) continue;
        if (u.w <= 0.0f) { u = q; continue; }
        float x0 = u.x < q.x ? u.x : q.x;
        float y0 = u.y < q.y ? u.y : q.y;
        float x1 = (u.x + u.w) > (q.x + q.w) ? (u.x + u.w) : (q.x + q.w);
        float y1 = (u.y + u.h) > (q.y + q.h) ? (u.y + u.h) : (q.y + q.h);
        u = (cui_rect){ x0, y0, x1 - x0, y1 - y0 };
    }
    st->r_stage_buttons = u;
}
