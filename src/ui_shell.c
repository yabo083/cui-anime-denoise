/* ui_shell.c -- the window chrome: the 40px title bar and the second-layer
 * surfaces (settings sheet, help/about/update dialogs).
 *
 * The title bar is hand-drawn inside a canvas for the same reason as the
 * floating layers: exact WinUI caption buttons (46x40, red close hover) and
 * a clickable filename need pixel control. Hit rects are published into
 * app_state and additionally to chrome_set_zones() so WM_NCHITTEST reports
 * HTCLIENT over them -- the rest of the band still drags the window.
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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* Own copy of the press-edge tracker (see ui_float.c's note). */
static bool tb_pressed(cui_ctx *ui) {
    static bool was;
    bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
    bool hit = down && !was;
    was = down;
    return hit;
}

static cui_color text1(cui_ctx *ui) { return cui_token_color(ui, CUI_TOKEN_FOREGROUND); }

/* ------------------------------------------------------------------------ */
/* Title bar                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct title_ctx { app_state *st; } title_ctx;

/* One caption button: state layer on hover, Fluent red for close. */
static void caption_draw(cui_ctx *ui, cui_rect r, const char *icon, bool is_close) {
    cui_vec2 m = cui_mouse_pos(ui);
    int s = ui_hit(r, m) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? 2 : 1) : 0;
    if (s && is_close) {
        cui_draw_rect(ui, r, CUI_CORNERS(0),
                      s == 2 ? CUI_RGBA(0xA52F1Bff) : CUI_RGBA(0xC42B1Cff));
        cui_draw_icon(ui, icon, r, 15.0f, CUI_RGBA(0xffffffff));
        return;
    }
    if (s == 1) cui_draw_rect(ui, r, CUI_CORNERS(0), fl_hover(ui));
    else if (s == 2) cui_draw_rect(ui, r, CUI_CORNERS(0), fl_press(ui));
    cui_draw_icon(ui, icon, r, 15.0f,
                  chrome_is_active() ? text1(ui) : fl_text3(ui));
}

static void titlebar_paint(cui_ctx *ui, cui_rect r, void *user) {
    app_state *st = ((title_ctx *)user)->st;
    job *j = job_selected(st);

    cui_draw_rect(ui, r, CUI_CORNERS(0), cui_token_color(ui, CUI_TOKEN_BACKGROUND));
    cui_draw_rect(ui, (cui_rect){ r.x, r.y + r.h - 1.0f, r.w, 1.0f }, CUI_CORNERS(0),
                  fl_border(ui));

    float zones[24 * 4];
    int zn = 0;
#define ZONE(rect) do { \
        cui_rect _z = (rect); \
        if (zn < 24) { zones[zn*4] = _z.x; zones[zn*4+1] = _z.y; \
                       zones[zn*4+2] = _z.w; zones[zn*4+3] = _z.h; zn++; } \
    } while (0)

    /* left: icon + name + filename link */
    float x = r.x + UI_S12;
    float cy = r.y + r.h * 0.5f;
    cui_draw_icon(ui, CUI_ICON_SPARKLES, (cui_rect){ x, cy - 8, 16, 16 }, 16.0f,
                  cui_token_color(ui, CUI_TOKEN_ACCENT));
    x += 22.0f;
    cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, APP_TITLE, -1);
    cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, (cui_vec2){ x, cy - tm.height * 0.5f },
                  text1(ui), APP_TITLE, -1);
    x += tm.width + UI_S16;

    if (j) {
        cui_text_metrics fn = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, j->name, -1);
        cui_rect link = { x - 6.0f, cy - 12.0f, fn.width + 12.0f, 24.0f };
        bool hot = ui_hit(link, cui_mouse_pos(ui));
        if (hot) cui_draw_rect(ui, link, CUI_CORNERS(UI_R_MD), fl_hover(ui));
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                      (cui_vec2){ x, cy - fn.height * 0.5f },
                      hot ? cui_token_color(ui, CUI_TOKEN_ACCENT) : fl_text3(ui), j->name, -1);
        if (hot)
            cui_draw_rect(ui, (cui_rect){ x, cy + fn.height * 0.5f + 1.0f, fn.width, 1.0f },
                          CUI_CORNERS(0), cui_token_color(ui, CUI_TOKEN_ACCENT));
        st->r_title_name = link;
        ZONE(link);
        x += fn.width + UI_S12;
    } else {
        st->r_title_name = (cui_rect){ 0, 0, 0, 0 };
    }

    /* right: [device pill] [update] [help] [settings]   [min][max][×] */
    float cx0 = r.x + r.w - UI_CAPTION_BUTTONS;
    st->r_close = (cui_rect){ cx0 + UI_CAPTION_W * 2.0f, r.y, UI_CAPTION_W, UI_TITLEBAR };
    st->r_max   = (cui_rect){ cx0 + UI_CAPTION_W,        r.y, UI_CAPTION_W, UI_TITLEBAR };
    st->r_min   = (cui_rect){ cx0,                        r.y, UI_CAPTION_W, UI_TITLEBAR };
    ZONE(st->r_min); ZONE(st->r_max); ZONE(st->r_close);
    caption_draw(ui, st->r_min, CUI_ICON_MINUS, false);
    caption_draw(ui, st->r_max,
                 chrome_is_maximized() ? ICON_MINIMIZE : ICON_MAXIMIZE, false);
    caption_draw(ui, st->r_close, CUI_ICON_X, true);

    float rx = cx0 - UI_S8;
    /* settings + help icon buttons (28x28) */
    cui_rect ib = { rx - UI_ICONBTN_SM, cy - UI_ICONBTN_SM * 0.5f, UI_ICONBTN_SM, UI_ICONBTN_SM };
    st->r_title_settings = ib;
    {
        cui_vec2 m = cui_mouse_pos(ui);
        int s = ui_hit(ib, m) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? 2 : 1) : 0;
        ui_iconbtn_draw(ui, ib, CUI_ICON_SETTINGS, s, st->show_settings);
    }
    ZONE(ib); rx = ib.x - UI_S4;
    ib.x = rx - UI_ICONBTN_SM;
    st->r_title_help = ib;
    {
        cui_vec2 m = cui_mouse_pos(ui);
        int s = ui_hit(ib, m) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? 2 : 1) : 0;
        ui_iconbtn_draw(ui, ib, CUI_ICON_CIRCLE_HELP, s, st->show_help);
    }
    ZONE(ib); rx = ib.x - UI_S4;

    /* update entry, only when there is something to show */
    st->r_title_update = (cui_rect){ 0, 0, 0, 0 };
    if (st->upd.state == UPD_AVAILABLE || st->upd.state == UPD_READY ||
        st->upd.state == UPD_DOWNLOADING) {
        ib.x = rx - UI_ICONBTN_SM;
        st->r_title_update = ib;
        cui_vec2 m = cui_mouse_pos(ui);
        int s = ui_hit(ib, m) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? 2 : 1) : 0;
        ui_iconbtn_draw(ui, ib, CUI_ICON_DOWNLOAD, s, false);
        if (st->upd.state != UPD_DOWNLOADING)
            cui_draw_circle(ui, (cui_vec2){ ib.x + ib.w - 5.0f, ib.y + 5.0f }, 3.0f,
                            cui_token_color(ui, CUI_TOKEN_ACCENT));
        ZONE(ib); rx = ib.x - UI_S4;
    }

    /* device pill: dot + name, warning tint when the engine fell back */
    {
        const char *name = st->device[0] ? st->device : "引擎";
        cui_text_metrics dm = cui_measure_text(ui, CUI_FONT_BODY, UI_FS_CAPTION, name, -1);
        float pw = dm.width + 30.0f;
        cui_rect dp = { rx - pw - UI_S4, cy - 12.0f, pw, 24.0f };
        cui_vec2 m = cui_mouse_pos(ui);
        bool hot = ui_hit(dp, m);
        cui_draw_rect_border(ui, dp, CUI_CORNERS(UI_R_FULL),
                             hot ? fl_hover(ui) : fl_card_bg(ui), fl_border(ui), 1.0f);
        bool warn = st->device_note[0] != 0;
        cui_color dot = st->engine_ready ? (warn ? cui_token_color(ui, CUI_TOKEN_WARNING)
                                               : cui_token_color(ui, CUI_TOKEN_SUCCESS))
                                         : cui_token_color(ui, CUI_TOKEN_DANGER);
        cui_draw_circle(ui, (cui_vec2){ dp.x + 12.0f, cy }, 4.0f, dot);
        cui_draw_text(ui, CUI_FONT_BODY, UI_FS_CAPTION,
                      (cui_vec2){ dp.x + 21.0f, cy - dm.height * 0.5f },
                      hot ? text1(ui) : fl_text2(ui), name, -1);
        if (warn)
            cui_draw_icon(ui, CUI_ICON_TRIANGLE_ALERT,
                          (cui_rect){ dp.x + dp.w - 8.0f, cy - 5.0f, 11, 11 }, 11.0f,
                          cui_token_color(ui, CUI_TOKEN_WARNING));
        st->r_title_device = dp;
        ZONE(dp);
    }

    chrome_set_zones(zones, zn);
#undef ZONE
}

void ui_titlebar(cui_ctx *ui, app_state *st) {
    title_ctx tc = { st };
    cui_canvas(ui, "titlebar", CUI_FILL, CUI_PX(UI_TITLEBAR), titlebar_paint, &tc, sizeof tc);
}

/* Reads last frame's title-bar hit rects; runs before the stage's own input
 * so a click on chrome never reaches the canvas. */
void ui_titlebar_input(app_state *st) {
    cui_ctx *ui = st->ui;
    cui_vec2 m = cui_mouse_pos(ui);
    if (!tb_pressed(ui)) return;
    if (ui_hit(st->r_close, m)) { chrome_close(); return; }
    if (ui_hit(st->r_max, m)) { chrome_toggle_maximize(); return; }
    if (ui_hit(st->r_min, m)) { chrome_minimize(); return; }
    if (ui_hit(st->r_title_settings, m)) { st->show_settings = true; return; }
    if (ui_hit(st->r_title_help, m)) { st->show_help = true; return; }
    if (ui_hit(st->r_title_update, m)) { st->show_update = true; return; }
    if (ui_hit(st->r_title_device, m)) {
        /* the device pill explains itself in the settings sheet; a note that
         * only shows on hover would hide the GPU->CPU fallback from the user */
        st->show_settings = true;
        return;
    }
    if (ui_hit(st->r_title_name, m)) {
        job *j = job_selected(st);
        if (j) app_reveal_in_explorer(j->path);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Settings sheet                                                            */
/* ------------------------------------------------------------------------ */

static void set_theme_mode(cui_ctx *ui, app_state *st, int mode) {
    st->set.theme_mode = mode;
    if (mode == 0) {
        cui_app_follow_system_theme(NULL, true);
    } else {
        cui_app_follow_system_theme(NULL, false);
        cui_set_theme(ui, mode == 1 ? CUI_THEME_LIGHT : CUI_THEME_DARK);
    }
    chrome_set_dark(cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cfg_save(&st->set);
}

static void ui_model_row(cui_ctx *ui, app_state *st, eng_model m, const char *label) {
    eng_model_state ms;
    memset(&ms, 0, sizeof ms);
    bool known = dn_model_status(m, &ms);
    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_column(ui, .width = CUI_FILL, .gap = 2) {
            cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 12);
            if (known && ms.available)
                cui_text(ui, "已就绪", .size = UI_FS_CAPTION, .color = CUI_COLOR_SUCCESS);
            else if (st->dl_busy[m])
                cui_text(ui, "正在下载...", .size = UI_FS_CAPTION, .color = CUI_COLOR_ACCENT);
            else
                cui_text(ui, "未下载（可选，换用更强的模型）", .size = UI_FS_CAPTION,
                         .color = CUI_COLOR_TEXT_MUTED);
        }
        if (st->dl_busy[m]) {
            float t = st->dl_total[m] ? (float)st->dl_done[m] / (float)st->dl_total[m] : 0.0f;
            cui_progress(ui, t, .width = CUI_PX(72), .height = 4);
        } else if (!known || !ms.available) {
            if (cui_button(ui, "下载", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_DOWNLOAD, .disabled = st->bench)) {
                st->dl_busy[m] = true;
                st->dl_done[m] = st->dl_total[m] = 0;
                dn_download_model(m);
            }
        }
    }
}

static void ui_settings_sheet(cui_ctx *ui, app_state *st) {
    if (!st->show_settings) return;
    bool open = true;
    cui_sheet(ui, "设置", &open, .side = CUI_SIDE_RIGHT, .size = UI_SHEET_W) {
        cui_column(ui, .width = CUI_FILL, .gap = 14) {
            /* 外观 */
            cui_text(ui, "外观", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            {
                static const char *themes[3] = { "跟随系统", "浅色", "深色" };
                int mode = st->set.theme_mode;
                if (cui_segmented(ui, "theme", themes, 3, &mode, .full_width = true,
                                  .size = CUI_SIZE_SM))
                    set_theme_mode(ui, st, mode);
            }
            cui_text(ui, "画布背景", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_text(ui, "毛玻璃用最近一次输出的模糊色做底",
                     .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
            {
                static const char *bgs[2] = { "纯色", "毛玻璃" };
                int bg = st->set.canvas_bg;
                if (cui_segmented(ui, "canvasbg", bgs, 2, &bg, .full_width = true,
                                  .size = CUI_SIZE_SM) && st->set.canvas_bg != bg) {
                    st->set.canvas_bg = bg;
                    settings_from_state(st);
                }
            }

            cui_separator(ui);
            /* 输出文件夹 */
            cui_text(ui, "输出文件夹", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_text(ui, st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir(),
                     .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
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
                    cui_app_open_url(st->set.output_dir[0] ? st->set.output_dir
                                                         : cfg_default_out_dir());
                }
            }

            cui_separator(ui);
            /* 保存文件名 */
            cui_text(ui, "保存文件名", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_text(ui, "另存为的默认名。变量：{app} {src} {date} {time} {suffix}",
                     .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
            if (cui_input(ui, "save_name", st->set.save_name, sizeof st->set.save_name,
                          .placeholder = "{app}_{date}-{time}", .size = CUI_SIZE_SM,
                          .width = CUI_FILL))
                settings_from_state(st);
            {
                char prev[256];
                app_save_default_name(st, job_selected(st), prev, sizeof prev);
                char line[280];
                snprintf(line, sizeof line, "预览：%s.png", prev);
                cui_text(ui, line, .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);
            }

            cui_separator(ui);
            /* 推理设备 */
            cui_text(ui, "推理设备", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            cui_text(ui, st->device, .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
            if (st->device_note[0])
                cui_text(ui, st->device_note, .size = UI_FS_CAPTION, .color = CUI_COLOR_WARNING);
            {
                static const char *devs[3] = { "自动", "GPU", "CPU" };
                int d = st->set.device;
                if (cui_segmented(ui, "device", devs, 3, &d, .full_width = true,
                                  .size = CUI_SIZE_SM) && st->set.device != d) {
                    st->set.device = d;
                    cfg_save(&st->set);
                    st->want_restart = true;
                }
            }
            if (st->engine_error[0])
                cui_text(ui, st->engine_error, .size = UI_FS_CAPTION, .color = CUI_COLOR_DANGER);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "重启引擎", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_REFRESH_CW))
                    st->want_restart = true;
                if (cui_button(ui, "模型目录", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FOLDER_OPEN))
                    cui_app_open_url(dn_models_dir());
            }

            cui_separator(ui);
            /* 模型管理 */
            cui_text(ui, "模型管理", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            ui_model_row(ui, st, ENG_M_CUGAN, "CUGAN");
            ui_model_row(ui, st, ENG_M_APISR, "APISR");

            cui_separator(ui);
            /* 更新与维护 */
            cui_text(ui, "更新与维护", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            if (cui_switch(ui, "启动时自动检查更新", &st->auto_update)) {
                st->set.auto_check_updates = st->auto_update ? 1 : 0;
                cfg_save(&st->set);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                const char *state_text = "未检查";
                switch (st->upd.state) {
                case UPD_CHECKING:    state_text = "正在检查..."; break;
                case UPD_UP_TO_DATE:  state_text = "已是最新"; break;
                case UPD_AVAILABLE:   state_text = "有新版本"; break;
                case UPD_DOWNLOADING: state_text = "正在下载..."; break;
                case UPD_READY:       state_text = "更新已就绪"; break;
                case UPD_FAILED:      state_text = "检查失败"; break;
                default: break;
                }
                cui_text(ui, state_text, .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
                if (cui_button(ui, "检查更新", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM))
                    upd_check(true);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "打开日志", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FILE_TEXT))
                    cui_app_open_url(cfg_log_dir());
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
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "版本 " APP_VERSION, .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);
                cui_spacer(ui);
                if (cui_button(ui, "关于", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_INFO))
                    st->show_about = true;
            }

            cui_separator(ui);
            if (cui_button(ui, "恢复默认设置", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_ROTATE_CCW)) {
                app_settings d;
                cfg_defaults(&d);
                st->set.theme_mode = d.theme_mode;
                st->set.device = d.device;
                st->set.output_dir[0] = 0;
                st->set.auto_check_updates = d.auto_check_updates;
                st->preset = DEFAULT_PRESET;
                st->down_pct = PRESETS[DEFAULT_PRESET].down_pct;
                st->l0 = PRESETS[DEFAULT_PRESET].l0;
                st->brush_size = d.brush_size;
                st->brush_mix = d.brush_mix;
                st->feather_px = d.feather_px;
                set_theme_mode(ui, st, st->set.theme_mode);
                cfg_save(&st->set);
                st->want_restart = true;
            }
        }
    }
    st->show_settings = open;
}

/* ------------------------------------------------------------------------ */
/* Dialogs                                                                   */
/* ------------------------------------------------------------------------ */

/* Alternatives separated by " / " become separate keycaps joined by a plain
 * 或 label: a bare "/" inside the string would render as its own cap and read
 * like a shortcut of its own. */
static void shortcut_row(cui_ctx *ui, const char *keys, const char *what) {
    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(176), .align = CUI_ALIGN_END) {
            cui_row(ui, .gap = 5, .align = CUI_ALIGN_CENTER) {
                const char *p = keys;
                for (;;) {
                    const char *sep = strstr(p, " / ");
                    char chunk[64];
                    if (sep) {
                        int n = (int)(sep - p);
                        if (n > (int)sizeof chunk - 1) n = (int)sizeof chunk - 1;
                        memcpy(chunk, p, (size_t)n);
                        chunk[n] = 0;
                    } else {
                        str_copy(chunk, sizeof chunk, p);
                    }
                    cui_kbd(ui, chunk);
                    if (!sep) break;
                    cui_text(ui, "或", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
                    p = sep + 3;
                }
            }
        }
        cui_text(ui, what, .size = 12);
    }
}

/* Same row, but the left column is plain text: gestures (右键拖动) are not
 * keys, and a keycap around a sentence would look like one weird key. */
static void shortcut_row_text(cui_ctx *ui, const char *label, const char *what) {
    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(176), .align = CUI_ALIGN_END) {
            cui_text(ui, label, .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_text(ui, what, .size = 12);
    }
}

static void ui_help_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_help) return;
    bool open = true;
    cui_dialog(ui, "帮助与快捷键", &open, .width = 560) {
        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            shortcut_row(ui, "Ctrl O / Ctrl V", "添加图片 / 粘贴剪贴板");
            shortcut_row(ui, "Ctrl C / Ctrl Shift C", "复制结果 / 复制原图");
            shortcut_row(ui, "Ctrl S", "另存为");
            shortcut_row(ui, "Ctrl Z / Ctrl Shift Z", "撤销 / 重做笔刷");
            shortcut_row(ui, "1 / 2 / 3 / X", "对比 / 原图 / 结果，X 互切");
            shortcut_row(ui, "Z / M", "按住查看原图 / 按住查看保护区");
            shortcut_row(ui, "B / E / L", "画笔 / 橡皮 / 磁性套索（再按退出）");
            shortcut_row(ui, "I", "反选保护区");
            shortcut_row(ui, "[ / ]", "调整笔刷大小（空格+滚轮也可）");
            shortcut_row_text(ui, "右键拖动", "临时当橡皮擦");
            shortcut_row_text(ui, "空格或中键拖动", "平移画布");
            shortcut_row(ui, "Enter", "闭合套索（双击/回起点亦可；Alt/右键=减选）");
            shortcut_row(ui, "Backspace", "删除套索锚点");
            shortcut_row(ui, "F / 0 / Ctrl +-", "适合窗口 / 1:1 / 缩放");
            shortcut_row_text(ui, "左右方向键", "上一张 / 下一张");
            shortcut_row(ui, "Esc", "放弃套索 · 退出保护 · 关浮层");
            cui_separator(ui);
            cui_text(ui, "局部保护：画笔涂过的地方保留原图，柔边自动过渡；"
                         "磁性套索沿线稿吸附。改预设或微调会立即重新去噪。",
                     .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
        }
    }
    st->show_help = open;
}

static void ui_about_dialog(cui_ctx *ui, app_state *st) {
    if (!st->show_about) return;
    bool open = true;
    cui_dialog(ui, "关于 Anime Denoise", &open, .width = 520) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_textf(ui, "版本 %s", APP_VERSION);
            cui_text(ui, "去除 AI 生图在二次元画风里留下的鱼鳞纹。所有推理都在本机完成。",
                     .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_separator(ui);
            cui_text(ui, "鸣谢：本项目是一个包装层——算法与交互来自 gpt-anime-denoise，"
                     "界面工具包是 CUI。", .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, "去噪算法：gpt-anime-denoise (MIT) —— ESRGAN + L0 平滑", .size = 11.5f);
            cui_text(ui, "界面：CUI (MIT)", .size = 11.5f);
            cui_text(ui, "图标：Lucide (ISC)", .size = 11.5f);
            cui_text(ui, "字体：Noto Sans SC（思源黑体，SIL OFL 子集）", .size = 11.5f);
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
                if (cui_button(ui, "重试", .variant = CUI_BUTTON_OUTLINE, .icon = CUI_ICON_REFRESH_CW))
                    upd_check(true);
            } else {
                cui_text(ui, "正在检查更新...", .size = 12);
            }
        }
    }
    st->show_update = open;
}

/* One call for every second-layer surface the app can show. */
void ui_dialogs(cui_ctx *ui, app_state *st) {
    ui_settings_sheet(ui, st);
    ui_help_dialog(ui, st);
    ui_about_dialog(ui, st);
    ui_update_dialog(ui, st);
}
