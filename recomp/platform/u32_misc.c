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

/* ---- Dialog items -------------------------------------------------------------------- */
#define MAX_DLG_ITEMS 256
#define DUMMY_DLG_HWND 0x00010042u

struct FakeDlgItem {
    u32 dlg;
    int id;
    u32 hwnd;
    char text[512];
    u32 check_state;
    u32 item_data;
    int in_use;
};

static struct FakeDlgItem dlg_items[MAX_DLG_ITEMS];

static struct FakeDlgItem *get_dlg_item(u32 dlg, int id, int create)
{
    /* 1. Exact match (dlg, id) */
    for (int i = 0; i < MAX_DLG_ITEMS; i++) {
        if (dlg_items[i].in_use && dlg_items[i].dlg == dlg && dlg_items[i].id == id)
            return &dlg_items[i];
    }
    /* 2. Match if dlg is a control HWND */
    if (dlg >= 0x80000000u) {
        for (int i = 0; i < MAX_DLG_ITEMS; i++) {
            if (dlg_items[i].in_use && dlg_items[i].hwnd == dlg)
                return &dlg_items[i];
        }
    }
    /* 3. Fallback: match by item ID if dialog handle is 0 or dummy handle */
    if (dlg == 0 || dlg == DUMMY_DLG_HWND) {
        for (int i = 0; i < MAX_DLG_ITEMS; i++) {
            if (dlg_items[i].in_use && dlg_items[i].id == id)
                return &dlg_items[i];
        }
    }
    if (!create) return NULL;
    for (int i = 0; i < MAX_DLG_ITEMS; i++) {
        if (!dlg_items[i].in_use) {
            dlg_items[i].in_use = 1;
            dlg_items[i].dlg = dlg;
            dlg_items[i].id = id;
            dlg_items[i].hwnd = 0x80000000u | (u32)(i + 1);
            dlg_items[i].text[0] = '\0';
            dlg_items[i].check_state = 0;
            dlg_items[i].item_data = 0;
            return &dlg_items[i];
        }
    }
    /* Reuse slot 0 if table overflows */
    dlg_items[0].dlg = dlg;
    dlg_items[0].id = id;
    dlg_items[0].hwnd = 0x80000001u;
    dlg_items[0].text[0] = '\0';
    dlg_items[0].check_state = 0;
    dlg_items[0].item_data = 0;
    return &dlg_items[0];
}

/* ---- Dialog and message box functions ------------------------------------------------ */

static u32 last_dialog_result = 1;
static int dialog_ended = 0;

/* int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) */
static u32 imp_MessageBoxA(Cpu *c)
{
    u32 hwnd = ARG(0);
    u32 text_p = ARG(1);
    u32 cap_p = ARG(2);
    u32 type = ARG(3);

    const char *text = text_p ? GSTR(text_p) : "";
    const char *caption = cap_p ? GSTR(cap_p) : "";

    plat_log("MessageBoxA(hwnd 0x%x, type 0x%x): [%s] %s", hwnd, type, caption, text);
    SDL_Delay(50);

    u32 btn = type & 0x0Fu;
    if (btn == 0x04 /* MB_YESNO */ || btn == 0x03 /* MB_YESNOCANCEL */)
        return 6; /* IDYES */
    return 1; /* IDOK */
}

/* INT_PTR DialogBoxParamA(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent,
                           DLGPROC lpDialogFunc, LPARAM dwInitParam) */
static u32 imp_DialogBoxParamA(Cpu *c)
{
    u32 hinst = ARG(0);
    u32 tmpl = ARG(1);
    u32 parent = ARG(2);
    u32 dlgproc = ARG(3);
    u32 init_param = ARG(4);
    (void)hinst; (void)parent;

    if (tmpl <= 0xffff) {
        plat_log("DialogBoxParamA: dialog resource ID #%u (dlgproc 0x%08x, initparam 0x%x)",
                 tmpl, dlgproc, init_param);
    } else {
        plat_log("DialogBoxParamA: dialog template '%s' (dlgproc 0x%08x, initparam 0x%x)",
                 GSTR(tmpl), dlgproc, init_param);
    }

    dialog_ended = 0;
    last_dialog_result = 1;

    if (dlgproc) {
        u32 args[4];
        args[0] = DUMMY_DLG_HWND;
        args[1] = 0x0110; /* WM_INITDIALOG */
        args[2] = DUMMY_DLG_HWND;
        args[3] = init_param;
        guest_call(c, dlgproc, 4, args);
    }

    return dialog_ended ? last_dialog_result : 1; /* IDOK */
}

/* BOOL EndDialog(HWND hDlg, INT_PTR nResult) */
static u32 imp_EndDialog(Cpu *c)
{
    u32 hdlg = ARG(0);
    u32 res = ARG(1);
    last_dialog_result = res;
    dialog_ended = 1;
    plat_log("EndDialog(hDlg 0x%x, result %d)", hdlg, (int)res);
    return 1;
}

/* HWND GetDlgItem(HWND hDlg, int nIDDlgItem) */
static u32 imp_GetDlgItem(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);
    struct FakeDlgItem *item = get_dlg_item(dlg, id, 1);
    return item->hwnd;
}

/* BOOL SetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPCSTR lpString) */
static u32 imp_SetDlgItemTextA(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);
    u32 str_p = ARG(2);

    struct FakeDlgItem *item = get_dlg_item(dlg, id, 1);
    if (str_p) {
        strncpy(item->text, GSTR(str_p), sizeof(item->text) - 1);
        item->text[sizeof(item->text) - 1] = '\0';
    } else {
        item->text[0] = '\0';
    }
    return 1;
}

/* UINT GetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPSTR lpString, int cchMax) */
static u32 imp_GetDlgItemTextA(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);
    u32 buf_p = ARG(2);
    int max_cch = (int)ARG(3);

    if (!buf_p || max_cch <= 0) return 0;

    struct FakeDlgItem *item = get_dlg_item(dlg, id, 0);
    const char *src = item ? item->text : "";
    int len = (int)strlen(src);
    if (len > max_cch - 1) len = max_cch - 1;
    memcpy(G(buf_p), src, (size_t)len);
    ((char *)G(buf_p))[len] = '\0';
    return (u32)len;
}

/* LRESULT SendDlgItemMessageA(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam) */
static u32 imp_SendDlgItemMessageA(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);
    u32 msg = ARG(2);
    u32 wparam = ARG(3);
    u32 lparam = ARG(4);

    struct FakeDlgItem *item = get_dlg_item(dlg, id, 1);

    switch (msg) {
    case 0x000C: /* WM_SETTEXT */
        if (lparam) {
            strncpy(item->text, GSTR(lparam), sizeof(item->text) - 1);
            item->text[sizeof(item->text) - 1] = '\0';
        } else {
            item->text[0] = '\0';
        }
        return 1;

    case 0x000D: /* WM_GETTEXT */
        if (lparam && wparam > 0) {
            u32 max_len = wparam - 1;
            u32 len = (u32)strlen(item->text);
            if (len > max_len) len = max_len;
            memcpy(G(lparam), item->text, len);
            ((char *)G(lparam))[len] = '\0';
            return len;
        }
        return 0;

    case 0x000E: /* WM_GETTEXTLENGTH */
        return (u32)strlen(item->text);

    case 0x00F0: /* BM_GETCHECK */
    case 0x00F2: /* BM_GETSTATE */
        return item->check_state;

    case 0x00F1: /* BM_SETCHECK */
        item->check_state = wparam;
        return 0;

    case 0x00F3: /* BM_SETSTATE */
        return 0;

    case 0x0143: /* CB_ADDSTRING */
    case 0x0180: /* LB_ADDSTRING */
        if (item->text[0] == '\0' && lparam) {
            strncpy(item->text, GSTR(lparam), sizeof(item->text) - 1);
            item->text[sizeof(item->text) - 1] = '\0';
        }
        return 0;

    case 0x014E: /* CB_SETCURSEL */
    case 0x0186: /* LB_SETCURSEL */
        return wparam;

    case 0x0147: /* CB_GETCURSEL */
    case 0x0188: /* LB_GETCURSEL */
        return 0;

    case 0x0146: /* CB_GETCOUNT */
    case 0x018B: /* LB_GETCOUNT */
        return 1;

    case 0x0148: /* CB_GETLBTEXT */
    case 0x0189: /* LB_GETTEXT */
        if (lparam) {
            u32 len = (u32)strlen(item->text);
            strcpy((char *)G(lparam), item->text);
            return len;
        }
        return 0;

    case 0x0149: /* CB_GETLBTEXTLEN */
    case 0x018A: /* LB_GETTEXTLEN */
        return (u32)strlen(item->text);

    case 0x0151: /* CB_SETITEMDATA */
    case 0x019A: /* LB_SETITEMDATA */
        item->item_data = lparam;
        return 1;

    case 0x0150: /* CB_GETITEMDATA */
    case 0x0199: /* LB_GETITEMDATA */
        return item->item_data;

    default:
        plat_log("SendDlgItemMessageA(dlg 0x%x, id %d, msg 0x%x, w 0x%x, l 0x%x)",
                 dlg, id, msg, wparam, lparam);
        return 0;
    }
}

/* BOOL CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck) */
static u32 imp_CheckDlgButton(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);
    u32 check = ARG(2);

    struct FakeDlgItem *item = get_dlg_item(dlg, id, 1);
    item->check_state = check;
    return 1;
}

/* UINT IsDlgButtonChecked(HWND hDlg, int nIDButton) */
static u32 imp_IsDlgButtonChecked(Cpu *c)
{
    u32 dlg = ARG(0);
    int id = (int)ARG(1);

    struct FakeDlgItem *item = get_dlg_item(dlg, id, 0);
    return item ? item->check_state : 0;
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
    IMPORT(MessageBoxA, 4),
    IMPORT(DialogBoxParamA, 5),
    IMPORT(EndDialog, 2),
    IMPORT(GetDlgItem, 2),
    IMPORT(SetDlgItemTextA, 3),
    IMPORT(GetDlgItemTextA, 4),
    IMPORT(SendDlgItemMessageA, 5),
    IMPORT(CheckDlgButton, 3),
    IMPORT(IsDlgButtonChecked, 2),
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
