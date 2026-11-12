/* Settings, paths, the rotating log, crash reports and diagnostics.
 *
 * Layout (see config.h):
 *   %LOCALAPPDATA%\AnimeDenoise\settings.json
 *   %LOCALAPPDATA%\AnimeDenoise\logs\app.log(.1 .2 .3)
 *   %LOCALAPPDATA%\AnimeDenoise\logs\crash-YYYYMMDD-HHMMSS.txt
 *   %LOCALAPPDATA%\AnimeDenoise\logs\diagnostics-YYYYMMDD-HHMMSS.zip
 *   %LOCALAPPDATA%\AnimeDenoise\{projects,runtime,updates}
 *
 * No third-party code: the JSON reader/writer, the CRC32 and the store-only
 * zip writer are all local. dbghelp is loaded with LoadLibrary solely for
 * optional symbol names in crash reports, so it is never a link dependency.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <share.h>
#include <wchar.h>

#include "config.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#define CFG_WCHAR_CAP 1024
#define LOG_ROTATE_BYTES (4u * 1024u * 1024u)
#define LOG_MSG_CAP 3600
#define LOG_LINE_CAP 4096
#define SETTINGS_MAX_BYTES (1024u * 1024u)
#define DIAG_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define DIAG_MAX_ENTRIES 64
#define CRASH_REPORT_CAP (96u * 1024u)
#define CRASH_LOG_TAIL_BYTES (64u * 1024u)

/* ------------------------------------------------------------------ paths */

static bool g_ready;

static wchar_t g_data_w[CFG_WCHAR_CAP];
static wchar_t g_log_w[CFG_WCHAR_CAP];
static wchar_t g_projects_w[CFG_WCHAR_CAP];
static wchar_t g_runtime_w[CFG_WCHAR_CAP];
static wchar_t g_updates_w[CFG_WCHAR_CAP];
static wchar_t g_exe_w[CFG_WCHAR_CAP];
static wchar_t g_out_w[CFG_WCHAR_CAP];
static wchar_t g_settings_w[CFG_WCHAR_CAP];
static wchar_t g_profile_w[CFG_WCHAR_CAP];

static char g_data_u8[CFG_PATH_CAP];
static char g_log_u8[CFG_PATH_CAP];
static char g_runtime_u8[CFG_PATH_CAP];
static char g_exe_u8[CFG_PATH_CAP];
static char g_out_u8[CFG_PATH_CAP];
static char g_profile_u8[CFG_PATH_CAP];

static bool to_utf8(const wchar_t *w, char *out, size_t cap)
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

static bool path_join(wchar_t *out, size_t cap, const wchar_t *base, const wchar_t *leaf)
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

static bool read_file_w(const wchar_t *path, char **out, size_t *out_len, size_t max_bytes)
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

static bool write_file_w(const wchar_t *path, const void *data, size_t len)
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
    ok = write_file_w(tmp, data, len);
    if (ok) {
        ok = MoveFileExW(tmp, final_path,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) {
        DeleteFileW(tmp);
    }
    return ok;
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

/* ---- tiny growable string buffer ---- */

typedef struct sb {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
} sb;

static void sb_reserve(sb *b, size_t extra)
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

static void sb_addn(sb *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    if (b->oom) {
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_adds(sb *b, const char *s)
{
    if (s) {
        sb_addn(b, s, strlen(s));
    }
}

static void sb_addc(sb *b, char c)
{
    sb_addn(b, &c, 1);
}

static void sb_addf(sb *b, const char *fmt, ...)
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
    sb_addn(b, tmp, len);
}

static void sb_free(sb *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

static void json_escape(sb *b, const char *s)
{
    const unsigned char *p;

    sb_addc(b, '"');
    for (p = (const unsigned char *)s; p && *p; ++p) {
        switch (*p) {
        case '"':  sb_addn(b, "\\\"", 2); break;
        case '\\': sb_addn(b, "\\\\", 2); break;
        case '\n': sb_addn(b, "\\n", 2); break;
        case '\r': sb_addn(b, "\\r", 2); break;
        case '\t': sb_addn(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                sb_addf(b, "\\u%04x", (unsigned)*p);
            } else {
                sb_addc(b, (char)*p);
            }
            break;
        }
    }
    sb_addc(b, '"');
}

static void settings_to_json(const app_settings *s, sb *b)
{
    sb_adds(b, "{\n");
    sb_addf(b, "    \"preset\": %d,\n", s->preset);
    sb_addf(b, "    \"down_pct\": %d,\n", s->down_pct);
    sb_addf(b, "    \"l0\": %.6g,\n", (double)s->l0);
    sb_addf(b, "    \"brush_size\": %.6g,\n", (double)s->brush_size);
    sb_addf(b, "    \"brush_mix\": %.6g,\n", (double)s->brush_mix);
    sb_addf(b, "    \"feather_px\": %.6g,\n", (double)s->feather_px);
    sb_addf(b, "    \"theme_mode\": %d,\n", s->theme_mode);
    sb_addf(b, "    \"view_mode\": %d,\n", s->view_mode);
    sb_addf(b, "    \"auto_check_updates\": %d,\n", s->auto_check_updates);
    sb_addf(b, "    \"seen_welcome\": %d,\n", s->seen_welcome);
    sb_addf(b, "    \"window_w\": %d,\n", s->window_w);
    sb_addf(b, "    \"window_h\": %d,\n", s->window_h);
    sb_addf(b, "    \"window_maximized\": %d,\n", s->window_maximized);
    sb_adds(b, "    \"output_dir\": ");
    json_escape(b, s->output_dir);
    sb_adds(b, ",\n    \"model_root\": ");
    json_escape(b, s->model_root);
    sb_adds(b, ",\n    \"last_open_dir\": ");
    json_escape(b, s->last_open_dir);
    sb_adds(b, "\n}\n");
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
    if (!g_ready || !g_settings_w[0]) {
        return true;
    }
    if (!read_file_w(g_settings_w, &buf, &len, SETTINGS_MAX_BYTES)) {
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
    sb b;
    bool ok;

    if (!s || !g_ready || !g_settings_w[0]) {
        return false;
    }
    copy = *s;
    cfg_clamp(&copy);

    memset(&b, 0, sizeof b);
    settings_to_json(&copy, &b);
    if (b.oom || !b.p) {
        sb_free(&b);
        return false;
    }
    ok = write_atomic_w(g_settings_w, b.p, b.len);
    sb_free(&b);
    return ok;
}

bool cfg_reset(void)
{
    if (!g_ready || !g_settings_w[0]) {
        return false;
    }
    if (DeleteFileW(g_settings_w)) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

/* -------------------------------------------------------------------- log */

static CRITICAL_SECTION g_log_lock;
static bool g_log_lock_ready;
static FILE *g_log_file;
static wchar_t g_log_path_w[CFG_WCHAR_CAP];

static const char *log_level_name(log_level lvl)
{
    switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "INFO ";
    }
}

static void log_rotate_if_needed(void)
{
    wchar_t p0[CFG_WCHAR_CAP];
    wchar_t p1[CFG_WCHAR_CAP];
    wchar_t p2[CFG_WCHAR_CAP];
    wchar_t p3[CFG_WCHAR_CAP];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    unsigned long long size;

    if (!g_log_w[0]) {
        return;
    }
    if (FAILED(StringCchPrintfW(p0, CFG_WCHAR_CAP, L"%s\\app.log", g_log_w)) ||
        FAILED(StringCchPrintfW(p1, CFG_WCHAR_CAP, L"%s\\app.log.1", g_log_w)) ||
        FAILED(StringCchPrintfW(p2, CFG_WCHAR_CAP, L"%s\\app.log.2", g_log_w)) ||
        FAILED(StringCchPrintfW(p3, CFG_WCHAR_CAP, L"%s\\app.log.3", g_log_w))) {
        return;
    }
    if (!GetFileAttributesExW(p0, GetFileExInfoStandard, &fad)) {
        return;
    }
    size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size <= LOG_ROTATE_BYTES) {
        return;
    }
    DeleteFileW(p3);
    MoveFileExW(p2, p3, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(p1, p2, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(p0, p1, MOVEFILE_REPLACE_EXISTING);
}

/* Called with the log lock held, after the file grew past the threshold. */
static void log_reopen_locked(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    log_rotate_if_needed();
    if (g_log_path_w[0]) {
        g_log_file = _wfsopen(g_log_path_w, L"ab", _SH_DENYNO);
    }
}

void log_open(void)
{
    FILE *f = NULL;

    if (g_log_file) {
        return;
    }
    if (!g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = true;
    }
    if (!g_log_w[0]) {
        return;
    }
    log_rotate_if_needed();
    if (FAILED(StringCchPrintfW(g_log_path_w, CFG_WCHAR_CAP, L"%s\\app.log", g_log_w))) {
        g_log_path_w[0] = 0;
        return;
    }
    /* _SH_DENYNO: the crash handler must be able to read app.log while the
     * process still has it open for append. */
    f = _wfsopen(g_log_path_w, L"ab", _SH_DENYNO);
    if (!f) {
        g_log_path_w[0] = 0;     /* degrade to OutputDebugString only */
        return;
    }
    g_log_file = f;
}

void log_close(void)
{
    if (!g_log_file) {
        return;
    }
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

void log_write(log_level lvl, const char *fmt, ...)
{
    char msg[LOG_MSG_CAP];
    char line[LOG_LINE_CAP];
    SYSTEMTIME st;
    va_list ap;
    int n;
    int len;

    if (!fmt) {
        fmt = "";
    }
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) {
        msg[0] = 0;
    }
    GetLocalTime(&st);
    len = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\n",
                   (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                   (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                   (unsigned)st.wMilliseconds, log_level_name(lvl), msg);
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof line) {
        size_t cut = sizeof line - 2;
        line[cut] = '\n';
        line[cut + 1] = 0;
        len = (int)(cut + 1);
    }
    OutputDebugStringA(line);
    if (!g_log_file) {
        return;
    }
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_file) {
        fwrite(line, 1, (size_t)len, g_log_file);
        fflush(g_log_file);
        if (_ftelli64(g_log_file) > (long long)LOG_ROTATE_BYTES) {
            log_reopen_locked();
        }
    }
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

/* ------------------------------------------------------------ crash report */

static volatile LONG g_crash_entered;
static volatile LONG g_veh_installed;
static volatile LONG g_veh_have;
static EXCEPTION_RECORD g_veh_record;

/* The CRT turns access violations into SIGSEGV before the unhandled filter
 * runs, which loses the exception code and fault address. This vectored
 * handler only records them for the signal path. */
static LONG CALLBACK cfg_veh_capture(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord) {
        memcpy(&g_veh_record, ep->ExceptionRecord, sizeof g_veh_record);
        InterlockedExchange(&g_veh_have, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static EXCEPTION_POINTERS *cfg_signal_exception(int sig, EXCEPTION_POINTERS *storage)
{
    DWORD code;
    bool match = false;

    if (!g_veh_have) {
        return NULL;
    }
    code = g_veh_record.ExceptionCode;
    switch (sig) {
    case SIGSEGV:
        match = code == 0xC0000005u || code == 0xC0000006u;
        break;
    case SIGFPE:
        match = code == 0xC000008Eu || code == 0xC0000090u ||
                code == 0xC0000091u || code == 0xC0000094u;
        break;
    case SIGILL:
        match = code == 0xC000001Du;
        break;
    case SIGABRT:
        match = code == 0xC0000409u || code == 0xC0000602u;
        break;
    default:
        break;
    }
    if (!match) {
        return NULL;
    }
    memset(storage, 0, sizeof *storage);
    storage->ExceptionRecord = &g_veh_record;
    return storage;
}

typedef struct cb {
    char *d;
    size_t cap;
    size_t n;
} cb;

static void cb_init(cb *b, char *data, size_t cap)
{
    b->d = data;
    b->cap = cap;
    b->n = 0;
    if (cap) {
        data[0] = 0;
    }
}

static void cb_addn(cb *b, const char *s, size_t n)
{
    size_t room;
    if (b->n + 1 >= b->cap) {
        return;
    }
    room = b->cap - b->n - 1;
    if (n > room) {
        n = room;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = 0;
}

static void cb_s(cb *b, const char *s)
{
    if (s) {
        cb_addn(b, s, strlen(s));
    }
}

static void cb_c(cb *b, char c)
{
    cb_addn(b, &c, 1);
}

static void cb_u(cb *b, unsigned long long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) {
        cb_c(b, '0');
        return;
    }
    while (v && i < (int)sizeof tmp) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i > 0) {
        cb_c(b, tmp[--i]);
    }
}

static void cb_hex(cb *b, unsigned long long v)
{
    static const char hx[] = "0123456789ABCDEF";
    char tmp[20];
    int i = 0;
    do {
        tmp[i++] = hx[v & 0xF];
        v >>= 4;
    } while (v && i < (int)sizeof tmp);
    cb_s(b, "0x");
    while (i > 0) {
        cb_c(b, tmp[--i]);
    }
}

static void cb_put2(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 10) % 10));
    cb_c(b, (char)('0' + v % 10));
}

static void cb_put3(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 100) % 10));
    cb_put2(b, v);
}

static void cb_put4(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 1000) % 10));
    cb_put3(b, v);
}

/* module+offset, and a symbol name when dbghelp is present */

typedef BOOL (WINAPI *sym_init_fn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *sym_from_addr_fn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef USHORT (WINAPI *rtl_capture_fn)(ULONG, ULONG, PVOID *, PULONG);

static HMODULE g_dbghelp;
static sym_init_fn g_sym_init;
static sym_from_addr_fn g_sym_from_addr;
static rtl_capture_fn g_rtl_capture;
static char g_symbol_buf[sizeof(SYMBOL_INFO) + 1024];

static void crash_load_helpers(void)
{
    FARPROC fp;

    if (!g_dbghelp) {
        HMODULE m = LoadLibraryW(L"dbghelp.dll");
        if (m) {
            fp = GetProcAddress(m, "SymInitialize");
            memcpy(&g_sym_init, &fp, sizeof fp);
            fp = GetProcAddress(m, "SymFromAddr");
            memcpy(&g_sym_from_addr, &fp, sizeof fp);
            if (g_sym_init && g_sym_from_addr) {
                g_sym_init(GetCurrentProcess(), NULL, TRUE);
            }
            g_dbghelp = m;
        }
    }
    if (!g_rtl_capture) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        fp = k ? GetProcAddress(k, "RtlCaptureStackBackTrace") : NULL;
        memcpy(&g_rtl_capture, &fp, sizeof fp);
    }
}

static void crash_module_name(void *addr, char *out, size_t cap)
{
    wchar_t path[CFG_WCHAR_CAP];
    wchar_t *base;
    HMODULE mod = NULL;

    if (cap) {
        out[0] = 0;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(ULONG_PTR)addr, &mod) || !mod) {
        strncpy_s(out, cap, "?", _TRUNCATE);
        return;
    }
    if (!GetModuleFileNameW(mod, path, CFG_WCHAR_CAP)) {
        strncpy_s(out, cap, "?", _TRUNCATE);
        return;
    }
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (!to_utf8(base, out, cap)) {
        strncpy_s(out, cap, "?", _TRUNCATE);
    }
}

static void crash_symbol_line(cb *b, void *addr)
{
    SYMBOL_INFO *si = (SYMBOL_INFO *)g_symbol_buf;
    DWORD64 disp = 0;
    char name[512];

    if (!g_sym_from_addr) {
        return;
    }
    memset(g_symbol_buf, 0, sizeof g_symbol_buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 480;
    if (!g_sym_from_addr(GetCurrentProcess(), (DWORD64)(ULONG_PTR)addr, &disp, si)) {
        return;
    }
    snprintf(name, sizeof name, "  symbol: %s+0x%llx\n",
             si->Name, (unsigned long long)disp);
    cb_s(b, name);
}

static void crash_frame_line(cb *b, void *addr, unsigned frame_no)
{
    char module[260];
    HMODULE mod = NULL;
    unsigned long long off = 0;

    crash_module_name(addr, module, sizeof module);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)addr, &mod) && mod) {
        off = (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)mod);
    }
    cb_s(b, "  #");
    cb_u(b, frame_no);
    cb_s(b, " ");
    cb_hex(b, (unsigned long long)(ULONG_PTR)addr);
    cb_s(b, " ");
    cb_s(b, module);
    cb_s(b, "+");
    cb_hex(b, off);
    cb_c(b, '\n');
    crash_symbol_line(b, addr);
}

static void crash_log_path(wchar_t *out, size_t cap)
{
    if (g_log_path_w[0]) {
        StringCchCopyW(out, cap, g_log_path_w);
        return;
    }
    if (g_log_w[0]) {
        StringCchPrintfW(out, cap, L"%s\\app.log", g_log_w);
        return;
    }
    if (cap) {
        out[0] = 0;
    }
}

static void crash_append_log_tail(cb *b)
{
    static char tail[CRASH_LOG_TAIL_BYTES];
    wchar_t path[CFG_WCHAR_CAP];
    HANDLE h;
    LARGE_INTEGER size;
    DWORD want;
    DWORD got = 0;
    size_t start = 0;
    int lines = 0;
    size_t i;

    cb_s(b, "\n---- app.log (tail) ----\n");
    crash_log_path(path, CFG_WCHAR_CAP);
    if (!path[0]) {
        cb_s(b, "<no log path>\n");
        return;
    }
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        cb_s(b, "<unavailable, win32 error ");
        cb_u(b, GetLastError());
        cb_s(b, ">\n");
        return;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        cb_s(b, "<empty>\n");
        return;
    }
    want = size.QuadPart > (LONGLONG)CRASH_LOG_TAIL_BYTES
         ? (DWORD)CRASH_LOG_TAIL_BYTES : (DWORD)size.QuadPart;
    {
        LARGE_INTEGER off;
        off.QuadPart = size.QuadPart - want;
        if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            return;
        }
    }
    if (!ReadFile(h, tail, want, &got, NULL)) {
        got = 0;
    }
    CloseHandle(h);
    if (!got) {
        return;
    }
    for (i = got; i > 0; --i) {
        if (tail[i - 1] == '\n') {
            lines++;
            if (lines >= 100) {
                start = i;
                break;
            }
        }
    }
    cb_addn(b, tail + start, got - start);
    if (got == 0 || tail[got - 1] != '\n') {
        cb_c(b, '\n');
    }
}

/* RtlGetVersion instead of the deprecated GetVersionExW (lies under a
 * compatibility manifest). Declared locally; ntdll always exports it. */
typedef struct cfg_osvi {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} cfg_osvi;

typedef LONG (WINAPI *rtl_get_version_fn)(cfg_osvi *);

static bool cfg_os_version(cfg_osvi *os)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC fp = ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    rtl_get_version_fn fn = NULL;

    memcpy(&fn, &fp, sizeof fn);
    if (!fn) {
        return false;
    }
    memset(os, 0, sizeof *os);
    os->dwOSVersionInfoSize = sizeof *os;
    return fn(os) == 0;
}

static void crash_system_line(cb *b)
{
    MEMORYSTATUSEX ms;
    SYSTEM_INFO si;
    cfg_osvi os;
    char buf[256];

    if (cfg_os_version(&os)) {
        snprintf(buf, sizeof buf, "Windows %lu.%lu.%lu",
                 (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                 (unsigned long)os.dwBuildNumber);
    } else {
        strncpy_s(buf, sizeof buf, "Windows (unknown version)", _TRUNCATE);
    }
    GetNativeSystemInfo(&si);
    {
        const char *arch = "unknown";
        switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
        default: break;
        }
        cb_s(b, "system: ");
        cb_s(b, buf);
        cb_s(b, " ");
        cb_s(b, arch);
        cb_s(b, ", cpus: ");
        cb_u(b, si.dwNumberOfProcessors);
    }
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        cb_s(b, ", memory: ");
        cb_u(b, ms.ullTotalPhys / (1024ull * 1024ull));
        cb_s(b, " MB");
    }
    cb_c(b, '\n');
}

static void crash_write_report(const char *reason, EXCEPTION_POINTERS *ep)
{
    static char report[CRASH_REPORT_CAP];
    wchar_t path[CFG_WCHAR_CAP];
    SYSTEMTIME st;
    cb b;
    DWORD code = 0;
    void *addr = NULL;
    void *frames[32];
    USHORT frame_count = 0;
    unsigned i;

    if (!g_log_w[0]) {
        return;                 /* nowhere to write before cfg_init */
    }
    crash_load_helpers();

    GetLocalTime(&st);
    cb_init(&b, report, sizeof report);
    cb_s(&b, "AnimeDenoise crash report\n");
    cb_s(&b, "version: " APP_VERSION "\n");
    cb_s(&b, "time: ");
    cb_put4(&b, st.wYear);
    cb_c(&b, '-');
    cb_put2(&b, st.wMonth);
    cb_c(&b, '-');
    cb_put2(&b, st.wDay);
    cb_c(&b, ' ');
    cb_put2(&b, st.wHour);
    cb_c(&b, ':');
    cb_put2(&b, st.wMinute);
    cb_c(&b, ':');
    cb_put2(&b, st.wSecond);
    cb_c(&b, '.');
    cb_put3(&b, st.wMilliseconds);
    cb_s(&b, " (local)\n");
    cb_s(&b, "reason: ");
    cb_s(&b, reason ? reason : "unknown");
    cb_c(&b, '\n');

    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
        cb_s(&b, "exception code: ");
        cb_hex(&b, code);
        if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            cb_s(&b, " ACCESS_VIOLATION (");
            cb_s(&b, op == 0 ? "read" : (op == 1 ? "write" : "execute"));
            cb_s(&b, " at ");
            cb_hex(&b, ep->ExceptionRecord->ExceptionInformation[1]);
            cb_s(&b, ")");
        } else if (code == 0xE06D7363) {
            cb_s(&b, " C++ exception");
        }
        cb_c(&b, '\n');
    } else {
        cb_s(&b, "exception code: n/a (signal/handler)\n");
    }
    cb_s(&b, "address: ");
    cb_hex(&b, (unsigned long long)(ULONG_PTR)addr);
    cb_c(&b, '\n');
    if (addr) {
        crash_frame_line(&b, addr, 0);
    }
    crash_system_line(&b);

    if (g_rtl_capture) {
        frame_count = g_rtl_capture(1, 32, frames, NULL);
    }
    cb_s(&b, "\nbacktrace (RtlCaptureStackBackTrace):\n");
    for (i = 0; i < frame_count; ++i) {
        crash_frame_line(&b, frames[i], i);
    }
    if (!frame_count) {
        cb_s(&b, "  <unavailable>\n");
    }
    crash_append_log_tail(&b);

    if (FAILED(StringCchPrintfW(path, CFG_WCHAR_CAP,
                                L"%s\\crash-%04u%02u%02u-%02u%02u%02u.txt",
                                g_log_w, (unsigned)st.wYear, (unsigned)st.wMonth,
                                (unsigned)st.wDay, (unsigned)st.wHour,
                                (unsigned)st.wMinute, (unsigned)st.wSecond))) {
        return;
    }
    write_file_w(path, report, b.n);
}

static LONG WINAPI cfg_unhandled_filter(EXCEPTION_POINTERS *ep)
{
    DWORD code;

    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0xC0000005u;
    crash_write_report("unhandled exception", ep);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int cfg_signal_code(int sig)
{
    switch (sig) {
    case SIGABRT: return 0xC0000409;   /* STATUS_STACK_BUFFER_OVERRUN-ish abort */
    case SIGSEGV: return 0xC0000005;
    case SIGFPE:  return 0xC0000094;
    case SIGILL:  return 0xC000001D;
    default:      return 0xC0000005;
    }
}

static void crash_signal_handler(int sig)
{
    EXCEPTION_POINTERS storage;
    EXCEPTION_POINTERS *ep = cfg_signal_exception(sig, &storage);
    int code;

    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    code = cfg_signal_code(sig);
    crash_write_report(sig == SIGABRT ? "abort (signal SIGABRT)" :
                       sig == SIGSEGV ? "segmentation fault (SIGSEGV)" :
                       sig == SIGFPE  ? "arithmetic fault (SIGFPE)" :
                       sig == SIGILL  ? "illegal instruction (SIGILL)" :
                                        "fatal signal", ep);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

static void crash_purecall_handler(void)
{
    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    crash_write_report("pure virtual function call", NULL);
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
}

static void crash_invalid_parameter_handler(const wchar_t *expression,
                                            const wchar_t *function,
                                            const wchar_t *file,
                                            unsigned int line,
                                            uintptr_t reserved)
{
    (void)expression;
    (void)function;
    (void)file;
    (void)line;
    (void)reserved;
    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    crash_write_report("invalid C runtime parameter", NULL);
    TerminateProcess(GetCurrentProcess(), 0xC000000D);
}

void log_install_crash_handler(void)
{
    if (InterlockedCompareExchange(&g_veh_installed, 1, 0) == 0) {
        AddVectoredExceptionHandler(1, cfg_veh_capture);
    }
    SetUnhandledExceptionFilter(cfg_unhandled_filter);
    _set_purecall_handler(crash_purecall_handler);
    _set_invalid_parameter_handler(crash_invalid_parameter_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGFPE, crash_signal_handler);
    signal(SIGILL, crash_signal_handler);
}

/* ----------------------------------------------------------- diagnostics */

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
static char *redact_alloc(const char *data, size_t len, size_t *out_len)
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

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t crc32_calc(const unsigned char *data, size_t len)
{
    uint32_t table[256];
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    unsigned j;

    for (i = 0; i < 256; ++i) {
        uint32_t c = (uint32_t)i;
        for (j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    for (i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct zip_entry {
    char name[160];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
    uint16_t dos_date;
    uint16_t dos_time;
} zip_entry;

typedef struct zip_state {
    HANDLE h;
    uint32_t offset;
    int count;
    zip_entry entries[DIAG_MAX_ENTRIES];
} zip_state;

static bool write_all(HANDLE h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        DWORD chunk = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, NULL) || written != chunk) {
            return false;
        }
        p += chunk;
        len -= chunk;
    }
    return true;
}

static void dos_datetime(uint16_t *dos_date, uint16_t *dos_time)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *dos_date = (uint16_t)((((unsigned)st.wYear - 1980) << 9) |
                           ((unsigned)st.wMonth << 5) | (unsigned)st.wDay);
    *dos_time = (uint16_t)(((unsigned)st.wHour << 11) |
                           ((unsigned)st.wMinute << 5) | ((unsigned)st.wSecond / 2));
}

static bool zip_begin(zip_state *z, const wchar_t *path)
{
    memset(z, 0, sizeof *z);
    z->h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    return z->h != INVALID_HANDLE_VALUE;
}

static bool zip_add(zip_state *z, const char *name, const void *data, size_t len)
{
    unsigned char hdr[30];
    size_t nlen;
    zip_entry *e;

    if (!z->h || z->count >= DIAG_MAX_ENTRIES || len > 0x7FFFFFFFu) {
        return false;
    }
    nlen = strlen(name);
    if (nlen == 0 || nlen > sizeof z->entries[0].name - 1) {
        return false;
    }
    e = &z->entries[z->count];
    memset(e, 0, sizeof *e);
    memcpy(e->name, name, nlen + 1);
    e->crc = crc32_calc((const unsigned char *)data, len);
    e->size = (uint32_t)len;
    dos_datetime(&e->dos_date, &e->dos_time);
    e->offset = z->offset;

    put32(hdr, 0x04034b50u);
    put16(hdr + 4, 20);
    put16(hdr + 6, 0x0800);      /* UTF-8 names */
    put16(hdr + 8, 0);           /* store */
    put16(hdr + 10, e->dos_time);
    put16(hdr + 12, e->dos_date);
    put32(hdr + 14, e->crc);
    put32(hdr + 18, e->size);
    put32(hdr + 22, e->size);
    put16(hdr + 26, (unsigned)nlen);
    put16(hdr + 28, 0);
    if (!write_all(z->h, hdr, sizeof hdr) ||
        !write_all(z->h, name, nlen) ||
        !write_all(z->h, data, len)) {
        return false;
    }
    z->offset += (uint32_t)(sizeof hdr + nlen + len);
    z->count++;
    return true;
}

static bool zip_end(zip_state *z)
{
    unsigned char hdr[46];
    unsigned char eocd[22];
    uint32_t cd_start = z->offset;
    int i;

    for (i = 0; i < z->count; ++i) {
        const zip_entry *e = &z->entries[i];
        size_t nlen = strlen(e->name);
        put32(hdr, 0x02014b50u);
        put16(hdr + 4, 20);
        put16(hdr + 6, 20);
        put16(hdr + 8, 0x0800);
        put16(hdr + 10, 0);
        put16(hdr + 12, e->dos_time);
        put16(hdr + 14, e->dos_date);
        put32(hdr + 16, e->crc);
        put32(hdr + 20, e->size);
        put32(hdr + 24, e->size);
        put16(hdr + 28, (unsigned)nlen);
        put16(hdr + 30, 0);
        put16(hdr + 32, 0);
        put16(hdr + 34, 0);
        put16(hdr + 36, 0);
        put32(hdr + 38, 0);
        put32(hdr + 42, e->offset);
        if (!write_all(z->h, hdr, sizeof hdr) || !write_all(z->h, e->name, nlen)) {
            return false;
        }
        z->offset += (uint32_t)(sizeof hdr + nlen);
    }
    put32(eocd, 0x06054b50u);
    put16(eocd + 4, 0);
    put16(eocd + 6, 0);
    put16(eocd + 8, (unsigned)z->count);
    put16(eocd + 10, (unsigned)z->count);
    put32(eocd + 12, z->offset - cd_start);
    put32(eocd + 16, cd_start);
    put16(eocd + 20, 0);
    return write_all(z->h, eocd, sizeof eocd);
}

static void build_system_info(sb *b)
{
    SYSTEMTIME st;
    cfg_osvi os;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    wchar_t cpu[256];
    DWORD cpu_len = (DWORD)sizeof cpu;
    char buf[512];
    const char *arch = "unknown";
    unsigned i;

    GetLocalTime(&st);
    sb_addf(b, "AnimeDenoise diagnostics\n");
    sb_addf(b, "version: %s\n", APP_VERSION);
    sb_addf(b, "time: %04u-%02u-%02u %02u:%02u:%02u (local)\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);

    if (cfg_os_version(&os)) {
        sb_addf(b, "os: Windows %lu.%lu build %lu\n",
                (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                (unsigned long)os.dwBuildNumber);
    } else {
        sb_adds(b, "os: Windows (unknown version)\n");
    }

    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
    default: break;
    }
    sb_addf(b, "arch: %s, logical cpus: %lu\n", arch,
            (unsigned long)si.dwNumberOfProcessors);

    cpu[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &cpu_len) != ERROR_SUCCESS) {
        cpu[0] = 0;
    }
    if (cpu[0] && to_utf8(cpu, buf, sizeof buf)) {
        sb_addf(b, "cpu: %s\n", buf);
    }

    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        sb_addf(b, "memory: %llu MB total, %llu MB available\n",
                (unsigned long long)(ms.ullTotalPhys / (1024ull * 1024ull)),
                (unsigned long long)(ms.ullAvailPhys / (1024ull * 1024ull)));
    }

    sb_adds(b, "gpu:");
    for (i = 0; i < 8; ++i) {
        DISPLAY_DEVICEW dd;
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) {
            break;
        }
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            if (to_utf8(dd.DeviceString, buf, sizeof buf)) {
                sb_addf(b, " %s;", buf);
            }
        }
    }
    sb_adds(b, "\n");
    sb_addf(b, "data dir: %s\n", g_data_u8[0] ? "<data>" : "(unset)");
}

static bool diag_add_file(zip_state *z, const wchar_t *path, const char *name)
{
    char *buf = NULL;
    char *red;
    size_t len = 0;
    size_t rlen = 0;
    bool ok;

    if (!read_file_w(path, &buf, &len, DIAG_MAX_FILE_BYTES)) {
        return false;
    }
    red = redact_alloc(buf, len, &rlen);
    if (red) {
        ok = zip_add(z, name, red, rlen);
        free(red);
    } else {
        ok = zip_add(z, name, buf, len);
    }
    free(buf);
    return ok;
}

static bool has_zip_extension(const wchar_t *name)
{
    size_t n = wcslen(name);
    if (n < 5) {
        return false;
    }
    return _wcsicmp(name + n - 4, L".zip") == 0;
}

bool log_export_diagnostics(char *out_path, size_t cap)
{
    sb info;
    wchar_t info_path[CFG_WCHAR_CAP];
    wchar_t zip_path[CFG_WCHAR_CAP];
    wchar_t pattern[CFG_WCHAR_CAP];
    const wchar_t *zip_base;
    bool zip_ok = true;
    bool any = false;
    zip_state z;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    SYSTEMTIME st;

    if (!out_path || cap == 0) {
        return false;
    }
    out_path[0] = 0;
    if (!g_ready || !g_log_w[0]) {
        return false;
    }

    memset(&info, 0, sizeof info);
    build_system_info(&info);
    if (info.oom) {
        sb_free(&info);
        return false;
    }
    if (FAILED(StringCchPrintfW(info_path, CFG_WCHAR_CAP, L"%s\\system-info.txt", g_log_w))) {
        sb_free(&info);
        return false;
    }
    if (!write_file_w(info_path, info.p ? info.p : "", info.len)) {
        sb_free(&info);
        return false;
    }
    sb_free(&info);

    GetLocalTime(&st);
    if (FAILED(StringCchPrintfW(zip_path, CFG_WCHAR_CAP,
                                L"%s\\diagnostics-%04u%02u%02u-%02u%02u%02u.zip",
                                g_log_w, (unsigned)st.wYear, (unsigned)st.wMonth,
                                (unsigned)st.wDay, (unsigned)st.wHour,
                                (unsigned)st.wMinute, (unsigned)st.wSecond))) {
        return false;
    }
    zip_base = wcsrchr(zip_path, L'\\');
    zip_base = zip_base ? zip_base + 1 : zip_path;

    if (!zip_begin(&z, zip_path)) {
        return false;
    }

    /* settings.json first so a zip that opened correctly always has it. */
    if (g_settings_w[0] && GetFileAttributesW(g_settings_w) != INVALID_FILE_ATTRIBUTES) {
        if (diag_add_file(&z, g_settings_w, "settings.json")) {
            any = true;
        }
    }
    if (diag_add_file(&z, info_path, "system-info.txt")) {
        any = true;
    }

    if (FAILED(StringCchPrintfW(pattern, CFG_WCHAR_CAP, L"%s\\*", g_log_w))) {
        zip_ok = false;
    } else {
        find = FindFirstFileW(pattern, &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                wchar_t full[CFG_WCHAR_CAP];
                char name_u8[512];
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                if (_wcsicmp(fd.cFileName, zip_base) == 0) {
                    continue;
                }
                if (_wcsicmp(fd.cFileName, L"system-info.txt") == 0) {
                    continue;   /* already added above */
                }
                if (has_zip_extension(fd.cFileName)) {
                    continue;
                }
                if (!to_utf8(fd.cFileName, name_u8, sizeof name_u8)) {
                    continue;
                }
                if (!path_join(full, CFG_WCHAR_CAP, g_log_w, fd.cFileName)) {
                    continue;
                }
                if (diag_add_file(&z, full, name_u8)) {
                    any = true;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    if (!zip_ok || !zip_end(&z) || !any) {
        CloseHandle(z.h);
        DeleteFileW(zip_path);
        return false;
    }
    CloseHandle(z.h);
    if (!to_utf8(zip_path, out_path, cap)) {
        out_path[0] = 0;
        return false;
    }
    return true;
}

/* --------------------------------------------------------- init/shutdown */

const char *cfg_data_dir(void)
{
    return g_data_u8;
}

const char *cfg_log_dir(void)
{
    return g_log_u8;
}

const char *cfg_runtime_dir(void)
{
    return g_runtime_u8;
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

    if (g_ready) {
        return true;
    }

    memset(g_data_w, 0, sizeof g_data_w);
    g_log_w[0] = g_projects_w[0] = g_runtime_w[0] = 0;
    g_updates_w[0] = g_exe_w[0] = g_out_w[0] = g_settings_w[0] = 0;
    g_profile_w[0] = 0;
    g_data_u8[0] = g_log_u8[0] = g_runtime_u8[0] = 0;
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
        path_join(g_data_w, CFG_WCHAR_CAP, local, L"AnimeDenoise");
        CoTaskMemFree(local);
    }

    if (!ensure_dir_w(g_data_w)) {
        return false;
    }
    path_join(g_log_w, CFG_WCHAR_CAP, g_data_w, L"logs");
    path_join(g_projects_w, CFG_WCHAR_CAP, g_data_w, L"projects");
    path_join(g_runtime_w, CFG_WCHAR_CAP, g_data_w, L"runtime");
    path_join(g_updates_w, CFG_WCHAR_CAP, g_data_w, L"updates");
    ensure_dir_w(g_log_w);
    ensure_dir_w(g_projects_w);
    ensure_dir_w(g_runtime_w);
    ensure_dir_w(g_updates_w);
    path_join(g_settings_w, CFG_WCHAR_CAP, g_data_w, L"settings.json");

    /* output dir: prefer <exe>\outputs, fall back to the data dir. */
    cand[0] = 0;
    {
        bool use_exe = false;
        if (g_exe_w[0]) {
            wchar_t outs[CFG_WCHAR_CAP];
            DWORD attr;
            if (path_join(outs, CFG_WCHAR_CAP, g_exe_w, L"outputs")) {
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
            path_join(cand, CFG_WCHAR_CAP, g_exe_w, L"outputs");
        } else {
            path_join(cand, CFG_WCHAR_CAP, g_data_w, L"outputs");
            ensure_dir_w(cand);
        }
    }
    StringCchCopyW(g_out_w, CFG_WCHAR_CAP, cand);

    to_utf8(g_data_w, g_data_u8, CFG_PATH_CAP);
    to_utf8(g_log_w, g_log_u8, CFG_PATH_CAP);
    to_utf8(g_runtime_w, g_runtime_u8, CFG_PATH_CAP);
    to_utf8(g_exe_w, g_exe_u8, CFG_PATH_CAP);
    to_utf8(g_out_w, g_out_u8, CFG_PATH_CAP);
    to_utf8(g_profile_w, g_profile_u8, CFG_PATH_CAP);

    g_ready = true;
    log_open();
    log_info("cfg: data dir %s", g_data_u8);
    log_info("cfg: exe dir %s, output dir %s", g_exe_u8, g_out_u8);
    return true;
}

void cfg_shutdown(void)
{
    log_close();
    g_ready = false;
    g_data_w[0] = g_log_w[0] = g_projects_w[0] = g_runtime_w[0] = 0;
    g_updates_w[0] = g_exe_w[0] = g_out_w[0] = g_settings_w[0] = g_profile_w[0] = 0;
    g_data_u8[0] = g_log_u8[0] = g_runtime_u8[0] = 0;
    g_exe_u8[0] = g_out_u8[0] = g_profile_u8[0] = 0;
}
