/* USER32.dll: u32_win
 * Window management, message loop, input and frame presentation.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "host.h"
#include "win.h"

/* Win32 Constants */
#define WM_CREATE            0x0001
#define WM_DESTROY           0x0002
#define WM_MOVE              0x0003
#define WM_SIZE              0x0005
#define WM_ACTIVATE          0x0006
#define WM_SETFOCUS          0x0007
#define WM_KILLFOCUS         0x0008
#define WM_ENABLE            0x000A
#define WM_SETTEXT           0x000C
#define WM_GETTEXT           0x000D
#define WM_GETTEXTLENGTH     0x000E
#define WM_PAINT             0x000F
#define WM_CLOSE             0x0010
#define WM_QUIT              0x0012
#define WM_ERASEBKGND        0x0014
#define WM_SHOWWINDOW        0x0018
#define WM_ACTIVATEAPP       0x001C
#define WM_SETCURSOR         0x0020
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE          0x0081
#define WM_NCDESTROY         0x0082
#define WM_NCHITTEST         0x0084
#define WM_KEYDOWN           0x0100
#define WM_KEYUP             0x0101
#define WM_CHAR              0x0102
#define WM_SYSKEYDOWN        0x0104
#define WM_SYSKEYUP          0x0105
#define WM_TIMER             0x0113
#define WM_MOUSEMOVE         0x0200
#define WM_LBUTTONDOWN       0x0201
#define WM_LBUTTONUP         0x0202
#define WM_LBUTTONDBLCLK     0x0203
#define WM_RBUTTONDOWN       0x0204
#define WM_RBUTTONUP         0x0205
#define WM_RBUTTONDBLCLK     0x0206
#define WM_MBUTTONDOWN       0x0207
#define WM_MBUTTONUP         0x0208
#define WM_MBUTTONDBLCLK     0x0209

#define GWL_WNDPROC          (-4)
#define GWL_HINSTANCE        (-6)
#define GWL_HWNDPARENT       (-8)
#define GWL_STYLE            (-16)
#define GWL_EXSTYLE          (-20)
#define GWL_USERDATA         (-21)
#define GWL_ID               (-12)

#define SWP_NOSIZE           0x0001
#define SWP_NOMOVE           0x0002
#define SWP_NOZORDER         0x0004
#define SWP_SHOWWINDOW       0x0040
#define SWP_HIDEWINDOW       0x0080

#define SM_CXSCREEN          0
#define SM_CYSCREEN          1
#define SM_CXVSCROLL         2
#define SM_CYHSCROLL         3
#define SM_CYCAPTION         4
#define SM_CXBORDER          5
#define SM_CYBORDER          6
#define SM_CXFRAME           32
#define SM_CYFRAME           33
#define SM_CXEDGE            45
#define SM_CYEDGE            46

/* Exported globals from win.h */
struct Win *main_win = NULL;
volatile u8 vk_down[256] = {0};
volatile u8 dik_down[256] = {0};
volatile int mouse_x = 0, mouse_y = 0, mouse_buttons = 0;

/* Internal Window Structure */
struct WinInternal {
    struct Win win;
    char class_name[128];
    char title[256];
    u32 instance;
    u32 extra[64];
    int activated;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    Uint32 tex_format;
};

#define MAX_WINDOWS 64
static struct WinInternal *win_table[MAX_WINDOWS];
static int win_count = 0;
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static int sdl_inited = 0;

/* Registered Window Classes */
struct WinClass {
    char name[128];
    u32 style;
    u32 wndproc;
    int cbClsExtra;
    int cbWndExtra;
    u32 hInstance;
    u32 hIcon;
    u32 hCursor;
    u32 hbrBackground;
    u32 menuName;
};
#define MAX_CLASSES 32
static struct WinClass win_classes[MAX_CLASSES];
static int win_class_count = 0;

/* Timers */
struct WinTimer {
    int active;
    u32 hwnd;
    u32 id;
    u32 interval;
    u32 last_tick;
    u32 proc;
};
#define MAX_TIMERS 32
static struct WinTimer win_timers[MAX_TIMERS];
static u32 next_timer_id = 1;

/* Message Queue */
#define MSG_QUEUE_CAP 512
static struct Msg msg_queue[MSG_QUEUE_CAP];
static int msg_head = 0, msg_tail = 0;
static pthread_mutex_t msg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Window Lookup */
struct Win *win_get(u32 hwnd)
{
    if (!hwnd) return NULL;
    pthread_mutex_lock(&win_lock);
    for (int i = 0; i < win_count; i++) {
        if (win_table[i] && win_table[i]->win.hwnd == hwnd) {
            pthread_mutex_unlock(&win_lock);
            return &win_table[i]->win;
        }
    }
    pthread_mutex_unlock(&win_lock);
    return NULL;
}

static struct WinInternal *win_get_internal(u32 hwnd)
{
    if (!hwnd) return NULL;
    pthread_mutex_lock(&win_lock);
    for (int i = 0; i < win_count; i++) {
        if (win_table[i] && win_table[i]->win.hwnd == hwnd) {
            pthread_mutex_unlock(&win_lock);
            return win_table[i];
        }
    }
    pthread_mutex_unlock(&win_lock);
    return NULL;
}

/* Ensure SDL is initialized and create SDL window if needed */
static void ensure_sdl_init(void)
{
    if (!sdl_inited) {
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            sdl_inited = 1;
        } else {
            plat_log("warn: SDL_Init failed: %s", SDL_GetError());
        }
    }
}

static void ensure_window_sdl(struct WinInternal *w)
{
    if (!w) return;
    ensure_sdl_init();
    if (!w->win.sdl) {
        int width = w->win.w > 0 ? w->win.w : 640;
        int height = w->win.h > 0 ? w->win.h : 480;
        w->win.sdl = SDL_CreateWindow(
            "Future Cop: L.A.P.D.",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height,
            SDL_WINDOW_HIDDEN
        );
        if (!w->win.sdl) {
            plat_log("warn: SDL_CreateWindow failed: %s", SDL_GetError());
        }
    }
}

/* Message Queue Functions */
void msg_post(u32 hwnd, u32 message, u32 wparam, u32 lparam)
{
    if (getenv("FCOP_TRACE_MSG")) plat_log("msg_post hwnd %x msg %x wp %x lp %x", hwnd, message, wparam, lparam);
    pthread_mutex_lock(&msg_lock);
    int next_tail = (msg_tail + 1) % MSG_QUEUE_CAP;
    if (next_tail == msg_head) {
        /* queue full: drop oldest */
        msg_head = (msg_head + 1) % MSG_QUEUE_CAP;
    }
    struct Msg *m = &msg_queue[msg_tail];
    m->hwnd = hwnd;
    m->message = message;
    m->wparam = wparam;
    m->lparam = lparam;
    m->time = SDL_GetTicks();
    m->pt_x = (u32)mouse_x;
    m->pt_y = (u32)mouse_y;
    msg_tail = next_tail;
    pthread_mutex_unlock(&msg_lock);
}

int msg_peek(struct Msg *m, u32 hwnd, u32 min, u32 max, int remove)
{
    pthread_mutex_lock(&msg_lock);
    int idx = msg_head;
    while (idx != msg_tail) {
        struct Msg *cand = &msg_queue[idx];
        int hwnd_ok = (hwnd == 0 || cand->hwnd == hwnd);
        int filter_ok = (min == 0 && max == 0) || (cand->message >= min && cand->message <= max);
        if (hwnd_ok && filter_ok) {
            if (m) *m = *cand;
            if (remove) {
                int cur = idx;
                int next = (cur + 1) % MSG_QUEUE_CAP;
                while (cur != msg_tail) {
                    msg_queue[cur] = msg_queue[next];
                    cur = next;
                    next = (cur + 1) % MSG_QUEUE_CAP;
                }
                msg_tail = (msg_tail - 1 + MSG_QUEUE_CAP) % MSG_QUEUE_CAP;
            }
            pthread_mutex_unlock(&msg_lock);
            return 1;
        }
        idx = (idx + 1) % MSG_QUEUE_CAP;
    }
    pthread_mutex_unlock(&msg_lock);
    return 0;
}

/* Direct dispatch of message to window procedure */
static u32 send_message_direct(u32 hwnd, u32 msg, u32 wparam, u32 lparam)
{
    struct WinInternal *w = win_get_internal(hwnd);
    if (w && w->win.wndproc) {
        if (!cur_cpu) {
            plat_log("warn: send_message_direct without cur_cpu");
            return 0;
        }
        u32 args[4] = { hwnd, msg, wparam, lparam };
        return guest_call(cur_cpu, w->win.wndproc, 4, args);
    }
    /* Fallback to default window procedure */
    switch (msg) {
        case WM_CLOSE:
            /* Default WM_CLOSE calls DestroyWindow */
            if (w) {
                if (w->win.sdl) {
                    if (w->renderer) { SDL_DestroyRenderer(w->renderer); w->renderer = NULL; }
                    if (w->texture) { SDL_DestroyTexture(w->texture); w->texture = NULL; }
                    SDL_DestroyWindow(w->win.sdl);
                    w->win.sdl = NULL;
                }
                handle_free(hwnd);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return 1; /* HTCLIENT */
        case WM_SETCURSOR:
            return 1;
        default:
            return 0;
    }
}

/* Scancode & Virtual Key mappings */
u32 sdl_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 0x41 + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return 0x30 + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (k - SDLK_KP_1);
    if (k == SDLK_KP_0) return 0x60;

    switch (k) {
        case SDLK_BACKSPACE: return 0x08; /* VK_BACK */
        case SDLK_TAB:       return 0x09; /* VK_TAB */
        case SDLK_RETURN:    return 0x0D; /* VK_RETURN */
        case SDLK_ESCAPE:    return 0x1B; /* VK_ESCAPE */
        case SDLK_SPACE:     return 0x20; /* VK_SPACE */
        case SDLK_PAGEUP:    return 0x21; /* VK_PRIOR */
        case SDLK_PAGEDOWN:  return 0x22; /* VK_NEXT */
        case SDLK_END:       return 0x23; /* VK_END */
        case SDLK_HOME:      return 0x24; /* VK_HOME */
        case SDLK_LEFT:      return 0x25; /* VK_LEFT */
        case SDLK_UP:        return 0x26; /* VK_UP */
        case SDLK_RIGHT:     return 0x27; /* VK_RIGHT */
        case SDLK_DOWN:      return 0x28; /* VK_DOWN */
        case SDLK_INSERT:    return 0x2D; /* VK_INSERT */
        case SDLK_DELETE:    return 0x2E; /* VK_DELETE */
        case SDLK_PAUSE:     return 0x13; /* VK_PAUSE */
        case SDLK_CAPSLOCK:  return 0x14; /* VK_CAPITAL */
        case SDLK_LSHIFT:    return 0xA0; /* VK_LSHIFT */
        case SDLK_RSHIFT:    return 0xA1; /* VK_RSHIFT */
        case SDLK_LCTRL:     return 0xA2; /* VK_LCONTROL */
        case SDLK_RCTRL:     return 0xA3; /* VK_RCONTROL */
        case SDLK_LALT:      return 0xA4; /* VK_LMENU */
        case SDLK_RALT:      return 0xA5; /* VK_RMENU */
        case SDLK_KP_MULTIPLY: return 0x6A;
        case SDLK_KP_PLUS:     return 0x6B;
        case SDLK_KP_MINUS:    return 0x6D;
        case SDLK_KP_PERIOD:   return 0x6E;
        case SDLK_KP_DIVIDE:   return 0x6F;
        case SDLK_SEMICOLON:   return 0xBA;
        case SDLK_EQUALS:      return 0xBB;
        case SDLK_COMMA:       return 0xBC;
        case SDLK_MINUS:       return 0xBD;
        case SDLK_PERIOD:      return 0xBE;
        case SDLK_SLASH:       return 0xBF;
        case SDLK_BACKQUOTE:   return 0xC0;
        case SDLK_LEFTBRACKET: return 0xDB;
        case SDLK_BACKSLASH:   return 0xDC;
        case SDLK_RIGHTBRACKET:return 0xDD;
        case SDLK_QUOTE:       return 0xDE;
        default: return 0;
    }
}

u32 sdl_to_dik(SDL_Scancode s)
{
    switch (s) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_1: return 0x02;
        case SDL_SCANCODE_2: return 0x03;
        case SDL_SCANCODE_3: return 0x04;
        case SDL_SCANCODE_4: return 0x05;
        case SDL_SCANCODE_5: return 0x06;
        case SDL_SCANCODE_6: return 0x07;
        case SDL_SCANCODE_7: return 0x08;
        case SDL_SCANCODE_8: return 0x09;
        case SDL_SCANCODE_9: return 0x0A;
        case SDL_SCANCODE_0: return 0x0B;
        case SDL_SCANCODE_MINUS: return 0x0C;
        case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_BACKSPACE: return 0x0E;
        case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_Q: return 0x10;
        case SDL_SCANCODE_W: return 0x11;
        case SDL_SCANCODE_E: return 0x12;
        case SDL_SCANCODE_R: return 0x13;
        case SDL_SCANCODE_T: return 0x14;
        case SDL_SCANCODE_Y: return 0x15;
        case SDL_SCANCODE_U: return 0x16;
        case SDL_SCANCODE_I: return 0x17;
        case SDL_SCANCODE_O: return 0x18;
        case SDL_SCANCODE_P: return 0x19;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
        case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_A: return 0x1E;
        case SDL_SCANCODE_S: return 0x1F;
        case SDL_SCANCODE_D: return 0x20;
        case SDL_SCANCODE_F: return 0x21;
        case SDL_SCANCODE_G: return 0x22;
        case SDL_SCANCODE_H: return 0x23;
        case SDL_SCANCODE_J: return 0x24;
        case SDL_SCANCODE_K: return 0x25;
        case SDL_SCANCODE_L: return 0x26;
        case SDL_SCANCODE_SEMICOLON: return 0x27;
        case SDL_SCANCODE_APOSTROPHE: return 0x28;
        case SDL_SCANCODE_GRAVE: return 0x29;
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_BACKSLASH: return 0x2B;
        case SDL_SCANCODE_Z: return 0x2C;
        case SDL_SCANCODE_X: return 0x2D;
        case SDL_SCANCODE_C: return 0x2E;
        case SDL_SCANCODE_V: return 0x2F;
        case SDL_SCANCODE_B: return 0x30;
        case SDL_SCANCODE_N: return 0x31;
        case SDL_SCANCODE_M: return 0x32;
        case SDL_SCANCODE_COMMA: return 0x33;
        case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_SLASH: return 0x35;
        case SDL_SCANCODE_RSHIFT: return 0x36;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
        case SDL_SCANCODE_LALT: return 0x38;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_CAPSLOCK: return 0x3A;
        case SDL_SCANCODE_F1: return 0x3B;
        case SDL_SCANCODE_F2: return 0x3C;
        case SDL_SCANCODE_F3: return 0x3D;
        case SDL_SCANCODE_F4: return 0x3E;
        case SDL_SCANCODE_F5: return 0x3F;
        case SDL_SCANCODE_F6: return 0x40;
        case SDL_SCANCODE_F7: return 0x41;
        case SDL_SCANCODE_F8: return 0x42;
        case SDL_SCANCODE_F9: return 0x43;
        case SDL_SCANCODE_F10: return 0x44;
        case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
        case SDL_SCANCODE_SCROLLLOCK: return 0x46;
        case SDL_SCANCODE_KP_7: return 0x47;
        case SDL_SCANCODE_KP_8: return 0x48;
        case SDL_SCANCODE_KP_9: return 0x49;
        case SDL_SCANCODE_KP_MINUS: return 0x4A;
        case SDL_SCANCODE_KP_4: return 0x4B;
        case SDL_SCANCODE_KP_5: return 0x4C;
        case SDL_SCANCODE_KP_6: return 0x4D;
        case SDL_SCANCODE_KP_PLUS: return 0x4E;
        case SDL_SCANCODE_KP_1: return 0x4F;
        case SDL_SCANCODE_KP_2: return 0x50;
        case SDL_SCANCODE_KP_3: return 0x51;
        case SDL_SCANCODE_KP_0: return 0x52;
        case SDL_SCANCODE_KP_PERIOD: return 0x53;
        case SDL_SCANCODE_F11: return 0x57;
        case SDL_SCANCODE_F12: return 0x58;
        case SDL_SCANCODE_KP_ENTER: return 0x9C;
        case SDL_SCANCODE_RCTRL: return 0x9D;
        case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
        case SDL_SCANCODE_RALT: return 0xB8;
        case SDL_SCANCODE_PAUSE: return 0xC5;
        case SDL_SCANCODE_HOME: return 0xC7;
        case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9;
        case SDL_SCANCODE_LEFT: return 0xCB;
        case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF;
        case SDL_SCANCODE_DOWN: return 0xD0;
        case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2;
        case SDL_SCANCODE_DELETE: return 0xD3;
        default: return 0;
    }
}

/* Check and trigger active timers */
static void check_timers(void)
{
    u32 now = SDL_GetTicks();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (win_timers[i].active && (now - win_timers[i].last_tick >= win_timers[i].interval)) {
            win_timers[i].last_tick = now;
            if (win_timers[i].proc) {
                if (cur_cpu) {
                    u32 args[4] = { win_timers[i].hwnd, WM_TIMER, win_timers[i].id, now };
                    guest_call(cur_cpu, win_timers[i].proc, 4, args);
                } else {
                    msg_post(win_timers[i].hwnd, WM_TIMER, win_timers[i].id, win_timers[i].proc);
                }
            } else {
                msg_post(win_timers[i].hwnd, WM_TIMER, win_timers[i].id, 0);
            }
        }
    }
}

static void handle_sdl_event(const SDL_Event *e)
{
    u32 target_hwnd = main_win ? main_win->hwnd : 0;

    switch (e->type) {
        case SDL_KEYDOWN: {
            u32 vk = sdl_to_vk(e->key.keysym.sym);
            u32 dik = sdl_to_dik(e->key.keysym.scancode);
            if (vk < 256) vk_down[vk] = 0x80;
            if (dik < 256) dik_down[dik] = 0x80;
            if (vk == 0xA0 || vk == 0xA1) vk_down[0x10] = 0x80; /* VK_SHIFT */
            if (vk == 0xA2 || vk == 0xA3) vk_down[0x11] = 0x80; /* VK_CONTROL */
            if (vk == 0xA4 || vk == 0xA5) vk_down[0x12] = 0x80; /* VK_MENU */

            int alt = (e->key.keysym.mod & KMOD_ALT) != 0;
            u32 msg = alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
            u32 lparam = 1 | ((dik & 0xFF) << 16) | (alt ? (1 << 29) : 0);
            if (e->key.repeat) lparam |= (1 << 30);
            msg_post(target_hwnd, msg, vk, lparam);

            /* WM_CHAR for printable keys and basic control characters */
            SDL_Keycode k = e->key.keysym.sym;
            if (k >= 32 && k < 127) {
                char ch = (char)k;
                if ((e->key.keysym.mod & KMOD_SHIFT) && ch >= 'a' && ch <= 'z') ch -= 32;
                msg_post(target_hwnd, WM_CHAR, (u32)(u8)ch, lparam);
            } else if (k == SDLK_BACKSPACE) {
                msg_post(target_hwnd, WM_CHAR, 8, lparam);
            } else if (k == SDLK_TAB) {
                msg_post(target_hwnd, WM_CHAR, 9, lparam);
            } else if (k == SDLK_RETURN) {
                msg_post(target_hwnd, WM_CHAR, 13, lparam);
            }
            break;
        }
        case SDL_KEYUP: {
            u32 vk = sdl_to_vk(e->key.keysym.sym);
            u32 dik = sdl_to_dik(e->key.keysym.scancode);
            if (vk < 256) vk_down[vk] = 0;
            if (dik < 256) dik_down[dik] = 0;
            if (!vk_down[0xA0] && !vk_down[0xA1]) vk_down[0x10] = 0;
            if (!vk_down[0xA2] && !vk_down[0xA3]) vk_down[0x11] = 0;
            if (!vk_down[0xA4] && !vk_down[0xA5]) vk_down[0x12] = 0;

            int alt = (e->key.keysym.mod & KMOD_ALT) != 0;
            u32 msg = alt ? WM_SYSKEYUP : WM_KEYUP;
            u32 lparam = 1 | ((dik & 0xFF) << 16) | (alt ? (1 << 29) : 0) | (1u << 30) | (1u << 31);
            msg_post(target_hwnd, msg, vk, lparam);
            break;
        }
        case SDL_MOUSEMOTION: {
            mouse_x = e->motion.x;
            mouse_y = e->motion.y;
            u32 wparam = 0;
            if (mouse_buttons & 1) wparam |= 0x0001; /* MK_LBUTTON */
            if (mouse_buttons & 2) wparam |= 0x0002; /* MK_RBUTTON */
            if (mouse_buttons & 4) wparam |= 0x0010; /* MK_MBUTTON */
            u32 lparam = ((u32)(u16)mouse_x) | (((u32)(u16)mouse_y) << 16);
            msg_post(target_hwnd, WM_MOUSEMOVE, wparam, lparam);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: {
            mouse_x = e->button.x;
            mouse_y = e->button.y;
            u32 lparam = ((u32)(u16)mouse_x) | (((u32)(u16)mouse_y) << 16);
            u32 msg = 0;
            if (e->button.button == SDL_BUTTON_LEFT) {
                mouse_buttons |= 1;
                msg = (e->button.clicks > 1) ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN;
            } else if (e->button.button == SDL_BUTTON_RIGHT) {
                mouse_buttons |= 2;
                msg = (e->button.clicks > 1) ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN;
            } else if (e->button.button == SDL_BUTTON_MIDDLE) {
                mouse_buttons |= 4;
                msg = (e->button.clicks > 1) ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN;
            }
            if (msg) {
                u32 wparam = 0;
                if (mouse_buttons & 1) wparam |= 0x0001;
                if (mouse_buttons & 2) wparam |= 0x0002;
                if (mouse_buttons & 4) wparam |= 0x0010;
                msg_post(target_hwnd, msg, wparam, lparam);
            }
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            mouse_x = e->button.x;
            mouse_y = e->button.y;
            u32 lparam = ((u32)(u16)mouse_x) | (((u32)(u16)mouse_y) << 16);
            u32 msg = 0;
            if (e->button.button == SDL_BUTTON_LEFT) {
                mouse_buttons &= ~1;
                msg = WM_LBUTTONUP;
            } else if (e->button.button == SDL_BUTTON_RIGHT) {
                mouse_buttons &= ~2;
                msg = WM_RBUTTONUP;
            } else if (e->button.button == SDL_BUTTON_MIDDLE) {
                mouse_buttons &= ~4;
                msg = WM_MBUTTONUP;
            }
            if (msg) {
                u32 wparam = 0;
                if (mouse_buttons & 1) wparam |= 0x0001;
                if (mouse_buttons & 2) wparam |= 0x0002;
                if (mouse_buttons & 4) wparam |= 0x0010;
                msg_post(target_hwnd, msg, wparam, lparam);
            }
            break;
        }
        case SDL_QUIT:
            msg_post(target_hwnd, WM_CLOSE, 0, 0);
            break;
        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_CLOSE) {
                msg_post(target_hwnd, WM_CLOSE, 0, 0);
            } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                msg_post(target_hwnd, WM_SETFOCUS, 0, 0);
            } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                msg_post(target_hwnd, WM_KILLFOCUS, 0, 0);
            }
            break;
    }
}

/* FCOP_INPUT="5000:Down 6000:Return*500 9000:click@320,240": scripted input for headless tests.
   Each item is milliseconds since the first pump, then an SDL key name (held for 100 ms, or
   for *N ms; "_" stands for a space, as in Keypad_8) or a left click at game coordinates. */
static void scripted_input(void)
{
    static const char *script;
    static int inited;
    static Uint32 t0;
    static struct { Uint32 t; int down, x, y; SDL_Keycode k; } ev[256];  /* down: 0 up, 1 down, 2 mouse move */
    static int n, next;
    Uint32 now;
    if (!inited) {
        char buf[64];
        const char *p;
        inited = 1;
        script = getenv("FCOP_INPUT");
        t0 = SDL_GetTicks();
        for (p = script; p && *p && n < 253; ) {
            unsigned t;
            int x, y, len = 0;
            while (*p == ' ') p++;
            if (!*p || sscanf(p, "%u:%63[^ ]%n", &t, buf, &len) != 2) break;
            p += len;
            if (sscanf(buf, "click@%d,%d", &x, &y) == 2) {
                /* move there first (down = 2), as a real mouse does, then press and release */
                ev[n].t = t >= 200 ? t - 200 : 0; ev[n].down = 2; ev[n].x = x; ev[n].y = y; ev[n].k = 0; n++;
                ev[n] = ev[n - 1]; ev[n].t = t; ev[n].down = 1; n++;
                ev[n] = ev[n - 1]; ev[n].t = t + 100; ev[n].down = 0; n++;
            } else {
                char *star = strchr(buf, '*');
                unsigned hold = star ? (unsigned)atoi(star + 1) : 100;
                SDL_Keycode k;
                if (star) *star = 0;
                for (char *q = buf; *q; q++) if (*q == '_') *q = ' ';  /* "Keypad_8" */
                k = SDL_GetKeyFromName(buf);
                if (k == SDLK_UNKNOWN) { plat_log("FCOP_INPUT: unknown key '%s'", buf); continue; }
                ev[n].t = t; ev[n].down = 1; ev[n].k = k; n++;
                ev[n] = ev[n - 1]; ev[n].t = t + hold; ev[n].down = 0; n++;
            }
        }
        /* sort by time (insertion sort, the list is short) */
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && ev[j].t < ev[j - 1].t; j--) {
                __typeof__(ev[0]) tmp = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = tmp;
            }
    }
    now = SDL_GetTicks() - t0;
    while (next < n && ev[next].t <= now) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        if (ev[next].k) {
            e.type = ev[next].down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.state = ev[next].down ? SDL_PRESSED : SDL_RELEASED;
            e.key.keysym.sym = ev[next].k;
            e.key.keysym.scancode = SDL_GetScancodeFromKey(ev[next].k);
        } else if (ev[next].down == 2) {
            e.type = SDL_MOUSEMOTION;
            e.motion.x = ev[next].x;
            e.motion.y = ev[next].y;
        } else {
            e.type = ev[next].down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.state = ev[next].down ? SDL_PRESSED : SDL_RELEASED;
            e.button.clicks = 1;
            e.button.x = ev[next].x;
            e.button.y = ev[next].y;
        }
        plat_log("FCOP_INPUT: %s %s at %u ms", ev[next].down == 2 ? "move" : ev[next].down ? "press" : "release",
                 ev[next].k ? SDL_GetKeyName(ev[next].k) : "click", now);
        handle_sdl_event(&e);
        next++;
    }
}

void plat_pump(void)
{
    if (!sdl_inited) return;
    SDL_Event e;
    scripted_input();
    while (SDL_PollEvent(&e)) {
        handle_sdl_event(&e);
    }
    check_timers();
}

/* Present frame: upload to SDL texture and present scaled */
/* FCOP_SHOT=path: write each presented 16-bit frame there as a PPM (for headless checks) */
static void shot(const void *pixels, int w, int h, int pitch, int bits, u32 gmask)
{
    static int n;
    const char *path = getenv("FCOP_SHOT");
    FILE *f;
    int x, y;
    n++;
    char name[512];
    if (!path || bits != 16) return;
    snprintf(name, sizeof name, path, n);  /* path may contain %d for the frame number */
    if (!(f = fopen(name, "wb"))) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            u16 p = ((const u16 *)((const u8 *)pixels + y * pitch))[x];
            u8 rgb[3];
            if (gmask == 0x7e0) { rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3; }
            else { rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3; }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    if (getenv("FCOP_SHOT_LOG")) plat_log("frame %d presented (%dx%d)", n, w, h);
}

void win_present(const void *pixels, int w, int h, int pitch_bytes, int bits, u32 rmask, u32 gmask, u32 bmask)
{
    shot(pixels, w, h, pitch_bytes, bits, gmask);
    if (!main_win) return;
    struct WinInternal *win_int = (struct WinInternal *)main_win;
    ensure_window_sdl(win_int);
    if (!win_int->win.sdl) return;

    if (!win_int->win.visible) {
        SDL_ShowWindow(win_int->win.sdl);
        win_int->win.visible = 1;
    }

    /* Determine SDL pixel format */
    Uint32 format = SDL_PIXELFORMAT_RGB565;
    if (bits == 16) {
        if (gmask == 0x07e0) {
            format = (rmask == 0xf800) ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_BGR565;
        } else if (gmask == 0x03e0) {
            format = (rmask == 0x7c00) ? SDL_PIXELFORMAT_RGB555 : SDL_PIXELFORMAT_BGR555;
        }
    } else if (bits == 32) {
        if (rmask == 0x00ff0000) format = SDL_PIXELFORMAT_ARGB8888;
        else if (rmask == 0x000000ff) format = SDL_PIXELFORMAT_ABGR8888;
        else format = SDL_PIXELFORMAT_RGBA8888;
    } else if (bits == 24) {
        format = SDL_PIXELFORMAT_RGB24;
    }

    if (!win_int->renderer) {
        win_int->renderer = SDL_CreateRenderer(win_int->win.sdl, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!win_int->renderer) {
            win_int->renderer = SDL_CreateRenderer(win_int->win.sdl, -1, 0);
        }
    }
    if (!win_int->renderer) return;

    if (!win_int->texture || win_int->tex_w != w || win_int->tex_h != h || win_int->tex_format != format) {
        if (win_int->texture) SDL_DestroyTexture(win_int->texture);
        win_int->texture = SDL_CreateTexture(win_int->renderer, format, SDL_TEXTUREACCESS_STREAMING, w, h);
        win_int->tex_w = w;
        win_int->tex_h = h;
        win_int->tex_format = format;
    }
    if (!win_int->texture) return;

    SDL_UpdateTexture(win_int->texture, NULL, pixels, pitch_bytes);
    SDL_RenderClear(win_int->renderer);
    SDL_RenderCopy(win_int->renderer, win_int->texture, NULL, NULL);
    SDL_RenderPresent(win_int->renderer);
}

/* ---- USER32 Import Implementations ---------------------------------------------------- */

/* ATOM RegisterClassA(const WNDCLASSA *lpWndClass) */
static u32 imp_RegisterClassA(Cpu *c)
{
    u32 p = ARG(0);
    if (!p) { last_error = 87; return 0; }
    if (win_class_count >= MAX_CLASSES) {
        plat_log("warn: out of class slots");
        return 0;
    }

    struct WinClass *cls = &win_classes[win_class_count];
    cls->style = rd32(p + 0);
    cls->wndproc = rd32(p + 4);
    cls->cbClsExtra = (int)rd32(p + 8);
    cls->cbWndExtra = (int)rd32(p + 12);
    cls->hInstance = rd32(p + 16);
    cls->hIcon = rd32(p + 20);
    cls->hCursor = rd32(p + 24);
    cls->hbrBackground = rd32(p + 28);
    cls->menuName = rd32(p + 32);

    u32 class_name_ptr = rd32(p + 36);
    if (class_name_ptr > 0xFFFF) {
        strncpy(cls->name, GSTR(class_name_ptr), sizeof(cls->name) - 1);
    } else {
        snprintf(cls->name, sizeof(cls->name), "#%u", class_name_ptr);
    }

    win_class_count++;
    return 0xC000 + win_class_count; /* Atom */
}

/* BOOL UnregisterClassA(LPCSTR lpClassName, HINSTANCE hInstance) */
static u32 imp_UnregisterClassA(Cpu *c)
{
    u32 name_ptr = ARG(0);
    const char *name = (name_ptr > 0xFFFF) ? GSTR(name_ptr) : "";
    for (int i = 0; i < win_class_count; i++) {
        if (!strcasecmp(win_classes[i].name, name)) {
            win_classes[i] = win_classes[--win_class_count];
            return 1;
        }
    }
    return 1;
}

/* HWND CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
                       DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                       HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) */
static u32 imp_CreateWindowExA(Cpu *c)
{
    u32 dwExStyle = ARG(0);
    u32 lpClassName = ARG(1);
    u32 lpWindowName = ARG(2);
    u32 dwStyle = ARG(3);
    int x = (int)ARG(4);
    int y = (int)ARG(5);
    int w = (int)ARG(6);
    int h = (int)ARG(7);
    u32 hWndParent = ARG(8);
    u32 hMenu = ARG(9);
    u32 hInstance = ARG(10);
    u32 lpParam = ARG(11);

    if (x == (int)0x80000000) x = 0;
    if (y == (int)0x80000000) y = 0;
    if (w == (int)0x80000000 || w <= 0) w = 640;
    if (h == (int)0x80000000 || h <= 0) h = 480;

    const char *cls_name = (lpClassName > 0xFFFF) ? GSTR(lpClassName) : "";
    u32 wndproc = 0;
    for (int i = 0; i < win_class_count; i++) {
        if (!strcasecmp(win_classes[i].name, cls_name)) {
            wndproc = win_classes[i].wndproc;
            break;
        }
    }

    struct WinInternal *win_int = calloc(1, sizeof(struct WinInternal));
    if (!win_int) return 0;

    win_int->win.hwnd = handle_new(H_OTHER, win_int);
    win_int->win.wndproc = wndproc;
    win_int->win.style = dwStyle;
    win_int->win.exstyle = dwExStyle;
    win_int->win.parent = hWndParent;
    win_int->win.menu = hMenu;
    win_int->win.x = x;
    win_int->win.y = y;
    win_int->win.w = w;
    win_int->win.h = h;
    win_int->win.visible = (dwStyle & 0x10000000) ? 1 : 0; /* WS_VISIBLE */
    win_int->instance = hInstance;
    strncpy(win_int->class_name, cls_name, sizeof(win_int->class_name) - 1);
    if (lpWindowName) {
        strncpy(win_int->title, GSTR(lpWindowName), sizeof(win_int->title) - 1);
    }

    pthread_mutex_lock(&win_lock);
    if (win_count < MAX_WINDOWS) {
        win_table[win_count++] = win_int;
    }
    pthread_mutex_unlock(&win_lock);

    if (!main_win) {
        main_win = &win_int->win;
        ensure_window_sdl(win_int);
    }

    /* Guest CREATESTRUCT: 48 bytes */
    u32 cs = galloc(48);
    if (cs) {
        wr32(cs + 0x00, lpParam);
        wr32(cs + 0x04, hInstance);
        wr32(cs + 0x08, hMenu);
        wr32(cs + 0x0c, hWndParent);
        wr32(cs + 0x10, (u32)h);
        wr32(cs + 0x14, (u32)w);
        wr32(cs + 0x18, (u32)y);
        wr32(cs + 0x1c, (u32)x);
        wr32(cs + 0x20, dwStyle);
        wr32(cs + 0x24, lpWindowName);
        wr32(cs + 0x28, lpClassName);
        wr32(cs + 0x2c, dwExStyle);

        u32 ret = send_message_direct(win_int->win.hwnd, WM_CREATE, 0, cs);
        gfree(cs);
        if ((int)ret == -1) {
            plat_log("WM_CREATE returned -1: window creation rejected");
            return 0;
        }
    }

    return win_int->win.hwnd;
}

/* BOOL DestroyWindow(HWND hWnd) */
static u32 imp_DestroyWindow(Cpu *c)
{
    u32 hwnd = ARG(0);
    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    send_message_direct(hwnd, WM_DESTROY, 0, 0);
    send_message_direct(hwnd, WM_NCDESTROY, 0, 0);

    if (w->win.sdl) {
        if (w->renderer) { SDL_DestroyRenderer(w->renderer); w->renderer = NULL; }
        if (w->texture) { SDL_DestroyTexture(w->texture); w->texture = NULL; }
        SDL_DestroyWindow(w->win.sdl);
        w->win.sdl = NULL;
    }

    pthread_mutex_lock(&win_lock);
    for (int i = 0; i < win_count; i++) {
        if (win_table[i] == w) {
            win_table[i] = win_table[--win_count];
            break;
        }
    }
    pthread_mutex_unlock(&win_lock);

    if (main_win == &w->win) {
        main_win = (win_count > 0) ? &win_table[0]->win : NULL;
    }

    handle_free(hwnd);
    free(w);
    return 1;
}

/* HWND FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName) */
static u32 imp_FindWindowA(Cpu *c)
{
    u32 cls_ptr = ARG(0);
    u32 title_ptr = ARG(1);
    const char *cls = (cls_ptr > 0xFFFF) ? GSTR(cls_ptr) : NULL;
    const char *title = title_ptr ? GSTR(title_ptr) : NULL;

    pthread_mutex_lock(&win_lock);
    for (int i = 0; i < win_count; i++) {
        struct WinInternal *w = win_table[i];
        if (!w) continue;
        int match_cls = !cls || !strcasecmp(w->class_name, cls);
        int match_title = !title || !strcasecmp(w->title, title);
        if (match_cls && match_title) {
            u32 res = w->win.hwnd;
            pthread_mutex_unlock(&win_lock);
            return res;
        }
    }
    pthread_mutex_unlock(&win_lock);
    return 0;
}

/* LRESULT DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) */
static u32 imp_DefWindowProcA(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 msg = ARG(1);
    u32 wparam = ARG(2);
    u32 lparam = ARG(3);
    struct WinInternal *w = win_get_internal(hwnd);

    switch (msg) {
        case WM_CLOSE: {
            u32 args[1] = { hwnd };
            c->r[4] -= 8;
            wr32(c->r[4] + 4, hwnd);
            imp_DestroyWindow(c);
            c->r[4] += 8;
            return 0;
        }
        case WM_GETTEXTLENGTH:
            return w ? (u32)strlen(w->title) : 0;
        case WM_GETTEXT: {
            if (!w || !lparam) return 0;
            u32 max_chars = wparam;
            if (!max_chars) return 0;
            gstrcpy(lparam, w->title, max_chars);
            return (u32)strlen(w->title);
        }
        case WM_SETTEXT: {
            if (w && lparam) {
                strncpy(w->title, GSTR(lparam), sizeof(w->title) - 1);
                if (w->win.sdl) SDL_SetWindowTitle(w->win.sdl, w->title);
            }
            return 1;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            return 1;
        case WM_NCHITTEST:
            return 1; /* HTCLIENT */
        default:
            return 0;
    }
}

/* BOOL ShowWindow(HWND hWnd, int nCmdShow) */
static u32 imp_ShowWindow(Cpu *c)
{
    u32 hwnd = ARG(0);
    int cmd = (int)ARG(1);
    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    int was_visible = w->win.visible;
    ensure_window_sdl(w);

    if (cmd == 0 /* SW_HIDE */) {
        if (w->win.sdl) SDL_HideWindow(w->win.sdl);
        w->win.visible = 0;
    } else {
        if (w->win.sdl) SDL_ShowWindow(w->win.sdl);
        w->win.visible = 1;
        if (!w->activated) {
            w->activated = 1;
            send_message_direct(hwnd, WM_ACTIVATEAPP, 1, 0);
            send_message_direct(hwnd, WM_ACTIVATE, 1 /* WA_ACTIVE */, 0);
        }
    }
    return was_visible ? 1 : 0;
}

/* BOOL UpdateWindow(HWND hWnd) */
static u32 imp_UpdateWindow(Cpu *c)
{
    u32 hwnd = ARG(0);
    send_message_direct(hwnd, WM_PAINT, 0, 0);
    return 1;
}

/* BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) */
static u32 imp_SetWindowPos(Cpu *c)
{
    u32 hwnd = ARG(0);
    int x = (int)ARG(2);
    int y = (int)ARG(3);
    int cx = (int)ARG(4);
    int cy = (int)ARG(5);
    u32 flags = ARG(6);

    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    if (!(flags & SWP_NOMOVE)) {
        w->win.x = x;
        w->win.y = y;
        if (w->win.sdl) SDL_SetWindowPosition(w->win.sdl, x, y);
    }
    if (!(flags & SWP_NOSIZE)) {
        w->win.w = cx;
        w->win.h = cy;
        if (w->win.sdl) SDL_SetWindowSize(w->win.sdl, cx, cy);
    }
    if (flags & SWP_SHOWWINDOW) {
        ensure_window_sdl(w);
        if (w->win.sdl) SDL_ShowWindow(w->win.sdl);
        w->win.visible = 1;
    }
    if (flags & SWP_HIDEWINDOW) {
        if (w->win.sdl) SDL_HideWindow(w->win.sdl);
        w->win.visible = 0;
    }
    return 1;
}

/* BOOL GetWindowRect(HWND hWnd, LPRECT lpRect) */
static u32 imp_GetWindowRect(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 rect = ARG(1);
    struct Win *w = win_get(hwnd);
    if (!w || !rect) return 0;

    wr32(rect + 0, (u32)w->x);
    wr32(rect + 4, (u32)w->y);
    wr32(rect + 8, (u32)(w->x + w->w));
    wr32(rect + 12, (u32)(w->y + w->h));
    return 1;
}

/* BOOL GetClientRect(HWND hWnd, LPRECT lpRect) */
static u32 imp_GetClientRect(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 rect = ARG(1);
    struct Win *w = win_get(hwnd);
    if (!w || !rect) return 0;

    wr32(rect + 0, 0);
    wr32(rect + 4, 0);
    wr32(rect + 8, (u32)w->w);
    wr32(rect + 12, (u32)w->h);
    return 1;
}

/* BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) */
static u32 imp_AdjustWindowRect(Cpu *c)
{
    /* Client area = whole window */
    return 1;
}

/* BOOL AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle) */
static u32 imp_AdjustWindowRectEx(Cpu *c)
{
    /* Client area = whole window */
    return 1;
}

/* BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint) */
static u32 imp_ClientToScreen(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 pt = ARG(1);
    struct Win *w = win_get(hwnd);
    if (!w || !pt) return 0;

    /* the window is the whole screen here (the primary surface is the window's content),
       so client and screen coordinates are the same */
    (void)w;
    return 1;
}

/* LONG GetWindowLongA(HWND hWnd, int nIndex) */
static u32 imp_GetWindowLongA(Cpu *c)
{
    u32 hwnd = ARG(0);
    int idx = (int)ARG(1);
    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    switch (idx) {
        case GWL_WNDPROC:   return w->win.wndproc;
        case GWL_HINSTANCE: return w->instance;
        case GWL_HWNDPARENT:return w->win.parent;
        case GWL_STYLE:     return w->win.style;
        case GWL_EXSTYLE:   return w->win.exstyle;
        case GWL_USERDATA:  return w->win.userdata;
        case GWL_ID:        return w->win.menu;
        default:
            if (idx >= 0 && idx + 4 <= (int)sizeof(w->extra)) {
                return w->extra[idx / 4];
            }
            return 0;
    }
}

/* LONG SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) */
static u32 imp_SetWindowLongA(Cpu *c)
{
    u32 hwnd = ARG(0);
    int idx = (int)ARG(1);
    u32 val = ARG(2);
    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    u32 old = 0;
    switch (idx) {
        case GWL_WNDPROC:
            old = w->win.wndproc;
            w->win.wndproc = val;
            break;
        case GWL_HINSTANCE:
            old = w->instance;
            w->instance = val;
            break;
        case GWL_HWNDPARENT:
            old = w->win.parent;
            w->win.parent = val;
            break;
        case GWL_STYLE:
            old = w->win.style;
            w->win.style = val;
            break;
        case GWL_EXSTYLE:
            old = w->win.exstyle;
            w->win.exstyle = val;
            break;
        case GWL_USERDATA:
            old = w->win.userdata;
            w->win.userdata = val;
            break;
        case GWL_ID:
            old = w->win.menu;
            w->win.menu = val;
            break;
        default:
            if (idx >= 0 && idx + 4 <= (int)sizeof(w->extra)) {
                old = w->extra[idx / 4];
                w->extra[idx / 4] = val;
            }
            break;
    }
    return old;
}

/* BOOL SetWindowTextA(HWND hWnd, LPCSTR lpString) */
static u32 imp_SetWindowTextA(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 str_ptr = ARG(1);
    struct WinInternal *w = win_get_internal(hwnd);
    if (!w) return 0;

    if (str_ptr) {
        strncpy(w->title, GSTR(str_ptr), sizeof(w->title) - 1);
        if (w->win.sdl) SDL_SetWindowTitle(w->win.sdl, w->title);
    }
    return 1;
}

/* BOOL SetMenu(HWND hWnd, HMENU hMenu) */
static u32 imp_SetMenu(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 menu = ARG(1);
    struct Win *w = win_get(hwnd);
    if (!w) return 0;
    w->menu = menu;
    return 1;
}

/* HMENU GetMenu(HWND hWnd) */
static u32 imp_GetMenu(Cpu *c)
{
    u32 hwnd = ARG(0);
    struct Win *w = win_get(hwnd);
    return w ? w->menu : 0;
}

/* HWND SetActiveWindow(HWND hWnd) */
static u32 imp_SetActiveWindow(Cpu *c)
{
    u32 hwnd = ARG(0);
    struct Win *w = win_get(hwnd);
    if (w && w->sdl) SDL_RaiseWindow(w->sdl);
    return hwnd;
}

/* BOOL SetForegroundWindow(HWND hWnd) */
static u32 imp_SetForegroundWindow(Cpu *c)
{
    u32 hwnd = ARG(0);
    struct Win *w = win_get(hwnd);
    if (w && w->sdl) SDL_RaiseWindow(w->sdl);
    return 1;
}

/* HWND SetFocus(HWND hWnd) */
static u32 imp_SetFocus(Cpu *c)
{
    u32 hwnd = ARG(0);
    struct Win *w = win_get(hwnd);
    if (w && w->sdl) SDL_RaiseWindow(w->sdl);
    return hwnd;
}

/* BOOL EnableWindow(HWND hWnd, BOOL bEnable) */
static u32 imp_EnableWindow(Cpu *c)
{
    return 0; /* was previously enabled */
}

/* BOOL IsIconic(HWND hWnd) */
static u32 imp_IsIconic(Cpu *c)
{
    return 0; /* not minimized */
}

/* BOOL PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) */
static u32 imp_PeekMessageA(Cpu *c)
{
    u32 pmsg = ARG(0);
    u32 hwnd = ARG(1);
    u32 min = ARG(2);
    u32 max = ARG(3);
    u32 remove = ARG(4) & 1;

    plat_pump();

    struct Msg m;
    if (msg_peek(&m, hwnd, min, max, (int)remove)) {
        if (pmsg) {
            wr32(pmsg + 0, m.hwnd);
            wr32(pmsg + 4, m.message);
            wr32(pmsg + 8, m.wparam);
            wr32(pmsg + 12, m.lparam);
            wr32(pmsg + 16, m.time);
            wr32(pmsg + 20, m.pt_x);
            wr32(pmsg + 24, m.pt_y);
        }
        return 1;
    }
    return 0;
}

/* BOOL GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) */
static u32 imp_GetMessageA(Cpu *c)
{
    u32 pmsg = ARG(0);
    u32 hwnd = ARG(1);
    u32 min = ARG(2);
    u32 max = ARG(3);

    struct Msg m;
    while (1) {
        plat_pump();
        if (msg_peek(&m, hwnd, min, max, 1)) {
            if (pmsg) {
                wr32(pmsg + 0, m.hwnd);
                wr32(pmsg + 4, m.message);
                wr32(pmsg + 8, m.wparam);
                wr32(pmsg + 12, m.lparam);
                wr32(pmsg + 16, m.time);
                wr32(pmsg + 20, m.pt_x);
                wr32(pmsg + 24, m.pt_y);
            }
            return (m.message == WM_QUIT) ? 0 : 1;
        }

        if (sdl_inited) {
            SDL_Event ev;
            if (SDL_WaitEventTimeout(&ev, 10)) {
                handle_sdl_event(&ev);
            }
        } else {
            SDL_Delay(10);
        }
    }
}

/* BOOL TranslateMessage(const MSG *lpMsg) */
static u32 imp_TranslateMessage(Cpu *c)
{
    u32 pmsg = ARG(0);
    if (!pmsg) return 0;
    u32 msg = rd32(pmsg + 4);
    return (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) ? 1 : 0;
}

/* LRESULT DispatchMessageA(const MSG *lpMsg) */
static u32 imp_DispatchMessageA(Cpu *c)
{
    u32 pmsg = ARG(0);
    if (!pmsg) return 0;

    u32 hwnd = rd32(pmsg + 0);
    u32 msg = rd32(pmsg + 4);
    u32 wparam = rd32(pmsg + 8);
    u32 lparam = rd32(pmsg + 12);
    u32 time = rd32(pmsg + 16);

    if (msg == WM_TIMER && lparam != 0) {
        u32 args[4] = { hwnd, msg, wparam, time };
        return guest_call(c, lparam, 4, args);
    }

    struct WinInternal *w = win_get_internal(hwnd);
    if (w && w->win.wndproc) {
        u32 args[4] = { hwnd, msg, wparam, lparam };
        return guest_call(c, w->win.wndproc, 4, args);
    }
    return 0;
}

/* BOOL PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) */
static u32 imp_PostMessageA(Cpu *c)
{
    msg_post(ARG(0), ARG(1), ARG(2), ARG(3));
    return 1;
}

/* LRESULT SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) */
static u32 imp_SendMessageA(Cpu *c)
{
    return send_message_direct(ARG(0), ARG(1), ARG(2), ARG(3));
}

/* BOOL SendNotifyMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) */
static u32 imp_SendNotifyMessageA(Cpu *c)
{
    send_message_direct(ARG(0), ARG(1), ARG(2), ARG(3));
    return 1;
}

/* VOID PostQuitMessage(int nExitCode) */
static u32 imp_PostQuitMessage(Cpu *c)
{
    msg_post(0, WM_QUIT, ARG(0), 0);
    return 0;
}

/* UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc) */
static u32 imp_SetTimer(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 id = ARG(1);
    u32 elapse = ARG(2);
    u32 proc = ARG(3);

    if (elapse < 10) elapse = 10;
    if (id == 0) id = next_timer_id++;

    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (win_timers[i].active && win_timers[i].hwnd == hwnd && win_timers[i].id == id) {
            slot = i;
            break;
        }
        if (!win_timers[i].active && slot == -1) {
            slot = i;
        }
    }
    if (slot != -1) {
        win_timers[slot].active = 1;
        win_timers[slot].hwnd = hwnd;
        win_timers[slot].id = id;
        win_timers[slot].interval = elapse;
        win_timers[slot].last_tick = SDL_GetTicks();
        win_timers[slot].proc = proc;
        return id;
    }
    plat_log("warn: out of timer slots");
    return 0;
}

/* BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent) */
static u32 imp_KillTimer(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 id = ARG(1);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (win_timers[i].active && win_timers[i].hwnd == hwnd && win_timers[i].id == id) {
            win_timers[i].active = 0;
            return 1;
        }
    }
    return 0;
}

/* BOOL ValidateRect(HWND hWnd, const RECT *lpRect) */
static u32 imp_ValidateRect(Cpu *c)
{
    return 1;
}

/* BOOL InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase) */
static u32 imp_InvalidateRect(Cpu *c)
{
    return 1;
}

/* HDC GetDC(HWND hWnd) */
static u32 imp_GetDC(Cpu *c)
{
    return 0x00DC0001u; /* Dummy DC handle */
}

/* int ReleaseDC(HWND hWnd, HDC hDC) */
static u32 imp_ReleaseDC(Cpu *c)
{
    return 1;
}

/* HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint) */
static u32 imp_BeginPaint(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 ps = ARG(1);
    struct Win *w = win_get(hwnd);

    if (ps) {
        memset(G(ps), 0, 64);
        wr32(ps + 0, 0x00DC0001u); /* hdc */
        if (w) {
            wr32(ps + 16, (u32)w->w);
            wr32(ps + 20, (u32)w->h);
        }
    }
    return 0x00DC0001u;
}

/* BOOL EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint) */
static u32 imp_EndPaint(Cpu *c)
{
    return 1;
}

/* HICON LoadIconA(HINSTANCE hInstance, LPCSTR lpIconName) */
static u32 imp_LoadIconA(Cpu *c)
{
    return 0x001C0001u; /* Dummy icon handle */
}

/* HCURSOR LoadCursorA(HINSTANCE hInstance, LPCSTR lpCursorName) */
static u32 imp_LoadCursorA(Cpu *c)
{
    return 0x002C0001u; /* Dummy cursor handle */
}

/* BOOL SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) */
static u32 imp_SystemParametersInfoA(Cpu *c)
{
    u32 action = ARG(0), pv = ARG(2);
    if (action == 0x30 && pv) {  /* SPI_GETWORKAREA: the whole 640x480 "desktop" */
        wr32(pv, 0); wr32(pv + 4, 0); wr32(pv + 8, 640); wr32(pv + 12, 480);
    } else if (pv && action != 0x61 /* SPI_SCREENSAVERRUNNING */) {
        plat_log("warn: SystemParametersInfoA action 0x%x", action);
    }
    return 1;
}

/* int GetSystemMetrics(int nIndex) */
static u32 imp_GetSystemMetrics(Cpu *c)
{
    int idx = (int)ARG(0);
    switch (idx) {
        case SM_CXSCREEN:   return 640;
        case SM_CYSCREEN:   return 480;
        case SM_CXVSCROLL:  return 16;
        case SM_CYHSCROLL:  return 16;
        case SM_CYCAPTION:  return 19;
        case SM_CXBORDER:   return 1;
        case SM_CYBORDER:   return 1;
        case SM_CXFRAME:    return 4;
        case SM_CYFRAME:    return 4;
        case SM_CXEDGE:     return 2;
        case SM_CYEDGE:     return 2;
        default: return 0;
    }
}

/* HHOOK SetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hMod, DWORD dwThreadId) */
static u32 imp_SetWindowsHookExA(Cpu *c)
{
    static u32 next_hook = 0x003C0001u;
    return next_hook++;
}

/* BOOL UnhookWindowsHookEx(HHOOK hhk) */
static u32 imp_UnhookWindowsHookEx(Cpu *c)
{
    return 1;
}

/* LRESULT CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam) */
static u32 imp_CallNextHookEx(Cpu *c)
{
    return 0;
}

/* Table of USER32 window & message imports */
const struct ImpDef imp_u32_win[] = {
    IMPORT(CreateWindowExA, 12),
    IMPORT(DestroyWindow, 1),
    IMPORT(RegisterClassA, 1),
    IMPORT(UnregisterClassA, 2),
    IMPORT(FindWindowA, 2),
    IMPORT(DefWindowProcA, 4),
    IMPORT(ShowWindow, 2),
    IMPORT(UpdateWindow, 1),
    IMPORT(SetWindowPos, 7),
    IMPORT(GetWindowRect, 2),
    IMPORT(GetClientRect, 2),
    IMPORT(AdjustWindowRect, 3),
    IMPORT(AdjustWindowRectEx, 4),
    IMPORT(ClientToScreen, 2),
    IMPORT(GetWindowLongA, 2),
    IMPORT(SetWindowLongA, 3),
    IMPORT(SetWindowTextA, 2),
    IMPORT(SetMenu, 2),
    IMPORT(GetMenu, 1),
    IMPORT(SetActiveWindow, 1),
    IMPORT(SetForegroundWindow, 1),
    IMPORT(SetFocus, 1),
    IMPORT(EnableWindow, 2),
    IMPORT(IsIconic, 1),
    IMPORT(PeekMessageA, 5),
    IMPORT(GetMessageA, 4),
    IMPORT(TranslateMessage, 1),
    IMPORT(DispatchMessageA, 1),
    IMPORT(PostMessageA, 4),
    IMPORT(SendMessageA, 4),
    IMPORT(SendNotifyMessageA, 4),
    IMPORT(PostQuitMessage, 1),
    IMPORT(SetTimer, 4),
    IMPORT(KillTimer, 2),
    IMPORT(ValidateRect, 2),
    IMPORT(InvalidateRect, 3),
    IMPORT(GetDC, 1),
    IMPORT(ReleaseDC, 2),
    IMPORT(BeginPaint, 2),
    IMPORT(EndPaint, 2),
    IMPORT(LoadIconA, 2),
    IMPORT(LoadCursorA, 2),
    IMPORT(SystemParametersInfoA, 4),
    IMPORT(GetSystemMetrics, 1),
    IMPORT(SetWindowsHookExA, 4),
    IMPORT(UnhookWindowsHookEx, 1),
    IMPORT(CallNextHookEx, 4),
};
IMPORT_TABLE(u32_win);
