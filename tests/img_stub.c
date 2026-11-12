/* Stand-in for src/img.c so that tests/test_mask.c links without WIC.
 *
 * src/mask.c only needs the two PNG entry points for mask_save/mask_load.
 * Every stub fails, which makes the PNG round-trip case report SKIP. Link
 * src/img.c instead of this file to run that case for real (see
 * tests/README.md). Never part of the application build.
 */
#include "../src/img.h"

bool img_init(void) { return false; }
void img_shutdown(void) {}
bool img_encode_png(const char *utf8_path, const img_pixels *p) {
    (void)utf8_path;
    (void)p;
    return false;
}
bool img_decode_file(const char *utf8_path, img_pixels *out) {
    (void)utf8_path;
    (void)out;
    return false;
}
void img_free(img_pixels *p) {
    if (!p) return;
    p->rgba = NULL;
    p->w = p->h = 0;
}
