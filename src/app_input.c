/* app_input.c -- keyboard commands, drag & drop, paste, save-as and the file
 * dialogs. Everything here turns input into intent flags or job actions;
 * nothing draws (see AGENTS.md's routing table). */

#define _CRT_SECURE_NO_WARNINGS 1

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "config.h"
#include "img.h"
#include "mask.h"


static void jump_job(app_state *st, int delta) {
    if (st->count == 0) return;
    int idx = st->selected + delta;
    if (idx < 0) idx = st->count - 1;
    if (idx >= st->count) idx = 0;
    job_focus(st, idx);
}

void app_handle_keys(cui_ctx *ui, app_state *st) {
    job *j = job_selected(st);

    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) st->want_browse = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Q)) st->want_quit = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_V)) st->want_paste = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S)) st->want_save = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_S)) st->want_save_as = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_ENTER)) {
        if (j && j->status != JOB_RUNNING) { j->status = JOB_QUEUED; engine_submit_next(st); }
    }

    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Z)) {
        if (j && mask_undo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
    }
    if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_Z) ||
        cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Y)) {
        if (j && mask_redo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
    }

    if (cui_key_pressed(ui, CUI_KEY_1)) st->view = VIEW_COMPARE;
    if (cui_key_pressed(ui, CUI_KEY_2)) { st->view = VIEW_ORIGINAL; st->peek_orig = false; }
    if (cui_key_pressed(ui, CUI_KEY_3)) st->view = VIEW_RESULT;
    if (cui_key_pressed(ui, CUI_KEY_B)) st->tool = st->tool == TOOL_BRUSH ? TOOL_VIEW : TOOL_BRUSH;
    if (cui_key_pressed(ui, CUI_KEY_E)) st->tool = st->tool == TOOL_ERASE ? TOOL_VIEW : TOOL_ERASE;
    if (cui_key_pressed(ui, CUI_KEY_L)) {
        st->tool = st->tool == TOOL_LASSO ? TOOL_VIEW : TOOL_LASSO;
        if (st->tool == TOOL_LASSO && j) ensure_edges(j);
    }
    /* lasso: Enter closes, Backspace drops the last anchor (the tool itself
     * lives in ui_stage.c) */
    if (j && lasso_active(st)) {
        bool alt = (cui_key_mods(ui) & CUI_MOD_ALT) != 0;
        if (cui_key_pressed(ui, CUI_KEY_ENTER)) lasso_close_now(st, j, alt);
        if (cui_key_pressed(ui, CUI_KEY_BACKSPACE)) lasso_drop_anchor(st);
    }

    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->lasso_active) lasso_reset(st);
        else st->tool = TOOL_VIEW;
    }
    if (cui_key_pressed(ui, CUI_KEY_F) && j) stage_fit(j, st->stage);
    if (cui_key_pressed(ui, CUI_KEY_0) && j) {
        /* the view lives in image pixels (mask space), so 1:1 centres on the
         * image, not on the downscaled display texture */
        float iw = (float)(j->m.w ? j->m.w : j->disp_w);
        float ih = (float)(j->m.h ? j->m.h : j->disp_h);
        j->view.zoom = 1.0f;
        j->view.cx = iw * 0.5f;
        j->view.cy = ih * 0.5f;
        j->view_fitted = false;
    }
    if (cui_key_pressed(ui, CUI_KEY_LEFT)) jump_job(st, -1);
    if (cui_key_pressed(ui, CUI_KEY_RIGHT)) jump_job(st, +1);

    st->peek_orig = cui_key_down(ui, CUI_KEY_Z) && !cui_key_mods(ui);
    st->peek_mask = cui_key_down(ui, CUI_KEY_M);
    st->space_down = cui_key_down(ui, CUI_KEY_SPACE);
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

void app_handle_drops(cui_ctx *ui, app_state *st) {
    int n = cui_dropped_file_count(ui);
    int added = 0;
    for (int i = 0; i < n; i++) {
        const char *path = cui_dropped_file(ui, i);
        if (path && *path && img_supported_path(path)) {
            enqueue_path(st, path);
            added++;
        }
    }
    if (added) engine_submit_next(st);
}

void app_do_paste(app_state *st) {
    img_pixels px = {0};
    if (!img_clipboard_get(&px)) { toast(st, "剪贴板里没有图片"); return; }
    char dir[PATH_CAP], path[PATH_CAP];
    path_join(dir, sizeof dir, cfg_data_dir(), "projects");
    time_t t = time(NULL);
    struct tm lt;
    localtime_s(&lt, &t);
    char name[64];
    strftime(name, sizeof name, "pasted-%Y%m%d-%H%M%S.png", &lt);
    path_join(path, sizeof path, dir, name);
    bool ok = img_encode_png(path, &px);
    img_free(&px);
    if (!ok) { toast(st, "无法保存粘贴的图片"); return; }
    enqueue_path(st, path);
}

void app_do_save(app_state *st, bool save_as) {
    job *j = job_selected(st);
    if (!j || !j->has_result) { toast(st, "还没有可以保存的结果"); return; }
    const char *src = (j->mask_active && path_exists(j->out_protected)) ? j->out_protected : j->out;
    if (!path_exists(src)) { toast(st, "输出文件不在了，请重新去噪"); return; }

    char dir[PATH_CAP];
    str_copy(dir, sizeof dir, st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir());
    char name[256];
    str_copy(name, sizeof name, path_basename(src));

    if (!save_as) {
        /* still confirm the target: a desktop app never silently overwrites */
    }
    char out[PATH_CAP];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_SAVE;
    opts.title = "另存为";
    opts.filter_name = "PNG";
    opts.filter = "png";
    opts.default_name = name;
    if (!cui_app_file_dialog(NULL, &opts, out, sizeof out)) return;
    size_t n = strlen(out);
    if (n < 4 || _stricmp(out + n - 4, ".png") != 0) snprintf(out + n, sizeof out - n, ".png");
    if (file_copy(src, out)) {
        toast(st, "已保存");
        cui_app_open_url(out);
    } else {
        toast(st, "保存失败：目标文件可能正在被占用");
    }
    (void)dir;
}

void app_browse_dialog(cui_app *app, app_state *st) {
    char path[PATH_CAP];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_OPEN;
    opts.title = "选择要处理的图片";
    opts.filter_name = "图片";
    opts.filter = IMAGE_FILTER;
    if (cui_app_file_dialog(app, &opts, path, sizeof path)) enqueue_path(st, path);
}
