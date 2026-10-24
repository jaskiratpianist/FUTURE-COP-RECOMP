/* Unit test for thrash_tex.c (texture conversion, content hash):
   gcc -Irecomp/rt -Irecomp/platform recomp/test_thrash_tex.c recomp/platform/thrash_tex.c -o build/test_thrash_tex */
#include <stdio.h>
#include <string.h>
#include "thrash_tex.h"

static int fails;

static void check_pixel(uint16_t px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t out[4];
    thrash_tex_to_rgba8(&px, out, 1, 1, 7);
    if (out[0] != r || out[1] != g || out[2] != b || out[3] != a) {
        printf("FAIL 4444 %04x: got %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
               px, out[0], out[1], out[2], out[3], r, g, b, a);
        fails++;
    }
}

int main(void)
{
    uint16_t zero[4] = {0};
    uint32_t h;
    check_pixel(0xf001, 0x00, 0x00, 0x11, 0xff);  /* opaque, blue 1 */
    check_pixel(0x0fff, 0xff, 0xff, 0xff, 0x00);  /* transparent white */
    check_pixel(0x8421, 0x44, 0x22, 0x11, 0x88);
    h = thrash_tex_hash(zero, 2, 2);
    if (h != 0x9be17165u) {  /* FNV-1a over 8 zero bytes */
        printf("FAIL hash of 2x2 zero pixels: %08x\n", h);
        fails++;
    }
    if (!fails) printf("ok\n");
    return fails != 0;
}
