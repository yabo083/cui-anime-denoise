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
    WB_EV_PROGRESS = 1,  /* value in 0..1, id = job */
    WB_EV_DONE = 2,      /* path/w/h/ms/preview_b64 */
    WB_EV_ERROR = 3,     /* message (worker-level or job-level) */
    WB_EV_CANCELLED = 4, /* id handled */
    WB_EV_READY = 5,     /* worker initialised the model stack */
    WB_EV_BYE = 6,       /* worker acknowledged shutdown */
    WB_EV_DEAD = 9       /* stdout closed / process gone */
};

typedef struct wb_event {
    int id;            /* job id, 0 for worker-level events */
    int type;          /* WB_EV_* */
    float value;       /* progress 0..1 */
    char path[1024];   /* done: output PNG path, UTF-8 */
    char message[512]; /* error/dead: text; ready: GPU name */
    char *preview_b64; /* done: base64 PNG of the result; owned by receiver, free() it */
    char *src_preview_b64; /* done: base64 PNG of the source (formats CUI cannot load) */
    int w, h;          /* done: full output size in pixels */
    int ms;            /* done: worker-side elapsed milliseconds */
} wb_event;

/* Overrides how long wb_start waits for "ready" (default 180 s: the first run
 * imports torch, loads the model and may download weights). Call before wb_start. */
void wb_set_start_timeout(int ms);

/* Spawns the worker and waits for its "ready" event. The ready event stays in the poll queue so the
 * caller sees it like any other event. Returns false on spawn failure, worker
 * death before ready, or timeout; wb_last_error() explains.
 * models_dir may be NULL (worker picks its default cache dir). */
bool wb_start(const char *python_exe, const char *worker_py, const char *venv_dir, const char *models_dir);

/* Fire-and-forget job; progress/done/error arrive through wb_poll. */
void wb_push_denoise(int id, const char *path, double scale, double l0, const char *out_dir);

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

/* base64 PNG -> RGBA8 pixels. Returns 1 on success; *rgba must be released with
 * wb_free_pixels(). Returns 0 (and NULL/0) on malformed base64 or PNG.
 * Requires the CUI PNG decoder (cui__png_decode/cui__png_free). */
int wb_decode_preview(const char *b64, unsigned char **rgba, int *w, int *h);
void wb_free_pixels(unsigned char *rgba);

void wb_event_free(wb_event *ev);

/* "cuda"/"cpu", torch version, GPU description -- filled by the ready event. */
const char *wb_device(void);
const char *wb_torch(void);
const char *wb_gpu(void);
const char *wb_last_error(void);

/* UTF-8 path of the child's stderr log (%TEMP%\cui_anime_denoise_worker.log). */
const char *wb_log_path(void);

#ifdef __cplusplus
}
#endif

#endif /* CUI_ANIME_BACKEND_H */
