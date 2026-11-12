/* denoiser.c -- the in-process inference worker. See denoiser.h for the API;
 * this file owns the engine lifetime, the job/event rings, the two-entry SR
 * cache and the idle session release.
 *
 * Threading: one background thread at THREAD_PRIORITY_BELOW_NORMAL owns the
 * eng * (engine calls are thread-compatible, not thread-safe). The UI thread
 * pushes onto the job ring and drains the event ring once per frame; the two
 * rings plus the published status fields are the only shared state, all under
 * one critical section.
 *
 * Progress parity with the Python worker: the engine reports "upscale" /
 * "smooth" as 0..1; the bands the UI learned to show (decode 0.10, upscale
 * 0.12..0.68, smooth 0.70..0.95, done 1.0) are applied here so app.c does not
 * have to know they ever came from another process.
 */
#define _CRT_SECURE_NO_WARNINGS 1

#include "denoiser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define DN_QUEUE_CAP         8      /* the app serializes jobs anyway */
#define DN_EVENT_CAP         96     /* ~1.6 s of progress chatter at 60 fps */
#define DN_SR_CACHE          2      /* worker's LRU, minus one */
#define DN_IDLE_RELEASE_MS   60000  /* quiet period before sessions release */
#define DN_JOIN_MS           8000   /* bounded by one tile / one L0 step */
#define DN_DIR_CAP           520

typedef enum {
    DN_JOB_DENOISE = 1,
    DN_JOB_SET_DEVICE,
    DN_JOB_DOWNLOAD
} dn_job_kind;

typedef struct {
    int kind;
    int id;
    int src_key;
    int down_pct;
    double l0;
    eng_image src;        /* RGB8, copied and alpha-stripped at push */
    eng_device device;    /* SET_DEVICE */
    eng_model model;      /* DOWNLOAD */
    volatile long cancel;
} dn_job;

/* LRU-2 SR cache: index 0 is the most recently used entry. */
typedef struct {
    int src_key;
    int down_pct;
    eng_image im;
} dn_sr_entry;

static struct {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    int lock_ready;
    int alive;
    int stop;
    HANDLE thread;

    dn_state state;             /* set on the dn thread, read via accessors */
    char last_error[512];
    char device_name[160];
    int using_gpu;
    eng_device pref;

    dn_job jobs[DN_QUEUE_CAP];
    int jobs_head, jobs_count;
    dn_job *running;            /* slot the thread is executing (cancel target) */

    dn_event events[DN_EVENT_CAP];
    int ev_head, ev_count;

    wchar_t runtime_dir[DN_DIR_CAP];
    wchar_t models_dir[DN_DIR_CAP];
    wchar_t user_models_dir[DN_DIR_CAP];
    char user_models_dir_u8[DN_DIR_CAP];
    int dirs_ready;
} g_dn;

static void dn__lock(void) {
    if (!g_dn.lock_ready) {
        InitializeCriticalSection(&g_dn.lock);
        InitializeConditionVariable(&g_dn.cv);
        g_dn.lock_ready = 1;
    }
    EnterCriticalSection(&g_dn.lock);
}
static void dn__unlock(void) { LeaveCriticalSection(&g_dn.lock); }

/* ---------------------------------------------------------------- events */

/* The ring never blocks the engine: a full ring can only be backed-up
 * progress chatter, so the oldest PROGRESS event is dropped to make room.
 * Terminal events always land; the pathological overflow drops the new one. */
static void dn__post(dn_event *ev) {
    dn__lock();
    if (g_dn.ev_count >= DN_EVENT_CAP &&
        g_dn.events[g_dn.ev_head].type == DN_EV_PROGRESS) {
        g_dn.ev_head = (g_dn.ev_head + 1) % DN_EVENT_CAP;
        g_dn.ev_count--;
    }
    if (g_dn.ev_count >= DN_EVENT_CAP) {
        dn__unlock();
        eng_image_free(&ev->rgb); /* would leak otherwise */
        return;
    }
    int slot = (g_dn.ev_head + g_dn.ev_count) % DN_EVENT_CAP;
    g_dn.events[slot] = *ev;
    g_dn.ev_count++;
    dn__unlock();
}

static void dn__post_progress(int id, const char *stage, float value) {
    dn_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = DN_EV_PROGRESS;
    ev.id = id;
    snprintf(ev.stage, sizeof ev.stage, "%s", stage);
    ev.value = value;
    dn__post(&ev);
}

static void dn__post_terminal(int type, int id, const char *message) {
    dn_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.id = id;
    snprintf(ev.message, sizeof ev.message, "%s", message ? message : "");
    dn__post(&ev);
}

/* READY doubles as the published-state update: the fields the accessors read
 * are refreshed under the same lock the event is posted with, so a poll that
 * returns READY always sees a consistent name/gpu/error triple. */
static void dn__post_ready(int ok, const char *device, int gpu, const char *note_or_err) {
    dn_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = DN_EV_READY;
    ev.ready_ok = ok;
    ev.using_gpu = gpu != 0;
    snprintf(ev.device, sizeof ev.device, "%s", device ? device : "");
    snprintf(ev.message, sizeof ev.message, "%s", note_or_err ? note_or_err : "");

    dn__lock();
    g_dn.state = ok ? DN_STATE_READY : DN_STATE_FAILED;
    snprintf(g_dn.device_name, sizeof g_dn.device_name, "%s", ev.device);
    g_dn.using_gpu = ev.using_gpu;
    snprintf(g_dn.last_error, sizeof g_dn.last_error, "%s", ok ? "" : ev.message);
    dn__unlock();
    dn__post(&ev);
}

int dn_poll(dn_event *ev) {
    if (!ev) return 0;
    memset(ev, 0, sizeof *ev);
    if (!g_dn.lock_ready) return 0;
    dn__lock();
    if (g_dn.ev_count == 0) {
        dn__unlock();
        return 0;
    }
    *ev = g_dn.events[g_dn.ev_head];
    memset(&g_dn.events[g_dn.ev_head], 0, sizeof g_dn.events[g_dn.ev_head]);
    g_dn.ev_head = (g_dn.ev_head + 1) % DN_EVENT_CAP;
    g_dn.ev_count--;
    dn__unlock();
    return 1;
}

void dn_event_free(dn_event *ev) {
    if (ev) eng_image_free(&ev->rgb);
}

/* ------------------------------------------------------------ directories */

static void join_w(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *leaf) {
    size_t n = dir ? wcslen(dir) : 0;
    if (n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/'))
        swprintf(out, cap, L"%s%s", dir, leaf);
    else
        swprintf(out, cap, L"%s\\%s", dir, leaf);
}

static void env_to_wide(const char *env, wchar_t *out, size_t cap) {
    if (!env || !*env || MultiByteToWideChar(CP_UTF8, 0, env, -1, out, (int)cap) <= 0)
        out[0] = 0;
}

/* Resolves the three engine directories once per process. Environment
 * variables (DN_RUNTIME_DIR / DN_MODELS_DIR / DN_USER_MODELS_DIR, UTF-8)
 * override the defaults; they exist for tests, which run away from the
 * staged <exe>\runtime + <exe>\models layout. */
static void dn__resolve_dirs(void) {
    wchar_t exe[DN_DIR_CAP];
    wchar_t tmp[DN_DIR_CAP];
    wchar_t data[DN_DIR_CAP];

    if (g_dn.dirs_ready) return;

    exe[0] = 0;
    GetModuleFileNameW(NULL, exe, DN_DIR_CAP);
    wchar_t *end = wcsrchr(exe, L'\\');
    if (end) *end = 0;

    env_to_wide(getenv("DN_RUNTIME_DIR"), tmp, DN_DIR_CAP);
    if (tmp[0]) wcsncpy(g_dn.runtime_dir, tmp, DN_DIR_CAP - 1);
    else join_w(g_dn.runtime_dir, DN_DIR_CAP, exe, L"runtime");

    env_to_wide(getenv("DN_MODELS_DIR"), tmp, DN_DIR_CAP);
    if (tmp[0]) wcsncpy(g_dn.models_dir, tmp, DN_DIR_CAP - 1);
    else join_w(g_dn.models_dir, DN_DIR_CAP, exe, L"models");

    env_to_wide(getenv("DN_USER_MODELS_DIR"), tmp, DN_DIR_CAP);
    if (tmp[0]) {
        wcsncpy(g_dn.user_models_dir, tmp, DN_DIR_CAP - 1);
    } else {
        const char *la = getenv("LOCALAPPDATA");
        data[0] = 0;
        if (la && *la) env_to_wide(la, data, DN_DIR_CAP);
        if (data[0]) {
            wchar_t ad[DN_DIR_CAP];
            join_w(ad, DN_DIR_CAP, data, L"AnimeDenoise");
            join_w(g_dn.user_models_dir, DN_DIR_CAP, ad, L"models");
        } else {
            join_w(g_dn.user_models_dir, DN_DIR_CAP, exe, L"models-user");
        }
    }
    g_dn.user_models_dir[DN_DIR_CAP - 1] = 0;
    g_dn.runtime_dir[DN_DIR_CAP - 1] = 0;
    g_dn.models_dir[DN_DIR_CAP - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, g_dn.user_models_dir, -1,
                        g_dn.user_models_dir_u8, DN_DIR_CAP, NULL, NULL);
    g_dn.dirs_ready = 1;
}

/* -------------------------------------------------------------- SR cache */

static int cache_find(dn_sr_entry c[DN_SR_CACHE], int src_key, int down_pct) {
    if (c[0].im.rgb && c[0].src_key == src_key && c[0].down_pct == down_pct)
        return 1;
    if (c[1].im.rgb && c[1].src_key == src_key && c[1].down_pct == down_pct) {
        dn_sr_entry t = c[0];
        c[0] = c[1];           /* promote the hit to MRU */
        c[1] = t;
        return 1;
    }
    return 0;
}

static void cache_put(dn_sr_entry c[DN_SR_CACHE], int src_key, int down_pct,
                      eng_image im) {
    eng_image_free(&c[1].im);  /* evict the least recently used */
    c[1] = c[0];
    c[0].src_key = src_key;
    c[0].down_pct = down_pct;
    c[0].im = im;
}

static int copy_image(const eng_image *src, eng_image *out) {
    size_t n = (size_t)src->w * (size_t)src->h * 3;
    out->w = src->w;
    out->h = src->h;
    out->rgb = (uint8_t *)malloc(n);
    if (!out->rgb) return -1;
    memcpy(out->rgb, src->rgb, n);
    return 0;
}

/* ------------------------------------------------------------- job runner */

typedef struct {
    int job_id;
    char stage[24];
    float value;                 /* last posted, in the UI band */
} dn_prog;

/* eng_progress_fn: remaps the engine's per-stage 0..1 into the band the UI
 * already shows (upscale 0.12..0.68, smooth 0.70..0.95), coalesced to ~1%
 * steps like the Python worker. Never goes backwards, even across a stage
 * change -- that was the worker's rule and the progress bar relies on it. */
static void dn__progress(void *ud, const char *stage, float v01) {
    dn_prog *p = (dn_prog *)ud;
    float base = 0.0f, span = 1.0f;
    if (!stage || !(v01 >= 0.0f)) return;   /* NaN guard */
    if (v01 > 1.0f) v01 = 1.0f;
    if (strcmp(stage, "upscale") == 0) { base = 0.12f; span = 0.56f; }
    else if (strcmp(stage, "smooth") == 0) { base = 0.70f; span = 0.25f; }
    float v = base + span * v01;
    if (strcmp(stage, p->stage) == 0 && v < p->value + 0.01f && v01 < 1.0f)
        return;
    if (v < p->value) v = p->value;
    snprintf(p->stage, sizeof p->stage, "%s", stage);
    p->value = v;
    dn__post_progress(p->job_id, stage, v);
}

static void run_denoise(eng *e, dn_job *job, dn_sr_entry cache[DN_SR_CACHE]) {
    char err[512];
    LARGE_INTEGER t0, t1, freq;
    dn_prog prog;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    memset(&prog, 0, sizeof prog);
    prog.job_id = job->id;
    err[0] = 0;

    /* decode already happened on the loader thread; post the stage once so
     * the UI's stage label still starts the same way */
    dn__post_progress(job->id, "decode", 0.10f);
    prog.value = 0.10f;
    snprintf(prog.stage, sizeof prog.stage, "decode");

    if (job->cancel) {
        dn__post_terminal(DN_EV_CANCELLED, job->id, NULL);
        return;
    }
    if (!e) {
        dn__post_terminal(DN_EV_ERROR, job->id, "推理引擎未就绪。");
        return;
    }
    /* sessions may have been released by the idle timer; eng_prepare is a
     * no-op when the model is still compiled */
    if (!eng_prepare(e, ENG_M_ESRGAN, err, sizeof err)) {
        dn__post_terminal(DN_EV_ERROR, job->id,
                          err[0] ? err : "模型编译失败。");
        return;
    }

    eng_image *sr;
    int sr_cached;
    if (cache_find(cache, job->src_key, job->down_pct)) {
        sr = &cache[0].im;
        sr_cached = 1;
        /* the band's upscale leg is already spent; land the bar where a
         * finished pass would have left it */
        dn__post_progress(job->id, "upscale", 0.68f);
        prog.value = 0.68f;
    } else {
        eng_image out = {0, 0, NULL};
        eng_params p;
        memset(&p, 0, sizeof p);
        p.model = ENG_M_ESRGAN;
        p.down = job->down_pct / 100.0;
        int r = eng_upscale(e, &job->src, &p, &out, dn__progress, &prog,
                            (volatile long *)&job->cancel, err, sizeof err);
        if (r == 1) {
            eng_image_free(&out);
            dn__post_terminal(DN_EV_CANCELLED, job->id, NULL);
            return;
        }
        if (r != 0) {
            eng_image_free(&out);
            dn__post_terminal(DN_EV_ERROR, job->id,
                              err[0] ? err : "超分失败。");
            return;
        }
        cache_put(cache, job->src_key, job->down_pct, out);
        sr = &cache[0].im;
        sr_cached = 0;
    }

    eng_image result = {0, 0, NULL};
    if (job->l0 > 0.0) {
        int r = eng_l0(sr, job->l0, &result, 0, dn__progress, &prog,
                       (volatile long *)&job->cancel);
        if (r == 1) {
            eng_image_free(&result);
            dn__post_terminal(DN_EV_CANCELLED, job->id, NULL);
            return;
        }
        if (r != 0) {
            eng_image_free(&result);
            dn__post_terminal(DN_EV_ERROR, job->id, "L0 平滑失败。");
            return;
        }
    } else {
        /* the cache keeps its copy; the app always gets its own buffer */
        if (copy_image(sr, &result) != 0) {
            dn__post_terminal(DN_EV_ERROR, job->id, "内存不足。");
            return;
        }
    }

    /* the worker checked once more before reporting, so a cancel that landed
     * during the last stage still wins over a finished image */
    if (job->cancel) {
        eng_image_free(&result);
        dn__post_terminal(DN_EV_CANCELLED, job->id, NULL);
        return;
    }

    QueryPerformanceCounter(&t1);
    {
        dn_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = DN_EV_DONE;
        ev.id = job->id;
        ev.rgb = result;                       /* ownership moves */
        ev.ms = (int)((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
        ev.sr_cached = sr_cached != 0;
        dn__post(&ev);
    }
}

static void dl_progress(void *ud, unsigned long long done,
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

static void run_download(dn_job *job) {
    char err[256];
    dn_event ev;
    err[0] = 0;
    int r = eng_model_download(job->model, g_dn.user_models_dir,
                               dl_progress, job,
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

/* Engine bring-up, on the dn thread only. One READY event either way:
 * ready_ok + device (+ eng_note for DML->CPU fallbacks), or the error text
 * the UI shows as a retryable banner. */
static eng *dn__boot(const eng_config *cfg) {
    char err[512];
    eng *e;
    err[0] = 0;
    e = eng_create(cfg, err, sizeof err);
    if (e && eng_prepare(e, ENG_M_ESRGAN, err, sizeof err)) {
        const char *note = eng_note(e);
        dn__post_ready(1, eng_device_name(e), eng_using_gpu(e) ? 1 : 0,
                       (note && *note) ? note : "");
        return e;
    }
    if (e) eng_destroy(e);
    dn__post_ready(0, "", 0, err[0] ? err : "推理引擎启动失败。");
    return NULL;
}

/* ------------------------------------------------------------ main loop */

static DWORD WINAPI dn_main(void *arg) {
    eng_config cfg;
    eng *e = NULL;
    dn_sr_entry cache[DN_SR_CACHE];
    ULONGLONG last_busy;
    (void)arg;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    memset(&cfg, 0, sizeof cfg);
    cfg.runtime_dir = g_dn.runtime_dir;
    cfg.models_dir = g_dn.models_dir;
    cfg.user_models_dir = g_dn.user_models_dir;
    cfg.device = g_dn.pref;
    cfg.threads = 0;

    /* user models live under the data dir and may not exist yet (fresh
     * install): the engine treats a missing dir as "no downloads" but the
     * download path needs it to land files in */
    {
        wchar_t parent[DN_DIR_CAP];
        wcsncpy(parent, g_dn.user_models_dir, DN_DIR_CAP - 1);
        parent[DN_DIR_CAP - 1] = 0;
        wchar_t *end = wcsrchr(parent, L'\\');
        if (end) *end = 0;
        CreateDirectoryW(parent, NULL);
        CreateDirectoryW(g_dn.user_models_dir, NULL);
    }

    memset(cache, 0, sizeof cache);
    e = dn__boot(&cfg);
    last_busy = GetTickCount64();

    for (;;) {
        dn_job *job = NULL;
        int stop;

        dn__lock();
        for (;;) {
            if (g_dn.stop || g_dn.jobs_count > 0) break;
            SleepConditionVariableCS(&g_dn.cv, &g_dn.lock, 1000);
            /* ~60 s idle: hand the GPU memory back. The next job re-prepares
             * lazily (eng_prepare is a no-op while sessions are alive). */
            if (!g_dn.stop && g_dn.jobs_count == 0 && e &&
                GetTickCount64() - last_busy > DN_IDLE_RELEASE_MS) {
                LeaveCriticalSection(&g_dn.lock);
                eng_release_sessions(e);
                last_busy = GetTickCount64();
                EnterCriticalSection(&g_dn.lock);
            }
        }
        stop = g_dn.stop;
        if (!stop && g_dn.jobs_count > 0) {
            job = &g_dn.jobs[g_dn.jobs_head];
            g_dn.jobs_head = (g_dn.jobs_head + 1) % DN_QUEUE_CAP;
            g_dn.jobs_count--;
            g_dn.running = job;
        }
        dn__unlock();
        if (stop) break;
        if (!job) continue;

        switch (job->kind) {
        case DN_JOB_DENOISE:
            run_denoise(e, job, cache);
            break;
        case DN_JOB_SET_DEVICE:
            if (e) { eng_destroy(e); e = NULL; }
            cfg.device = job->device;
            dn__lock();
            g_dn.pref = job->device;
            dn__unlock();
            e = dn__boot(&cfg);
            break;
        case DN_JOB_DOWNLOAD:
            run_download(job);
            break;
        default:
            break;
        }
        eng_image_free(&job->src);
        job->src.rgb = NULL;

        dn__lock();
        g_dn.running = NULL;
        dn__unlock();
        WakeConditionVariable(&g_dn.cv); /* a pusher may be waiting for room */
        last_busy = GetTickCount64();
    }

    /* shutdown: drop queued jobs (their src buffers are ours), free the
     * cache and the engine, and mark the thread gone */
    dn__lock();
    g_dn.running = NULL;
    while (g_dn.jobs_count > 0) {
        dn_job *j = &g_dn.jobs[g_dn.jobs_head];
        eng_image_free(&j->src);
        g_dn.jobs_head = (g_dn.jobs_head + 1) % DN_QUEUE_CAP;
        g_dn.jobs_count--;
    }
    g_dn.alive = 0;
    g_dn.state = DN_STATE_IDLE;
    dn__unlock();
    eng_image_free(&cache[0].im);
    eng_image_free(&cache[1].im);
    if (e) eng_destroy(e);
    return 0;
}

/* -------------------------------------------------------------- public */

bool dn_start(eng_device dev) {
    HANDLE t;
    dn__lock();
    if (g_dn.alive) {
        dn__unlock();
        return true;   /* already up (READY or FAILED): use dn_restart */
    }
    g_dn.stop = 0;
    g_dn.state = DN_STATE_BOOT;
    g_dn.last_error[0] = 0;
    g_dn.device_name[0] = 0;
    g_dn.using_gpu = 0;
    g_dn.pref = dev;
    g_dn.running = NULL;
    dn__resolve_dirs();
    g_dn.alive = 1;
    dn__unlock();

    t = CreateThread(NULL, 0, dn_main, NULL, 0, NULL);
    if (!t) {
        dn__lock();
        g_dn.alive = 0;
        g_dn.state = DN_STATE_IDLE;
        snprintf(g_dn.last_error, sizeof g_dn.last_error,
                 "CreateThread failed (%lu)", GetLastError());
        dn__unlock();
        return false;
    }
    dn__lock();
    g_dn.thread = t;
    dn__unlock();
    return true;
}

void dn_restart(eng_device dev) {
    dn_shutdown();
    dn_start(dev);
}

void dn_shutdown(void) {
    HANDLE t;
    DWORD w;
    int joined;

    dn__lock();
    g_dn.stop = 1;
    for (int i = 0; i < g_dn.jobs_count; i++) {
        int slot = (g_dn.jobs_head + i) % DN_QUEUE_CAP;
        g_dn.jobs[slot].cancel = 1;
    }
    if (g_dn.running) g_dn.running->cancel = 1;
    t = g_dn.thread;
    dn__unlock();
    WakeAllConditionVariable(&g_dn.cv);

    joined = 1;
    if (t) {
        w = WaitForSingleObject(t, DN_JOIN_MS);
        if (w == WAIT_TIMEOUT) joined = 0;  /* one tile can outlive the wait */
        CloseHandle(t);
        dn__lock();
        g_dn.thread = NULL;
        dn__unlock();
    }
    if (!joined) return;   /* the thread still owns g_dn; leave it to finish */

    /* the thread is gone: events the app never polled would leak pixels */
    dn__lock();
    while (g_dn.ev_count > 0) {
        dn_event *ev = &g_dn.events[g_dn.ev_head];
        eng_image_free(&ev->rgb);
        memset(ev, 0, sizeof *ev);
        g_dn.ev_head = (g_dn.ev_head + 1) % DN_EVENT_CAP;
        g_dn.ev_count--;
    }
    dn__unlock();
}

dn_state dn_state_get(void) { return g_dn.state; }
const char *dn_last_error(void) { return g_dn.last_error; }
const char *dn_device_name(void) { return g_dn.device_name; }
bool dn_using_gpu(void) { return g_dn.using_gpu != 0; }
eng_device dn_get_device(void) { return g_dn.pref; }
const char *dn_models_dir(void) {
    dn__lock();
    dn__resolve_dirs();
    dn__unlock();
    return g_dn.user_models_dir_u8;
}

bool dn_push_denoise(int id, int src_key, const img_pixels *src,
                     int down_pct, double l0) {
    uint8_t *rgb;
    size_t n;
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0) return false;
    if (down_pct < 1 || down_pct > 100 || !(l0 >= 0.0)) return false;
    n = (size_t)src->w * (size_t)src->h;
    if (n > (SIZE_MAX / 3)) return false;
    rgb = (uint8_t *)malloc(n * 3);
    if (!rgb) return false;
    {
        const uint8_t *s = src->rgba;
        uint8_t *d = rgb;
        for (size_t i = 0; i < n; i++, s += 4, d += 3) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }

    dn__lock();
    if (!g_dn.alive || g_dn.jobs_count >= DN_QUEUE_CAP) {
        dn__unlock();
        free(rgb);
        return false;
    }
    {
        int slot = (g_dn.jobs_head + g_dn.jobs_count) % DN_QUEUE_CAP;
        dn_job *job = &g_dn.jobs[slot];
        memset(job, 0, sizeof *job);
        job->kind = DN_JOB_DENOISE;
        job->id = id;
        job->src_key = src_key;
        job->down_pct = down_pct;
        job->l0 = l0;
        job->src.w = src->w;
        job->src.h = src->h;
        job->src.rgb = rgb;
        g_dn.jobs_count++;
    }
    dn__unlock();
    WakeConditionVariable(&g_dn.cv);
    return true;
}

void dn_cancel(int id) {
    int i;
    dn__lock();
    if (g_dn.running && g_dn.running->id == id)
        InterlockedExchange(&g_dn.running->cancel, 1);
    for (i = 0; i < g_dn.jobs_count; i++) {
        int slot = (g_dn.jobs_head + i) % DN_QUEUE_CAP;
        if (g_dn.jobs[slot].kind == DN_JOB_DENOISE && g_dn.jobs[slot].id == id)
            InterlockedExchange(&g_dn.jobs[slot].cancel, 1);
    }
    dn__unlock();
    /* a queued cancel lands without the engine: nothing to wake (the job is
     * already dequeued by id), but a running one needs no nudge either --
     * the engine polls the flag itself */
}

void dn_set_device(eng_device dev) {
    int queued = 0;
    dn__lock();
    g_dn.pref = dev;
    if (g_dn.alive && g_dn.jobs_count < DN_QUEUE_CAP) {
        int slot = (g_dn.jobs_head + g_dn.jobs_count) % DN_QUEUE_CAP;
        dn_job *job = &g_dn.jobs[slot];
        memset(job, 0, sizeof *job);
        job->kind = DN_JOB_SET_DEVICE;
        job->device = dev;
        g_dn.jobs_count++;
        queued = 1;
    }
    dn__unlock();
    if (queued) WakeConditionVariable(&g_dn.cv);
}

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

bool dn_model_status(eng_model m, eng_model_state *out) {
    eng_config cfg;
    if (!out) return false;
    dn__lock();
    dn__resolve_dirs();
    memset(&cfg, 0, sizeof cfg);
    cfg.runtime_dir = g_dn.runtime_dir;
    cfg.models_dir = g_dn.models_dir;
    cfg.user_models_dir = g_dn.user_models_dir;
    dn__unlock();
    eng_model_status(&cfg, m, out);
    return true;
}
