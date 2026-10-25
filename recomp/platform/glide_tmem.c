/* Glide texture memory (glide_tmem.h) */
#include <stdlib.h>
#include <string.h>
#include "glide_tmem.h"

static uint8_t *mem;
/* textures looked up so far: their range, and whether a write touched it since */
#define GTM_MAX 512
static struct { uint32_t addr, size; int dirty; } used[GTM_MAX];
static int nused;

void gtm_reset(void)
{
    nused = 0;
    if (mem) memset(mem, 0, GTM_SIZE);
}

int gtm_lod_size(int lod) { return lod >= 0 && lod <= 8 ? 256 >> lod : 1; }

static int fits(uint32_t addr, uint32_t size) { return addr <= GTM_SIZE && size <= GTM_SIZE - addr; }

static int write(uint32_t addr, const void *px, uint32_t size)
{
    if (!fits(addr, size) || (!mem && !(mem = calloc(1, GTM_SIZE)))) return 0;
    memcpy(mem + addr, px, size);
    for (int k = 0; k < nused; k++)
        if (addr < used[k].addr + used[k].size && used[k].addr < addr + size) used[k].dirty = 1;
    return 1;
}

int gtm_download(uint32_t addr, int w, int h, const uint16_t *px)
{
    return write(addr, px, 2u * (uint32_t)w * (uint32_t)h);
}

int gtm_download_rows(uint32_t addr, int w, int h, int row0, int row1, const uint16_t *px)
{
    if (row0 < 0 || row1 < row0 || row1 >= h) return 0;
    return write(addr + 2u * (uint32_t)w * (uint32_t)row0, px, 2u * (uint32_t)w * (uint32_t)(row1 - row0 + 1));
}

const uint16_t *gtm_texels(uint32_t addr, int w, int h, int *dirty)
{
    uint32_t size = 2u * (uint32_t)w * (uint32_t)h;
    int k;
    *dirty = 0;
    if (!fits(addr, size) || (!mem && !(mem = calloc(1, GTM_SIZE)))) return NULL;
    for (k = 0; k < nused; k++)
        if (used[k].addr == addr && used[k].size == size) break;
    if (k == nused) {
        if (nused == GTM_MAX) return NULL;
        used[nused].addr = addr;
        used[nused].size = size;
        used[nused++].dirty = 1;
    }
    *dirty = used[k].dirty;
    used[k].dirty = 0;
    return (const uint16_t *)(mem + addr);
}

int glide_blend_to_glp(int f, int is_dst)
{
    switch (f) {
    case 0: return 1;                  /* ZERO */
    case 1: return 2;                  /* SRC_ALPHA */
    case 2: return is_dst ? 6 : 7;     /* SRC_COLOR / DST_COLOR */
    case 3: return 4;                  /* DST_ALPHA */
    case 5: return 3;                  /* ONE_MINUS_SRC_ALPHA */
    case 6: return is_dst ? 8 : 9;     /* ONE_MINUS_SRC_COLOR / ONE_MINUS_DST_COLOR */
    case 7: return 5;                  /* ONE_MINUS_DST_ALPHA */
    default: return 0;                 /* 4 ONE (and ALPHA_SATURATE, approximated) */
    }
}
