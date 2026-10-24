/* THRASH texture helpers without GL or guest memory (unit-tested: recomp/test_thrash_tex.c) */
#ifndef FCOP_THRASH_TEX_H
#define FCOP_THRASH_TEX_H
#include <stdint.h>

/* convert w*h pixels of THRASH texture format fmt to RGBA bytes. Format 7 (the only one the
   game uses) is ARGB4444; a nibble x becomes x * 0x11 */
void thrash_tex_to_rgba8(const uint16_t *src, uint8_t *dst_rgba, int w, int h, int fmt);
/* FNV-1a over the pixel bytes: identifies a texture's content (for replacement packs later) */
uint32_t thrash_tex_hash(const uint16_t *src, int w, int h);

#endif
