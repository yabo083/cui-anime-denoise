/* Self-update: WinHTTP manifest fetch, version compare, verified installer
 * download, then hand-off to Inno Setup.
 *
 * Everything that can block (DNS, proxy resolution, TLS, HTTP, disk) happens
 * on one worker thread. The UI only ever touches the protected snapshot in
 * upd_get() and the two non-blocking starters upd_check()/upd_download().
 *
 * State machine:
 *
 *   IDLE --upd_check--> CHECKING --+--> UP_TO_DATE
 *                                  +--> AVAILABLE --upd_download--> DOWNLOADING
 *                                  +--> FAILED                    --+--> READY
 *                                                                  +--> FAILED
 *
 * Downloaded bytes go to update.bin.tmp and are only renamed to update.bin
 * after the size and SHA-256 match the manifest. update.json (the metadata)
 * is written last, so a snapshot of the directory is always consistent.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <process.h>

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"
#include "config.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_QUERY_FLAG_NUMBER64
#define WINHTTP_QUERY_FLAG_NUMBER64 0x10000000
#endif

#define UPD_TIMEOUT_MS       20000u
#define UPD_MANIFEST_MAX     (1024u * 1024u)   /* 1 MiB */
#define UPD_MIN_INTERVAL_MS  (6ull * 60ull * 60ull * 1000ull)
#define UPD_IO_CHUNK         65536u
#define UPD_PATH_CAP         CFG_PATH_CAP
#define UPD_WPATH_CAP        (CFG_PATH_CAP * 2)

#define UPD_OP_CHECK    1
#define UPD_OP_DOWNLOAD 2

/* ------------------------------------------------------------------------ */
/* Shared state                                                             */
/* ------------------------------------------------------------------------ */

static CRITICAL_SECTION g_lock;
static bool g_lock_ready;

static HANDLE g_thread;               /* current worker, NULL when none */
static volatile LONG g_stop;          /* set by upd_shutdown */

static upd_info g_info;               /* guarded by g_lock */
static char g_manifest_url[UPD_PATH_CAP];
static char g_download_url[1024];
static char g_sha256[80];             /* lower-case hex expected hash */
static unsigned long long g_expected_size;
static bool g_have_manifest;          /* g_download_url/g_sha256 are valid */
static ULONGLONG g_last_check;        /* tick of the last check start */
static bool g_inited;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static bool upd_stop_requested(void) {
    return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

static bool to_wide(const char *s, wchar_t *out, size_t cap) {
    int n;
    if (!s || !out || cap == 0 || cap > (size_t)INT_MAX) return false;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap);
    return n > 0;
}

static bool path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t la;
    int n;
    if (!out || !a || !b || cap == 0) return false;
    la = strlen(a);
    while (la > 0 && (a[la - 1] == '\\' || a[la - 1] == '/')) la--;
    n = snprintf(out, cap, "%.*s\\%s", (int)la, a, b);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_dir_w(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Builds <data>\updates\<name> and, for `updates_dir`, the directory itself. */
static bool updates_path(char *out, size_t cap, const char *name) {
    char dir[UPD_PATH_CAP];
    const char *data = cfg_data_dir();
    if (!data || !data[0]) return false;
    if (!path_join(dir, sizeof dir, data, "updates")) return false;
    if (!name || !name[0]) {
        int n = snprintf(out, cap, "%s", dir);
        return n > 0 && (size_t)n < cap;
    }
    return path_join(out, cap, dir, name);
}

static bool ensure_updates_dir(void) {
    char dir[UPD_PATH_CAP];
    wchar_t wdir[UPD_WPATH_CAP];
    if (!updates_path(dir, sizeof dir, NULL)) return false;
    if (!to_wide(dir, wdir, UPD_WPATH_CAP)) return false;
    return ensure_dir_w(wdir);
}

/* ------------------------------------------------------------------------ */
/* SHA-256 (BCrypt)                                                         */
/* ------------------------------------------------------------------------ */

typedef struct hash_ctx {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    PUCHAR object;
} hash_ctx;

static void hash_free(hash_ctx *h) {
    if (h->hash) BCryptDestroyHash(h->hash);
    if (h->object) HeapFree(GetProcessHeap(), 0, h->object);
    if (h->alg) BCryptCloseAlgorithmProvider(h->alg, 0);
    memset(h, 0, sizeof *h);
}

static bool hash_init(hash_ctx *h) {
    DWORD object_len = 0, got = 0;
    memset(h, 0, sizeof *h);
    if (BCryptOpenAlgorithmProvider(&h->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        h->alg = NULL;
        return false;
    }
    if (BCryptGetProperty(h->alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                          sizeof object_len, &got, 0) < 0 ||
        object_len == 0) {
        hash_free(h);
        return false;
    }
    h->object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!h->object) {
        hash_free(h);
        return false;
    }
    if (BCryptCreateHash(h->alg, &h->hash, h->object, object_len, NULL, 0, 0) < 0) {
        hash_free(h);
        return false;
    }
    return true;
}

static bool hash_update(hash_ctx *h, const void *data, size_t len) {
    if (len == 0) return true;
    return BCryptHashData(h->hash, (PUCHAR)(const void *)data, (ULONG)len, 0) >= 0;
}

static bool hash_finish(hash_ctx *h, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    int i;
    if (BCryptFinishHash(h->hash, digest, (ULONG)sizeof digest, 0) < 0) return false;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = 0;
    return true;
}

static bool file_sha256_w(const wchar_t *path, char out[65],
                          unsigned long long *size_out) {
    HANDLE f;
    hash_ctx h;
    unsigned char buf[UPD_IO_CHUNK];
    DWORD got = 0;
    unsigned long long total = 0;
    bool ok = true;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (!hash_init(&h)) {
        CloseHandle(f);
        return false;
    }
    for (;;) {
        if (!ReadFile(f, buf, (DWORD)sizeof buf, &got, NULL)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!hash_update(&h, buf, got)) {
            ok = false;
            break;
        }
        total += got;
    }
    if (ok) ok = hash_finish(&h, out);
    hash_free(&h);
    CloseHandle(f);
    if (ok && size_out) *size_out = total;
    return ok;
}

static bool hex_equal(const char *a, const char *b) {
    size_t i;
    if (!a || !b) return false;
    if (strlen(a) != strlen(b)) return false;
    for (i = 0; a[i]; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool sha256_looks_valid(const char *s) {
    size_t i;
    if (!s) return false;
    for (i = 0; s[i]; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return i == 64;
}

static void sha256_lower(char *s) {
    size_t i;
    for (i = 0; s && s[i]; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

/* ------------------------------------------------------------------------ */
/* Minimal JSON reader                                                      */
/* ------------------------------------------------------------------------ */

typedef struct json_reader {
    const char *p;
    const char *end;
} json_reader;

typedef struct manifest {
    char version[32];
    char url[1024];
    char sha256[80];
    char notes[4096];
    char page[512];
    unsigned long long size;
} manifest;

static void jr_ws(json_reader *r) {
    while (r->p < r->end) {
        char c = *r->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r->p++;
        else break;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Appends one code point as UTF-8; silently drops it when it does not fit. */
static void utf8_append(char *out, size_t cap, size_t *len, unsigned cp) {
    char tmp[4];
    size_t n;
    size_t i;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    for (i = 0; i < n; i++) {
        if (out && *len + 1 < cap) out[*len] = tmp[i];
        if (out && *len + 1 < cap) (*len)++;
    }
}

static bool jr_hex4(json_reader *r, unsigned *out) {
    unsigned v = 0;
    int i;
    if (r->end - r->p < 4) return false;
    for (i = 0; i < 4; i++) {
        int h = hex_value(r->p[i]);
        if (h < 0) return false;
        v = (v << 4) | (unsigned)h;
    }
    r->p += 4;
    *out = v;
    return true;
}

/* Parses a JSON string. `out` may be NULL to validate and skip. */
static bool jr_string(json_reader *r, char *out, size_t cap) {
    size_t len = 0;
    if (r->p >= r->end || *r->p != '"') return false;
    r->p++;
    if (out && cap) out[0] = 0;
    while (r->p < r->end) {
        unsigned char c = (unsigned char)*r->p++;
        if (c == '"') {
            if (out && cap) out[len < cap ? len : cap - 1] = 0;
            return true;
        }
        if (c == '\\') {
            char esc;
            if (r->p >= r->end) return false;
            esc = *r->p++;
            switch (esc) {
            case '"': utf8_append(out, cap, &len, '"'); break;
            case '\\': utf8_append(out, cap, &len, '\\'); break;
            case '/': utf8_append(out, cap, &len, '/'); break;
            case 'b': utf8_append(out, cap, &len, '\b'); break;
            case 'f': utf8_append(out, cap, &len, '\f'); break;
            case 'n': utf8_append(out, cap, &len, '\n'); break;
            case 'r': utf8_append(out, cap, &len, '\r'); break;
            case 't': utf8_append(out, cap, &len, '\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!jr_hex4(r, &cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                        r->p += 2;
                        if (!jr_hex4(r, &lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        } else {
                            cp = 0xFFFDu;
                        }
                    } else {
                        cp = 0xFFFDu;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFDu;
                }
                utf8_append(out, cap, &len, cp);
                break;
            }
            default:
                return false;
            }
        } else {
            if (out && len + 1 < cap) out[len] = (char)c;
            if (out && len + 1 < cap) len++;
        }
    }
    return false;
}

static bool jr_uint64(json_reader *r, unsigned long long *out) {
    unsigned long long v = 0;
    if (r->p >= r->end || !isdigit((unsigned char)*r->p)) return false;
    while (r->p < r->end && isdigit((unsigned char)*r->p)) {
        unsigned d = (unsigned)(*r->p - '0');
        if (v > (ULLONG_MAX - d) / 10ull) return false;
        v = v * 10ull + d;
        r->p++;
    }
    *out = v;
    return true;
}

/* Skips one JSON value of any type. Depth-limited to keep the stack sane. */
static bool jr_skip(json_reader *r, int depth) {
    char c;
    if (depth > 32) return false;
    jr_ws(r);
    if (r->p >= r->end) return false;
    c = *r->p;
    if (c == '"') return jr_string(r, NULL, 0);
    if (c == '{') {
        r->p++;
        jr_ws(r);
        if (r->p < r->end && *r->p == '}') {
            r->p++;
            return true;
        }
        for (;;) {
            jr_ws(r);
            if (!jr_string(r, NULL, 0)) return false;
            jr_ws(r);
            if (r->p >= r->end || *r->p != ':') return false;
            r->p++;
            if (!jr_skip(r, depth + 1)) return false;
            jr_ws(r);
            if (r->p >= r->end) return false;
            if (*r->p == ',') {
                r->p++;
                continue;
            }
            if (*r->p == '}') {
                r->p++;
                return true;
            }
            return false;
        }
    }
    if (c == '[') {
        r->p++;
        jr_ws(r);
        if (r->p < r->end && *r->p == ']') {
            r->p++;
            return true;
        }
        for (;;) {
            if (!jr_skip(r, depth + 1)) return false;
            jr_ws(r);
            if (r->p >= r->end) return false;
            if (*r->p == ',') {
                r->p++;
                continue;
            }
            if (*r->p == ']') {
                r->p++;
                return true;
            }
            return false;
        }
    }
    if (c == 't') {
        if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) {
            r->p += 4;
            return true;
        }
        return false;
    }
    if (c == 'f') {
        if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) {
            r->p += 5;
            return true;
        }
        return false;
    }
    if (c == 'n') {
        if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) {
            r->p += 4;
            return true;
        }
        return false;
    }
    /* number: digits, '-', '.', 'e', 'E' (validated loosely) */
    if (c == '-' || isdigit((unsigned char)c)) {
        r->p++;
        while (r->p < r->end) {
            char n = *r->p;
            if (isdigit((unsigned char)n) || n == '.' || n == 'e' || n == 'E' ||
                n == '+' || n == '-') {
                r->p++;
            } else {
                break;
            }
        }
        return true;
    }
    return false;
}

/* Extracts the six manifest fields from a flat JSON object. Unknown keys are
 * skipped, so extra metadata never breaks an older client. */
static bool parse_manifest(const char *json, size_t len, manifest *m, char *err,
                           size_t errcap) {
    json_reader r;
    bool ok = true;

    memset(m, 0, sizeof *m);
    r.p = json;
    r.end = json + len;
    jr_ws(&r);
    if (r.p >= r.end || *r.p != '{') {
        snprintf(err, errcap, "manifest: not a JSON object");
        return false;
    }
    r.p++;
    jr_ws(&r);
    if (r.p < r.end && *r.p == '}') {
        r.p++;
    } else {
        for (;;) {
            char key[32];
            jr_ws(&r);
            if (!jr_string(&r, key, sizeof key)) {
                ok = false;
                break;
            }
            jr_ws(&r);
            if (r.p >= r.end || *r.p != ':') {
                ok = false;
                break;
            }
            r.p++;
            jr_ws(&r);
            if (strcmp(key, "version") == 0) {
                ok = jr_string(&r, m->version, sizeof m->version);
            } else if (strcmp(key, "url") == 0) {
                ok = jr_string(&r, m->url, sizeof m->url);
            } else if (strcmp(key, "sha256") == 0) {
                ok = jr_string(&r, m->sha256, sizeof m->sha256);
            } else if (strcmp(key, "notes") == 0) {
                ok = jr_string(&r, m->notes, sizeof m->notes);
            } else if (strcmp(key, "page") == 0) {
                ok = jr_string(&r, m->page, sizeof m->page);
            } else if (strcmp(key, "size") == 0) {
                ok = jr_uint64(&r, &m->size);
            } else {
                ok = jr_skip(&r, 0);
            }
            if (!ok) break;
            jr_ws(&r);
            if (r.p >= r.end) {
                ok = false;
                break;
            }
            if (*r.p == ',') {
                r.p++;
                continue;
            }
            if (*r.p == '}') {
                r.p++;
                break;
            }
            ok = false;
            break;
        }
    }
    if (!ok) {
        snprintf(err, errcap, "manifest: invalid JSON near byte %zu",
                 (size_t)(r.p - json));
        return false;
    }
    if (m->version[0] == 0) {
        snprintf(err, errcap, "manifest: missing \"version\"");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Version compare (x.y.z numeric, missing segments count as 0)             */
/* ------------------------------------------------------------------------ */

static int version_segment(const char **p) {
    const char *s = *p;
    unsigned long v = 0;
    bool any = false;
    while (*s && !isdigit((unsigned char)*s)) s++;
    while (*s && isdigit((unsigned char)*s)) {
        any = true;
        if (v < 100000000ul) v = v * 10ul + (unsigned long)(*s - '0');
        s++;
    }
    *p = s;
    return any ? (int)v : 0;
}

/* -1 when a < b, 0 when equal, 1 when a > b. */
static int version_cmp(const char *a, const char *b) {
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";
    int i;
    for (i = 0; i < 4; i++) {
        int va = version_segment(&pa);
        int vb = version_segment(&pb);
        if (va != vb) return va < vb ? -1 : 1;
        if (!*pa && !*pb) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* WinHTTP                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct http_req {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
} http_req;

static void http_close(http_req *h) {
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    memset(h, 0, sizeof *h);
}

static void free_ie_proxy_config(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *ie) {
    if (ie->lpszAutoConfigUrl) GlobalFree(ie->lpszAutoConfigUrl);
    if (ie->lpszProxy) GlobalFree(ie->lpszProxy);
    if (ie->lpszProxyBypass) GlobalFree(ie->lpszProxyBypass);
    memset(ie, 0, sizeof *ie);
}

/* Applies the current user's IE/WinINET proxy settings: an explicit proxy, a
 * PAC script, or WPAD auto-detection. The PAC lookup is resolved for the
 * actual request URL, which is what makes per-host proxy rules work. */
static void apply_system_proxy(HINTERNET session, HINTERNET request,
                               const wchar_t *url) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie;
    WINHTTP_AUTOPROXY_OPTIONS opts;
    BOOL have;

    ZeroMemory(&ie, sizeof ie);
    have = WinHttpGetIEProxyConfigForCurrentUser(&ie);
    if (!have) return;

    ZeroMemory(&opts, sizeof opts);
    if (ie.lpszAutoConfigUrl) {
        opts.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        opts.lpszAutoConfigUrl = ie.lpszAutoConfigUrl;
        opts.fAutoLogonIfChallenged = TRUE;
    } else if (ie.fAutoDetect) {
        opts.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
        opts.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        opts.fAutoLogonIfChallenged = FALSE;
    } else if (ie.lpszProxy) {
        WINHTTP_PROXY_INFO manual;
        ZeroMemory(&manual, sizeof manual);
        manual.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        manual.lpszProxy = ie.lpszProxy;
        manual.lpszProxyBypass = ie.lpszProxyBypass;
        WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &manual, sizeof manual);
        free_ie_proxy_config(&ie);
        return;
    } else {
        free_ie_proxy_config(&ie);
        return;
    }

    {
        WINHTTP_PROXY_INFO info;
        ZeroMemory(&info, sizeof info);
        if (WinHttpGetProxyForUrl(session, url, &opts, &info)) {
            /* The session is created per request, so setting the resolved
             * proxy on it is equivalent to a per-request option. */
            if (!WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &info, sizeof info)) {
                WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &info, sizeof info);
            }
            if (info.lpszProxy) GlobalFree(info.lpszProxy);
            if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
        } else if (ie.lpszProxy) {
            WINHTTP_PROXY_INFO manual;
            ZeroMemory(&manual, sizeof manual);
            manual.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            manual.lpszProxy = ie.lpszProxy;
            manual.lpszProxyBypass = ie.lpszProxyBypass;
            WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &manual, sizeof manual);
        }
    }
    free_ie_proxy_config(&ie);
}

static void set_error(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Opens a GET request for `url_utf8`. On success the caller must http_close(). */
static bool http_open(http_req *h, const char *url_utf8, char *err,
                      size_t errcap) {
    wchar_t wurl[2048];
    wchar_t host[256];
    wchar_t path[2048];
    URL_COMPONENTS uc;
    size_t plen = 0;
    bool secure;

    memset(h, 0, sizeof *h);
    if (!to_wide(url_utf8, wurl, sizeof wurl / sizeof wurl[0])) {
        set_error(err, errcap, "invalid URL (not UTF-8 or too long)");
        return false;
    }
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.lpszHostName ||
        uc.dwHostNameLength == 0 ||
        uc.dwHostNameLength >= sizeof host / sizeof host[0]) {
        set_error(err, errcap, "invalid URL");
        return false;
    }
    memcpy(host, uc.lpszHostName, (size_t)uc.dwHostNameLength * sizeof(wchar_t));
    host[uc.dwHostNameLength] = 0;

    if (uc.lpszUrlPath && uc.dwUrlPathLength) {
        if (uc.dwUrlPathLength >= sizeof path / sizeof path[0]) {
            set_error(err, errcap, "URL path too long");
            return false;
        }
        memcpy(path, uc.lpszUrlPath, (size_t)uc.dwUrlPathLength * sizeof(wchar_t));
        plen = uc.dwUrlPathLength;
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) {
        if (plen + uc.dwExtraInfoLength >= sizeof path / sizeof path[0]) {
            set_error(err, errcap, "URL path too long");
            return false;
        }
        memcpy(path + plen, uc.lpszExtraInfo,
               (size_t)uc.dwExtraInfoLength * sizeof(wchar_t));
        plen += uc.dwExtraInfoLength;
    }
    if (plen == 0) {
        path[0] = L'/';
        plen = 1;
    }
    path[plen] = 0;
    secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    h->session = WinHttpOpen(L"AnimeDenoise-Updater/1.0",
                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) {
        set_error(err, errcap, "WinHttpOpen failed (error %lu)", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(h->session, (int)UPD_TIMEOUT_MS, (int)UPD_TIMEOUT_MS,
                       (int)UPD_TIMEOUT_MS, (int)UPD_TIMEOUT_MS);

    h->connect = WinHttpConnect(h->session, host, (INTERNET_PORT)uc.nPort, 0);
    if (!h->connect) {
        set_error(err, errcap, "WinHttpConnect failed (error %lu)", GetLastError());
        http_close(h);
        return false;
    }
    h->request = WinHttpOpenRequest(h->connect, L"GET", path, NULL,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h->request) {
        set_error(err, errcap, "WinHttpOpenRequest failed (error %lu)", GetLastError());
        http_close(h);
        return false;
    }
    apply_system_proxy(h->session, h->request, wurl);

    if (!WinHttpSendRequest(h->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        set_error(err, errcap, "WinHttpSendRequest failed (error %lu)", GetLastError());
        http_close(h);
        return false;
    }
    if (!WinHttpReceiveResponse(h->request, NULL)) {
        set_error(err, errcap, "WinHttpReceiveResponse failed (error %lu)", GetLastError());
        http_close(h);
        return false;
    }
    {
        DWORD status = 0, sz = sizeof status;
        if (!WinHttpQueryHeaders(h->request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                 WINHTTP_NO_HEADER_INDEX)) {
            set_error(err, errcap, "cannot read HTTP status (error %lu)", GetLastError());
            http_close(h);
            return false;
        }
        if (status != 200) {
            set_error(err, errcap, "HTTP %lu", (unsigned long)status);
            http_close(h);
            return false;
        }
    }
    return true;
}

static unsigned long long http_content_length(HINTERNET request) {
    unsigned long long clen = 0;
    DWORD sz = sizeof clen;
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                            WINHTTP_HEADER_NAME_BY_INDEX, &clen, &sz,
                            WINHTTP_NO_HEADER_INDEX)) {
        return clen;
    }
    {
        DWORD small = 0;
        sz = sizeof small;
        if (WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &small, &sz,
                                WINHTTP_NO_HEADER_INDEX)) {
            return small;
        }
    }
    return 0;
}

/* Reads the whole response body into a heap buffer, bounded by `max`. */
static bool http_read_all(http_req *h, char **out, size_t *out_len, size_t max,
                          char *err, size_t errcap) {
    char *buf;
    size_t len = 0;
    size_t cap = 65536;
    bool ok = true;

    if (cap > max + 1) cap = max + 1;
    buf = (char *)malloc(cap);
    if (!buf) {
        set_error(err, errcap, "out of memory");
        return false;
    }
    for (;;) {
        DWORD avail = 0;
        DWORD got = 0;
        if (upd_stop_requested()) {
            set_error(err, errcap, "cancelled");
            ok = false;
            break;
        }
        if (!WinHttpQueryDataAvailable(h->request, &avail)) {
            set_error(err, errcap, "WinHttpQueryDataAvailable failed (error %lu)",
                      GetLastError());
            ok = false;
            break;
        }
        if (avail == 0) break;
        if (len + avail > max) {
            set_error(err, errcap, "response larger than %lu bytes",
                      (unsigned long)max);
            ok = false;
            break;
        }
        if (len + avail + 1 > cap) {
            size_t want = cap;
            char *nb;
            while (want < len + avail + 1) want *= 2;
            if (want > max + 1) want = max + 1;
            nb = (char *)realloc(buf, want);
            if (!nb) {
                set_error(err, errcap, "out of memory");
                ok = false;
                break;
            }
            buf = nb;
            cap = want;
        }
        if (!WinHttpReadData(h->request, buf + len, avail, &got)) {
            set_error(err, errcap, "WinHttpReadData failed (error %lu)", GetLastError());
            ok = false;
            break;
        }
        if (got == 0) break;
        len += got;
    }
    if (!ok) {
        free(buf);
        return false;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return true;
}

static bool fetch_manifest(manifest *m, char *err, size_t errcap) {
    http_req h;
    char *body = NULL;
    size_t len = 0;
    bool ok;

    if (!http_open(&h, g_manifest_url, err, errcap)) return false;
    ok = http_read_all(&h, &body, &len, UPD_MANIFEST_MAX, err, errcap);
    http_close(&h);
    if (!ok) {
        free(body);
        return false;
    }
    ok = parse_manifest(body, len, m, err, errcap);
    free(body);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* State helpers                                                            */
/* ------------------------------------------------------------------------ */

static void set_state(upd_state state) {
    EnterCriticalSection(&g_lock);
    g_info.state = state;
    LeaveCriticalSection(&g_lock);
}

static void set_failed(const char *message) {
    EnterCriticalSection(&g_lock);
    g_info.state = UPD_FAILED;
    g_info.progress = 0.0f;
    snprintf(g_info.error, sizeof g_info.error, "%s", message ? message : "unknown error");
    LeaveCriticalSection(&g_lock);
    log_error("update: %s", message ? message : "unknown error");
}

/* ------------------------------------------------------------------------ */
/* Worker: check                                                            */
/* ------------------------------------------------------------------------ */

static void do_check(void) {
    manifest m;
    char err[512];
    char current[32];
    bool ok;
    int cmp;

    err[0] = 0;
    EnterCriticalSection(&g_lock);
    snprintf(current, sizeof current, "%s", g_info.current);
    LeaveCriticalSection(&g_lock);

    ok = fetch_manifest(&m, err, sizeof err);
    if (!ok) {
        if (!upd_stop_requested()) set_failed(err[0] ? err : "manifest download failed");
        return;
    }
    if (upd_stop_requested()) return;

    cmp = version_cmp(m.version, current);
    log_info("update: manifest version %s, current %s", m.version, current);

    if (cmp > 0) {
        if (m.url[0] == 0 || !sha256_looks_valid(m.sha256)) {
            set_failed("manifest: newer version but missing url/sha256");
            return;
        }
        sha256_lower(m.sha256);
        EnterCriticalSection(&g_lock);
        snprintf(g_info.latest, sizeof g_info.latest, "%s", m.version);
        snprintf(g_info.notes, sizeof g_info.notes, "%s", m.notes);
        snprintf(g_info.page, sizeof g_info.page, "%s", m.page);
        g_info.size = m.size;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        snprintf(g_download_url, sizeof g_download_url, "%s", m.url);
        snprintf(g_sha256, sizeof g_sha256, "%s", m.sha256);
        g_expected_size = m.size;
        g_have_manifest = true;
        g_info.state = UPD_AVAILABLE;
        LeaveCriticalSection(&g_lock);
        log_info("update: %s available (%llu bytes)", m.version, m.size);
    } else {
        EnterCriticalSection(&g_lock);
        snprintf(g_info.latest, sizeof g_info.latest, "%s", m.version);
        snprintf(g_info.notes, sizeof g_info.notes, "%s", m.notes);
        snprintf(g_info.page, sizeof g_info.page, "%s", m.page);
        g_info.size = m.size;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        g_download_url[0] = 0;
        g_sha256[0] = 0;
        g_expected_size = 0;
        g_have_manifest = false;
        g_info.state = UPD_UP_TO_DATE;
        LeaveCriticalSection(&g_lock);
        log_info("update: already up to date");
    }
}

/* ------------------------------------------------------------------------ */
/* Worker: download                                                         */
/* ------------------------------------------------------------------------ */

static void publish_progress(unsigned long long done, unsigned long long total) {
    float p;
    if (total == 0) return;
    p = (float)((double)done / (double)total);
    if (p < 0.0f) p = 0.0f;
    if (p > 0.995f) p = 0.995f;
    EnterCriticalSection(&g_lock);
    g_info.progress = p;
    LeaveCriticalSection(&g_lock);
}

static bool read_text_w(const wchar_t *path, char **out, size_t *out_len,
                        size_t max) {
    HANDLE f;
    DWORD size;
    DWORD got = 0;
    char *buf;

    *out = NULL;
    *out_len = 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > max) {
        CloseHandle(f);
        return false;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        CloseHandle(f);
        return false;
    }
    if (!ReadFile(f, buf, size, &got, NULL) || got != size) {
        free(buf);
        CloseHandle(f);
        return false;
    }
    CloseHandle(f);
    buf[size] = 0;
    *out = buf;
    *out_len = (size_t)size;
    return true;
}

static bool write_text_w(const wchar_t *path, const char *utf8) {
    HANDLE f;
    DWORD len = (DWORD)strlen(utf8);
    DWORD wrote = 0;
    bool ok;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    ok = WriteFile(f, utf8, len, &wrote, NULL) && wrote == len;
    CloseHandle(f);
    return ok;
}

static void do_download(void) {
    char url[1024];
    char sha[80];
    char version[32];
    char tmp_path[UPD_PATH_CAP];
    char bin_path[UPD_PATH_CAP];
    char json_path[UPD_PATH_CAP];
    wchar_t wtmp[UPD_WPATH_CAP];
    wchar_t wbin[UPD_WPATH_CAP];
    wchar_t wjson[UPD_WPATH_CAP];
    unsigned long long expected_size;
    http_req h;
    HANDLE f = INVALID_HANDLE_VALUE;
    hash_ctx hc;
    char computed[65];
    char errbuf[512];
    unsigned char *buf = NULL;
    unsigned long long total = 0;
    unsigned long long content_len = 0;
    unsigned long long denom = 0;
    ULONGLONG last_publish = 0;
    bool ok = true;

    errbuf[0] = 0;
    EnterCriticalSection(&g_lock);
    snprintf(url, sizeof url, "%s", g_download_url);
    snprintf(sha, sizeof sha, "%s", g_sha256);
    snprintf(version, sizeof version, "%s", g_info.latest);
    expected_size = g_expected_size;
    LeaveCriticalSection(&g_lock);

    if (!g_have_manifest || url[0] == 0 || sha[0] == 0) {
        set_failed("no download metadata");
        return;
    }
    if (!ensure_updates_dir() ||
        !updates_path(tmp_path, sizeof tmp_path, "update.bin.tmp") ||
        !updates_path(bin_path, sizeof bin_path, "update.bin") ||
        !updates_path(json_path, sizeof json_path, "update.json")) {
        set_failed("cannot resolve the updates directory");
        return;
    }
    if (!to_wide(tmp_path, wtmp, UPD_WPATH_CAP) ||
        !to_wide(bin_path, wbin, UPD_WPATH_CAP) ||
        !to_wide(json_path, wjson, UPD_WPATH_CAP)) {
        set_failed("update path is too long");
        return;
    }
    DeleteFileW(wtmp);

    if (!http_open(&h, url, errbuf, sizeof errbuf)) {
        set_failed(errbuf);
        return;
    }
    content_len = http_content_length(h.request);
    denom = expected_size ? expected_size : content_len;

    f = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(errbuf, sizeof errbuf, "cannot create update.bin.tmp (error %lu)",
                 GetLastError());
        http_close(&h);
        set_failed(errbuf);
        return;
    }
    if (!hash_init(&hc)) {
        http_close(&h);
        CloseHandle(f);
        DeleteFileW(wtmp);
        set_failed("cannot initialise SHA-256");
        return;
    }
    buf = (unsigned char *)malloc(UPD_IO_CHUNK);
    if (!buf) {
        http_close(&h);
        CloseHandle(f);
        hash_free(&hc);
        DeleteFileW(wtmp);
        set_failed("out of memory");
        return;
    }

    log_info("update: downloading %s", url);
    for (;;) {
        DWORD avail = 0;
        DWORD got = 0;
        DWORD wrote = 0;

        if (upd_stop_requested()) {
            snprintf(errbuf, sizeof errbuf, "cancelled");
            ok = false;
            break;
        }
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            snprintf(errbuf, sizeof errbuf,
                     "WinHttpQueryDataAvailable failed (error %lu)", GetLastError());
            ok = false;
            break;
        }
        if (avail == 0) break;
        if (avail > UPD_IO_CHUNK) avail = UPD_IO_CHUNK;
        if (!WinHttpReadData(h.request, buf, avail, &got)) {
            snprintf(errbuf, sizeof errbuf, "WinHttpReadData failed (error %lu)",
                     GetLastError());
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
            snprintf(errbuf, sizeof errbuf, "cannot write update.bin.tmp (error %lu)",
                     GetLastError());
            ok = false;
            break;
        }
        if (!hash_update(&hc, buf, got)) {
            snprintf(errbuf, sizeof errbuf, "SHA-256 update failed");
            ok = false;
            break;
        }
        total += got;
        {
            ULONGLONG now = GetTickCount64();
            if (now - last_publish >= 100 || total == denom) {
                publish_progress(total, denom);
                last_publish = now;
            }
        }
    }
    http_close(&h);
    free(buf);
    buf = NULL;

    if (ok && !hash_finish(&hc, computed)) {
        snprintf(errbuf, sizeof errbuf, "SHA-256 finalize failed");
        ok = false;
    }
    hash_free(&hc);
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;

    if (!ok) {
        DeleteFileW(wtmp);
        set_failed(errbuf);
        return;
    }
    if (expected_size != 0 && total != expected_size) {
        DeleteFileW(wtmp);
        snprintf(errbuf, sizeof errbuf,
                 "size mismatch: got %llu bytes, manifest says %llu", total,
                 expected_size);
        set_failed(errbuf);
        return;
    }
    if (!hex_equal(sha, computed)) {
        DeleteFileW(wtmp);
        snprintf(errbuf, sizeof errbuf, "SHA-256 mismatch");
        set_failed(errbuf);
        return;
    }
    if (!MoveFileExW(wtmp, wbin, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(errbuf, sizeof errbuf, "cannot finalize update.bin (error %lu)",
                 GetLastError());
        DeleteFileW(wtmp);
        set_failed(errbuf);
        return;
    }
    {
        char json[512];
        int n = snprintf(json, sizeof json,
                         "{\n  \"version\": \"%s\",\n  \"sha256\": \"%s\",\n"
                         "  \"size\": %llu\n}\n",
                         version, computed, total);
        if (n > 0 && (size_t)n < sizeof json) {
            if (!write_text_w(wjson, json)) {
                log_warn("update: update.bin verified but update.json could not be written");
            }
        }
    }
    EnterCriticalSection(&g_lock);
    g_info.progress = 1.0f;
    g_info.size = total;
    g_info.state = UPD_READY;
    g_info.error[0] = 0;
    LeaveCriticalSection(&g_lock);
    log_info("update: %s downloaded and verified (%llu bytes)", version, total);
}

/* ------------------------------------------------------------------------ */
/* Thread plumbing                                                          */
/* ------------------------------------------------------------------------ */

static unsigned __stdcall update_thread(void *arg) {
    int op = (int)(intptr_t)arg;
    if (op == UPD_OP_CHECK) do_check();
    else if (op == UPD_OP_DOWNLOAD) do_download();
    return 0;
}

/* Closes the handle of a finished worker; true when no worker is running.
 * A worker that just published its final state may need a few more
 * microseconds to actually exit, so the caller can allow a short grace
 * period instead of failing a brand-new operation. */
static bool reap_thread(DWORD timeout_ms) {
    HANDLE h;

    EnterCriticalSection(&g_lock);
    h = g_thread;
    LeaveCriticalSection(&g_lock);
    if (!h) return true;
    if (WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0) return false;

    EnterCriticalSection(&g_lock);
    if (g_thread == h) g_thread = NULL;
    LeaveCriticalSection(&g_lock);
    CloseHandle(h);
    return true;
}

static bool start_thread(int op) {
    uintptr_t handle;

    if (!reap_thread(250)) return false;
    InterlockedExchange(&g_stop, 0);
    handle = _beginthreadex(NULL, 0, update_thread, (void *)(intptr_t)op, 0, NULL);
    if (!handle) return false;
    EnterCriticalSection(&g_lock);
    g_thread = (HANDLE)handle;
    LeaveCriticalSection(&g_lock);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

bool upd_init(const char *current, const char *manifest_url) {
    const char *data;

    if (g_inited) upd_shutdown();

    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;

    memset(&g_info, 0, sizeof g_info);
    memset(g_manifest_url, 0, sizeof g_manifest_url);
    memset(g_download_url, 0, sizeof g_download_url);
    memset(g_sha256, 0, sizeof g_sha256);
    g_have_manifest = false;
    g_expected_size = 0;
    g_last_check = 0;
    g_thread = NULL;
    InterlockedExchange(&g_stop, 0);
    g_inited = true;

    snprintf(g_info.current, sizeof g_info.current, "%s",
             (current && current[0]) ? current : "0.0.0");
    if (manifest_url && manifest_url[0]) {
        size_t n = strlen(manifest_url);
        if (n >= sizeof g_manifest_url) {
            g_inited = false;
            DeleteCriticalSection(&g_lock);
            g_lock_ready = false;
            return false;
        }
        memcpy(g_manifest_url, manifest_url, n + 1);
        g_info.state = UPD_IDLE;
    } else {
        g_info.state = UPD_UP_TO_DATE;
    }

    data = cfg_data_dir();
    if (!data || !data[0]) {
        upd_shutdown();
        return false;
    }
    if (!ensure_updates_dir()) {
        log_warn("update: cannot create the updates directory");
    }
    log_info("update: initialised, current %s%s%s", g_info.current,
             g_manifest_url[0] ? ", manifest " : "",
             g_manifest_url[0] ? g_manifest_url : "");
    return true;
}

void upd_shutdown(void) {
    HANDLE h;

    if (!g_inited) return;
    InterlockedExchange(&g_stop, 1);

    EnterCriticalSection(&g_lock);
    h = g_thread;
    g_thread = NULL;
    LeaveCriticalSection(&g_lock);
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    if (g_lock_ready) {
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }
    g_inited = false;
    memset(&g_info, 0, sizeof g_info);
    g_info.state = UPD_IDLE;
}

void upd_check(bool force) {
    bool disabled;
    bool busy;
    bool recent;
    bool can_start;
    ULONGLONG now;

    if (!g_inited) return;

    EnterCriticalSection(&g_lock);
    disabled = g_manifest_url[0] == 0;
    busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    now = GetTickCount64();
    recent = g_last_check != 0 && (now - g_last_check) < UPD_MIN_INTERVAL_MS;
    can_start = !busy && (force || !recent);
    if (can_start) {
        g_info.state = UPD_CHECKING;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        g_last_check = now;
    }
    LeaveCriticalSection(&g_lock);

    if (disabled) {
        set_state(UPD_UP_TO_DATE);
        return;
    }
    if (!can_start) return;
    if (!start_thread(UPD_OP_CHECK)) {
        set_failed("cannot start the update thread");
    }
}

void upd_download(void) {
    bool can_start = false;
    bool busy;

    if (!g_inited) return;

    EnterCriticalSection(&g_lock);
    busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    if (!busy && g_info.state == UPD_AVAILABLE && g_have_manifest) {
        g_info.state = UPD_DOWNLOADING;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        can_start = true;
    }
    LeaveCriticalSection(&g_lock);

    if (!can_start) return;
    if (!start_thread(UPD_OP_DOWNLOAD)) {
        /* start_thread() may have failed because another worker is still
         * alive; only report FAILED when nothing is running. */
        if (reap_thread(0)) set_failed("cannot start the update thread");
        else set_state(UPD_AVAILABLE);
    }
}

void upd_get(upd_info *out) {
    if (!out) return;
    if (!g_inited) {
        memset(out, 0, sizeof *out);
        out->state = UPD_IDLE;
        return;
    }
    EnterCriticalSection(&g_lock);
    *out = g_info;
    LeaveCriticalSection(&g_lock);
}

bool upd_run_installer(void) {
    char bin_path[UPD_PATH_CAP];
    char marker[UPD_PATH_CAP];
    wchar_t wbin[UPD_WPATH_CAP];
    wchar_t wmarker[UPD_WPATH_CAP];
    char actual[65];
    char expected[80];
    char version[32];
    unsigned long long size = 0;
    SHELLEXECUTEINFOW sei;
    bool have_expected = false;

    if (!g_inited) return false;
    if (!updates_path(bin_path, sizeof bin_path, "update.bin") ||
        !updates_path(marker, sizeof marker, "pending-version.txt")) {
        log_error("update: cannot resolve installer paths");
        return false;
    }
    if (!to_wide(bin_path, wbin, UPD_WPATH_CAP) ||
        !to_wide(marker, wmarker, UPD_WPATH_CAP)) {
        log_error("update: installer path is too long");
        return false;
    }
    if (GetFileAttributesW(wbin) == INVALID_FILE_ATTRIBUTES) {
        log_error("update: update.bin is missing");
        return false;
    }
    if (!file_sha256_w(wbin, actual, &size)) {
        log_error("update: cannot hash update.bin");
        return false;
    }
    EnterCriticalSection(&g_lock);
    snprintf(expected, sizeof expected, "%s", g_sha256);
    snprintf(version, sizeof version, "%s", g_info.latest);
    have_expected = g_have_manifest && expected[0] != 0;
    LeaveCriticalSection(&g_lock);

    /* A previous session may have left a verified update.bin behind: trust
     * the metadata written last, not the payload, for the expected digest. */
    if (!have_expected) {
        char json_path[UPD_PATH_CAP];
        wchar_t wjson[UPD_WPATH_CAP];
        char *body = NULL;
        size_t len = 0;
        manifest meta;
        char perr[128];
        if (updates_path(json_path, sizeof json_path, "update.json") &&
            to_wide(json_path, wjson, UPD_WPATH_CAP) &&
            read_text_w(wjson, &body, &len, 65536)) {
            if (parse_manifest(body, len, &meta, perr, sizeof perr) &&
                sha256_looks_valid(meta.sha256)) {
                sha256_lower(meta.sha256);
                snprintf(expected, sizeof expected, "%s", meta.sha256);
                snprintf(version, sizeof version, "%s", meta.version);
                have_expected = true;
            }
            free(body);
        }
    }

    if (!have_expected) {
        log_error("update: no expected hash for update.bin");
        return false;
    }
    if (!hex_equal(expected, actual)) {
        log_error("update: update.bin hash mismatch, removing it");
        DeleteFileW(wbin);
        return false;
    }

    if (!write_text_w(wmarker, version)) {
        log_error("update: cannot write pending-version.txt");
        return false;
    }
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wbin;
    sei.lpParameters = L"/SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        log_error("update: ShellExecuteExW failed (error %lu)", GetLastError());
        DeleteFileW(wmarker);
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    log_info("update: installer launched for %s (%llu bytes)", version, size);
    return true;
}

bool upd_take_updated_flag(char *version, size_t cap) {
    char marker[UPD_PATH_CAP];
    wchar_t wmarker[UPD_WPATH_CAP];
    HANDLE f;
    char buf[128];
    DWORD got = 0;
    bool existed;
    size_t len;

    if (version && cap) version[0] = 0;
    if (!g_inited) return false;
    if (!updates_path(marker, sizeof marker, "pending-version.txt") ||
        !to_wide(marker, wmarker, UPD_WPATH_CAP)) {
        return false;
    }
    f = CreateFileW(wmarker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    existed = true;
    if (!ReadFile(f, buf, (DWORD)sizeof buf - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    DeleteFileW(wmarker);
    if (got == 0) return existed;
    buf[got] = 0;
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = 0;
    }
    if (version && cap) snprintf(version, cap, "%s", buf);
    return existed;
}
