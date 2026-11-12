/* update_io.c -- the I/O half of the updater: paths and files, SHA-256
 * (BCrypt), the minimal JSON manifest reader, and everything WinHTTP
 * (proxy resolution, GET, throttled progress reporting, streamed download).
 *
 * Everything here blocks. It is only ever called from the update worker
 * thread in update.c, which owns the state machine and the public API.
 */
#include "update_priv.h"

#include <winhttp.h>
#include <bcrypt.h>

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_QUERY_FLAG_NUMBER64
#define WINHTTP_QUERY_FLAG_NUMBER64 0x10000000
#endif

/* ------------------------------------------------------------------------ */
/* Paths                                                                    */
/* ------------------------------------------------------------------------ */

bool upd__to_wide(const char *s, wchar_t *out, size_t cap) {
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
bool upd__updates_path(char *out, size_t cap, const char *name) {
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

bool upd__ensure_updates_dir(void) {
    char dir[UPD_PATH_CAP];
    wchar_t wdir[UPD_WPATH_CAP];
    if (!upd__updates_path(dir, sizeof dir, NULL)) return false;
    if (!upd__to_wide(dir, wdir, UPD_WPATH_CAP)) return false;
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

bool upd__file_sha256_w(const wchar_t *path, char out[65],
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

bool upd__hex_equal(const char *a, const char *b) {
    size_t i;
    if (!a || !b) return false;
    if (strlen(a) != strlen(b)) return false;
    for (i = 0; a[i]; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

bool upd__sha256_looks_valid(const char *s) {
    size_t i;
    if (!s) return false;
    for (i = 0; s[i]; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return i == 64;
}

void upd__sha256_lower(char *s) {
    size_t i;
    for (i = 0; s && s[i]; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

/* ------------------------------------------------------------------------ */
/* Small text files                                                         */
/* ------------------------------------------------------------------------ */

bool upd__read_text_w(const wchar_t *path, char **out, size_t *out_len,
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

bool upd__write_text_w(const wchar_t *path, const char *utf8) {
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

/* ------------------------------------------------------------------------ */
/* Minimal JSON reader                                                      */
/* ------------------------------------------------------------------------ */

typedef struct json_reader {
    const char *p;
    const char *end;
} json_reader;

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
bool upd__parse_manifest(const char *json, size_t len, upd_manifest *m,
                         char *err, size_t errcap) {
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
    if (!upd__to_wide(url_utf8, wurl, sizeof wurl / sizeof wurl[0])) {
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
        if (upd__stop_requested()) {
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

bool upd__fetch_manifest(const char *url, upd_manifest *m, char *err,
                         size_t errcap) {
    http_req h;
    char *body = NULL;
    size_t len = 0;
    bool ok;

    if (!http_open(&h, url, err, errcap)) return false;
    ok = http_read_all(&h, &body, &len, UPD_MANIFEST_MAX, err, errcap);
    http_close(&h);
    if (!ok) {
        free(body);
        return false;
    }
    ok = upd__parse_manifest(body, len, m, err, errcap);
    free(body);
    return ok;
}

bool upd__download_to_file(const char *url, const wchar_t *dest,
                           unsigned long long expected_size,
                           const char *expected_sha256,
                           upd__progress_fn on_progress, void *ctx,
                           char computed[65], unsigned long long *total_out,
                           char *err, size_t errcap) {
    http_req h;
    HANDLE f = INVALID_HANDLE_VALUE;
    hash_ctx hc;
    unsigned char *buf = NULL;
    unsigned long long total = 0;
    unsigned long long denom;
    ULONGLONG last_publish = 0;
    bool ok = true;

    computed[0] = 0;
    DeleteFileW(dest);

    if (!http_open(&h, url, err, errcap)) return false;
    denom = expected_size ? expected_size : http_content_length(h.request);

    f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        set_error(err, errcap, "cannot create update.bin.tmp (error %lu)",
                  GetLastError());
        http_close(&h);
        return false;
    }
    if (!hash_init(&hc)) {
        http_close(&h);
        CloseHandle(f);
        DeleteFileW(dest);
        set_error(err, errcap, "cannot initialise SHA-256");
        return false;
    }
    buf = (unsigned char *)malloc(UPD_IO_CHUNK);
    if (!buf) {
        http_close(&h);
        CloseHandle(f);
        hash_free(&hc);
        DeleteFileW(dest);
        set_error(err, errcap, "out of memory");
        return false;
    }

    log_info("update: downloading %s", url);
    for (;;) {
        DWORD avail = 0;
        DWORD got = 0;
        DWORD wrote = 0;

        if (upd__stop_requested()) {
            set_error(err, errcap, "cancelled");
            ok = false;
            break;
        }
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            set_error(err, errcap, "WinHttpQueryDataAvailable failed (error %lu)",
                      GetLastError());
            ok = false;
            break;
        }
        if (avail == 0) break;
        if (avail > UPD_IO_CHUNK) avail = UPD_IO_CHUNK;
        if (!WinHttpReadData(h.request, buf, avail, &got)) {
            set_error(err, errcap, "WinHttpReadData failed (error %lu)",
                      GetLastError());
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
            set_error(err, errcap, "cannot write update.bin.tmp (error %lu)",
                      GetLastError());
            ok = false;
            break;
        }
        if (!hash_update(&hc, buf, got)) {
            set_error(err, errcap, "SHA-256 update failed");
            ok = false;
            break;
        }
        total += got;
        {
            ULONGLONG now = GetTickCount64();
            if (now - last_publish >= 100 || total == denom) {
                if (on_progress) on_progress(ctx, total, denom);
                last_publish = now;
            }
        }
    }
    http_close(&h);
    free(buf);
    buf = NULL;

    if (ok && !hash_finish(&hc, computed)) {
        set_error(err, errcap, "SHA-256 finalize failed");
        ok = false;
    }
    hash_free(&hc);
    CloseHandle(f);

    if (!ok) {
        DeleteFileW(dest);
        return false;
    }
    if (expected_size != 0 && total != expected_size) {
        DeleteFileW(dest);
        set_error(err, errcap, "size mismatch: got %llu bytes, manifest says %llu",
                  total, expected_size);
        return false;
    }
    if (!upd__hex_equal(expected_sha256, computed)) {
        DeleteFileW(dest);
        set_error(err, errcap, "SHA-256 mismatch");
        return false;
    }
    if (total_out) *total_out = total;
    return true;
}
