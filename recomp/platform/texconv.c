/* Texture conversion for the GL renderers (texconv.h) */
#include "texconv.h"

void tex_4444_to_rgba8(const uint16_t *src, uint8_t *dst, int n)
{
    for (long k = 0; k < n; k++) {
        uint16_t p = src[k];
        dst[4 * k + 0] = (uint8_t)(((p >> 8) & 15) * 0x11);
        dst[4 * k + 1] = (uint8_t)(((p >> 4) & 15) * 0x11);
        dst[4 * k + 2] = (uint8_t)((p & 15) * 0x11);
        dst[4 * k + 3] = (uint8_t)((p >> 12) * 0x11);
    }
}

static uint8_t five_to_eight(unsigned x) { return (uint8_t)(x << 3 | x >> 2); }

void tex_1555_to_rgba8(const uint16_t *src, uint8_t *dst, int n)
{
    for (long k = 0; k < n; k++) {
        uint16_t p = src[k];
        dst[4 * k + 0] = five_to_eight((p >> 10) & 31);
        dst[4 * k + 1] = five_to_eight((p >> 5) & 31);
        dst[4 * k + 2] = five_to_eight(p & 31);
        dst[4 * k + 3] = p & 0x8000 ? 0xff : 0;
    }
}

uint32_t tex_hash(const uint16_t *src, int n)
{
    const uint8_t *b = (const uint8_t *)src;
    uint32_t x = 2166136261u;
    for (long k = 0; k < (long)n * 2; k++) x = (x ^ b[k]) * 16777619u;
    return x;
}
