/* USER32.dll: u32_misc
 * Dialogs, input, cursor, formatted print, rectangles.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "host.h"
#include "win.h"

/* Weak fallbacks until u32_win.c is implemented by the other worker */
__attribute__((weak)) volatile u8 vk_down[256];
__attribute__((weak)) volatile u8 dik_down[256];
__attribute__((weak)) volatile int mouse_x = 0, mouse_y = 0, mouse_buttons = 0;
__attribute__((weak)) struct Win *main_win = NULL;
__attribute__((weak)) void plat_pump(void) {}
__attribute__((weak)) void msg_post(u32 hwnd, u32 message, u32 wparam, u32 lparam)
{
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
}



/* ---- Keyboard, mouse and cursor ------------------------------------------------------ */

/* SHORT GetAsyncKeyState(int vKey) */
static u32 imp_GetAsyncKeyState(Cpu *c)
{
    plat_pump();
    u32 vk = ARG(0) & 0xff;
    int down = vk_down[vk];
    if (vk == 1 && (mouse_buttons & 1)) down = 1;
    if (vk == 2 && (mouse_buttons & 2)) down = 1;
    if (vk == 4 && (mouse_buttons & 4)) down = 1;
    if (vk == 0x10 /* VK_SHIFT */ && (vk_down[0xa0] || vk_down[0xa1])) down = 1;
    if (vk == 0x11 /* VK_CONTROL */ && (vk_down[0xa2] || vk_down[0xa3])) down = 1;
    if (vk == 0x12 /* VK_MENU */ && (vk_down[0xa4] || vk_down[0xa5])) down = 1;
    if (getenv("FCOP_TRACE_KEYS")) {  /* each key the game asks about, and every press it sees */
        static u8 seen[256];
        if (!seen[vk] || down) plat_log("GetAsyncKeyState(0x%02x) -> %s (from 0x%08x)", vk, down ? "down" : "up", rd32(c->r[4]));
        seen[vk] = 1;
    }
    return down ? 0x8000u : 0;
}

/* SHORT GetKeyState(int nVirtKey) */
static u32 imp_GetKeyState(Cpu *c)
{
    plat_pump();
    u32 vk = ARG(0) & 0xff;
    int down = vk_down[vk];
    if (vk == 1 && (mouse_buttons & 1)) down = 1;
    if (vk == 2 && (mouse_buttons & 2)) down = 1;
    if (vk == 4 && (mouse_buttons & 4)) down = 1;
    if (vk == 0x10 /* VK_SHIFT */ && (vk_down[0xa0] || vk_down[0xa1])) down = 1;
    if (vk == 0x11 /* VK_CONTROL */ && (vk_down[0xa2] || vk_down[0xa3])) down = 1;
    if (vk == 0x12 /* VK_MENU */ && (vk_down[0xa4] || vk_down[0xa5])) down = 1;
    return down ? 0x8000u : 0;
}

/* VOID keybd_event(BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo) */
static u32 imp_keybd_event(Cpu *c)
{
    u8 vk = (u8)ARG(0);
    u8 scan = (u8)ARG(1);
    u32 flags = ARG(2);
    u32 extra = ARG(3);
    (void)extra;

    int is_up = (flags & 0x0002 /* KEYEVENTF_KEYUP */) != 0;
    vk_down[vk] = is_up ? 0 : 1;

    if (main_win) {
        u32 msg = is_up ? 0x0101 /* WM_KEYUP */ : 0x0100 /* WM_KEYDOWN */;
        u32 lparam = ((u32)scan << 16) | (is_up ? 0xc0000001u : 0x00000001u);
        msg_post(main_win->hwnd, msg, vk, lparam);
    }
    return 0;
}

static int cursor_display_count = 0;

/* int ShowCursor(BOOL bShow) */
static u32 imp_ShowCursor(Cpu *c)
{
    int show = (int)ARG(0);
    if (show) {
        cursor_display_count++;
    } else {
        cursor_display_count--;
    }
    SDL_ShowCursor(cursor_display_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    return (u32)cursor_display_count;
}

static u32 current_cursor = 0;

/* HCURSOR SetCursor(HCURSOR hCursor) */
static u32 imp_SetCursor(Cpu *c)
{
    u32 prev = current_cursor;
    current_cursor = ARG(0);
    if (current_cursor == 0) {
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_ShowCursor(cursor_display_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    }
    return prev;
}

/* BOOL SetCursorPos(int X, int Y) */
static u32 imp_SetCursorPos(Cpu *c)
{
    int x = (int)ARG(0);
    int y = (int)ARG(1);
    mouse_x = x;
    mouse_y = y;
    if (main_win && main_win->sdl) {
        SDL_WarpMouseInWindow(main_win->sdl, x, y);
    }
    return 1;
}

/* ---- Rectangles ---------------------------------------------------------------------- */

/* BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom) */
static u32 imp_SetRect(Cpu *c)
{
    u32 rc = ARG(0);
    if (!rc) return 0;
    wr32(rc + 0, ARG(1));
    wr32(rc + 4, ARG(2));
    wr32(rc + 8, ARG(3));
    wr32(rc + 12, ARG(4));
    return 1;
}

/* BOOL InvertRect(HDC hDC, const RECT *lprc) */
static u32 imp_InvertRect(Cpu *c)
{
    u32 hdc = ARG(0);
    u32 rc = ARG(1);
    (void)hdc;
    if (!rc) return 0;
    return 1;
}

/* ---- Formatted output (wsprintfA, cdecl) ---------------------------------------------- */

/* int wsprintfA(LPSTR lpOut, LPCSTR lpFmt, ...) */
static u32 imp_wsprintfA(Cpu *c)
{
    u32 dst = ARG(0);
    u32 fmt_p = ARG(1);
    if (!dst) return 0;
    if (!fmt_p) { wr8(dst, 0); return 0; }

    const char *fmt = GSTR(fmt_p);
    char out_buf[4096];
    int out_pos = 0;
    int arg_idx = 2;

    while (*fmt && out_pos < (int)sizeof(out_buf) - 1) {
        if (*fmt != '%') {
            out_buf[out_pos++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */
        if (*fmt == '%') {
            out_buf[out_pos++] = '%';
            fmt++;
            continue;
        }

        /* Parse flags */
        char flags[16];
        int num_flags = 0;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '0' || *fmt == '#') {
            if (!strchr(flags, *fmt) && num_flags < (int)sizeof(flags) - 1) {
                flags[num_flags++] = *fmt;
                flags[num_flags] = '\0';
            }
            fmt++;
        }

        /* Parse width */
        int width = -1;
        if (*fmt == '*') {
            width = (int)ARG(arg_idx++);
            fmt++;
        } else if (*fmt >= '0' && *fmt <= '9') {
            width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = (int)ARG(arg_idx++);
                fmt++;
            } else if (*fmt >= '0' && *fmt <= '9') {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            } else {
                precision = 0;
            }
        }
        if (precision < 0) precision = -1;

        /* Parse length modifier */
        int is_long = 0;
        int is_short = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L' || *fmt == 'w' || *fmt == 'I') {
            if (*fmt == 'l') is_long = 1;
            else if (*fmt == 'h') is_short = 1;
            else if (*fmt == 'I') {
                if (fmt[1] == '3' && fmt[2] == '2') fmt += 2;
                else if (fmt[1] == '6' && fmt[2] == '4') fmt += 2;
            }
            fmt++;
        }

        char type = *fmt;
        if (type) fmt++;

        if (width < 0 && width != -1) {
            if (!strchr(flags, '-') && num_flags < (int)sizeof(flags) - 1) {
                flags[num_flags++] = '-';
                flags[num_flags] = '\0';
            }
            width = -width;
        }

        char spec[64];
        int spec_pos = 0;
        spec[spec_pos++] = '%';
        for (int i = 0; i < num_flags; i++) {
            spec[spec_pos++] = flags[i];
        }
        if (width >= 0) {
            spec_pos += snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "%d", width);
        }
        if (precision >= 0) {
            spec_pos += snprintf(spec + spec_pos, sizeof(spec) - spec_pos, ".%d", precision);
        }

        char item[4096];
        item[0] = '\0';

        switch (type) {
        case 'd':
        case 'i': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "lld");
            s32 val = (s32)ARG(arg_idx++);
            if (is_short) val = (s16)val;
            snprintf(item, sizeof(item), spec, (long long)val);
            break;
        }
        case 'u': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "llu");
            u32 val = ARG(arg_idx++);
            if (is_short) val = (u16)val;
            snprintf(item, sizeof(item), spec, (unsigned long long)val);
            break;
        }
        case 'x': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "llx");
            u32 val = ARG(arg_idx++);
            if (is_short) val = (u16)val;
            snprintf(item, sizeof(item), spec, (unsigned long long)val);
            break;
        }
        case 'X': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "llX");
            u32 val = ARG(arg_idx++);
            if (is_short) val = (u16)val;
            snprintf(item, sizeof(item), spec, (unsigned long long)val);
            break;
        }
        case 's': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "s");
            u32 str_p = ARG(arg_idx++);
            if (is_long) {
                char wbuf[1024];
                int wi = 0;
                while (str_p && rd16(str_p + 2 * wi) && wi < 1023) {
                    wbuf[wi] = (char)(rd16(str_p + 2 * wi) & 0x7f);
                    wi++;
                }
                wbuf[wi] = '\0';
                snprintf(item, sizeof(item), spec, wbuf);
            } else {
                const char *s = str_p ? GSTR(str_p) : "";
                snprintf(item, sizeof(item), spec, s);
            }
            break;
        }
        case 'S': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "s");
            u32 str_p = ARG(arg_idx++);
            char wbuf[1024];
            int wi = 0;
            while (str_p && rd16(str_p + 2 * wi) && wi < 1023) {
                wbuf[wi] = (char)(rd16(str_p + 2 * wi) & 0x7f);
                wi++;
            }
            wbuf[wi] = '\0';
            snprintf(item, sizeof(item), spec, wbuf);
            break;
        }
        case 'c': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "c");
            int ch = (int)(u8)ARG(arg_idx++);
            snprintf(item, sizeof(item), spec, ch);
            break;
        }
        case 'C': {
            snprintf(spec + spec_pos, sizeof(spec) - spec_pos, "c");
            int ch = (int)(rd16(c->r[4] + 4 + 4 * arg_idx++) & 0x7f);
            snprintf(item, sizeof(item), spec, ch);
            break;
        }
        case 'p': {
            u32 val = ARG(arg_idx++);
            snprintf(item, sizeof(item), "%08X", val);
            break;
        }
        default:
            item[0] = type;
            item[1] = '\0';
            break;
        }

        for (int i = 0; item[i] && out_pos < (int)sizeof(out_buf) - 1; i++) {
            out_buf[out_pos++] = item[i];
        }
    }

    out_buf[out_pos] = '\0';
    memcpy(G(dst), out_buf, (size_t)out_pos + 1);
    return (u32)out_pos;
}

/* ---- Import table -------------------------------------------------------------------- */

const struct ImpDef imp_u32_misc[] = {
    IMPORT(GetAsyncKeyState, 1),
    IMPORT(GetKeyState, 1),
    IMPORT(keybd_event, 4),
    IMPORT(ShowCursor, 1),
    IMPORT(SetCursor, 1),
    IMPORT(SetCursorPos, 2),
    IMPORT(wsprintfA, -1),
    IMPORT(SetRect, 5),
    IMPORT(InvertRect, 2),
};
IMPORT_TABLE(u32_misc);
