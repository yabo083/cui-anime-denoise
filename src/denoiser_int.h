/* denoiser_int.h -- internals shared by the denoiser's two translation
 * units: denoiser.c (thread core: engine lifetime, job/event rings, SR
 * cache) and denoiser_models.c (model management: status queries, selection
 * resolution, deletion, the download job). NOT part of the public API --
 * only src/*.c and the test drivers that link the same objects (e.g.
 * tests/test_denoiser.c, which pokes g_dn's flags for lifecycle checks)
 * may include it, and only the denoiser pair may use g_dn at runtime. */
#ifndef ANIME_DENOISER_INT_H
#define ANIME_DENOISER_INT_H

#include "denoiser.h"

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
    eng_model model;      /* DENOISE + DOWNLOAD */
    volatile long cancel;
} dn_job;

/* The job ring, event ring and the published status fields share one
 * critical section; the dn thread owns the eng * and reads the latched
 * directories without locking (dirs_ready is set before the thread
 * spawns). */
typedef struct {
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
    dn_job *running;            /* slot the thread is executing */

    dn_event events[DN_EVENT_CAP];
    int ev_head, ev_count;

    wchar_t runtime_dir[DN_DIR_CAP];
    wchar_t models_dir[DN_DIR_CAP];
    wchar_t user_models_dir[DN_DIR_CAP];
    char user_models_dir_u8[DN_DIR_CAP];
    int dirs_ready;
} dn_shared;

extern dn_shared g_dn;

void dn__lock(void);
void dn__unlock(void);
void dn__resolve_dirs(void);
void dn__post(dn_event *ev);
void dn__post_terminal(int type, int id, const char *message);

/* Implemented in denoiser_models.c, dispatched by the worker loop. */
void dn__run_download(dn_job *job);

#endif /* ANIME_DENOISER_INT_H */
