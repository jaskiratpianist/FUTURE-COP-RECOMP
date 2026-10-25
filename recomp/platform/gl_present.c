/* OpenGL backend (see gl_present.h). GL functions are loaded at run time with
 * SDL_GL_GetProcAddress; GLP_NO_GL builds only the pure helpers (unit test). */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_present.h"

void glp_viewport_43(int draw_w, int draw_h, int out_xywh[4])
{
    glp_viewport_fit(draw_w, draw_h, 4, 3, out_xywh);
}

void glp_viewport_fit(int draw_w, int draw_h, int aspect_w, int aspect_h, int out_xywh[4])
{
    int w = draw_w, h = (int)((long long)draw_w * aspect_h / aspect_w);
    if (h > draw_h) {
        h = draw_h;
        w = (int)((long long)draw_h * aspect_w / aspect_h);
    }
    out_xywh[0] = (draw_w - w) / 2;
    out_xywh[1] = (draw_h - h) / 2;
    out_xywh[2] = w;
    out_xywh[3] = h;
}

void glp_clip_rect(const int vp4[4], int gw, int gh, int x0, int y0, int x1, int y1, int out_xywh[4])
{
    long long xe, ye;
    x0 = x0 < 0 ? 0 : x0 > gw ? gw : x0;
    x1 = x1 < x0 ? x0 : x1 > gw ? gw : x1;
    y0 = y0 < 0 ? 0 : y0 > gh ? gh : y0;
    y1 = y1 < y0 ? y0 : y1 > gh ? gh : y1;
    out_xywh[0] = vp4[0] + (int)((long long)x0 * vp4[2] / gw);
    xe = vp4[0] + (long long)x1 * vp4[2] / gw;
    out_xywh[1] = vp4[1] + (int)((long long)(gh - y1) * vp4[3] / gh);  /* GL counts y from the bottom */
    ye = vp4[1] + (long long)(gh - y0) * vp4[3] / gh;
    out_xywh[2] = (int)(xe - out_xywh[0]);
    out_xywh[3] = (int)(ye - out_xywh[1]);
}

#ifndef GLP_NO_GL
#include <SDL_opengl.h>

void plat_log(const char *fmt, ...);

#define GLP_FUNCS(X) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Clear, (GLbitfield)) \
    X(void, Enable, (GLenum)) \
    X(void, Disable, (GLenum)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, DepthFunc, (GLenum)) \
    X(void, DepthMask, (GLboolean)) \
    X(void, FrontFace, (GLenum)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint))

#define GLP_DECL(ret, name, args) ret (APIENTRY *name) args;
static struct { GLP_FUNCS(GLP_DECL) } gl;

static SDL_Window *glp_window;
static SDL_GLContext glp_ctx;
static SDL_threadID glp_thread;
static GLuint prog, vao, vbo, frame_tex;
static GLint u_size, u_use_tex, u_glide, u_cc, u_ac, u_const, u_chroma, u_key, u_gamma;
static int game_w = 640, game_h = 480;
/* everything is drawn into an offscreen framebuffer of the drawable's size (readable even when
   the window is hidden, for FCOP_SHOT) and copied to the window on swap */
static GLuint fbo, rb_color, rb_depth;
static int fbo_w, fbo_h;
static int vp[4];  /* the 4:3 viewport inside it */
static GLuint small_fbo, small_rb;  /* glp_read_frame16 */
static int small_w, small_h;
static GLuint post_fbo, post_tex;  /* gamma pass */
static int post_w, post_h;
static float frame_gamma = 1;

/* the batch: vertices sharing one state */
#define BATCH_MAX 65536
static struct GlpVertex batch[BATCH_MAX];
static int batch_n;
/* glide: 0 = THRASH colour (texture x vertex), 1 = the Glide combine in cc/ac, which pack
   function | factor << 8 | local << 16 | other << 24; chroma = key colour + 1 (0 = off);
   clip = 1 with the scissor rectangle cx0..cy1 in game coordinates */
static struct {
    unsigned tex; int blend, bsrc, bdst, depth, cull, lines;
    int glide, cc, ac; uint32_t cconst, chroma; int clip, cx0, cy0, cx1, cy1;
} want = { 0, 0, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, have;
static int have_valid;
static void flush(void);

static const char *vs_src =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_col;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform vec2 u_size;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    float w = a_pos.w > 0.0 ? 1.0 / a_pos.w : 1.0;\n"  /* q = 1/w: perspective-correct u, v */
    "    vec2 ndc = vec2(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0);\n"
    "    gl_Position = vec4(ndc, a_pos.z * 2.0 - 1.0, 1.0) * w;\n"
    "    v_col = a_col;\n"
    "    v_uv = a_uv;\n"
    "}\n";
/* u_glide = 1: Glide 2 colour/alpha combine (docs/GLIDE_NOTES.md): u_cc / u_ac = (function,
   factor, local, other); local 0 iterated, 1 constant; other 0 iterated, 1 texture, 2 constant.
   The chromakey compares the "other" colour (for texture modes the unfiltered texel), as the
   Voodoo did. u_gamma is only set for the gamma pass at present time. */
static const char *fs_src =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_use_tex, u_glide, u_chroma;\n"
    "uniform ivec4 u_cc, u_ac;\n"
    "uniform vec4 u_const;\n"
    "uniform vec3 u_key;\n"
    "uniform float u_gamma;\n"
    "out vec4 o_col;\n"
    /* factor: 0 zero, 1 local, 2 other alpha, 3 local alpha, 4 texture alpha, 8 one, 9..12 one minus 1..4 */
    "vec3 fac(int f, vec3 local, float la, float oa, float ta) {\n"
    "    vec3 v = f == 0 ? vec3(0.0) : f == 1 || f == 9 ? local : f == 2 || f == 10 ? vec3(oa)\n"
    "           : f == 3 || f == 11 ? vec3(la) : f == 4 || f == 12 ? vec3(ta) : vec3(1.0);\n"
    "    return f >= 9 ? 1.0 - v : v;\n"
    "}\n"
    "vec3 comb(int fn, vec3 f, vec3 local, float la, vec3 other) {\n"
    "    if (fn == 0) return vec3(0.0);\n"
    "    if (fn == 1) return local;\n"
    "    if (fn == 2) return vec3(la);\n"
    "    if (fn == 3) return f * other;\n"
    "    if (fn == 4) return f * other + local;\n"
    "    if (fn == 5) return f * other + la;\n"
    "    if (fn == 6) return f * (other - local);\n"
    "    if (fn == 7) return f * (other - local) + local;\n"
    "    if (fn == 8) return f * (other - local) + la;\n"
    "    if (fn == 9) return local - f * local;\n"
    "    return la - f * local;\n"
    "}\n"
    "void main() {\n"
    "    vec4 t = u_use_tex != 0 ? texture(u_tex, v_uv) : vec4(1.0);\n"
    "    if (u_glide == 0) {\n"
    "        o_col = v_col * t;\n"
    "    } else {\n"
    "        vec3 cl = u_cc.z == 1 ? u_const.rgb : u_cc.z == 0 ? v_col.rgb : vec3(0.0);\n"
    "        vec3 co = u_cc.w == 1 ? t.rgb : u_cc.w == 2 ? u_const.rgb : v_col.rgb;\n"
    "        float al = u_ac.z == 1 ? u_const.a : u_ac.z == 0 ? v_col.a : 0.0;\n"
    "        float ao = u_ac.w == 1 ? t.a : u_ac.w == 2 ? u_const.a : v_col.a;\n"
    "        if (u_chroma != 0) {\n"
    "            vec3 k = co;\n"
    "            if (u_cc.w == 1 && u_use_tex != 0) {\n"
    "                ivec2 sz = textureSize(u_tex, 0);\n"
    "                k = texelFetch(u_tex, clamp(ivec2(floor(v_uv * vec2(sz))), ivec2(0), sz - 1), 0).rgb;\n"
    "            }\n"
    "            if (all(lessThan(abs(k - u_key), vec3(0.5 / 255.0)))) discard;\n"
    "        }\n"
    "        o_col.rgb = clamp(comb(u_cc.x, fac(u_cc.y, cl, al, ao, t.a), cl, al, co), 0.0, 1.0);\n"
    "        o_col.a = clamp(comb(u_ac.x, fac(u_ac.y, vec3(al), al, ao, t.a), vec3(al), al, vec3(ao)).x, 0.0, 1.0);\n"
    "    }\n"
    "    o_col.rgb = pow(o_col.rgb, vec3(1.0 / u_gamma));\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
    GLint ok = 0;
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        plat_log("GL shader error: %s", log);
        return 0;
    }
    return s;
}

static int load_functions(void)
{
#define GLP_LOAD(ret, name, args) \
    if (!(*(void **)&gl.name = SDL_GL_GetProcAddress("gl" #name))) { plat_log("GL: missing gl" #name); return 0; }
    GLP_FUNCS(GLP_LOAD)
    return 1;
}

int glp_init(SDL_Window *w)
{
    GLuint vs, fs;
    GLint ok = 0;
    if (glp_ctx) return 1;
    if (getenv("FCOP_GL_FAIL")) {
        plat_log("GL: start-up failure forced by FCOP_GL_FAIL");
        return 0;
    }
    if (!(glp_ctx = SDL_GL_CreateContext(w))) {
        plat_log("GL: no context: %s", SDL_GetError());
        return 0;
    }
    glp_window = w;
    glp_thread = SDL_ThreadID();
    if (!load_functions() || !(vs = compile(GL_VERTEX_SHADER, vs_src)) || !(fs = compile(GL_FRAGMENT_SHADER, fs_src))) {
        SDL_GL_DeleteContext(glp_ctx);
        glp_ctx = NULL;
        return 0;
    }
    prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    gl.LinkProgram(prog);
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        plat_log("GL: shader link failed");
        SDL_GL_DeleteContext(glp_ctx);
        glp_ctx = NULL;
        return 0;
    }
    gl.UseProgram(prog);
    u_size = gl.GetUniformLocation(prog, "u_size");
    u_use_tex = gl.GetUniformLocation(prog, "u_use_tex");
    u_glide = gl.GetUniformLocation(prog, "u_glide");
    u_cc = gl.GetUniformLocation(prog, "u_cc");
    u_ac = gl.GetUniformLocation(prog, "u_ac");
    u_const = gl.GetUniformLocation(prog, "u_const");
    u_chroma = gl.GetUniformLocation(prog, "u_chroma");
    u_key = gl.GetUniformLocation(prog, "u_key");
    u_gamma = gl.GetUniformLocation(prog, "u_gamma");
    gl.Uniform1f(u_gamma, 1);
    gl.Uniform1i(gl.GetUniformLocation(prog, "u_tex"), 0);
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(struct GlpVertex), (void *)offsetof(struct GlpVertex, x));
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct GlpVertex), (void *)offsetof(struct GlpVertex, r));
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(struct GlpVertex), (void *)offsetof(struct GlpVertex, u));
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
    gl.GenTextures(1, &frame_tex);
    SDL_GL_SetSwapInterval(1);
    plat_log("GL: %s, %s", gl.GetString(GL_VERSION), gl.GetString(GL_RENDERER));
    return 1;
}

int glp_active(void)
{
    return glp_ctx != NULL;
}

static void check_thread(void)
{
    static int warned;
    if (SDL_ThreadID() != glp_thread && !warned) {
        warned = 1;
        plat_log("warn: GL used from thread %lu, context belongs to %lu", (unsigned long)SDL_ThreadID(), (unsigned long)glp_thread);
    }
}

static void fbo_resize(int w, int h)
{
    if (!fbo) {
        gl.GenFramebuffers(1, &fbo);
        gl.GenRenderbuffers(1, &rb_color);
        gl.GenRenderbuffers(1, &rb_depth);
    }
    gl.BindRenderbuffer(GL_RENDERBUFFER, rb_color);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    gl.BindRenderbuffer(GL_RENDERBUFFER, rb_depth);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_color);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) plat_log("GL: framebuffer incomplete");
    fbo_w = w;
    fbo_h = h;
}

/* clear the whole drawable to black and set the 4:3 viewport for a game_w x game_h space */
static void start_drawable(int w, int h)
{
    int dw, dh;
    check_thread();
    SDL_GL_GetDrawableSize(glp_window, &dw, &dh);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw != fbo_w || dh != fbo_h) fbo_resize(dw, dh);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, dw, dh);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glp_viewport_fit(dw, dh, w, h, vp);
    gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    game_w = w;
    game_h = h;
    gl.Uniform2f(u_size, (float)w, (float)h);
}

void glp_show16(const void *px, int w, int h, int pitch_bytes, uint32_t rmask, uint32_t gmask, uint32_t bmask)
{
    struct GlpVertex q[6];
    static const float corner[6][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1} };
    int k, is565 = gmask == 0x7e0;
    (void)rmask; (void)bmask;
    if (!glp_ctx) return;
    flush();
    start_drawable(w, h);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Uniform1i(u_glide, 0);
    frame_gamma = 1;
    gl.BindTexture(GL_TEXTURE_2D, frame_tex);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, pitch_bytes / 2);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (is565) gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    else gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, px);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (k = 0; k < 6; k++) {
        q[k].x = corner[k][0] * w;
        q[k].y = corner[k][1] * h;
        q[k].z = 0;
        q[k].q = 1;
        q[k].r = q[k].g = q[k].b = q[k].a = 255;
        q[k].u = corner[k][0];
        q[k].v = corner[k][1];
    }
    gl.Uniform1i(u_use_tex, 1);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    have_valid = 0;
    glp_swap();
}

static void flush(void)
{
    if (!batch_n) return;
    if (!have_valid || want.blend != have.blend || want.bsrc != have.bsrc || want.bdst != have.bdst) {
        static const GLenum factor[10] = { GL_ONE, GL_ZERO, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
            GL_ONE_MINUS_DST_ALPHA, GL_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_COLOR };
        if (want.blend) gl.Enable(GL_BLEND); else gl.Disable(GL_BLEND);
        gl.BlendFunc(factor[want.bsrc], factor[want.bdst]);
    }
    if (!have_valid || want.depth != have.depth) {
        if (want.depth) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_LEQUAL);
    }
    if (!have_valid || want.cull != have.cull) {
        if (want.cull) gl.Enable(GL_CULL_FACE); else gl.Disable(GL_CULL_FACE);
        gl.FrontFace(want.cull == 1 ? GL_CW : GL_CCW);
    }
    if (!have_valid || want.tex != have.tex) {
        if (want.tex) gl.BindTexture(GL_TEXTURE_2D, want.tex);
        gl.Uniform1i(u_use_tex, want.tex != 0);
    }
    if (!have_valid || want.glide != have.glide || want.cc != have.cc || want.ac != have.ac) {
        gl.Uniform1i(u_glide, want.glide);
        gl.Uniform4i(u_cc, want.cc & 255, want.cc >> 8 & 255, want.cc >> 16 & 255, want.cc >> 24 & 255);
        gl.Uniform4i(u_ac, want.ac & 255, want.ac >> 8 & 255, want.ac >> 16 & 255, want.ac >> 24 & 255);
    }
    if (!have_valid || want.cconst != have.cconst)
        gl.Uniform4f(u_const, (want.cconst >> 16 & 255) / 255.0f, (want.cconst >> 8 & 255) / 255.0f, (want.cconst & 255) / 255.0f,
                     (want.cconst >> 24) / 255.0f);
    if (!have_valid || want.chroma != have.chroma) {
        uint32_t k = want.chroma - 1;
        gl.Uniform1i(u_chroma, want.chroma != 0);
        gl.Uniform3f(u_key, (k >> 16 & 255) / 255.0f, (k >> 8 & 255) / 255.0f, (k & 255) / 255.0f);
    }
    if (!have_valid || want.clip != have.clip || want.cx0 != have.cx0 || want.cy0 != have.cy0 || want.cx1 != have.cx1 ||
        want.cy1 != have.cy1) {
        if (want.clip) {
            int r[4];
            glp_clip_rect(vp, game_w, game_h, want.cx0, want.cy0, want.cx1, want.cy1, r);
            gl.Enable(GL_SCISSOR_TEST);
            gl.Scissor(r[0], r[1], r[2], r[3]);
        } else {
            gl.Disable(GL_SCISSOR_TEST);
        }
    }
    have = want;
    have_valid = 1;
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(batch_n * sizeof batch[0]), batch, GL_STREAM_DRAW);
    gl.DrawArrays(want.lines ? GL_LINES : GL_TRIANGLES, 0, batch_n);
    batch_n = 0;
}

#define SET_STATE(field, value) do { if (want.field != (value)) { flush(); want.field = (value); } } while (0)
void glp_set_texture(unsigned tex) { SET_STATE(tex, tex); }
void glp_set_blend(int on, int src, int dst)
{
    SET_STATE(blend, on != 0);
    SET_STATE(bsrc, src >= 0 && src <= 9 ? src : 2);
    SET_STATE(bdst, dst >= 0 && dst <= 9 ? dst : 3);
}
void glp_set_depth_test(int on) { SET_STATE(depth, on != 0); }
void glp_set_cull(int mode) { SET_STATE(cull, mode); }
void glp_set_combine(int glide, int cc, int ac)
{
    SET_STATE(glide, glide != 0);
    SET_STATE(cc, cc);
    SET_STATE(ac, ac);
}
void glp_set_const_color(uint32_t argb) { SET_STATE(cconst, argb); }
void glp_set_chromakey(int on, uint32_t rgb) { SET_STATE(chroma, on ? (rgb & 0xffffff) + 1 : 0); }
void glp_set_clip(int on, int x0, int y0, int x1, int y1)
{
    SET_STATE(clip, on != 0);
    SET_STATE(cx0, x0);
    SET_STATE(cy0, y0);
    SET_STATE(cx1, x1);
    SET_STATE(cy1, y1);
}
void glp_set_gamma(float g) { frame_gamma = g > 0.1f && g < 10 ? g : 1; }

void glp_draw(int lines, const struct GlpVertex *v, int n)
{
    if (!glp_ctx || n > BATCH_MAX) return;
    SET_STATE(lines, lines != 0);
    if (batch_n + n > BATCH_MAX) flush();
    memcpy(batch + batch_n, v, (size_t)n * sizeof *v);
    batch_n += n;
}

/* the 4:3 picture in window coordinates {x, y from the top, w, h}, as start_drawable places it */
static int picture_rect(double r[4])
{
    int ww, wh, dw, dh, d[4];
    if (!glp_window) return 0;
    SDL_GetWindowSize(glp_window, &ww, &wh);
    SDL_GL_GetDrawableSize(glp_window, &dw, &dh);
    if (ww < 1 || wh < 1 || dw < 1 || dh < 1) return 0;
    glp_viewport_fit(dw, dh, game_w, game_h, d);
    r[0] = (double)d[0] * ww / dw;
    r[1] = (double)(dh - d[1] - d[3]) * wh / dh;  /* GL counts y from the bottom */
    r[2] = (double)d[2] * ww / dw;
    r[3] = (double)d[3] * wh / dh;
    return r[2] > 0 && r[3] > 0;
}

void glp_window_to_game(int *x, int *y)
{
    double r[4];
    int gx, gy;
    if (!picture_rect(r)) return;
    gx = (int)((*x - r[0]) * game_w / r[2]);
    gy = (int)((*y - r[1]) * game_h / r[3]);
    *x = gx < 0 ? 0 : gx >= game_w ? game_w - 1 : gx;
    *y = gy < 0 ? 0 : gy >= game_h ? game_h - 1 : gy;
}

void glp_game_to_window(int *x, int *y)
{
    double r[4];
    if (!picture_rect(r)) return;
    *x = (int)(r[0] + (*x + 0.5) * r[2] / game_w);
    *y = (int)(r[1] + (*y + 0.5) * r[3] / game_h);
}

int glp_read_frame16(uint16_t *dst, int w, int h)
{
    static uint8_t *rgb;
    static size_t cap;
    size_t need = (size_t)w * h * 3;
    if (!glp_ctx || !fbo || w < 1 || h < 1) return 0;
    if (need > cap && !(rgb = realloc(rgb, cap = need))) { cap = 0; return 0; }
    flush();
    gl.Disable(GL_SCISSOR_TEST);
    have_valid = 0;
    if (!small_fbo) {
        gl.GenFramebuffers(1, &small_fbo);
        gl.GenRenderbuffers(1, &small_rb);
    }
    if (w != small_w || h != small_h) {
        gl.BindRenderbuffer(GL_RENDERBUFFER, small_rb);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        gl.BindFramebuffer(GL_FRAMEBUFFER, small_fbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, small_rb);
        small_w = w;
        small_h = h;
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo);
    gl.BlitFramebuffer(vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, small_fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (int y = 0; y < h; y++) {
        const uint8_t *p = rgb + (size_t)(h - 1 - y) * w * 3;  /* GL rows are bottom-up */
        for (int x = 0; x < w; x++, p += 3)
            dst[(size_t)y * w + x] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
    }
    return 1;
}

void glp_begin_frame(int w, int h)
{
    if (!glp_ctx) return;
    flush();
    start_drawable(w, h);
    have_valid = 0;
    frame_gamma = 1;
}

unsigned glp_tex_new(void)
{
    GLuint t = 0;
    if (glp_ctx) gl.GenTextures(1, &t);
    return t;
}

void glp_tex_upload(unsigned tex, int w, int h, const uint8_t *rgba, int linear, int clamp)
{
    if (!glp_ctx || !tex) return;
    flush();  /* draws already batched must use the old content */
    have_valid = 0;  /* the binding changes */
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

void glp_tex_delete(unsigned tex)
{
    GLuint t = tex;
    if (!glp_ctx || !t) return;
    flush();
    have_valid = 0;
    gl.DeleteTextures(1, &t);
}

/* FCOP_SHOT=path: write each shown frame (the 4:3 picture as drawn) as a PPM */
static void shot(void)
{
    static int n;
    static uint8_t *buf;
    static size_t cap;
    const char *path = getenv("FCOP_SHOT");
    char name[512];
    FILE *f;
    size_t need = (size_t)vp[2] * vp[3] * 3;
    n++;
    if (!path) return;
    if (need > cap && !(buf = realloc(buf, cap = need))) { cap = 0; return; }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGB, GL_UNSIGNED_BYTE, buf);
    snprintf(name, sizeof name, path, n);
    if (!(f = fopen(name, "wb"))) return;
    fprintf(f, "P6\n%d %d\n255\n", vp[2], vp[3]);
    for (int y = vp[3] - 1; y >= 0; y--) fwrite(buf + (size_t)y * vp[2] * 3, 1, (size_t)vp[2] * 3, f);  /* GL rows are bottom-up */
    fclose(f);
    if (getenv("FCOP_SHOT_LOG")) plat_log("frame %d presented (%dx%d)", n, vp[2], vp[3]);
}

/* the picture through out = in^(1/g) (Glide's gamma table), in place: copy the frame to a
   texture and draw it back through the shader */
static void gamma_pass(float g)
{
    struct GlpVertex q[6];
    static const float corner[6][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1} };
    if (!post_fbo) {
        gl.GenFramebuffers(1, &post_fbo);
        gl.GenTextures(1, &post_tex);
    }
    gl.BindTexture(GL_TEXTURE_2D, post_tex);
    if (post_w != fbo_w || post_h != fbo_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbo_w, fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, post_fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, post_tex, 0);
        post_w = fbo_w;
        post_h = fbo_h;
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, post_fbo);
    gl.BlitFramebuffer(0, 0, fbo_w, fbo_h, 0, 0, fbo_w, fbo_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.Viewport(0, 0, fbo_w, fbo_h);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    for (int k = 0; k < 6; k++) {
        q[k].x = corner[k][0] * fbo_w;
        q[k].y = corner[k][1] * fbo_h;
        q[k].z = 0;
        q[k].q = 1;
        q[k].r = q[k].g = q[k].b = q[k].a = 255;
        q[k].u = corner[k][0];
        q[k].v = 1 - corner[k][1];  /* texture rows are bottom-up */
    }
    gl.Uniform2f(u_size, (float)fbo_w, (float)fbo_h);
    gl.Uniform1i(u_glide, 0);
    gl.Uniform1i(u_use_tex, 1);
    gl.Uniform1f(u_gamma, g);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    gl.Uniform1f(u_gamma, 1);
    gl.Uniform2f(u_size, (float)game_w, (float)game_h);
    gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    have_valid = 0;
}

void glp_swap(void)
{
    if (!glp_ctx || !fbo) return;
    flush();
    gl.Disable(GL_SCISSOR_TEST);
    have_valid = 0;
    if (frame_gamma != 1) gamma_pass(frame_gamma);
    shot();
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BlitFramebuffer(0, 0, fbo_w, fbo_h, 0, 0, fbo_w, fbo_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SDL_GL_SwapWindow(glp_window);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
}
#endif
