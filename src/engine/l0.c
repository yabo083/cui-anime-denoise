/* l0.c -- L0 gradient minimization (Xu et al. 2011), the exact algorithm of
 * the reference denoise.py's l0_smooth(), in double precision on RGB8.
 *
 *   S = image / 255 (HxWx3 float64)
 *   otf[i,j] = |Fw[j]|^2 + |Fh[i]|^2      (rank-1: 1D spectra of the two
 *                                          finite-difference kernels)
 *   N1 = fft2(S) (per channel)
 *   beta = 2*lam; while beta < 1e5:
 *       h = roll(S,-1,x) - S; v = roll(S,-1,y) - S
 *       t = (h^2+v^2) summed over channels < lam/beta;  h[t]=v[t]=0
 *       div = (roll(h,1,x)-h) + (roll(v,1,y)-v)
 *       S = real(ifft2((N1 + beta*fft2(div)) / (1 + beta*otf)))
 *       beta *= 2
 *   out = clip(S,0,1)
 *
 * np.roll == periodic/circular boundary. Channels are independent except for
 * the shared keep-mask. 2D FFTs run as row transforms + a transpose, so all
 * pocketfft calls are on contiguous rows. Workers are created per call at
 * THREAD_PRIORITY_BELOW_NORMAL.
 */
#include "engine.h"

#include <math.h>
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

#include "../../third_party/pocketfft/pocketfft_double.h"

typedef struct {
    int w, h;
    double *s[3];      /* planar image, one HxW plane per channel */
    double *div[3];    /* scratch real plane per channel */
    double *keep;      /* shared mask, HxW (1.0 keep, 0.0 drop) */
    double *otf_i;     /* |Fh[i]|^2 (H) and |Fw[j]|^2 (W) */
    double *otf_j;
    double *n1[3];     /* transposed spectrum per channel, W rows x H cols complex */
    double *f[3];      /* transposed spectrum scratch per channel */
    double *c[3];      /* row-major spectrum scratch per channel, H rows x W cols */
} l0_bufs;

typedef struct {
    l0_bufs *b;
    int r0, r1;        /* row range [r0,r1) this worker owns */
    cfft_plan pw, ph;  /* row-length (W) / col-length (H) transform plans */
    volatile long *cancel;
} l0_worker;

#define CANCELLED(w) ((w)->cancel && *(w)->cancel)

static void c2(double *p, double re, double im) { p[0] = re; p[1] = im; }

/* ---------------------- parallel stage bodies ----------------------------- */

/* keep[i,j] = 1 iff sum_c (gx^2 + gy^2) >= lam/beta */
static void phase_mask(l0_worker *w, double thr) {
    l0_bufs *b = w->b;
    for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
        int in = i + 1 < b->h ? i + 1 : 0;
        for (int j = 0; j < b->w; j++) {
            int jn = j + 1 < b->w ? j + 1 : 0;
            double e = 0.0;
            for (int c = 0; c < 3; c++) {
                size_t p = (size_t)i * b->w + j;
                double gx = b->s[c][(size_t)i * b->w + jn] - b->s[c][p];
                double gy = b->s[c][(size_t)in * b->w + j] - b->s[c][p];
                e += gx * gx + gy * gy;
            }
            b->keep[(size_t)i * b->w + j] = (e < thr) ? 0.0 : 1.0;
        }
    }
}

/* div[c] = gx(i,j-1)-gx(i,j) + gy(i-1,j)-gy(i,j), gradients zeroed by `keep` */
static void phase_div(l0_worker *w, int c) {
    l0_bufs *b = w->b;
    const double *S = b->s[c];
    const double *m = b->keep;
    for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
        int im = i > 0 ? i - 1 : b->h - 1;
        int in = i + 1 < b->h ? i + 1 : 0;
        for (int j = 0; j < b->w; j++) {
            int jm = j > 0 ? j - 1 : b->w - 1;
            int jn = j + 1 < b->w ? j + 1 : 0;
            size_t p = (size_t)i * b->w + j;
            size_t pm = (size_t)i * b->w + jm;
            size_t pv = (size_t)im * b->w + j;
            double gx_ij = S[(size_t)i * b->w + jn] - S[p];
            double gx_ijm = S[p] - S[pm];
            double gy_ij = S[(size_t)in * b->w + j] - S[p];
            double gy_imj = S[p] - S[(size_t)im * b->w + j];
            /* (roll(h,1,1) - h) + (roll(v,1,0) - v) -- keep np's grouping:
             * (a - b) + (c - d), not ((a - b) + c) - d, for bit-equal fp */
            b->div[c][p] = (m[pm] * gx_ijm - m[p] * gx_ij)
                         + (m[pv] * gy_imj - m[p] * gy_ij);
        }
    }
}

/* rows of div -> complex (imag 0), forward cfft into c[] row-major */
static void phase_frow(l0_worker *w, int c) {
    l0_bufs *b = w->b;
    for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
        double *dst = b->c[c] + (size_t)i * b->w * 2;
        for (int j = 0; j < b->w; j++) {
            c2(dst + (size_t)j * 2, b->div[c][(size_t)i * b->w + j], 0.0);
        }
        cfft_forward(w->pw, dst, 1.0);
    }
}

/* transpose row-major complex A(src_rows x src_cols) -> T(src_cols x src_rows);
 * this worker owns source rows [r0,r1). */
static void phase_transpose(l0_worker *w, const double *A, double *T,
                            int src_cols, int dst_cols) {
    for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
        for (int j = 0; j < src_cols; j++) {
            size_t a = ((size_t)i * src_cols + j) * 2;
            size_t t = ((size_t)j * dst_cols + i) * 2;
            T[t] = A[a];
            T[t + 1] = A[a + 1];
        }
    }
}

/* row transforms on transposed spectrum F (W rows x H cols), forward or back.
 * np.ifft2 normalizes by 1/(H*W) = (1/W)(1/H), applied on the two backward
 * passes here and in the inverse row pass below. */
static void phase_fcol(l0_worker *w, int c, int backward) {
    l0_bufs *b = w->b;
    for (int j = w->r0; j < w->r1 && !CANCELLED(w); j++) {
        double *row = b->f[c] + (size_t)j * b->h * 2;
        if (backward) {
            cfft_backward(w->ph, row, 1.0 / (double)b->h);
        } else {
            cfft_forward(w->ph, row, 1.0);
        }
    }
}

/* F = (N1 + beta*F) / (1 + beta*otf) on transposed spectra W x H */
static void phase_update(l0_worker *w, int c, double beta) {
    l0_bufs *b = w->b;
    for (int j = w->r0; j < w->r1 && !CANCELLED(w); j++) {
        double gj = b->otf_j[j];          /* |Fw[j]|^2 constant over i */
        for (int i = 0; i < b->h; i++) {
            size_t p = ((size_t)j * b->h + i) * 2;
            double den = 1.0 + beta * (gj + b->otf_i[i]);
            double re = (b->n1[c][p] + beta * b->f[c][p]) / den;
            double im = (b->n1[c][p + 1] + beta * b->f[c][p + 1]) / den;
            b->f[c][p] = re;
            b->f[c][p + 1] = im;
        }
    }
}

/* after inverse transform, C row-major complex -> real image plane */
static void phase_real(l0_worker *w, int c) {
    l0_bufs *b = w->b;
    for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
        double *row = b->c[c] + (size_t)i * b->w * 2;
        for (int j = 0; j < b->w; j++) {
            b->s[c][(size_t)i * b->w + j] = row[(size_t)j * 2];
        }
    }
}

typedef struct {
    int stage;              /* 0 mask, 1 div, 2 frow, 4 fcol, 5 update, 6 real */
    int channel;
    int backward;
    int transpose_fw;       /* 0: c(HxW)->f(WxH) fwd; 1: f(WxH)->c(HxW) inv */
    double arg;
} l0_job;

static void run_stage(const l0_job *j, l0_worker *w) {
    l0_bufs *b = w->b;
    int c = j->channel;
    switch (j->stage) {
        case 0: phase_mask(w, j->arg); break;
        case 1: phase_div(w, c); break;
        case 2: phase_frow(w, c); break;
        case 3:
            if (j->transpose_fw) {
                /* f(W x H) -> c(H x W) on the way back */
                phase_transpose(w, b->f[c], b->c[c], b->h, b->w);
            } else {
                /* c(H x W) -> f(W x H) on the way in */
                phase_transpose(w, b->c[c], b->f[c], b->w, b->h);
            }
            break;
        case 4:
            if (j->transpose_fw) {
                /* inverse row pass: rows of c[] (H rows x W cols), length W */
                for (int i = w->r0; i < w->r1 && !CANCELLED(w); i++) {
                    cfft_backward(w->pw, b->c[c] + (size_t)i * b->w * 2,
                                  1.0 / (double)b->w);
                }
            } else {
                phase_fcol(w, c, j->backward);
            }
            break;
        case 5: phase_update(w, c, j->arg); break;
        case 6: phase_real(w, c); break;
    }
}

static DWORD WINAPI worker_main(void *pv) {
    char *block = (char *)pv;
    l0_job *j = (l0_job *)block;
    l0_worker *w = (l0_worker *)(block + sizeof(l0_job));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    run_stage(j, w);
    return 0;
}

/* Split `n` rows across `plans` workers; thread 0's share runs on the caller,
 * the rest on CreateThread'd workers. Returns 0 ok, -1 spawn failure. */
static int run_parallel(const l0_job *job, int n, int nthreads,
                        volatile long *cancel, HANDLE *handles,
                        char **blocks, l0_worker *plans) {
    int t, spawned = 0;
    int nw = nthreads < n ? nthreads : n;
    int step = (n + nw - 1) / nw;
    for (t = 0; t < nw; t++) {
        int r0 = t * step;
        int r1 = r0 + step < n ? r0 + step : n;
        l0_job *j;
        l0_worker *w;
        if (r0 >= r1) {
            break;
        }
        j = (l0_job *)blocks[t];
        w = (l0_worker *)(blocks[t] + sizeof(l0_job));
        *j = *job;
        *w = plans[t];
        w->r0 = r0;
        w->r1 = r1;
        w->cancel = cancel;
        if (t == 0) {
            run_stage(j, w);
            continue;
        }
        handles[spawned] = CreateThread(NULL, 0, worker_main, blocks[t],
                                        0, NULL);
        if (!handles[spawned]) {
            WaitForMultipleObjects((DWORD)spawned, handles, TRUE, INFINITE);
            for (t = 0; t < spawned; t++) {
                CloseHandle(handles[t]);
            }
            return -1;
        }
        spawned++;
    }
    if (spawned) {
        WaitForMultipleObjects((DWORD)spawned, handles, TRUE, INFINITE);
        for (t = 0; t < spawned; t++) {
            CloseHandle(handles[t]);
        }
    }
    return 0;
}

int eng_l0(const eng_image *in, double lam, eng_image *out, int threads,
           eng_progress_fn cb, void *ud, volatile long *cancel) {
    l0_bufs b;
    int W, H, c, i, j, nt = 0, it;
    double beta, beta_max = 1e5;
    cfft_plan pw = NULL, ph = NULL;
    int ret = -1;
    char **blocks = NULL;
    HANDLE *handles = NULL;
    l0_worker *plans = NULL;

    memset(&b, 0, sizeof(b));
    if (!in || !in->rgb || !out || !isfinite(lam) || lam < 0.0) {
        return -1;
    }
    W = in->w;
    H = in->h;
    out->w = W;
    out->h = H;
    out->rgb = NULL;
    if (lam == 0.0) {
        out->rgb = (uint8_t *)malloc((size_t)W * H * 3);
        if (!out->rgb) return -1;
        memcpy(out->rgb, in->rgb, (size_t)W * H * 3);
        return 0;
    }
    if (threads <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        threads = (int)si.dwNumberOfProcessors;
        if (threads < 1) threads = 1;
    }

    /* planes */
    for (c = 0; c < 3; c++) {
        b.s[c] = (double *)malloc((size_t)W * H * sizeof(double));
        b.div[c] = (double *)malloc((size_t)W * H * sizeof(double));
        b.n1[c] = (double *)malloc((size_t)W * H * 2 * sizeof(double));
        b.f[c] = (double *)malloc((size_t)W * H * 2 * sizeof(double));
        b.c[c] = (double *)malloc((size_t)W * H * 2 * sizeof(double));
        if (!b.s[c] || !b.div[c] || !b.n1[c] || !b.f[c] || !b.c[c]) goto end;
    }
    b.keep = (double *)malloc((size_t)W * H * sizeof(double));
    b.otf_i = (double *)malloc((size_t)H * sizeof(double));
    b.otf_j = (double *)malloc((size_t)W * sizeof(double));
    b.w = W;
    b.h = H;
    if (!b.keep || !b.otf_i || !b.otf_j) goto end;

    pw = make_cfft_plan((size_t)W);
    ph = make_cfft_plan((size_t)H);
    if (!pw || !ph) goto end;

    nt = threads;
    /* per-thread plans + arg blocks (pocketfft plans are read-only during exec,
     * but a plan per thread keeps the contract trivially safe) */
    plans = (l0_worker *)calloc((size_t)nt, sizeof(l0_worker));
    blocks = (char **)calloc((size_t)nt, sizeof(char *));
    handles = (HANDLE *)calloc((size_t)nt, sizeof(HANDLE));
    if (!plans || !blocks || !handles) goto end;
    for (i = 0; i < nt; i++) {
        blocks[i] = (char *)calloc(1, sizeof(l0_job) + sizeof(l0_worker));
        plans[i].pw = make_cfft_plan((size_t)W);
        plans[i].ph = make_cfft_plan((size_t)H);
        plans[i].b = &b;
        if (!blocks[i] || !plans[i].pw || !plans[i].ph) goto end;
    }

    /* S = rgb/255 planar; N1 = fft2(S) stored transposed (W x H complex) */
    for (c = 0; c < 3; c++) {
        for (i = 0; i < H; i++) {
            double *row = b.c[c] + (size_t)i * W * 2;
            const uint8_t *px = in->rgb + (size_t)i * W * 3;
            for (j = 0; j < W; j++) {
                double v = px[(size_t)j * 3 + c] / 255.0;
                b.s[c][(size_t)i * W + j] = v;
                c2(row + (size_t)j * 2, v, 0.0);
            }
            cfft_forward(pw, row, 1.0);
        }
        /* transpose into N1 (W x H), then row-transform length H */
        for (i = 0; i < H; i++) {
            for (j = 0; j < W; j++) {
                size_t a = ((size_t)i * W + j) * 2;
                size_t t = ((size_t)j * H + i) * 2;
                b.n1[c][t] = b.c[c][a];
                b.n1[c][t + 1] = b.c[c][a + 1];
            }
        }
        for (j = 0; j < W; j++) {
            cfft_forward(ph, b.n1[c] + (size_t)j * H * 2, 1.0);
        }
    }

    /* 1D spectra of the kernels: fx row0 = [-1,0,...,1] over W;
       fy col0 = [-1,0,...,1] over H */
    {
        double *row = (double *)calloc((size_t)(W > H ? W : H) * 2, sizeof(double));
        if (!row) goto end;
        row[0] = -1.0;
        row[(W - 1) * 2] = 1.0;
        cfft_forward(pw, row, 1.0);
        for (j = 0; j < W; j++) {
            b.otf_j[j] = row[j * 2] * row[j * 2] + row[j * 2 + 1] * row[j * 2 + 1];
        }
        memset(row, 0, (size_t)H * 2 * sizeof(double));
        row[0] = -1.0;
        row[(H - 1) * 2] = 1.0;
        cfft_forward(ph, row, 1.0);
        for (i = 0; i < H; i++) {
            b.otf_i[i] = row[i * 2] * row[i * 2] + row[i * 2 + 1] * row[i * 2 + 1];
        }
        free(row);
    }

    {
        l0_job job;
        memset(&job, 0, sizeof(job));
        beta = 2 * lam;
        it = 0;
        while (beta < beta_max) {
            double thr = lam / beta;
            if (cancel && *cancel) { ret = 1; goto end; }

            job.stage = 0; job.arg = thr;
            if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                goto end;
            for (c = 0; c < 3; c++) {
                job.channel = c;
                job.transpose_fw = 0;
                job.backward = 0;
                job.stage = 1;   /* div over image rows */
                if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 2;   /* fft rows of div -> c[] */
                if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 3;   /* transpose c(HxW) -> f(WxH) */
                if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 4;   /* fft columns (rows of f), forward */
                job.backward = 0;
                if (run_parallel(&job, W, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 5; job.arg = beta;   /* spectral update */
                if (run_parallel(&job, W, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 4; job.backward = 1; /* inverse columns */
                if (run_parallel(&job, W, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 3; job.transpose_fw = 1; /* f(WxH) -> c(HxW) */
                if (run_parallel(&job, W, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 4;   /* inverse rows (rows of c, length W) */
                if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                    goto end;
                job.stage = 6;   /* real -> S */
                if (run_parallel(&job, H, nt, cancel, handles, blocks, plans))
                    goto end;
            }
            beta *= 2.0;
            it++;
            if (cb) {
                /* total iterations ~= log2(beta_max / (2*lam)) + 1 */
                float total = (float)(log(beta_max / (2 * lam)) / log(2.0)) + 1.0f;
                cb(ud, "smooth", (float)it / (total > 1 ? total : 1));
            }
        }
    }

    out->rgb = (uint8_t *)malloc((size_t)W * H * 3);
    if (!out->rgb) goto end;
    for (i = 0; i < H; i++) {
        for (j = 0; j < W; j++) {
            for (c = 0; c < 3; c++) {
                double v = b.s[c][(size_t)i * W + j];
                long q;
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                q = (long)(v * 255.0 + 0.5);   /* (a*255+0.5) truncate -> uint8 */
                out->rgb[((size_t)i * W + j) * 3 + c] = (uint8_t)q;
            }
        }
    }
    ret = 0;

end:
    for (i = 0; plans && i < nt; i++) {
        if (plans[i].pw) destroy_cfft_plan(plans[i].pw);
        if (plans[i].ph) destroy_cfft_plan(plans[i].ph);
    }
    for (i = 0; blocks && i < nt; i++) free(blocks[i]);
    free(plans); free(blocks); free(handles);
    if (pw) destroy_cfft_plan(pw);
    if (ph) destroy_cfft_plan(ph);
    for (c = 0; c < 3; c++) {
        free(b.s[c]); free(b.div[c]); free(b.n1[c]); free(b.f[c]); free(b.c[c]);
    }
    free(b.keep); free(b.otf_i); free(b.otf_j);
    if (ret != 0 && out->rgb) {
        free(out->rgb);
        out->rgb = NULL;
    }
    return ret;
}
