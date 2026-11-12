/* app_input.c -- keyboard commands, drag & drop, paste, save-as, the file
 * dialogs and protection-mode entry/exit. Everything here turns input into
 * intent flags or job actions; nothing draws (see AGENTS.md's routing table).
 * The key table is docs/fluent-redesign.md §8, kept one-to-one with the
 * original web app. */

#define _CRT_SECURE_NO_WARNINGS 1

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

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

/* ------------------------------------------------------------------------ */
/* Protection mode                                                          */
/* ------------------------------------------------------------------------ */

void brush_step(app_state *st, float factor) {
    float s = st->brush_size * factor;
    if (s < BRUSH_KEY_MIN) s = BRUSH_KEY_MIN;
    if (s > BRUSH_KEY_MAX) s = BRUSH_KEY_MAX;
    if (s != st->brush_size) { st->brush_size = s; settings_from_state(st); }
}

/* Enters the immersive mode: the pill sinks, the protect toolbar slides in.
 * The mask is snapshotted so 取消 can roll the whole session back in one
 * step (the web UI kept mask edits in the undo journal instead; both are
 * honest, and cancel-as-snapshot is cheaper to reason about). */
void protect_enter(app_state *st) {
    job *j = job_selected(st);
    if (!j || !j->m.px || st->protect) return;
    free(st->protect_backup);
    st->protect_backup = (uint8_t *)malloc((size_t)j->m.w * (size_t)j->m.h);
    if (st->protect_backup)
        memcpy(st->protect_backup, j->m.px, (size_t)j->m.w * (size_t)j->m.h);
    st->protect = true;
    st->tune_open = false;
    st->view_unprotected = false;
    if (st->tool == TOOL_VIEW) st->tool = TOOL_BRUSH;
}

/* Shared exit: ends any open stroke and drops the overlay state. */
void protect_exit(app_state *st) {
    job *j = job_selected(st);
    if (st->painting && j) {
        mask_stroke_end(&j->m, j->hist);
        st->painting = false;
        st->stroke_open = false;
        j->composite_pending = true;
    }
    lasso_reset(st);
    st->protect = false;
    st->tool = TOOL_VIEW;
    st->view_unprotected = false;
    st->drag_ctl = CTL_NONE;
    free(st->protect_backup);
    st->protect_backup = NULL;
}

/* 完成: keep the edits, make the pending composite flush on this frame. */
void protect_done(app_state *st) {
    job *j = job_selected(st);
    if (j && j->composite_pending) j->dirty_at = 0.0;
    protect_exit(st);
}

/* 取消: put the entry snapshot back and re-composite so the written PNG and
 * the preview return to what they were before the session -- a cancel that
 * silently keeps earlier strokes would not be a cancel. */
void protect_cancel(app_state *st) {
    job *j = job_selected(st);
    if (j && j->m.px && st->protect_backup) {
        memcpy(j->m.px, st->protect_backup, (size_t)j->m.w * (size_t)j->m.h);
        mask_note(st, j);
        j->composite_pending = true;
        j->dirty_at = 0.0;
    }
    protect_exit(st);
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                     */
/* ------------------------------------------------------------------------ */

void app_handle_keys(cui_ctx *ui, app_state *st) {
    job *j = job_selected(st);
    int mods = cui_key_mods(ui);

    /* typing in a settings field must not fire canvas shortcuts */
    if (cui_wants_keyboard(ui)) {
        st->peek_orig = st->peek_mask = st->space_down = false;
        return;
    }

    /* --- file ops --- */
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) st->want_browse = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Q)) st->want_quit = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_V)) st->want_paste = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S) ||
        cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_S))
        st->want_save = true;

    /* --- mask history --- */
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Z)) {
        if (j && mask_undo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
    }
    if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_Z) ||
        cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Y)) {
        if (j && mask_redo(&j->m, j->hist)) { mask_note(st, j); j->composite_pending = true; }
    }

    /* --- view modes: 1/2/3 pick one, X toggles original<->result --- */
    if (!mods) {
        if (cui_key_pressed(ui, CUI_KEY_1)) st->view = VIEW_COMPARE;
        if (cui_key_pressed(ui, CUI_KEY_2)) st->view = VIEW_ORIGINAL;
        if (cui_key_pressed(ui, CUI_KEY_3)) st->view = VIEW_RESULT;
        if (cui_key_pressed(ui, CUI_KEY_X) && j)
            st->view = st->view == VIEW_ORIGINAL ? VIEW_RESULT : VIEW_ORIGINAL;
        if (cui_key_pressed(ui, CUI_KEY_F) && j) stage_fit(j, st->stage);
        if (cui_key_pressed(ui, CUI_KEY_0) && j) {
            /* the view lives in image pixels (mask space), so 1:1 centres on
             * the image, not on the downscaled display texture */
            j->view.zoom = 1.0f;
            j->view.cx = stage_image_w(j) * 0.5f;
            j->view.cy = stage_image_h(j) * 0.5f;
            j->view_fitted = false;
        }
        if (cui_key_pressed(ui, CUI_KEY_LEFT)) jump_job(st, -1);
        if (cui_key_pressed(ui, CUI_KEY_RIGHT)) jump_job(st, +1);
    }
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_EQUAL)) stage_zoom_center(st, j, 1.25f);
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_MINUS)) stage_zoom_center(st, j, 0.8f);
    if (cui_key_pressed(ui, CUI_KEY_SLASH) && (mods == 0 || (mods & CUI_MOD_SHIFT)))
        st->show_help = true;
    if (cui_key_pressed(ui, CUI_KEY_F1)) st->show_help = true;

    /* --- protection mode tools (B/E/L toggle, I inverts) --- */
    if (!mods && j) {
        if (cui_key_pressed(ui, CUI_KEY_B)) {
            if (!st->protect) { protect_enter(st); st->tool = TOOL_BRUSH; }
            else if (st->tool == TOOL_BRUSH) protect_done(st);
            else { lasso_reset(st); st->tool = TOOL_BRUSH; }
        }
        if (cui_key_pressed(ui, CUI_KEY_E)) {
            if (!st->protect) { protect_enter(st); st->tool = TOOL_ERASE; }
            else if (st->tool == TOOL_ERASE) protect_done(st);
            else { lasso_reset(st); st->tool = TOOL_ERASE; }
        }
        if (cui_key_pressed(ui, CUI_KEY_L)) {
            if (!st->protect) { protect_enter(st); st->tool = TOOL_LASSO; }
            else if (st->tool == TOOL_LASSO) protect_done(st);
            else { lasso_reset(st); st->tool = TOOL_LASSO; }
            if (st->tool == TOOL_LASSO) ensure_edges(j);
        }
        if (cui_key_pressed(ui, CUI_KEY_I) && st->protect) {
            if (st->lasso_active) lasso_close_now(st, j, false);
            if (!j->mask_active) {
                toast(st, "还没有保护区：先圈住要保留的部分再反选");
            } else {
                mask_stroke_begin(&j->m, j->hist);
                mask_invert(&j->m);
                mask_stroke_end(&j->m, j->hist);
                mask_note(st, j);
                j->composite_pending = true;
                toast(st, "已反选（Ctrl+Z 可撤销）");
            }
        }
    }

    /* --- lasso: Enter closes (Alt = subtract), Backspace drops an anchor --- */
    if (j && lasso_active(st)) {
        bool alt = (mods & CUI_MOD_ALT) != 0;
        if (cui_key_pressed(ui, CUI_KEY_ENTER)) lasso_close_now(st, j, alt);
        if (cui_key_pressed(ui, CUI_KEY_BACKSPACE)) lasso_drop_anchor(st);
    }

    /* Esc peels layers in order: abandon the lasso, leave protection (edits
     * keep -- that's what leaving brush mode meant in the web UI), then
     * dismiss the floating card (spec §8). */
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->lasso_active) lasso_reset(st);
        else if (st->protect) protect_done(st);
        else if (st->tune_open) st->tune_open = false;
        else if (st->tool != TOOL_VIEW) st->tool = TOOL_VIEW;
    }

    /* --- holds --- */
    st->peek_orig = cui_key_down(ui, CUI_KEY_Z) && mods == 0;
    st->peek_mask = cui_key_down(ui, CUI_KEY_M) && mods == 0;
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
    toast(st, "已粘贴剪贴板图片");
    enqueue_path(st, path);
}

/* Expands st->set.save_name into a dialog default. Tokens: {app} the product
 * name, {src} the source file's stem, {date}/{time} the local save moment,
 * {suffix} "_protected" when the result carries a protection mask. Anything
 * Windows filenames cannot contain becomes an underscore, a stray ".png"
 * tail is dropped (the dialog adds it back), and an empty expansion falls
 * back to the built-in default. */
void app_save_default_name(const app_state *st, const job *j, char *out, size_t cap) {
    const char *tpl = st->set.save_name;
    char date[16], clock_[16];
    time_t t = time(NULL);
    struct tm lt;
    localtime_s(&lt, &t);
    strftime(date, sizeof date, "%Y%m%d", &lt);
    strftime(clock_, sizeof clock_, "%H%M%S", &lt);

    size_t n = 0;
    for (const char *p = tpl; *p && n + 1 < cap; ) {
        const char *val = NULL;
        size_t tok = 0;
        if (strncmp(p, "{app}", 5) == 0)      { val = "AnimeDenoise"; tok = 5; }
        else if (strncmp(p, "{src}", 5) == 0) { val = j ? j->stem : "image"; tok = 5; }
        else if (strncmp(p, "{date}", 6) == 0) { val = date; tok = 6; }
        else if (strncmp(p, "{time}", 6) == 0) { val = clock_; tok = 6; }
        else if (strncmp(p, "{suffix}", 8) == 0) {
            val = (j && j->mask_active) ? "_protected" : ""; tok = 8;
        }
        if (tok) {
            p += tok;
            for (; *val && n + 1 < cap; val++) out[n++] = *val;
            continue;
        }
        char c = *p++;
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
        out[n++] = c;
    }
    out[n] = 0;
    /* no trailing dots or spaces, no double ".png" */
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) out[--n] = 0;
    if (n >= 4 && _stricmp(out + n - 4, ".png") == 0) out[n - 4] = 0;
    if (!out[0]) str_copy(out, cap, "AnimeDenoise");
}

void app_do_save(app_state *st, bool save_as) {
    job *j = job_selected(st);
    (void)save_as;   /* every save here asks for a target: 另存为 semantics */
    if (!j || !j->has_result) { toast(st, "还没有可以保存的结果"); return; }
    const char *src = (j->mask_active && path_exists(j->out_protected)) ? j->out_protected : j->out;
    if (!path_exists(src)) { toast(st, "输出文件不在了，请重新去噪"); return; }

    char name[256];
    app_save_default_name(st, j, name, sizeof name);

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

/* explorer /select,"path" -- the title bar's filename link. */
void app_reveal_in_explorer(const char *utf8_path) {
    if (!utf8_path || !*utf8_path) return;
    wchar_t w[PATH_CAP * 2], args[PATH_CAP * 2 + 32];
    utf8_to_wide(utf8_path, w, PATH_CAP * 2);
    _snwprintf(args, PATH_CAP * 2 + 32, L"/select,\"%s\"", w);
    args[PATH_CAP * 2 + 31] = 0;
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOW);
}
