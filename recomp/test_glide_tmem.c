/* Unit test for glide_tmem.c (Glide texture memory):
   gcc -Irecomp/rt -Irecomp/platform recomp/test_glide_tmem.c recomp/platform/glide_tmem.c -o build/test_glide_tmem */
#include <stdio.h>
#include <stdlib.h>
#include "glide_tmem.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    static uint16_t tex[256 * 256], rows[256 * 2];
    int dirty = -1;
    const uint16_t *t;
    for (int k = 0; k < 256 * 256; k++) tex[k] = (uint16_t)k;
    for (int k = 0; k < 256 * 2; k++) rows[k] = 0xabcd;
    gtm_reset();
    CHECK(gtm_lod_size(0) == 256 && gtm_lod_size(8) == 1, "lod sizes %d %d", gtm_lod_size(0), gtm_lod_size(8));
    CHECK(gtm_download(0, 256, 256, tex) == 1, "download at 0");
    t = gtm_texels(0, 256, 256, &dirty);
    CHECK(t && dirty == 1 && t[300] == 300, "first lookup: dirty %d", dirty);
    gtm_texels(0, 256, 256, &dirty);
    CHECK(dirty == 0, "second lookup still dirty");
    gtm_texels(0x20000, 256, 256, &dirty);
    gtm_texels(0x20000, 256, 256, &dirty);
    CHECK(gtm_download(0x20000, 256, 256, tex) == 1, "download at 0x20000");
    gtm_texels(0, 256, 256, &dirty);
    CHECK(dirty == 0, "download at 0x20000 dirtied address 0");
    gtm_texels(0x20000, 256, 256, &dirty);
    CHECK(dirty == 1, "download at 0x20000 not dirty");
    CHECK(gtm_download_rows(0, 256, 256, 10, 11, rows) == 1, "partial download");
    t = gtm_texels(0, 256, 256, &dirty);
    CHECK(dirty == 1 && t[10 * 256] == 0xabcd && t[11 * 256 + 255] == 0xabcd && t[12 * 256] == 12 * 256 && t[9 * 256] == 9 * 256,
          "partial rows: dirty %d", dirty);
    CHECK(gtm_download(GTM_SIZE - 2, 2, 1, tex) == 0, "download past the end accepted");
    CHECK(gtm_texels(GTM_SIZE - 2, 2, 1, &dirty) == NULL, "lookup past the end accepted");
    /* Glide blend factors -> gl_present codes (0 ONE, 1 ZERO, 2 SRC_ALPHA, 3 ONE_MINUS_SRC_ALPHA,
       4 DST_ALPHA, 5 ONE_MINUS_DST_ALPHA, 6 SRC_COLOR, 7 DST_COLOR, 8 ONE_MINUS_SRC_COLOR,
       9 ONE_MINUS_DST_COLOR) */
    CHECK(glide_blend_to_glp(1, 0) == 2 && glide_blend_to_glp(5, 1) == 3, "SRC_ALPHA / ONE_MINUS_SRC_ALPHA");
    CHECK(glide_blend_to_glp(0, 0) == 1 && glide_blend_to_glp(4, 1) == 0, "ZERO / ONE");
    CHECK(glide_blend_to_glp(2, 0) == 7 && glide_blend_to_glp(2, 1) == 6, "code 2: DST_COLOR as source, SRC_COLOR as destination");
    CHECK(glide_blend_to_glp(6, 0) == 9 && glide_blend_to_glp(6, 1) == 8, "code 6");
    CHECK(glide_blend_to_glp(3, 0) == 4 && glide_blend_to_glp(7, 1) == 5, "DST_ALPHA / ONE_MINUS_DST_ALPHA");
    if (!fails) printf("ok\n");
    return fails != 0;
}
