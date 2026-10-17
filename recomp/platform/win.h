/* State shared by the USER32, DirectDraw, DirectInput and DirectSound implementations:
 * windows, the message queue, keyboard and mouse state. Implemented in u32_win.c. */
#ifndef FCOP_WIN_H
#define FCOP_WIN_H
#include <SDL.h>
#include "host.h"

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

#endif
