/* State shared by the USER32, DirectDraw, DirectInput and DirectSound implementations:
 * windows, the message queue, keyboard and mouse state. Implemented in u32_win.c. */
#ifndef FCOP_WIN_H
#define FCOP_WIN_H
#include <SDL.h>
#include "host.h"

/* The "desktop" the game sees: big enough for its largest display mode (1600x1200), so a
   window of any game resolution fits on it, as on a real Windows desktop. */
#define DESKTOP_W 1600
#define DESKTOP_H 1200

/* a window created by CreateWindowExA (the game creates one main window) */
struct Win {
    u32 hwnd;            /* handle_new(H_OTHER, this) */
    u32 wndproc;         /* guest address of the window procedure */
    u32 style, exstyle, userdata, menu, parent;
    int x, y, w, h;      /* outer rectangle (client area = whole window, no frame) */
    int visible;
    SDL_Window *sdl;     /* NULL until shown */
};
struct Win *win_get(u32 hwnd);       /* NULL if not a window */
extern struct Win *main_win;         /* first top-level window created */

/* message queue (thread-safe; one queue for the process is enough for this game) */
struct Msg { u32 hwnd, message, wparam, lparam, time, pt_x, pt_y; };
void msg_post(u32 hwnd, u32 message, u32 wparam, u32 lparam);
int  msg_peek(struct Msg *m, u32 hwnd, u32 min, u32 max, int remove);  /* 1 if one was found */

/* translate pending SDL events into window messages and the key/mouse state below.
   Must be called regularly (PeekMessageA/GetMessageA do it). */
void plat_pump(void);

/* keyboard state by Windows virtual-key code (VK_*), and DirectInput scancodes (DIK_*) */
extern volatile u8 vk_down[256];
extern volatile u8 dik_down[256];
extern volatile int mouse_x, mouse_y, mouse_buttons;  /* client coordinates, bit 0 left, 1 right, 2 middle */
u32 sdl_to_vk(SDL_Keycode k);
u32 sdl_to_dik(SDL_Scancode s);

/* the frame the game wants shown: DirectDraw calls this after Flip / Blt to the primary */
void win_present(const void *pixels, int w, int h, int pitch_bytes, int bits, u32 rmask, u32 gmask, u32 bmask);
/* fullscreen (DirectDraw exclusive mode) or a normal window; Alt+Enter toggles it too */
void win_set_fullscreen(int on);
/* show a frame without keeping it as the game's last frame (dialogs drawn over the game) */
void win_show(const void *pixels, int w, int h, int pitch_bytes, int bits, u32 rmask, u32 gmask, u32 bmask);
/* the game's last presented 16-bit frame (w*h pixels, no padding); 0 if none yet */
int win_last_frame(const u16 **px, int *w, int *h, int *is565);
/* if set, plat_pump offers every SDL event (except SDL_QUIT) to it first; returning 1 consumes
   it (a modal dialog takes the mouse and keyboard). Scripted FCOP_INPUT events go through it too. */
extern int (*win_event_hook)(const SDL_Event *e);

#endif
