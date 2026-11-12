/* Test-only provider of the two CUI PNG entry points that backend.c links
 * against (CUI itself ships them in src/cui_image.c). Build:
 *   cl ... /I"C:/Users/yabo/cui_probe/third_party" png_shim.c
 */
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned char *cui__png_decode(const unsigned char *data, int len, int *w, int *h);
void cui__png_free(unsigned char *px);

unsigned char *cui__png_decode(const unsigned char *data, int len, int *w, int *h) {
    int comp = 0;
    return stbi_load_from_memory(data, len, w, h, &comp, 4);
}

void cui__png_free(unsigned char *px) { stbi_image_free(px); }
