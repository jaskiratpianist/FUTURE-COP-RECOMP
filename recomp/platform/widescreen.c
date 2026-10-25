/* Widescreen (Hor+) in missions: the game projects into a fixed 4:3 screen of 2560x1920
 * units. Narrowing camera-space x by k = (4/3) / aspect before its projections and view tests
 * puts the wider view into that screen; the renderer stretches it over the whole drawable and
 * draws the HUD with the 4:3 viewport. docs/superpowers/plans/2026-10-25-widescreen.md. */
#include <stdlib.h>
#include "host.h"
#include "gl_present.h"
#include "thrash.h"

static double k = 1.0;

static double wide_k(void) { return k; }

static u32 placed[256];  /* records moved by to_screen this frame (drawn with the centred HUD) */
static int nplaced;

static int enabled = -1;  /* the player's choice (Preferences), read on first use */

void widescreen_enable(int on) { enabled = on; }

/* between frames (after each present): follow the render size's aspect. FCOP_WIDE=0 turns it
   off, FCOP_WIDE=<aspect> forces one (tests) */
void widescreen_update(void)
{
    const char *e = getenv("FCOP_WIDE");
    double a;
    if (enabled < 0) enabled = widescreen_pref();
    a = e && *e ? atof(e) : enabled ? glp_render_aspect() : 0;
    double nk = a > 4.0 / 3.0 + 0.01 ? (4.0 / 3.0) / a : 1.0;
    if (nk != k) plat_log("widescreen: aspect %.3f, x scale %.3f", a, nk);
    k = nk;
    glp_set_wide(k != 1.0);
    nplaced = 0;
}

static s16 sx(s32 v) { return (s16)(v * wide_k()); }

/* 0x453740(out, x, y, z): one terrain vertex */
void G_453740(Cpu *c);
void F_453740(Cpu *c)
{
    u32 a = c->r[4] + 8;
    wr32(a, (u32)(s32)((s32)rd32(a) * wide_k()));
    G_453740(c);
}

/* 0x457140(p): a sprite {x, y, z, _, w, h} (s16) */
void G_457140(Cpu *c);
void F_457140(Cpu *c)
{
    u32 p = rd32(c->r[4] + 4);
    wr16(p, (u16)sx((s16)rd16(p)));
    wr16(p + 8, (u16)sx((s16)rd16(p + 8)));
    G_457140(c);
}

/* 0x457340(points, flags, n): n points {x, y, z, _} (s16, 8 bytes each) */
void G_457340(Cpu *c);
void F_457340(Cpu *c)
{
    u32 p = rd32(c->r[4] + 4);
    s32 n = (s32)rd32(c->r[4] + 12);
    for (s32 i = 0; i < n; i++) wr16(p + 8 * i, (u16)sx((s16)rd16(p + 8 * i)));
    G_457340(c);
}

/* 0x452fd0(p, r): is the sphere at camera-space p {x, y, z} (s32) with radius r inside the
   4:3 view (|x| * 16 <= 5z + ..)? Terrain blocks and cells (0x452a20) are culled with it. */
void G_452fd0(Cpu *c);
void F_452fd0(Cpu *c)
{
    u32 p = rd32(c->r[4] + 4);
    s32 x = (s32)rd32(p);
    wr32(p, (u32)(s32)(x * wide_k()));
    G_452fd0(c);
    wr32(p, (u32)x);
}

/* 0x4572a0(p, r): the same test for objects, p {x, y, z} s16 (0x4280f0, 0x430bc0, 0x4373e0,
   0x447de0) */
void G_4572a0(Cpu *c);
void F_4572a0(Cpu *c)
{
    u32 p = rd32(c->r[4] + 4);
    u16 x = rd16(p);
    wr16(p, (u16)sx((s16)x));
    G_4572a0(c);
    wr16(p, x);
}

/* the centre of the view, in 2560x1920 units (split screen: each view's own coordinates,
   1280 wide; the walker adds the right view's origin) */
static int centre(void) { return MEM[0x511f54] ? 640 : 1280; }

/* 2560x1920 units per unit of a record: type 5 sprites are in 640x480 units, type 6 in pixels
   of the game's resolution (index 0x511e58 into its table at 0x48707c) */
static int units(int type)
{
    u32 r = rd32(0x511e58);
    int w = r <= 10 ? (int)rd32(0x487080 + 20 * r) : 1280;
    return type == 5 ? 4 : w >= 320 ? 2560 / w : 2;
}

/* a record's extent in 2560x1920 units; 0 for records without one */
static int extent(u32 r, int *x0, int *y0, int *x1, int *y1)
{
    u8 t = MEM[r + 4];
    int n = t & 7;
    *x0 = *y0 = 0x7fff;
    *x1 = *y1 = -0x7fff;
    if (n >= 1 && n <= 4) {
        for (int i = 0; i < n; i++) {
            int x = (s16)rd16(r + 0x28 + 4 * i), y = (s16)rd16(r + 0x2a + 4 * i);
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
        return 1;
    }
    if (n == 5 || n == 6) {
        int u = units(n), m = n == 5 ? 2 : 1;
        int w = t & 0x80 ? MEM[r + 10] * m : (s16)rd16(r + 0x2c), h = t & 0x80 ? MEM[r + 11] * m : (s16)rd16(r + 0x2e);
        *x0 = (s16)rd16(r + 0x28) * u;
        *y0 = (s16)rd16(r + 0x2a) * u;
        *x1 = *x0 + w * u;
        *y1 = *y0 + h * u;
        return 1;
    }
    return 0;
}

/* A HUD-level record whose place belongs to the 3D picture (a target marker, the split-screen
   divider): the HUD is drawn with the 4:3 viewport, so move it from x to cx + (x - cx) / k.
   Polygons: every vertex; sprites {x, y, w, h}: the centre (the size stays). */
static void to_screen(u32 r)
{
    if (nplaced < 256) placed[nplaced++] = r;
    u8 t = MEM[r + 4];
    int n = t & 7, cx = centre();
    if (n >= 1 && n <= 4) {
        for (int i = 0; i < n; i++) {
            s32 x = (s16)rd16(r + 0x28 + 4 * i);
            wr16(r + 0x28 + 4 * i, (u16)(s16)(cx + (x - cx) / wide_k()));
        }
    } else if (n == 5 || n == 6) {
        int u = units(n), w = t & 0x80 ? MEM[r + 10] * (n == 5 ? 2 : 1) : (s16)rd16(r + 0x2c);
        s32 x = (s16)rd16(r + 0x28) + w / 2;
        cx /= u;
        wr16(r + 0x28, (u16)(s16)(cx + (x - cx) / wide_k() - w / 2));
    }
}

/* 0x435330 draws the target markers at the projected position of their target, on the HUD
   level (depth key 0) */
void G_435330(Cpu *c);
void F_435330(Cpu *c)
{
    u32 from = rd32(0x511dc0), to;
    G_435330(c);
    to = rd32(0x511dc0);
    if (wide_k() == 1.0 || to < from || to - from > 0x38 * 256) return;
    for (u32 r = from; r < to; r += 0x38)
        if (MEM[r + 4] && !rd32(r)) to_screen(r);  /* +0 holds the depth key until the list is sorted */
}

/* Single-player HUD at the screen edges: records on the left of the 4:3 HUD are drawn with a
   left-aligned 4:3 viewport, those on the right right-aligned, the rest centred. Font glyphs in
   a row (texture page 0xd, type 6) are classified together, so a word or number stays whole;
   records placed by to_screen stay centred. Returns 0 to draw the HUD in one centred call
   instead (FCOP_HUD_EDGES=0, or too many records). */
#define MAX_HUD 2048
static int draw_hud_anchored(Cpu *c, void (*f)(Cpu *), u32 node0)
{
    static u32 rec[MAX_HUD], next[MAX_HUD];
    static signed char side[MAX_HUD];  /* -1 left, 0 centre, 1 right */
    const char *e = getenv("FCOP_HUD_EDGES");
    u32 sp = c->r[4];
    int n = 0;
    if (e && *e == '0') return 0;
    for (u32 r = node0; r; r = rd32(r)) {
        if (n == MAX_HUD) return 0;
        rec[n] = r;
        next[n++] = rd32(r);
    }
    for (int i = 0; i < n;) {
        int x0, y0, x1, y1, j = i + 1, s0 = 0;
        if (extent(rec[i], &x0, &y0, &x1, &y1)) {
            int glyph = (MEM[rec[i] + 4] & 7) == 6 && MEM[rec[i] + 7] == 0xd, cx;
            while (glyph && j < n && (MEM[rec[j] + 4] & 7) == 6 && MEM[rec[j] + 7] == 0xd) {
                int a0, b0, a1, b1;
                extent(rec[j], &a0, &b0, &a1, &b1);
                if (abs(b0 - y0) > 8 || (a0 > x1 ? a0 - x1 : x0 - a1) > 48) break;
                if (a0 < x0) x0 = a0;
                if (a1 > x1) x1 = a1;
                j++;
            }
            cx = (x0 + x1) / 2;
            s0 = cx < 1280 - 160 ? -1 : cx > 1280 + 160 ? 1 : 0;
            for (int m = 0; m < nplaced; m++)
                for (int q = i; q < j; q++) s0 = placed[m] == rec[q] ? 0 : s0;
        }
        for (; i < j; i++) side[i] = (signed char)s0;
    }
    for (int sd = -1; sd <= 1; sd++) {  /* one walker call per side, over its records in order */
        u32 first = 0, last = 0;
        for (int i = 0; i < n; i++) {
            if (side[i] != sd) continue;
            if (last) wr32(last, rec[i]); else first = rec[i];
            last = rec[i];
        }
        if (!first) continue;
        wr32(last, 0);
        c->r[4] = sp;
        wr32(sp + 4, first);
        glp_hud(sd < 0 ? 4 : sd > 0 ? 5 : 1);
        f(c);
    }
    for (int i = 0; i < n; i++) wr32(rec[i], next[i]);
    return 1;
}

/* The display list is one linked list of depth buckets, farthest first; bucket 0 (depth key 0:
   the HUD) comes last, behind its node at [0x499ad0]. Draw the list in two calls, the HUD with
   the 4:3 viewport. f = the game's list walker (0x4393e0 Glide, 0x456270 THRASH).
   In split screen the game draws each player's list in its own call, which starts with control
   records (type bits 0-2 = 0, from 0x40ca10): 0x10 sets the view's x origin (+0x28, 640x480
   units: 0 left, 0x140 right), 0x08 its extent (0x140 a half, 0x280 the whole screen). The HUD
   call gets them too, and its player's half of the screen. HUD records over the whole height
   (the divider) belong to the screen, not the HUD (to_screen). */
static void draw_split(Cpu *c, void (*f)(Cpu *))
{
    u32 sp = c->r[4], head = rd32(sp + 4), node0 = rd32(0x499ad0), prev = 0, ctl[4], ctl_next[4];
    int nctl = 0, mode = 1;
    for (u32 r = head; r && r != node0; r = rd32(r)) {
        u8 t = MEM[r + 4];
        if (t && !(t & 7) && nctl < 4) {
            ctl[nctl++] = r;
            if (t & 0x10) mode = (s16)rd16(r + 0x28) ? 3 : 2;
            if (t & 0x08 && (s16)rd16(r + 0x28) >= 0x280) mode = 1;
        }
        prev = r;
    }
    if (wide_k() == 1.0 || !prev || !rd32(prev)) { f(c); return; }
    if (!MEM[0x511f54]) mode = 1;
    wr32(prev, 0);
    f(c);
    wr32(prev, node0);
    for (u32 r = rd32(node0); r; r = rd32(r)) {  /* full-height bars (the split-screen divider) */
        int x0, y0, x1, y1;
        if (extent(r, &x0, &y0, &x1, &y1) && y0 <= 8 && y1 >= 1911) {
            if (getenv("FCOP_WIDE_LOG")) plat_log("widescreen: full-height HUD record type %02x", MEM[r + 4]);
            to_screen(r);
        }
    }
    for (int i = 0; i < nctl; i++) {  /* chain the control records in front of the HUD */
        ctl_next[i] = rd32(ctl[i]);
        wr32(ctl[i], i + 1 < nctl ? ctl[i + 1] : node0);
    }
    c->r[4] = sp;  /* the walker's ret popped the return address: call it again */
    if (mode != 1 || !draw_hud_anchored(c, f, node0)) {
        wr32(sp + 4, nctl ? ctl[0] : node0);
        glp_hud(mode);
        f(c);
    }
    glp_hud(0);
    for (int i = 0; i < nctl; i++) wr32(ctl[i], ctl_next[i]);
    wr32(sp + 4, head);
}

void G_4393e0(Cpu *c);
void F_4393e0(Cpu *c) { draw_split(c, G_4393e0); }

void G_456270(Cpu *c);
void F_456270(Cpu *c) { draw_split(c, G_456270); }
