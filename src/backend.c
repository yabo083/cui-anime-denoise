#include "backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

/* Previews and composited results come back as base64 PNG; img.c decodes them
 * through WIC, so this file needs no image decoder of its own. */

#define WB_QUEUE_CAP 256
#define WB_START_TIMEOUT_MS 180000
#define WB_STOP_TIMEOUT_MS 10000
#define WB_LINE_LIMIT (64u * 1024u * 1024u)

/* ------------------------------------------------------------------ state */

typedef struct {
    CRITICAL_SECTION lock;  /* queue + ready flags + boot state */
    CRITICAL_SECTION wlock; /* stdin writes */
    bool inited;

    wb_event q[WB_QUEUE_CAP];
    int qhead, qcount;

    HANDLE proc;
    DWORD pid;
    HANDLE in;  /* write end of the child's stdin */
    HANDLE out; /* read end of the child's stdout */
    HANDLE thread;

    volatile LONG dead;
    volatile LONG ready;    /* set when the worker signalled ready (any thread may pop the event) */
    volatile LONG failed;   /* worker-level failure before/at ready */
    volatile LONG stopping; /* shutdown requested: do not report WB_EV_DEAD */
    bool started;
    wb_state state;         /* boot progress (guarded by lock) */

    char device[32];
    char torch[64];
    char gpu[160];
    char models_dir[1024]; /* UTF-8, where the ESRGAN weights live */
    bool model_ready;
    char last_error[512];
    char log_path[1024]; /* UTF-8 */
} wb_ctx;

static wb_ctx g;
static int g_start_timeout_ms = WB_START_TIMEOUT_MS;

/* ------------------------------------------------------------ byte buffer */

typedef struct {
    char *p;
    size_t len, cap;
} wb_buf;

static bool wb__buf_reserve(wb_buf *b, size_t extra) {
    if (b->p && b->len + extra + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra + 1) {
        if (cap > (size_t)1 << 40) return false;
        cap *= 2;
    }
    char *np = (char *)realloc(b->p, cap);
    if (!np) return false;
    b->p = np;
    b->cap = cap;
    return true;
}

static void wb__buf_put(wb_buf *b, const char *s, size_t n) {
    if (!wb__buf_reserve(b, n)) return;
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void wb__buf_puts(wb_buf *b, const char *s) { wb__buf_put(b, s, strlen(s)); }

static void wb__buf_putc(wb_buf *b, char c) { wb__buf_put(b, &c, 1); }

static void wb__buf_free(wb_buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* --------------------------------------------------------- UTF-8 helpers */

static wchar_t *wb__u8_to_w(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static void wb__w_to_u8(const wchar_t *w, char *dst, size_t cap) {
    if (dst && cap) dst[0] = 0;
    if (!w || !dst || cap < 2) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, (int)cap, NULL, NULL);
    if (n <= 0) dst[0] = 0;
}

/* ----------------------------------------------------------- JSON output */

static void wb__json_escape(wb_buf *b, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': wb__buf_puts(b, "\\\""); break;
        case '\\': wb__buf_puts(b, "\\\\"); break;
        case '\n': wb__buf_puts(b, "\\n"); break;
        case '\r': wb__buf_puts(b, "\\r"); break;
        case '\t': wb__buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)*p);
                wb__buf_puts(b, tmp);
            } else {
                wb__buf_putc(b, (char)*p);
            }
        }
    }
}

/* ------------------------------------------------------------ JSON input */

static const char *wb__ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* p at the opening quote; returns the char after the closing quote and stores
 * the raw (still escaped) content range in *content / *len. */
static const char *wb__scan_string(const char *p, const char **content, size_t *len) {
    if (*p != '"') return NULL;
    const char *start = ++p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    if (*p != '"') return NULL;
    if (content) *content = start;
    if (len) *len = (size_t)(p - start);
    return p + 1;
}

static bool wb__skip_value(const char **pp) {
    const char *p = wb__ws(*pp);
    if (*p == '"') {
        p = wb__scan_string(p, NULL, NULL);
        if (!p) return false;
    } else if (*p == '{' || *p == '[') {
        int depth = 0;
        for (;;) {
            if (*p == '"') {
                p = wb__scan_string(p, NULL, NULL);
                if (!p) return false;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            } else if (!*p) {
                return false;
            }
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
    }
    *pp = p;
    return true;
}

/* Looks up a top-level key of a flat object; returns a pointer to its value. */
static const char *wb__value_of(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = wb__ws(json);
    if (*p == '{') p++;
    for (;;) {
        p = wb__ws(p);
        if (*p != '"') return NULL;
        const char *kraw = NULL;
        size_t kn = 0;
        p = wb__scan_string(p, &kraw, &kn);
        if (!p) return NULL;
        p = wb__ws(p);
        if (*p != ':') return NULL;
        p = wb__ws(p + 1);
        if (kn == klen && memcmp(kraw, key, klen) == 0) return p;
        if (!wb__skip_value(&p)) return NULL;
        p = wb__ws(p);
        if (*p != ',') return NULL;
        p++;
    }
}

static int wb__hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned wb__hex4(const char *s) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = wb__hexval(s[i]);
        if (h < 0) return 0;
        v = (v << 4) | (unsigned)h;
    }
    return v;
}

static void wb__utf8_put(wb_buf *b, unsigned cp) {
    char t[4];
    size_t n;
    if (cp < 0x80) {
        t[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    wb__buf_put(b, t, n);
}

/* Decodes the escapes of a raw JSON string body into UTF-8 (incl. \uXXXX
 * with surrogate pairs, which is what json.dumps emits by default). */
static void wb__unescape(wb_buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= len) {
            wb__buf_putc(b, c);
            continue;
        }
        char e = s[++i];
        switch (e) {
        case 'n': wb__buf_putc(b, '\n'); break;
        case 'r': wb__buf_putc(b, '\r'); break;
        case 't': wb__buf_putc(b, '\t'); break;
        case 'b': wb__buf_putc(b, '\b'); break;
        case 'f': wb__buf_putc(b, '\f'); break;
        case '/': wb__buf_putc(b, '/'); break;
        case '"': wb__buf_putc(b, '"'); break;
        case '\\': wb__buf_putc(b, '\\'); break;
        case 'u':
            if (i + 4 < len) {
                unsigned cp = wb__hex4(s + i + 1);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len && s[i + 1] == '\\' &&
                    s[i + 2] == 'u') {
                    unsigned lo = wb__hex4(s + i + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                wb__utf8_put(b, cp);
            }
            break;
        default: wb__buf_putc(b, e); break;
        }
    }
}

static bool wb__json_raw(const char *json, const char *key, const char **content, size_t *len) {
    const char *v = wb__value_of(json, key);
    if (!v) return false;
    const char *c = NULL;
    size_t n = 0;
    if (!wb__scan_string(v, &c, &n)) return false;
    *content = c;
    *len = n;
    return true;
}

static bool wb__json_str_into(const char *json, const char *key, char *dst, size_t cap) {
    const char *content = NULL;
    size_t len = 0;
    if (!wb__json_raw(json, key, &content, &len)) return false;
    if (!dst || cap == 0) return true;
    wb_buf b = {0};
    wb__unescape(&b, content, len);
    size_t n = b.len < cap - 1 ? b.len : cap - 1;
    if (n && b.p) memcpy(dst, b.p, n);
    dst[n] = 0;
    wb__buf_free(&b);
    return true;
}

static char *wb__json_alloc_str(const char *json, const char *key) {
    const char *content = NULL;
    size_t len = 0;
    if (!wb__json_raw(json, key, &content, &len)) return NULL;
    wb_buf b = {0};
    wb__unescape(&b, content, len);
    if (!b.p) {
        b.p = (char *)malloc(1);
        if (b.p) b.p[0] = 0;
    }
    return b.p;
}

/* Locale-independent number parse (strtod would follow the CRT locale). */
static bool wb__parse_number(const char *p, double *out) {
    int neg = 0;
    int digits = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9') return false;
    double v = 0.0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + (double)(*p - '0');
        p++;
        digits++;
    }
    if (*p == '.') {
        double f = 0.1;
        p++;
        while (*p >= '0' && *p <= '9') {
            v += (double)(*p - '0') * f;
            f *= 0.1;
            p++;
            digits++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        int eneg = 0, e = 0;
        if (*p == '-') {
            eneg = 1;
            p++;
        } else if (*p == '+') {
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            if (e < 400) e = e * 10 + (*p - '0');
            p++;
        }
        while (e-- > 0) v = eneg ? v / 10.0 : v * 10.0;
    }
    if (digits == 0) return false;
    *out = neg ? -v : v;
    return true;
}

static bool wb__json_number(const char *json, const char *key, double *out) {
    const char *v = wb__value_of(json, key);
    if (!v) return false;
    return wb__parse_number(v, out);
}

static bool wb__json_int(const char *json, const char *key, int *out) {
    double d = 0;
    if (!wb__json_number(json, key, &d)) return false;
    if (d > 2147483647.0) d = 2147483647.0;
    if (d < -2147483648.0) d = -2147483648.0;
    *out = (int)d;
    return true;
}

static bool wb__json_bool(const char *json, const char *key) {
    const char *p = wb__value_of(json, key);
    return p && strncmp(p, "true", 4) == 0;
}

/* -------------------------------------------------------------- event queue */

static void wb__set_error(const char *msg) {
    if (!msg) msg = "";
    snprintf(g.last_error, sizeof g.last_error, "%s", msg);
}

/* Boot state transitions. Every write happens under g.lock, like the queue. */
static void wb__set_state(wb_state s) {
    EnterCriticalSection(&g.lock);
    g.state = s;
    LeaveCriticalSection(&g.lock);
}

/* A boot event moves SPAWNING -> BOOTING; it never walks a live worker back. */
static void wb__note_boot(void) {
    EnterCriticalSection(&g.lock);
    if (g.state == WB_STATE_SPAWNING) g.state = WB_STATE_BOOTING;
    LeaveCriticalSection(&g.lock);
}

static void wb__push(wb_event *ev) {
    EnterCriticalSection(&g.lock);
    if (g.qcount == WB_QUEUE_CAP) {
        free(g.q[g.qhead].preview_b64);
        memset(&g.q[g.qhead], 0, sizeof g.q[g.qhead]);
        g.qhead = (g.qhead + 1) % WB_QUEUE_CAP;
        g.qcount--;
    }
    int idx = (g.qhead + g.qcount) % WB_QUEUE_CAP;
    g.q[idx] = *ev;
    g.qcount++;
    LeaveCriticalSection(&g.lock);
}

/* Drops everything queued (events of a previous worker are meaningless). */
static void wb__purge(void) {
    EnterCriticalSection(&g.lock);
    while (g.qcount) {
        free(g.q[g.qhead].preview_b64);
        memset(&g.q[g.qhead], 0, sizeof g.q[g.qhead]);
        g.qhead = (g.qhead + 1) % WB_QUEUE_CAP;
        g.qcount--;
    }
    g.qhead = 0;
    LeaveCriticalSection(&g.lock);
}

/* ------------------------------------------------------------ line dispatch */

static void wb__handle_line(const char *json) {
    if (!json || !*json) return;
    char typebuf[32];
    if (!wb__json_str_into(json, "type", typebuf, sizeof typebuf)) {
        /* not protocol output: keep it as the last error for diagnostics */
        wb__set_error(json);
        return;
    }
    wb_event ev;
    memset(&ev, 0, sizeof ev);
    wb__json_int(json, "id", &ev.id);

    if (strcmp(typebuf, "progress") == 0) {
        double v = 0.0;
        wb__json_number(json, "value", &v);
        ev.type = WB_EV_PROGRESS;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        ev.value = (float)v;
        wb__json_str_into(json, "stage", ev.stage, sizeof ev.stage);
    } else if (strcmp(typebuf, "boot") == 0) {
        double v = 0.0;
        wb__json_number(json, "value", &v);
        ev.type = WB_EV_BOOT;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        ev.value = (float)v;
        wb__json_str_into(json, "stage", ev.message, sizeof ev.message);
        wb__note_boot();
    } else if (strcmp(typebuf, "ready") == 0) {
        ev.type = WB_EV_READY;
        EnterCriticalSection(&g.lock);
        wb__json_str_into(json, "device", g.device, sizeof g.device);
        wb__json_str_into(json, "torch", g.torch, sizeof g.torch);
        wb__json_str_into(json, "gpu", g.gpu, sizeof g.gpu);
        wb__json_str_into(json, "models_dir", g.models_dir, sizeof g.models_dir);
        g.model_ready = wb__json_bool(json, "model_ready");
        g.state = WB_STATE_READY;
        ev.model_ready = g.model_ready;
        snprintf(ev.message, sizeof ev.message, "%s", g.gpu[0] ? g.gpu : g.device);
        LeaveCriticalSection(&g.lock);
        InterlockedExchange(&g.ready, 1);
    } else if (strcmp(typebuf, "done") == 0) {
        ev.type = WB_EV_DONE;
        wb__json_str_into(json, "out", ev.path, sizeof ev.path);
        wb__json_int(json, "w", &ev.w);
        wb__json_int(json, "h", &ev.h);
        wb__json_int(json, "ms", &ev.ms);
        ev.sr_cached = wb__json_bool(json, "sr_cached");
        ev.preview_b64 = wb__json_alloc_str(json, "preview_b64");
    } else if (strcmp(typebuf, "composited") == 0) {
        ev.type = WB_EV_COMPOSITED;
        wb__json_str_into(json, "out", ev.path, sizeof ev.path);
        ev.preview_b64 = wb__json_alloc_str(json, "preview_b64");
    } else if (strcmp(typebuf, "models") == 0) {
        double b = 0.0;
        wb__json_number(json, "bytes", &b);
        ev.type = WB_EV_MODELS;
        ev.bytes = (unsigned long long)(b < 0.0 ? 0.0 : b);
        ev.model_ready = wb__json_bool(json, "ready");
        wb__json_str_into(json, "path", ev.message, sizeof ev.message);
    } else if (strcmp(typebuf, "error") == 0) {
        ev.type = WB_EV_ERROR;
        wb__json_str_into(json, "message", ev.message, sizeof ev.message);
        if (!ev.message[0]) wb__set_error("worker reported an unspecified error");
        else wb__set_error(ev.message);
        if (ev.id == 0) InterlockedExchange(&g.failed, 1);
    } else if (strcmp(typebuf, "cancelled") == 0 || strcmp(typebuf, "canceled") == 0) {
        ev.type = WB_EV_CANCELLED;
    } else if (strcmp(typebuf, "bye") == 0) {
        ev.type = WB_EV_BYE;
    } else {
        return;
    }
    wb__push(&ev);
}

/* ----------------------------------------------------------- reader thread */

static DWORD WINAPI wb__reader_thread(LPVOID param) {
    (void)param;
    wb_buf line = {0};
    char chunk[16384];
    bool overflow = false;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(g.out, chunk, (DWORD)sizeof chunk, &got, NULL) || got == 0) break;
        size_t start = 0;
        for (DWORD i = 0; i < got; i++) {
            if (chunk[i] != '\n') continue;
            if (!overflow) {
                wb__buf_put(&line, chunk + start, (size_t)i - start);
                size_t n = line.len;
                if (n && line.p[n - 1] == '\r') n--;
                if (n) wb__handle_line(line.p);
            }
            overflow = false;
            line.len = 0;
            if (line.p) line.p[0] = 0;
            start = (size_t)i + 1;
        }
        if (!overflow) {
            wb__buf_put(&line, chunk + start, (size_t)got - start);
            if (line.len > WB_LINE_LIMIT) {
                overflow = true;
                line.len = 0;
                if (line.p) line.p[0] = 0;
            }
        }
    }
    wb__buf_free(&line);

    InterlockedExchange(&g.failed, 1);
    if (!g.stopping) {
        wb_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = WB_EV_DEAD;
        snprintf(ev.message, sizeof ev.message, "worker exited");
        wb__push(&ev);
        wb__set_state(WB_STATE_DEAD);
    }
    InterlockedExchange(&g.dead, 1);
    return 0;
}

/* -------------------------------------------------------------- transport */

static bool wb__send(const char *json, size_t len) {
    HANDLE in;
    if (!g.inited) return false;
    EnterCriticalSection(&g.lock);
    in = g.in;
    LeaveCriticalSection(&g.lock);
    if (!in) return false;

    EnterCriticalSection(&g.wlock);
    bool ok = true;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off > (size_t)1 << 20 ? (size_t)1 << 20 : len - off;
        DWORD wrote = 0;
        if (!WriteFile(in, json + off, (DWORD)chunk, &wrote, NULL) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    /* No FlushFileBuffers: on a pipe it waits until the child has read the bytes
     * (measured 49-61 ms for the init line), which is exactly the block
     * wb_start_async must not have. WriteFile hands the data to the pipe; a
     * payload larger than the pipe buffer blocks on the reader draining it,
     * which is the backpressure we want anyway. */
    LeaveCriticalSection(&g.wlock);
    if (!ok) wb__set_error("failed to write to the worker");
    return ok;
}

static bool wb__pipe(bool parent_reads, HANDLE *child_end, HANDLE *parent_end) {
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    sa.nLength = (DWORD)sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 1u << 20)) return false;
    if (parent_reads) {
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        *parent_end = rd;
        *child_end = wr;
    } else {
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        *parent_end = wr;
        *child_end = rd;
    }
    return true;
}

/* Kills the child if needed and releases every handle. Queued events survive. */
static void wb__teardown(void) {
    HANDLE in, proc, thread, out;

    EnterCriticalSection(&g.lock);
    in = g.in;
    g.in = NULL;
    proc = g.proc;
    g.proc = NULL;
    thread = g.thread;
    g.thread = NULL;
    out = g.out;
    g.out = NULL;
    bool was_started = g.started;
    g.started = false;
    LeaveCriticalSection(&g.lock);

    if (!was_started && !proc && !thread) {
        if (in) CloseHandle(in);
        if (out) CloseHandle(out);
        return;
    }

    if (in) CloseHandle(in); /* child sees EOF on stdin and should quit */
    if (proc) {
        if (WaitForSingleObject(proc, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(proc, 0);
            WaitForSingleObject(proc, 2000);
        }
        CloseHandle(proc);
    }
    if (thread) {
        if (WaitForSingleObject(thread, 3000) == WAIT_TIMEOUT) {
            CancelSynchronousIo(thread);
            WaitForSingleObject(thread, 1000);
        }
        CloseHandle(thread);
    }
    if (out) CloseHandle(out);
}

/* ------------------------------------------------------------------- API */

bool wb_start_async(const char *python_exe, const char *worker_py, const char *venv_dir,
                    const char *models_dir) {
    wchar_t *wpy = NULL, *wscript = NULL, tmp[MAX_PATH], logw[MAX_PATH + 64];
    wchar_t *cmd = NULL, *full_py = NULL, *full_script = NULL;
    HANDLE in_child = NULL, out_child = NULL, err_child = NULL;
    HANDLE in_parent = NULL, out_parent = NULL;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attr_size = 0;
    bool ok = false;

    if (!python_exe || !worker_py) {
        wb__set_error("wb_start_async: python_exe and worker_py are required");
        return false;
    }
    if (!g.inited) {
        InitializeCriticalSection(&g.lock);
        InitializeCriticalSection(&g.wlock);
        g.inited = true;
    }
    bool already;
    EnterCriticalSection(&g.lock);
    already = g.started;
    if (!already) {
        g.started = true;
        g.dead = 0;
        g.ready = 0;
        g.failed = 0;
        g.stopping = 0;
        g.state = WB_STATE_SPAWNING;
        g.last_error[0] = 0;
        g.device[0] = g.torch[0] = g.gpu[0] = 0;
    }
    LeaveCriticalSection(&g.lock);
    if (already) return true;
    wb__purge();

    wpy = wb__u8_to_w(python_exe);
    wscript = wb__u8_to_w(worker_py);
    if (!wpy || !wscript) {
        wb__set_error("wb_start_async: cannot convert paths to UTF-16");
        goto done;
    }

    /* absolute interpreter/script paths, so the inherited cwd cannot break us */
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(wpy, (DWORD)(sizeof buf / sizeof buf[0]), buf, NULL);
        if (n > 0 && n < sizeof buf / sizeof buf[0]) {
            size_t bytes = (wcslen(buf) + 1) * sizeof(wchar_t);
            full_py = (wchar_t *)malloc(bytes);
            if (full_py) memcpy(full_py, buf, bytes);
        }
        n = GetFullPathNameW(wscript, (DWORD)(sizeof buf / sizeof buf[0]), buf, NULL);
        if (n > 0 && n < sizeof buf / sizeof buf[0]) {
            size_t bytes = (wcslen(buf) + 1) * sizeof(wchar_t);
            full_script = (wchar_t *)malloc(bytes);
            if (full_script) memcpy(full_script, buf, bytes);
        }
    }

    if (!wb__pipe(true, &out_child, &out_parent)) {
        wb__set_error("wb_start_async: CreatePipe(stdout) failed");
        goto done;
    }
    if (!wb__pipe(false, &in_child, &in_parent)) {
        wb__set_error("wb_start_async: CreatePipe(stdin) failed");
        goto done;
    }

    /* child's stderr -> %TEMP%\cui_anime_denoise_worker.log */
    {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = (DWORD)sizeof sa;
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        logw[0] = 0;
        DWORD tn = GetTempPathW((DWORD)(sizeof tmp / sizeof tmp[0]), tmp);
        if (tn > 0 && tn < sizeof tmp / sizeof tmp[0])
            _snwprintf_s(logw, sizeof logw / sizeof logw[0], _TRUNCATE,
                         L"%ls%ls", tmp, L"cui_anime_denoise_worker.log");
        if (logw[0]) {
            HANDLE lf = CreateFileW(logw, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (lf != INVALID_HANDLE_VALUE) {
                err_child = lf;
                wb__w_to_u8(logw, g.log_path, sizeof g.log_path);
            }
        }
        if (!err_child) {
            err_child = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (!err_child) {
            wb__set_error("wb_start_async: cannot open a stderr sink");
            goto done;
        }
    }

    size_t cmd_cap = (wcslen(full_py ? full_py : wpy) + wcslen(full_script ? full_script : wscript)) * 2 + 64;
    cmd = (wchar_t *)malloc(cmd_cap * sizeof(wchar_t));
    if (!cmd) {
        wb__set_error("wb_start_async: out of memory");
        goto done;
    }
    _snwprintf_s(cmd, cmd_cap, _TRUNCATE, L"\"%ls\" -u \"%ls\"", full_py ? full_py : wpy,
                 full_script ? full_script : wscript);

    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = (DWORD)sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_child;
    si.StartupInfo.hStdOutput = out_child;
    si.StartupInfo.hStdError = err_child;
    ZeroMemory(&pi, sizeof pi);

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        wb__set_error("wb_start_async: cannot build the handle inheritance list");
        goto done;
    }
    {
        HANDLE inherit[3];
        inherit[0] = in_child;
        inherit[1] = out_child;
        inherit[2] = err_child;
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                       sizeof inherit, NULL, NULL)) {
            wb__set_error("wb_start_async: UpdateProcThreadAttribute failed");
            goto done;
        }
    }
    si.lpAttributeList = attrs;

    if (!CreateProcessW(full_py ? full_py : wpy, cmd, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        char msg[256];
        snprintf(msg, sizeof msg, "wb_start_async: CreateProcess failed (error %lu)", GetLastError());
        wb__set_error(msg);
        goto done;
    }

    EnterCriticalSection(&g.lock);
    g.proc = pi.hProcess;
    g.pid = pi.dwProcessId;
    g.in = in_parent;
    g.out = out_parent;
    in_parent = NULL;
    out_parent = NULL;
    g.thread = CreateThread(NULL, 0, wb__reader_thread, NULL, 0, NULL);
    LeaveCriticalSection(&g.lock);

    /* the parent must not hold the child's ends open */
    CloseHandle(in_child);
    in_child = NULL;
    CloseHandle(out_child);
    out_child = NULL;
    CloseHandle(err_child); /* the child owns its inherited copy now */
    err_child = NULL;

    if (!g.thread) {
        wb__set_error("wb_start_async: CreateThread failed");
        goto done;
    }

    {
        wb_buf b = {0};
        wb__buf_puts(&b, "{\"cmd\":\"init\",\"venv_dir\":\"");
        wb__json_escape(&b, venv_dir ? venv_dir : "");
        wb__buf_puts(&b, "\",\"device\":\"auto\",\"models_dir\":");
        if (models_dir) {
            wb__buf_putc(&b, '"');
            wb__json_escape(&b, models_dir);
            wb__buf_putc(&b, '"');
        } else {
            wb__buf_puts(&b, "null");
        }
        wb__buf_puts(&b, "}\n");
        bool sent = b.p && wb__send(b.p, b.len);
        wb__buf_free(&b);
        if (!sent) {
            wb__set_error("wb_start_async: cannot send init");
            goto done;
        }
    }

    /* No wait here: ready arrives on the reader thread and wb_poll()/wb_state_get()
     * observe it. Blocking here is what froze the UI (LESSONS L-002). */
    ok = true;

done:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    if (in_child) CloseHandle(in_child);
    if (out_child) CloseHandle(out_child);
    if (err_child) CloseHandle(err_child);
    if (in_parent) CloseHandle(in_parent);
    if (out_parent) CloseHandle(out_parent);
    free(cmd);
    free(full_py);
    free(full_script);
    free(wpy);
    free(wscript);
    if (!ok) {
        InterlockedExchange(&g.stopping, 1);
        wb__teardown();
        wb__set_state(WB_STATE_IDLE);
    }
    return ok;
}

/* Waits for the worker's ready flag. Readiness is a flag rather than a queue peek
 * so a concurrent wb_poll() cannot steal the event from under us; the event itself
 * stays queued for the caller. */
static bool wb__wait_ready(void) {
    ULONGLONG t0 = GetTickCount64();
    bool ok = false;
    for (;;) {
        if (g.ready) {
            ok = true;
            break;
        }
        if (g.failed) {
            char msg[512];
            const char *why = g.last_error[0] ? g.last_error : "worker exited before signalling ready";
            snprintf(msg, sizeof msg, "worker not ready: %s", why);
            wb__set_error(msg);
            break;
        }
        if (!wb_alive()) {
            wb__set_error("worker exited before signalling ready");
            break;
        }
        if (GetTickCount64() - t0 > (ULONGLONG)g_start_timeout_ms) {
            char msg[128];
            snprintf(msg, sizeof msg, "worker did not become ready within %d s",
                     g_start_timeout_ms / 1000);
            wb__set_error(msg);
            break;
        }
        Sleep(20);
    }
    if (!ok) {
        InterlockedExchange(&g.stopping, 1);
        wb__teardown();
        wb__set_state(WB_STATE_IDLE);
    }
    return ok;
}

bool wb_start(const char *python_exe, const char *worker_py, const char *venv_dir,
              const char *models_dir) {
    if (!wb_start_async(python_exe, worker_py, venv_dir, models_dir)) return false;
    return wb__wait_ready();
}

wb_state wb_state_get(void) {
    wb_state s;
    if (!g.inited) return WB_STATE_IDLE;
    EnterCriticalSection(&g.lock);
    s = g.state;
    LeaveCriticalSection(&g.lock);
    return s;
}

void wb_set_start_timeout(int ms) {
    if (ms > 0) g_start_timeout_ms = ms;
}

void wb_push_denoise(int id, const char *path, int down_pct, double l0, const char *out_path) {
    wb_buf b = {0};
    char num[64];
    wb__buf_puts(&b, "{\"cmd\":\"denoise\",\"id\":");
    snprintf(num, sizeof num, "%d", id);
    wb__buf_puts(&b, num);
    wb__buf_puts(&b, ",\"path\":\"");
    wb__json_escape(&b, path ? path : "");
    wb__buf_puts(&b, "\",\"down_pct\":");
    snprintf(num, sizeof num, "%d", down_pct);
    wb__buf_puts(&b, num);
    wb__buf_puts(&b, ",\"l0\":");
    snprintf(num, sizeof num, "%.6g", l0);
    wb__buf_puts(&b, num);
    wb__buf_puts(&b, ",\"out\":\"");
    wb__json_escape(&b, out_path ? out_path : "");
    wb__buf_puts(&b, "\"}\n");
    if (b.p) wb__send(b.p, b.len);
    wb__buf_free(&b);
}

void wb_push_composite(int id, const char *mask_png_b64, const char *out_path) {
    wb_buf b = {0};
    char num[64];
    wb__buf_puts(&b, "{\"cmd\":\"composite\",\"id\":");
    snprintf(num, sizeof num, "%d", id);
    wb__buf_puts(&b, num);
    wb__buf_puts(&b, ",\"mask_b64\":\"");
    wb__buf_puts(&b, mask_png_b64 ? mask_png_b64 : "");
    wb__buf_puts(&b, "\",\"out\":\"");
    wb__json_escape(&b, out_path ? out_path : "");
    wb__buf_puts(&b, "\"}\n");
    if (b.p) wb__send(b.p, b.len);
    wb__buf_free(&b);
}

void wb_models(const char *action) {
    if (!g.inited) return;
    wb_buf b = {0};
    wb__buf_puts(&b, "{\"cmd\":\"models\",\"action\":\"");
    wb__json_escape(&b, action && *action ? action : "status");
    wb__buf_puts(&b, "\"}\n");
    if (b.p) wb__send(b.p, b.len);
    wb__buf_free(&b);
}

void wb_cancel(int id) {
    if (!g.inited) return;
    char line[64];
    snprintf(line, sizeof line, "{\"cmd\":\"cancel\",\"id\":%d}\n", id);
    wb__send(line, strlen(line));
}

void wb_shutdown(void) {
    static const char line[] = "{\"cmd\":\"shutdown\"}\n";
    bool started;
    if (!g.inited) return;
    EnterCriticalSection(&g.lock);
    started = g.started;
    LeaveCriticalSection(&g.lock);
    if (!started) {
        wb__set_state(WB_STATE_IDLE);
        return;
    }
    InterlockedExchange(&g.stopping, 1);
    wb__send(line, sizeof line - 1);
    wb__teardown();
    wb__set_state(WB_STATE_IDLE);
}

void wb_kill(void) {
    HANDLE proc;
    if (!g.inited) return;
    EnterCriticalSection(&g.lock);
    proc = g.proc;
    LeaveCriticalSection(&g.lock);
    if (!proc) {
        wb__teardown();
        wb__set_state(WB_STATE_IDLE);
        return;
    }
    TerminateProcess(proc, 0);
    WaitForSingleObject(proc, 2000);
    wb__teardown();
    wb__set_state(WB_STATE_IDLE);
}

int wb_poll(wb_event *ev) {
    if (!ev) return 0;
    if (!g.inited) return 0;
    EnterCriticalSection(&g.lock);
    if (g.qcount == 0) {
        LeaveCriticalSection(&g.lock);
        return 0;
    }
    *ev = g.q[g.qhead];
    memset(&g.q[g.qhead], 0, sizeof g.q[g.qhead]);
    g.qhead = (g.qhead + 1) % WB_QUEUE_CAP;
    g.qcount--;
    LeaveCriticalSection(&g.lock);
    return 1;
}

bool wb_alive(void) {
    HANDLE proc;
    if (!g.inited) return false;
    EnterCriticalSection(&g.lock);
    proc = g.proc;
    LeaveCriticalSection(&g.lock);
    if (!proc) return false;
    return WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
}

void wb_event_free(wb_event *ev) {
    if (!ev) return;
    free(ev->preview_b64);
    ev->preview_b64 = NULL;
}

/* ------------------------------------------------------------ base64/PNG */

static int wb__b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static unsigned char *wb__b64_decode(const char *s, size_t *out_len) {
    size_t n = strlen(s);
    size_t cap = (n / 4) * 3 + 4;
    unsigned char *out = (unsigned char *)malloc(cap ? cap : 1);
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = wb__b64v((unsigned char)s[i]);
        if (v < 0) continue; /* '=', whitespace, junk */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFFu);
        }
    }
    *out_len = o;
    return out;
}

int wb_decode_preview(const char *b64, unsigned char **rgba, int *w, int *h) {
    if (rgba) *rgba = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!b64 || !*b64 || !rgba || !w || !h) return 0;

    size_t n = 0;
    unsigned char *png = wb__b64_decode(b64, &n);
    if (!png) return 0;
    img_pixels px = {0};
    bool ok = n > 0 && img_decode_memory(png, n, &px);
    free(png);
    if (!ok) return 0;
    *rgba = px.rgba;
    *w = px.w;
    *h = px.h;
    return 1;
}

void wb_free_pixels(unsigned char *rgba) {
    free(rgba);
}

bool wb_model_ready(void) {
    bool v;
    if (!g.inited) return false;
    EnterCriticalSection(&g.lock);
    v = g.model_ready;
    LeaveCriticalSection(&g.lock);
    return v;
}

const char *wb_models_dir(void) { return g.models_dir; }

const char *wb_device(void) { return g.device; }
const char *wb_torch(void) { return g.torch; }
const char *wb_gpu(void) { return g.gpu; }
const char *wb_last_error(void) { return g.last_error; }
const char *wb_log_path(void) { return g.log_path; }
