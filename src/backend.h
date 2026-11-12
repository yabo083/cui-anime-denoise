/* backend.h -- C side of the Python denoise worker sidecar.
 *
 * Spawns `<python_exe> -u <worker_py>` as a hidden child process, talks the
 * JSONL protocol over its stdin/stdout (see docs/worker-protocol.md), and
 * turns every stdout line into an event the UI can poll.
 *
 * The child's stderr goes to a log file in %TEMP% (see wb_log_path()), which is
 * where the worker writes its tracebacks.
 *
 * Single worker per process; all functions are thread safe.
 */
#ifndef CUI_ANIME_BACKEND_H
#define CUI_ANIME_BACKEND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WB_EV_NONE = 0,
    WB_EV_PROGRESS = 1,   /* value in 0..1, stage = "upscale"/"smooth"/..., id = job */
    WB_EV_DONE = 2,       /* path/w/h/ms/preview_b64/sr_cached */
    WB_EV_ERROR = 3,      /* message (worker-level when id == 0) */
    WB_EV_CANCELLED = 4,  /* id handled */
    WB_EV_READY = 5,      /* worker initialised: device/gpu/model_ready */
    WB_EV_BYE = 6,        /* worker acknowledged shutdown */
    WB_EV_DEAD = 9,       /* stdout closed / process gone */
    WB_EV_BOOT = 10,      /* value/message: init or model-download progress */
    WB_EV_COMPOSITED = 11,/* path/preview_b64: mask applied to the cached result */
    WB_EV_MODELS = 12     /* message = models dir, bytes = size, model_ready */
};

typedef struct wb_event {
    int id;            /* job id, 0 for worker-level events */
    int type;          /* WB_EV_* */
    float value;       /* progress 0..1 */
    char stage[24];    /* progress: "decode" / "upscale" / "smooth" / ... */
    char path[1024];   /* done/composited: output PNG path, UTF-8 */
    char message[512]; /* error/dead text; ready: GPU name; models: dir; boot: stage */
    char *preview_b64; /* done/composited: base64 PNG preview; owned by receiver */
    int w, h;          /* done: full output size in pixels */
    int ms;            /* done: worker-side elapsed milliseconds */
    bool sr_cached;    /* done: the upscaler result came from the worker's cache */
    bool model_ready;  /* ready/models: the ESRGAN weights are on disk */
    unsigned long long bytes; /* models: weight file size */
} wb_event;

/* Boot progress of the worker, for the banner the UI draws while tasks wait.
 * IDLE = not started; SPAWNING = init sent, no boot/ready yet; BOOTING = at
 * least one boot event; READY = ready arrived; DEAD = the process is gone. */
typedef enum wb_state {
    WB_STATE_IDLE = 0,
    WB_STATE_SPAWNING,
    WB_STATE_BOOTING,
    WB_STATE_READY,
    WB_STATE_DEAD
} wb_state;

/* Overrides how long wb_start waits for "ready" (default 180 s: the first run
 * imports torch, loads the model and may download weights). Call before wb_start. */
void wb_set_start_timeout(int ms);

/* Everything wb_start does except waiting for "ready": pipes, reader thread,
 * CreateProcessW and the init command. Returns as soon as the child is running
 * (well under the frame budget), so the UI thread may call this from a frame.
 * Returns false only when the worker could not be spawned (state back to IDLE,
 * wb_last_error() explains). models_dir may be NULL (worker picks
 * <runtime>/models). */
bool wb_start_async(const char *python_exe, const char *worker_py, const char *venv_dir,
                    const char *models_dir);

/* wb_start_async() + waits for "ready" (blocking; UI thread must not call it,
 * tests and the shutdown path may). The ready event stays in the poll queue so
 * the caller sees it like any other event. Returns false on spawn failure,
 * worker death before ready, or timeout; wb_last_error() explains. */
bool wb_start(const char *python_exe, const char *worker_py, const char *venv_dir,
              const char *models_dir);

/* Current boot state; IDLE before the first start and after shutdown/kill. */
wb_state wb_state_get(void);

/* Denoise one file into `out_path`. Progress/done/error arrive through wb_poll. */
void wb_push_denoise(int id, const char *path, int down_pct, double l0, const char *out_path);

/* Re-blend the cached denoised result with a full-resolution 8-bit grey mask
 * (PNG, base64) and write `out_path`. Answered with WB_EV_COMPOSITED. */
void wb_push_composite(int id, const char *mask_png_b64, const char *out_path);

/* "status" or "download" for the ESRGAN weights; answered with WB_EV_MODELS. */
void wb_models(const char *action);

/* Asks the worker to abort job `id`; it answers with WB_EV_CANCELLED. */
void wb_cancel(int id);

/* Sends shutdown, waits for the process to exit, closes everything. Events the
 * worker emitted before exiting (WB_EV_BYE) remain pollable. */
void wb_shutdown(void);

/* Hard stop: TerminateProcess + reap. No WB_EV_BYE, the reader posts WB_EV_DEAD. */
void wb_kill(void);

/* Pops one event. Returns 1 & fills *ev, or 0 when the queue is empty.
 * Caller frees ev->preview_b64 (free() or wb_event_free()). */
int wb_poll(wb_event *ev);

bool wb_alive(void);

/* base64 PNG -> RGBA8 pixels (via img_decode_memory). Returns 1 on success;
 * *rgba must be released with wb_free_pixels(). */
int wb_decode_preview(const char *b64, unsigned char **rgba, int *w, int *h);
void wb_free_pixels(unsigned char *rgba);

void wb_event_free(wb_event *ev);

/* "cuda"/"cpu", torch version, GPU description -- filled by the ready event. */
const char *wb_device(void);
const char *wb_torch(void);
const char *wb_gpu(void);
const char *wb_last_error(void);

/* ready: the ESRGAN weights exist on disk; wb_models_dir() is where. */
bool wb_model_ready(void);
const char *wb_models_dir(void);

/* UTF-8 path of the child's stderr log (%TEMP%\cui_anime_denoise_worker.log). */
const char *wb_log_path(void);

#ifdef __cplusplus
}
#endif

#endif /* CUI_ANIME_BACKEND_H */
