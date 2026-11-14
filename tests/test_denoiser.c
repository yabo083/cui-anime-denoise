/* test_denoiser.c -- integration checks for the in-process inference worker
 * (src/denoiser.c): thread lifecycle, READY event, job queueing, monotonic
 * progress, the two-entry SR cache, queued-job cancellation, and shutdown.
 *
 * Needs the real engine: point it at the staged runtime + models first:
 *   set DN_RUNTIME_DIR=<repo>\build\runtime
 *   set DN_MODELS_DIR=<repo>\build\models
 *   set DN_USER_MODELS_DIR=<repo>\tmp\dnbuild\models-user
 * Runs on CPU (ENG_DEV_CPU) so it does not depend on a GPU.
 *
 * Build (from the repo root, after `pwsh -File build.ps1` produced
 * build\anime_engine.lib and build\third_party\CUI\cui.lib):
 *   pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
 *     New-Item -ItemType Directory -Force tmp\dnbuild | Out-Null;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c tests\test_denoiser.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\denoiser.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\denoiser_models.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\update_io.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\update.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\config.c /Fo:tmp\dnbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /c src\log.c /Fo:tmp\dnbuild\;
 *     link /nologo tmp\dnbuild\test_denoiser.obj tmp\dnbuild\denoiser.obj
 *       tmp\dnbuild\denoiser_models.obj
 *       tmp\dnbuild\update_io.obj tmp\dnbuild\update.obj
 *       tmp\dnbuild\config.obj tmp\dnbuild\log.obj
 *       build\anime_engine.lib build\third_party\CUI\cui.lib
 *       dxgi.lib winhttp.lib bcrypt.lib windowscodecs.lib ole32.lib oleaut32.lib
 *       uuid.lib shlwapi.lib shell32.lib advapi32.lib user32.lib gdi32.lib
 *       /OUT:tmp\dnbuild\test_denoiser.exe"
 *   tmp\dnbuild\test_denoiser.exe
 */
#include "denoiser.h"
#include "denoiser_int.h"   /* whitebox: g_dn flags for the lifecycle checks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int fails, checks;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            fails++;                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);             \
        }                                                                    \
    } while (0)

/* Uniform RGBA test image (alpha exercises the strip path in dn_push). */
static void make_rgba(img_pixels *px, int w, int h, int seed) {
    px->w = w;
    px->h = h;
    px->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        px->rgba[i * 4 + 0] = (uint8_t)((i * 37 + seed) & 0xFF);
        px->rgba[i * 4 + 1] = (uint8_t)((i * 17 + seed * 3) & 0xFF);
        px->rgba[i * 4 + 2] = (uint8_t)((i * 7 + seed * 11) & 0xFF);
        px->rgba[i * 4 + 3] = (uint8_t)(128 + (i & 127));
    }
}

/* Drains the event ring until `want` for job `id` arrives; returns the event.
 * Terminal events for other ids are reported through `other` so the caller
 * can assert ordering. Progress is folded into stats (monotonic + count). */
typedef struct {
    int progress_n;
    float last_value;
    int went_back;
    int other_type, other_id;
} dn_watch;

static int wait_for(int want_type, int want_id, dn_event *out, dn_watch *w,
                    DWORD timeout_ms) {
    dn_event ev;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        while (dn_poll(&ev)) {
            if (ev.type == DN_EV_PROGRESS) {
                w->progress_n++;
                if (w->progress_n > 1 && ev.value < w->last_value - 1e-6f)
                    w->went_back = 1;
                w->last_value = ev.value;
                dn_event_free(&ev);
                continue;
            }
            if (ev.type == want_type && (want_id == 0 || ev.id == want_id)) {
                *out = ev;
                return 1;
            }
            if (w && ev.type != DN_EV_READY) {
                w->other_type = ev.type;
                w->other_id = ev.id;
            }
            dn_event_free(&ev);
        }
        if (GetTickCount64() > deadline) return 0;
        Sleep(15);
    }
}

int main(void) {
    dn_event ev;
    dn_watch w;
    img_pixels px;
    int done1_w = 0, done1_h = 0, saw_cached = 0;

    if (!getenv("DN_RUNTIME_DIR") || !getenv("DN_MODELS_DIR")) {
        printf("SKIP: set DN_RUNTIME_DIR / DN_MODELS_DIR to the staged "
               "build\\runtime and build\\models\n");
        return 0;
    }

    /* ---- lifecycle: start -> READY ------------------------------------- */
    CHECK(dn_start(ENG_DEV_CPU), "dn_start returns true");
    CHECK(dn_state_get() == DN_STATE_BOOT, "state is BOOT after start");
    memset(&w, 0, sizeof w);
    CHECK(wait_for(DN_EV_READY, 0, &ev, &w, 90000), "READY arrives within 90 s");
    CHECK(ev.ready_ok == 1, "READY reports success");
    CHECK(ev.device[0] != 0, "READY carries a device name");
    printf("     device: %s\n", ev.device);
    dn_event_free(&ev);
    CHECK(dn_state_get() == DN_STATE_READY, "state is READY");
    CHECK(dn_device_name()[0] != 0, "dn_device_name is set");
    CHECK(dn_using_gpu() == false, "CPU preference does not pick the GPU");

    /* ---- job 1: full pipeline, no cache --------------------------------- */
    make_rgba(&px, 32, 32, 5);
    CHECK(dn_push_denoise(11, 1001, &px, 50, 0.0, ENG_M_ESRGAN), "push job 11");
    memset(&w, 0, sizeof w);
    CHECK(wait_for(DN_EV_DONE, 11, &ev, &w, 60000), "job 11 DONE");
    CHECK(w.progress_n > 0, "job 11 emitted progress");
    CHECK(!w.went_back, "job 11 progress never went backwards");
    CHECK(ev.rgb.rgb && ev.rgb.w == 32 && ev.rgb.h == 32,
          "DONE carries full-res RGB at source size");
    CHECK(ev.sr_cached == false, "first run is not a cache hit");
    CHECK(ev.model == ENG_M_ESRGAN, "DONE reports the model that ran");
    CHECK(ev.message[0] == 0, "bundled model needs no fallback note");
    done1_w = ev.rgb.w; done1_h = ev.rgb.h;
    dn_event_free(&ev);

    /* ---- job 2: same src_key+down_pct, different l0 -> SR cache hit ----- */
    CHECK(dn_push_denoise(12, 1001, &px, 50, 0.0004, ENG_M_ESRGAN),
          "push job 12");
    memset(&w, 0, sizeof w);
    CHECK(wait_for(DN_EV_DONE, 12, &ev, &w, 60000), "job 12 DONE");
    CHECK(ev.sr_cached == true, "same src_key+down reuses the SR pass");
    saw_cached = ev.sr_cached;
    CHECK(ev.rgb.w == done1_w && ev.rgb.h == done1_h,
          "cached run returns same dimensions");
    dn_event_free(&ev);
    (void)saw_cached;

    /* ---- queueing + queued-job cancellation ----------------------------- */
    {
        img_pixels big;
        int got3 = 0, got4 = 0, type3 = 0, type4 = 0;
        make_rgba(&big, 96, 96, 9);
        CHECK(dn_push_denoise(13, 2002, &big, 100, 0.0, ENG_M_ESRGAN),
              "push job 13 (running)");
        CHECK(dn_push_denoise(14, 3003, &big, 100, 0.0, ENG_M_ESRGAN),
              "push job 14 (queued)");
        dn_cancel(14);
        /* job 13 runs to DONE or gets cancelled by shutdown; job 14 must be
         * CANCELLED -- its flag was set before the thread ever saw it */
        {
            ULONGLONG deadline = GetTickCount64() + 120000;
            while ((!got3 || !got4) && GetTickCount64() < deadline) {
                dn_event e2;
                while (dn_poll(&e2)) {
                    if (e2.id == 13 && e2.type != DN_EV_PROGRESS) {
                        got3 = 1; type3 = e2.type;
                    }
                    if (e2.id == 14 && e2.type != DN_EV_PROGRESS) {
                        got4 = 1; type4 = e2.type;
                    }
                    dn_event_free(&e2);
                }
                Sleep(15);
            }
        }
        CHECK(got3, "job 13 produced a terminal event");
        CHECK(got4, "job 14 produced a terminal event");
        CHECK(type4 == DN_EV_CANCELLED, "queued job cancels without running");
        free(big.rgba);
    }

    /* ---- model selection: unavailable picks fall back to the bundled ----- */
    /* DN_USER_MODELS_DIR points at an empty dir, so CUGAN is not a complete
     * pair here: the resolver must answer ESRGAN, and a job pushed on CUGAN
     * must come back DONE on ESRGAN with the fallback note -- never a
     * silent wrong-model run. */
    CHECK(dn_resolve_model(ENG_M_CUGAN) == ENG_M_ESRGAN,
          "undownloaded model resolves to the bundled one");
    CHECK(dn_resolve_model(ENG_M_ESRGAN) == ENG_M_ESRGAN,
          "bundled model resolves to itself");
    CHECK(dn_push_denoise(15, 1001, &px, 50, 0.0, ENG_M_CUGAN),
          "push job 15 on an unavailable model");
    memset(&w, 0, sizeof w);
    CHECK(wait_for(DN_EV_DONE, 15, &ev, &w, 60000), "job 15 DONE");
    CHECK(ev.model == ENG_M_ESRGAN, "job 15 ran on the bundled fallback");
    CHECK(ev.message[0] != 0, "job 15 DONE carries the fallback note");
    dn_event_free(&ev);

    /* ---- model status is a synchronous file check ------------------------ */
    {
        eng_model_state ms;
        memset(&ms, 0, sizeof ms);
        CHECK(dn_model_status(ENG_M_ESRGAN, &ms), "model_status runs");
        CHECK(ms.available, "bundled ESRGAN reports available");
        memset(&ms, 0, sizeof ms);
        dn_model_status(ENG_M_CUGAN, &ms);
        CHECK(!ms.available, "absent CUGAN pair reports unavailable");
    }

    /* ---- shutdown is prompt and leaves state IDLE ------------------------ */
    dn_shutdown();
    CHECK(dn_state_get() == DN_STATE_IDLE, "state is IDLE after shutdown");
    CHECK(dn_poll(&ev) == 0, "event ring is drained on shutdown");

    /* ---- dn_start discriminates healthy vs dying workers ----------------- */
    /* A join that times out orphans the thread holding g_dn (alive=1,
     * stop=1) until its epilogue lands; dn_start must refuse that state
     * honestly instead of answering "already up" (a false READY wait was
     * the 60 s phantom-timeout bug). The thread is gone for real now, so
     * poking the flags simulates the orphan window safely. */
    {
        dn__lock();
        g_dn.alive = 1;
        g_dn.stop = 1;
        dn__unlock();
        CHECK(!dn_start(ENG_DEV_CPU),
              "dying worker (alive+stop): dn_start refuses");
        CHECK(dn_last_error()[0] != 0,
              "dying worker: last_error explains the refusal");
        dn__lock();
        g_dn.stop = 0;
        dn__unlock();
        CHECK(dn_start(ENG_DEV_CPU),
              "healthy worker (alive, !stop): dn_start reports already-up");
        dn__lock();
        g_dn.alive = 0;
        dn__unlock();
    }

    free(px.rgba);
    printf("%s: %d checks, %d failures\n",
           fails ? "FAIL" : "OK", checks, fails);
    return fails ? 1 : 0;
}
