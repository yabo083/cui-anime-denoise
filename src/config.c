/* Settings, paths and the profile-path redaction used by the diagnostics pack.
 *
 * Layout (see config.h):
 *   %LOCALAPPDATA%\AnimeDenoise\settings.json
 *   %LOCALAPPDATA%\AnimeDenoise\{logs,projects,models,updates}
 *
 * The rotating log, crash reports and the diagnostics zip live in log.c; the
 * two halves of the module share config_priv.h. No third-party code: the JSON
 * reader/writer is local.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>

#include "config.h"
#include "config_priv.h"

/* ------------------------------------------------------------------ paths */

/* The cfg__* entries in this block are read by log.c as well; see
 * config_priv.h for the contract. */
bool cfg__ready;

static wchar_t g_data_w[CFG_WCHAR_CAP];
wchar_t cfg__log_dir_w[CFG_WCHAR_CAP];
static wchar_t g_projects_w[CFG_WCHAR_CAP];
static wchar_t g_models_w[CFG_WCHAR_CAP];
static wchar_t g_updates_w[CFG_WCHAR_CAP];
static wchar_t g_exe_w[CFG_WCHAR_CAP];
static wchar_t g_out_w[CFG_WCHAR_CAP];
wchar_t cfg__settings_path_w[CFG_WCHAR_CAP];
static wchar_t g_profile_w[CFG_WCHAR_CAP];

char cfg__data_u8[CFG_PATH_CAP];
static char g_log_u8[CFG_PATH_CAP];
static char g_models_u8[CFG_PATH_CAP];
static char g_exe_u8[CFG_PATH_CAP];
static char g_out_u8[CFG_PATH_CAP];
static char g_profile_u8[CFG_PATH_CAP];

bool cfg__to_utf8(const wchar_t *w, char *out, size_t cap)
{
    int n;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!w || !w[0]) {
        return true;
    }
    if (cap > (size_t)INT32_MAX) {
        cap = (size_t)INT32_MAX;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

bool cfg__path_join(wchar_t *out, size_t cap, const wchar_t *base, const wchar_t *leaf)
{
    size_t n;
    bool sep;
    if (!out || !base || !leaf) {
        return false;
    }
    n = wcslen(base);
    sep = n > 0 && base[n - 1] != L'\\' && base[n - 1] != L'/';
    return SUCCEEDED(StringCchPrintfW(out, cap, sep ? L"%s\\%s" : L"%s%s", base, leaf));
}

static bool ensure_dir_w(const wchar_t *path)
{
    wchar_t tmp[CFG_WCHAR_CAP];
    wchar_t *p;
    size_t skip;
    DWORD attr;
    DWORD err;

    attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (FAILED(StringCchCopyW(tmp, CFG_WCHAR_CAP, path))) {
        return false;
    }
    skip = (wcslen(tmp) > 2 && tmp[1] == L':') ? 3 : 1;
    for (p = tmp + skip; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t save = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = save;
        }
    }
    if (CreateDirectoryW(tmp, NULL)) {
        return true;
    }
    err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        attr = GetFileAttributesW(tmp);
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    return false;
}

/* Probe write access without leaving anything behind. */
static bool dir_is_writable_w(const wchar_t *dir)
{
    wchar_t probe[CFG_WCHAR_CAP];
    HANDLE h;
    if (FAILED(StringCchPrintfW(probe, CFG_WCHAR_CAP, L"%s\\.write-probe-%lu.tmp",
                                dir, (unsigned long)GetCurrentProcessId()))) {
        return false;
    }
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe);
    return true;
}

bool cfg__read_file_w(const wchar_t *path, char **out, size_t *out_len, size_t max_bytes)
{
    HANDLE h;
    LARGE_INTEGER size;
    char *buf;
    size_t total;
    size_t got;

    *out = NULL;
    *out_len = 0;
    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    if (size.QuadPart < 0 || (unsigned long long)size.QuadPart > (unsigned long long)max_bytes) {
        CloseHandle(h);
        return false;
    }
    total = (size_t)size.QuadPart;
    buf = (char *)malloc(total + 1);
    if (!buf) {
        CloseHandle(h);
        return false;
    }
    got = 0;
    while (got < total) {
        DWORD chunk = (DWORD)((total - got) > 0x10000000u ? 0x10000000u : (total - got));
        DWORD read = 0;
        if (!ReadFile(h, buf + got, chunk, &read, NULL) || read == 0) {
            break;
        }
        got += read;
    }
    CloseHandle(h);
    if (got != total) {
        free(buf);
        return false;
    }
    buf[total] = 0;
    *out = buf;
    *out_len = total;
    return true;
}

bool cfg__write_file_w(const wchar_t *path, const void *data, size_t len)
{
    HANDLE h;
    bool ok = true;
    const unsigned char *p = (const unsigned char *)data;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    while (len) {
        DWORD chunk = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, NULL) || written != chunk) {
            ok = false;
            break;
        }
        p += chunk;
        len -= chunk;
    }
    CloseHandle(h);
    return ok;
}

/* tmp file + MoveFileEx: the old settings.json survives any failure. */
static bool write_atomic_w(const wchar_t *final_path, const void *data, size_t len)
{
    wchar_t tmp[CFG_WCHAR_CAP];
    bool ok;

    if (FAILED(StringCchPrintfW(tmp, CFG_WCHAR_CAP, L"%s.tmp", final_path))) {
        return false;
    }
    ok = cfg__write_file_w(tmp, data, len);
    if (ok) {
        ok = MoveFileExW(tmp, final_path,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) {
        DeleteFileW(tmp);
    }
    return ok;
}

/* ------------------------------------------------------- path redaction */

/* Match g_profile_u8 at data[pos], tolerating JSON-escaped backslashes
 * ("C:\\Users\\x" in settings.json) and ASCII case differences. */
static bool redact_match_at(const char *data, size_t len, size_t pos,
                            size_t plen, size_t *consumed)
{
    size_t j = pos;
    size_t i;

    for (i = 0; i < plen; ++i) {
        char nc = g_profile_u8[i];
        char dc;

        if (j >= len) {
            return false;
        }
        dc = data[j];
        if (nc == '\\') {
            if (dc != '\\') {
                return false;
            }
            j++;
            if (j < len && data[j] == '\\' &&
                !(i + 1 < plen && g_profile_u8[i + 1] == '\\')) {
                j++;
            }
            continue;
        }
        if (nc >= 'A' && nc <= 'Z') {
            nc = (char)(nc - 'A' + 'a');
        }
        if (dc >= 'A' && dc <= 'Z') {
            dc = (char)(dc - 'A' + 'a');
        }
        if (nc != dc) {
            return false;
        }
        j++;
    }
    *consumed = j - pos;
    return true;
}

/* Replace the user profile path with a neutral marker. */
char *cfg__redact_alloc(const char *data, size_t len, size_t *out_len)
{
    static const char mask[] = "<\xE7\x94\xA8\xE6\x88\xB7\xE7\x9B\xAE\xE5\xBD\x95>";
    size_t mlen = sizeof mask - 1;
    size_t plen = strlen(g_profile_u8);
    size_t count = 0;
    size_t i;
    size_t o = 0;
    size_t consumed = 0;
    char *out;

    *out_len = len;
    if (plen == 0) {
        out = (char *)malloc(len + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, data, len);
        out[len] = 0;
        return out;
    }
    i = 0;
    while (i < len) {
        if (redact_match_at(data, len, i, plen, &consumed)) {
            count++;
            i += consumed;
        } else {
            i++;
        }
    }
    if (count == 0) {
        out = (char *)malloc(len + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, data, len);
        out[len] = 0;
        return out;
    }
    out = (char *)malloc(len + count * mlen + 1);
    if (!out) {
        return NULL;
    }
    i = 0;
    while (i < len) {
        if (redact_match_at(data, len, i, plen, &consumed)) {
            memcpy(out + o, mask, mlen);
            o += mlen;
            i += consumed;
        } else {
            out[o++] = data[i++];
        }
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

/* --------------------------------------------------------------- settings */

void cfg_defaults(app_settings *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof *s);
    /* product defaults: the "recommended" preset and the brush the web UI ships */
    s->preset = 1;                 /* balanced */
    s->down_pct = 60;
    s->l0 = 0.001f;
    s->device = 0;                 /* auto */
    s->brush_size = 14.0f;
    s->brush_mix = 90.0f;
    s->feather_px = 10.0f;
    s->theme_mode = 0;
    s->view_mode = 0;
    s->auto_check_updates = 1;
    s->seen_welcome = 0;
    s->window_w = 1280;
    s->window_h = 820;
    s->window_maximized = 0;
    s->output_dir[0] = 0;
    s->model_root[0] = 0;
    s->last_open_dir[0] = 0;
}

static void clamp_int(int *v, int lo, int hi)
{
    if (*v < lo) {
        *v = lo;
    } else if (*v > hi) {
        *v = hi;
    }
}

static void clamp_float(float *v, float lo, float hi, float fallback)
{
    if (*v != *v) {              /* NaN */
        *v = fallback;
    } else if (*v < lo) {
        *v = lo;
    } else if (*v > hi) {
        *v = hi;
    }
}

static void cfg_clamp(app_settings *s)
{
    clamp_int(&s->preset, 0, 3);
    clamp_int(&s->down_pct, 10, 100);
    clamp_float(&s->l0, 0.0f, 0.005f, 0.0f);
    clamp_int(&s->device, 0, 2);
    clamp_float(&s->brush_size, 2.0f, 300.0f, 60.0f);
    clamp_float(&s->brush_mix, 0.0f, 100.0f, 50.0f);
    clamp_float(&s->feather_px, 0.0f, 150.0f, 8.0f);
    clamp_int(&s->theme_mode, 0, 2);
    clamp_int(&s->view_mode, 0, 2);
    clamp_int(&s->auto_check_updates, 0, 1);
    clamp_int(&s->seen_welcome, 0, 1);
    clamp_int(&s->window_maximized, 0, 1);
    clamp_int(&s->window_w, 0, 32767);
    clamp_int(&s->window_h, 0, 32767);
    s->output_dir[CFG_PATH_CAP - 1] = 0;
    s->model_root[CFG_PATH_CAP - 1] = 0;
    s->last_open_dir[CFG_PATH_CAP - 1] = 0;
}

/* ---- tiny growable string buffer (system info in log.c) ---- */

static void cfg__sb_reserve(cfg__sb *b, size_t extra)
{
    size_t need;
    size_t cap;
    char *np;

    if (b->oom) {
        return;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return;
    }
    cap = b->cap ? b->cap : 512;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) {
            b->oom = true;
            return;
        }
        cap *= 2;
    }
    np = (char *)realloc(b->p, cap);
    if (!np) {
        b->oom = true;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void cfg__sb_addn(cfg__sb *b, const char *s, size_t n)
{
    cfg__sb_reserve(b, n);
    if (b->oom) {
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void cfg__sb_adds(cfg__sb *b, const char *s)
{
    if (s) {
        cfg__sb_addn(b, s, strlen(s));
    }
}

static void cfg__sb_addc(cfg__sb *b, char c)
{
    cfg__sb_addn(b, &c, 1);
}

void cfg__sb_addf(cfg__sb *b, const char *fmt, ...)
{
    char tmp[1600];
    va_list ap;
    int n;
    size_t len;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = true;
        return;
    }
    len = (size_t)n;
    if (len >= sizeof tmp) {
        len = sizeof tmp - 1;
    }
    cfg__sb_addn(b, tmp, len);
}

void cfg__sb_free(cfg__sb *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

static void json_escape(cfg__sb *b, const char *s)
{
    const unsigned char *p;

    cfg__sb_addc(b, '"');
    for (p = (const unsigned char *)s; p && *p; ++p) {
        switch (*p) {
        case '"':  cfg__sb_addn(b, "\\\"", 2); break;
        case '\\': cfg__sb_addn(b, "\\\\", 2); break;
        case '\n': cfg__sb_addn(b, "\\n", 2); break;
        case '\r': cfg__sb_addn(b, "\\r", 2); break;
        case '\t': cfg__sb_addn(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                cfg__sb_addf(b, "\\u%04x", (unsigned)*p);
            } else {
                cfg__sb_addc(b, (char)*p);
            }
            break;
        }
    }
    cfg__sb_addc(b, '"');
}

static void settings_to_json(const app_settings *s, cfg__sb *b)
{
    cfg__sb_adds(b, "{\n");
    cfg__sb_addf(b, "    \"preset\": %d,\n", s->preset);
    cfg__sb_addf(b, "    \"down_pct\": %d,\n", s->down_pct);
    cfg__sb_addf(b, "    \"l0\": %.6g,\n", (double)s->l0);
    cfg__sb_addf(b, "    \"device\": %d,\n", s->device);
    cfg__sb_addf(b, "    \"brush_size\": %.6g,\n", (double)s->brush_size);
    cfg__sb_addf(b, "    \"brush_mix\": %.6g,\n", (double)s->brush_mix);
    cfg__sb_addf(b, "    \"feather_px\": %.6g,\n", (double)s->feather_px);
    cfg__sb_addf(b, "    \"theme_mode\": %d,\n", s->theme_mode);
    cfg__sb_addf(b, "    \"view_mode\": %d,\n", s->view_mode);
    cfg__sb_addf(b, "    \"auto_check_updates\": %d,\n", s->auto_check_updates);
    cfg__sb_addf(b, "    \"seen_welcome\": %d,\n", s->seen_welcome);
    cfg__sb_addf(b, "    \"window_w\": %d,\n", s->window_w);
    cfg__sb_addf(b, "    \"window_h\": %d,\n", s->window_h);
    cfg__sb_addf(b, "    \"window_maximized\": %d,\n", s->window_maximized);
    cfg__sb_adds(b, "    \"output_dir\": ");
    json_escape(b, s->output_dir);
    cfg__sb_adds(b, ",\n    \"model_root\": ");
    json_escape(b, s->model_root);
    cfg__sb_adds(b, ",\n    \"last_open_dir\": ");
    json_escape(b, s->last_open_dir);
    cfg__sb_adds(b, "\n}\n");
}

/* ---- tolerant JSON reader ---- */

typedef struct jscan {
    const char *p;
    const char *end;
} jscan;

typedef enum jtype {
    J_NONE = 0,
    J_STRING,
    J_NUMBER,
    J_BOOL,
    J_NULL
} jtype;

static void j_skip(jscan *j)
{
    for (;;) {
        while (j->p < j->end && (unsigned char)*j->p <= 0x20) {
            j->p++;
        }
        if ((size_t)(j->end - j->p) >= 2 && j->p[0] == '/' && j->p[1] == '/') {
            j->p += 2;
            while (j->p < j->end && *j->p != '\n') {
                j->p++;
            }
            continue;
        }
        if ((size_t)(j->end - j->p) >= 2 && j->p[0] == '/' && j->p[1] == '*') {
            j->p += 2;
            while ((size_t)(j->end - j->p) >= 2 && !(j->p[0] == '*' && j->p[1] == '/')) {
                j->p++;
            }
            if ((size_t)(j->end - j->p) >= 2) {
                j->p += 2;
            }
            continue;
        }
        break;
    }
}

static bool j_hex4(jscan *j, unsigned *out)
{
    unsigned v = 0;
    int k;

    if ((size_t)(j->end - j->p) < 4) {
        return false;
    }
    for (k = 0; k < 4; ++k) {
        char h = *j->p++;
        unsigned d;
        if (h >= '0' && h <= '9') {
            d = (unsigned)(h - '0');
        } else if (h >= 'a' && h <= 'f') {
            d = (unsigned)(h - 'a' + 10);
        } else if (h >= 'A' && h <= 'F') {
            d = (unsigned)(h - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static size_t utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static bool j_string(jscan *j, char *out, size_t cap)
{
    size_t n = 0;

    j_skip(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    if (cap) {
        out[0] = 0;
    }
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p;
        if (c == '"') {
            j->p++;
            if (cap) {
                out[n < cap ? n : cap - 1] = 0;
            }
            return true;
        }
        j->p++;
        if (c == '\\') {
            unsigned ch;
            char e;
            if (j->p >= j->end) {
                break;
            }
            e = *j->p++;
            switch (e) {
            case '"':  ch = '"';  break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/';  break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            case 'u': {
                unsigned cp = 0xFFFD;
                if (j_hex4(j, &cp) && cp >= 0xD800 && cp <= 0xDBFF &&
                    (size_t)(j->end - j->p) >= 2 && j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo = 0;
                    j->p += 2;
                    if (j_hex4(j, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                }
                ch = cp;
                break;
            }
            default:   ch = (unsigned char)e; break;
            }
            {
                char enc[4];
                size_t el = utf8_encode(ch, enc);
                size_t k;
                for (k = 0; k < el; ++k) {
                    if (n + 1 < cap) {
                        out[n++] = enc[k];
                    }
                }
            }
        } else {
            if (n + 1 < cap) {
                out[n++] = (char)c;
            }
        }
    }
    if (cap) {
        out[n < cap ? n : cap - 1] = 0;
    }
    return false;
}

static void j_skip_compound(jscan *j)
{
    int depth = 0;
    bool in_string = false;

    while (j->p < j->end) {
        char c = *j->p++;
        if (in_string) {
            if (c == '\\') {
                if (j->p < j->end) {
                    j->p++;
                }
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
            if (depth <= 0) {
                return;
            }
        }
    }
}

static jtype j_value(jscan *j, char *str, size_t str_cap, double *num, bool *b)
{
    char c;

    j_skip(j);
    if (j->p >= j->end) {
        return J_NONE;
    }
    c = *j->p;
    if (c == '"') {
        return j_string(j, str, str_cap) ? J_STRING : J_NONE;
    }
    if (c == 't' && (size_t)(j->end - j->p) >= 4 && memcmp(j->p, "true", 4) == 0) {
        j->p += 4;
        *b = true;
        return J_BOOL;
    }
    if (c == 'f' && (size_t)(j->end - j->p) >= 5 && memcmp(j->p, "false", 5) == 0) {
        j->p += 5;
        *b = false;
        return J_BOOL;
    }
    if (c == 'n' && (size_t)(j->end - j->p) >= 4 && memcmp(j->p, "null", 4) == 0) {
        j->p += 4;
        return J_NULL;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = NULL;
        double v = strtod(j->p, &endp);
        if (!endp || endp == j->p) {
            return J_NONE;
        }
        j->p = endp;
        *num = v;
        return J_NUMBER;
    }
    if (c == '[' || c == '{') {
        j_skip_compound(j);
        return J_NONE;
    }
    j->p++;                      /* unknown junk: make progress and ignore */
    return J_NONE;
}

static bool j_int(jtype t, double num, int *out)
{
    if (t != J_NUMBER || !(num >= -2147483648.0 && num <= 2147483647.0)) {
        return false;
    }
    *out = (int)num;
    return true;
}

static bool j_float(jtype t, double num, float *out)
{
    if (t != J_NUMBER || !(num >= -1e30 && num <= 1e30)) {
        return false;
    }
    *out = (float)num;
    return true;
}

static void j_string_field(jtype t, const char *val, char *dst)
{
    if (t == J_STRING) {
        strncpy_s(dst, CFG_PATH_CAP, val, _TRUNCATE);
    }
}

static void apply_setting(app_settings *s, const char *key, jtype t,
                          const char *str, double num, bool b)
{
    (void)b;
    if (strcmp(key, "preset") == 0) {
        j_int(t, num, &s->preset);
    } else if (strcmp(key, "down_pct") == 0) {
        j_int(t, num, &s->down_pct);
    } else if (strcmp(key, "device") == 0) {
        j_int(t, num, &s->device);
    } else if (strcmp(key, "l0") == 0) {
        j_float(t, num, &s->l0);
    } else if (strcmp(key, "brush_size") == 0) {
        j_float(t, num, &s->brush_size);
    } else if (strcmp(key, "brush_mix") == 0) {
        j_float(t, num, &s->brush_mix);
    } else if (strcmp(key, "feather_px") == 0) {
        j_float(t, num, &s->feather_px);
    } else if (strcmp(key, "theme_mode") == 0) {
        j_int(t, num, &s->theme_mode);
    } else if (strcmp(key, "view_mode") == 0) {
        j_int(t, num, &s->view_mode);
    } else if (strcmp(key, "auto_check_updates") == 0) {
        j_int(t, num, &s->auto_check_updates);
    } else if (strcmp(key, "seen_welcome") == 0) {
        j_int(t, num, &s->seen_welcome);
    } else if (strcmp(key, "window_w") == 0) {
        j_int(t, num, &s->window_w);
    } else if (strcmp(key, "window_h") == 0) {
        j_int(t, num, &s->window_h);
    } else if (strcmp(key, "window_maximized") == 0) {
        j_int(t, num, &s->window_maximized);
    } else if (strcmp(key, "output_dir") == 0) {
        j_string_field(t, str, s->output_dir);
    } else if (strcmp(key, "model_root") == 0) {
        j_string_field(t, str, s->model_root);
    } else if (strcmp(key, "last_open_dir") == 0) {
        j_string_field(t, str, s->last_open_dir);
    }
}

bool cfg_load(app_settings *s)
{
    char *buf = NULL;
    size_t len = 0;
    jscan j;
    char key[64];
    char str[CFG_PATH_CAP];

    if (!s) {
        return false;
    }
    cfg_defaults(s);
    if (!cfg__ready || !cfg__settings_path_w[0]) {
        return true;
    }
    if (!cfg__read_file_w(cfg__settings_path_w, &buf, &len, SETTINGS_MAX_BYTES)) {
        return true;             /* missing or unreadable: defaults */
    }
    j.p = buf;
    j.end = buf + len;
    if (len >= 3 && (unsigned char)j.p[0] == 0xEF &&
        (unsigned char)j.p[1] == 0xBB && (unsigned char)j.p[2] == 0xBF) {
        j.p += 3;
    }
    j_skip(&j);
    if (j.p >= j.end || *j.p != '{') {
        free(buf);
        return true;
    }
    j.p++;
    for (;;) {
        jtype t;
        double num = 0.0;
        bool b = false;

        j_skip(&j);
        if (j.p >= j.end || *j.p == '}') {
            break;
        }
        if (*j.p == ',') {       /* tolerate trailing/stray commas */
            j.p++;
            continue;
        }
        if (*j.p != '"') {
            j.p++;               /* junk token: resynchronize */
            continue;
        }
        key[0] = 0;
        if (!j_string(&j, key, sizeof key)) {
            break;
        }
        j_skip(&j);
        if (j.p < j.end && *j.p == ':') {
            j.p++;
        } else {
            continue;
        }
        str[0] = 0;
        t = j_value(&j, str, sizeof str, &num, &b);
        apply_setting(s, key, t, str, num, b);
    }
    free(buf);
    cfg_clamp(s);
    return true;
}

bool cfg_save(const app_settings *s)
{
    app_settings copy;
    cfg__sb b;
    bool ok;

    if (!s || !cfg__ready || !cfg__settings_path_w[0]) {
        return false;
    }
    copy = *s;
    cfg_clamp(&copy);

    memset(&b, 0, sizeof b);
    settings_to_json(&copy, &b);
    if (b.oom || !b.p) {
        cfg__sb_free(&b);
        return false;
    }
    ok = write_atomic_w(cfg__settings_path_w, b.p, b.len);
    cfg__sb_free(&b);
    return ok;
}

bool cfg_reset(void)
{
    if (!cfg__ready || !cfg__settings_path_w[0]) {
        return false;
    }
    if (DeleteFileW(cfg__settings_path_w)) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

/* --------------------------------------------------------- init/shutdown */

const char *cfg_data_dir(void)
{
    return cfg__data_u8;
}

const char *cfg_log_dir(void)
{
    return g_log_u8;
}

const char *cfg_models_dir(void)
{
    return g_models_u8;
}

const char *cfg_default_out_dir(void)
{
    return g_out_u8;
}

const char *cfg_exe_dir(void)
{
    return g_exe_u8;
}

bool cfg_init(void)
{
    PWSTR local = NULL;
    PWSTR profile = NULL;
    wchar_t cand[CFG_WCHAR_CAP];
    DWORD n;

    if (cfg__ready) {
        return true;
    }

    memset(g_data_w, 0, sizeof g_data_w);
    cfg__log_dir_w[0] = g_projects_w[0] = g_models_w[0] = 0;
    g_updates_w[0] = g_exe_w[0] = g_out_w[0] = cfg__settings_path_w[0] = 0;
    g_profile_w[0] = 0;
    cfg__data_u8[0] = g_log_u8[0] = g_models_u8[0] = 0;
    g_exe_u8[0] = g_out_u8[0] = g_profile_u8[0] = 0;

    n = GetModuleFileNameW(NULL, g_exe_w, CFG_WCHAR_CAP);
    if (n == 0 || n >= CFG_WCHAR_CAP) {
        g_exe_w[0] = 0;
    } else {
        wchar_t *slash = wcsrchr(g_exe_w, L'\\');
        if (slash) {
            *slash = 0;
        }
    }

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &profile)) && profile) {
        StringCchCopyW(g_profile_w, CFG_WCHAR_CAP, profile);
        CoTaskMemFree(profile);
    }

    /* Test hook: an explicit root keeps automated runs out of the real
     * profile. Production always uses %LOCALAPPDATA%\AnimeDenoise. */
    n = GetEnvironmentVariableW(L"ANIME_DENOISE_DATA_DIR", g_data_w, CFG_WCHAR_CAP);
    if (n == 0 || n >= CFG_WCHAR_CAP) {
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &local)) ||
            !local) {
            if (local) {
                CoTaskMemFree(local);
            }
            return false;
        }
        cfg__path_join(g_data_w, CFG_WCHAR_CAP, local, L"AnimeDenoise");
        CoTaskMemFree(local);
    }

    if (!ensure_dir_w(g_data_w)) {
        return false;
    }
    cfg__path_join(cfg__log_dir_w, CFG_WCHAR_CAP, g_data_w, L"logs");
    cfg__path_join(g_projects_w, CFG_WCHAR_CAP, g_data_w, L"projects");
    cfg__path_join(g_models_w, CFG_WCHAR_CAP, g_data_w, L"models");
    cfg__path_join(g_updates_w, CFG_WCHAR_CAP, g_data_w, L"updates");
    ensure_dir_w(cfg__log_dir_w);
    ensure_dir_w(g_projects_w);
    ensure_dir_w(g_models_w);
    ensure_dir_w(g_updates_w);
    cfg__path_join(cfg__settings_path_w, CFG_WCHAR_CAP, g_data_w, L"settings.json");

    /* output dir: prefer <exe>\outputs, fall back to the data dir. */
    cand[0] = 0;
    {
        bool use_exe = false;
        if (g_exe_w[0]) {
            wchar_t outs[CFG_WCHAR_CAP];
            DWORD attr;
            if (cfg__path_join(outs, CFG_WCHAR_CAP, g_exe_w, L"outputs")) {
                attr = GetFileAttributesW(outs);
                if (attr != INVALID_FILE_ATTRIBUTES) {
                    use_exe = (attr & FILE_ATTRIBUTE_DIRECTORY) &&
                              dir_is_writable_w(outs);
                } else {
                    use_exe = dir_is_writable_w(g_exe_w) &&
                              ensure_dir_w(outs) && dir_is_writable_w(outs);
                }
            }
        }
        if (use_exe) {
            cfg__path_join(cand, CFG_WCHAR_CAP, g_exe_w, L"outputs");
        } else {
            cfg__path_join(cand, CFG_WCHAR_CAP, g_data_w, L"outputs");
            ensure_dir_w(cand);
        }
    }
    StringCchCopyW(g_out_w, CFG_WCHAR_CAP, cand);

    cfg__to_utf8(g_data_w, cfg__data_u8, CFG_PATH_CAP);
    cfg__to_utf8(cfg__log_dir_w, g_log_u8, CFG_PATH_CAP);
    cfg__to_utf8(g_models_w, g_models_u8, CFG_PATH_CAP);
    cfg__to_utf8(g_exe_w, g_exe_u8, CFG_PATH_CAP);
    cfg__to_utf8(g_out_w, g_out_u8, CFG_PATH_CAP);
    cfg__to_utf8(g_profile_w, g_profile_u8, CFG_PATH_CAP);

    cfg__ready = true;
    log_open();
    log_info("cfg: data dir %s", cfg__data_u8);
    log_info("cfg: exe dir %s, output dir %s", g_exe_u8, g_out_u8);
    return true;
}

void cfg_shutdown(void)
{
    log_close();
    cfg__ready = false;
    g_data_w[0] = cfg__log_dir_w[0] = g_projects_w[0] = g_models_w[0] = 0;
    g_updates_w[0] = g_exe_w[0] = g_out_w[0] = cfg__settings_path_w[0] = g_profile_w[0] = 0;
    cfg__data_u8[0] = g_log_u8[0] = g_models_u8[0] = 0;
    g_exe_u8[0] = g_out_u8[0] = g_profile_u8[0] = 0;
}
