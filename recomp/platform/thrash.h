/* The game's THRASH 3D driver interface, implemented natively on OpenGL (thrash_gl.c).
 * docs/THRASH_NOTES.md describes the calls. */
#ifndef FCOP_THRASH_H
#define FCOP_THRASH_H
#include "host.h"
#include "renderer.h"

/* 1 when the OpenGL (THRASH) renderer should be offered: renderer_wanted() == REND_OPENGL (gdi_misc.c) */
int thrash_wanted(void);

/* LoadLibraryA / GetProcAddress hooks (k32_proc.c): a THRASH driver name ("...a.dll") gives a
   pseudo module when OpenGL is wanted and running, else 0; its "_THRASH_<fn>@N" exports are
   thunks into thrash_gl.c */
u32 thrash_load(const char *dll_name);
u32 thrash_proc(u32 module, const char *name);

/* 1 while the picture on screen is a THRASH (mission) frame, not a 16-bit DirectDraw one */
extern int thrash_frame_last;
/* the game's current resolution: the coordinate space of THRASH vertices */
void thrash_game_size(int *w, int *h);
/* widescreen.c: follow the drawable's aspect (call between frames) */
void widescreen_update(void);
/* turn widescreen on or off (Preferences; applies from the next frame) */
void widescreen_enable(int on);
/* widescreen.c: 1 when the display-list walkers hand over vertices in the list's own 2560x1920
   units (text in game pixels), not rounded to the game's resolution (FCOP_PRECISE=0: off) */
int precise_vertices(void);
/* start the current frame if not yet (thrash_gl.c, glide_gl.c) */
void thrash_frame_begin(void);
void glide_frame_begin(void);

#endif
