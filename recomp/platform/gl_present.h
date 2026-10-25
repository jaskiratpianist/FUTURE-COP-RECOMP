/* OpenGL backend: one GL 3.3 core context on the main window, used for everything shown in
 * OpenGL mode (menus, dialogs and videos as 16-bit frames, missions as THRASH triangles).
 * Kept apart from host.h: SDL_opengl.h pulls in <windows.h> on Windows. */
#ifndef FCOP_GL_PRESENT_H
#define FCOP_GL_PRESENT_H
#include <stdint.h>
#include <SDL.h>

/* create the context on w (once); 0 on failure or when FCOP_GL_FAIL=1 */
int  glp_init(SDL_Window *w);
int  glp_active(void);
/* largest centred 4:3 rectangle {x, y, w, h} in a drawable of draw_w x draw_h */
void glp_viewport_43(int draw_w, int draw_h, int out_xywh[4]);
/* the same for any aspect aspect_w:aspect_h (square game pixels: frames are drawn in the
   aspect of their game coordinate space, e.g. Glide's 1280x1024 screen) */
void glp_viewport_fit(int draw_w, int draw_h, int aspect_w, int aspect_h, int out_xywh[4]);
/* the scissor rectangle {x, y from the bottom, w, h} in drawable pixels for the game
   rectangle x0..x1, y0..y1 (clamped to the game_w x game_h space) inside viewport vp */
void glp_clip_rect(const int vp[4], int game_w, int game_h, int x0, int y0, int x1, int y1, int out_xywh[4]);
/* show a 16-bit frame (555 or 565, by masks) scaled into the 4:3 viewport, and swap */
void glp_show16(const void *px, int w, int h, int pitch_bytes, uint32_t rmask, uint32_t gmask, uint32_t bmask);
/* start a THRASH frame in a game coordinate space of game_w x game_h: clear to black */
void glp_begin_frame(int game_w, int game_h);
void glp_swap(void);

/* drawing THRASH primitives in the current frame's game coordinates: x, y pixels, z depth
   0..1, q = 1/w (perspective-correct texturing), colour, texture coordinates. Batched;
   state changes and glp_swap flush the batch. */
struct GlpVertex { float x, y, z, q; uint8_t r, g, b, a; float u, v; };
void glp_set_texture(unsigned tex);  /* 0: untextured */
/* blending on/off with THRASH blend factor codes (setstate 0x194 source, 0x195 destination):
   0 ONE, 1 ZERO, 2 SRC_ALPHA, 3 ONE_MINUS_SRC_ALPHA, 4 DST_ALPHA, 5 ONE_MINUS_DST_ALPHA,
   6 SRC_COLOR, 7 DST_COLOR, 8 ONE_MINUS_SRC_COLOR, 9 ONE_MINUS_DST_COLOR */
void glp_set_blend(int on, int src, int dst);
void glp_set_depth_test(int on);     /* LEQUAL */
void glp_set_cull(int mode);         /* 0 off, 1 front faces clockwise, 2 counter-clockwise */
void glp_draw(int lines, const struct GlpVertex *v, int n);  /* triangles (n % 3 == 0) or lines */
/* Glide (glide_gl.c, docs/GLIDE_NOTES.md): glide = 0 for THRASH colour (texture x vertex),
   1 for Glide's colour/alpha combine; cc and ac pack function | factor << 8 | local << 16 |
   other << 24 (Glide 2 codes) */
void glp_set_combine(int glide, int cc, int ac);
void glp_set_const_color(uint32_t argb);
/* discard where the "other" colour is rgb; smooth (filtered texture): soft edges (gl_present.c) */
void glp_set_chromakey(int on, uint32_t rgb, int smooth);
void glp_set_clip(int on, int x0, int y0, int x1, int y1);  /* game coordinates, scissor */
/* gamma for the frame being drawn (out = in^(1/g), whole picture, at present time); each new
   frame starts at 1 */
void glp_set_gamma(float g);
/* mouse positions: window coordinates (SDL events) <-> the game coordinates of the last frame
   (SDL only scales mouse events itself when a renderer has a logical size) */
void glp_window_to_game(int *x, int *y);
void glp_game_to_window(int *x, int *y);
/* the last frame drawn, scaled to w x h as RGB565 (dialogs and the pause bar draw over it) */
int  glp_read_frame16(uint16_t *dst, int w, int h);

/* textures (RGBA8); linear/clamp choose filtering and wrapping */
unsigned glp_tex_new(void);
void glp_tex_upload(unsigned tex, int w, int h, const uint8_t *rgba, int linear, int clamp);
void glp_tex_delete(unsigned tex);

#endif
