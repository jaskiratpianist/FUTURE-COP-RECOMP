/* THRASH texture helpers (thrash_tex.h) */
#include "thrash_tex.h"

void thrash_tex_to_rgba8(const uint16_t *src, uint8_t *dst, int w, int h, int fmt)
{
    (void)fmt;  /* only format 7 (ARGB4444) is used */
    for (long k = 0; k < (long)w * h; k++) {
        uint16_t p = src[k];
        dst[4 * k + 0] = (uint8_t)(((p >> 8) & 15) * 0x11);
        dst[4 * k + 1] = (uint8_t)(((p >> 4) & 15) * 0x11);
        dst[4 * k + 2] = (uint8_t)((p & 15) * 0x11);
        dst[4 * k + 3] = (uint8_t)((p >> 12) * 0x11);
    }
}

uint32_t thrash_tex_hash(const uint16_t *src, int w, int h)
{
    const uint8_t *b = (const uint8_t *)src;
    uint32_t x = 2166136261u;
    for (long k = 0; k < (long)w * h * 2; k++) x = (x ^ b[k]) * 16777619u;
    return x;
}
