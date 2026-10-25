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
/* display size in OpenGL/3Dfx mode (Preferences): REG_DWORD PortResolution, the size of the
   picture we render (fullscreen: scaled to the display; windowed: the window size); 0 = the
   desktop's. Returns 1 and the size when one is set. */
int display_res_pref(int *w, int *h);
void display_res_pref_save(int w, int h);
/* widescreen in missions (REG_DWORD PortWidescreen, missing: on) */
int widescreen_pref(void);
void widescreen_pref_save(int on);
/* frame rate limit (Preferences, REG_DWORD FpsLimit in registry.txt): FPS_ORIGINAL is the
   game's own 37.5 fps mission limit, 0 none, else frames per second (missing: none) */
#define FPS_ORIGINAL 37
int fps_limit_pref(void);
void fps_limit_pref_save(int fps);
/* fps.c: apply a limit now; the one in effect */
void fps_limit_apply(int fps);
int fps_limit_current(void);

#endif
