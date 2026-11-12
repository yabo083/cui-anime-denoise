/* test_engine.c -- unit checks for the native engine's deterministic maths:
 * Python round-half-even sizing, Pillow-identical BOX/LANCZOS output bytes,
 * the L0 pass against reference fixture bytes, the fixed-window tiling math,
 * and cancel semantics. No ONNX Runtime or network needed -- sessions are
 * exercised by the tmp/phase2 parity driver instead.
 *
 * Build (from the repo root, like the other tests -- see tests/README.md):
 *   pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
 *     New-Item -ItemType Directory -Force tmp\engbuild | Out-Null;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /c tests\test_engine.c /Fo:tmp\engbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /c src\engine\resample.c /Fo:tmp\engbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /c src\engine\l0.c /Fo:tmp\engbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /I third_party\onnxruntime\include /c src\engine\engine.c /Fo:tmp\engbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /c third_party\pocketfft\pocketfft_double.c /Fo:tmp\engbuild\;
 *     link /nologo tmp\engbuild\test_engine.obj tmp\engbuild\engine.obj
 *       tmp\engbuild\resample.obj tmp\engbuild\l0.obj tmp\engbuild\pocketfft_double.obj
 *       dxgi.lib dxguid.lib /OUT:tmp\engbuild\test_engine.exe"
 *   tmp\engbuild\test_engine.exe
 *
 * Fixtures below were produced by the reference denoise.py (Pillow 13.0.0 /
 * numpy) and are frozen -- regenerate only against that source of truth.
 */
#include "engine/engine.h"
#include "engine/engine_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int fails, checks;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            fails++;                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);            \
        }                                                                    \
    } while (0)

/* ---- fixtures (denoise.py / Pillow output, see header comment) ----------- */

/* rng.rand(6,8,3) seed 7 -> uint8; resize inputs */
static const uint8_t IMG8[6 * 8 * 3] = {
    19,199,112,184,249,137,128,18,68,127,173,205,97,17,73,232,54,115,237,6,
    153,242,59,140,232,34,133,191,171,119,52,125,95,122,93,214,196,80,146,
    70,115,90,168,94,117,183,105,231,46,189,108,109,162,133,106,0,24,181,
    134,178,244,174,14,79,151,60,246,241,216,120,215,33,79,118,189,124,35,
    88,83,77,42,106,114,198,203,133,117,198,226,172,204,239,10,223,71,121,
    203,183,38,168,18,91,207,109,153,186,209,194,2,107,118,14,138,155,211,
    240,33,59,168,34,57,147,43,199,219,9,136,203,249,70,43,224,232,50,113,
    183,216,43,170,206,140,42,9,72
};

/* Image.fromarray(IMG8).resize((5,4), Image.BOX) */
static const uint8_t IMG8_BOX_5x4[5 * 4 * 3] = {
    157,164,126,90,72,82,136,91,160,151,85,103,208,67,161,78,176,121,106,0,
    24,213,154,96,79,151,60,183,228,125,144,89,102,145,93,98,125,141,157,
    106,182,164,175,180,50,128,183,26,136,203,249,151,47,169,183,216,43,106,
    108,106
};

/* Image.fromarray(IMG8).resize((11,9), Image.LANCZOS) */
static const uint8_t IMG8_LZ_11x9[11 * 9 * 3] = {
    0,210,105,103,255,138,196,171,108,134,5,66,135,151,188,91,119,145,115,2,
    53,243,48,112,255,13,158,241,20,147,245,63,122,127,97,124,181,181,134,
    171,183,104,85,79,91,105,125,197,131,87,184,139,35,106,160,79,109,184,
    44,107,213,40,160,225,69,208,230,28,127,218,115,131,130,173,100,51,114,
    92,98,92,191,207,92,193,181,106,110,51,122,79,123,124,103,187,126,176,
    160,131,215,75,166,96,105,189,135,114,115,96,102,14,38,154,95,164,245,
    170,123,199,159,8,57,139,45,187,201,199,206,233,162,99,214,36,13,169,
    164,69,145,148,108,65,59,86,22,19,113,91,163,214,150,132,241,170,46,
    170,200,108,214,250,137,220,207,97,178,130,58,106,128,183,125,53,135,
    125,16,53,97,91,60,105,130,179,140,122,170,174,139,139,176,213,189,193,
    250,32,209,165,25,213,63,135,209,191,58,175,66,79,163,0,107,200,107,
    132,208,209,190,93,174,154,2,101,133,16,139,176,180,227,58,176,218,6,73,
    157,52,130,175,26,165,143,20,199,135,100,179,163,221,142,144,229,120,86,
    159,111,84,99,82,165,88,182,236,103,131,167,76,16,86,40,26,134,57,136,
    193,0,211,255,70,119,203,255,44,61,255,157,0,172,255,87,80,207,220,31,
    188,255,120,110,102,132,36,0,69
};

/* flat + ripples + hard edge, 6x8 -- L0 input (uint8) */
static const uint8_t L0_SRC[6 * 8 * 3] = {
    3,4,1,3,5,5,1,1,0,2,0,2,207,205,207,207,204,207,205,206,
    205,208,206,205,3,4,2,1,2,5,5,3,5,4,2,0,207,207,206,205,
    206,206,205,205,206,208,207,205,1,2,1,1,3,0,2,2,4,4,4,4,
    206,207,206,207,206,206,204,205,209,205,208,207,4,2,3,0,0,1,1,1,
    1,3,1,5,207,207,208,205,209,208,208,206,208,208,207,209,3,2,3,1,
    1,4,4,5,4,3,0,2,209,208,206,206,205,205,204,209,207,205,206,204,
    1,4,3,2,1,3,5,2,0,0,0,5,207,208,205,205,208,209,206,204,
    208,205,208,205
};

/* l0_smooth(L0_SRC/255, 0.02) -> uint8, from denoise.py (float64 path) */
static const uint8_t L0_OUT[6 * 8 * 3] = {
    2,2,3,2,2,3,2,2,3,2,2,3,206,207,207,206,207,207,206,207,
    207,206,207,207,2,2,3,2,2,3,2,2,3,2,2,3,206,207,207,206,
    207,207,206,207,207,206,207,207,2,2,3,2,2,3,2,2,3,2,2,3,
    206,207,207,206,207,207,206,207,207,206,207,207,2,2,3,2,2,3,2,2,
    3,2,2,3,206,207,207,206,207,207,206,207,207,206,207,206,2,2,3,2,
    2,3,2,2,3,2,2,3,206,207,207,206,207,207,206,207,207,206,207,207,
    2,2,3,2,2,3,2,2,3,2,2,3,206,207,207,206,207,207,206,207,
    207,206,207,207
};

/* ------------------------------- tests ----------------------------------- */

static void test_round(void) {
    CHECK(eng_round_half_even(1024 * 0.6) == 614, "round 1024*.6 -> 614");
    CHECK(eng_round_half_even(1536 * 0.6) == 922, "round 1536*.6 -> 922");
    CHECK(eng_round_half_even(756 * 0.6) == 454, "round 756*.6 -> 454");
    CHECK(eng_round_half_even(0.5) == 0, "round .5 -> 0 (half-even)");
    CHECK(eng_round_half_even(1.5) == 2, "round 1.5 -> 2");
    CHECK(eng_round_half_even(2.5) == 2, "round 2.5 -> 2 (half-even)");
    CHECK(eng_round_half_even(3.5) == 4, "round 3.5 -> 4");
    CHECK(eng_round_half_even(-2.5) == -2, "round -2.5 -> -2");
}

static void test_resize(void) {
    eng_image src = {8, 6, (uint8_t *)IMG8}, out = {0, 0, NULL};
    CHECK(eng_resize(&src, 5, 4, ENG_BOX, &out), "BOX resize ran");
    CHECK(out.rgb && !memcmp(out.rgb, IMG8_BOX_5x4, sizeof(IMG8_BOX_5x4)),
          "BOX 5x4 bytes == PIL");
    eng_image_free(&out);
    CHECK(eng_resize(&src, 11, 9, ENG_LANCZOS, &out), "LANCZOS resize ran");
    CHECK(out.rgb && !memcmp(out.rgb, IMG8_LZ_11x9, sizeof(IMG8_LZ_11x9)),
          "LANCZOS 11x9 bytes == PIL");
    eng_image_free(&out);
    /* identity = copy */
    CHECK(eng_resize(&src, 8, 6, ENG_LANCZOS, &out), "identity resize ran");
    CHECK(out.rgb && !memcmp(out.rgb, IMG8, sizeof(IMG8)), "identity bytes");
    eng_image_free(&out);
}

static void test_l0(void) {
    eng_image src = {8, 6, (uint8_t *)L0_SRC}, out = {0, 0, NULL};
    CHECK(eng_l0(&src, 0.02, &out, 4, NULL, NULL, NULL) == 0, "l0 ran");
    /* The reference is float64 numpy/pocketfft; our C pocketfft agrees to ~1
     * ulp per iteration (verified: max 2.2e-16 after iter 1), and the ~21 beta
     * iterations amplify that into a few pixels landing on the other side of
     * the .5 rounding boundary. Assert max diff <= 1 level, >= 80% exact, and
     * that smoothing actually changed pixels. */
    if (out.rgb) {
        int i, maxd = 0, eq = 0, changed = 0;
        for (i = 0; i < 6 * 8 * 3; i++) {
            int d = (int)out.rgb[i] - (int)L0_OUT[i];
            if (d < 0) d = -d;
            if (d > maxd) maxd = d;
            if (d == 0) eq++;
            if (out.rgb[i] != L0_SRC[i]) changed++;
        }
        CHECK(maxd <= 1, "l0 max diff <= 1 level vs denoise.py fixture");
        CHECK(eq >= (int)(sizeof(L0_OUT) * 8 / 10), "l0 >=80% exact vs fixture");
        CHECK(changed > 50, "l0 actually smoothed the ripples");
    }
    eng_image_free(&out);
    /* lam = 0 -> copy */
    CHECK(eng_l0(&src, 0.0, &out, 0, NULL, NULL, NULL) == 0, "l0 lam=0 ran");
    CHECK(out.rgb && !memcmp(out.rgb, L0_SRC, sizeof(L0_SRC)), "lam=0 copies");
    eng_image_free(&out);
    /* invalid lambda rejected */
    CHECK(eng_l0(&src, -1.0, &out, 0, NULL, NULL, NULL) == -1, "lam<0 fails");
    CHECK(eng_l0(&src, NAN, &out, 0, NULL, NULL, NULL) == -1,
          "lam NaN fails");
}

/* windows must cover [0,n); between window i and i+1 only `pad` may overlap */
static void check_axis(int n, int tile, int pad, const char *what) {
    int starts[512], sizes[512], k, i, prev_end = 0;
    k = eng__axis_windows(n, tile, pad, starts, sizes, 512);
    CHECK(k >= 1, what);
    for (i = 0; i < k; i++) {
        CHECK(starts[i] >= 0 && starts[i] + sizes[i] <= n, what);
        CHECK(sizes[i] <= tile, what);
        /* interior starts (not at 0) crop pad on the left: their kept region
         * starts at starts[i]+pad and must meet the previous kept end */
        {
            int keep_l = starts[i] > 0 ? pad : 0;
            int keep_r = starts[i] + sizes[i] < n ? pad : 0;
            int kl = starts[i] + keep_l;
            int kr = starts[i] + sizes[i] - keep_r;
            CHECK(i == 0 || kl <= prev_end, what); /* no gap */
            prev_end = kr;
        }
    }
    CHECK(prev_end == n, what);
}

static void test_tiling_math(void) {
    check_axis(300, 512, 64, "n < tile: one true-size window");
    check_axis(512, 512, 64, "n == tile: one window");
    check_axis(513, 512, 64, "n == tile+1: two overlapping windows");
    check_axis(2000, 512, 64, "n large: coverage");
    check_axis(2000, 384, 64, "n large tile 384");
    check_axis(1024, 256, 64, "n large tile 256");
    check_axis(129, 512, 64, "n much smaller than tile");
    check_axis(1, 512, 64, "n == 1");
    /* beyond 256 windows the axis array fills and the caller must refuse:
     * eng__axis_windows stops at cap and the kept region no longer reaches n */
    {
        int starts[512], sizes[512], k;
        k = eng__axis_windows(40000, 256, 64, starts, sizes, 256);
        CHECK(k == 256, "cap: returns exactly 256 windows");
        CHECK(starts[k - 1] + sizes[k - 1] < 40000,
              "cap: coverage short -> caller rejects");
    }
}

static void test_cancel(void) {
    volatile long cancel = 1;
    eng_image src = {8, 6, (uint8_t *)L0_SRC}, out = {0, 0, NULL};
    CHECK(eng_l0(&src, 0.02, &out, 4, NULL, NULL, &cancel) == 1,
          "pre-set cancel returns 1");
    CHECK(out.rgb == NULL, "cancelled output empty");
}

int main(void) {
    test_round();
    test_resize();
    test_l0();
    test_tiling_math();
    test_cancel();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
