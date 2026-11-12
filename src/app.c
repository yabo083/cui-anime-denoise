/* Anime Denoise -- native front end for the GPT anime de-screening model.
 *
 * CUI immediate-mode UI in C, inference on the in-process engine thread
 * (denoiser.h): the ONNX Runtime + DirectML engine runs behind one
 * below-normal-priority thread and answers through an event ring.
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
 * Layout
 *   topbar 56
 *   +---------- sidebar 300 ---------+- stage ---------------------------+
 *   | strength presets               | head: name, status, actions      |
 *   | scale / l0                     | canvas: compare / brush / lasso  |
 *   | brush (size, mix, feather)     | strip: thumbnails                |
 *   | queue                          | status: progress, paths          |
 *   +--------------------------------+----------------------------------+
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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
#include "icons.h"
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

#define PRESET_CUSTOM 3
#define DEFAULT_PRESET 1

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

/* Center-crops to a square so a 128x38 thumbnail fills a 62x62 tile instead of
 * letterboxing into a thin band. */
static void thumb_crop_square(img_pixels *p) {
    if (!p || !p->rgba) return;
    int side = p->w < p->h ? p->w : p->h;
    if (side <= 0) return;
    int ox = (p->w - side) / 2, oy = (p->h - side) / 2;
    uint8_t *out = (uint8_t *)malloc((size_t)side * side * 4);
    if (!out) return;
    for (int y = 0; y < side; y++)
        memcpy(out + (size_t)y * side * 4,
               p->rgba + ((size_t)(y + oy) * p->w + ox) * 4,
               (size_t)side * 4);
    free(p->rgba);
    p->rgba = out;
    p->w = side;
    p->h = side;
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
    str_copy(st->toast, sizeof st->toast, msg);
    st->toast_until = now_seconds() + 3.5;
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
/* Jobs                                                                     */
/* ------------------------------------------------------------------------ */

job *job_selected(app_state *st) {
    if (st->selected < 0 || st->selected >= st->count) return NULL;
    return &st->jobs[st->selected];
}

static int job_find(const app_state *st, int dn_id) {
    if (dn_id == 0) return -1;
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].id == dn_id) return i;
    return -1;
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

static void job_drop_textures(cui_ctx *ui, job *j) {
    if (j->orig_tex) { cui_image_destroy(ui, j->orig_tex); j->orig_tex = 0; }
    if (j->res_tex) { cui_image_destroy(ui, j->res_tex); j->res_tex = 0; }
    if (j->mask_tex) { cui_image_destroy(ui, j->mask_tex); j->mask_tex = 0; }
}

static void job_free(cui_ctx *ui, job *j) {
    job_drop_textures(ui, j);
    if (j->thumb_tex) { cui_image_destroy(ui, j->thumb_tex); j->thumb_tex = 0; }
    img_free(&j->orig);
    eng_image_free(&j->res);
    img_free_icc(j->icc);
    mask_free(&j->m);
    mask_edges_free(&j->edges);
    if (j->hist) { mask_hist_free(j->hist); j->hist = NULL; }
    /* the rest of the struct is rebuilt by the next job that lands here */
    memset(j, 0, sizeof *j);
}

/* Stable identity for the denoiser's SR cache: the worker keyed it on the
 * file's sha1, so an edited source must not reuse the cached upscale. Path +
 * file size + write time gives the same "same bytes" test without reading
 * the file (the loader already decoded it, so the stamp is free). */
static int job_src_key(const char *path) {
    wchar_t w[PATH_CAP * 2];
    WIN32_FILE_ATTRIBUTE_DATA a;
    unsigned h = 2166136261u;
    utf8_to_wide(path, w, PATH_CAP * 2);
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
        a.nFileSizeLow = a.nFileSizeHigh = 0;
        a.ftLastWriteTime.dwLowDateTime = a.ftLastWriteTime.dwHighDateTime = 0;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    for (int i = 0; i < 4; i++) {
        unsigned v = i == 0 ? a.nFileSizeLow
                   : i == 1 ? a.nFileSizeHigh
                   : i == 2 ? a.ftLastWriteTime.dwLowDateTime
                            : a.ftLastWriteTime.dwHighDateTime;
        for (int b = 0; b < 4; b++) {
            h ^= (unsigned char)(v & 0xFF);
            h *= 16777619u;
            v >>= 8;
        }
    }
    return (int)(h & 0x7FFFFFFFu);
}

static void out_paths(app_state *st, job *j) {
    const char *dir = st->set.output_dir[0] ? st->set.output_dir : cfg_default_out_dir();
    char base[PATH_CAP];
    if (preset_of(j->down_pct, j->l0) == DEFAULT_PRESET)
        snprintf(base, sizeof base, "%s_denoise.png", j->stem);
    else {
        char l0[16];
        fmt_l0(j->l0, l0, sizeof l0);
        snprintf(base, sizeof base, "%s_denoise_s%d_l0-%s.png", j->stem, j->down_pct, l0);
    }
    path_join(j->out, sizeof j->out, dir, base);

    size_t n = strlen(j->out);
    if (n > 4 && strcmp(j->out + n - 4, ".png") == 0)
        snprintf(j->out_protected, sizeof j->out_protected, "%.*s_protected.png", (int)(n - 4), j->out);
    else
        snprintf(j->out_protected, sizeof j->out_protected, "%s_protected.png", j->out);
}

void lasso_reset(app_state *st) {
    free(st->lasso);
    st->lasso = NULL;
    st->lasso_n = st->lasso_cap = 0;
    free(st->lasso_live);
    st->lasso_live = NULL;
    st->lasso_live_n = 0;
    st->lasso_active = false;
}

void job_focus(app_state *st, int index) {
    if (index < 0 || index >= st->count) return;
    st->selected = index;
    job *j = &st->jobs[index];
    if (j->view.zoom <= 0.0f) j->view = (view2d){ 0.0f, 0.0f, 0.0f };   /* fit on first show */
    if (j->split <= 0.001f) j->split = 0.5f;
    st->tool = TOOL_VIEW;
    st->lasso_active = false;
    free(st->lasso);
    st->lasso = NULL;
    st->lasso_n = st->lasso_cap = 0;
    free(st->lasso_live);
    st->lasso_live = NULL;
    st->lasso_live_n = 0;
    st->painting = false;
    st->stroke_open = false;
}

/* Loads an image into a fresh job slot: full-res pixels, display texture,
 * thumbnail and mask. Returns NULL when the format cannot be decoded. */
/* Creates the job from pixels the loader thread already produced: the UI thread
 * only uploads textures here, so it never pays for a decode. */
static job *job_add_decoded(app_state *st, loader_result *r) {
    if (st->count >= MAX_JOBS) {
        img_free(&r->full);
        img_free(&r->display);
        img_free(&r->thumb);
        return NULL;
    }
    job *j = &st->jobs[st->count];
    memset(j, 0, sizeof *j);
    str_copy(j->path, sizeof j->path, r->path);
    str_copy(j->name, sizeof j->name, path_basename(r->path));
    path_stem(j->stem, sizeof j->stem, j->name);

    j->orig = r->full;
    /* ownership moves to the job: the caller still runs img_free() on every
     * result field, so clear this one or the pixels are freed twice */
    r->full = (img_pixels){ 0 };
    j->icc = r->icc;
    j->icc_size = r->icc_size;
    r->icc = NULL;
    r->icc_size = 0;
    j->src_key = job_src_key(r->path);
    j->has_alpha = r->has_alpha;
    j->status = JOB_QUEUED;
    j->down_pct = st->down_pct;
    j->l0 = st->l0;
    j->feather_px = (int)(st->feather_px + 0.5f);
    j->split = 0.5f;
    j->hist = mask_hist_new(0);
    mask_init(&j->m, r->full.w, r->full.h);
    out_paths(st, j);

    const img_pixels *disp = r->display.rgba ? &r->display : &j->orig;
    j->orig_tex = cui_image_create(st->ui, disp->w, disp->h, disp->rgba);
    j->disp_w = disp->w;
    j->disp_h = disp->h;
    if (r->thumb.rgba) {
        j->thumb_tex = cui_image_create(st->ui, r->thumb.w, r->thumb.h, r->thumb.rgba);
        j->thumb_w = r->thumb.w;
        j->thumb_h = r->thumb.h;
    }

    st->count++;
    job_focus(st, st->count - 1);
    log_info("queued %s (%dx%d)", j->name, r->src_w, r->src_h);
    return j;
}

static job *job_add(cui_ctx *ui, app_state *st, const char *path) {
    if (st->count >= MAX_JOBS) { toast(st, "最多同时处理 64 张图"); return NULL; }

    img_pixels px = {0};
    unsigned char *icc = NULL;
    size_t icc_size = 0;
    if (!img_decode_file_ex(path, &px, &icc, &icc_size)) {
        char msg[420];
        snprintf(msg, sizeof msg, "无法读取这张图片：%s", path_basename(path));
        toast(st, msg);
        log_warn("decode failed: %s", path);
        return NULL;
    }

    job *j = &st->jobs[st->count];
    memset(j, 0, sizeof *j);
    str_copy(j->path, sizeof j->path, path);
    str_copy(j->name, sizeof j->name, path_basename(path));
    path_stem(j->stem, sizeof j->stem, j->name);

    j->orig = px;
    j->icc = icc;
    j->icc_size = icc_size;
    j->src_key = job_src_key(path);
    j->has_alpha = img_has_alpha(&px);
    j->status = JOB_QUEUED;
    j->down_pct = st->down_pct;
    j->l0 = st->l0;
    j->feather_px = (int)(st->feather_px + 0.5f);
    j->split = 0.5f;
    j->hist = mask_hist_new(0);
    mask_init(&j->m, px.w, px.h);
    out_paths(st, j);

    /* display texture (downscaled for very large images) */
    img_pixels disp = {0};
    int longest = px.w > px.h ? px.w : px.h;
    bool scaled = false;
    if (longest > DISPLAY_MAX && img_thumbnail(&px, DISPLAY_MAX, &disp)) scaled = true;
    const img_pixels *src = scaled ? &disp : &px;
    j->orig_tex = cui_image_create(ui, src->w, src->h, src->rgba);
    j->disp_w = src->w;
    j->disp_h = src->h;
    if (scaled) img_free(&disp);

    img_pixels th = {0};
    if (img_thumbnail(&px, 128, &th)) {
        thumb_crop_square(&th);
        j->thumb_tex = cui_image_create(ui, th.w, th.h, th.rgba);
        j->thumb_w = th.w;
        j->thumb_h = th.h;
        img_free(&th);
    }

    st->count++;
    job_focus(st, st->count - 1);
    log_info("queued %s (%dx%d)", j->name, px.w, px.h);
    return j;
}

/* Queues an image for decoding; the frame loop decodes at most one per frame. */
void enqueue_path(app_state *st, const char *path) {
    if (!path || !*path) return;
    if (!loader_push(path)) toast(st, "一次最多添加 64 张图");
}

void pump_load_queue(app_state *st) {
    if (!st->ui) return;
    loader_result r;
    /* one finished decode per frame keeps texture uploads bounded */
    if (loader_pop(&r)) {
        if (r.ok) {
            if (job_add_decoded(st, &r)) engine_submit_next(st);
        } else {
            char msg[420];
            snprintf(msg, sizeof msg, "无法读取 %s：%s", path_basename(r.path), r.error);
            toast(st, msg);
            log_warn("decode failed: %s (%s)", r.path, r.error);
        }
        img_free(&r.full);
        img_free(&r.display);
        img_free(&r.thumb);
        img_free_icc(r.icc);
    }
    if (loader_pending() > 0) cui_request_redraw(st->ui);
}

void job_clear_finished(cui_ctx *ui, app_state *st) {
    int write = 0;
    int selected = -1;
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        bool finished = j->status == JOB_DONE || j->status == JOB_ERROR || j->status == JOB_CANCELLED;
        if (finished) {
            job_free(ui, j);
            continue;
        }
        if (write != i) {
            st->jobs[write] = st->jobs[i];
            memset(&st->jobs[i], 0, sizeof st->jobs[i]);
        }
        if (st->selected == i) selected = write;
        write++;
    }
    st->count = write;
    if (selected >= 0) st->selected = selected;
    else if (st->selected >= st->count) st->selected = st->count - 1;
    if (st->count == 0) st->selected = -1;
}

/* ------------------------------------------------------------------------ */
/* Denoiser                                                                 */
/* ------------------------------------------------------------------------ */

static eng_device device_pref(const app_state *st) {
    return st->set.device == 1 ? ENG_DEV_GPU
         : st->set.device == 2 ? ENG_DEV_CPU
                               : ENG_DEV_AUTO;
}

static void engine_start_now(app_state *st) {
    if (st->bench) return;

    /* asynchronous: the engine thread is spawned here and readiness arrives
     * as DN_EV_READY, so nothing on the UI thread waits for ORT (§2). A
     * restart joins the old thread first -- bounded by one engine tile. */
    st->booting = true;
    st->boot_value = 0.0f;
    str_copy(st->boot_stage, sizeof st->boot_stage, "加载推理引擎");
    st->boot_deadline = now_seconds() + 60.0;
    st->engine_error[0] = 0;
    if (dn_state_get() == DN_STATE_IDLE) {
        if (!dn_start(device_pref(st))) {
            st->booting = false;
            const char *err = dn_last_error();
            str_copy(st->engine_error, sizeof st->engine_error,
                     (err && *err) ? err : "无法启动推理引擎。");
            str_copy(st->device, sizeof st->device, "模型未就绪");
            log_error("denoiser start failed: %s", st->engine_error);
            return;
        }
    } else {
        dn_restart(device_pref(st));
    }
    str_copy(st->device, sizeof st->device, "正在启动…");
}

/* Sends the first queued job when nothing is running. The job's pixels are
 * already in memory (the loader decoded them), so the denoiser gets RGBA,
 * not a path -- nothing re-reads the source file. */
void engine_submit_next(app_state *st) {
    if (st->bench) return;
    if (!st->engine_ready || job_running(st)) return;
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        if (j->status != JOB_QUEUED) continue;
        j->id = ++st->next_id;
        j->status = JOB_RUNNING;
        j->progress = 0.0f;
        j->phase[0] = 0;
        j->cancel_sent = false;
        j->error[0] = 0;
        out_paths(st, j);
        if (!dn_push_denoise(j->id, j->src_key, &j->orig, j->down_pct, j->l0)) {
            j->status = JOB_ERROR;
            j->id = 0;
            str_copy(j->error, sizeof j->error, "无法提交去噪任务。");
        }
        return;
    }
}

/* The denoise result as RGBA: engine RGB plus the source alpha channel so
 * a transparent PNG stays transparent. Returns NULL on failure. */
static img_pixels *job_result_px(job *j, img_pixels *out) {
    memset(out, 0, sizeof *out);
    if (!j->has_result || !j->res.rgb) return NULL;
    out->w = j->res.w;
    out->h = j->res.h;
    out->rgba = (uint8_t *)malloc((size_t)out->w * out->h * 4);
    if (!out->rgba) { memset(out, 0, sizeof *out); return NULL; }
    bool alpha = j->orig.rgba && j->orig.w == j->res.w && j->orig.h == j->res.h;
    for (int i = 0, n = out->w * out->h; i < n; i++) {
        memcpy(out->rgba + i * 4, j->res.rgb + i * 3, 3);
        out->rgba[i * 4 + 3] = alpha ? j->orig.rgba[i * 4 + 3] : 255;
    }
    return out;
}

/* Composites the feathered mask into the cached result, on the host.
 * The worker did this on its side because it owned the pixels; now the job
 * owns them (j->res), so the blend runs here and only the preview goes to
 * the GPU. Returns the composited full-res RGBA (NULL on failure). */
static img_pixels *job_composite_px(job *j, img_pixels *out) {
    if (!j->mask_active) return job_result_px(j, out);
    memset(out, 0, sizeof *out);
    if (!j->has_result || !j->res.rgb) return NULL;
    uint8_t *w = NULL;
    if (!mask_feather(&j->m, j->feather_px, &w)) return NULL;
    bool ok = j->orig.w == j->res.w && j->orig.h == j->res.h &&
              img_composite_masked(out, &j->orig, j->res.rgb, w, j->res.w, j->res.h);
    free(w);
    return ok ? out : NULL;
}

static void job_res_texture(cui_ctx *ui, job *j, const img_pixels *px) {
    img_pixels prev = {0};
    if (!img_thumbnail(px, DISPLAY_MAX, &prev)) return;
    if (j->res_tex) cui_image_destroy(ui, j->res_tex);
    j->res_tex = cui_image_create(ui, prev.w, prev.h, prev.rgba);
    j->res_w = j->res_tex ? prev.w : 0;
    j->res_h = j->res_tex ? prev.h : 0;
    img_free(&prev);
}

/* The mask changed on a finished job: blend it locally, rewrite the
 * protected PNG, refresh the preview. Synchronous because it is CPU work
 * on memory the thread already owns -- a stroke commit costs a feather +
 * a blend, no IPC. */
void request_composite(app_state *st, job *j) {
    if (!j->has_result || j->id != 0 || !st->ui) return;

    img_pixels px = {0};
    if (!job_composite_px(j, &px)) {
        toast(st, "合成保护区域失败");
        j->composite_dirty = false;
        return;
    }
    const char *out = j->mask_active && j->out_protected[0]
                      ? j->out_protected : j->out;
    if (out[0]) {
        if (!img_encode_png_icc(out, &px, j->icc, j->icc_size))
            toast(st, "写入受保护结果失败");
    }
    job_res_texture(st->ui, j, &px);
    img_free(&px);
    j->composite_dirty = false;
    j->compositing = false;
    log_info("composite job %d: mask %dx%d", j->id, j->m.w, j->m.h);
}

void rebuild_mask_texture(app_state *st, job *j) {
    if (!j->m.px || !j->mask_active) {
        if (j->mask_tex) { cui_image_destroy(st->ui, j->mask_tex); j->mask_tex = 0; }
        j->mask_tex_dirty = false;
        return;
    }
    uint8_t *w = NULL;
    if (!mask_feather(&j->m, j->feather_px, &w)) return;

    int sw = j->m.w, sh = j->m.h;
    int longest = sw > sh ? sw : sh;
    int step = longest > MASK_TEX_MAX ? (longest + MASK_TEX_MAX - 1) / MASK_TEX_MAX : 1;
    int tw = sw / step, th = sh / step;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    uint8_t *rgba = (uint8_t *)malloc((size_t)tw * th * 4);
    if (!rgba) { free(w); return; }
    for (int y = 0; y < th; y++) {
        const uint8_t *row = w + (size_t)y * step * sw;
        for (int x = 0; x < tw; x++) {
            uint8_t v = row[(size_t)x * step];
            uint8_t *p = rgba + ((size_t)y * tw + x) * 4;
            p[0] = 224; p[1] = 122; p[2] = 95;          /* the web UI's mask colour */
            p[3] = (uint8_t)((int)v * 130 / 255);       /* 0.51 max, sqrt-ish on screen */
        }
    }
    free(w);
    if (j->mask_tex) cui_image_destroy(st->ui, j->mask_tex);
    j->mask_tex = cui_image_create(st->ui, tw, th, rgba);
    j->mask_tex_w = tw;
    j->mask_tex_h = th;
    j->mask_tex_dirty = false;
    free(rgba);
}

/* A DN_EV_DONE landed: take the pixels, write the PNG, build the preview.
 * All host-side now -- the worker's "path" field is gone, so j->out (set at
 * submit time by out_paths) is where the result is encoded, with the source
 * alpha re-applied and the source ICC profile written into the PNG. */
static void job_apply_done(cui_ctx *ui, app_state *st, job *j, dn_event *ev) {
    j->res = ev->rgb;                  /* ownership moves to the job */
    memset(&ev->rgb, 0, sizeof ev->rgb);
    j->has_result = true;
    j->res_down_pct = j->down_pct;
    j->res_l0 = j->l0;
    j->ms = ev->ms;
    j->sr_cached = ev->sr_cached;

    img_pixels px = {0};
    if (job_result_px(j, &px)) {
        if (j->out[0] && !img_encode_png_icc(j->out, &px, j->icc, j->icc_size)) {
            toast(st, "无法写入输出 PNG");
            log_error("encode failed: %s", j->out);
        }
        job_res_texture(ui, j, &px);
        img_free(&px);
    }
    j->status = JOB_DONE;
    j->id = 0;
    j->progress = 1.0f;
    if (j->mask_active) {
        j->composite_dirty = true;
        j->composite_pending = true;
    }
    char when[32];
    fmt_ms(ev->ms, when, sizeof when);
    snprintf(j->note, sizeof j->note, "已完成 · %s%s", when, ev->sr_cached ? " · 复用超分结果" : "");
    log_info("done %s in %d ms%s", j->name, ev->ms, ev->sr_cached ? " (sr cached)" : "");
    engine_submit_next(st);
}

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
                if (ev.message[0]) log_info("engine note: %s", ev.message);
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

        case DN_EV_DL_DONE:
            toast(st, "模型已就绪");
            break;

        case DN_EV_DL_ERROR:
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
/* Shortcuts and keys                                                       */
/* ------------------------------------------------------------------------ */

static void jump_job(app_state *st, int delta) {
    if (st->count == 0) return;
    int idx = st->selected + delta;
    if (idx < 0) idx = st->count - 1;
    if (idx >= st->count) idx = 0;
    job_focus(st, idx);
}

static void handle_shortcuts(cui_ctx *ui, app_state *st) {
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
/* Background work                                                          */
/* ------------------------------------------------------------------------ */

static void flush_composite(app_state *st) {
    double now = now_seconds();
    for (int i = 0; i < st->count; i++) {
        job *j = &st->jobs[i];
        if (!j->composite_pending || j->compositing || !j->has_result) continue;
        if (j->status != JOB_DONE) continue;
        if (now - j->dirty_at < COMPOSITE_DEBOUNCE) continue;
        j->composite_pending = false;
        request_composite(st, j);
    }
}

static void do_paste(app_state *st) {
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

static void do_save(app_state *st, bool save_as) {
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

static void handle_drops(cui_ctx *ui, app_state *st) {
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

static void open_browse_dialog(cui_app *app, app_state *st) {
    char path[PATH_CAP];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_OPEN;
    opts.title = "选择要处理的图片";
    opts.filter_name = "图片";
    opts.filter = IMAGE_FILTER;
    if (cui_app_file_dialog(app, &opts, path, sizeof path)) enqueue_path(st, path);
}

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

    handle_drops(ui, st);
    handle_shortcuts(ui, st);
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

    job *j = job_selected(st);
    if (j && st->stage_valid) stage_handle_input(st, j);
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

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        ui_topbar(ui, st);
        cui_separator(ui);
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH) {
            ui_sidebar(ui, st);
            cui_separator(ui);
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = PANE_PAD, .gap = PANE_GAP) {
                if (!st->bench && (st->engine_error[0] || st->booting)) {
                    cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER, .pad = 11,
                            .radius = 12, .bg = CUI_COLOR_SURFACE_MUTED, .border = 1) {
                        if (st->booting) cui_spinner(ui, 16);
                        else cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 18, CUI_COLOR_DANGER);
                        cui_column(ui, .width = CUI_FILL, .gap = 2) {
                            if (st->booting) {
                                cui_text(ui, "正在准备 AI 模型", .font = CUI_FONT_MEDIUM, .size = 12.5f);
                                cui_text(ui, st->boot_stage[0] ? st->boot_stage : "加载中...",
                                         .size = 11, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                            } else {
                                cui_text(ui, "推理引擎未就绪", .font = CUI_FONT_MEDIUM, .size = 12.5f);
                                cui_text(ui, st->engine_error, .size = 11, .nowrap = true,
                                         .color = CUI_COLOR_TEXT_SECONDARY);
                            }
                        }
                        if (!st->booting && cui_button(ui, "重试", .variant = CUI_BUTTON_OUTLINE,
                                                        .size = CUI_SIZE_SM, .icon = CUI_ICON_REFRESH_CW)) {
                            st->want_restart = true;
                        }
                    }
                }

                if (j) {
                    ui_job_header(ui, st, j);
                    ui_stage_toolbar(ui, st, j);
                }
                float used = PANE_PAD * 2 + (j ? HEAD_H + 34.0f : 0.0f) + STRIP_H + 26.0f + PANE_GAP * 4
                             + (st->engine_error[0] || st->booting ? 62.0f : 0.0f);
                float canvas = cui_viewport_size(ui).y - TOPBAR_H - used;
                if (canvas < 160.0f) canvas = 160.0f;
                draw_stage_area(ui, st, canvas);
                ui_strip(ui, st);
                ui_status(ui, st, j);
            }
        }
    }

    /* transient toast */
    if (st->toast[0] && now_seconds() < st->toast_until) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad = 8) {
            cui_badge(ui, st->toast, .size = CUI_SIZE_MD, .tone = CUI_TONE_INFO, .solid = true);
        }
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
        do_paste(st);
    }
    if (st->want_save || st->want_save_as) {
        bool as_copy = st->want_save_as;
        st->want_save = st->want_save_as = false;
        do_save(st, as_copy);
    }
    if (st->want_browse) {
        st->want_browse = false;
        open_browse_dialog(app, st);
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
    return job_add(st->ui, st, path) != NULL;
}

void app_test_enqueue(app_state *st, const char *path) { enqueue_path(st, path); }
int app_test_load_pending(void) { return loader_pending(); }

void app_test_set_preset(app_state *st, int index) { apply_preset(st, index); }

job *app_test_selected(app_state *st) { return job_selected(st); }

void app_test_fake_result(app_state *st, int index) {
    if (index < 0 || index >= st->count) return;
    job *j = &st->jobs[index];
    if (!j->orig.rgba) return;
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

