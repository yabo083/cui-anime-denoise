/* Background image loader: one worker thread, two bounded rings (pending paths
 * and finished results) behind a single critical section.
 *
 * The lock only ever guards queue bookkeeping. Decode, downscale, thumbnail
 * probing and the has_alpha scan all run with the lock released, so a frame
 * that pops a result never waits for WIC. The UI thread's share of a load is
 * one memcpy of the result struct plus, later, three img_free() calls.
 */
#include "loader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

typedef struct loader_item {
    char *path;   /* _strdup'ed on push, freed after the decode */
    int   id;
} loader_item;

static struct {
    CRITICAL_SECTION   lock;      /* guards everything below except the thread handle */
    CONDITION_VARIABLE cv;        /* work arrived / room freed / stop requested */
    bool     lock_ready;
    bool     inited;
    bool     alive;               /* worker thread exists or is still winding down */
    bool     stop;
    bool     busy;                /* the worker is decoding one item right now */
    HANDLE   thread;
    int      next_id;

    loader_item   todo[LOADER_QUEUE_CAP];
    int           todo_head, todo_count;

    loader_result done[LOADER_QUEUE_CAP];
    int           done_head, done_count;
} g_ld;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void result_release(loader_result *r) {
    img_free(&r->full);
    img_free(&r->display);
    img_free(&r->thumb);
}

/* Centre-crops in place so a 4:3 frame fills a square tile instead of
 * letterboxing into a thin band. Same idea as app.c's thumb_crop_square, but
 * it reports allocation failure instead of silently keeping the wide image. */
static bool crop_square(img_pixels *p) {
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0) return false;

    int side = p->w < p->h ? p->w : p->h;
    if (p->w == side && p->h == side) return true;

    int ox = (p->w - side) / 2, oy = (p->h - side) / 2;
    size_t row = (size_t)side * 4u;
    uint8_t *out = (uint8_t *)malloc(row * (size_t)side);
    if (!out) return false;

    for (int y = 0; y < side; y++) {
        memcpy(out + (size_t)y * row,
               p->rgba + ((size_t)(y + oy) * (size_t)p->w + (size_t)ox) * 4u, row);
    }
    free(p->rgba);
    p->rgba = out;
    p->w = side;
    p->h = side;
    return true;
}

/* Builds an exact LOADER_THUMB_SIDE square. The scaler is asked for the size
 * that puts 128 on the *short* edge, so the centre crop below needs no second
 * resample; only rounding (or a source smaller than 128) takes the exact-size
 * resize at the end. */
static bool build_thumb(const img_pixels *src, img_pixels *out) {
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0) return false;

    int short_side = src->w < src->h ? src->w : src->h;
    int long_side  = src->w > src->h ? src->w : src->h;

    double want = (double)LOADER_THUMB_SIDE * (double)long_side / (double)short_side;
    int max_side = (int)(want + 0.5);
    if (max_side < LOADER_THUMB_SIDE) max_side = LOADER_THUMB_SIDE;

    img_pixels th = {0};
    if (!img_thumbnail(src, max_side, &th)) return false;
    if (!crop_square(&th)) {
        img_free(&th);
        return false;
    }
    if (th.w == LOADER_THUMB_SIDE && th.h == LOADER_THUMB_SIDE) {
        *out = th;
        return true;
    }

    img_pixels exact = {0};
    bool ok = img_resize(&th, LOADER_THUMB_SIDE, LOADER_THUMB_SIDE, &exact);
    img_free(&th);
    if (!ok) return false;
    *out = exact;
    return true;
}

/* Everything expensive lives here; called with no lock held. */
static void load_one(const char *path, int id, loader_result *r) {
    memset(r, 0, sizeof *r);
    r->id = id;
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");

    img_pixels px = {0};
    if (!img_decode_file(r->path, &px)) {
        snprintf(r->error, sizeof r->error, "无法解码该文件（格式不受支持或文件已损坏）");
        log_warn("loader: decode failed: %s", r->path);
        return;
    }

    r->ok = true;
    r->src_w = px.w;
    r->src_h = px.h;
    r->has_alpha = img_has_alpha(&px);
    r->full = px;

    int long_side = px.w > px.h ? px.w : px.h;
    if (long_side > LOADER_DISPLAY_MAX &&
        !img_thumbnail(&px, LOADER_DISPLAY_MAX, &r->display)) {
        log_warn("loader: display downscale failed: %s", r->path);
    }
    if (!build_thumb(&px, &r->thumb)) {
        log_warn("loader: thumbnail failed: %s", r->path);
    }
    log_info("loader: %s %dx%d display %dx%d alpha %d", r->path, r->src_w, r->src_h,
             r->display.w, r->display.h, r->has_alpha ? 1 : 0);
}

/* ------------------------------------------------------------------------- */
/* Worker                                                                     */
/* ------------------------------------------------------------------------- */

static DWORD WINAPI loader_main(void *arg) {
    (void)arg;

    /* WIC's factory is free-threaded, but the decoding helpers still want the
     * thread's COM apartment set up; RPC_E_CHANGED_MODE just means the caller
     * already picked one. */
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    for (;;) {
        loader_item item = {NULL, 0};
        bool abort;

        EnterCriticalSection(&g_ld.lock);
        while (!g_ld.stop &&
               (g_ld.todo_count == 0 || g_ld.done_count >= LOADER_QUEUE_CAP)) {
            SleepConditionVariableCS(&g_ld.cv, &g_ld.lock, INFINITE);
        }
        abort = g_ld.stop;
        if (!abort) {
            item = g_ld.todo[g_ld.todo_head];
            g_ld.todo[g_ld.todo_head].path = NULL;
            g_ld.todo_head = (g_ld.todo_head + 1) % LOADER_QUEUE_CAP;
            g_ld.todo_count--;
            g_ld.busy = true;
        }
        LeaveCriticalSection(&g_ld.lock);
        if (abort) break;

        loader_result res;
        load_one(item.path, item.id, &res); /* no lock held while WIC works */
        free(item.path);

        EnterCriticalSection(&g_ld.lock);
        g_ld.busy = false;
        while (!g_ld.stop && g_ld.done_count >= LOADER_QUEUE_CAP) {
            SleepConditionVariableCS(&g_ld.cv, &g_ld.lock, INFINITE);
        }
        abort = g_ld.stop;
        if (!abort) {
            int slot = (g_ld.done_head + g_ld.done_count) % LOADER_QUEUE_CAP;
            g_ld.done[slot] = res;
            g_ld.done_count++;
        }
        LeaveCriticalSection(&g_ld.lock);

        if (abort) {
            result_release(&res); /* shutting down: nobody will pop this */
            break;
        }
    }

    EnterCriticalSection(&g_ld.lock);
    g_ld.busy = false;
    g_ld.alive = false;
    LeaveCriticalSection(&g_ld.lock);

    if (com) CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void loader_init(void) {
    if (g_ld.inited) return;

    if (!g_ld.lock_ready) {
        InitializeCriticalSection(&g_ld.lock);
        InitializeConditionVariable(&g_ld.cv);
        g_ld.lock_ready = true;
    }

    if (!img_init()) log_warn("loader: img_init failed");

    EnterCriticalSection(&g_ld.lock);
    g_ld.stop = false;
    bool start = !g_ld.alive;
    if (start) g_ld.alive = true;
    LeaveCriticalSection(&g_ld.lock);

    if (start) {
        g_ld.thread = CreateThread(NULL, 0, loader_main, NULL, 0, NULL);
        if (!g_ld.thread) {
            EnterCriticalSection(&g_ld.lock);
            g_ld.alive = false;
            LeaveCriticalSection(&g_ld.lock);
            log_error("loader: CreateThread failed, loading stays synchronous");
            return;
        }
    }
    log_info("loader: decode thread up (queue %d)", LOADER_QUEUE_CAP);
    g_ld.inited = true;
}

void loader_shutdown(void) {
    if (!g_ld.inited) return;

    EnterCriticalSection(&g_ld.lock);
    g_ld.stop = true;
    for (int i = 0; i < g_ld.todo_count; i++) {
        int slot = (g_ld.todo_head + i) % LOADER_QUEUE_CAP;
        free(g_ld.todo[slot].path);
        g_ld.todo[slot].path = NULL;
        g_ld.todo[slot].id = 0;
    }
    g_ld.todo_head = 0;
    g_ld.todo_count = 0;
    LeaveCriticalSection(&g_ld.lock);
    WakeAllConditionVariable(&g_ld.cv);

    bool joined = true;
    if (g_ld.thread) {
        if (WaitForSingleObject(g_ld.thread, 3000) == WAIT_TIMEOUT) {
            /* A stalled decode (dead network share, huge image) is still
             * running. Keep the lock and let it exit on its own: deleting a
             * section a live thread is sleeping in would crash the process. */
            joined = false;
            log_warn("loader: decode thread still busy after 3 s, leaving it to finish");
        }
        CloseHandle(g_ld.thread);
        g_ld.thread = NULL;
    }

    if (joined) {
        EnterCriticalSection(&g_ld.lock);
        for (int i = 0; i < g_ld.done_count; i++) {
            int slot = (g_ld.done_head + i) % LOADER_QUEUE_CAP;
            result_release(&g_ld.done[slot]);
            memset(&g_ld.done[slot], 0, sizeof g_ld.done[slot]);
        }
        g_ld.done_head = 0;
        g_ld.done_count = 0;
        LeaveCriticalSection(&g_ld.lock);

        DeleteCriticalSection(&g_ld.lock);
        g_ld.lock_ready = false;
    }

    g_ld.inited = false;
    log_info("loader: shut down");
}

int loader_push(const char *utf8_path) {
    if (!g_ld.inited || !utf8_path || !*utf8_path) return 0;

    char *copy = _strdup(utf8_path);
    if (!copy) return 0;

    EnterCriticalSection(&g_ld.lock);
    if (g_ld.todo_count >= LOADER_QUEUE_CAP) {
        LeaveCriticalSection(&g_ld.lock);
        free(copy);
        return 0;
    }
    int id = ++g_ld.next_id;
    if (id <= 0) id = g_ld.next_id = 1; /* overflow after 2^31 pushes */
    int slot = (g_ld.todo_head + g_ld.todo_count) % LOADER_QUEUE_CAP;
    g_ld.todo[slot].path = copy;
    g_ld.todo[slot].id = id;
    g_ld.todo_count++;
    LeaveCriticalSection(&g_ld.lock);

    WakeConditionVariable(&g_ld.cv);
    return id;
}

bool loader_pop(loader_result *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!g_ld.inited) return false;

    EnterCriticalSection(&g_ld.lock);
    if (g_ld.done_count == 0) {
        LeaveCriticalSection(&g_ld.lock);
        return false;
    }
    *out = g_ld.done[g_ld.done_head];
    memset(&g_ld.done[g_ld.done_head], 0, sizeof g_ld.done[g_ld.done_head]);
    g_ld.done_head = (g_ld.done_head + 1) % LOADER_QUEUE_CAP;
    g_ld.done_count--;
    LeaveCriticalSection(&g_ld.lock);

    WakeConditionVariable(&g_ld.cv); /* the worker may be waiting for room */
    return true;
}

int loader_pending(void) {
    if (!g_ld.inited) return 0;

    EnterCriticalSection(&g_ld.lock);
    int n = g_ld.todo_count + (g_ld.busy ? 1 : 0);
    LeaveCriticalSection(&g_ld.lock);
    return n;
}
