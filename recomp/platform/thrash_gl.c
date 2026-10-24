/* The game's THRASH 3D driver (normally opengla.dll and friends from the CD) implemented
 * natively: LoadLibraryA of a "...a.dll" driver returns a pseudo module, GetProcAddress hands
 * out thunks to the functions below, and they draw through gl_present.c.
 * docs/THRASH_NOTES.md describes every call. */
#include <SDL.h>
#include "thrash.h"
#include "win.h"
#include "gl_present.h"
#include "thrash_tex.h"

#define THRASH_MODULE 0x60000000u

int thrash_frame_last;
static u32 about_p;
static u32 states[24];      /* setstate values for ids 1..23 (opengla's range) */
static u32 blend_src = 2, blend_dst = 3;  /* setstate 0x194 / 0x195 (d3da.dll's blend factors) */

/* textures: the game gets a small guest object holding our table index + 1 at +0 */
#define MAX_TEX 1024
static struct { unsigned gl; int w, h, fmt; uint32_t hash; } texs[MAX_TEX];
static int ntex;
static int frame_open;      /* a frame was started (cleared) since the last pageflip */
static unsigned frames;
static Uint32 fps_t0;

/* The game's coordinate space: its current resolution, an index (0x511e58) into its table of
   20-byte entries {.., w, h, bpp} at 0x48707c. (The DirectDraw display mode can differ: with
   a saved 1600x1200 the game sets that mode, then renders at 1280x960.) */
void thrash_game_size(int *w, int *h)
{
    u32 k = rd32(0x511e58);
    *w = k <= 10 ? (int)rd32(0x487080 + 20 * k) : 640;
    *h = k <= 10 ? (int)rd32(0x487084 + 20 * k) : 480;
    if (*w < 320 || *h < 200) { *w = 640; *h = 480; }
}

static void begin_if_needed(void)
{
    int w, h;
    if (frame_open) return;
    frame_open = 1;
    thrash_game_size(&w, &h);
    glp_begin_frame(w, h);
}

static void log_once(const char *name)
{
    static char seen[64][24];
    static int n;
    for (int k = 0; k < n; k++)
        if (!strcmp(seen[k], name)) return;
    if (n < 64) snprintf(seen[n++], sizeof seen[0], "%s", name);
    plat_log("THRASH %s called (not used by the game so far)", name);
}

/* ---- set-up ---------------------------------------------------------------------------- */

static u32 imp_about(Cpu *c)
{
    (void)c;
    if (!about_p) {
        about_p = galloc(0x80);
        wr32(about_p + 0x00, 0x4c47504f);  /* 'OPGL'; must not be 'XFD3' (the game's 3Dfx flag) */
        wr32(about_p + 0x04, 0x80);
        wr32(about_p + 0x08, 0x68);        /* interface version the game requires */
        /* display modes the driver offers: entries 1..n of 0x28 bytes {w, h, bpp}; the game
           enables the matching entries of its resolution list (widths >= 512 only). No
           1600x1200: its HUD scaling is broken (CLAUDE.md). */
        static const int modes[][2] = { {512, 384}, {640, 480}, {800, 600}, {960, 720}, {1024, 768}, {1120, 840}, {1280, 960} };
        int n = sizeof modes / sizeof modes[0];
        u32 list = galloc(0x28 * (n + 1));
        for (int k = 0; k < n; k++) {
            wr32(list + 0x28 * (k + 1) + 0, modes[k][0]);
            wr32(list + 0x28 * (k + 1) + 4, modes[k][1]);
            wr32(list + 0x28 * (k + 1) + 8, 16);
        }
        wr32(about_p + 0x3c, n);
        wr32(about_p + 0x40, list);
        gstrcpy(about_p + 0x4c, "OpenGL 3.3 (native)", 0x30);
    }
    return about_p;
}

static u32 imp_is(Cpu *c) { (void)c; return glp_active(); }
static u32 imp_selectdisplay(Cpu *c) { (void)c; return 0; }

static u32 imp_init(Cpu *c)
{
    (void)c;
    if (!glp_active()) {
        plat_log("THRASH init: no OpenGL context");
        return 0;
    }
    plat_log("THRASH init ok");
    return 1;
}

static u32 imp_setvideomode(Cpu *c)
{
    /* opengla.dll's defaults (docs/THRASH_NOTES.md) */
    static const u32 defaults[][2] = { {1, 0}, {2, 1}, {7, 1}, {3, 0}, {5, 0}, {6, 1}, {10, 2}, {11, 1}, {4, 1}, {14, 0}, {13, 0} };
    (void)c;
    for (unsigned k = 0; k < sizeof defaults / sizeof defaults[0]; k++) states[defaults[k][0]] = defaults[k][1];
    frame_open = 0;
    {
        int w, h;
        thrash_game_size(&w, &h);
        plat_log("THRASH setvideomode: %dx%d", w, h);
    }
    return 1;
}

static u32 imp_window(Cpu *c) { (void)c; return 1; }
static u32 imp_treset(Cpu *c);
static u32 imp_restore(Cpu *c) { frame_open = 0; imp_treset(c); return 1; }
static u32 imp_setstate(Cpu *c)
{
    u32 id = ARG(0), v = ARG(1);
    if (id >= 1 && id <= 23) states[id] = v;
    else if (id == 0x194) blend_src = v;
    else if (id == 0x195) blend_dst = v;
    return 1;
}

/* texture filter: env FCOP_TEXFILTER (linear/nearest) wins, else the game's own Filtering
   option (state 7 = 1), as opengla.dll does */
static int tex_linear(void)
{
    const char *e = getenv("FCOP_TEXFILTER");
    if (e && *e) return strcasecmp(e, "nearest") != 0;
    return states[7] == 1;
}

/* ---- textures -------------------------------------------------------------------------- */

/* talloc(w, h, fmt, palette, mips) -> texture object */
static u32 imp_talloc(Cpu *c)
{
    u32 obj;
    if (ntex >= MAX_TEX) {
        plat_log("THRASH talloc: out of textures");
        return 0;
    }
    obj = galloc(16);
    texs[ntex].gl = glp_tex_new();
    texs[ntex].w = (int)ARG(0);
    texs[ntex].h = (int)ARG(1);
    texs[ntex].fmt = (int)ARG(2);
    if (texs[ntex].fmt != 7) plat_log("THRASH talloc: format %d treated as 4444", texs[ntex].fmt);
    wr32(obj, (u32)++ntex);
    return obj;
}

static int tex_index(u32 obj)
{
    u32 k = obj ? rd32(obj) : 0;
    return k >= 1 && k <= (u32)ntex ? (int)k - 1 : -1;
}

/* tupdate(tex, pixels, palette): convert and upload */
static u32 imp_tupdate(Cpu *c)
{
    static u8 *rgba;
    static size_t cap;
    int k = tex_index(ARG(0));
    u32 px = ARG(1);
    size_t need;
    if (k < 0 || !px) return 0;
    need = (size_t)texs[k].w * texs[k].h * 4;
    if (need > cap && !(rgba = realloc(rgba, cap = need))) { cap = 0; return 0; }
    thrash_tex_to_rgba8((const u16 *)G(px), rgba, texs[k].w, texs[k].h, texs[k].fmt);
    texs[k].hash = thrash_tex_hash((const u16 *)G(px), texs[k].w, texs[k].h);
    glp_tex_upload(texs[k].gl, texs[k].w, texs[k].h, rgba, tex_linear(), states[13] != 0);
    return ARG(0);
}

static u32 imp_settexture(Cpu *c)
{
    states[1] = ARG(0);
    return 1;
}

static u32 imp_treset(Cpu *c)
{
    (void)c;
    for (int k = 0; k < ntex; k++) glp_tex_delete(texs[k].gl);
    ntex = 0;
    states[1] = 0;
    return 1;
}

/* ---- drawing --------------------------------------------------------------------------- */

/* a game vertex (32 bytes, docs/THRASH_NOTES.md) as the GL backend takes it */
static void vertex(u32 p, struct GlpVertex *o)
{
    u32 col = rd32(p + 0x10);
    memcpy(&o->x, G(p), 16);  /* x, y, z, 1/w */
    o->r = (u8)(col >> 16);
    o->g = (u8)(col >> 8);
    o->b = (u8)col;
    o->a = (u8)(col >> 24);
    memcpy(&o->u, G(p + 0x18), 8);
}

static void apply_states(void)
{
    int k = states[1] ? tex_index(states[1]) : -1;
    begin_if_needed();
    glp_set_texture(k >= 0 ? texs[k].gl : 0);
    glp_set_blend(states[10] != 0, (int)blend_src, (int)blend_dst);
    glp_set_depth_test(states[4] != 0);
    glp_set_cull((int)states[2]);
}

static u32 imp_drawtri(Cpu *c)
{
    struct GlpVertex v[3];
    apply_states();
    for (int k = 0; k < 3; k++) vertex(ARG(k), &v[k]);
    glp_draw(0, v, 3);
    return 1;
}

/* drawtrimesh(n, verts, idx): n triangles, 3n u32 indices into the 32-byte vertex array */
static u32 imp_drawtrimesh(Cpu *c)
{
    static struct GlpVertex v[3 * 256];
    int n = (int)ARG(0), done = 0;
    u32 verts = ARG(1), idx = ARG(2);
    apply_states();
    while (done < 3 * n) {
        int m = 3 * n - done > (int)(sizeof v / sizeof v[0]) ? (int)(sizeof v / sizeof v[0]) : 3 * n - done;
        for (int k = 0; k < m; k++) vertex(verts + 32 * rd32(idx + 4 * (u32)(done + k)), &v[k]);
        glp_draw(0, v, m);
        done += m;
    }
    return 1;
}

static u32 imp_drawline(Cpu *c)
{
    struct GlpVertex v[2];
    apply_states();
    vertex(ARG(0), &v[0]);
    vertex(ARG(1), &v[1]);
    glp_draw(1, v, 2);
    return 1;
}

static u32 imp_flushwindow(Cpu *c) { (void)c; begin_if_needed(); return 1; }

static u32 imp_pageflip(Cpu *c)
{
    Uint32 now = SDL_GetTicks();
    (void)c;
    begin_if_needed();
    if (!getenv("FCOP_THRASH_NODRAW")) glp_swap();  /* NODRAW: measure the game without the GPU path */
    frame_open = 0;
    thrash_frame_last = 1;
    frames++;
    if (now - fps_t0 >= 1000) {
        if (fps_t0 && getenv("FCOP_FPS")) plat_log("THRASH fps %u", frames * 1000 / (now - fps_t0));
        fps_t0 = now;
        frames = 0;
    }
    return 1;
}

/* ---- calls the game never makes: safe no-ops ------------------------------------------- */

#define UNUSED(name, ret) static u32 imp_##name(Cpu *c) { (void)c; log_once(#name); return ret; }
UNUSED(clearwindow, 1) UNUSED(clip, 1) UNUSED(idle, 1) UNUSED(sync, 0) UNUSED(lockwindow, 0)
UNUSED(unlockwindow, 1) UNUSED(readrect, 0) UNUSED(writerect, 0) UNUSED(drawquad, 1)
UNUSED(drawquadmesh, 1) UNUSED(drawtristrip, 1) UNUSED(drawtrifan, 1) UNUSED(drawlinemesh, 1)
UNUSED(drawlinestrip, 1) UNUSED(drawpoint, 1) UNUSED(drawpointmesh, 1) UNUSED(drawpointstrip, 1)

static const struct ImpDef thrash_funcs[] = {
    IMPORT(about, 0), IMPORT(is, 0), IMPORT(window, 1), IMPORT(clearwindow, 0), IMPORT(flushwindow, 0),
    IMPORT(pageflip, 0), IMPORT(idle, 0), IMPORT(sync, 1), IMPORT(lockwindow, 0), IMPORT(unlockwindow, 1),
    IMPORT(clip, 4), IMPORT(setstate, 2), IMPORT(readrect, 5), IMPORT(writerect, 5), IMPORT(drawquad, 4),
    IMPORT(drawquadmesh, 3), IMPORT(drawtri, 3), IMPORT(drawtrimesh, 3), IMPORT(drawtristrip, 2),
    IMPORT(drawtrifan, 2), IMPORT(drawline, 2), IMPORT(drawlinemesh, 3), IMPORT(drawlinestrip, 2),
    IMPORT(drawpoint, 1), IMPORT(drawpointmesh, 3), IMPORT(drawpointstrip, 2), IMPORT(init, 0),
    IMPORT(restore, 0), IMPORT(selectdisplay, 1), IMPORT(setvideomode, 3), IMPORT(talloc, 5),
    IMPORT(tupdate, 3), IMPORT(settexture, 1), IMPORT(treset, 0),
};

u32 thrash_load(const char *dll_name)
{
    size_t n = strlen(dll_name);
    if (n < 6 || strcasecmp(dll_name + n - 5, "a.dll") || !thrash_wanted()) return 0;
    if (!glp_active()) {
        plat_log("THRASH: %s requested but OpenGL is not available: software renderer", dll_name);
        return 0;
    }
    plat_log("THRASH: %s -> OpenGL", dll_name);
    return THRASH_MODULE;
}

u32 thrash_proc(u32 module, const char *name)
{
    if (module != THRASH_MODULE || strncmp(name, "_THRASH_", 8)) return 0;
    name += 8;
    for (unsigned k = 0; k < sizeof thrash_funcs / sizeof thrash_funcs[0]; k++) {
        size_t l = strlen(thrash_funcs[k].name);
        if (!strncmp(name, thrash_funcs[k].name, l) && (name[l] == '@' || !name[l]))
            return thunk_new(thrash_funcs[k].name, thrash_funcs[k].nargs, thrash_funcs[k].fn);
    }
    return 0;
}
