/* The game's built-in 3dfx Glide 2.x renderer (normally glide2x.dll) implemented natively:
 * LoadLibraryA("glide2x.dll") returns a pseudo module, GetProcAddress hands out thunks to the
 * functions below, and they draw through gl_present.c at the real screen resolution.
 * docs/GLIDE_NOTES.md describes every call the game makes. */
#include <SDL.h>
#include "glide.h"
#include "thrash.h"
#include "win.h"
#include "gl_present.h"
#include "texconv.h"
#include "glide_tmem.h"

#define GLIDE_MODULE 0x61000000u

static int frame_open;      /* a frame was started since the last grBufferSwap */
static unsigned frames;
static Uint32 fps_t0;
static float gamma_value = 1;
static int filter_linear = 1, clamp_st = 1;
static int chroma_on;
static uint32_t chroma_key;
static int tex_size = 256;  /* of the texture chosen by grTexSource */

/* one GL texture per texture-memory address the game draws from */
#define MAX_TEX 512
static struct { u32 addr; int size; unsigned gl; int linear, clamp; } texs[MAX_TEX];
static int ntex;

/* the Glide screen grSstWinOpen opened: the coordinate space of the game's vertices (at its
   1280x960 setting the game opens 1280x1024 and centres the 3D view in it) */
static int screen_w = 640, screen_h = 480;

static void begin_if_needed(void)
{
    if (frame_open) return;
    frame_open = 1;
    glp_begin_frame(screen_w, screen_h);
    glp_set_gamma(gamma_value);
}

static void log_once(const char *what)
{
    static char seen[32][48];
    static int n;
    for (int k = 0; k < n; k++)
        if (!strcmp(seen[k], what)) return;
    if (n < 32) snprintf(seen[n++], sizeof seen[0], "%s", what);
    plat_log("Glide: %s", what);
}

/* ---- set-up ---------------------------------------------------------------------------- */

static u32 imp_grGlideInit(Cpu *c)
{
    (void)c;
    for (int k = 0; k < ntex; k++) glp_tex_delete(texs[k].gl);
    ntex = 0;
    gtm_reset();
    return 1;
}

static u32 imp_grSstQueryBoards(Cpu *c) { wr32(ARG(0), 1); return 1; }

/* GrHwConfiguration: one Voodoo with 6 MB frame buffer: the game then offers resolutions up to
   1280x960 and not 1600x1200, whose HUD scaling is broken (docs/GLIDE_NOTES.md) */
static u32 imp_grSstQueryHardware(Cpu *c)
{
    u32 h = ARG(0);
    memset(G(h), 0, 0x90);
    wr32(h + 0x00, 1);    /* num_sst */
    wr32(h + 0x04, 0);    /* GR_SSTTYPE_VOODOO */
    wr32(h + 0x08, 6);    /* fbRam (MB) */
    wr32(h + 0x0c, 2);    /* fbiRev */
    wr32(h + 0x10, 1);    /* nTexelfx */
    wr32(h + 0x18, 1);    /* tmu 0 revision */
    wr32(h + 0x1c, 4);    /* tmu 0 RAM (MB) */
    return 1;
}

/* fails without OpenGL: the game then uses its software renderer */
static u32 imp_grSstWinOpen(Cpu *c)
{
    /* GR_RESOLUTION_* codes 0..15 */
    static const short size[16][2] = { {320, 200}, {320, 240}, {400, 256}, {512, 384}, {640, 200}, {640, 350}, {640, 400}, {640, 480},
        {800, 600}, {960, 720}, {856, 480}, {512, 256}, {1024, 768}, {1280, 1024}, {1600, 1200}, {400, 300} };
    u32 res = ARG(1);
    if (!win_gl_ready(ARG(0))) {
        plat_log("Glide grSstWinOpen: OpenGL is not available: software renderer");
        return 0;
    }
    if (res < 16) {
        screen_w = size[res][0];
        screen_h = size[res][1];
    } else {
        thrash_game_size(&screen_w, &screen_h);
    }
    plat_log("Glide grSstWinOpen: resolution code %u, %dx%d", res, screen_w, screen_h);
    frame_open = 0;
    return 1;
}

static u32 imp_grSstWinClose(Cpu *c) { (void)c; frame_open = 0; return 1; }
static u32 imp_grTexMinAddress(Cpu *c) { (void)c; return 0; }
static u32 imp_grTexMaxAddress(Cpu *c) { (void)c; return GTM_SIZE; }

static u32 imp_grGammaCorrectionValue(Cpu *c)
{
    u32 bits = ARG(0);
    memcpy(&gamma_value, &bits, 4);
    return 0;
}

/* texture filter: env FCOP_TEXFILTER (linear/nearest) wins, else the game's choice */
static int tex_linear(void)
{
    const char *e = getenv("FCOP_TEXFILTER");
    if (e && *e) return strcasecmp(e, "nearest") != 0;
    return filter_linear;
}

static u32 imp_grTexFilterMode(Cpu *c) { filter_linear = ARG(2) == 1; return 0; }        /* GR_TEXTUREFILTER_BILINEAR */
static u32 imp_grTexClampMode(Cpu *c) { clamp_st = ARG(1) == 1 && ARG(2) == 1; return 0; } /* GR_TEXTURECLAMP_CLAMP */
static u32 imp_grChromakeyMode(Cpu *c) { chroma_on = ARG(0) != 0; glp_set_chromakey(chroma_on, chroma_key); return 0; }
static u32 imp_grChromakeyValue(Cpu *c) { chroma_key = ARG(0); glp_set_chromakey(chroma_on, chroma_key); return 0; }

/* ---- textures -------------------------------------------------------------------------- */

/* GrTexInfo {smallLod, largeLod, aspectRatio, format, data}: the game only uses one 256x256
   ARGB1555 level */
static int info_size(u32 info)
{
    if (rd32(info + 8) != 3) log_once("texture aspect ratio other than 1:1 (drawn as square)");
    if (rd32(info + 12) != 0xb) log_once("texture format other than ARGB1555 (drawn as 1555)");
    return gtm_lod_size((int)rd32(info + 4));
}

static u32 imp_grTexDownloadMipMap(Cpu *c)
{
    u32 info = ARG(3);
    int s = info_size(info);
    if (!gtm_download(ARG(1), s, s, (const u16 *)G(rd32(info + 16)))) log_once("texture download out of range");
    return 0;
}

/* (tmu, start, thisLod, largeLod, aspect, format, evenOdd, data, first row, last row); data
   points at the first row */
static u32 imp_grTexDownloadMipMapLevelPartial(Cpu *c)
{
    int s = gtm_lod_size((int)ARG(2));
    if (!gtm_download_rows(ARG(1), s, s, (int)ARG(8), (int)ARG(9), (const u16 *)G(ARG(7)))) log_once("partial download out of range");
    return 0;
}

/* (tmu, start, thisLod, largeLod, aspect, format, evenOdd, data) */
static u32 imp_grTexDownloadMipMapLevel(Cpu *c)
{
    int s = gtm_lod_size((int)ARG(2));
    if (!gtm_download(ARG(1), s, s, (const u16 *)G(ARG(7)))) log_once("level download out of range");
    return 0;
}

static u32 imp_grTexSource(Cpu *c)
{
    static u8 *rgba;
    static size_t cap;
    u32 addr = ARG(1);
    int s = info_size(ARG(3)), k, dirty;
    const u16 *px;
    for (k = 0; k < ntex; k++)
        if (texs[k].addr == addr && texs[k].size == s) break;
    if (k == ntex) {
        if (ntex == MAX_TEX) { log_once("out of textures"); return 0; }
        texs[k].addr = addr;
        texs[k].size = s;
        texs[k].gl = glp_tex_new();
        texs[k].linear = -1;
        ntex++;
    }
    px = gtm_texels(addr, s, s, &dirty);
    if (!px) { log_once("texture source out of range"); return 0; }
    if (dirty || texs[k].linear != tex_linear() || texs[k].clamp != clamp_st) {
        size_t need = (size_t)s * s * 4;
        if (need > cap && !(rgba = realloc(rgba, cap = need))) { cap = 0; return 0; }
        tex_1555_to_rgba8(px, rgba, s * s);
        texs[k].linear = tex_linear();
        texs[k].clamp = clamp_st;
        glp_tex_upload(texs[k].gl, s, s, rgba, texs[k].linear, texs[k].clamp);
    }
    tex_size = s;
    glp_set_texture(texs[k].gl);
    return 0;
}

/* ---- states ---------------------------------------------------------------------------- */

static int color_cc = 0x01000103, alpha_cc = 0x02010001;  /* texture x iterated, constant alpha */

static void combine_args(Cpu *c, int *packed, const char *what)
{
    if (ARG(4)) log_once(what);
    *packed = (int)(ARG(0) & 255) | (int)(ARG(1) & 255) << 8 | (int)(ARG(2) & 255) << 16 | (int)(ARG(3) & 255) << 24;
    glp_set_combine(1, color_cc, alpha_cc);
}

static u32 imp_grColorCombine(Cpu *c) { combine_args(c, &color_cc, "inverted colour combine (ignored)"); return 0; }
static u32 imp_grAlphaCombine(Cpu *c) { combine_args(c, &alpha_cc, "inverted alpha combine (ignored)"); return 0; }

/* the game always lets the texture through (GLIDE_NOTES.md); anything else is logged */
static u32 imp_grTexCombine(Cpu *c)
{
    if (ARG(1) != 1 || ARG(3) != 1) log_once("texture combine other than LOCAL (ignored)");
    return 0;
}

static u32 imp_grConstantColorValue(Cpu *c) { glp_set_const_color(ARG(0)); return 0; }

static u32 imp_grAlphaBlendFunction(Cpu *c)
{
    glp_set_blend(1, glide_blend_to_glp((int)ARG(0), 0), glide_blend_to_glp((int)ARG(1), 1));
    return 0;
}

static u32 imp_grClipWindow(Cpu *c)
{
    glp_set_clip(1, (int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3));
    return 0;
}

/* ---- drawing --------------------------------------------------------------------------- */

/* GrVertex (60 bytes): x, y, z, r, g, b, ooz, a, oow, then TMU 0's sow, tow, oow */
static void vertex(u32 p, struct GlpVertex *o)
{
    float f[12];
    memcpy(f, G(p), sizeof f);
    o->x = f[0];
    o->y = f[1];
    o->z = 0.5f;  /* no depth buffer: the game sorts */
    o->q = f[11] > 0 ? f[11] : 1;
    o->r = (u8)(f[3] <= 0 ? 0 : f[3] >= 255 ? 255 : f[3]);
    o->g = (u8)(f[4] <= 0 ? 0 : f[4] >= 255 ? 255 : f[4]);
    o->b = (u8)(f[5] <= 0 ? 0 : f[5] >= 255 ? 255 : f[5]);
    o->a = (u8)(f[7] <= 0 ? 0 : f[7] >= 255 ? 255 : f[7]);
    o->u = f[9] / o->q / (float)tex_size;   /* s/w over 1/w: texels, then 0..1 */
    o->v = f[10] / o->q / (float)tex_size;
}

static u32 imp_grDrawTriangle(Cpu *c)
{
    struct GlpVertex v[3];
    begin_if_needed();
    for (int k = 0; k < 3; k++) vertex(ARG(k), &v[k]);
    glp_draw(0, v, 3);
    return 0;
}

static u32 imp_grDrawLine(Cpu *c)
{
    struct GlpVertex v[2];
    begin_if_needed();
    vertex(ARG(0), &v[0]);
    vertex(ARG(1), &v[1]);
    glp_draw(1, v, 2);
    return 0;
}

/* a point: one game pixel as two triangles */
static u32 imp_grDrawPoint(Cpu *c)
{
    struct GlpVertex p, v[6];
    static const float d[6][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1} };
    begin_if_needed();
    vertex(ARG(0), &p);
    for (int k = 0; k < 6; k++) {
        v[k] = p;
        v[k].x += d[k][0];
        v[k].y += d[k][1];
    }
    glp_draw(0, v, 6);
    return 0;
}

static u32 imp_grBufferClear(Cpu *c)
{
    (void)c;
    begin_if_needed();  /* a new frame starts cleared to black, as the game clears */
    return 0;
}

static u32 imp_grBufferSwap(Cpu *c)
{
    Uint32 now = SDL_GetTicks();
    (void)c;
    begin_if_needed();
    if (!getenv("FCOP_THRASH_NODRAW")) glp_swap();
    frame_open = 0;
    thrash_frame_last = 1;  /* a GL mission frame is on screen (u32_win.c reads it back for dialogs) */
    frames++;
    if (now - fps_t0 >= 1000) {
        if (fps_t0 && getenv("FCOP_FPS")) plat_log("Glide fps %u", frames * 1000 / (now - fps_t0));
        fps_t0 = now;
        frames = 0;
    }
    return 0;
}

/* calls that need no work here: return 1 (FXTRUE where it matters) */
static u32 imp_ok(Cpu *c) { (void)c; return 1; }

/* the other exports the game loads but does not call (FCOP_TRACE_API names any call) */
static u32 imp_unused(Cpu *c)
{
    (void)c;
    log_once("a function the game was not seen calling was called (FCOP_TRACE_API=1 names it)");
    return 0;
}

static const struct ImpDef glide_funcs[] = {
    { "grGlideInit", 0, imp_grGlideInit }, { "grGlideShutdown", 0, imp_ok }, { "grSstQueryBoards", 1, imp_grSstQueryBoards },
    { "grSstQueryHardware", 1, imp_grSstQueryHardware }, { "grSstSelect", 1, imp_ok }, { "grSstWinOpen", 7, imp_grSstWinOpen },
    { "grSstWinClose", 0, imp_grSstWinClose }, { "grSstControl", 1, imp_ok }, { "grSstIdle", 0, imp_ok },
    { "grBufferClear", 3, imp_grBufferClear }, { "grBufferSwap", 1, imp_grBufferSwap }, { "grClipWindow", 4, imp_grClipWindow },
    { "grDrawTriangle", 3, imp_grDrawTriangle }, { "guDrawTriangleWithClip", 3, imp_grDrawTriangle },
    { "grDrawLine", 2, imp_grDrawLine }, { "grDrawPoint", 1, imp_grDrawPoint }, { "grColorCombine", 5, imp_grColorCombine },
    { "grAlphaCombine", 5, imp_grAlphaCombine }, { "grTexCombine", 7, imp_grTexCombine },
    { "grConstantColorValue", 1, imp_grConstantColorValue }, { "grAlphaBlendFunction", 4, imp_grAlphaBlendFunction },
    { "grChromakeyMode", 1, imp_grChromakeyMode }, { "grChromakeyValue", 1, imp_grChromakeyValue }, { "grDitherMode", 1, imp_ok },
    { "grDepthBufferMode", 1, imp_ok }, { "grGammaCorrectionValue", 1, imp_grGammaCorrectionValue },
    { "grTexSource", 4, imp_grTexSource }, { "grTexMinAddress", 1, imp_grTexMinAddress }, { "grTexMaxAddress", 1, imp_grTexMaxAddress },
    { "grTexDownloadMipMap", 4, imp_grTexDownloadMipMap }, { "grTexDownloadMipMapLevel", 8, imp_grTexDownloadMipMapLevel },
    { "grTexDownloadMipMapLevelPartial", 10, imp_grTexDownloadMipMapLevelPartial }, { "grTexClampMode", 3, imp_grTexClampMode },
    { "grTexFilterMode", 3, imp_grTexFilterMode }, { "grTexMipMapMode", 3, imp_ok },
};

u32 glide_load(const char *dll_name)
{
    /* loaded at start-up, before the OpenGL window exists: grSstWinOpen checks for OpenGL */
    if (strcasecmp(dll_name, "glide2x.dll") || renderer_wanted() != REND_GLIDE) return 0;
    plat_log("Glide: %s -> OpenGL", dll_name);
    return GLIDE_MODULE;
}

u32 glide_proc(u32 module, const char *name)
{
    /* the game looks every function up again at each mission start: one thunk each, made once */
    static struct { char name[48]; u32 thunk; } made[160];
    static int nmade;
    const char *at = strchr(name, '@');
    size_t len = at ? (size_t)(at - name) : strlen(name);
    int nargs = at ? atoi(at + 1) / 4 : 0;
    ImpFn fn = imp_unused;
    if (module != GLIDE_MODULE || name[0] != '_' || len >= sizeof made[0].name) return 0;
    for (int k = 0; k < nmade; k++)
        if (!strcmp(made[k].name, name)) return made[k].thunk;
    if (nmade == (int)(sizeof made / sizeof made[0])) return 0;
    for (unsigned k = 0; k < sizeof glide_funcs / sizeof glide_funcs[0]; k++)
        if (strlen(glide_funcs[k].name) == len - 1 && !strncmp(name + 1, glide_funcs[k].name, len - 1)) {
            fn = glide_funcs[k].fn;
            if (glide_funcs[k].nargs != nargs) plat_log("Glide: %s takes %d arguments, not %d", name, nargs, glide_funcs[k].nargs);
        }
    snprintf(made[nmade].name, sizeof made[0].name, "%s", name);
    made[nmade].thunk = thunk_new(made[nmade].name, nargs, fn);
    return made[nmade++].thunk;
}
