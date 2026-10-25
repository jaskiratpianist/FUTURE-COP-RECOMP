/* Widescreen (Hor+) in missions: the game projects into a fixed 4:3 screen of 2560x1920
 * units. Narrowing camera-space x by k = (4/3) / aspect before its projections and view tests
 * puts the wider view into that screen; the renderer stretches it over the whole drawable and
 * draws the HUD with the 4:3 viewport. docs/superpowers/plans/2026-10-25-widescreen.md. */
#include <stdlib.h>
#include <string.h>
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

/* ---- drawing the display list ------------------------------------------------------------

   The display list is one linked list of depth buckets, farthest first; bucket 0 (depth key 0:
   the HUD) comes last, behind its node at [0x499ad0]. The walkers (0x4393e0 Glide, 0x456270
   THRASH) convert each vertex from 2560x1920 units to the game's resolution through a jump
   table indexed by the resolution (0x511e58): at 1280x960, x / 2 rounded down. Precise mode
   points the current index's entry at the walkers' own plain path instead, which passes the
   units unchanged (records of type 6, text, are in game pixels and stay so); x86c reads the
   table from guest memory. So every call draws either non-text records (2560x1920 space) or
   text (game pixels), each with its own coordinate space (glp_set_space).

   Widescreen draws the HUD in further calls with 4:3 viewports (glp_hud): at the screen edges in
   single player (FCOP_HUD_EDGES=0: centred), one per player's half in split screen.

   In split screen the game draws each player's list in its own call, which starts with control
   records (type bits 0-2 = 0, from 0x40ca10): 0x10 sets the view's x origin (+0x28, in 640x480
   units: 0 left, 0x140 right; the walker keeps x * 4 and adds a quarter of that on the plain
   path, so the game's own text was 320 pixels too far left at 1280x960), 0x08 its extent.
   Every call gets them, with the origin rescaled for its records. */

struct Walker {
    void (*f)(Cpu *);
    void (*begin)(void);
    u32 table;   /* jump table of the resolution cases */
    int first;   /* resolution index of its entry 0 */
    int count;
    u32 plain;   /* the unscaled path */
};

int precise_vertices(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("FCOP_PRECISE");
        on = !(e && *e == '0');
    }
    return on;
}

/* the game's resolution in pixels (the space of type-6 records) */
static void game_px(int *w, int *h)
{
    u32 r = rd32(0x511e58);
    *w = r <= 10 ? (int)rd32(0x487080 + 20 * r) : 1280;
    *h = r <= 10 ? (int)rd32(0x487084 + 20 * r) : 960;
    if (*w < 320 || *h < 200) *w = 1280, *h = 960;
}

/* HUD records -> -1 left, 0 centre, 1 right of the 4:3 HUD, by the centre of their extent.
   Font glyphs in a row (texture page 0xd, type 6) count as one, so a word or number stays
   whole; records placed by to_screen stay centred. */
static void hud_sides(const u32 *rec, signed char *side, int n)
{
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
}

#define MAX_REC 32768
static u32 rec[MAX_REC], rec_next[MAX_REC];
static signed char rec_pass[MAX_REC];  /* which pass draws it: 0 3D, 1..3 HUD */
static u32 ctl[4], ctl_next[4];
static s16 ctl_x[4];
static int nctl;

static int unit_clip;  /* a walker call over non-text records in precise mode is running */

/* 0x469d90(x, y, w, h, ..): the THRASH walker's clip rectangle for its own polygon clipper
   (0x466300), in game pixels (floats); in 2560x1920 units while unit_clip */
void G_469d90(Cpu *c);
void F_469d90(Cpu *c)
{
    float v[4], save[4];
    u32 args = c->r[4] + 4;
    int gw, gh;
    if (!unit_clip) { G_469d90(c); return; }
    game_px(&gw, &gh);
    memcpy(save, G(args), sizeof save);
    for (int i = 0; i < 4; i++) v[i] = save[i] * (i & 1 ? 1920.0f / gh : 2560.0f / gw);
    memcpy(G(args), v, sizeof v);
    G_469d90(c);
    memcpy(G(args), save, sizeof save);
}

/* call the walker over the records of one pass (text or not), with the control records in
   front */
static void walk(Cpu *c, const struct Walker *w, u32 sp, int n, int pass, int text)
{
    u32 first = 0, last = 0;
    int precise = precise_vertices(), gw, gh;
    game_px(&gw, &gh);
    for (int i = 0; i < n; i++) {
        if (rec_pass[i] != pass || (precise && ((MEM[rec[i] + 4] & 7) == 6) != text)) continue;
        if (last) wr32(last, rec[i]); else first = rec[i];
        last = rec[i];
    }
    if (!first) return;
    wr32(last, 0);
    for (int i = nctl - 1; i >= 0; i--) {
        wr32(ctl[i], first);
        first = ctl[i];
        if (precise && MEM[ctl[i] + 4] & 0x10)
            wr16(ctl[i] + 0x28, (u16)(s16)(text ? ctl_x[i] * gw / 640 : ctl_x[i] * 4));
    }
    if (precise) glp_set_space(text ? gw : 2560, text ? gh : 1920);
    c->r[4] = sp;  /* the walker's ret popped the return address: each call pushes it back */
    wr32(sp + 4, first);
    unit_clip = precise && !text;
    w->f(c);
    unit_clip = 0;
}

static void draw_list(Cpu *c, const struct Walker *w)
{
    u32 sp = c->r[4], head = rd32(sp + 4), node0 = rd32(0x499ad0), entry = 0, entry_val = 0;
    int n = 0, hud = -1, wide = wide_k() != 1.0, precise = precise_vertices(), mode = 1, idx;
    if (!precise && !wide) { w->f(c); return; }
    nctl = 0;
    for (u32 r = head; r; r = rd32(r)) {
        u8 t = MEM[r + 4];
        if (r == node0) hud = n;
        if (!t) continue;  /* bucket nodes */
        if (!(t & 7)) {
            if (nctl < 4 && hud < 0) {
                ctl_x[nctl] = (s16)rd16(r + 0x28);
                ctl_next[nctl] = rd32(r);
                ctl[nctl++] = r;
                if (t & 0x10) mode = (s16)rd16(r + 0x28) ? 3 : 2;
                if (t & 0x08 && (s16)rd16(r + 0x28) >= 0x280) mode = 1;
            }
            continue;
        }
        if (n == MAX_REC) {  /* not expected: draw it the game's way */
            plat_log("widescreen: display list longer than %d records", MAX_REC);
            w->f(c);
            return;
        }
        rec[n] = r;
        rec_next[n] = rd32(r);
        rec_pass[n++] = hud < 0 ? 0 : 2;
    }
    if (hud < 0) hud = n;
    if (!MEM[0x511f54]) mode = 1;
    if (wide) {
        for (int i = hud; i < n; i++) {  /* full-height bars (the split-screen divider) */
            int x0, y0, x1, y1;
            if (extent(rec[i], &x0, &y0, &x1, &y1) && y0 <= 8 && y1 >= 1911) {
                if (getenv("FCOP_WIDE_LOG")) plat_log("widescreen: full-height HUD record type %02x", MEM[rec[i] + 4]);
                to_screen(rec[i]);
            }
        }
        if (mode == 1) {
            const char *e = getenv("FCOP_HUD_EDGES");
            static signed char side[MAX_REC];
            if (!(e && *e == '0')) {
                hud_sides(rec + hud, side, n - hud);
                for (int i = hud; i < n; i++) rec_pass[i] = (signed char)(2 + side[i - hud]);
            }
        }
    }
    idx = (int)rd32(0x511e58) - w->first;
    if (precise && idx >= 0 && idx < w->count) {
        entry = w->table + 4 * (u32)idx;
        entry_val = rd32(entry);
        wr32(entry, w->plain);
    }
    w->begin();
    for (int pass = 0; pass <= 3; pass++) {
        /* 0: 3D; HUD: 1 left, 2 centre, 3 right (single player), else 2 */
        static const int hud_mode[4] = { 0, 4, 1, 5 };
        if (wide) glp_hud(pass == 0 ? 0 : mode == 1 ? hud_mode[pass] : mode);
        walk(c, w, sp, n, pass, 0);
        if (precise) walk(c, w, sp, n, pass, 1);
    }
    if (wide) glp_hud(0);
    if (precise) glp_set_space(2560, 1920);
    if (entry) wr32(entry, entry_val);
    for (int i = 0; i < n; i++) wr32(rec[i], rec_next[i]);
    for (int i = 0; i < nctl; i++) {
        wr16(ctl[i] + 0x28, (u16)ctl_x[i]);
        wr32(ctl[i], ctl_next[i]);
    }
    wr32(sp + 4, head);
    c->r[4] = sp + 4;  /* as the walker's ret leaves it */
}

void G_4393e0(Cpu *c);
void G_456270(Cpu *c);
static const struct Walker glide_walker = { G_4393e0, glide_frame_begin, 0x43a168, 4, 7, 0x4397cd };
static const struct Walker thrash_walker = { G_456270, thrash_frame_begin, 0x456e00, 0, 11, 0x4566fb };

void F_4393e0(Cpu *c) { draw_list(c, &glide_walker); }
void F_456270(Cpu *c) { draw_list(c, &thrash_walker); }
