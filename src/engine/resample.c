/* resample.c -- byte-identical port of Pillow's libImaging/Resample.c 8bpc
 * path for tightly packed RGB8 images (BOX and LANCZOS filters only).
 *
 * Ported from Pillow (src/libImaging/Resample.c, 13.0.0), which carries the
 * Historical Permission Notice and Disclaimer (HPND) -- see
 * THIRD-PARTY-NOTICES.txt. Differences from the original: pixels are 3 bytes
 * per RGB pixel instead of Pillow's UINT32 storage, and the image/threading
 * scaffolding is ours; the coefficient maths, fixed-point accumulation and
 * pass ordering are verbatim so output bytes match PIL Image.resize.
 */
#include "engine.h"

#define _USE_MATH_DEFINES /* MSVC: expose M_PI */
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double (*filter)(double x);
    double support;
} eng_filter;

static double sinc_filter(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    x = x * M_PI;
    return sin(x) / x;
}

static double box_filter(double x) {
    return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
}

static double lanczos_filter(double x) {
    /* truncated sinc */
    if (-3.0 <= x && x < 3.0) {
        return sinc_filter(x) * sinc_filter(x / 3);
    }
    return 0.0;
}

static const eng_filter FILTER_BOX = { box_filter, 0.5 };
static const eng_filter FILTER_LANCZOS = { lanczos_filter, 3.0 };

/* 8 bits for the result. A filter can have negative areas, so the coefficient
 * sum can exceed 1.0 or go negative -- two extra bits of headroom. */
#define PRECISION_BITS (32 - 8 - 2)

static uint8_t clip8(int in) {
    int v = in >> PRECISION_BITS;
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

static int precompute_coeffs(int inSize, float in0, float in1, int outSize,
                             const eng_filter *filterp, int **boundsp,
                             double **kkp) {
    double support, scale, filterscale;
    int xx, x, ksize, xmin, xmax;
    int *bounds;
    double *kk, *k;

    /* prepare for horizontal stretch */
    filterscale = scale = (double)(in1 - in0) / outSize;
    if (filterscale < 1.0) {
        filterscale = 1.0;
    }

    /* determine support size (length of resampling filter) */
    support = filterp->support * filterscale;

    /* maximum number of coeffs */
    ksize = (int)ceil(support) * 2 + 1;

    kk = (double *)malloc((size_t)outSize * (size_t)ksize * sizeof(double));
    if (!kk) {
        return 0;
    }
    bounds = (int *)malloc((size_t)outSize * 2 * sizeof(int));
    if (!bounds) {
        free(kk);
        return 0;
    }

    {
        double inv_filterscale = 1.0 / filterscale;
        for (xx = 0; xx < outSize; xx++) {
            double center = in0 + (xx + 0.5) * scale;
            double ww = 0.0;
            /* round the value */
            xmin = (int)(center - support + 0.5);
            if (xmin < 0) {
                xmin = 0;
            }
            xmax = (int)(center + support + 0.5);
            if (xmax > inSize) {
                xmax = inSize;
            }
            xmax -= xmin;
            k = &kk[xx * ksize];
            for (x = 0; x < xmax; x++) {
                double w = filterp->filter((x + xmin - center + 0.5) *
                                           inv_filterscale);
                k[x] = w;
                ww += w;
            }
            if (ww != 0.0) {
                for (x = 0; x < xmax; x++) {
                    k[x] /= ww;
                }
            }
            /* remaining values stay empty if they are used despite xmax */
            for (; x < ksize; x++) {
                k[x] = 0;
            }
            bounds[xx * 2 + 0] = xmin;
            bounds[xx * 2 + 1] = xmax;
        }
    }
    *boundsp = bounds;
    *kkp = kk;
    return ksize;
}

static void normalize_coeffs_8bpc(int outSize, int ksize, double *prekk) {
    int x;
    int32_t *kk = (int32_t *)prekk; /* reuse the same buffer */
    for (x = 0; x < outSize * ksize; x++) {
        if (prekk[x] < 0) {
            kk[x] = (int)(-0.5 + prekk[x] * (1 << PRECISION_BITS));
        } else {
            kk[x] = (int)(0.5 + prekk[x] * (1 << PRECISION_BITS));
        }
    }
}

/* Horizontal pass on RGB8 rows (row `offset`-based window like Pillow). */
static void resample_horizontal_8bpc(eng_image *imOut, const eng_image *imIn,
                                     int offset, int ksize, int *bounds,
                                     double *prekk) {
    int ss0, ss1, ss2;
    int xx, yy, x, xmin, xmax;
    int32_t *k, *kk;

    kk = (int32_t *)prekk;
    normalize_coeffs_8bpc(imOut->w, ksize, prekk);

    for (yy = 0; yy < imOut->h; yy++) {
        const uint8_t *lineIn = imIn->rgb + (size_t)(yy + offset) * (size_t)imIn->w * 3;
        uint8_t *lineOut = imOut->rgb + (size_t)yy * (size_t)imOut->w * 3;
        for (xx = 0; xx < imOut->w; xx++) {
            xmin = bounds[xx * 2 + 0];
            xmax = bounds[xx * 2 + 1];
            k = &kk[xx * ksize];
            ss0 = ss1 = ss2 = 1 << (PRECISION_BITS - 1);
            for (x = 0; x < xmax; x++) {
                ss0 += lineIn[(x + xmin) * 3 + 0] * k[x];
                ss1 += lineIn[(x + xmin) * 3 + 1] * k[x];
                ss2 += lineIn[(x + xmin) * 3 + 2] * k[x];
            }
            lineOut[xx * 3 + 0] = clip8(ss0);
            lineOut[xx * 3 + 1] = clip8(ss1);
            lineOut[xx * 3 + 2] = clip8(ss2);
        }
    }
}

static void resample_vertical_8bpc(eng_image *imOut, const eng_image *imIn,
                                   int offset, int ksize, int *bounds,
                                   double *prekk) {
    int ss0, ss1, ss2;
    int xx, yy, y, ymin, ymax;
    int32_t *k, *kk;

    (void)offset;
    kk = (int32_t *)prekk;
    normalize_coeffs_8bpc(imOut->h, ksize, prekk);

    for (yy = 0; yy < imOut->h; yy++) {
        uint8_t *lineOut = imOut->rgb + (size_t)yy * (size_t)imOut->w * 3;
        k = &kk[yy * ksize];
        ymin = bounds[yy * 2 + 0];
        ymax = bounds[yy * 2 + 1];
        for (xx = 0; xx < imOut->w; xx++) {
            const uint8_t *col = imIn->rgb + (size_t)ymin * (size_t)imIn->w * 3 + (size_t)xx * 3;
            ss0 = ss1 = ss2 = 1 << (PRECISION_BITS - 1);
            for (y = 0; y < ymax; y++) {
                ss0 += col[(size_t)y * (size_t)imIn->w * 3 + 0] * k[y];
                ss1 += col[(size_t)y * (size_t)imIn->w * 3 + 1] * k[y];
                ss2 += col[(size_t)y * (size_t)imIn->w * 3 + 2] * k[y];
            }
            lineOut[xx * 3 + 0] = clip8(ss0);
            lineOut[xx * 3 + 1] = clip8(ss1);
            lineOut[xx * 3 + 2] = clip8(ss2);
        }
    }
}

bool eng_resize(const eng_image *src, int w, int h, int filter, eng_image *out) {
    const eng_filter *filterp;
    int i, need_horizontal, need_vertical;
    int ybox_first = 0, ybox_last = 0;
    int ksize_horiz = 0, ksize_vert = 0;
    int *bounds_horiz = NULL, *bounds_vert = NULL;
    double *kk_horiz = NULL, *kk_vert = NULL;
    eng_image imTemp = { 0, 0, NULL };
    const eng_image *imIn = src;
    bool horizontal_first, ok = false;

    if (!src || !src->rgb || !out || w <= 0 || h <= 0) {
        return false;
    }
    out->w = 0;
    out->h = 0;
    out->rgb = NULL;

    filterp = (filter == ENG_BOX) ? &FILTER_BOX : &FILTER_LANCZOS;
    /* box = whole input, like Image.resize() */
    need_horizontal = w != src->w;
    need_vertical = h != src->h;

    /* if height is scaled down more than twice the width, run the vertical
     * pass first to make the horizontal pass faster (Pillow's heuristic; it
     * also changes the math, so keep the order verbatim) */
    horizontal_first =
        !((src->h - h) > 0 && (src->h - h) > (src->w - w) * 2);

    if ((need_horizontal && horizontal_first) || need_vertical) {
        ksize_vert = precompute_coeffs(src->h, 0.0f, (float)src->h, h, filterp,
                                       &bounds_vert, &kk_vert);
        if (!ksize_vert) {
            goto end;
        }
    }
    if (need_horizontal) {
        if (horizontal_first) {
            /* first and last used rows in the source image */
            ybox_first = bounds_vert[0];
            ybox_last = bounds_vert[h * 2 - 2] + bounds_vert[h * 2 - 1];
            if (ybox_first != 0) {
                for (i = 0; i < h; i++) {
                    bounds_vert[i * 2] -= ybox_first;
                }
            }
        }
        ksize_horiz = precompute_coeffs(src->w, 0.0f, (float)src->w, w, filterp,
                                        &bounds_horiz, &kk_horiz);
        if (!ksize_horiz) {
            goto end;
        }
    }

    if (horizontal_first) {
        if (need_horizontal) {
            eng_image t = { w, ybox_last - ybox_first, NULL };
            t.rgb = (uint8_t *)malloc((size_t)w * (size_t)t.h * 3);
            if (!t.rgb) {
                goto end;
            }
            resample_horizontal_8bpc(&t, imIn, ybox_first, ksize_horiz,
                                     bounds_horiz, kk_horiz);
            imTemp = t;
            imIn = &imTemp;
        }
        if (need_vertical) {
            out->w = w;
            out->h = h;
            out->rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
            if (!out->rgb) {
                eng_image_free(&imTemp);
                goto end;
            }
            resample_vertical_8bpc(out, imIn, 0, ksize_vert, bounds_vert,
                                   kk_vert);
        }
    } else {
        if (need_vertical) {
            eng_image t = { src->w, h, NULL };
            t.rgb = (uint8_t *)malloc((size_t)src->w * (size_t)h * 3);
            if (!t.rgb) {
                goto end;
            }
            resample_vertical_8bpc(&t, imIn, 0, ksize_vert, bounds_vert,
                                   kk_vert);
            imTemp = t;
            imIn = &imTemp;
        }
        if (need_horizontal) {
            out->w = w;
            out->h = h;
            out->rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
            if (!out->rgb) {
                eng_image_free(&imTemp);
                goto end;
            }
            resample_horizontal_8bpc(out, imIn, 0, ksize_horiz, bounds_horiz,
                                     kk_horiz);
        }
    }
    if (!need_horizontal && !need_vertical) {
        /* nothing to do: plain copy */
        out->w = src->w;
        out->h = src->h;
        out->rgb = (uint8_t *)malloc((size_t)src->w * (size_t)src->h * 3);
        if (out->rgb) {
            memcpy(out->rgb, src->rgb, (size_t)src->w * (size_t)src->h * 3);
        }
    } else if (!out->rgb && imTemp.rgb) {
        /* a single pass ran; it owns the result */
        *out = imTemp;
        imTemp.rgb = NULL;
    }
    ok = (out->rgb != NULL);

end:
    eng_image_free(&imTemp);
    free(bounds_horiz);
    free(kk_horiz);
    free(bounds_vert);
    free(kk_vert);
    return ok;
}
