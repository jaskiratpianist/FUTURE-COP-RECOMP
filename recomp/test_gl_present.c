/* Unit test for gl_present.c's pure helpers (no GL needed):
   gcc -Irecomp/rt -Irecomp/platform $(sdl2-config --cflags) recomp/test_gl_present.c -o build/test_gl_present $(sdl2-config --libs) */
#define GLP_NO_GL
#include "platform/gl_present.c"

static int fails;

static void check_viewport(int dw, int dh, int x, int y, int w, int h)
{
    int r[4];
    glp_viewport_43(dw, dh, r);
    if (r[0] != x || r[1] != y || r[2] != w || r[3] != h) {
        printf("FAIL viewport %dx%d: got %d,%d %dx%d, want %d,%d %dx%d\n", dw, dh, r[0], r[1], r[2], r[3], x, y, w, h);
        fails++;
    }
}

static void check_clip(const int vpr[4], int gw, int gh, int x0, int y0, int x1, int y1, int x, int y, int w, int h)
{
    int r[4];
    glp_clip_rect(vpr, gw, gh, x0, y0, x1, y1, r);
    if (r[0] != x || r[1] != y || r[2] != w || r[3] != h) {
        printf("FAIL clip %d,%d-%d,%d: got %d,%d %dx%d, want %d,%d %dx%d\n", x0, y0, x1, y1, r[0], r[1], r[2], r[3], x, y, w, h);
        fails++;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    check_viewport(2560, 1440, 320, 0, 1920, 1440);   /* 16:9: bars left and right */
    check_viewport(1600, 1200, 0, 0, 1600, 1200);     /* exactly 4:3 */
    check_viewport(1280, 1024, 0, 32, 1280, 960);     /* 5:4: bars top and bottom */
    {
        int r[4];
        glp_viewport_fit(2560, 1440, 1280, 1024, r);  /* Glide's 1280x1024 screen: square pixels */
        if (r[0] != 380 || r[1] != 0 || r[2] != 1800 || r[3] != 1440) {
            printf("FAIL fit 1280x1024 in 2560x1440: got %d,%d %dx%d\n", r[0], r[1], r[2], r[3]);
            fails++;
        }
    }
    if (!fails) printf("ok\n");
    return fails != 0;
}
