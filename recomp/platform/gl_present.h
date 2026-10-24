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
/* show a 16-bit frame (555 or 565, by masks) scaled into the 4:3 viewport, and swap */
void glp_show16(const void *px, int w, int h, int pitch_bytes, uint32_t rmask, uint32_t gmask, uint32_t bmask);
/* start a THRASH frame in a game coordinate space of game_w x game_h: clear to black */
void glp_begin_frame(int game_w, int game_h);
void glp_swap(void);

/* textures (RGBA8); linear/clamp choose filtering and wrapping */
unsigned glp_tex_new(void);
void glp_tex_upload(unsigned tex, int w, int h, const uint8_t *rgba, int linear, int clamp);
void glp_tex_delete(unsigned tex);

#endif
