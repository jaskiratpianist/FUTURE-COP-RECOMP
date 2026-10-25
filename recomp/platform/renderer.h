/* Which mission renderer the player chose (gdi_misc.c) */
#ifndef FCOP_RENDERER_H
#define FCOP_RENDERER_H

enum { REND_SOFTWARE, REND_OPENGL, REND_GLIDE };

/* env FCOP_RENDERER (software/opengl/glide) wins, else the registry value Group under
   HKLM\Software\Electronic Arts\Future Cop: "Software", "3Dfx" (Glide, glide_gl.c), anything
   else OpenGL (THRASH, thrash_gl.c) */
int renderer_wanted(void);
/* write REND_* as Group to registry.txt (the Preferences dialog's choice); the game reads it
   at start-up, so it applies at the next start */
void renderer_save(int r);
/* fullscreen in OpenGL/3Dfx mode, where the game itself always asks for fullscreen: the
   player's choice (Preferences, Alt+Enter), kept as PortFullScreen in registry.txt */
int fullscreen_pref(void);
void fullscreen_pref_save(int on);
/* 1 while the game runs a hardware 3D mode (Glide or THRASH): its own mode value, set at
   start-up (0 when it fell back to software) */
int renderer_hardware_running(void);

#endif
