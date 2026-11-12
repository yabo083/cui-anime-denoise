/* Driver for src/backend.c against tmp/stub_worker.py (or the real worker).
 *
 *   test_backend.exe [python.exe] [worker.py] [venv_dir] [image]
 *
 * Checks: ready -> progress -> done (with a decodable preview) -> bye, plus
 * cancel and abnormal-exit (WB_EV_DEAD) paths.
 */
#include "../src/backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define OUT_DIR "E:\\Codes\\AI\\lab\\cui-anime-denoise\\tmp\\out"

static const char *tname(int t) {
    switch (t) {
    case WB_EV_PROGRESS: return "progress";
    case WB_EV_DONE: return "done";
    case WB_EV_ERROR: return "error";
    case WB_EV_CANCELLED: return "cancelled";
    case WB_EV_READY: return "ready";
    case WB_EV_BYE: return "bye";
    case WB_EV_DEAD: return "dead";
    default: return "?";
    }
}

static unsigned long long now_ms(void) { return GetTickCount64(); }

/* Polls until `want` arrives; prints everything it sees. Returns the event type
 * that stopped the loop, or WB_EV_NONE on timeout. */
static int wait_for(int want, int timeout_ms) {
    unsigned long long t0 = now_ms();
    for (;;) {
        wb_event ev;
        while (wb_poll(&ev)) {
            printf("  ev %-9s id=%d value=%.2f w=%d h=%d ms=%d%s%s\n", tname(ev.type), ev.id,
                   (double)ev.value, ev.w, ev.h, ev.ms, ev.path[0] ? " out=" : "",
                   ev.path[0] ? ev.path : "");
            if (ev.type == WB_EV_DONE) {
                if (ev.preview_b64) {
                    unsigned char *rgba = NULL;
                    int w = 0, h = 0;
                    if (wb_decode_preview(ev.preview_b64, &rgba, &w, &h)) {
                        printf("     preview decoded %dx%d (event says %dx%d), first px %u,%u,%u,%u\n",
                               w, h, ev.w, ev.h, rgba[0], rgba[1], rgba[2], rgba[3]);
                        /* the worker downscales the preview (longest edge <= 1280),
                         * ev.w/ev.h are the full output size */
                        if (w > ev.w || h > ev.h || w <= 0 || h <= 0) {
                            printf("     FAIL preview %dx%d is not a downscale of %dx%d\n", w, h, ev.w, ev.h);
                            failures++;
                        }
                        wb_free_pixels(rgba);
                    } else {
                        printf("     FAIL preview base64/PNG decode\n");
                        failures++;
                    }
                } else {
                    printf("     FAIL done without preview_b64\n");
                    failures++;
                }
            }
            if (ev.type == WB_EV_ERROR) {
                printf("     msg: %s\n", ev.message);
            }
            if (ev.type == WB_EV_READY) {
                printf("     device=%s torch=%s gpu=%s\n", wb_device(), wb_torch(), wb_gpu());
            }
            int hit = ev.type == want;
            wb_event_free(&ev);
            if (hit) return want;
        }
        if (now_ms() - t0 > (unsigned long long)timeout_ms) return WB_EV_NONE;
        Sleep(10);
    }
}

int main(int argc, char **argv) {
    const char *py = argc > 1 ? argv[1] : "E:\\AI\\gpt-anime-denoise\\.venv\\Scripts\\python.exe";
    const char *worker = argc > 2 ? argv[2] : "E:\\Codes\\AI\\lab\\cui-anime-denoise\\tmp\\stub_worker.py";
    const char *venv = argc > 3 ? argv[3] : "E:/AI/gpt-anime-denoise";
    const char *image = argc > 4 ? argv[4] : "E:\\AI\\gpt-anime-denoise\\examples\\furina_before_after.webp";

    setvbuf(stdout, NULL, _IONBF, 0);
    wb_set_start_timeout(argc > 6 ? atoi(argv[6]) : 240000);
    printf("python: %s\nworker: %s\n", py, worker);

    /* 1. ready -> progress -> done -> bye ---------------------------------- */
    if (!wb_start(py, worker, venv, NULL)) {
        printf("FAIL wb_start: %s (log: %s)\n", wb_last_error(), wb_log_path());
        return 1;
    }
    printf("started, alive=%d, log=%s\n", (int)wb_alive(), wb_log_path());
    if (wait_for(WB_EV_READY, 5000) != WB_EV_READY) {
        printf("FAIL no ready event\n");
        failures++;
    }
    const char *out_dir = argc > 8 ? argv[8] : OUT_DIR;
    wb_push_denoise(1, image, 0.6, 0.001, out_dir);
    int last = wait_for(WB_EV_DONE, argc > 7 ? atoi(argv[7]) : 60000);
    if (last != WB_EV_DONE) {
        printf("FAIL no done event (stopped on %s)\n", tname(last));
        failures++;
    }
    wb_shutdown();
    printf("after shutdown alive=%d\n", (int)wb_alive());
    if (wait_for(WB_EV_BYE, 5000) != WB_EV_BYE) {
        printf("FAIL no bye event after shutdown\n");
        failures++;
    }

    if (argc > 5 && strcmp(argv[5], "phase1") == 0) {
        printf("\nRESULT: %s\n", failures ? "FAILURE(S)" : "PASS (phase 1 only)");
        return failures ? 1 : 0;
    }

    /* 2. cancel ------------------------------------------------------------ */
    printf("--- cancel ---\n");
    if (!wb_start(py, worker, venv, NULL)) {
        printf("FAIL restart: %s\n", wb_last_error());
        return 1;
    }
    (void)wait_for(WB_EV_READY, 5000);
    wb_push_denoise(7, image, 0.6, 0.001, out_dir);
    Sleep(120);
    wb_cancel(7);
    if (wait_for(WB_EV_CANCELLED, 20000) != WB_EV_CANCELLED) {
        printf("FAIL no cancelled event\n");
        failures++;
    }
    wb_shutdown();

    /* 3. abnormal exit ----------------------------------------------------- */
    printf("--- kill ---\n");
    if (!wb_start(py, worker, venv, NULL)) {
        printf("FAIL restart: %s\n", wb_last_error());
        return 1;
    }
    (void)wait_for(WB_EV_READY, 5000);
    wb_kill();
    printf("after kill alive=%d\n", (int)wb_alive());
    if (wait_for(WB_EV_DEAD, 5000) != WB_EV_DEAD) {
        printf("FAIL no dead event after kill\n");
        failures++;
    }

    if (failures) printf("\nRESULT: %d FAILURE(S)\n", failures);
    else printf("\nRESULT: PASS\n");
    return failures ? 1 : 0;
}
