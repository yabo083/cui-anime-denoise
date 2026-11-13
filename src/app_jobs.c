/* app_jobs.c -- job lifecycle, the loader queue, denoiser submission and the
 * host-side mask composite. app.c owns the frame and the event drain; every
 * function here is called from there or from the ui_* files, so this file has
 * no frame logic of its own (see AGENTS.md's routing table). */

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
#include "ui_fluent.h"


/* ------------------------------------------------------------------------ */
/* Lookups                                                                  */
/* ------------------------------------------------------------------------ */

int job_find(const app_state *st, int dn_id) {
    if (dn_id == 0) return -1;
    for (int i = 0; i < st->count; i++)
        if (st->jobs[i].id == dn_id) return i;
    return -1;
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

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                 */
/* ------------------------------------------------------------------------ */

static void job_drop_textures(cui_ctx *ui, job *j) {
    if (j->orig_tex) { cui_image_destroy(ui, j->orig_tex); j->orig_tex = 0; }
    if (j->res_tex) { cui_image_destroy(ui, j->res_tex); j->res_tex = 0; }
    if (j->fade_tex) { cui_image_destroy(ui, j->fade_tex); j->fade_tex = 0; }
    if (j->raw_tex) { cui_image_destroy(ui, j->raw_tex); j->raw_tex = 0; }
    if (j->mask_tex) { cui_image_destroy(ui, j->mask_tex); j->mask_tex = 0; }
}

void job_free(cui_ctx *ui, job *j) {
    job_drop_textures(ui, j);
    if (j->thumb_tex) { cui_image_destroy(ui, j->thumb_tex); j->thumb_tex = 0; }
    if (j->bg_tex) { cui_image_destroy(ui, j->bg_tex); j->bg_tex = 0; }
    img_free(&j->orig);
    eng_image_free(&j->res);
    img_free_icc(j->icc);
    mask_free(&j->m);
    mask_edges_free(&j->edges);
    if (j->hist) { mask_hist_free(j->hist); j->hist = NULL; }
    /* the rest of the struct is rebuilt by the next job that lands here */
    memset(j, 0, sizeof *j);
}

/* Removes a job from the queue (the tray's hover ×). A running job is asked
 * to cancel first; the stale CANCELLED event is ignored because the slot is
 * reused -- freeing moves the struct's tail over it, so job_find cannot see
 * the old id afterwards. */
void job_remove(cui_ctx *ui, app_state *st, int index) {
    if (index < 0 || index >= st->count) return;
    job *j = &st->jobs[index];
    if (j->status == JOB_RUNNING && j->id) dn_cancel(j->id);
    job_free(ui, j);
    if (index + 1 < st->count)
        memmove(&st->jobs[index], &st->jobs[index + 1],
                (size_t)(st->count - index - 1) * sizeof(job));
    memset(&st->jobs[st->count - 1], 0, sizeof(job));
    st->count--;

    if (st->selected == index) {
        st->selected = -1;
        if (st->count > 0) job_focus(st, index < st->count ? index : st->count - 1);
    } else if (st->selected > index) {
        st->selected--;
    }
    engine_submit_next(st);
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

void out_paths(app_state *st, job *j) {
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
    mask_init(&j->m, j->orig.w, j->orig.h);
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

/* Synchronous add used by the test harness (app_test_add_image) and the paste
 * path: the loader also exists, but the tests count on an immediate job. */
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

job *app_job_add_now(app_state *st, const char *path) {
    return job_add(st->ui, st, path);
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
/* Denoiser submission                                                      */
/* ------------------------------------------------------------------------ */

eng_device device_pref(const app_state *st) {
    return st->set.device == 1 ? ENG_DEV_GPU
         : st->set.device == 2 ? ENG_DEV_CPU
                               : ENG_DEV_AUTO;
}

void engine_start_now(app_state *st) {
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

/* Parameter edits re-run the selected job (spec §9: 改预设/微调即重跑，处理中
 * 改参数立即中止旧任务). A running job is told to cancel and re-queued in the
 * same breath: the denoiser serialises pushes behind the aborted one, so the
 * replacement starts the moment the engine unwinds. The stale CANCELLED event
 * is ignored because job_find keys on the id we just zeroed. Queued jobs take
 * the new parameters too -- the pill's preset is a queue-level intent. */
void job_rerun(app_state *st, job *j) {
    if (!j || !j->orig.rgba) return;
    for (int i = 0; i < st->count; i++) {
        job *q = &st->jobs[i];
        if (q->status == JOB_QUEUED) { q->down_pct = st->down_pct; q->l0 = st->l0; }
    }
    j->down_pct = st->down_pct;
    j->l0 = st->l0;
    if (j->status == JOB_RUNNING && j->id) dn_cancel(j->id);
    j->status = JOB_QUEUED;
    j->id = 0;
    j->progress = 0.0f;
    j->cancel_sent = false;
    out_paths(st, j);
    engine_submit_next(st);
}

/* ------------------------------------------------------------------------ */
/* Result assembly and the host-side mask composite                          */
/* ------------------------------------------------------------------------ */

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

img_pixels *job_raw_result_px(job *j, img_pixels *out) { return job_result_px(j, out); }

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

img_pixels *job_output_px(job *j, img_pixels *out) { return job_composite_px(j, out); }

static void job_res_texture(cui_ctx *ui, job *j, const img_pixels *px) {
    img_pixels prev = {0};
    if (!img_thumbnail(px, DISPLAY_MAX, &prev)) return;
    /* keep the texture being replaced one cross-fade longer: the stage fades
     * it out over UI_T_RESULT seconds instead of letting the result pop in */
    if (j->res_tex) {
        if (j->fade_tex) cui_image_destroy(ui, j->fade_tex);
        j->fade_tex = j->res_tex;
        j->fade_w = j->res_w;
        j->fade_h = j->res_h;
        j->fade_at = now_seconds();
    }
    j->res_tex = cui_image_create(ui, prev.w, prev.h, prev.rgba);
    j->res_w = j->res_tex ? prev.w : 0;
    j->res_h = j->res_tex ? prev.h : 0;
    j->bg_dirty = true;
    /* the unprotected preview no longer matches the new result */
    if (j->raw_tex) { cui_image_destroy(ui, j->raw_tex); j->raw_tex = 0; }
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
    cui_color mc = fl_mask_rgb(st->ui);
    int amax = fl_mask_alpha(st->ui);                   /* spec §4 MASK_RED */
    for (int y = 0; y < th; y++) {
        const uint8_t *row = w + (size_t)y * step * sw;
        for (int x = 0; x < tw; x++) {
            uint8_t v = row[(size_t)x * step];
            uint8_t *p = rgba + ((size_t)y * tw + x) * 4;
            p[0] = mc.r; p[1] = mc.g; p[2] = mc.b;
            p[3] = (uint8_t)((int)v * amax / 255);
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
void job_apply_done(cui_ctx *ui, app_state *st, job *j, dn_event *ev) {
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

/* The composite is CPU work on the UI thread, so it waits for a quiet gap
 * between strokes instead of running per mouse move. */
void flush_composite(app_state *st) {
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
