/* The game's built-in 3dfx Glide 2.x renderer, implemented natively on OpenGL (glide_gl.c).
 * docs/GLIDE_NOTES.md describes the calls. */
#ifndef FCOP_GLIDE_H
#define FCOP_GLIDE_H
#include "host.h"

/* LoadLibraryA / GetProcAddress hooks (k32_proc.c): "glide2x.dll" gives a pseudo module when
   Glide is the chosen renderer (renderer.h) and OpenGL runs, else 0; every "_gr..."/"_gu..."
   name gets a thunk (the game's loader fails on any NULL) */
u32 glide_load(const char *dll_name);
u32 glide_proc(u32 module, const char *name);

#endif
