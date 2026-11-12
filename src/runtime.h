/* AI runtime bootstrap.
 *
 * The app ships without Python or torch (that would be a 3 GB installer).
 * On first run this module builds the runtime on demand inside
 * %LOCALAPPDATA%\AnimeDenoise\runtime:
 *
 *   bin\uv.exe            uv, a single self-contained binary
 *   pythons\              interpreters uv manages (3.12, torch wheels target it)
 *   python\Scripts\python.exe   the venv the worker runs on
 *   models\               ESRGAN weights (the worker downloads these)
 *
 * The steps are: download uv -> `uv python install 3.12` -> `uv venv` ->
 * `uv pip install torch` (CUDA build when nvidia-smi answers, CPU otherwise) ->
 * spandrel/numpy/pillow/psutil. Every step streams its output into the status
 * line so a slow install does not look like a hang.
 *
 * All of it runs on one worker thread; the UI polls rt_get() and may cancel.
 */
#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdbool.h>

typedef enum rt_state {
    RT_UNKNOWN = 0,
    RT_MISSING,       /* no venv yet: the app cannot denoise */
    RT_INSTALLING,
    RT_READY,
    RT_FAILED
} rt_state;

typedef struct rt_info {
    rt_state state;
    float progress;          /* 0..1, coarse (per step) */
    char stage[160];         /* what is happening now */
    char log_tail[1024];     /* last line of tool output */
    char error[512];
    bool cuda;               /* installing/owned a CUDA build */
    char device_note[160];   /* "NVIDIA GeForce RTX 5060 Ti" or "CPU only" */
} rt_info;

/* Probes for an existing runtime; call once before the first frame. */
void rt_init(void);
bool rt_ready(void);
const char *rt_python(void);       /* UTF-8 path of runtime\python\Scripts\python.exe */
const char *rt_root(void);

/* Starts the install on a worker thread. Safe to call when already ready. */
void rt_install(void);
void rt_cancel(void);
void rt_get(rt_info *out);

void rt_shutdown(void);

#endif /* RUNTIME_H */
