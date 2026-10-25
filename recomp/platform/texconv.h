/* Texture conversion for the GL renderers, without GL or guest memory (unit-tested:
   recomp/test_texconv.c). Used by thrash_gl.c and glide_gl.c. */
#ifndef FCOP_TEXCONV_H
#define FCOP_TEXCONV_H
#include <stdint.h>

/* n pixels of ARGB4444 (THRASH format 7, the only one the game uses) to RGBA bytes; a nibble
   x becomes x * 0x11 */
void tex_4444_to_rgba8(const uint16_t *src, uint8_t *dst_rgba, int n);
/* n pixels of ARGB1555 (Glide's format 0xb) to RGBA bytes; 5 bits x become x << 3 | x >> 2,
   A = bit 15 ? 0xff : 0. With key_on, texels whose colour (low 15 bits) equals the key's get
   A = 0 (Glide's chromakey) */
void tex_1555_to_rgba8(const uint16_t *src, uint8_t *dst_rgba, int n, int key_on, uint16_t key);
/* FNV-1a over the pixel bytes: identifies a texture's content (for replacement packs later) */
uint32_t tex_hash(const uint16_t *src, int n);

#endif
