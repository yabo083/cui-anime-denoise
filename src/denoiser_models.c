/* denoiser_models.c -- the model-management surface of the denoiser worker
 * (split from denoiser.c; the thread core lives there). Two halves:
 *
 *   - synchronous queries the UI may call on any thread, before or after
 *     dn_start: dn_model_status / dn_resolve_model / dn_delete_model take
 *     the denoiser lock only long enough to latch the directories, then
 *     talk to the filesystem lock-free -- per-frame calls are fine;
 *   - the download: dn_download_model pushes a ring job like
 *     dn_push_denoise, dn__run_download executes it on the dn thread and
 *     answers with DN_EV_DL_PROGRESS / DN_EV_DL_DONE / DN_EV_DL_ERROR.
 *
 * The engine's same-directory pair invariant (skeleton + .pth weights in
 * one directory) lives in src/engine/models.c; this file only wires the
 * latched directories to it. A finished download is a complete pair --
 * eng_model_download copies the bundled skeleton in and rolls the .pth
 * back out when it cannot.
 */
#define _CRT_SECURE_NO_WARNINGS 1

#include "denoiser_int.h"

#include <stdio.h>
#include <string.h>

/* The latched directories as an eng_config. Only safe once dn__resolve_dirs
 * has run (dn_start does it before spawning; the query paths latch first) --
 * the returned pointers then stay stable for the process lifetime. */
static void dnm__eng_config(eng_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->runtime_dir = g_dn.runtime_dir;
    cfg->models_dir = g_dn.models_dir;
    cfg->user_models_dir = g_dn.user_models_dir;
}

/* ---------------------------------------------------- synchronous query */

const char *dn_models_dir(void) {
    dn__lock();
    dn__resolve_dirs();
    dn__unlock();
    return g_dn.user_models_dir_u8;
}

bool dn_model_status(eng_model m, eng_model_state *out) {
    eng_config cfg;
    if (!out) return false;
    dn__lock();
    dn__resolve_dirs();
    dn__unlock();
    dnm__eng_config(&cfg);
    eng_model_status(&cfg, m, out);
    return true;
}

eng_model dn_resolve_model(eng_model want) {
    eng_config cfg;
    eng_model_state ms;
    if (want == ENG_M_ESRGAN) return ENG_M_ESRGAN;
    dn__lock();
    dn__resolve_dirs();
    dn__unlock();
    dnm__eng_config(&cfg);
    memset(&ms, 0, sizeof ms);
    eng_model_status(&cfg, want, &ms);
    return ms.available ? want : ENG_M_ESRGAN;
}

bool dn_delete_model(eng_model m) {
    eng_config cfg;
    dn__lock();
    dn__resolve_dirs();
    dn__unlock();
    dnm__eng_config(&cfg);
    return eng_model_delete(&cfg, m);
}

/* ------------------------------------------------------------ download */

void dn_download_model(eng_model m) {
    int queued = 0;
    dn__lock();
    if (g_dn.alive && g_dn.jobs_count < DN_QUEUE_CAP) {
        int slot = (g_dn.jobs_head + g_dn.jobs_count) % DN_QUEUE_CAP;
        dn_job *job = &g_dn.jobs[slot];
        memset(job, 0, sizeof *job);
        job->kind = DN_JOB_DOWNLOAD;
        job->model = m;
        g_dn.jobs_count++;
        queued = 1;
    }
    dn__unlock();
    if (queued) {
        WakeConditionVariable(&g_dn.cv);
    } else {
        dn__post_terminal(DN_EV_DL_ERROR, (int)m, "推理引擎未运行。");
    }
}

static void dnm__dl_progress(void *ud, unsigned long long done,
                             unsigned long long total) {
    dn_job *job = (dn_job *)ud;
    dn_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = DN_EV_DL_PROGRESS;
    ev.id = (int)job->model;
    ev.dl_done = done;
    ev.dl_total = total;
    dn__post(&ev);
}

void dn__run_download(dn_job *job) {
    char err[256];
    dn_event ev;
    eng_config cfg;
    int r;
    err[0] = 0;
    dnm__eng_config(&cfg);
    r = eng_model_download(&cfg, job->model, dnm__dl_progress, job,
                           (volatile long *)&job->cancel, err, sizeof err);
    memset(&ev, 0, sizeof ev);
    ev.id = (int)job->model;
    if (r == 0) {
        ev.type = DN_EV_DL_DONE;
    } else {
        ev.type = DN_EV_DL_ERROR;
        snprintf(ev.message, sizeof ev.message, "%s",
                 r == 1 ? "下载已取消。" : (err[0] ? err : "模型下载失败。"));
    }
    dn__post(&ev);
}
