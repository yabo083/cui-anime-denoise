/* Anime Denoise -- native front end for the GPT anime de-screening model.
 *
 * The UI is a CUI immediate-mode app: one frame function builds the whole
 * window from `app_state`, a Python sidecar (backend.h) does the inference.
 *
 * Threading / blocking rules:
 *   - worker events are drained at the top of every frame, before anything is
 *     drawn, so the frame always shows post-event state;
 *   - the two blocking calls in the app (starting the worker, the file dialog)
 *     are deferred to the end of a frame through intent flags, so the window
 *     has already painted a "working on it" frame when they block;
 *   - every texture the app owns is destroyed with the item that owns it.
 *
 * Layout:
 *   topbar (title, device badge, output folder, quit)
 *   +---- sidebar 288 + main ------------------------------------------+
 *   | presets          | banner (worker down)                          |
 *   | scale / l0       | dropzone   or   compare viewer               |
 *   | queue            | status line (progress, paths, open output)    |
 *   +------------------------------------------------------------------+
 *
 * TODO mask: the web app also has a protection brush / lasso so parts of the
 * line art keep their screentone. Not in v1; it would be a paintable mask
 * sent with the job and a `cui_canvas` overlay on top of the compare view.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "backend.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

#define APP_TITLE      "Anime Denoise"
#define MAX_JOBS       64
#define PATH_CAP       1024
#define IMAGE_FILTER   "png;jpg;jpeg;webp;bmp;gif;tif;tiff;avif"

/* Where the model environment lives. The worker imports `denoise` from here,
 * so this is both the venv root and the python path root. */
#define MODEL_ROOT     "E:/AI/gpt-anime-denoise"

#define SIDEBAR_W      288.0f
#define TOPBAR_H       54.0f
#define PANE_PAD       14.0f
#define PANE_GAP       12.0f

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum job_status {
    JOB_PENDING = 0, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED
} job_status;

typedef struct job {
    char path[PATH_CAP];   /* UTF-8 source path */
    char name[192];        /* basename, shown in the queue */
    char out[PATH_CAP];    /* UTF-8 output path, filled by the done event */
    char error[512];       /* error event text */

    int id;                /* worker job id, 0 when not submitted */
    job_status status;
    bool cancel_sent;
    float progress;        /* 0..1 */
    int ms;                /* worker-side elapsed time */

    double scale;          /* parameters captured when the job was queued */
    double l0;

    cui_texture src_tex;   /* original, PNG/JPEG only (cui_image_load) */
    int src_w, src_h;
    cui_texture res_tex;   /* preview RGBA from the worker, or the saved PNG */
    int res_w, res_h;      /* preview dimensions */
    int full_w, full_h;    /* full output dimensions */
} job;

typedef struct app_state {
    job jobs[MAX_JOBS];
    int count;
    int selected;              /* index into jobs, -1 = nothing selected */

    /* settings: one of the three presets, or custom (preset == 3) */
    int preset;
    float scale_pct;           /* 40..80 */
    float l0;                  /* 0..0.003 */

    /* viewer */
    float compare_pos;         /* before/after divider, 0..1 */
    cui_image_view view;       /* zoom/pan of the single-image viewer */
    int view_job;              /* job the viewer state belongs to */

    /* worker */
    bool worker_up;
    bool start_wanted;         /* a start is due */
    bool start_shown;          /* the "starting" frame has been painted */
    char device[128];          /* "RTX 5060 Ti (CUDA)" */
    char worker_error[512];    /* worker-level failure text */
    int next_id;

    /* paths */
    char exe_dir[PATH_CAP];
    char out_dir[PATH_CAP];
    char python[PATH_CAP];
    char worker_py[PATH_CAP];
    char venv[PATH_CAP];

    /* intents raised while drawing, handled after the frame is built */
    bool want_browse;
    bool want_quit;
    bool want_clear;
    bool want_restart;
} app_state;

typedef struct preset_def {
    const char *name;
    double scale;              /* 0..1 */
    double l0;
} preset_def;

static const preset_def PRESETS[3] = {
    { "Soft",     0.80, 0.0006 },
    { "Balanced", 0.60, 0.0010 },
    { "Strong",   0.45, 0.0018 },
};

#define PRESET_CUSTOM 3
#define DEFAULT_PRESET 1

/* ------------------------------------------------------------------------ */
/* Small utilities: strings, paths, Win32                                   */
/* ------------------------------------------------------------------------ */

static void str_copy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static const char *path_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

static void path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t n = dir ? strlen(dir) : 0;
    if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        snprintf(out, cap, "%s%s", dir, leaf);
    else
        snprintf(out, cap, "%s\\%s", dir, leaf);
}

static void utf8_to_wide(const char *s, wchar_t *out, int cap) {
    if (!s || !out || cap <= 0) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) <= 0) out[0] = 0;
}

static void wide_to_utf8(const wchar_t *s, char *out, int cap) {
    if (!s || !out || cap <= 0) return;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

static bool path_is_file(const char *path) {
    wchar_t w[PATH_CAP * 2];
    utf8_to_wide(path, w, PATH_CAP * 2);
    DWORD attr = GetFileAttributesW(w);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void dir_create(const char *path) {
    wchar_t w[PATH_CAP * 2];
    utf8_to_wide(path, w, PATH_CAP * 2);
    CreateDirectoryW(w, NULL);   /* already exists is fine */
}

/* worker/worker.py lives beside the sources; the exe may sit in build/, dist/,
 * tmp/uicheck/out or the repo root, so walk up a few parents looking for it. */
static bool find_worker_script(const char *start_dir, char *out, size_t cap) {
    char dir[PATH_CAP];
    str_copy(dir, sizeof dir, start_dir);
    for (int depth = 0; depth < 6; depth++) {
        char cand[PATH_CAP];
        path_join(cand, sizeof cand, dir, "worker\\worker.py");
        if (path_is_file(cand)) { str_copy(out, cap, cand); return true; }

        size_t n = strlen(dir);
        while (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = 0;
        while (n > 0 && dir[n - 1] != '\\' && dir[n - 1] != '/') dir[--n] = 0;   /* drop the last component */
        while (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = 0;
        if (n == 0) break;
    }
    return false;
}

static void find_exe_dir(char *out, size_t cap) {
    wchar_t w[PATH_CAP * 2];
    DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(PATH_CAP * 2));
    if (n == 0 || n >= PATH_CAP * 2) { str_copy(out, cap, "."); return; }
    w[n] = 0;
    for (DWORD i = n; i > 0; i--) {
        if (w[i - 1] == L'\\' || w[i - 1] == L'/') { w[i - 1] = 0; break; }
    }
    char u8[PATH_CAP * 4];
    wide_to_utf8(w, u8, (int)sizeof u8);
    str_copy(out, cap, u8[0] ? u8 : ".");
}

static double l0_snap(float v) {
    double r = floor((double)v * 10000.0 + 0.5) / 10000.0;
    return r < 0.0 ? 0.0 : r;
}

static void fmt_ms(int ms, char *buf, size_t cap) {
    if (ms <= 0) snprintf(buf, cap, "-");
    else if (ms < 1000) snprintf(buf, cap, "%d ms", ms);
    else snprintf(buf, cap, "%.1f s", (double)ms / 1000.0);
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                 */
/* ------------------------------------------------------------------------ */

static void preset_apply(app_state *st, int index) {
    if (index < 0 || index > 2) return;
    st->preset = index;
    st->scale_pct = (float)(PRESETS[index].scale * 100.0);
    st->l0 = (float)PRESETS[index].l0;
}

/* ------------------------------------------------------------------------ */
/* Jobs                                                                     */
/* ------------------------------------------------------------------------ */

static void job_free_textures(cui_ctx *ui, job *j) {
    if (j->src_tex) { cui_image_destroy(ui, j->src_tex); j->src_tex = 0; }
    if (j->res_tex) { cui_image_destroy(ui, j->res_tex); j->res_tex = 0; }
}

static int job_find(const app_state *st, int worker_id) {
    if (worker_id == 0) return -1;
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].id == worker_id) return i;
    return -1;
}

static job *job_selected(app_state *st) {
    if (st->selected < 0 || st->selected >= st->count) return NULL;
    return &st->jobs[st->selected];
}

static bool job_active(const app_state *st) {
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].status == JOB_RUNNING || st->jobs[i].status == JOB_PENDING) return true;
    return false;
}

static bool job_running(const app_state *st) {
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].status == JOB_RUNNING) return true;
    return false;
}

static const char *status_label(job_status s) {
    switch (s) {
    case JOB_PENDING:   return "Queued";
    case JOB_RUNNING:   return "Running";
    case JOB_DONE:      return "Done";
    case JOB_ERROR:     return "Error";
    case JOB_CANCELLED: return "Cancelled";
    }
    return "";
}

static cui_tone status_tone(job_status s) {
    switch (s) {
    case JOB_PENDING:   return CUI_TONE_NEUTRAL;
    case JOB_RUNNING:   return CUI_TONE_INFO;
    case JOB_DONE:      return CUI_TONE_SUCCESS;
    case JOB_ERROR:     return CUI_TONE_DANGER;
    case JOB_CANCELLED: return CUI_TONE_WARNING;
    }
    return CUI_TONE_NEUTRAL;
}

static void job_focus(app_state *st, int index) {
    st->selected = index;
    st->compare_pos = 0.5f;
    st->view = (cui_image_view){ 0 };
    st->view_job = index;
}

/* Queues one image with the settings as they are right now. */
static void job_enqueue(cui_ctx *ui, app_state *st, const char *path) {
    if (!path || !*path) return;
    if (st->count >= MAX_JOBS) return;

    job *j = &st->jobs[st->count];
    memset(j, 0, sizeof *j);
    str_copy(j->path, sizeof j->path, path);
    str_copy(j->name, sizeof j->name, path_basename(path));
    j->status = JOB_PENDING;
    j->scale = (double)st->scale_pct / 100.0;
    j->l0 = l0_snap(st->l0);

    int w = 0, h = 0;
    j->src_tex = cui_image_load(ui, j->path, &w, &h);
    j->src_w = j->src_tex ? w : 0;
    j->src_h = j->src_tex ? h : 0;

    st->count++;
    job_focus(st, st->count - 1);
}

static void job_clear_finished(cui_ctx *ui, app_state *st) {
    bool selection_removed = false;
    int write = 0;
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        bool finished = j->status == JOB_DONE || j->status == JOB_ERROR || j->status == JOB_CANCELLED;
        if (finished) {
            if (i == st->selected) selection_removed = true;
            job_free_textures(ui, j);
            continue;
        }
        if (write != i) {
            st->jobs[write] = st->jobs[i];
            if (st->selected == i) st->selected = write;
        }
        write++;
    }
    st->count = write;
    if (selection_removed) st->selected = st->count > 0 ? 0 : -1;
    if (st->selected >= st->count) st->selected = st->count - 1;
    st->view_job = -1;
    st->compare_pos = 0.5f;
    st->view = (cui_image_view){ 0 };
}

/* ------------------------------------------------------------------------ */
/* Worker                                                                   */
/* ------------------------------------------------------------------------ */

static void device_refresh(app_state *st) {
    const char *gpu = wb_gpu();
    const char *dev = wb_device();
    if (gpu && *gpu) {
        char upper[48];
        size_t k = 0;
        for (const char *p = dev; p && *p && k + 1 < sizeof upper; p++)
            upper[k++] = (char)toupper((unsigned char)*p);
        upper[k] = 0;
        if (upper[0]) snprintf(st->device, sizeof st->device, "%s (%s)", gpu, upper);
        else str_copy(st->device, sizeof st->device, gpu);
        return;
    }
    str_copy(st->device, sizeof st->device, st->worker_up ? "worker ready" : "worker offline");
}

static void worker_start_now(app_state *st) {
    st->worker_up = wb_start(st->python, st->worker_py, st->venv, NULL);
    if (st->worker_up) {
        device_refresh(st);
        st->worker_error[0] = 0;
    } else {
        const char *err = wb_last_error();
        str_copy(st->worker_error, sizeof st->worker_error,
                 (err && *err) ? err : "Could not start the Python worker.");
        str_copy(st->device, sizeof st->device, "worker offline");
    }
}

/* Sends the first queued job when nothing is running. */
static void worker_submit_next(app_state *st) {
    if (!st->worker_up || job_running(st)) return;
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        if (j->status != JOB_PENDING) continue;
        j->id = ++st->next_id;
        j->status = JOB_RUNNING;
        j->progress = 0.0f;
        j->cancel_sent = false;
        j->error[0] = 0;
        wb_push_denoise(j->id, j->path, j->scale, j->l0, st->out_dir);
        return;
    }
}

static void job_apply_done(cui_ctx *ui, job *j, const wb_event *ev) {
    if (j->res_tex) { cui_image_destroy(ui, j->res_tex); j->res_tex = 0; }

    unsigned char *rgba = NULL;
    int w = 0, h = 0;
    if (ev->preview_b64 && wb_decode_preview(ev->preview_b64, &rgba, &w, &h) && rgba) {
        j->res_tex = cui_image_create(ui, w, h, rgba);
        j->res_w = j->res_tex ? w : 0;
        j->res_h = j->res_tex ? h : 0;
        wb_free_pixels(rgba);
    }

    /* Formats cui_image_load cannot read (webp/avif/...): the worker sends the
     * source preview too, so the compare view has a real "before" side. */
    if (!j->src_tex && ev->src_preview_b64) {
        rgba = NULL;
        int sw = 0, sh = 0;
        if (wb_decode_preview(ev->src_preview_b64, &rgba, &sw, &sh) && rgba) {
            j->src_tex = cui_image_create(ui, sw, sh, rgba);
            j->src_w = j->src_tex ? sw : 0;
            j->src_h = j->src_tex ? sh : 0;
            wb_free_pixels(rgba);
        }
    }
    str_copy(j->out, sizeof j->out, ev->path);

    /* No preview (huge or odd PNG): fall back to the saved file, which is PNG. */
    if (!j->res_tex && j->out[0]) {
        int lw = 0, lh = 0;
        cui_texture t = cui_image_load(ui, j->out, &lw, &lh);
        if (t) { j->res_tex = t; j->res_w = lw; j->res_h = lh; }
    }

    j->full_w = ev->w;
    j->full_h = ev->h;
    j->ms = ev->ms;
    j->progress = 1.0f;
    j->status = JOB_DONE;
    j->id = 0;
}

static void worker_mark_dead(cui_ctx *ui, app_state *st) {
    st->worker_up = false;
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        if (j->status != JOB_RUNNING) continue;
        j->status = JOB_ERROR;
        str_copy(j->error, sizeof j->error, "The worker stopped while this image was running.");
        j->id = 0;
    }
    if (!st->worker_error[0]) {
        const char *err = wb_last_error();
        str_copy(st->worker_error, sizeof st->worker_error,
                 (err && *err) ? err : "The worker process exited unexpectedly.");
    }
    str_copy(st->device, sizeof st->device, "worker offline");
    cui_request_redraw(ui);
}

/* Drains the worker queue. Runs before any drawing so the frame is consistent. */
static void poll_worker(cui_ctx *ui, app_state *st) {
    wb_event ev;
    while (wb_poll(&ev)) {
        switch (ev.type) {
        case WB_EV_READY:
            st->worker_up = true;
            st->worker_error[0] = 0;
            device_refresh(st);
            worker_submit_next(st);
            break;

        case WB_EV_PROGRESS: {
            int i = job_find(st, ev.id);
            if (i >= 0 && st->jobs[i].status == JOB_RUNNING) {
                float v = ev.value;
                if (!(v >= 0.0f)) v = 0.0f;      /* also catches NaN */
                if (v > 1.0f) v = 1.0f;
                st->jobs[i].progress = v;
            }
            break;
        }

        case WB_EV_DONE: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job_apply_done(ui, &st->jobs[i], &ev);
                worker_submit_next(st);
            }
            break;
        }

        case WB_EV_ERROR: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job *j = &st->jobs[i];
                j->status = JOB_ERROR;
                j->id = 0;
                str_copy(j->error, sizeof j->error, ev.message[0] ? ev.message : "The worker reported an error.");
                worker_submit_next(st);
            } else if (ev.message[0]) {
                /* worker-level failure (bad model, bad parameters) */
                str_copy(st->worker_error, sizeof st->worker_error, ev.message);
                st->worker_up = wb_alive();
            }
            break;
        }

        case WB_EV_CANCELLED: {
            int i = job_find(st, ev.id);
            if (i >= 0) {
                job *j = &st->jobs[i];
                j->status = JOB_CANCELLED;
                j->id = 0;
                j->progress = 0.0f;
                worker_submit_next(st);
            }
            break;
        }

        case WB_EV_DEAD:
            /* A killed-and-restarted worker leaves its "dead" event behind;
             * only believe it when nothing newer is alive. */
            if (!wb_alive()) worker_mark_dead(ui, st);
            break;

        case WB_EV_BYE:
            st->worker_up = false;
            str_copy(st->device, sizeof st->device, "worker offline");
            break;

        default:
            break;
        }
        wb_event_free(&ev);   /* releases ev.preview_b64 if the worker sent one */
        /* Progress arrives ~25 times a second; the frame wakeup below already
         * repaints, so only state changes force an immediate redraw. */
        if (ev.type != WB_EV_PROGRESS) cui_request_redraw(ui);
    }
}

/* ------------------------------------------------------------------------ */
/* Paths                                                                    */
/* ------------------------------------------------------------------------ */

static void paths_init(app_state *st) {
    find_exe_dir(st->exe_dir, sizeof st->exe_dir);

    path_join(st->out_dir, sizeof st->out_dir, st->exe_dir, "outputs");
    dir_create(st->out_dir);

    str_copy(st->venv, sizeof st->venv, MODEL_ROOT);
    path_join(st->python, sizeof st->python, MODEL_ROOT, ".venv/Scripts/python.exe");

    /* The worker ships next to the sources (repo/worker/worker.py). */
    if (!find_worker_script(st->exe_dir, st->worker_py, sizeof st->worker_py))
        path_join(st->worker_py, sizeof st->worker_py, st->exe_dir, "worker\\worker.py");
}

/* ------------------------------------------------------------------------ */
/* UI: top bar                                                              */
/* ------------------------------------------------------------------------ */

static void ui_topbar(cui_ctx *ui, app_state *st) {
    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(TOPBAR_H), .pad_x = 16, .gap = 10,
            .align = CUI_ALIGN_CENTER, .bg = CUI_COLOR_SURFACE) {
        cui_text(ui, APP_TITLE, .font = CUI_FONT_DISPLAY, .size = 17, .nowrap = true);
        cui_badge(ui, st->device, .size = CUI_SIZE_SM, .dot = true,
                  .tone = st->worker_up ? CUI_TONE_SUCCESS : CUI_TONE_DANGER);
        cui_spacer(ui);
        if (cui_button(ui, "Open output folder", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                       .icon = CUI_ICON_FOLDER)) {
            cui_app_open_url(st->out_dir);
        }
        if (cui_button(ui, "Quit", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                       .icon = CUI_ICON_X, .shortcut = "Ctrl Q")) {
            st->want_quit = true;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* UI: sidebar                                                              */
/* ------------------------------------------------------------------------ */

static void ui_queue_row(cui_ctx *ui, app_state *st, int index) {
    job *j = &st->jobs[index];
    bool selected = st->selected == index;
    bool cancel = false;

    char card_id[48];
    snprintf(card_id, sizeof card_id, "job_row_%d", index);
    char params[64];
    snprintf(params, sizeof params, "scale %.0f%% - l0 %.4f", j->scale * 100.0, j->l0);

    cui_push_id_int(ui, index);

    cui_card_opts co;
    memset(&co, 0, sizeof co);
    co.id = card_id;
    co.dir = CUI_ROW;
    co.align = CUI_ALIGN_CENTER;
    co.gap = 8;
    co.pad = 8;
    co.width = CUI_FILL;
    co.hoverable = true;
    co.selected = selected;
    bool clicked = cui_card_begin(ui, &co);

    cui_column(ui, .width = CUI_FILL, .gap = 1) {
        cui_text(ui, j->name, .font = CUI_FONT_MEDIUM, .size = 12, .nowrap = true);
        cui_text(ui, params, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
    }
    cui_badge(ui, status_label(j->status), .tone = status_tone(j->status), .size = CUI_SIZE_SM,
              .dot = j->status == JOB_RUNNING);
    if (j->status == JOB_RUNNING && !j->cancel_sent) {
        if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                            .tooltip = "Cancel this image")) {
            cancel = true;
        }
    }
    cui_card_end(ui);
    cui_pop_id(ui);

    if (cancel && j->status == JOB_RUNNING && !j->cancel_sent) {
        j->cancel_sent = true;
        wb_cancel(j->id);
    } else if (clicked && !selected) {
        job_focus(st, index);
    }
}

static void ui_sidebar(cui_ctx *ui, app_state *st) {
    cui_column(ui, .width = CUI_PX(SIDEBAR_W), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad = 14, .gap = 10) {
        /* --- strength presets --- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_label(ui, "Strength");
            cui_spacer(ui);
            if (st->preset == PRESET_CUSTOM) cui_badge(ui, "Custom", .size = CUI_SIZE_SM, .tone = CUI_TONE_WARNING);
        }
        static const char *names[3] = { "Soft", "Balanced", "Strong" };
        int seg = st->preset <= 2 ? st->preset : 0;
        if (cui_segmented(ui, "preset", names, 3, &seg, .full_width = true, .size = CUI_SIZE_SM)) {
            preset_apply(st, seg);
        }
        {
            char hint[96];
            if (st->preset <= 2)
                snprintf(hint, sizeof hint, "%s: %.0f%% scale, l0 %.4f",
                         PRESETS[st->preset].name, PRESETS[st->preset].scale * 100.0, PRESETS[st->preset].l0);
            else
                snprintf(hint, sizeof hint, "Custom: %.0f%% scale, l0 %.4f",
                         (double)st->scale_pct, (double)st->l0);
            cui_text(ui, hint, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
        }

        /* --- fine controls (touching either one leaves the presets) --- */
        {
            float scale0 = st->scale_pct;
            float l0_0 = st->l0;
            cui_slider(ui, "Scale %", &st->scale_pct, 40.0f, 80.0f,
                       .step = 1.0f, .show_value = true, .format = "%.0f%%");
            cui_slider(ui, "L0", &st->l0, 0.0f, 0.003f,
                       .step = 0.0001f, .show_value = true, .format = "%.4f");
            /* The sliders re-snap their value on the first frame, so compare
             * against the value we had instead of trusting their return flag. */
            if (fabsf(st->scale_pct - scale0) > 0.5f || fabsf(st->l0 - l0_0) > 0.00005f)
                st->preset = PRESET_CUSTOM;
        }

        cui_separator(ui);

        /* --- queue --- */
        {
            char count[48];
            snprintf(count, sizeof count, "%d", st->count);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_label(ui, "Queue");
                cui_spacer(ui);
                cui_badge(ui, count, .size = CUI_SIZE_SM);
            }
        }
        if (st->count == 0) {
            cui_text(ui, "No images yet. Drop some here or on the canvas.",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_scroll(ui, .id = "queue", .height = CUI_FILL, .gap = 6, .fade_edges = true) {
            for (int i = 0; i < st->count; i++) ui_queue_row(ui, st, i);
        }

        if (cui_button(ui, "Add images", .variant = CUI_BUTTON_PRIMARY, .full_width = true,
                       .icon = CUI_ICON_PLUS, .shortcut = "Ctrl O")) {
            st->want_browse = true;
        }
        if (cui_button(ui, "Clear finished", .variant = CUI_BUTTON_GHOST, .full_width = true,
                       .icon = CUI_ICON_TRASH_2, .disabled = st->count == 0)) {
            st->want_clear = true;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* UI: main pane                                                            */
/* ------------------------------------------------------------------------ */

/* Height left for the canvas after the fixed rows of the main pane. */
static float canvas_height(cui_ctx *ui, bool banner) {
    float vh = cui_viewport_size(ui).y;
    float body = vh - TOPBAR_H - 1.0f;                     /* below the topbar separator */
    float used = 2.0f * PANE_PAD + 24.0f + 76.0f + 2.0f * PANE_GAP;
    if (banner) used += 72.0f;
    float h = body - used;
    if (h < 200.0f) h = 200.0f;
    if (h > 2400.0f) h = 2400.0f;
    return h;
}

typedef enum banner_kind {
    BANNER_NONE = 0,
    BANNER_STARTING,   /* the worker is being spawned / loading its model */
    BANNER_DOWN,       /* no worker process: nothing can run */
    BANNER_WARN        /* worker alive but it reported a problem */
} banner_kind;

static banner_kind banner_for(const app_state *st) {
    if (st->start_wanted) return BANNER_STARTING;
    if (!st->worker_up) return BANNER_DOWN;
    if (st->worker_error[0]) return BANNER_WARN;
    return BANNER_NONE;
}

static void ui_worker_banner(cui_ctx *ui, app_state *st, banner_kind kind) {
    if (kind == BANNER_STARTING) {
        cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER, .pad = 12,
                .radius = 12, .bg = CUI_COLOR_SURFACE_MUTED, .border = 1) {
            cui_spinner(ui, 18);
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "Starting the model worker", .font = CUI_FONT_MEDIUM, .size = 13);
                cui_text(ui, "The first launch loads torch and the ESRGAN weights - this can take a while.",
                         .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        return;
    }

    bool down = kind == BANNER_DOWN;
    cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER, .pad = 12,
            .radius = 12, .bg = CUI_COLOR_SURFACE_MUTED, .border = 1) {
        cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 20, CUI_COLOR_DANGER);
        cui_column(ui, .width = CUI_FILL, .gap = 2) {
            cui_text(ui, down ? "The Python worker is not running" : "The worker reported a problem",
                     .font = CUI_FONT_MEDIUM, .size = 13);
            cui_text(ui, st->worker_error[0] ? st->worker_error
                                             : "Images cannot be processed until it is restarted.",
                     .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        if (down) {
            if (cui_button(ui, "Restart worker", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_PLAY)) {
                st->want_restart = true;
            }
        } else if (cui_button(ui, "Dismiss", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) {
            st->worker_error[0] = 0;
        }
    }
}

static void ui_empty_view(cui_ctx *ui, app_state *st, float canvas) {
    float drop_h = canvas - 64.0f;
    if (drop_h < 180.0f) drop_h = 180.0f;
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER,
            .justify = CUI_JUSTIFY_CENTER) {
        cui_column(ui, .width = CUI_FILL, .max_width = 820, .gap = 10) {
            int r = cui_dropzone(ui, "main_drop",
                                 .title = "Drop images to denoise",
                                 .description = "or click to browse - PNG JPEG WebP BMP TIFF AVIF",
                                 .accept = IMAGE_FILTER,
                                 .icon = CUI_ICON_IMAGE,
                                 .height = drop_h);
            if (r < 0) st->want_browse = true;

            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Strength on the left applies to every image you add.",
                         .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_spacer(ui);
                cui_text(ui, st->out_dir, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
}

static void ui_job_header(cui_ctx *ui, job *j) {
    char meta[128];
    char when[32];
    fmt_ms(j->ms, when, sizeof when);
    if (j->status == JOB_DONE) {
        if (j->full_w > 0)
            snprintf(meta, sizeof meta, "%d x %d - %s", j->full_w, j->full_h, when);
        else
            snprintf(meta, sizeof meta, "%s", when);
    } else {
        snprintf(meta, sizeof meta, "scale %.0f%% - l0 %.4f", j->scale * 100.0, j->l0);
    }

    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, j->name, .font = CUI_FONT_MEDIUM, .size = 15, .nowrap = true);
        cui_badge(ui, status_label(j->status), .tone = status_tone(j->status), .size = CUI_SIZE_SM,
                  .dot = j->status == JOB_RUNNING);
        cui_text(ui, meta, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
        cui_spacer(ui);
        if (j->status == JOB_RUNNING && !j->cancel_sent) {
            if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_X)) {
                j->cancel_sent = true;
                wb_cancel(j->id);
            }
        }
    }
}

/* cui_image_compare always fills its box (it crops, never stretches), so make
 * the box match the picture's aspect: the whole frame then stays visible. */
static void fit_box(cui_ctx *ui, float canvas_h, float aspect, float *w, float *h) {
    float pane_w = cui_viewport_size(ui).x - SIDEBAR_W - 1.0f - 2.0f * PANE_PAD;
    if (pane_w < 160.0f) pane_w = 160.0f;
    if (aspect <= 0.01f) { *w = pane_w; *h = canvas_h; return; }
    float height = pane_w / aspect;
    if (height > canvas_h) { height = canvas_h; *w = height * aspect; }
    else *w = pane_w;
    *h = height;
}

static void ui_canvas(cui_ctx *ui, app_state *st, job *j, float canvas) {
    /* TODO mask: overlay a paintable mask canvas here for protected regions. */
    float aspect = 0.0f;
    if (j->res_w > 0 && j->res_h > 0) aspect = (float)j->res_w / (float)j->res_h;
    else if (j->src_w > 0 && j->src_h > 0) aspect = (float)j->src_w / (float)j->src_h;

    if (j->src_tex && j->res_tex) {
        float bw = 0.0f, bh = 0.0f;
        fit_box(ui, canvas, aspect, &bw, &bh);
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER) {
            cui_image_compare(ui, "compare", j->src_tex, j->res_tex, &st->compare_pos,
                              .width = CUI_PX(bw), .height = bh,
                              .before_label = "Original", .after_label = "Denoised",
                              .aspect = aspect);
        }
        return;
    }

    cui_texture tex = j->res_tex ? j->res_tex : j->src_tex;
    if (tex) {
        if (st->view_job != st->selected) {
            st->view = (cui_image_view){ 0 };
            st->view_job = st->selected;
        }
        cui_image_viewer(ui, "viewer", tex,
                         j->res_tex ? j->res_w : j->src_w,
                         j->res_tex ? j->res_h : j->src_h,
                         &st->view, .width = CUI_FILL, .height = CUI_PX(canvas),
                         .checkerboard = true);
        if (!j->src_tex) {
            cui_text(ui, "Original preview is not available for this format; the worker still denoised it.",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        return;
    }

    /* Nothing to show yet: running, queued, failed, or a format CUI cannot decode. */
    cui_box(ui, .width = CUI_FILL, .height = CUI_PX(canvas), .radius = 14, .border = 1,
            .bg = CUI_COLOR_SURFACE_MUTED, .dir = CUI_STACK, .align = CUI_ALIGN_CENTER,
            .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
        const char *msg = "Waiting for a preview";
        if (j->status == JOB_PENDING) msg = "Queued - it starts as soon as the current image finishes";
        else if (j->status == JOB_RUNNING) msg = "Denoising...";
        else if (j->status == JOB_CANCELLED) msg = "Cancelled before it finished";
        else if (j->status == JOB_ERROR) msg = "This image failed";
        else if (j->status == JOB_DONE) msg = "Done - the output has no preview this decoder can read";
        cui_icon(ui, CUI_ICON_IMAGE, 28, CUI_COLOR_TEXT_MUTED);
        cui_text(ui, msg, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .align = CUI_TEXT_CENTER);
    }
}

static void ui_job_status(cui_ctx *ui, app_state *st, job *j) {
    cui_column(ui, .width = CUI_FILL, .gap = 6) {
        cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
            if (j->status == JOB_RUNNING) {
                char pct[16];
                snprintf(pct, sizeof pct, "%d%%", (int)(j->progress * 100.0f + 0.5f));
                cui_progress(ui, j->progress, .width = CUI_FILL, .height = 6,
                             .tone = CUI_TONE_ACCENT);
                cui_text(ui, j->cancel_sent ? "cancelling..." : pct, .size = 12, .nowrap = true,
                         .tabular = true, .color = CUI_COLOR_TEXT_SECONDARY);
            } else if (j->status == JOB_ERROR) {
                cui_text(ui, j->error[0] ? j->error : "The worker reported an error.",
                         .size = 12, .color = CUI_COLOR_DANGER, .nowrap = true);
                cui_spacer(ui);
            } else if (j->status == JOB_DONE) {
                char when[32];
                fmt_ms(j->ms, when, sizeof when);
                cui_textf(ui, "Done in %s", when);
                cui_spacer(ui);
                if (cui_button(ui, "Open output", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_ARROW_UP_RIGHT)) {
                    cui_app_open_url(j->out);
                }
                if (cui_button(ui, "Show folder", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_FOLDER)) {
                    cui_app_open_url(st->out_dir);
                }
            } else {
                cui_text(ui, j->status == JOB_CANCELLED ? "Cancelled" : "Waiting in the queue",
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                cui_spacer(ui);
            }
        }
        if (j->status == JOB_DONE && j->out[0]) {
            cui_text(ui, j->out, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            cui_text(ui, j->path, .size = 11, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED);
        }
    }
}

static void ui_job_view(cui_ctx *ui, app_state *st, float canvas) {
    job *j = job_selected(st);
    if (!j) { ui_empty_view(ui, st, canvas); return; }
    ui_job_header(ui, j);
    ui_canvas(ui, st, j, canvas);
    ui_job_status(ui, st, j);
}

static void ui_main(cui_ctx *ui, app_state *st) {
    banner_kind kind = banner_for(st);
    float canvas = canvas_height(ui, kind != BANNER_NONE);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = PANE_PAD, .gap = PANE_GAP) {
        if (kind != BANNER_NONE) ui_worker_banner(ui, st, kind);
        ui_job_view(ui, st, canvas);
    }
}

/* ------------------------------------------------------------------------ */
/* Frame                                                                    */
/* ------------------------------------------------------------------------ */

static void handle_drops(cui_ctx *ui, app_state *st) {
    /* Drops anywhere on the window are accepted, not only on the dropzone. */
    int n = cui_dropped_file_count(ui);
    for (int i = 0; i < n; i++) {
        const char *path = cui_dropped_file(ui, i);
        if (path && *path && cui_path_has_extension(path, IMAGE_FILTER)) job_enqueue(ui, st, path);
    }
}

static void open_browse_dialog(cui_app *app, cui_ctx *ui, app_state *st) {
    char path[PATH_CAP];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_OPEN;
    opts.title = "Choose images to denoise";
    opts.filter_name = "Images";
    opts.filter = IMAGE_FILTER;
    if (cui_app_file_dialog(app, &opts, path, sizeof path)) job_enqueue(ui, st, path);
}

static void app_frame(cui_app *app, cui_ctx *ui, app_state *st) {
    poll_worker(ui, st);
    handle_drops(ui, st);

    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) st->want_browse = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Q)) st->want_quit = true;

    /* Starting the worker blocks (up to a minute on a cold model cache), so give
     * the window one frame that shows the banner before it happens. */
    if (st->start_wanted) {
        if (st->start_shown) {
            st->start_wanted = false;
            st->start_shown = false;
            worker_start_now(st);
            worker_submit_next(st);
        } else {
            st->start_shown = true;
            cui_request_redraw(ui);
        }
    }

    if (job_active(st)) cui_request_wakeup(ui, 0.03f);      /* progress keeps moving */
    else if (st->worker_up) cui_request_wakeup(ui, 1.0f);   /* notice a worker that died */

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        ui_topbar(ui, st);
        cui_separator(ui);
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH) {
            ui_sidebar(ui, st);
            cui_separator(ui);
            ui_main(ui, st);
        }
    }

    /* Intents that block or rebuild state run after the frame has been built. */
    if (st->want_clear) {
        st->want_clear = false;
        job_clear_finished(ui, st);
        worker_submit_next(st);
    }
    if (st->want_restart) {
        st->want_restart = false;
        if (wb_alive()) wb_kill();
        st->worker_error[0] = 0;
        st->start_wanted = true;
        st->start_shown = false;
        cui_request_redraw(ui);
    }
    if (st->want_browse) {
        st->want_browse = false;
        open_browse_dialog(app, ui, st);
        worker_submit_next(st);
    }
}

static void app_init(app_state *st) {
    memset(st, 0, sizeof *st);
    st->selected = -1;
    st->view_job = -1;
    st->compare_pos = 0.5f;
    preset_apply(st, DEFAULT_PRESET);
    str_copy(st->device, sizeof st->device, "starting...");
    paths_init(st);
    st->start_wanted = true;
}

static void app_shutdown(cui_ctx *ui, app_state *st) {
    wb_shutdown();
    for (int i = 0; i < st->count; i++) job_free_textures(ui, &st->jobs[i]);
    st->count = 0;
}

int main(void) {
    static app_state st;
    app_init(&st);

    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = APP_TITLE,
        .width = 1200,
        .height = 820,
        .min_width = 900,
        .min_height = 620,
        .accent = CUI_ACCENT_VIOLET,
        .follow_system_theme = true,
    });
    if (!app) return 1;

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        app_frame(app, ui, &st);
        if (st.want_quit) break;
    }

    app_shutdown(cui_app_ui(app), &st);
    cui_app_destroy(app);
    return 0;
}
