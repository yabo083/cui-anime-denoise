/* backend_priv.h -- internals of the worker bridge, shared by backend.c and
 * backend_json.c.
 *
 * Internal header: not installed, not part of the backend.h contract. Only the
 * two bridge translation units may include it.
 */
#ifndef CUI_ANIME_BACKEND_PRIV_H
#define CUI_ANIME_BACKEND_PRIV_H

#include "backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#define WB_QUEUE_CAP 256

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

/* Defined once, in backend.c. */
extern wb_ctx g;

/* ------------------------------------------------------------ byte buffer */

typedef struct {
    char *p;
    size_t len, cap;
} wb_buf;

bool wb__buf_reserve(wb_buf *b, size_t extra);
void wb__buf_put(wb_buf *b, const char *s, size_t n);
void wb__buf_puts(wb_buf *b, const char *s);
void wb__buf_putc(wb_buf *b, char c);
void wb__buf_free(wb_buf *b);

/* ------------------------------------- backend_json.c: UTF-8/JSON/base64 */

wchar_t *wb__u8_to_w(const char *s);
void wb__w_to_u8(const wchar_t *w, char *dst, size_t cap);

void wb__json_escape(wb_buf *b, const char *s);

const char *wb__ws(const char *p);
const char *wb__scan_string(const char *p, const char **content, size_t *len);
bool wb__skip_value(const char **pp);
const char *wb__value_of(const char *json, const char *key);
int wb__hexval(char c);
unsigned wb__hex4(const char *s);
void wb__utf8_put(wb_buf *b, unsigned cp);
void wb__unescape(wb_buf *b, const char *s, size_t len);
bool wb__json_raw(const char *json, const char *key, const char **content, size_t *len);
bool wb__json_str_into(const char *json, const char *key, char *dst, size_t cap);
char *wb__json_alloc_str(const char *json, const char *key);
bool wb__parse_number(const char *p, double *out);
bool wb__json_number(const char *json, const char *key, double *out);
bool wb__json_int(const char *json, const char *key, int *out);
bool wb__json_bool(const char *json, const char *key);

int wb__b64v(int c);
unsigned char *wb__b64_decode(const char *s, size_t *out_len);

/* ---------------------------- backend.c: state, queue and stdin transport */

void wb__set_error(const char *msg);
void wb__set_state(wb_state s);
void wb__note_boot(void);
void wb__push(wb_event *ev);
void wb__purge(void);
bool wb__send(const char *json, size_t len);

#endif /* CUI_ANIME_BACKEND_PRIV_H */
