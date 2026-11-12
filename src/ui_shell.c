/* ui_shell.c -- see AGENTS.md for the routing table and docs/design.md for the rules. */

#include "app.h"
#include "icons.h"
#include "ui_tokens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What cui_separator draws: 1pt. The strip centers its tile row in what its own
 * hairline leaves, so the tiles are optically centered in UI_STRIP. */
#define UI_HAIRLINE 1.0f

/* The queue is a result list, not a parameter: it keeps a fixed share of the
 * sidebar's height (percent, for CUI_PCT) instead of growing with its content. */
#define UI_QUEUE_PCT 40.0f

/* ------------------------------------------------------------------------ */
/* Tiles                                                                    */
/* ------------------------------------------------------------------------ */

/* One painter for every 62pt tile: the thumbnails and the "add" tile differ
 * only in what sits inside the frame, never in the frame itself (§4.4). */
typedef struct tile_ctx {
    cui_texture tex;    /* 0 draws the placeholder; the "add" tile ignores it */
    bool is_add;
    bool selected;
} tile_ctx;

/* Paint runs after layout, when CUI's own hit state already belongs to the
 * last widget built, so the tile asks the pointer about its own rect. */
static bool pointer_in(cui_ctx *ui, cui_rect r) {
    cui_vec2 m = cui_mouse_pos(ui);
    return m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
}

static void tile_paint(cui_ctx *ui, cui_rect r, void *user) {
    const tile_ctx *t = (const tile_ctx *)user;
    bool hot = t->is_add && pointer_in(ui, r);
    cui_color bg = t->selected ? CUI_COLOR_ACCENT_SUBTLE : CUI_COLOR_SURFACE_RAISED;
    cui_draw_rect(ui, r, CUI_CORNERS(UI_R_TILE), bg);

    /* the picture is inset one point so the border never eats into it */
    cui_rect inner = { r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h - 2.0f };
    if (t->is_add) {
        cui_draw_icon(ui, CUI_ICON_PLUS, inner, UI_ICON_LG, hot ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_MUTED);
    } else if (t->tex) {
        cui_draw_image(ui, t->tex, inner, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(UI_R_TILE - 1.0f),
                       CUI_RGBA(0xffffffff));
    } else {
        cui_draw_icon(ui, CUI_ICON_IMAGE, inner, UI_ICON_LG, CUI_COLOR_TEXT_MUTED);
    }
    /* the ring insets one point like the picture: a selected tile never grows
     * past the frame its neighbours are laid out at */
    cui_draw_rect_border(ui, inner, CUI_CORNERS(UI_R_TILE - 1.0f), CUI_RGBA(0x00000000),
                         (t->selected || hot) ? CUI_COLOR_ACCENT : CUI_COLOR_BORDER,
                         t->selected ? 2.0f : 1.0f);
}

/* Top bar                                                                  */
/* ------------------------------------------------------------------------ */

void ui_topbar(cui_ctx *ui, app_state *st) {
    /* chrome.c hands only the rightmost UI_CAPTION_BUTTONS to the UI (that
     * strip is HTCLIENT, everything left of it drags the window), so the title
     * bar controls must fit: eight 30pt icon buttons, one hairline and seven
     * UI_GAP gaps. */
    _Static_assert(8 * UI_CONTROL_H + 7 * UI_GAP + 1.0f <= UI_CAPTION_BUTTONS,
                   "title bar controls overflow the caption no-drag zone");

    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(UI_TOPBAR), .pad_x = UI_PAD, .gap = UI_GAP,
            .align = CUI_ALIGN_CENTER, .bg = CUI_COLOR_SURFACE) {
        cui_icon(ui, CUI_ICON_SPARKLES, UI_ICON_LG, CUI_COLOR_ACCENT);
        cui_text(ui, APP_TITLE, .font = CUI_FONT_DISPLAY, .size = UI_FS_BRAND, .nowrap = true);
        cui_badge(ui, st->device, .size = CUI_SIZE_SM, .dot = true,
                  .tone = st->engine_ready ? CUI_TONE_SUCCESS : CUI_TONE_DANGER);
        cui_spacer(ui);

        cui_row(ui, .height = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
            if (st->upd.state == UPD_AVAILABLE || st->upd.state == UPD_READY) {
                char tip[64];
                if (st->upd.state == UPD_READY) snprintf(tip, sizeof tip, "重启以更新");
                else snprintf(tip, sizeof tip, "更新到 %s", st->upd.latest);
                if (cui_icon_button(ui, CUI_ICON_DOWNLOAD, .size = CUI_SIZE_SM,
                                    .variant = CUI_BUTTON_GHOST, .color = CUI_COLOR_ACCENT,
                                    .tooltip = tip)) {
                    st->show_update = true;
                }
            }

            if (cui_icon_button(ui, CUI_ICON_CIRCLE_HELP, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = "帮助与快捷键（?）", .shortcut = "?")) {
                st->show_help = true;
            }
            if (cui_icon_button(ui, CUI_ICON_SETTINGS, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = "设置")) {
                st->show_settings = true;
            }
            if (cui_icon_button(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_ICON_MOON : CUI_ICON_SUN,
                                .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = "切换浅色 / 深色")) {
                int mode = st->set.theme_mode;
                mode = (mode + 1) % 3;
                st->set.theme_mode = mode;
                if (mode == 0) { cui_app_follow_system_theme(NULL, true); }
                else { cui_app_follow_system_theme(NULL, false); cui_set_theme(ui, mode == 1 ? CUI_THEME_LIGHT : CUI_THEME_DARK); }
                /* the DWM caption/border follows the app theme too */
                chrome_set_dark(cui_get_theme_mode(ui) == CUI_THEME_DARK);
                cfg_save(&st->set);
            }
            cui_separator(ui);
            if (cui_icon_button(ui, CUI_ICON_MINUS, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = "最小化")) {
                chrome_minimize();
            }
            if (cui_icon_button(ui, chrome_is_maximized() ? ICON_MINIMIZE : ICON_MAXIMIZE,
                                .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = chrome_is_maximized() ? "还原" : "最大化")) {
                chrome_toggle_maximize();
            }
            if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .tooltip = "关闭（Ctrl+Q）", .shortcut = "Ctrl Q")) {
                chrome_close();
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Header row: file name, status, actions                                   */
/* ------------------------------------------------------------------------ */

void ui_job_header(cui_ctx *ui, app_state *st, job *j) {
    cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER, .height = CUI_PX(UI_HEAD)) {
        cui_text(ui, j->name, .font = CUI_FONT_MEDIUM, .size = UI_FS_TITLE, .nowrap = true);
        cui_badge(ui, status_label(j->status), .tone = status_tone(j->status), .size = CUI_SIZE_SM,
                  .dot = j->status == JOB_RUNNING);
        if (j->mask_active) cui_badge(ui, "局部保护", .size = CUI_SIZE_SM, .tone = CUI_TONE_WARNING,
                                      .icon = CUI_ICON_SHIELD);
        cui_spacer(ui);

        if (j->status == JOB_RUNNING && !j->cancel_sent) {
            if (cui_button(ui, "取消", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_X)) {
                j->cancel_sent = true;
                dn_cancel(j->id);
            }
        }
        if (j->status == JOB_DONE) {
            bool stale = j->down_pct != j->res_down_pct || fabsf(j->l0 - j->res_l0) > 1e-6f;
            if (stale) {
                if (cui_button(ui, "用新参数重新去噪", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_REFRESH_CW)) {
                    j->status = JOB_QUEUED;
                    engine_submit_next(st);
                }
            }
            if (cui_button(ui, "保存", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_DOWNLOAD, .shortcut = "Ctrl S")) {
                st->want_save = true;
            }
            if (cui_button(ui, "打开文件夹", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_FOLDER_OPEN)) {
                cui_app_open_url(st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir());
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Sidebar                                                                  */
/* ------------------------------------------------------------------------ */

static void ui_queue_row(cui_ctx *ui, app_state *st, int index) {
    job *j = &st->jobs[index];
    bool selected = st->selected == index;
    bool cancel = false;

    char card_id[48];
    snprintf(card_id, sizeof card_id, "job_row_%d", index);
    char params[96];
    {
        char l0[16];
        fmt_l0(j->l0, l0, sizeof l0);
        snprintf(params, sizeof params, "预缩 %d%% · L0 %s", j->down_pct, l0);
    }

    cui_push_id_int(ui, index);
    cui_card_opts co;
    memset(&co, 0, sizeof co);
    co.id = card_id;
    co.dir = CUI_ROW;
    co.align = CUI_ALIGN_CENTER;
    co.gap = UI_GAP;
    co.pad = 7;                 /* card padding, docs/design.md §4.4 (no token) */
    co.width = CUI_FILL;
    co.hoverable = true;
    co.muted = selected;   /* selection reads as a surface change, not a red frame */
    bool clicked = cui_card_begin(ui, &co);

    /* 40pt list thumb: no token covers it; UI_TILE is the strip/big tile */
    if (j->thumb_tex) {
        cui_image(ui, j->thumb_tex, .width = CUI_PX(40), .height = CUI_PX(40), .radius = UI_R_CONTROL);
    } else {
        cui_box(ui, .width = CUI_PX(40), .height = CUI_PX(40), .radius = UI_R_CONTROL,
                .bg = CUI_COLOR_SURFACE_MUTED,
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_icon(ui, CUI_ICON_IMAGE, UI_ICON_MD, CUI_COLOR_TEXT_MUTED);
        }
    }
    cui_column(ui, .width = CUI_FILL, .gap = 1) {
        cui_text(ui, j->name, .font = CUI_FONT_MEDIUM, .size = UI_FS_BODY, .nowrap = true);
        cui_text(ui, params, .size = UI_FS_CAPTION, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
    }
    if (j->status == JOB_RUNNING && !j->cancel_sent) {
        if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "取消这张图")) {
            cancel = true;
        }
    } else {
        cui_badge(ui, status_label(j->status), .tone = status_tone(j->status), .size = CUI_SIZE_SM,
                  .dot = j->status == JOB_RUNNING);
    }
    st->r_queue[index] = cui_last_rect(ui);
    cui_card_end(ui);
    cui_pop_id(ui);

    if (cancel && j->status == JOB_RUNNING) {
        j->cancel_sent = true;
        dn_cancel(j->id);
    } else if (clicked && !selected) {
        job_focus(st, index);
    }
}

/* Slider row: label left, value right, neutral track; the accent appears only
 * while the handle is under the pointer. CUI's own slider paints the fill in
 * the accent colour, which turned the whole sidebar red. */
typedef struct slider_ctx {
    float *value;
    float lo, hi, step;
    const char *fmt;
    bool dragging;
} slider_ctx;

static void slider_paint(cui_ctx *ui, cui_rect r, void *user) {
    slider_ctx *sc = (slider_ctx *)user;
    float t = (*sc->value - sc->lo) / (sc->hi - sc->lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float inset = 6.0f, track_h = 4.0f;
    float x0 = r.x + inset, x1 = r.x + r.w - inset;
    float y = r.y + (r.h - track_h) * 0.5f;
    cui_draw_rect(ui, (cui_rect){ x0, y, x1 - x0, track_h }, CUI_CORNERS(2.0f), CUI_COLOR_BORDER);
    float handle_x = x0 + (x1 - x0) * t;
    cui_draw_rect(ui, (cui_rect){ x0, y, handle_x - x0, track_h }, CUI_CORNERS(2.0f),
                  CUI_COLOR_BORDER_STRONG);
    bool hot = cui_mouse_pos(ui).x >= r.x && cui_mouse_pos(ui).x <= r.x + r.w &&
               cui_mouse_pos(ui).y >= r.y && cui_mouse_pos(ui).y <= r.y + r.h;
    cui_draw_circle(ui, (cui_vec2){ handle_x, r.y + r.h * 0.5f }, 6.0f,
                    (hot || sc->dragging) ? CUI_COLOR_ACCENT : CUI_COLOR_FOREGROUND);
    cui_draw_circle(ui, (cui_vec2){ handle_x, r.y + r.h * 0.5f }, 4.5f, CUI_COLOR_SURFACE);
}

static bool ui_slider_row(cui_ctx *ui, const char *id, const char *label, float *value,
                          float lo, float hi, float step, const char *fmt) {
    bool changed = false;
    char text[32];
    snprintf(text, sizeof text, fmt ? fmt : "%.0f", *value);
    cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, label, .size = UI_FS_BODY, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        cui_spacer(ui);
        cui_text(ui, text, .size = UI_FS_BODY, .tabular = true, .nowrap = true);
    }

    static const char *drag_id;   /* which slider owns the drag */
    slider_ctx sc = { value, lo, hi, step, fmt, drag_id == id };
    cui_canvas_result cr = cui_canvas(ui, id, CUI_FILL, CUI_PX(22), slider_paint, &sc, sizeof sc);
    if (cr.pressed && !drag_id) drag_id = id;
    if (!cui_mouse_down(ui, CUI_MOUSE_LEFT) && drag_id == id) drag_id = NULL;
    if (drag_id == id && cr.rect.w > 1.0f) {
        float t = (cui_mouse_pos(ui).x - (cr.rect.x + 6.0f)) / (cr.rect.w - 12.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float v = lo + (hi - lo) * t;
        if (step > 0.0f) v = lo + floorf((v - lo) / step + 0.5f) * step;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (fabsf(v - *value) > 1e-6f) { *value = v; changed = true; }
    }
    return changed;
}

void ui_sidebar(cui_ctx *ui, app_state *st) {
    /* Two segments, not three stacked groups: parameters grow with their content
     * and stop where it ends; the queue is a result list, so it keeps a fixed
     * share of the pane and carries its own surface and hairline. Groups sit
     * UI_GAP apart, their own rows UI_GAP_SM apart. */
    cui_column(ui, .width = CUI_PX(UI_SIDEBAR), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad = UI_PANE_PAD, .gap = UI_GAP) {
        /* --- segment 1: parameters (content height) --- */
        /* --- strength --- */
        cui_column(ui, .width = CUI_FILL, .gap = UI_GAP_SM) {
            cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "去噪强度", .font = CUI_FONT_MEDIUM, .size = UI_FS_LABEL,
                         .color = CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
                if (st->preset == PRESET_CUSTOM)
                    cui_badge(ui, "自定义", .size = CUI_SIZE_SM, .tone = CUI_TONE_WARNING);
            }
            static const char *names[3] = { "轻柔", "推荐", "强力" };
            int seg = st->preset <= 2 ? st->preset : 0;
            bool hit_preset = cui_segmented(ui, "preset", names, 3, &seg, .full_width = true, .size = CUI_SIZE_SM);
            st->r_preset = cui_last_rect(ui);
            if (hit_preset) {
                apply_preset(st, seg);
                settings_from_state(st);
            }
            cui_text(ui, st->preset <= 2 ? PRESETS[st->preset].hint : "自定义参数：下面两个滑块决定。",
                     .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);

            int d0 = st->down_pct;
            float l0_0 = st->l0;
            float down_f = (float)st->down_pct;
            if (ui_slider_row(ui, "s_down", "预缩比例", &down_f, 10.0f, 100.0f, 5.0f, "%.0f%%")) {
                st->down_pct = (int)(down_f + 0.5f);
            }
            ui_slider_row(ui, "s_l0", "L0 平滑", &st->l0, 0.0f, 0.005f, 0.0001f, "%.4f");
            if (st->down_pct != d0 || fabsf(st->l0 - l0_0) > 5e-5f) {
                st->preset = preset_of(st->down_pct, st->l0);
                settings_from_state(st);
            }
        }

        cui_separator(ui);

        /* --- brush --- */
        cui_column(ui, .width = CUI_FILL, .gap = UI_GAP_SM) {
            cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "局部保护", .font = CUI_FONT_MEDIUM, .size = UI_FS_LABEL,
                         .color = CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
                cui_text(ui, "B 笔刷 · E 橡皮 · L 套索", .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);
            }
            /* brush rows only matter while a paint tool is on */
            if (st->tool == TOOL_VIEW) {
                cui_text(ui, "在画布工具行选择笔刷 / 橡皮 / 套索后可用。", .size = UI_FS_CAPTION,
                         .color = CUI_COLOR_TEXT_MUTED);
            } else {
            float s0 = st->brush_size, m0 = st->brush_mix, f0 = st->feather_px;
            ui_slider_row(ui, "s_brush", "笔刷大小", &st->brush_size, 2.0f, 300.0f, 1.0f, "%.0f px");
            ui_slider_row(ui, "s_mix", "恢复强度", &st->brush_mix, 0.0f, 100.0f, 5.0f, "%.0f%%");
            if (fabsf(st->brush_size - s0) > 0.01f || fabsf(st->brush_mix - m0) > 0.01f)
                settings_from_state(st);
            if (ui_slider_row(ui, "s_feather", "柔边", &st->feather_px, 0.0f, 150.0f, 2.0f, "%.0f px")) {
                job *j = job_selected(st);
                if (j) {
                    j->feather_px = (int)(st->feather_px + 0.5f);
                    j->mask_tex_dirty = true;
                    j->composite_pending = true;
                }
            }
            if (fabsf(st->feather_px - f0) > 0.01f) settings_from_state(st);
            }
        }

        /* the parameter block ends here; the spacer absorbs what the queue
         * segment does not take, so the queue stays docked to the pane's bottom */
        cui_spacer(ui);

        /* --- segment 2: queue (fixed share, own surface) --- */
        cui_column(ui, .id = "sidebar_queue", .width = CUI_FILL,
                   .height = CUI_PCT(UI_QUEUE_PCT), .bg = CUI_COLOR_SURFACE, .gap = 0) {
            cui_separator(ui);   /* the segment's top hairline */
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = UI_PANE_PAD,
                       .gap = UI_GAP_SM) {
                char count[48];
                snprintf(count, sizeof count, "%d", st->count);
                cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "队列", .font = CUI_FONT_MEDIUM, .size = UI_FS_LABEL,
                             .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_spacer(ui);
                    cui_badge(ui, count, .size = CUI_SIZE_SM);
                }
                if (st->count == 0) {
                    cui_text(ui, "还没有图片。拖入、粘贴或点下面的按钮添加。", .size = UI_FS_CAPTION,
                             .color = CUI_COLOR_TEXT_MUTED);
                } else {
                    /* cards lift 2pt on hover and cast a raised shadow; UI_GAP is the
                     * clearance docs/design.md §4.4 asks the scroll area to keep, so the
                     * lift is never cut off by the scroll clip (cui_card_opts has no clip). */
                    cui_scroll(ui, .id = "queue", .height = CUI_FILL, .gap = UI_GAP, .pad = UI_GAP,
                               .fade_edges = true) {
                        for (int i = 0; i < st->count; i++) ui_queue_row(ui, st, i);
                    }
                }

                cui_row(ui, .width = CUI_FILL, .gap = UI_GAP) {
                    if (cui_button(ui, "添加图片", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLUS,
                                   .shortcut = "Ctrl O")) {
                        st->want_browse = true;
                    }
                    if (cui_icon_button(ui, CUI_ICON_CLIPBOARD, .variant = CUI_BUTTON_OUTLINE,
                                        .tooltip = "粘贴剪贴板里的图片（Ctrl+V）")) {
                        st->want_paste = true;
                    }
                }
                if (cui_button(ui, "清空已完成", .variant = CUI_BUTTON_GHOST, .full_width = true,
                               .icon = CUI_ICON_TRASH_2, .disabled = st->count == 0)) {
                    st->want_clear = true;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Thumbnail strip                                                          */
/* ------------------------------------------------------------------------ */

void ui_strip(cui_ctx *ui, app_state *st) {
    /* the strip is its own surface: a hairline separates the file list from the
     * canvas instead of both sitting on the same white */
    cui_column(ui, .width = CUI_FILL, .height = CUI_PX(UI_STRIP), .bg = CUI_COLOR_SURFACE, .gap = 0) {
        cui_separator(ui);
        /* Every tile - thumbnail or "add" - is a child of one row, gets the same
         * CUI_PX(UI_TILE) box and starts at the same edge, so their frames (and
         * their top edge) are identical by construction, whatever the contents. */
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_x = UI_GAP,
                .pad_y = (UI_STRIP - UI_TILE - UI_HAIRLINE) * 0.5f, .gap = UI_GAP,
                .align = CUI_ALIGN_START) {
            cui_scroll(ui, .id = "strip", .width = CUI_FILL, .height = CUI_PX(UI_TILE),
                       .horizontal = true, .gap = UI_GAP) {
                /* the scroll container stacks children vertically, so the tiles
                 * need an explicit row inside it; that row is top-aligned in the
                 * viewport, which is exactly UI_TILE tall */
                cui_row(ui, .width = CUI_FIT, .height = CUI_PX(UI_TILE), .gap = UI_GAP,
                        .align = CUI_ALIGN_START) {
                    for (int i = 0; i < st->count; i++) {
                        job *j = &st->jobs[i];
                        cui_push_id_int(ui, 9000 + i);
                        tile_ctx tc = { .tex = j->thumb_tex, .is_add = false, .selected = st->selected == i };
                        char cid[32];
                        snprintf(cid, sizeof cid, "thumb_%d", i);
                        cui_canvas_result cr = cui_canvas(ui, cid, CUI_PX(UI_TILE), CUI_PX(UI_TILE),
                                                          tile_paint, &tc, sizeof tc);
                        if (cr.clicked && st->selected != i) job_focus(st, i);
                        cui_pop_id(ui);
                    }
                    /* the "add" tile is the same box as a thumbnail, only its contents differ */
                    tile_ctx add = { .tex = 0, .is_add = true, .selected = false };
                    if (cui_canvas(ui, "strip_add", CUI_PX(UI_TILE), CUI_PX(UI_TILE),
                                   tile_paint, &add, sizeof add).clicked) {
                        st->want_browse = true;
                    }
                    cui_tooltip(ui, "添加图片（Ctrl+O）");
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Status line                                                              */
/* ------------------------------------------------------------------------ */

void ui_status(cui_ctx *ui, app_state *st, job *j) {
    (void)st;
    cui_column(ui, .width = CUI_FILL, .gap = UI_GAP_SM) {
        if (j && j->status == JOB_RUNNING) {
            cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
                cui_progress(ui, j->progress, .width = CUI_FILL, .height = 4, .tone = CUI_TONE_ACCENT);
                char pct[16];
                snprintf(pct, sizeof pct, "%d%%", (int)(j->progress * 100.0f + 0.5f));
                cui_text(ui, j->cancel_sent ? "正在取消..." : pct, .size = UI_FS_CAPTION, .nowrap = true,
                         .tabular = true, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }

        char left[192] = "";
        if (j && j->status == JOB_DONE) {
            char when[32], l0[16];
            fmt_ms(j->ms, when, sizeof when);
            fmt_l0(j->res_l0, l0, sizeof l0);
            snprintf(left, sizeof left, "%d × %d · %s · 预缩 %d%% · L0 %s%s",
                     j->orig.w, j->orig.h, when, j->res_down_pct, l0,
                     j->sr_cached ? " · 复用超分" : "");
        } else if (j && j->status == JOB_RUNNING) {
            const char *phase = j->phase[0] ? j->phase : "处理中";
            if (strcmp(phase, "upscale") == 0) phase = "超分辨率重建";
            else if (strcmp(phase, "smooth") == 0) phase = "L0 平滑";
            else if (strcmp(phase, "decode") == 0) phase = "解码";
            else if (strcmp(phase, "prepare") == 0) phase = "准备模型";
            else if (strcmp(phase, "compose") == 0) phase = "合成保护区域";
            char l0[16];
            fmt_l0(j->l0, l0, sizeof l0);
            snprintf(left, sizeof left, "%s · 预缩 %d%% · L0 %s", phase, j->down_pct, l0);
        } else if (j && j->status == JOB_ERROR) {
            snprintf(left, sizeof left, "%s", j->error[0] ? j->error : "处理失败。");
        }

        cui_row(ui, .width = CUI_FILL, .gap = UI_GAP, .align = CUI_ALIGN_CENTER) {
            if (left[0]) {
                cui_text(ui, left, .size = UI_FS_CAPTION, .nowrap = true, .tabular = true,
                         .color = j && j->status == JOB_ERROR ? CUI_COLOR_DANGER : CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
            }
            const char *shown = j && j->status == JOB_DONE
                                    ? (j->mask_active ? j->out_protected : j->out)
                                    : (j ? j->path : "");
            cui_text(ui, shown, .size = UI_FS_CAPTION, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Dialogs                                                                  */
/* ------------------------------------------------------------------------ */

static void shortcut_row(cui_ctx *ui, const char *keys, const char *what) {
    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(176), .align = CUI_ALIGN_END) { cui_kbd(ui, keys); }
        cui_text(ui, what, .size = 12);
    }
}

static void ui_help_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_help) return;
    bool open = true;
    cui_dialog(ui, "帮助与快捷键", &open, .width = 560) {
        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            shortcut_row(ui, "Ctrl O", "添加图片");
            shortcut_row(ui, "Ctrl V", "粘贴剪贴板里的图片");
            shortcut_row(ui, "Ctrl S", "另存为");
            shortcut_row(ui, "Ctrl Enter", "用当前参数重新去噪");
            shortcut_row(ui, "Ctrl Z / Ctrl Shift Z", "撤销 / 重做笔刷");
            shortcut_row(ui, "1 / 2 / 3", "对比 / 原图 / 结果");
            shortcut_row(ui, "B / E / L", "笔刷 / 橡皮 / 套索");
            shortcut_row(ui, "左侧右键拖动", "临时当橡皮擦");
            shortcut_row(ui, "按住 Z", "临时查看原图");
            shortcut_row(ui, "按住 M", "临时查看保护区域");
            shortcut_row(ui, "F / 0", "适合窗口 / 1:1");
            shortcut_row(ui, "左右方向键", "上一张 / 下一张");
            shortcut_row(ui, "Esc", "结束笔刷、取消套索");
            cui_separator(ui);
            cui_text(ui, "局部保护：笔刷涂过的地方恢复原图，柔边会自动过渡。"
                         "套索可以沿着线稿圈出一块区域。",
                     .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
        }
    }
    st->show_help = open;
}

static void ui_settings_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_settings) return;
    bool open = true;
    cui_dialog(ui, "设置", &open, .width = 620) {
        cui_column(ui, .width = CUI_FILL, .gap = 10) {
            cui_text(ui, "输出目录", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir(),
                         .size = 11.5f, .nowrap = true, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
                if (cui_button(ui, "更改", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FOLDER)) {
                    char path[PATH_CAP];
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_FOLDER, .title = "选择输出目录" }, path, sizeof path)) {
                        str_copy(st->set.output_dir, sizeof st->set.output_dir, path);
                        cfg_save(&st->set);
                    }
                }
                if (cui_button(ui, "打开", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) {
                    cui_app_open_url(st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir());
                }
            }

            cui_separator(ui);
            cui_text(ui, "推理引擎", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_text(ui, st->device, .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED);
            if (st->engine_error[0]) {
                cui_text(ui, st->engine_error, .size = 11.5f, .color = CUI_COLOR_DANGER);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "设备", .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                const char *dev_names[3] = { "自动", "GPU", "CPU" };
                for (int d = 0; d < 3; d++) {
                    bool on = st->set.device == d;
                    if (cui_button(ui, dev_names[d],
                                   .variant = on ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM)) {
                        if (st->set.device != d) {
                            st->set.device = d;
                            cfg_save(&st->set);
                            /* a device switch needs a clean engine: same
                             * path as the retry button */
                            st->want_restart = true;
                        }
                    }
                }
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "重启引擎", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_REFRESH_CW)) {
                    st->want_restart = true;
                }
                if (cui_button(ui, "打开模型目录", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FOLDER_OPEN)) {
                    cui_app_open_url(dn_models_dir());
                }
                if (cui_button(ui, "打开日志", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FILE_TEXT)) {
                    cui_app_open_url(cfg_log_dir());
                }
            }

            cui_separator(ui);
            cui_text(ui, "更新", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                const char *state_text = "未检查";
                switch (st->upd.state) {
                case UPD_CHECKING: state_text = "正在检查..."; break;
                case UPD_UP_TO_DATE: state_text = "已是最新版本"; break;
                case UPD_AVAILABLE: state_text = "有新版本"; break;
                case UPD_DOWNLOADING: state_text = "正在下载..."; break;
                case UPD_READY: state_text = "更新已就绪"; break;
                case UPD_FAILED: state_text = "检查失败"; break;
                default: break;
                }
                cui_text(ui, state_text, .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, APP_VERSION, .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED);
                cui_spacer(ui);
                if (cui_button(ui, "检查更新", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_REFRESH_CW)) {
                    upd_check(true);
                }
            }
            if (cui_checkbox(ui, "启动时自动检查更新", &st->auto_update)) {
                st->set.auto_check_updates = st->auto_update ? 1 : 0;
                cfg_save(&st->set);
            }

            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "导出诊断包", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_BUG)) {
                    char out[PATH_CAP];
                    if (log_export_diagnostics(out, sizeof out)) {
                        toast(st, "诊断包已生成");
                        cui_app_open_url(out);
                    } else {
                        toast(st, "诊断包生成失败");
                    }
                }
                if (cui_button(ui, "关于", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_INFO)) {
                    st->show_about = true;
                }
            }
        }
    }
    st->show_settings = open;
}

static void ui_about_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_about) return;
    bool open = true;
    cui_dialog(ui, "关于 Anime Denoise", &open, .width = 520) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_textf(ui, "版本 %s", APP_VERSION);
            cui_text(ui, "去除 GPT 生图在二次元画风里留下的鱼鳞纹。所有推理都在本机完成。",
                     .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_separator(ui);
            cui_text(ui, "去噪算法：gpt-anime-denoise（ESRGAN + L0 平滑）", .size = 11.5f);
            cui_text(ui, "界面：CUI (MIT)", .size = 11.5f);
            cui_text(ui, "图标：Lucide (ISC)", .size = 11.5f);
            cui_text(ui, "字体：HarmonyOS Sans（华为设备有限公司授权）", .size = 11.5f);
            cui_separator(ui);
            if (cui_button(ui, "查看许可证目录", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_FILE_TEXT)) {
                char dir[PATH_CAP];
                path_join(dir, sizeof dir, cfg_exe_dir(), "assets\\fonts");
                cui_app_open_url(dir);
            }
        }
    }
    st->show_about = open;
}

static void ui_update_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_update) return;
    bool open = true;
    cui_dialog(ui, "软件更新", &open, .width = 620) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            if (st->upd.state == UPD_AVAILABLE || st->upd.state == UPD_DOWNLOADING) {
                cui_textf(ui, "发现新版本 %s（当前 %s）", st->upd.latest, APP_VERSION);
                if (st->upd.notes[0]) {
                    cui_scroll(ui, .id = "upd_notes", .height = CUI_PX(220)) {
                        cui_text(ui, st->upd.notes, .size = 12);
                    }
                }
                if (st->upd.state == UPD_DOWNLOADING) {
                    cui_progress(ui, st->upd.progress, .width = CUI_FILL, .height = 6);
                    char pct[32];
                    snprintf(pct, sizeof pct, "正在下载 %.0f%%", st->upd.progress * 100.0f);
                    cui_text(ui, pct, .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                } else if (cui_button(ui, "下载更新", .variant = CUI_BUTTON_PRIMARY,
                                      .icon = CUI_ICON_DOWNLOAD)) {
                    upd_download();
                }
            } else if (st->upd.state == UPD_READY) {
                cui_textf(ui, "版本 %s 已下载并校验完成。", st->upd.latest);
                cui_text(ui, "点击“立即更新”会关闭程序并运行安装包，安装完成后自动重新打开。",
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                if (cui_button(ui, "立即更新", .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_REFRESH_CW)) {
                    if (upd_run_installer()) st->want_quit = true;
                }
            } else if (st->upd.state == UPD_FAILED) {
                cui_text(ui, st->upd.error[0] ? st->upd.error : "检查更新失败。", .color = CUI_COLOR_DANGER);
                if (cui_button(ui, "重试", .variant = CUI_BUTTON_OUTLINE, .icon = CUI_ICON_REFRESH_CW)) {
                    upd_check(true);
                }
            } else {
                cui_text(ui, "正在检查更新...", .size = 12);
            }
        }
    }
    st->show_update = open;
}

/* ------------------------------------------------------------------------ */

/* One call for every dialog the app can show. */
void ui_dialogs(cui_ctx *ui, app_state *st) {
    ui_help_dialog(ui, st);
    ui_settings_dialog(ui, st);
    ui_about_dialog(ui, st);
    ui_update_dialog(ui, st);
}
