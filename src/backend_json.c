/* backend_json.c -- pure string helpers for the worker bridge: the byte buffer,
 * UTF-8 <-> UTF-16 conversion, JSON escaping/parsing and base64 decoding.
 *
 * No globals, no I/O, no worker state: everything here is a function of its
 * arguments (backend_priv.h carries the prototypes).
 */
#include "backend_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ byte buffer */

bool wb__buf_reserve(wb_buf *b, size_t extra) {
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

void wb__buf_put(wb_buf *b, const char *s, size_t n) {
    if (!wb__buf_reserve(b, n)) return;
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void wb__buf_puts(wb_buf *b, const char *s) { wb__buf_put(b, s, strlen(s)); }

void wb__buf_putc(wb_buf *b, char c) { wb__buf_put(b, &c, 1); }

void wb__buf_free(wb_buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* --------------------------------------------------------- UTF-8 helpers */

wchar_t *wb__u8_to_w(const char *s) {
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

void wb__w_to_u8(const wchar_t *w, char *dst, size_t cap) {
    if (dst && cap) dst[0] = 0;
    if (!w || !dst || cap < 2) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, (int)cap, NULL, NULL);
    if (n <= 0) dst[0] = 0;
}

/* ----------------------------------------------------------- JSON output */

void wb__json_escape(wb_buf *b, const char *s) {
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

const char *wb__ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* p at the opening quote; returns the char after the closing quote and stores
 * the raw (still escaped) content range in *content / *len. */
const char *wb__scan_string(const char *p, const char **content, size_t *len) {
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

bool wb__skip_value(const char **pp) {
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
const char *wb__value_of(const char *json, const char *key) {
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

int wb__hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

unsigned wb__hex4(const char *s) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = wb__hexval(s[i]);
        if (h < 0) return 0;
        v = (v << 4) | (unsigned)h;
    }
    return v;
}

void wb__utf8_put(wb_buf *b, unsigned cp) {
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
void wb__unescape(wb_buf *b, const char *s, size_t len) {
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

bool wb__json_raw(const char *json, const char *key, const char **content, size_t *len) {
    const char *v = wb__value_of(json, key);
    if (!v) return false;
    const char *c = NULL;
    size_t n = 0;
    if (!wb__scan_string(v, &c, &n)) return false;
    *content = c;
    *len = n;
    return true;
}

bool wb__json_str_into(const char *json, const char *key, char *dst, size_t cap) {
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

char *wb__json_alloc_str(const char *json, const char *key) {
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
bool wb__parse_number(const char *p, double *out) {
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

bool wb__json_number(const char *json, const char *key, double *out) {
    const char *v = wb__value_of(json, key);
    if (!v) return false;
    return wb__parse_number(v, out);
}

bool wb__json_int(const char *json, const char *key, int *out) {
    double d = 0;
    if (!wb__json_number(json, key, &d)) return false;
    if (d > 2147483647.0) d = 2147483647.0;
    if (d < -2147483648.0) d = -2147483648.0;
    *out = (int)d;
    return true;
}

bool wb__json_bool(const char *json, const char *key) {
    const char *p = wb__value_of(json, key);
    return p && strncmp(p, "true", 4) == 0;
}

/* ---------------------------------------------------------------- base64 */

int wb__b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

unsigned char *wb__b64_decode(const char *s, size_t *out_len) {
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
