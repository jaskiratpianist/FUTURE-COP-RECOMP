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

#endif
