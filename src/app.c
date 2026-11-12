/* Anime Denoise -- native front end for the GPT anime de-screening model.
 *
 * CUI immediate-mode UI in C, inference on the in-process engine thread
 * (denoiser.h): the ONNX Runtime + DirectML engine runs behind one
 * below-normal-priority thread and answers through an event ring.
 *
 * This file owns state, the denoiser event drain and the frame itself.
 * Job lifetime and submission live in app_jobs.c, keyboard/file input in
 * app_input.c, drawing in ui_shell.c / ui_stage.c / ui_float.c / ui_lasso.c
 * (AGENTS.md routing table).
 *
 * Threading / blocking rules:
 *   - denoiser events are drained at the top of every frame, before anything
 *     is drawn, so a frame always shows post-event state;
 *   - the potentially blocking calls (engine bring-up, the file dialog) are
 *     deferred to the end of a frame through intent flags, so the window has
 *     already painted a "working on it" frame when they run;
 *   - the protection mask is composited on the host: the full-res denoised
 *     pixels live in the job (eng_image), mask_feather produces the weight
 *     map and img_composite_masked applies the worker's exact blend;
 *   - writing the output PNG is a host job: result RGB + source alpha + the
 *     ICC profile the loader lifted with img_decode_file_ex.
 *
 * Layout (docs/fluent-redesign.md -- canvas first):
 *   title bar 40   (brand, filename link, device pill, gear, ?, captions)
 *   canvas   FILL  (the image; pill/tray/protect/tune float inside it)
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "app.h"
#include "config.h"
#include "img.h"
#include "loader.h"
#include "mask.h"
#include "update.h"

#ifndef APP_VERSION
#define APP_VERSION "1.1.0"
#endif



const preset_def PRESETS[3] = {
    { "轻柔", "去噪最轻，留住最多细节和淡阴影。", 80, 0.0006f },
    { "推荐", "去噪和细节平衡，适合大多数二次元图。", 60, 0.0010f },
    { "强力", "纹理去得最干净，淡渐变和细节也损失最多。", 45, 0.0018f },
};

/* ------------------------------------------------------------------------ */
/* Small utilities                                                          */
/* ------------------------------------------------------------------------ */

double now_seconds(void) {
    static LARGE_INTEGER freq;
    static bool have;
    if (!have) { QueryPerformanceFrequency(&freq); have = true; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void str_copy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

const char *path_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

void path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t n = dir ? strlen(dir) : 0;
    if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        snprintf(out, cap, "%s%s", dir, leaf);
    else
        snprintf(out, cap, "%s\\%s", dir, leaf);
}

void path_stem(char *out, size_t cap, const char *name) {
    str_copy(out, cap, name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

void utf8_to_wide(const char *s, wchar_t *out, int cap) {
    if (!s || !out || cap <= 0) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) <= 0) out[0] = 0;
}

void wide_to_utf8(const wchar_t *s, char *out, int cap) {
    if (!s || !out || cap <= 0) return;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

bool path_exists(const char *utf8) {
    wchar_t w[PATH_CAP * 2];
    utf8_to_wide(utf8, w, PATH_CAP * 2);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

bool file_copy(const char *from, const char *to) {
    wchar_t a[PATH_CAP * 2], b[PATH_CAP * 2];
    utf8_to_wide(from, a, PATH_CAP * 2);
    utf8_to_wide(to, b, PATH_CAP * 2);
    return CopyFileW(a, b, FALSE) != 0;
}

void fmt_ms(int ms, char *buf, size_t cap) {
    if (ms <= 0) snprintf(buf, cap, "-");
    else if (ms < 1000) snprintf(buf, cap, "%d 毫秒", ms);
    else snprintf(buf, cap, "%.1f 秒", (double)ms / 1000.0);
}

/* 0.0018 -> "0.0018", 0.001 -> "0.001", 0 -> "0.0" (the web UI's fmtL0). */
void fmt_l0(float v, char *buf, size_t cap) {
    snprintf(buf, cap, "%.4f", (double)v);
    char *end = buf + strlen(buf);
    while (end > buf && end[-1] == '0') *--end = 0;
    if (end > buf && end[-1] == '.') snprintf(end, (size_t)(cap - (size_t)(end - buf)), "0");
}

void toast(app_state *st, const char *msg) {
    /* the buffer stays: tests read it; the visible toast is CUI's own */
    str_copy(st->toast, sizeof st->toast, msg);
    st->toast_until = now_seconds() + 3.5;
    if (st->ui) cui_toast(st->ui, msg, .tone = CUI_TONE_INFO, .duration = 3.5f);
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                 */
/* ------------------------------------------------------------------------ */

void apply_preset(app_state *st, int index) {
    if (index < 0 || index > 2) return;
    st->preset = index;
    st->down_pct = PRESETS[index].down_pct;
    st->l0 = PRESETS[index].l0;
}

int preset_of(int down_pct, float l0) {
    for (int i = 0; i < 3; i++)
        if (PRESETS[i].down_pct == down_pct && fabsf(PRESETS[i].l0 - l0) < 1e-6f) return i;
    return PRESET_CUSTOM;
}

/* Marks the settings dirty; the frame loop writes them once the user stops
 * moving things (500 ms), so dragging a slider does not hit the disk per frame. */
void settings_from_state(app_state *st) {
    st->settings_dirty = true;
    st->settings_dirty_at = now_seconds();
}

static void settings_flush(app_state *st, bool force) {
    if (!st->settings_dirty) return;
    if (!force && now_seconds() - st->settings_dirty_at < 0.5) return;
    app_settings *s = &st->set;
    s->preset = st->preset;
    s->down_pct = st->down_pct;
    s->l0 = st->l0;
    s->brush_size = st->brush_size;
    s->brush_mix = st->brush_mix;
    s->feather_px = st->feather_px;
    s->view_mode = (int)st->view;
    cfg_save(s);
    st->settings_dirty = false;
}

/* ------------------------------------------------------------------------ */
/* Shared state helpers                                                     */
/* ------------------------------------------------------------------------ */

job *job_selected(app_state *st) {
    if (st->selected < 0 || st->selected >= st->count) return NULL;
    return &st->jobs[st->selected];
}

bool job_running(const app_state *st) {
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].status == JOB_RUNNING) return true;
    return false;
}

const char *status_label(job_status s) {
    switch (s) {
    case JOB_QUEUED:    return "排队中";
    case JOB_RUNNING:   return "处理中";
    case JOB_DONE:      return "已完成";
    case JOB_ERROR:     return "出错";
    case JOB_CANCELLED: return "已取消";
    }
    return "";
}

cui_tone status_tone(job_status s) {
    switch (s) {
    case JOB_QUEUED:    return CUI_TONE_NEUTRAL;
    case JOB_RUNNING:   return CUI_TONE_INFO;
    case JOB_DONE:      return CUI_TONE_SUCCESS;
    case JOB_ERROR:     return CUI_TONE_DANGER;
    case JOB_CANCELLED: return CUI_TONE_WARNING;
    }
    return CUI_TONE_NEUTRAL;
}

void lasso_reset(app_state *st) {
    free(st->lasso);
    st->lasso = NULL;
    st->lasso_n = st->lasso_cap = 0;
    free(st->lasso_anchor);
    st->lasso_anchor = NULL;
    st->lasso_anchor_n = st->lasso_anchor_cap = 0;
    free(st->lasso_live);
    st->lasso_live = NULL;
    st->lasso_live_n = 0;
    st->lasso_active = false;
}

/* ------------------------------------------------------------------------ */
/* Denoiser events                                                          */
/* ------------------------------------------------------------------------ */

/* Drains the denoiser's event ring. Runs before any drawing so the frame is
 * consistent. */
static void poll_denoiser(cui_ctx *ui, app_state *st) {
    dn_event ev;
    while (dn_poll(&ev)) {
        switch (ev.type) {
        case DN_EV_READY:
            if (ev.ready_ok) {
                st->engine_ready = true;
                st->booting = false;
                st->engine_error[0] = 0;
                str_copy(st->device, sizeof st->device,
                         ev.device[0] ? ev.device : "引擎已就绪");
                /* the READY note is how a GPU->CPU fallback surfaces: it sits
                 * in the title-bar device pill and in the settings sheet */
                str_copy(st->device_note, sizeof st->device_note, ev.message);
                if (ev.message[0]) {
                    log_info("engine note: %s", ev.message);
                    toast(st, ev.message);
                }
                engine_submit_next(st);
            } else {
                st->engine_ready = false;
                st->booting = false;
                str_copy(st->engine_error, sizeof st->engine_error,
                         ev.message[0] ? ev.message : "推理引擎启动失败。");
                str_copy(st->device, sizeof st->device, "模型未就绪");
                log_error("engine boot failed: %s", st->engine_error);
            }
            break;

        case DN_EV_PROGRESS: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job *j = &st->jobs[i];
                float v = ev.value;
                if (!(v >= 0.0f)) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                j->progress = v;
                str_copy(j->phase, sizeof j->phase, ev.stage);
            }
            break;
        }

        case DN_EV_DONE: {
            int i = job_find(st, ev.id);
            if (i >= 0) job_apply_done(ui, st, &st->jobs[i], &ev);
            break;
        }

        case DN_EV_ERROR: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job *j = &st->jobs[i];
                j->status = JOB_ERROR;
                j->id = 0;
                str_copy(j->error, sizeof j->error, ev.message[0] ? ev.message : "推理失败。");
                log_error("job %d failed: %s", i, j->error);
                engine_submit_next(st);
            } else if (ev.message[0]) {
                str_copy(st->engine_error, sizeof st->engine_error, ev.message);
                st->booting = false;
                st->engine_ready = dn_state_get() == DN_STATE_READY;
            }
            break;
        }

        case DN_EV_CANCELLED: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job *j = &st->jobs[i];
                j->status = JOB_CANCELLED;
                j->id = 0;
                j->progress = 0.0f;
                engine_submit_next(st);
            }
            break;
        }

        case DN_EV_DL_PROGRESS:
            if (ev.id >= 0 && ev.id < 4) {
                st->dl_done[ev.id] = ev.dl_done;
                st->dl_total[ev.id] = ev.dl_total;
            }
            break;

        case DN_EV_DL_DONE:
            if (ev.id >= 0 && ev.id < 4) st->dl_busy[ev.id] = false;
            toast(st, "模型已就绪");
            break;

        case DN_EV_DL_ERROR:
            if (ev.id >= 0 && ev.id < 4) st->dl_busy[ev.id] = false;
            if (ev.message[0]) toast(st, ev.message);
            break;

        default:
            break;
        }
        bool heavy = ev.type != DN_EV_PROGRESS;
        dn_event_free(&ev);
        if (heavy) cui_request_redraw(ui);
    }
}

/* ------------------------------------------------------------------------ */
/* Fonts                                                                    */
/* ------------------------------------------------------------------------ */

static void load_fonts(cui_ctx *ui, app_state *st) {
    char dir[PATH_CAP], path[PATH_CAP];

    path_join(dir, sizeof dir, cfg_exe_dir(), "assets\\fonts");

    path_join(path, sizeof path, dir, "HarmonyOS-Sans-SC-Regular.ttf");
    cui_font regular = path_exists(path) ? cui_font_load_file(ui, path) : -1;
    path_join(path, sizeof path, dir, "HarmonyOS-Sans-SC-Medium.ttf");
    cui_font medium = path_exists(path) ? cui_font_load_file(ui, path) : -1;
    path_join(path, sizeof path, dir, "lucide.ttf");
    cui_font icons = path_exists(path) ? cui_font_load_file(ui, path) : -1;

    if (regular >= 0) {
        cui_font_set_role(ui, CUI_FONT_BODY, regular);
        /* rare CJK beyond the bundled subset falls back to the system face */
        cui_font sys = cui_font_load_file(ui, "C:\\Windows\\Fonts\\msyh.ttc");
        if (sys >= 0) cui_font_add_fallback(ui, regular, sys);
    }
    if (medium >= 0) {
        cui_font_set_role(ui, CUI_FONT_MEDIUM, medium);
        cui_font_set_role(ui, CUI_FONT_DISPLAY, medium);
    }
    if (icons >= 0) cui_font_set_role(ui, CUI_FONT_ICONS, icons);
    log_info("fonts: regular=%d medium=%d icons=%d", regular, medium, icons);
    (void)st;
}

/* ------------------------------------------------------------------------ */
/* Frame                                                                    */
/* ------------------------------------------------------------------------ */

void app_frame(cui_app *app, cui_ctx *ui, app_state *st) {
    static bool fonts_done;
    st->ui = ui;
    if (!fonts_done) { load_fonts(ui, st); fonts_done = true; }

    poll_denoiser(ui, st);

    /* an engine that never answers must not leave a spinner forever */
    if (st->booting && st->boot_deadline > 0.0 && now_seconds() > st->boot_deadline) {
        st->booting = false;
        st->boot_deadline = 0.0;
        st->engine_ready = false;
        str_copy(st->engine_error, sizeof st->engine_error,
                 "推理引擎启动超时。可以点“重试”，或检查日志。");
        str_copy(st->device, sizeof st->device, "启动超时");
        dn_shutdown();
        log_error("engine start timed out");
    }

    if (st->pending_n > 0) {
        int added = 0;
        for (int i = 0; i < st->pending_n; i++) {
            enqueue_path(st, st->pending[i]);
            added++;
        }
        free(st->pending);
        st->pending = NULL;
        st->pending_n = 0;
        if (added) engine_submit_next(st);
    }

    /* A modal surface owns input: the canvas, the floats and the shortcuts
     * must not see clicks and keys meant for it. CUI closes the topmost modal
     * itself (Esc or a click outside), so our own Esc peeling stays off too. */
    bool modal_open = st->show_settings || st->show_help ||
                      st->show_about   || st->show_update;
    app_handle_drops(ui, st);
    if (!modal_open) {
        app_handle_keys(ui, st);
    } else {
        st->peek_orig = st->peek_mask = st->space_down = false;
        st->drag_ctl = st->dragging_split = false;
        if (st->painting) {           /* a stroke interrupted by a modal ends whole */
            st->painting = st->stroke_open = false;
            job *pj = job_selected(st);
            if (pj) { mask_stroke_end(&pj->m, pj->hist); pj->composite_pending = true; }
        }
    }
    flush_composite(st);

    if (st->start_wanted) {
        if (st->start_shown) {
            st->start_wanted = false;
            st->start_shown = false;
            engine_start_now(st);
            engine_submit_next(st);
        } else {
            st->start_shown = true;
            cui_request_redraw(ui);
        }
    }

    settings_flush(st, false);
    pump_load_queue(st);

    upd_get(&st->upd);
    if (st->set.auto_check_updates && !st->bench) upd_check(false);

    bool busy = job_running(st) || st->booting;
    cui_request_wakeup(ui, busy ? 0.05f : 0.5f);

    /* the Fluent palette rides on the token roles; re-apply it whenever the
     * theme mode flips (system theme follows can change it any frame) */
    if (st->theme_applied != (int)cui_get_theme_mode(ui)) apply_fluent_theme(ui, st);

    /* Input order: chrome -> floating layers -> canvas. Each reads the hit
     * rects the previous frame's paint pass published; a click on a control
     * never reaches the layer beneath it. */
    job *j = job_selected(st);
    ui_titlebar_input(st);
    if (modal_open) {
        (void)chrome_take_wheel();
        /* gated input must still feed the edge trackers, or the press that
         * opened the modal leaves them armed and eats the next click */
        ui_overlay_track(ui);
        stage_track_buttons(ui);
    } else {
        bool over_ui = ui_overlay_input(st, j);
        if (j && st->stage_valid && !over_ui) stage_handle_input(st, j);
        else (void)chrome_take_wheel();   /* nowhere to zoom: drop the notches */
    }

    /* While a stroke is running the overlay is rebuilt at most every 40 ms:
     * feathering the whole mask per frame costs several ms and would eat the
     * frame budget. The final rebuild after the stroke is exact. */
    if (j && j->mask_tex_dirty) {
        double now = now_seconds();
        if (!st->painting || now - j->mask_tex_at > 0.04) {
            rebuild_mask_texture(st, j);
            j->mask_tex_at = now;
        }
    }

    /* Layer 1: the 40px title bar and one canvas that fills everything else.
     * Every other surface floats inside the canvas (ui_float.c). */
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        ui_titlebar(ui, st);
        draw_stage_area(ui, st, 0.0f);
    }

    ui_dialogs(ui, st);

    /* intents that block or rebuild state run after the frame has been built */
    if (st->want_clear) {
        st->want_clear = false;
        job_clear_finished(ui, st);
        engine_submit_next(st);
    }
    if (st->want_restart) {
        st->want_restart = false;
        st->engine_error[0] = 0;
        st->engine_ready = false;
        st->start_wanted = true;
        st->start_shown = false;
        cui_request_redraw(ui);
    }
    if (st->want_paste) {
        st->want_paste = false;
        app_do_paste(st);
    }
    if (st->want_save || st->want_save_as) {
        bool as_copy = st->want_save_as;
        st->want_save = st->want_save_as = false;
        app_do_save(st, as_copy);
    }
    if (st->want_browse) {
        st->want_browse = false;
        app_browse_dialog(app, st);
    }
}

/* ------------------------------------------------------------------------ */
/* Startup                                                                  */
/* ------------------------------------------------------------------------ */

void app_init(app_state *st) {
    memset(st, 0, sizeof *st);
    st->selected = -1;

    cfg_defaults(&st->set);
    cfg_load(&st->set);

    st->preset = st->set.preset >= 0 && st->set.preset <= 3 ? st->set.preset : DEFAULT_PRESET;
    st->down_pct = st->set.down_pct;
    st->l0 = st->set.l0;
    st->brush_size = st->set.brush_size > 0.0f ? st->set.brush_size : 14.0f;
    st->brush_mix = st->set.brush_mix > 0.0f ? st->set.brush_mix : 90.0f;
    st->feather_px = st->set.feather_px >= 0.0f ? st->set.feather_px : 10.0f;
    st->view = (view_mode)st->set.view_mode;
    st->auto_update = st->set.auto_check_updates != 0;
    st->tool = TOOL_VIEW;
    st->theme_applied = -1;   /* forces the Fluent palette on the first frame */
    str_copy(st->device, sizeof st->device, "正在启动...");

    loader_init();

    const char *env = getenv("ANIME_DENOISE_UPDATE_URL");
    str_copy(st->upd_manifest, sizeof st->upd_manifest, env ? env : "");
    upd_init(APP_VERSION, st->upd_manifest);

    char updated[32];
    if (upd_take_updated_flag(updated, sizeof updated)) {
        toast(st, "已更新到新版本");
        log_info("running after update to %s", updated);
    }

    st->bench = getenv("ANIME_DENOISE_BENCH") != NULL;
    st->start_wanted = !st->bench;
    if (st->bench) str_copy(st->device, sizeof st->device, "bench");
}

void app_shutdown(cui_ctx *ui, app_state *st) {
    cui_vec2 sz = cui_app_size(NULL);
    if (sz.x > 640.0f && sz.y > 480.0f) {
        st->set.window_w = (int)(sz.x + 0.5f);
        st->set.window_h = (int)(sz.y + 0.5f);
    }
    settings_flush(st, true);
    loader_shutdown();
    dn_shutdown();
    upd_shutdown();
    for (int i = 0; i < st->count; i++) job_free(ui, &st->jobs[i]);
    st->count = 0;
}


/* ------------------------------------------------------------------------ */
/* Public entry points                                                      */
/* ------------------------------------------------------------------------ */

bool app_quit_requested(const app_state *st) { return st->want_quit; }
int app_job_count(const app_state *st) { return st->count; }
const job *app_job_at(const app_state *st, int index) {
    if (index < 0 || index >= st->count) return NULL;
    return &st->jobs[index];
}
void app_test_select(app_state *st, int index) { job_focus(st, index); }

bool app_test_add_image(app_state *st, const char *path) {
    return app_job_add_now(st, path) != NULL;
}

void app_test_enqueue(app_state *st, const char *path) { enqueue_path(st, path); }
int app_test_load_pending(void) { return loader_pending(); }

void app_test_set_preset(app_state *st, int index) { apply_preset(st, index); }

job *app_test_selected(app_state *st) { return job_selected(st); }

void app_test_fake_result(app_state *st, int index) {
    if (index < 0 || index >= st->count) return;
    job *j = &st->jobs[index];
    if (!j->orig.rgba) return;
    /* A real DN_EV_DONE hands the job RGB pixels at source dimensions; the
     * composite path reads j->res directly. A fake that only flips
     * has_result lies, and the first mask stroke trips "合成保护区域失败"
     * -- so build res as a half-brightness copy of the source. Freed by
     * eng_image_free on job teardown like any real result. */
    if (!j->res.rgb) {
        size_t n = (size_t)j->orig.w * (size_t)j->orig.h;
        j->res.rgb = (uint8_t *)malloc(n * 3);
        if (j->res.rgb) {
            j->res.w = j->orig.w;
            j->res.h = j->orig.h;
            for (size_t i = 0; i < n; i++) {
                j->res.rgb[i * 3 + 0] = (uint8_t)(j->orig.rgba[i * 4 + 0] >> 1);
                j->res.rgb[i * 3 + 1] = (uint8_t)(j->orig.rgba[i * 4 + 1] >> 1);
                j->res.rgb[i * 3 + 2] = (uint8_t)(j->orig.rgba[i * 4 + 2] >> 1);
            }
        }
    }
    img_pixels prev = {0};
    if (!img_thumbnail(&j->orig, DISPLAY_MAX, &prev)) return;
    if (j->res_tex) cui_image_destroy(st->ui, j->res_tex);
    j->res_tex = cui_image_create(st->ui, prev.w, prev.h, prev.rgba);
    j->res_w = prev.w;
    j->res_h = prev.h;
    img_free(&prev);
    j->has_result = true;
    j->status = JOB_DONE;
    j->res_down_pct = j->down_pct;
    j->res_l0 = j->l0;
    j->ms = 1234;
    snprintf(j->note, sizeof j->note, "已完成 · 1.2 秒");
}
