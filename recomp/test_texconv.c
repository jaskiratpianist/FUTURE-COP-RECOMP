/* Unit test for texconv.c (texture conversion, content hash):
   gcc -Irecomp/rt -Irecomp/platform recomp/test_texconv.c recomp/platform/texconv.c -o build/test_texconv */
#include <stdio.h>
#include <string.h>
#include "texconv.h"

static int fails;

static void check(const char *what, uint16_t px, const uint8_t *out, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (out[0] != r || out[1] != g || out[2] != b || out[3] != a) {
        printf("FAIL %s %04x: got %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
               what, px, out[0], out[1], out[2], out[3], r, g, b, a);
        fails++;
    }
}

static void check_4444(uint16_t px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t out[4];
    tex_4444_to_rgba8(&px, out, 1);
    check("4444", px, out, r, g, b, a);
}

static void check_1555(uint16_t px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t out[4];
    tex_1555_to_rgba8(&px, out, 1);
    check("1555", px, out, r, g, b, a);
}

int main(void)
{
    uint16_t zero[4] = {0};
    uint32_t h;
    check_4444(0xf001, 0x00, 0x00, 0x11, 0xff);  /* opaque, blue 1 */
    check_4444(0x0fff, 0xff, 0xff, 0xff, 0x00);  /* transparent white */
    check_4444(0x8421, 0x44, 0x22, 0x11, 0x88);
    check_1555(0x7c1f, 0xff, 0x00, 0xff, 0x00);  /* magenta, alpha bit clear */
    check_1555(0xfc00, 0xff, 0x00, 0x00, 0xff);  /* opaque red */
    check_1555(0x8421, 0x08, 0x08, 0x08, 0xff);  /* 1 -> 1 << 3 | 1 >> 2 */
    check_1555(0x8001, 0x00, 0x00, 0x08, 0xff);  /* the game's opaque near-black */
    h = tex_hash(zero, 4);
    if (h != 0x9be17165u) {  /* FNV-1a over 8 zero bytes */
        printf("FAIL hash of 4 zero pixels: %08x\n", h);
        fails++;
    }
    if (!fails) printf("ok\n");
    return fails != 0;
}
