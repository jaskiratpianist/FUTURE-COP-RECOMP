/* Glide texture memory (TMU 0) without GL or guest memory (unit-tested:
   recomp/test_glide_tmem.c). The game downloads 16-bit texels to addresses and later selects
   a texture by its start address; glide_gl.c turns each used address into a GL texture. */
#ifndef FCOP_GLIDE_TMEM_H
#define FCOP_GLIDE_TMEM_H
#include <stdint.h>

#define GTM_SIZE (16u << 20)  /* what grTexMaxAddress reports; the game needs about 2 MB */

void gtm_reset(void);
/* GR_LOD_256 (0) .. GR_LOD_1 (8) -> 256 .. 1 */
int gtm_lod_size(int lod);
/* copy w*h texels to addr; 0 when it does not fit */
int gtm_download(uint32_t addr, int w, int h, const uint16_t *px);
/* rows row0..row1 (inclusive) of a w x h level at addr (grTexDownloadMipMapLevelPartial) */
int gtm_download_rows(uint32_t addr, int w, int h, int row0, int row1, const uint16_t *px);
/* the w x h texels at addr, or NULL when out of range; *dirty is 1 the first time after any
   write overlapping them (or the first lookup), then 0 */
const uint16_t *gtm_texels(uint32_t addr, int w, int h, int *dirty);

#endif
