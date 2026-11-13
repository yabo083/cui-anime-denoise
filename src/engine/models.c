/* models.c -- optional model management. APISR and CUGAN weights are NOT
 * redistributed with the app: the user downloads the upstream .pth into
 * user_models_dir, and a weightless <name>.skel.onnx (emitted by
 * tools/export_onnx.py) reads it as ONNX external data. The skeleton must sit
 * in the same directory as its .pth -- that is the "pair" every status and
 * load check looks for, and the download only counts as complete once the
 * bundled skeleton has been copied next to the fresh .pth.
 *
 * Downloads reuse the updater's WinHTTP helper (system proxy, streaming
 * SHA-256, progress); CUGAN arrives as a zip -- the one member whose name ends
 * with the .pth file name is extracted (stored or raw-DEFLATE via CUI's
 * bounds-checked inflate) and verified against the reference's SHA-256 table,
 * exactly like denoise.py's fetch().
 *
 * Cancel note: `cancel` is checked between download/extract stages; the
 * helper's own stream loop cannot be interrupted mid-flight (it answers the
 * updater's stop flag instead).
 */
#include "engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "../update_priv.h"

/* from third_party/CUI/src/cui_inflate.c: raw DEFLATE, bounds-checked;
 * returns bytes written, 0 on corrupt input (and 0 for empty output, which
 * never happens for a real member) */
size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap);

typedef struct {
    eng_model model;
    const char *skel;         /* skeleton file in the models dirs */
    const char *weights;      /* upstream .pth name */
    const char *url;
    const char *sha256_pth;   /* of the final .pth bytes (denoise.py's table) */
    const char *sha256_zip;   /* of the downloaded zip itself, or NULL */
    int is_zip;
    unsigned long long weights_size;
    unsigned long long download_size;
} model_def;

static const model_def MODELS[] = {
    { ENG_M_APISR, "apisr_rrdb_x2.skel.onnx", "2x_APISR_RRDB_GAN_generator.pth",
      "https://huggingface.co/camenduru/APISR/resolve/main/"
      "2x_APISR_RRDB_GAN_generator.pth",
      "3b0d2b3a3c0461ac17d00f4f32240666fb832b738ea5a48449b1acf07fbb07e5",
      NULL, 0, 17960938ull, 17960938ull },
    { ENG_M_CUGAN, "cugan_up2x_denoise3x.skel.onnx",
      "up2x-latest-denoise3x.pth",
      "https://github.com/bilibili/ailab/releases/download/Real-CUGAN/"
      "updated_weights.zip",
      "0a14739f3f5fcbd74ec3ce2806d13a47916c916b20afe4a39d95f6df4ca6abd8",
      "99a8ce2e4a6ce0548668879f148b3db3e143201d3df42c825a4143490396da53",
      1, 5147249ull, 54001450ull },
};

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!err || !cap) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static const model_def *find_def(eng_model m) {
    size_t i;
    for (i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++) {
        if (MODELS[i].model == m) {
            return &MODELS[i];
        }
    }
    return NULL;
}

static wchar_t *join_path(const wchar_t *dir, const char *name_utf8) {
    wchar_t wname[260];
    size_t dl, nl;
    wchar_t *p;
    if (!dir) {
        return NULL;
    }
    if (!upd__to_wide(name_utf8, wname, sizeof(wname) / sizeof(wchar_t))) {
        return NULL;
    }
    dl = wcslen(dir);
    nl = wcslen(wname);
    p = (wchar_t *)malloc((dl + nl + 2) * sizeof(wchar_t));
    if (!p) {
        return NULL;
    }
    memcpy(p, dir, dl * sizeof(wchar_t));
    if (dl && p[dl - 1] != L'\\' && p[dl - 1] != L'/') {
        p[dl++] = L'\\';
    }
    memcpy(p + dl, wname, (nl + 1) * sizeof(wchar_t));
    return p;
}

static int file_exists(const wchar_t *p) {
    return p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* skeleton + .pth both present in `dir`; fills st fields when true. */
static int pair_in(const model_def *d, const wchar_t *dir,
                   eng_model_state *st) {
    wchar_t *sk, *wt;
    int ok;
    if (!dir) {
        return 0;
    }
    wt = join_path(dir, d->weights);
    ok = file_exists(wt);
    if (ok && st) {
        wcsncpy(st->weights_path, wt,
                sizeof(st->weights_path) / sizeof(wchar_t) - 1);
    }
    free(wt);
    if (!ok) {
        return 0;
    }
    sk = join_path(dir, d->skel);
    ok = file_exists(sk);
    free(sk);
    return ok;
}

const char *eng_model_name(eng_model m) {
    switch (m) {
        case ENG_M_ESRGAN: return "ESRGAN";
        case ENG_M_CUGAN:  return "CUGAN";
        case ENG_M_APISR:  return "APISR";
        case ENG_M_NONE:   return "L0";
        default:           return "?";
    }
}

void eng_model_status(const eng_config *cfg, eng_model m,
                      eng_model_state *st) {
    const model_def *d = find_def(m);
    memset(st, 0, sizeof(*st));
    if (!d) {
        /* bundled ESRGAN: still a file check, a broken install reports it */
        st->available = (m == ENG_M_ESRGAN);
        if (m == ENG_M_ESRGAN && cfg && cfg->models_dir) {
            wchar_t *p = join_path(cfg->models_dir, "esrgan_anime6b_x4.onnx");
            st->available = file_exists(p);
            if (!st->available) {
                snprintf(st->missing, sizeof(st->missing),
                         "esrgan_anime6b_x4.onnx");
            }
            free(p);
        }
        return;
    }
    st->weights_name = d->weights;
    st->url = d->url;
    st->sha256_hex = d->sha256_pth;
    st->weights_size = d->weights_size;
    if (cfg) {
        wchar_t *p;
        /* report presence individually too */
        p = join_path(cfg->user_models_dir, d->weights);
        st->weights_present = file_exists(p);
        if (st->weights_present) {
            wcsncpy(st->weights_path, p,
                    sizeof(st->weights_path) / sizeof(wchar_t) - 1);
        }
        free(p);
        if (!st->weights_present) {
            p = join_path(cfg->models_dir, d->weights);
            st->weights_present = file_exists(p);
            if (st->weights_present) {
                wcsncpy(st->weights_path, p,
                        sizeof(st->weights_path) / sizeof(wchar_t) - 1);
            }
            free(p);
        }
        p = join_path(cfg->user_models_dir, d->skel);
        st->skeleton_present = file_exists(p);
        free(p);
        if (!st->skeleton_present) {
            p = join_path(cfg->models_dir, d->skel);
            st->skeleton_present = file_exists(p);
            free(p);
        }
        st->available = pair_in(d, cfg->user_models_dir, st) ||
                        pair_in(d, cfg->models_dir, st);
    }
    if (!st->available) {
        /* both halves exist but in different directories: still not a pair */
        if (st->weights_present && st->skeleton_present) {
            snprintf(st->missing, sizeof(st->missing), "%s",
                     "skeleton and weights not co-located");
        } else {
            snprintf(st->missing, sizeof(st->missing),
                     "%s%s%s", st->weights_present ? "" : d->weights,
                     (!st->weights_present && !st->skeleton_present) ? " + " : "",
                     st->skeleton_present ? "" : d->skel);
        }
    }
}

/* ----------------------------- zip reading ------------------------------- */

static uint32_t rd_u16(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Load a whole file; returns NULL on failure. */
static unsigned char *read_all(const wchar_t *path, size_t *out_len) {
    HANDLE f;
    LARGE_INTEGER sz;
    unsigned char *buf;
    DWORD got = 0, chunk;
    size_t off = 0;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (LONGLONG)0x7fffffff) {
        CloseHandle(f);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz.QuadPart ? (size_t)sz.QuadPart : 1);
    if (!buf) {
        CloseHandle(f);
        return NULL;
    }
    while (off < (size_t)sz.QuadPart) {
        chunk = (DWORD)((sz.QuadPart - (LONGLONG)off > 1 << 24)
                            ? 1 << 24
                            : sz.QuadPart - (LONGLONG)off);
        if (!ReadFile(f, buf + off, chunk, &got, NULL) || got == 0) {
            free(buf);
            CloseHandle(f);
            return NULL;
        }
        off += got;
    }
    CloseHandle(f);
    *out_len = off;
    return buf;
}

/* True when zip entry name `e` ends with `suffix` (denoise.py: name ends with
 * the .pth file name, so any leading directory prefix is ignored). */
static int name_ends_with(const char *e, size_t elen, const char *suffix) {
    size_t sl = strlen(suffix);
    return elen >= sl && memcmp(e + elen - sl, suffix, sl) == 0;
}

/* Extract the member ending with `pth_name` from `zip` into a fresh malloc'ed
 * buffer. Returns length via out_len, NULL on failure. */
static unsigned char *zip_extract(const unsigned char *zip, size_t zip_len,
                                  const char *pth_name, size_t *out_len,
                                  char *err, size_t cap) {
    size_t pos, eocd = 0;
    uint16_t entries;
    uint32_t cd_off;
    int found = 0;

    /* locate the End Of Central Directory record (scan tail for signature) */
    if (zip_len < 22) {
        set_err(err, cap, "zip too small");
        return NULL;
    }
    for (pos = zip_len - 22;; pos--) {
        if (zip[pos] == 0x50 && zip[pos + 1] == 0x4b && zip[pos + 2] == 0x05 &&
            zip[pos + 3] == 0x06) {
            eocd = pos;
            break;
        }
        if (pos == 0 || zip_len - 22 - pos > 65557) {
            set_err(err, cap, "zip EOCD not found");
            return NULL;
        }
    }
    entries = (uint16_t)rd_u16(zip + eocd + 10);
    cd_off = rd_u32(zip + eocd + 16);

    for (pos = cd_off; pos + 46 <= zip_len && entries > 0; entries--) {
        uint32_t sig = rd_u32(zip + pos);
        uint16_t method, nlen, xlen, clen;
        uint32_t csize, usize, lho;
        const char *ename;
        const unsigned char *lh;
        if (sig != 0x02014b50) {
            break;
        }
        method = (uint16_t)rd_u16(zip + pos + 10);
        csize = rd_u32(zip + pos + 20);
        usize = rd_u32(zip + pos + 24);
        nlen = (uint16_t)rd_u16(zip + pos + 28);
        xlen = (uint16_t)rd_u16(zip + pos + 30);
        clen = (uint16_t)rd_u16(zip + pos + 32);
        lho = rd_u32(zip + pos + 42);
        ename = (const char *)(zip + pos + 46);
        if ((size_t)pos + 46 + nlen > zip_len) {
            break;
        }
        if (name_ends_with(ename, nlen, pth_name)) {
            const unsigned char *data;
            uint16_t lnlen, lxlen;
            if (lho + 30 > zip_len || rd_u32(zip + lho) != 0x04034b50) {
                set_err(err, cap, "bad local header");
                return NULL;
            }
            lh = zip + lho;
            lnlen = (uint16_t)rd_u16(lh + 26);
            lxlen = (uint16_t)rd_u16(lh + 28);
            data = lh + 30 + lnlen + lxlen;
            if ((size_t)(data - zip) + csize > zip_len) {
                set_err(err, cap, "member out of bounds");
                return NULL;
            }
            found = 1;
            if (method == 0) {        /* stored */
                unsigned char *out = (unsigned char *)malloc(csize ? csize : 1);
                if (!out) {
                    set_err(err, cap, "out of memory");
                    return NULL;
                }
                memcpy(out, data, csize);
                *out_len = csize;
                return out;
            }
            if (method == 8) {        /* deflated */
                unsigned char *out = (unsigned char *)malloc(usize ? usize : 1);
                size_t wrote;
                if (!out) {
                    set_err(err, cap, "out of memory");
                    return NULL;
                }
                wrote = cui__inflate(data, csize, out, usize);
                if (wrote != usize) {
                    free(out);
                    set_err(err, cap, "inflate failed (%zu of %u)", wrote,
                            usize);
                    return NULL;
                }
                *out_len = usize;
                return out;
            }
            set_err(err, cap, "unsupported zip method %u", method);
            return NULL;
        }
        pos += 46 + nlen + xlen + clen;
    }
    set_err(err, cap, "%s: member ending in %s not found",
            found ? "zip" : "zip", pth_name);
    return NULL;
}

/* SHA-256 of a memory block via BCrypt. */
static int mem_sha256(const unsigned char *data, size_t len, char out[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char digest[32];
    int ok = 0, i;
    char *hex = NULL;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) {
        return -1;
    }
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) {
        goto end;
    }
    if (BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0)) {
        goto end;
    }
    if (BCryptFinishHash(h, digest, sizeof(digest), 0)) {
        goto end;
    }
    hex = out;
    for (i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    out[64] = 0;
    ok = 0;
end:
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ------------------------------ download --------------------------------- */

typedef struct {
    eng_dl_progress_fn cb;
    void *ud;
} dl_ctx;

static void dl_progress(void *ctx, unsigned long long done,
                        unsigned long long total) {
    dl_ctx *c = (dl_ctx *)ctx;
    if (c->cb) {
        c->cb(c->ud, done, total);
    }
}

bool eng_model_pair_complete(const eng_config *cfg, eng_model m,
                             char *err, size_t cap) {
    const model_def *d = find_def(m);
    wchar_t *w, *dst, *src;
    int ok;

    if (!d) {
        set_err(err, cap, "model %d has no downloadable weights", (int)m);
        return false;
    }
    if (!cfg || !cfg->user_models_dir || !cfg->models_dir) {
        set_err(err, cap, "models_dir and user_models_dir are required");
        return false;
    }
    w = join_path(cfg->user_models_dir, d->weights);
    ok = file_exists(w);
    free(w);
    if (!ok) {
        set_err(err, cap, "%s: weights not downloaded", d->weights);
        return false;
    }
    dst = join_path(cfg->user_models_dir, d->skel);
    if (file_exists(dst)) {
        free(dst);
        return true;             /* the pair is already co-located */
    }
    src = join_path(cfg->models_dir, d->skel);
    if (!src || !file_exists(src)) {
        /* the weightless skeleton ships with the app; losing it is an
         * install problem, not a download problem -- say so plainly */
        set_err(err, cap, "bundled skeleton missing: %S "
                "(reinstall the app)", src ? src : L"");
        free(src);
        free(dst);
        return false;
    }
    ok = CopyFileW(src, dst, FALSE) != 0;
    if (!ok) {
        set_err(err, cap, "cannot copy skeleton to %S (error %lu)",
                dst, GetLastError());
    }
    free(src);
    free(dst);
    return ok;
}

bool eng_model_delete(const eng_config *cfg, eng_model m) {
    const model_def *d = find_def(m);
    wchar_t *p;
    int gone = 0;

    if (!d || !cfg || !cfg->user_models_dir) {
        return false;
    }
    p = join_path(cfg->user_models_dir, d->weights);
    if (p) {
        gone |= DeleteFileW(p) != 0;
        free(p);
    }
    p = join_path(cfg->user_models_dir, d->skel);
    if (p) {
        gone |= DeleteFileW(p) != 0;
        free(p);
    }
    return gone != 0;
}

/* The stored .pth already looks like the finished download: the pinned byte
 * size plus the PyTorch zip magic. A mismatch just means "fetch again". */
static int pth_looks_done(const wchar_t *path, const model_def *d) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned char magic[4] = {0};
    HANDLE f;
    DWORD got = 0;
    int ok = 0;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        return 0;
    }
    if ((((unsigned long long)fa.nFileSizeHigh << 32) |
         fa.nFileSizeLow) != d->weights_size) {
        return 0;
    }
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (ReadFile(f, magic, 4, &got, NULL) && got == 4) {
        ok = magic[0] == 'P' && magic[1] == 'K' &&
             magic[2] == 3 && magic[3] == 4;
    }
    CloseHandle(f);
    return ok;
}

int eng_model_download(const eng_config *cfg, eng_model m,
                       eng_dl_progress_fn cb, void *ud, volatile long *cancel,
                       char *err, size_t cap) {
    const model_def *d = find_def(m);
    const wchar_t *user_models_dir;
    wchar_t *dest = NULL, *tmp = NULL;
    char computed[65];
    unsigned long long got_total = 0;
    dl_ctx ctx;
    int ret = -1;

    if (!d) {
        set_err(err, cap, "model %d has no downloadable weights", (int)m);
        return -1;
    }
    user_models_dir = cfg ? cfg->user_models_dir : NULL;
    if (!user_models_dir || !cfg->models_dir) {
        set_err(err, cap, "models_dir and user_models_dir are required");
        return -1;
    }
    if (cancel && *cancel) {
        return 1;
    }
    ctx.cb = cb;
    ctx.ud = ud;
    CreateDirectoryW(user_models_dir, NULL); /* ok if it already exists */

    dest = join_path(user_models_dir, d->weights);
    tmp = join_path(user_models_dir, d->is_zip ? "weights_download.zip.tmp"
                                             : "weights_download.pth.tmp");
    if (!dest || !tmp) {
        set_err(err, cap, "out of memory");
        goto end;
    }

    /* the weights are already on disk and verify as the finished download
     * (a previous run was interrupted after the .pth landed, or the user
     * dropped the file in by hand): skip the fetch entirely and just make
     * the pair complete -- a "download" click then also repairs a broken
     * half-state for free instead of refetching tens of MB */
    if (pth_looks_done(dest, d)) {
        if (!eng_model_pair_complete(cfg, m, err, cap)) {
            goto end;
        }
        if (cb) {
            cb(ud, d->weights_size, d->weights_size);
        }
        ret = 0;
        goto end;
    }

    /* stage 1: download the remote object (zip for CUGAN, .pth for APISR),
     * verified against its own pinned SHA-256 */
    if (!upd__download_to_file(d->url, tmp, d->download_size,
                               d->is_zip ? d->sha256_zip : d->sha256_pth,
                               dl_progress, &ctx, computed, &got_total,
                               err, cap)) {
        goto end; /* helper filled err */
    }
    if (cancel && *cancel) {
        ret = 1;
        goto end;
    }

    /* stage 2: for a zip, extract the member ending with the .pth name and
     * verify the member's own SHA-256 (denoise.py semantics) */
    if (d->is_zip) {
        size_t zl = 0, ml = 0;
        unsigned char *zb = read_all(tmp, &zl);
        unsigned char *mb = NULL;
        char hex[65];
        HANDLE f = INVALID_HANDLE_VALUE;
        DWORD wrote = 0;
        if (!zb) {
            set_err(err, cap, "cannot read downloaded zip");
            goto end;
        }
        mb = zip_extract(zb, zl, d->weights, &ml, err, cap);
        free(zb);
        if (!mb) {
            goto end;
        }
        if (mem_sha256(mb, ml, hex) || !upd__hex_equal(d->sha256_pth, hex)) {
            free(mb);
            set_err(err, cap, "SHA-256 mismatch on extracted member");
            goto end;
        }
        f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            free(mb);
            set_err(err, cap, "cannot create %S (error %lu)", dest,
                    GetLastError());
            goto end;
        }
        if (!WriteFile(f, mb, (DWORD)ml, &wrote, NULL) || wrote != ml) {
            CloseHandle(f);
            DeleteFileW(dest);
            free(mb);
            set_err(err, cap, "cannot write %S", dest);
            goto end;
        }
        CloseHandle(f);
        free(mb);
        if (cb) {
            cb(ud, ml, ml);
        }
    } else {
        /* the .pth is already hash-verified by the helper: rename into place */
        DeleteFileW(dest);
        if (!MoveFileW(tmp, dest)) {
            set_err(err, cap, "cannot rename %S (error %lu)", dest,
                    GetLastError());
            goto end;
        }
    }

    /* stage 3: the download is not complete until the skeleton sits next to
     * the weights -- ORT resolves external data relative to the .onnx, so a
     * .pth that lands here is invisible to the engine (and to
     * eng_model_status) until the pair is co-located. Roll the .pth back
     * out on failure rather than leave the broken half-state behind. */
    if (!eng_model_pair_complete(cfg, m, err, cap)) {
        DeleteFileW(dest);
        goto end;
    }
    ret = 0;

end:
    if (tmp && GetFileAttributesW(tmp) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tmp);
    }
    free(dest);
    free(tmp);
    return ret;
}
