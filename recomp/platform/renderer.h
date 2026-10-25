/* Which mission renderer the player chose (gdi_misc.c) */
#ifndef FCOP_RENDERER_H
#define FCOP_RENDERER_H

enum { REND_SOFTWARE, REND_OPENGL, REND_GLIDE };

/* env FCOP_RENDERER (software/opengl/glide) wins, else the registry value Group under
   HKLM\Software\Electronic Arts\Future Cop: "Software", "3Dfx" (Glide, glide_gl.c), anything
   else OpenGL (THRASH, thrash_gl.c) */
int renderer_wanted(void);

#endif
