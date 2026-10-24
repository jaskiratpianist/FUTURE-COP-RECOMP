/* The game's THRASH 3D driver interface, implemented natively on OpenGL (thrash_gl.c).
 * docs/THRASH_NOTES.md describes the calls. */
#ifndef FCOP_THRASH_H
#define FCOP_THRASH_H
#include "host.h"

/* 1 when the OpenGL renderer should be offered: env FCOP_RENDERER (opengl/software) wins,
   else the registry value Group under HKLM\Software\Electronic Arts\Future Cop is not
   "Software" (gdi_misc.c) */
int thrash_wanted(void);

#endif
