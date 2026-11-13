/* engine.h -- native denoise engine: ONNX Runtime (DirectML / CPU) super
 * resolution, Pillow-exact BOX/LANCZOS resampling, and the L0 gradient
 * smoothing step from the reference denoise.py.
 *
 * Threading contract: calls are thread-compatible, not thread-safe -- one
 * eng_upscale / eng_denoise at a time per eng. eng_* calls are expected to run
 * on a background thread at THREAD_PRIORITY_BELOW_NORMAL; L0 worker threads
 * spawned internally are below-normal themselves. `cancel` is a caller-owned
 * flag (InterlockedIncrement to request stop); checked per tile / per L0
 * iteration -- a long call returns promptly with ENG result 1.
 */
#ifndef ANIME_ENGINE_H
#define ANIME_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ENG_DEV_AUTO = 0, ENG_DEV_GPU, ENG_DEV_CPU } eng_device;
typedef enum {
    ENG_M_ESRGAN = 0,
    ENG_M_CUGAN,
    ENG_M_APISR,
    ENG_M_NONE   /* L0 only */
} eng_model;

/* Tightly packed RGB8, malloc'ed; free with eng_image_free. */
typedef struct { int w, h; uint8_t *rgb; } eng_image;

typedef struct {
    eng_model model;
    double down;   /* 0.6; ignored for APISR / NONE */
    double l0;     /* 0 = skip */
} eng_params;

typedef struct {
    const wchar_t *runtime_dir;      /* dir holding onnxruntime.dll + DirectML.dll */
    const wchar_t *models_dir;       /* bundled models (esrgan + skeletons) */
    const wchar_t *user_models_dir;  /* downloaded .pth + skeletons, may be NULL */
    eng_device device;
    int threads;                     /* 0 = auto (physical cores) */
} eng_config;

/* stage is "upscale" or "smooth"; value in [0,1] */
typedef void (*eng_progress_fn)(void *ud, const char *stage, float value01);

typedef struct eng eng;  /* one per process is fine */

/* Loads the ORT DLL only; fast, no sessions. err gets a user-facing message. */
eng *eng_create(const eng_config *cfg, char *err, size_t cap);
void eng_destroy(eng *e);

/* Creates/compiles the session for `m` -- slow on DML, call off the UI thread. */
bool eng_prepare(eng *e, eng_model m, char *err, size_t cap);

/* Frees GPU memory held by created sessions (idle cleanup). */
void eng_release_sessions(eng *e);

/* "NVIDIA GeForce RTX 5060 Ti (DirectML)" / "CPU (16 threads)" */
const char *eng_device_name(const eng *e);
bool eng_using_gpu(const eng *e);

/* Non-empty when the GPU was rejected and we fell back to CPU (Chinese text,
 * like denoise.py's GPU_NOTE). */
const char *eng_note(const eng *e);

/* downscale-BOX -> model -> LANCZOS back to src size. For ENG_M_NONE `out_sr`
 * is a copy of src. Returns 0 ok, 1 cancelled, -1 error (err filled). */
int eng_upscale(eng *e, const eng_image *src, const eng_params *p,
                eng_image *out_sr, eng_progress_fn cb, void *ud,
                volatile long *cancel, char *err, size_t cap);

/* L0 gradient smoothing (Xu et al. 2011), the exact denoise.py algorithm in
 * double precision. lam must be finite and >= 0. Returns 0/1/-1. */
int eng_l0(const eng_image *in, double lam, eng_image *out, int threads,
           eng_progress_fn cb, void *ud, volatile long *cancel);

int eng_denoise(eng *e, const eng_image *src, const eng_params *p,
                eng_image *out, eng_progress_fn cb, void *ud,
                volatile long *cancel, char *err, size_t cap);

void eng_image_free(eng_image *im);

/* resample.c -- Pillow-identical 8bpc resampling (BOX / LANCZOS), horizontal
 * pass first, exposed for tests and the comparison sheet. */
#define ENG_BOX 0
#define ENG_LANCZOS 1
bool eng_resize(const eng_image *src, int w, int h, int filter, eng_image *out);

/* models.c -- optional model management (APISR / CUGAN are downloaded by the
 * user, not redistributed). A downloadable model is usable only as a *pair*:
 * the weightless <name>.skel.onnx and its .pth in the SAME directory (the
 * skeleton reads the .pth as ONNX external data relative to its own path).
 * `available` below means such a pair is complete in user_models_dir or
 * models_dir; weights_present/skeleton_present report the halves separately
 * so the UI can tell "not downloaded" from "broken half-state". */
typedef struct {
    bool available;          /* skeleton + matching .pth in one directory */
    bool skeleton_present;
    bool weights_present;
    const char *weights_name;    /* "2x_APISR_RRDB_GAN_generator.pth" */
    const char *url;
    const char *sha256_hex;
    unsigned long long weights_size;
    wchar_t weights_path[520];   /* where the .pth must live */
    char missing[256];           /* human-readable "what is missing" (UTF-8) */
} eng_model_state;

/* Fills `st` for model `m` (only ENG_M_CUGAN / ENG_M_APISR have downloads).
 * Search order: user_models_dir, then models_dir. */
void eng_model_status(const eng_config *cfg, eng_model m, eng_model_state *st);

/* "ESRGAN" / "CUGAN" / "APISR" / "L0" -- short display name for notes. */
const char *eng_model_name(eng_model m);

/* Blocking download of the upstream .pth into cfg->user_models_dir:
 * temp file -> SHA-256 verify -> zip member extraction if needed ->
 * rename -> pair the bundled skeleton from cfg->models_dir next to it.
 * A weights file that is already complete on disk (right size + zip magic,
 * e.g. from a run that was interrupted after the .pth landed) skips the
 * fetch and only completes the pair, so a "download" click also repairs
 * a broken half-state for free. Returns 0 ok, 1 cancelled, -1 error.
 * Progress cb(done,total,ud). */
typedef void (*eng_dl_progress_fn)(void *ud, unsigned long long done,
                                   unsigned long long total);
int eng_model_download(const eng_config *cfg, eng_model m,
                       eng_dl_progress_fn cb, void *ud, volatile long *cancel,
                       char *err, size_t cap);

/* Completes the pair in cfg->user_models_dir: the .pth must already be there
 * (verified downloads land it); the bundled skeleton is copied from
 * cfg->models_dir when missing. Returns false with a clear error when the
 * weights are absent or the bundled skeleton is gone. */
bool eng_model_pair_complete(const eng_config *cfg, eng_model m,
                             char *err, size_t cap);

/* Deletes the downloaded pair (weights + copied skeleton) from
 * cfg->user_models_dir. Returns true when at least one file was removed. */
bool eng_model_delete(const eng_config *cfg, eng_model m);

/* Python round-half-even for the downscaled size (round(W*down)). */
int eng_round_half_even(double x);

#ifdef __cplusplus
}
#endif
#endif /* ANIME_ENGINE_H */
