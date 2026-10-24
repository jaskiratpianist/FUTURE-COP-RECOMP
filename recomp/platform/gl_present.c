/* OpenGL backend (see gl_present.h). GL functions are loaded at run time with
 * SDL_GL_GetProcAddress; GLP_NO_GL builds only the pure helpers (unit test). */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_present.h"

void glp_viewport_43(int draw_w, int draw_h, int out_xywh[4])
{
    int w = draw_w, h = draw_w * 3 / 4;
    if (h > draw_h) {
        h = draw_h;
        w = draw_h * 4 / 3;
    }
    out_xywh[0] = (draw_w - w) / 2;
    out_xywh[1] = (draw_h - h) / 2;
    out_xywh[2] = w;
    out_xywh[3] = h;
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
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))

#define GLP_DECL(ret, name, args) ret (APIENTRY *name) args;
static struct { GLP_FUNCS(GLP_DECL) } gl;

/* one vertex as the shader takes it: position (x, y in game pixels, z depth 0..1, q = 1/w),
   colour RGBA, texture coordinates */
struct GlpVertex { float x, y, z, q; uint8_t r, g, b, a; float u, v; };

static SDL_Window *glp_window;
static SDL_GLContext glp_ctx;
static SDL_threadID glp_thread;
static GLuint prog, vao, vbo, frame_tex;
static GLint u_size, u_use_tex;
static int game_w = 640, game_h = 480;

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
static const char *fs_src =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_use_tex;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "    o_col = u_use_tex != 0 ? v_col * texture(u_tex, v_uv) : v_col;\n"
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

/* clear the whole drawable to black and set the 4:3 viewport for a game_w x game_h space */
static void start_drawable(int w, int h)
{
    int dw, dh, r[4];
    check_thread();
    SDL_GL_GetDrawableSize(glp_window, &dw, &dh);
    gl.Viewport(0, 0, dw, dh);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glp_viewport_43(dw, dh, r);
    gl.Viewport(r[0], r[1], r[2], r[3]);
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
    start_drawable(w, h);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
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
    glp_swap();
}

void glp_begin_frame(int w, int h)
{
    if (!glp_ctx) return;
    start_drawable(w, h);
}

void glp_swap(void)
{
    if (!glp_ctx) return;
    SDL_GL_SwapWindow(glp_window);
}
#endif
