/* denoiser.h -- in-process inference worker around the native engine.
 *
 * One background thread (THREAD_PRIORITY_BELOW_NORMAL) owns the single eng *:
 * engine calls are thread-compatible, not thread-safe, so everything that
 * touches it is serialized there -- engine bring-up, denoise jobs, model
 * downloads and device switches. The UI thread only pushes jobs and drains
 * the event ring once per frame; it never waits for the engine.
 *
 * Differences from the Python worker this replaces:
 *   - input is pixels (img_pixels RGBA), not paths -- the loader already
 *     decoded the image, so nothing re-reads the file or re-encodes previews;
 *   - DONE events carry full-resolution RGB pixels (eng_image, owned by the
 *     receiver); writing the output PNG is the host's job;
 *   - the SR cache is keyed by a caller-supplied source key, not a file hash;
 *   - progress values arrive already mapped to the band the UI expects.
 */
#ifndef DENOISER_H
#define DENOISER_H

#include <stdbool.h>

#include "engine/engine.h"
#include "img.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DN_EV_NONE = 0,
    DN_EV_PROGRESS,     /* id, stage ("decode"/"upscale"/"smooth"), value     */
    DN_EV_DONE,         /* id, rgb, ms, sr_cached, model; message when the
                           requested model fell back to the bundled one      */
    DN_EV_ERROR,        /* id, message; id == 0 means engine-level            */
    DN_EV_CANCELLED,    /* id handled                                         */
    DN_EV_READY,        /* ready_ok, device, using_gpu, note -> message       */
    DN_EV_DL_PROGRESS,  /* id = eng_model; dl_done/dl_total bytes             */
    DN_EV_DL_DONE,      /* id = eng_model                                     */
    DN_EV_DL_ERROR      /* id = eng_model; message                            */
};

typedef struct dn_event {
    int type;
    int id;
    char stage[24];
    float value;                  /* progress, already in the UI band */
    char message[512];            /* error text; READY: eng_note(); DONE:
                                     non-empty when the model fell back     */
    eng_image rgb;                /* DONE only; owned by the receiver */
    int ms;                       /* DONE: engine-side elapsed        */
    bool sr_cached;               /* DONE: SR step came from the cache */
    int model;                    /* DONE: eng_model that produced the result */
    int ready_ok;                 /* READY: 1 prepared, 0 failed      */
    char device[160];             /* READY: eng_device_name()         */
    bool using_gpu;               /* READY: eng_using_gpu()           */
    unsigned long long dl_done, dl_total;  /* DL_PROGRESS bytes      */
} dn_event;

/* dn_state: IDLE = never started / fully shut down; BOOT = thread up, engine
 * bring-up running; READY = prepared and accepting jobs; FAILED = eng_create
 * or eng_prepare failed (last_error says why; dn_restart retries). */
typedef enum dn_state {
    DN_STATE_IDLE = 0,
    DN_STATE_BOOT,
    DN_STATE_READY,
    DN_STATE_FAILED
} dn_state;

/* Spawns the engine thread and returns immediately -- engine bring-up
 * (ORT load, adapter probe, session compile) runs in the background and
 * answers with DN_EV_READY. Resolves <exe>\runtime and <exe>\models, and
 * creates %LOCALAPPDATA%\AnimeDenoise\models when missing.
 *
 * The DN_RUNTIME_DIR / DN_MODELS_DIR / DN_USER_MODELS_DIR environment
 * variables override the three directories (UTF-8); they exist so tests can
 * point the denoiser at a build tree instead of <exe dir>.
 *
 * Safe to call from the UI thread inside a frame. Idempotent: returns true
 * immediately when the thread is already up. */
bool dn_start(eng_device dev);

/* Shutdown + start: the retry path for DN_STATE_FAILED and device changes
 * that need a clean engine. Joins the old thread first -- cancel is signalled
 * to any running job, so the wait is bounded by one tile. Returns false when
 * the old worker outlived its join window and still owns the rings (a slow
 * download chunk can do that): last_error says so and a later retry works
 * once the orphan's epilogue lands. */
bool dn_restart(eng_device dev);

/* Stops the thread (cancels the running job) and joins it. Safe before start
 * and after a failure. */
void dn_shutdown(void);

dn_state dn_state_get(void);
const char *dn_last_error(void);
const char *dn_device_name(void);    /* eng_device_name(), "" before READY */
bool dn_using_gpu(void);
const char *dn_models_dir(void);     /* user models dir, UTF-8 */
eng_device dn_get_device(void);

/* Queues a denoise job on model `model` (ENG_M_ESRGAN/CUGAN/APISR). `src_key`
 * is the caller's stable identity for the source image: together with
 * down_pct and the resolved model it keys the two-entry SR cache, so a re-run
 * that only changed l0 skips the super-resolution pass entirely.
 * The RGBA pixels are copied (alpha stripped) before the call returns.
 * down_pct is 10..100, l0 >= 0. Answers DN_EV_DONE / DN_EV_ERROR /
 * DN_EV_CANCELLED. If the requested model's files are not a complete pair
 * at run time, the job falls back to the bundled ESRGAN and the DONE event
 * carries the fallback note in `message`. */
bool dn_push_denoise(int id, int src_key, const img_pixels *src,
                     int down_pct, double l0, eng_model model);

/* Sets the job's cancel flag; the engine unwinds at the next tile / L0
 * iteration and the job answers DN_EV_CANCELLED. Unknown ids are ignored. */
void dn_cancel(int id);

/* Stores the device preference and applies it on the denoiser thread at the
 * next job boundary: the engine is recreated with the new device, prepared
 * again and DN_EV_READY is re-posted. */
void dn_set_device(eng_device dev);

/* Model management for the settings surface: status is a synchronous file
 * check (no engine needed); downloads are queued on the denoiser thread and
 * answered with DN_EV_DL_*. dn_resolve_model is the selection semantic the
 * engine applies per job: the requested model when its pair is complete,
 * else the bundled ESRGAN. dn_delete_model removes a downloaded pair from
 * the user models dir (synchronous, files only -- already-prepared sessions
 * are unaffected until released). */
bool dn_model_status(eng_model m, eng_model_state *out);
eng_model dn_resolve_model(eng_model want);
bool dn_delete_model(eng_model m);
void dn_download_model(eng_model m);

/* Pops one queued event (1) or returns 0. DONE ownership moves to the
 * caller: free ev.rgb with dn_event_free / eng_image_free when done. */
int dn_poll(dn_event *ev);
void dn_event_free(dn_event *ev);

#ifdef __cplusplus
}
#endif

#endif /* DENOISER_H */
