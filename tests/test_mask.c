/* Self-check for src/mask.c: brush geometry, overwrite semantics, lasso fill,
 * tile-journal undo/redo + memory budget, feathering, edge map / live wire /
 * snap, and the 4096x4096 timings from the acceptance list.
 *
 * Build and run (from the repository root, see tests/README.md):
 *
 *   pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet; cl /nologo /utf-8 /W4 /O2
 *       /I src tests\test_mask.c src\mask.c tests\img_stub.c /Fe:tmp\test_mask.exe"
 *   .\tmp\test_mask.exe
 *
 * The PNG round-trip only runs when src/img.c is linked instead of
 * tests/img_stub.c; otherwise it prints SKIP.
 */
#include "../src/img.h"
#include "../src/mask.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal, deliberately not in mask.h: live journal payload in bytes. */
extern size_t mask_hist_bytes_used(const mask_hist *h);

static int failures;
static int checks;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static int px(const mask *m, int x, int y) { return m->px[(size_t)y * (size_t)m->w + (size_t)x]; }

static double now_ms(void) { return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC; }

/* --------------------------------------------------------------- brush ---- */

static void test_brush(void) {
    printf("brush: 100x100, y=50, diameter 10\n");
    mask m = { 0, 0, 0 };
    if (!mask_init(&m, 100, 100)) {
        check(0, "mask_init");
        return;
    }
    mask_brush b;
    b.diameter = 10.0f;
    b.value = MASK_VALUE_BRUSH;
    mask_stroke_segment(&m, NULL, 10.0f, 50.0f, 90.0f, 50.0f, &b);

    check(px(&m, 50, 50) == 230, "centre row = 230");
    check(px(&m, 50, 46) == 230, "y-4 = 230");
    check(px(&m, 50, 54) == 230, "y+4 = 230");
    check(px(&m, 50, 44) == 0, "y-6 = 0");
    check(px(&m, 50, 56) == 0, "y+6 = 0");
    check(px(&m, 10, 50) == 230, "round cap covers the start");
    check(px(&m, 5, 50) == 230, "cap reaches 5 px past the start point");
    check(px(&m, 94, 50) == 230, "round cap covers the end");
    check(px(&m, 2, 50) == 0, "before the start cap = 0");
    check(px(&m, 96, 50) == 0, "past the end cap = 0");

    /* coverage, not accumulation */
    mask_brush b2;
    b2.diameter = 10.0f;
    b2.value = 100;
    mask_stroke_segment(&m, NULL, 10.0f, 50.0f, 90.0f, 50.0f, &b2);
    check(px(&m, 50, 50) == 100, "second stroke replaces (230 -> 100)");

    b2.value = 0;
    mask_stroke_segment(&m, NULL, 10.0f, 50.0f, 90.0f, 50.0f, &b2);
    check(px(&m, 50, 50) == 0, "erase stroke (value 0) clears");
    check(!mask_any(&m), "mask_any false after erasing the only stroke");

    /* a dot is a single round cap */
    b2.value = 230;
    mask_clear(&m);
    mask_stroke_dot(&m, NULL, 50.0f, 50.0f, &b);
    check(px(&m, 50, 50) == 230 && px(&m, 54, 50) == 230 && px(&m, 56, 50) == 0,
          "dot = circle of diameter 10");
    mask_free(&m);
}

/* ------------------------------------------------------------- polygon ---- */

static void test_polygon(void) {
    printf("lasso: square (10,10)-(40,40) value 200\n");
    mask m = { 0, 0, 0 };
    if (!mask_init(&m, 100, 100)) {
        check(0, "mask_init");
        return;
    }
    const float sq[8] = { 10.0f, 10.0f, 40.0f, 10.0f, 40.0f, 40.0f, 10.0f, 40.0f };
    mask_fill_polygon(&m, NULL, sq, 4, 200);

    int area = 0;
    for (size_t i = 0; i < (size_t)m.w * (size_t)m.h; i++)
        if (m.px[i] == 200) area++;
    printf("        painted area = %d px (expect 900 +/- 60)\n", area);
    check(area >= 840 && area <= 960, "area 900 +/- 60");

    check(px(&m, 25, 25) == 200, "inside = 200");
    check(px(&m, 39, 25) == 200, "innermost boundary column included");
    check(px(&m, 5, 25) == 0, "outside left = 0");
    check(px(&m, 45, 25) == 0, "outside right = 0");
    check(px(&m, 25, 5) == 0, "outside top = 0");
    check(px(&m, 25, 45) == 0, "outside bottom = 0");
    check(px(&m, 40, 25) == 0, "one px past the right edge = 0");

    /* degenerate polygons below 4 px are ignored */
    const float speck[6] = { 50.0f, 50.0f, 51.0f, 50.0f, 50.0f, 51.0f };
    mask_clear(&m);
    mask_fill_polygon(&m, NULL, speck, 3, 255);
    check(!mask_any(&m), "sub-4px polygon ignored");

    /* concave shape: four crossings on the middle rows */
    mask_clear(&m);
    const float u[16] = { 10, 10, 40, 10, 40, 40, 30, 40,
                          30, 20, 20, 20, 20, 40, 10, 40 };
    mask_fill_polygon(&m, NULL, u, 8, 200);
    check(px(&m, 25, 30) == 0, "concave notch stays empty");
    check(px(&m, 15, 30) == 200 && px(&m, 35, 30) == 200, "concave arms are filled");
    check(px(&m, 25, 15) == 200, "concave base is filled");

    /* value 0 subtracts */
    mask_fill_polygon(&m, NULL, sq, 4, 200);
    const float small[8] = { 20.0f, 20.0f, 30.0f, 20.0f, 30.0f, 30.0f, 20.0f, 30.0f };
    mask_fill_polygon(&m, NULL, small, 4, 0);
    check(px(&m, 25, 25) == 0 && px(&m, 35, 35) == 200, "value 0 subtracts inside the lasso");
    mask_free(&m);
}

/* ------------------------------------------------------------ undo/redo ---- */

static void test_undo(void) {
    printf("undo/redo\n");
    mask m = { 0, 0, 0 };
    mask_hist *h = mask_hist_new(0);
    if (!mask_init(&m, 100, 100) || !h) {
        check(0, "init");
        mask_free(&m);
        mask_hist_free(h);
        return;
    }
    mask_brush b;
    b.diameter = 10.0f;
    b.value = 230;

    mask_stroke_begin(&m, h);
    mask_stroke_segment(&m, h, 10.0f, 50.0f, 90.0f, 50.0f, &b);
    mask_stroke_end(&m, h);
    check(mask_can_undo(h) && !mask_can_redo(h), "one undoable stroke, empty redo");

    check(mask_undo(&m, h), "undo returns true");
    check(!mask_any(&m), "undo -> mask all 0");
    check(!mask_can_undo(h) && mask_can_redo(h), "undo exhausted, redo armed");

    check(mask_redo(&m, h), "redo returns true");
    check(px(&m, 50, 50) == 230, "redo restores the stroke");
    check(mask_can_undo(h) && !mask_can_redo(h), "redo re-arms undo");

    /* a new stroke drops the redo path */
    mask_undo(&m, h);
    check(mask_can_redo(h), "redo armed before the new stroke");
    mask_stroke_begin(&m, h);
    mask_stroke_segment(&m, h, 20.0f, 80.0f, 80.0f, 80.0f, &b);
    mask_stroke_end(&m, h);
    check(!mask_can_redo(h), "redo cleared by mask_stroke_begin/end");

    check(mask_undo(&m, h), "undo the second stroke");
    check(px(&m, 50, 80) == 0 && !mask_any(&m), "second stroke gone");
    check(mask_redo(&m, h), "redo the second stroke");
    check(px(&m, 50, 80) == 230, "second stroke back");

    /* tiles are clipped at the image edge (100 is not a multiple of 64) */
    mask_hist_reset(h);
    mask_stroke_begin(&m, h);
    mask_stroke_segment(&m, h, 95.0f, 95.0f, 99.0f, 99.0f, &b);
    mask_stroke_end(&m, h);
    check(px(&m, 97, 97) == 230, "stroke in the corner tile lands");
    check(mask_undo(&m, h) && px(&m, 97, 97) == 0, "corner tile undoes");
    check(mask_redo(&m, h) && px(&m, 97, 97) == 230, "corner tile redoes");

    /* clear wrapped in begin/end is one undo step (what the UI does) */
    mask_stroke_begin(&m, h);
    mask_clear(&m);
    mask_stroke_end(&m, h);
    check(!mask_any(&m), "clear empties the mask");
    check(mask_undo(&m, h), "clear is undoable");
    check(px(&m, 50, 80) == 230, "undo of clear brings every tile back");
    mask_redo(&m, h);
    check(!mask_any(&m), "redo of clear empties it again");

    /* polygon fill is undoable through the same journal */
    mask_hist_reset(h);
    const float sq[8] = { 10.0f, 10.0f, 40.0f, 10.0f, 40.0f, 40.0f, 10.0f, 40.0f };
    mask_fill_polygon(&m, h, sq, 4, 200);
    check(px(&m, 25, 25) == 200, "lasso fill applied");
    check(mask_undo(&m, h) && px(&m, 25, 25) == 0, "lasso fill undone");

    mask_free(&m);
    mask_hist_free(h);
}

/* --------------------------------------------------------- tile journal --- */

static void test_journal_memory(void) {
    printf("tile journal memory\n");
    mask m = { 0, 0, 0 };
    mask_hist *h = mask_hist_new(0);
    if (!mask_init(&m, 1000, 1000) || !h) {
        check(0, "init");
        mask_free(&m);
        mask_hist_free(h);
        return;
    }
    mask_brush b;
    b.diameter = 8.0f;
    b.value = 230;
    mask_stroke_begin(&m, h);
    mask_stroke_segment(&m, h, 500.0f, 500.0f, 560.0f, 520.0f, &b);
    mask_stroke_end(&m, h);
    size_t used = mask_hist_bytes_used(h);
    printf("        journal = %lu bytes\n", (unsigned long)used);
    check(used > 0 && used < 200u * 1024u, "small stroke on 1000x1000 journal < 200 KB");

    /* budget: 32 KB holds only a few of these, oldest dropped first */
    mask_hist_free(h);
    h = mask_hist_new(32u * 1024u);
    int kept = 0;
    for (int i = 0; i < 12; i++) {
        mask_stroke_begin(&m, h);
        mask_stroke_segment(&m, h, 100.0f + 40.0f * (float)i, 100.0f,
                            100.0f + 40.0f * (float)i, 160.0f, &b);
        mask_stroke_end(&m, h);
        check(mask_hist_bytes_used(h) <= 32u * 1024u || kept == 0,
              "journal stays inside the 32 KB budget");
        kept++;
    }
    int undos = 0;
    while (mask_can_undo(h) && undos < 100) {
        mask_undo(&m, h);
        undos++;
    }
    printf("        %d of 12 strokes were still undoable\n", undos);
    check(undos > 0 && undos < 12, "oldest entries were dropped, newest kept");

    mask_free(&m);
    mask_hist_free(h);
}

/* ------------------------------------------------------------- feather ---- */

static void test_feather(void) {
    printf("feather=10 on a 200x200 stroke\n");
    mask m = { 0, 0, 0 };
    if (!mask_init(&m, 200, 200)) {
        check(0, "mask_init");
        return;
    }
    mask_brush b;
    b.diameter = 10.0f;
    b.value = 230;
    mask_stroke_segment(&m, NULL, 40.0f, 100.0f, 160.0f, 100.0f, &b);
    /* stroke covers rows 95..105 */

    uint8_t *w = NULL;
    check(mask_feather(&m, 10, &w) && w, "mask_feather succeeds");
    if (w) {
        int row = 100;
        int inside = w[(size_t)row * 200 + 100];
        int near3 = w[(size_t)(105 + 3) * 200 + 100];
        int far30 = w[(size_t)(105 + 30) * 200 + 100];
        printf("        weight: stroke=%d, +3px=%d, +30px=%d\n", inside, near3, far30);
        check(inside == 230, "stroke keeps its own strength");
        check(near3 > 0 && near3 < 230, "3 px outside: 0 < w < 230");
        check(far30 == 0, "30 px outside: w = 0");
        free(w);
        w = NULL;
    }

    check(mask_feather(&m, 0, &w) && w, "feather=0 succeeds");
    if (w) {
        check(memcmp(w, m.px, (size_t)m.w * (size_t)m.h) == 0, "feather=0 is a plain copy");
        free(w);
    }

    /* empty mask stays empty */
    mask_clear(&m);
    if (mask_feather(&m, 10, &w) && w) {
        mask em;
        em.px = w;
        em.w = m.w;
        em.h = m.h;
        check(!mask_any(&em), "empty mask feathers to 0");
        free(w);
    }
    mask_free(&m);
}

/* --------------------------------------------------- edges / live wire ---- */

static void synth(uint8_t *rgba, int w, int h, int vertical) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int on = vertical ? (x >= w / 2) : (y >= h / 2);
            uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = p[1] = p[2] = on ? 255 : 0;
            p[3] = 255;
        }
}

static void test_wire(void) {
    printf("edge map / live wire / snap (64x64 synthetic)\n");
    const int W = 64, H = 64;
    uint8_t *rgba = malloc((size_t)W * (size_t)H * 4);
    int path[2 * 512];
    if (!rgba) {
        check(0, "malloc");
        return;
    }
    mask_edges e;
    memset(&e, 0, sizeof e);

    synth(rgba, W, H, 1);
    check(mask_build_edges(rgba, W, H, &e) && e.e, "build edges (vertical split)");
    printf("        e(31,32)=%d e(32,32)=%d e(30,32)=%d\n",
           e.e[(size_t)32 * W + 31], e.e[(size_t)32 * W + 32], e.e[(size_t)32 * W + 30]);
    check(e.e[(size_t)32 * W + 31] > 100, "strong edge at the black/white border");
    check(e.e[(size_t)32 * W + 20] == 0, "flat area has no edge");

    int n = mask_live_wire(&e, 20, 32, 44, 32, path, 512);
    check(n >= 2, "live wire returned a path");
    int hit = 0;
    for (int i = 0; i < n; i++) {
        int x = path[i * 2];
        if (x - 32 <= 2 && 32 - x <= 2) hit = 1;
    }
    check(hit, "path passes through x ~ 32");
    check(n >= 2 && path[0] == 20 && path[1] == 32 && path[(n - 1) * 2] == 44 &&
              path[(n - 1) * 2 + 1] == 32,
          "path starts and ends on the requested points");

    int sx = 0, sy = 0;
    check(mask_snap(&e, 28, 32, 8, &sx, &sy) && sx >= 29 && sx <= 34,
          "snap pulls the cursor onto the border");
    check(!mask_snap(&e, 10, 10, 6, &sx, &sy), "snap refuses on a flat area");
    mask_edges_free(&e);

    /* horizontal split: the cheapest route rides the strong edge */
    synth(rgba, W, H, 0);
    check(mask_build_edges(rgba, W, H, &e) && e.e, "build edges (horizontal split)");
    n = mask_live_wire(&e, 10, 20, 54, 20, path, 512);
    check(n >= 2, "live wire returned a detour path");
    int deepest = 0;
    for (int i = 0; i < n; i++) {
        int y = path[i * 2 + 1];
        if (y > deepest) deepest = y;
    }
    printf("        detour reaches y=%d (%d points)\n", deepest, n);
    check(deepest >= 30, "wire detours onto the edge at y=32 instead of going straight");
    mask_edges_free(&e);

    /* huge window: refuse instead of freezing. The check happens before any
     * pixel is read, so a 1-byte buffer is enough. */
    uint8_t byte = 0;
    mask_edges big;
    big.e = &byte;
    big.w = 2000;
    big.h = 2000;
    check(mask_live_wire(&big, 0, 0, 1999, 1999, path, 512) == 0, "window > 1.5e6 nodes returns 0");

    free(rgba);
}

/* --------------------------------------------------------------- PNG ------ */

static void test_png(void) {
    printf("PNG round trip\n");
    mask m = { 0, 0, 0 }, back = { 0, 0, 0 };
    if (!img_init() || !mask_init(&m, 32, 24) || !mask_init(&back, 32, 24)) {
        printf("  SKIP  src/img.c not linked (tests/img_stub.c in use)\n");
        mask_free(&m);
        mask_free(&back);
        return;
    }
    mask_brush b;
    b.diameter = 9.0f;
    b.value = 230;
    mask_stroke_segment(&m, NULL, 4.0f, 12.0f, 28.0f, 12.0f, &b);

    const char *path = "tmp/mask_roundtrip.png";
    if (!mask_save(&m, path)) {
        printf("  SKIP  mask_save failed (src/img.c not linked)\n");
        mask_free(&m);
        mask_free(&back);
        return;
    }
    check(mask_load(&back, path), "mask_load reads what mask_save wrote");
    check(memcmp(back.px, m.px, (size_t)m.w * (size_t)m.h) == 0, "pixels survive the round trip");

    mask_free(&m);
    mask_free(&back);
    check(!mask_load(&back, "tmp/definitely-not-a-mask.png"), "missing file returns false");
}

/* ------------------------------------------------------------ timing ----- */

static void test_perf(void) {
    printf("timing on 4096x4096\n");
    mask m = { 0, 0, 0 };
    mask_hist *h = mask_hist_new(0);
    if (!mask_init(&m, 4096, 4096) || !h) {
        check(0, "init 4096x4096");
        mask_free(&m);
        mask_hist_free(h);
        return;
    }
    mask_brush b;
    b.diameter = 64.0f;
    b.value = MASK_VALUE_BRUSH;

    double t0 = now_ms();
    mask_stroke_begin(&m, h);
    float lastx = 100.0f, lasty = 2048.0f;
    for (int i = 1; i <= 200; i++) {
        float x = 100.0f + (float)i * 19.0f;
        float y = 2048.0f + 900.0f * (float)sin(0.08 * (double)i);
        mask_stroke_segment(&m, h, lastx, lasty, x, y, &b);
        lastx = x;
        lasty = y;
    }
    mask_stroke_end(&m, h);
    double t1 = now_ms();
    printf("        brush: 200-segment stroke, d=64: %.1f ms (journal %.1f MB)\n",
           t1 - t0, (double)mask_hist_bytes_used(h) / (1024.0 * 1024.0));
    check(t1 - t0 < 30.0, "brush < 30 ms");

    uint8_t *w = NULL;
    t0 = now_ms();
    int ok = mask_feather(&m, 10, &w);
    t1 = now_ms();
    printf("        feather=10: %.1f ms\n", t1 - t0);
    check(ok && w, "mask_feather on 4096x4096");
    check(t1 - t0 < 150.0, "feather < 150 ms");
    free(w);

    mask_free(&m);
    mask_hist_free(h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("test_mask: src/mask.c self-check\n");
    test_brush();
    test_polygon();
    test_undo();
    test_journal_memory();
    test_feather();
    test_wire();
    test_png();
    test_perf();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
