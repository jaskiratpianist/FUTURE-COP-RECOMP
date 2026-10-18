/* USER32 dialog boxes and message boxes: drawn natively in the game's own style.
 * Supports DLGTEMPLATE and DLGTEMPLATEEX loaded from guest PE resources,
 * nesting modal dialogs, gamepad/keyboard navigation, and game-styled controls.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "host.h"
#include "dialog.h"
#include "win.h"
#include "third_party/font8x8_basic.h"

#define DLG_HWND_BASE 0x00DA0000u
#define DLG_HWND_MASK 0xFFFF0000u
#define MAX_OPEN_DLGS 16
#define MAX_CONTROLS  128
#define MAX_ITEMS     64

enum ControlType {
    CTRL_NONE = 0,
    CTRL_BUTTON,
    CTRL_STATIC,
    CTRL_COMBOBOX,
    CTRL_LISTBOX,
    CTRL_SCROLLBAR,
    CTRL_EDIT,
    CTRL_TRACKBAR,
};

enum ButtonType {
    BTN_PUSH = 0,
    BTN_DEFPUSH = 1,
    BTN_CHECK = 2,
    BTN_AUTOCHECK = 3,
    BTN_RADIO = 4,
    BTN_GROUP = 7,
    BTN_AUTORADIO = 9,
};

struct ListItem {
    char text[128];
    u32 data;
};

struct Control {
    u32 hwnd;
    u32 id;
    int type;
    int btn_type;
    u32 style;
    u32 ex_style;

    int x, y, w, h;             /* relative to dialog client area (pixels) */
    int screen_x, screen_y;     /* absolute game screen pixels */

    char text[256];
    int enabled;
    int visible;

    int check_state;
    int is_pressed;

    /* Combobox / Listbox */
    struct ListItem items[MAX_ITEMS];
    int item_count;
    int cur_sel;
    int is_open;
    int scroll_pos;
    int hovered_item;
    int open_h;

    /* Slider / Trackbar */
    int slider_min;
    int slider_max;
    int slider_pos;
    int slider_tic_freq;
    int slider_page_size;
    int slider_line_size;

    /* Edit */
    int cursor_pos;
    int replace_on_type;  /* editable combo box: its text came from the list, typing replaces it */
};

struct Dialog {
    u32 hwnd;
    u32 res_id;
    char title[128];
    u32 style;
    u32 ex_style;
    u32 proc;
    u32 init_param;

    int x, y, w, h;
    int header_h;

    int is_msgbox;
    int msgbox_type;
    char msgbox_lines[16][128];
    int msgbox_num_lines;

    struct Control controls[MAX_CONTROLS];
    int num_controls;

    int focused_ctrl;
    struct Control *drag_slider;
    struct Control *pressed_btn;

    int ended;
    u32 result;
    int active;
};

static struct Dialog dlg_stack[MAX_OPEN_DLGS];
static int dlg_stack_depth = 0;

static u16 draw_buf[1024 * 768];
/* set when a notify static (a controller key box) was clicked: the game then waits for a key
   through its keyboard hook, so the next key press goes to the game, not to dialog navigation */
static int key_capture;

static int cur_mouse_x = 0;
static int cur_mouse_y = 0;

/* ---- Logging -------------------------------------------------------------------------- */

static int dlg_log_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("FCOP_DLG_LOG") ? 1 : 0;
    return cached;
}

/* ---- Handles and lookup ---------------------------------------------------------------- */

int dlg_owns(u32 hwnd)
{
    return ((hwnd & DLG_HWND_MASK) == DLG_HWND_BASE);
}

static struct Dialog *find_dialog(u32 hwnd)
{
    if ((hwnd & DLG_HWND_MASK) != DLG_HWND_BASE) {
        if (hwnd == 0 && dlg_stack_depth > 0) return &dlg_stack[dlg_stack_depth - 1];
        return NULL;
    }
    int d = (hwnd >> 12) & 0x0F;
    if (d < dlg_stack_depth && dlg_stack[d].active) {
        return &dlg_stack[d];
    }
    return NULL;
}

static struct Control *find_ctrl(u32 hwnd, struct Dialog **out_dlg)
{
    if ((hwnd & DLG_HWND_MASK) != DLG_HWND_BASE) return NULL;
    int d = (hwnd >> 12) & 0x0F;
    int c = hwnd & 0x0FFF;
    if (d < dlg_stack_depth && dlg_stack[d].active) {
        if (out_dlg) *out_dlg = &dlg_stack[d];
        if (c > 0 && c <= dlg_stack[d].num_controls) {
            return &dlg_stack[d].controls[c - 1];
        }
    }
    return NULL;
}

static struct Control *find_ctrl_by_id(struct Dialog *dlg, u32 id)
{
    if (!dlg) {
        if (dlg_stack_depth > 0) dlg = &dlg_stack[dlg_stack_depth - 1];
        else return NULL;
    }
    for (int i = 0; i < dlg->num_controls; i++) {
        if (dlg->controls[i].id == id) return &dlg->controls[i];
    }
    return NULL;
}

/* CBS_DROPDOWN (style 2): a combo box with an edit field (the save-game name) */
static int is_edit_combo(const struct Control *c)
{
    return c->type == CTRL_COMBOBOX && (c->style & 3) == 2;
}

static void combo_select(struct Control *c, int idx)
{
    c->cur_sel = idx;
    if (is_edit_combo(c) && idx >= 0 && idx < c->item_count) {
        snprintf(c->text, sizeof c->text, "%s", c->items[idx].text);
        c->cursor_pos = (int)strlen(c->text);
        c->replace_on_type = 1;
    }
}

/* ---- Color and graphics helpers -------------------------------------------------------- */

static inline u16 rgb_to_pixel(int r, int g, int b, int is565)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    if (is565) {
        return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    } else {
        return (u16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    }
}

static inline void pixel_to_rgb(u16 p, int *r, int *g, int *b, int is565)
{
    if (is565) {
        *r = ((p >> 11) & 0x1F) * 255 / 31;
        *g = ((p >> 5) & 0x3F) * 255 / 63;
        *b = (p & 0x1F) * 255 / 31;
    } else {
        *r = ((p >> 10) & 0x1F) * 255 / 31;
        *g = ((p >> 5) & 0x1F) * 255 / 31;
        *b = (p & 0x1F) * 255 / 31;
    }
}

static void blend_pixel(u16 *buf, int scr_w, int scr_h, int x, int y, int r, int g, int b, int a, int is565)
{
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h || a <= 0) return;
    u16 *dst = &buf[y * scr_w + x];
    if (a >= 255) {
        *dst = rgb_to_pixel(r, g, b, is565);
        return;
    }
    int dr, dg, db;
    pixel_to_rgb(*dst, &dr, &dg, &db, is565);
    int out_r = (r * a + dr * (255 - a)) / 255;
    int out_g = (g * a + dg * (255 - a)) / 255;
    int out_b = (b * a + db * (255 - a)) / 255;
    *dst = rgb_to_pixel(out_r, out_g, out_b, is565);
}

static void fill_rect(u16 *buf, int scr_w, int scr_h, int rx, int ry, int rw, int rh, int r, int g, int b, int a, int is565)
{
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > scr_w) rw = scr_w - rx;
    if (ry + rh > scr_h) rh = scr_h - ry;
    if (rw <= 0 || rh <= 0) return;

    if (a >= 255) {
        u16 px = rgb_to_pixel(r, g, b, is565);
        for (int y = ry; y < ry + rh; y++) {
            u16 *row = &buf[y * scr_w + rx];
            for (int x = 0; x < rw; x++) row[x] = px;
        }
    } else {
        for (int y = ry; y < ry + rh; y++) {
            for (int x = rx; x < rx + rw; x++) {
                blend_pixel(buf, scr_w, scr_h, x, y, r, g, b, a, is565);
            }
        }
    }
}

static void draw_glyph(u16 *buf, int scr_w, int scr_h, int px, int py, char ch, int r, int g, int b, int is565, int clip_x2)
{
    if ((unsigned char)ch >= 128) ch = '?';
    for (int row = 0; row < 8; row++) {
        int y = py + row;
        if (y < 0 || y >= scr_h) continue;
        unsigned char bits = (unsigned char)font8x8_basic[(unsigned char)ch][row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if (x < 0 || x >= clip_x2 || x >= scr_w) continue;
            if ((bits >> col) & 1) {
                buf[y * scr_w + x] = rgb_to_pixel(r, g, b, is565);
            }
        }
    }
}

static void draw_string(u16 *buf, int scr_w, int scr_h, int x, int y, const char *s, int r, int g, int b, int is565, int adv, int clip_x2)
{
    if (!s) return;
    /* 1 px dark shadow (#000818) */
    for (int i = 0; s[i]; i++) {
        int gx = x + i * adv;
        if (gx >= clip_x2) break;
        draw_glyph(buf, scr_w, scr_h, gx + 1, y + 1, s[i], 0, 8, 24, is565, clip_x2);
    }
    /* Foreground text */
    for (int i = 0; s[i]; i++) {
        int gx = x + i * adv;
        if (gx >= clip_x2) break;
        draw_glyph(buf, scr_w, scr_h, gx, y, s[i], r, g, b, is565, clip_x2);
    }
}

/* ---- Dialog and control rendering ------------------------------------------------------ */

static void draw_rounded_panel(u16 *buf, int scr_w, int scr_h, int px, int py, int pw, int ph, int header_h, const char *title, int is565)
{
    const int R = 6;
    for (int y = py; y < py + ph; y++) {
        if (y < 0 || y >= scr_h) continue;
        for (int x = px; x < px + pw; x++) {
            if (x < 0 || x >= scr_w) continue;

            double dist = -1.0;
            if (x < px + R && y < py + R) {
                double dx = px + R - x - 0.5;
                double dy = py + R - y - 0.5;
                dist = sqrt(dx * dx + dy * dy);
            } else if (x >= px + pw - R && y < py + R) {
                double dx = x - (px + pw - R - 0.5);
                double dy = py + R - y - 0.5;
                dist = sqrt(dx * dx + dy * dy);
            } else if (x < px + R && y >= py + ph - R) {
                double dx = px + R - x - 0.5;
                double dy = y - (py + ph - R - 0.5);
                dist = sqrt(dx * dx + dy * dy);
            } else if (x >= px + pw - R && y >= py + ph - R) {
                double dx = x - (px + pw - R - 0.5);
                double dy = y - (py + ph - R - 0.5);
                dist = sqrt(dx * dx + dy * dy);
            }

            if (dist > R) continue;

            int edge_dist;
            if (dist >= 0.0) {
                if (dist >= R - 2.0) edge_dist = 0;
                else if (dist >= R - 3.0) edge_dist = 2;
                else edge_dist = 3;
            } else {
                int d_left = x - px;
                int d_right = px + pw - 1 - x;
                int d_top = y - py;
                int d_bottom = py + ph - 1 - y;
                edge_dist = d_left < d_right ? d_left : d_right;
                if (d_top < edge_dist) edge_dist = d_top;
                if (d_bottom < edge_dist) edge_dist = d_bottom;
            }

            if (edge_dist < 2) {
                /* 2 px bright blue border (#2E7BFF) */
                blend_pixel(buf, scr_w, scr_h, x, y, 46, 123, 255, 255, is565);
            } else if (edge_dist < 3) {
                /* 1 px darker inner line (#123A8C) */
                blend_pixel(buf, scr_w, scr_h, x, y, 18, 58, 140, 255, is565);
            } else {
                /* Interior */
                if (header_h > 0 && y < py + header_h - 1) {
                    /* Header strip: #16245E at 95% opacity */
                    blend_pixel(buf, scr_w, scr_h, x, y, 22, 36, 94, 242, is565);
                } else if (header_h > 0 && y == py + header_h - 1) {
                    /* Divider line: #2E7BFF */
                    blend_pixel(buf, scr_w, scr_h, x, y, 46, 123, 255, 255, is565);
                } else {
                    /* Dark navy (#0A1236) at 90% opacity (230/255) */
                    blend_pixel(buf, scr_w, scr_h, x, y, 10, 18, 54, 230, is565);
                }
            }
        }
    }

    if (header_h > 0 && title && title[0]) {
        int len = (int)strlen(title);
        int adv = 7;
        if (len * 7 > pw - 16) adv = 6;
        int tw = len * adv;
        int tx = px + (pw - tw) / 2;
        int ty = py + (header_h - 8) / 2;
        draw_string(buf, scr_w, scr_h, tx, ty, title, 184, 212, 255, is565, adv, px + pw - 6);
    }
}

static void draw_pill_button(u16 *buf, int scr_w, int scr_h, int x, int y, int w, int h, const char *text, int state, int is565)
{
    float r = h / 2.0f;
    float cy = y + r;
    float cx1 = x + r;
    float cx2 = x + w - r;

    int fill_r, fill_g, fill_b;
    int out_r, out_g, out_b;
    int txt_r, txt_g, txt_b;

    switch (state) {
        case 1: /* Hover / Focus */
            fill_r = 24;  fill_g = 48;  fill_b = 122; /* #18307A */
            out_r  = 111; out_g  = 176; out_b  = 255; /* #6FB0FF */
            txt_r  = 255; txt_g  = 255; txt_b  = 255; /* white */
            break;
        case 2: /* Pressed */
            fill_r = 42;  fill_g = 80;  fill_b = 192; /* #2A50C0 */
            out_r  = 111; out_g  = 176; out_b  = 255;
            txt_r  = 255; txt_g  = 255; txt_b  = 255;
            break;
        case 3: /* Disabled */
            fill_r = 10;  fill_g = 18;  fill_b = 54;
            out_r  = 52;  out_g  = 70;  out_b  = 110; /* #34466E */
            txt_r  = 90;  txt_g  = 106; txt_b  = 144; /* #5A6A90 */
            break;
        default: /* Normal */
            fill_r = 12;  fill_g = 26;  fill_b = 74;  /* #0C1A4A */
            out_r  = 46;  out_g  = 123; out_b  = 255; /* #2E7BFF */
            txt_r  = 184; txt_g  = 212; txt_b  = 255; /* #B8D4FF */
            break;
    }

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= scr_h) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= scr_w) continue;
            float dist;
            if (px < cx1) {
                float dx = px - cx1 + 0.5f, dy = py - cy + 0.5f;
                dist = sqrtf(dx * dx + dy * dy);
            } else if (px > cx2) {
                float dx = px - cx2 + 0.5f, dy = py - cy + 0.5f;
                dist = sqrtf(dx * dx + dy * dy);
            } else {
                dist = fabsf(py - cy + 0.5f);
            }

            if (dist > r) continue;

            if (dist >= r - 2.0f) {
                blend_pixel(buf, scr_w, scr_h, px, py, out_r, out_g, out_b, 255, is565);
            } else {
                blend_pixel(buf, scr_w, scr_h, px, py, fill_r, fill_g, fill_b, 255, is565);
            }
        }
    }

    if (text && text[0]) {
        char upper[128];
        int i;
        for (i = 0; text[i] && i < 127; i++) upper[i] = (char)toupper((unsigned char)text[i]);
        upper[i] = '\0';

        int avail_w = w - (int)(2.0f * r);
        if (avail_w < 10) avail_w = w - 8;
        int len = i;
        int adv = 6;
        if (len * 8 <= avail_w) adv = 8;
        else if (len * 7 <= avail_w) adv = 7;
        else snprintf(upper, sizeof upper, "%s", text);  /* capitals collide at 6 px: keep lower case */

        int tw = len * adv;
        int tx = x + (w - tw) / 2;
        int ty = y + (h - 8) / 2;
        draw_string(buf, scr_w, scr_h, tx, ty, upper, txt_r, txt_g, txt_b, is565, adv, x + w - 4);
    }
}

static void draw_checkbox(u16 *buf, int scr_w, int scr_h, int x, int y, int w, int h, int checked, const char *text, int focused, int is565)
{
    (void)w;
    int by = y + (h - 10) / 2;
    int bx = x;

    /* 10 px square outline (#2E7BFF or #6FB0FF if focused) */
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;
    fill_rect(buf, scr_w, scr_h, bx, by, 10, 10, 5, 10, 32, 255, is565);
    for (int i = 0; i < 10; i++) {
        blend_pixel(buf, scr_w, scr_h, bx + i, by, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, bx + i, by + 9, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, bx, by + i, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, bx + 9, by + i, out_r, out_g, out_b, 255, is565);
    }

    if (checked) {
        /* filled bright cyan-blue (#4FA8FF) inner square */
        fill_rect(buf, scr_w, scr_h, bx + 2, by + 2, 6, 6, 79, 168, 255, 255, is565);
    }

    if (text && text[0]) {
        int tx = bx + 14;
        int ty = y + (h - 8) / 2;
        draw_string(buf, scr_w, scr_h, tx, ty, text, 184, 212, 255, is565, 7, scr_w);
    }
}

static void draw_radio(u16 *buf, int scr_w, int scr_h, int x, int y, int w, int h, int checked, const char *text, int focused, int is565)
{
    (void)w;
    int cx = x + 5;
    int cy = y + h / 2;
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;

    for (int py = cy - 5; py <= cy + 5; py++) {
        for (int px = cx - 5; px <= cx + 5; px++) {
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            if (d <= 5.0f) {
                if (d >= 3.8f) {
                    blend_pixel(buf, scr_w, scr_h, px, py, out_r, out_g, out_b, 255, is565);
                } else if (checked && d <= 2.5f) {
                    blend_pixel(buf, scr_w, scr_h, px, py, 79, 168, 255, 255, is565);
                } else {
                    blend_pixel(buf, scr_w, scr_h, px, py, 5, 10, 32, 255, is565);
                }
            }
        }
    }

    if (text && text[0]) {
        int tx = x + 14;
        int ty = y + (h - 8) / 2;
        draw_string(buf, scr_w, scr_h, tx, ty, text, 184, 212, 255, is565, 7, scr_w);
    }
}

static void draw_groupbox(u16 *buf, int scr_w, int scr_h, int x, int y, int w, int h, const char *title, int is565)
{
    int tlen = title ? (int)strlen(title) : 0;
    int tw = tlen * 7;

    /* 1 px blue line (#2E7BFF at 70 % -> alpha 178) */
    int a = 178;
    int r = 46, g = 123, b = 255;

    /* Left */
    for (int py = y; py < y + h; py++) blend_pixel(buf, scr_w, scr_h, x, py, r, g, b, a, is565);
    /* Right */
    for (int py = y; py < y + h; py++) blend_pixel(buf, scr_w, scr_h, x + w - 1, py, r, g, b, a, is565);
    /* Bottom */
    for (int px = x; px < x + w; px++) blend_pixel(buf, scr_w, scr_h, px, y + h - 1, r, g, b, a, is565);
    /* Top line with gap for title */
    if (tlen > 0) {
        for (int px = x; px < x + 6; px++) blend_pixel(buf, scr_w, scr_h, px, y, r, g, b, a, is565);
        for (int px = x + 10 + tw; px < x + w; px++) blend_pixel(buf, scr_w, scr_h, px, y, r, g, b, a, is565);
        draw_string(buf, scr_w, scr_h, x + 8, y - 4, title, 184, 212, 255, is565, 7, x + w - 4);
    } else {
        for (int px = x; px < x + w; px++) blend_pixel(buf, scr_w, scr_h, px, y, r, g, b, a, is565);
    }
}

static void draw_combobox(u16 *buf, int scr_w, int scr_h, struct Control *c, int focused, int is565)
{
    int ch = 22;
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;

    fill_rect(buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, ch, 5, 10, 32, 255, is565);
    for (int px = c->screen_x; px < c->screen_x + c->w; px++) {
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y + ch - 1, out_r, out_g, out_b, 255, is565);
    }
    for (int py = c->screen_y; py < c->screen_y + ch; py++) {
        blend_pixel(buf, scr_w, scr_h, c->screen_x, py, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, c->screen_x + c->w - 1, py, out_r, out_g, out_b, 255, is565);
    }

    /* Down arrow */
    int ax = c->screen_x + c->w - 14;
    int ay = c->screen_y + 9;
    for (int row = 0; row < 4; row++) {
        for (int col = row; col < 7 - row; col++) {
            blend_pixel(buf, scr_w, scr_h, ax + col, ay + row, 79, 168, 255, 255, is565);
        }
    }

    /* Selected text */
    const char *sel_text = NULL;
    if (is_edit_combo(c)) {
        sel_text = c->text;
    } else if (c->cur_sel >= 0 && c->cur_sel < c->item_count) {
        sel_text = c->items[c->cur_sel].text;
    } else if (c->text[0] != '\0') {
        sel_text = c->text;
    } else if (c->item_count > 0) {
        sel_text = c->items[0].text;
    }
    if (sel_text) {
        draw_string(buf, scr_w, scr_h, c->screen_x + 6, c->screen_y + 7, sel_text,
                    184, 212, 255, is565, 7, c->screen_x + c->w - 18);
    }
}

static void draw_combobox_dropdown(u16 *buf, int scr_w, int scr_h, struct Control *c, int is565)
{
    int ch = 22;
    int lx = c->screen_x;
    int ly = c->screen_y + ch;
    int lw = c->w;
    int lh = c->open_h > ch ? c->open_h - ch : 80;
    if (lh < 48) lh = 48;
    if (c->item_count > 0 && lh > 4 + c->item_count * 16) lh = 4 + c->item_count * 16;
    if (ly + lh > scr_h - 4) lh = scr_h - 4 - ly;

    fill_rect(buf, scr_w, scr_h, lx, ly, lw, lh, 10, 18, 54, 255, is565);
    for (int px = lx; px < lx + lw; px++) {
        blend_pixel(buf, scr_w, scr_h, px, ly, 46, 123, 255, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, ly + lh - 1, 46, 123, 255, 255, is565);
    }
    for (int py = ly; py < ly + lh; py++) {
        blend_pixel(buf, scr_w, scr_h, lx, py, 46, 123, 255, 255, is565);
        blend_pixel(buf, scr_w, scr_h, lx + lw - 1, py, 46, 123, 255, 255, is565);
    }

    int vis = (lh - 4) / 16;
    for (int i = 0; i < vis && c->scroll_pos + i < c->item_count; i++) {
        int idx = c->scroll_pos + i;
        int iy = ly + 2 + i * 16;
        int is_highlight = (idx == c->hovered_item) || (c->hovered_item == -1 && idx == c->cur_sel);
        if (is_highlight) {
            fill_rect(buf, scr_w, scr_h, lx + 2, iy, lw - 4, 15, 30, 79, 200, 255, is565);
            draw_string(buf, scr_w, scr_h, lx + 6, iy + 4, c->items[idx].text, 255, 255, 255, is565, 7, lx + lw - 6);
        } else {
            draw_string(buf, scr_w, scr_h, lx + 6, iy + 4, c->items[idx].text, 184, 212, 255, is565, 7, lx + lw - 6);
        }
    }
}

static void draw_listbox(u16 *buf, int scr_w, int scr_h, struct Control *c, int focused, int is565)
{
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;
    fill_rect(buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, 5, 10, 32, 255, is565);
    for (int px = c->screen_x; px < c->screen_x + c->w; px++) {
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y + c->h - 1, out_r, out_g, out_b, 255, is565);
    }
    for (int py = c->screen_y; py < c->screen_y + c->h; py++) {
        blend_pixel(buf, scr_w, scr_h, c->screen_x, py, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, c->screen_x + c->w - 1, py, out_r, out_g, out_b, 255, is565);
    }

    int vis = (c->h - 4) / 16;
    for (int i = 0; i < vis && c->scroll_pos + i < c->item_count; i++) {
        int idx = c->scroll_pos + i;
        int iy = c->screen_y + 2 + i * 16;
        if (idx == c->cur_sel) {
            fill_rect(buf, scr_w, scr_h, c->screen_x + 2, iy, c->w - 4, 15, 30, 79, 200, 255, is565);
            draw_string(buf, scr_w, scr_h, c->screen_x + 6, iy + 4, c->items[idx].text, 255, 255, 255, is565, 7, c->screen_x + c->w - 6);
        } else {
            draw_string(buf, scr_w, scr_h, c->screen_x + 6, iy + 4, c->items[idx].text, 184, 212, 255, is565, 7, c->screen_x + c->w - 6);
        }
    }
}

static void draw_slider_thumb(u16 *buf, int scr_w, int scr_h, int x, int y, int w, int h, int focused, int is565)
{
    float r = w / 2.0f;
    float cx = x + r;
    float cy1 = y + r;
    float cy2 = y + h - r;
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;
    int fill_r = 184, fill_g = 212, fill_b = 255; /* #B8D4FF */

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= scr_h) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= scr_w) continue;
            float dist;
            if (py < cy1) {
                float dx = px - cx + 0.5f, dy = py - cy1 + 0.5f;
                dist = sqrtf(dx * dx + dy * dy);
            } else if (py > cy2) {
                float dx = px - cx + 0.5f, dy = py - cy2 + 0.5f;
                dist = sqrtf(dx * dx + dy * dy);
            } else {
                dist = fabsf(px - cx + 0.5f);
            }
            if (dist > r) continue;
            if (dist >= r - 1.5f) {
                blend_pixel(buf, scr_w, scr_h, px, py, out_r, out_g, out_b, 255, is565);
            } else {
                blend_pixel(buf, scr_w, scr_h, px, py, fill_r, fill_g, fill_b, 255, is565);
            }
        }
    }
}

static void draw_slider(u16 *buf, int scr_w, int scr_h, struct Control *c, int focused, int is565)
{
    int track_y = c->screen_y + c->h / 2;
    int thumb_w = 10;
    int thumb_h = c->h < 18 ? c->h : 18;
    int range = c->slider_max - c->slider_min;
    if (range <= 0) range = 1;
    int val = c->slider_pos;
    if (val < c->slider_min) val = c->slider_min;
    if (val > c->slider_max) val = c->slider_max;

    int thumb_cx = c->screen_x + 6 + (val - c->slider_min) * (c->w - 12) / range;
    int thumb_x = thumb_cx - thumb_w / 2;
    int thumb_y = c->screen_y + (c->h - thumb_h) / 2;

    /* Left track: bright cyan (#4FA8FF), 2 px high */
    for (int px = c->screen_x + 6; px < thumb_cx; px++) {
        blend_pixel(buf, scr_w, scr_h, px, track_y, 79, 168, 255, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, track_y + 1, 79, 168, 255, 255, is565);
    }
    /* Right track: bright blue (#2E7BFF), 2 px high */
    for (int px = thumb_cx; px < c->screen_x + c->w - 6; px++) {
        blend_pixel(buf, scr_w, scr_h, px, track_y, 46, 123, 255, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, track_y + 1, 46, 123, 255, 255, is565);
    }

    /* Thumb: vertical pill (#B8D4FF fill, blue outline) */
    draw_slider_thumb(buf, scr_w, scr_h, thumb_x, thumb_y, thumb_w, thumb_h, focused, is565);
}

static void draw_static(u16 *buf, int scr_w, int scr_h, struct Control *c, int is565)
{
    int boxed = (c->style & 0x00800000 /* WS_BORDER */) || (c->style & 0x1000 /* SS_SUNKEN */);
    if (boxed) {
        fill_rect(buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, 5, 10, 32, 255, is565);
        for (int px = c->screen_x; px < c->screen_x + c->w; px++) {
            blend_pixel(buf, scr_w, scr_h, px, c->screen_y, 46, 123, 255, 255, is565);
            blend_pixel(buf, scr_w, scr_h, px, c->screen_y + c->h - 1, 46, 123, 255, 255, is565);
        }
        for (int py = c->screen_y; py < c->screen_y + c->h; py++) {
            blend_pixel(buf, scr_w, scr_h, c->screen_x, py, 46, 123, 255, 255, is565);
            blend_pixel(buf, scr_w, scr_h, c->screen_x + c->w - 1, py, 46, 123, 255, 255, is565);
        }
    }

    if (c->text && c->text[0]) {
        int len = (int)strlen(c->text);
        int adv = 7;
        if (len * 7 > c->w - 4) adv = 6;
        int tw = len * adv;
        int tx = c->screen_x + 4;
        if ((c->style & 0x03) == 1 /* SS_CENTER */) {
            tx = c->screen_x + (c->w - tw) / 2;
        } else if ((c->style & 0x03) == 2 /* SS_RIGHT */) {
            tx = c->screen_x + c->w - tw - 4;
        }

        int ty = c->screen_y;
        if ((c->style & 0x0200 /* SS_CENTERIMAGE */) || boxed) {
            ty = c->screen_y + (c->h - 8) / 2;
        }
        draw_string(buf, scr_w, scr_h, tx, ty, c->text, 184, 212, 255, is565, adv, c->screen_x + c->w - 2);
    }
}

static void draw_edit(u16 *buf, int scr_w, int scr_h, struct Control *c, int focused, int is565)
{
    int out_r = focused ? 111 : 46, out_g = focused ? 176 : 123, out_b = 255;
    fill_rect(buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, 5, 10, 32, 255, is565);
    for (int px = c->screen_x; px < c->screen_x + c->w; px++) {
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, px, c->screen_y + c->h - 1, out_r, out_g, out_b, 255, is565);
    }
    for (int py = c->screen_y; py < c->screen_y + c->h; py++) {
        blend_pixel(buf, scr_w, scr_h, c->screen_x, py, out_r, out_g, out_b, 255, is565);
        blend_pixel(buf, scr_w, scr_h, c->screen_x + c->w - 1, py, out_r, out_g, out_b, 255, is565);
    }

    int ty = c->screen_y + (c->h - 8) / 2;
    draw_string(buf, scr_w, scr_h, c->screen_x + 4, ty, c->text, 184, 212, 255, is565, 7, c->screen_x + c->w - 4);

    if (focused && ((SDL_GetTicks() / 500) % 2 == 0)) {
        int cur_x = c->screen_x + 4 + c->cursor_pos * 7;
        if (cur_x < c->screen_x + c->w - 2) {
            for (int y = c->screen_y + 3; y < c->screen_y + c->h - 3; y++) {
                blend_pixel(buf, scr_w, scr_h, cur_x, y, 255, 255, 255, 255, is565);
            }
        }
    }
}

static void render_all_dialogs(void)
{
    const u16 *px = NULL;
    int scr_w = 640, scr_h = 480, is565 = 1;
    win_last_frame(&px, &scr_w, &scr_h, &is565);
    if (scr_w <= 0 || scr_h <= 0) { scr_w = 640; scr_h = 480; is565 = 1; }

    /* Darken last frame to ~40% brightness */
    if (px) {
        for (int y = 0; y < scr_h; y++) {
            for (int x = 0; x < scr_w; x++) {
                u16 p = px[y * scr_w + x];
                if (is565) {
                    int r = ((p >> 11) & 0x1F) * 40 / 100;
                    int g = ((p >> 5) & 0x3F) * 40 / 100;
                    int b = (p & 0x1F) * 40 / 100;
                    draw_buf[y * scr_w + x] = (u16)((r << 11) | (g << 5) | b);
                } else {
                    int r = ((p >> 10) & 0x1F) * 40 / 100;
                    int g = ((p >> 5) & 0x1F) * 40 / 100;
                    int b = (p & 0x1F) * 40 / 100;
                    draw_buf[y * scr_w + x] = (u16)((r << 10) | (g << 5) | b);
                }
            }
        }
    } else {
        memset(draw_buf, 0, (size_t)scr_w * scr_h * 2);
    }

    struct Control *open_combo = NULL;

    for (int d = 0; d < dlg_stack_depth; d++) {
        struct Dialog *dlg = &dlg_stack[d];
        if (!dlg->active) continue;

        draw_rounded_panel(draw_buf, scr_w, scr_h, dlg->x, dlg->y, dlg->w, dlg->h, dlg->header_h, dlg->title, is565);

        if (dlg->is_msgbox) {
            for (int line = 0; line < dlg->msgbox_num_lines; line++) {
                int ty = dlg->y + dlg->header_h + 16 + line * 14;
                draw_string(draw_buf, scr_w, scr_h, dlg->x + 24, ty, dlg->msgbox_lines[line],
                            184, 212, 255, is565, 7, dlg->x + dlg->w - 24);
            }
        }

        for (int i = 0; i < dlg->num_controls; i++) {
            struct Control *c = &dlg->controls[i];
            if (!c->visible) continue;
            int focused = (dlg->focused_ctrl == i);

            switch (c->type) {
                case CTRL_BUTTON:
                    if (c->btn_type == BTN_PUSH || c->btn_type == BTN_DEFPUSH) {
                        int state = 0;
                        int mouse_over = (cur_mouse_x >= c->screen_x && cur_mouse_x < c->screen_x + c->w &&
                                          cur_mouse_y >= c->screen_y && cur_mouse_y < c->screen_y + c->h);
                        if (!c->enabled) state = 3;
                        else if (c->is_pressed) state = 2;
                        else if (focused || mouse_over) state = 1;
                        draw_pill_button(draw_buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, c->text, state, is565);
                    } else if (c->btn_type == BTN_CHECK || c->btn_type == BTN_AUTOCHECK) {
                        draw_checkbox(draw_buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, c->check_state, c->text, focused, is565);
                    } else if (c->btn_type == BTN_RADIO || c->btn_type == BTN_AUTORADIO) {
                        draw_radio(draw_buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, c->check_state, c->text, focused, is565);
                    } else if (c->btn_type == BTN_GROUP) {
                        draw_groupbox(draw_buf, scr_w, scr_h, c->screen_x, c->screen_y, c->w, c->h, c->text, is565);
                    }
                    break;
                case CTRL_STATIC:
                    draw_static(draw_buf, scr_w, scr_h, c, is565);
                    break;
                case CTRL_COMBOBOX:
                    draw_combobox(draw_buf, scr_w, scr_h, c, focused, is565);
                    if (c->is_open) open_combo = c;
                    break;
                case CTRL_LISTBOX:
                    draw_listbox(draw_buf, scr_w, scr_h, c, focused, is565);
                    break;
                case CTRL_TRACKBAR:
                    draw_slider(draw_buf, scr_w, scr_h, c, focused, is565);
                    break;
                case CTRL_EDIT:
                    draw_edit(draw_buf, scr_w, scr_h, c, focused, is565);
                    break;
                default:
                    break;
            }
        }
    }

    if (open_combo) {
        draw_combobox_dropdown(draw_buf, scr_w, scr_h, open_combo, is565);
    }

    u32 rmask = is565 ? 0xF800 : 0x7C00;
    u32 gmask = is565 ? 0x07E0 : 0x03E0;
    u32 bmask = 0x001F;
    win_show(draw_buf, scr_w, scr_h, scr_w * 2, 16, rmask, gmask, bmask);
}

/* ---- Dialog action triggers ------------------------------------------------------------ */

static void trigger_button_click(Cpu *c, struct Dialog *dlg, struct Control *ctrl)
{
    if (!ctrl->enabled || !ctrl->visible) return;

    if (dlg->is_msgbox) {
        dlg->ended = 1;
        dlg->result = ctrl->id;
        return;
    }

    if (ctrl->type == CTRL_BUTTON) {
        if (ctrl->btn_type == BTN_AUTOCHECK) {
            ctrl->check_state = !ctrl->check_state;
        } else if (ctrl->btn_type == BTN_AUTORADIO) {
            int ctrl_idx = -1;
            for (int i = 0; i < dlg->num_controls; i++) {
                if (&dlg->controls[i] == ctrl) { ctrl_idx = i; break; }
            }
            if (ctrl_idx >= 0) {
                int g_start = ctrl_idx;
                while (g_start > 0 && !(dlg->controls[g_start].style & 0x00020000 /* WS_GROUP */)) {
                    g_start--;
                }
                int g_end = ctrl_idx + 1;
                while (g_end < dlg->num_controls && !(dlg->controls[g_end].style & 0x00020000 /* WS_GROUP */)) {
                    g_end++;
                }
                for (int i = g_start; i < g_end; i++) {
                    if (dlg->controls[i].type == CTRL_BUTTON &&
                        (dlg->controls[i].btn_type == BTN_AUTORADIO || dlg->controls[i].btn_type == BTN_RADIO)) {
                        dlg->controls[i].check_state = (i == ctrl_idx) ? 1 : 0;
                    }
                }
            }
        }
    }

    if (dlg->proc) {
        u32 wp = (0 /* BN_CLICKED */ << 16) | (ctrl->id & 0xFFFF);
        u32 lp = ctrl->hwnd;
        if (dlg_log_enabled()) {
            plat_log("dlg 0x%x notify WM_COMMAND: id %u BN_CLICKED (ctrl 0x%x)", dlg->hwnd, ctrl->id, ctrl->hwnd);
        }
        guest_call(c ? c : cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111 /* WM_COMMAND */, wp, lp });
    }
}

static void trigger_static_click(Cpu *c, struct Dialog *dlg, struct Control *ctrl)
{
    if (!ctrl->enabled || !ctrl->visible) return;
    if (dlg->proc && (ctrl->style & 0x0100 /* SS_NOTIFY */)) {
        key_capture = 1;
        u32 wp = (0 /* STN_CLICKED */ << 16) | (ctrl->id & 0xFFFF);
        u32 lp = ctrl->hwnd;
        if (dlg_log_enabled()) {
            plat_log("dlg 0x%x notify WM_COMMAND: id %u STN_CLICKED (ctrl 0x%x)", dlg->hwnd, ctrl->id, ctrl->hwnd);
        }
        guest_call(c ? c : cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111 /* WM_COMMAND */, wp, lp });
    }
}

static int is_focusable(struct Control *c)
{
    if (!c->visible || !c->enabled) return 0;
    if (c->type == CTRL_BUTTON) {
        return c->btn_type != BTN_GROUP;
    }
    if (c->type == CTRL_COMBOBOX || c->type == CTRL_LISTBOX || c->type == CTRL_TRACKBAR || c->type == CTRL_EDIT) {
        return 1;
    }
    if (c->type == CTRL_STATIC && (c->style & 0x0100 /* SS_NOTIFY */)) {
        return 1;
    }
    return 0;
}

static void focus_next(struct Dialog *dlg, int dir)
{
    if (dlg->num_controls <= 0) return;
    int cur = dlg->focused_ctrl;
    for (int step = 1; step <= dlg->num_controls; step++) {
        int next = (cur + dir * step + dlg->num_controls * 10) % dlg->num_controls;
        if (is_focusable(&dlg->controls[next])) {
            dlg->focused_ctrl = next;
            return;
        }
    }
}

static void update_slider_pos_from_mouse(struct Dialog *dlg, struct Control *c, int mx, int is_drag)
{
    int track_start = c->screen_x + 6;
    int track_len = c->w - 12;
    if (track_len <= 0) return;
    int rel = mx - track_start;
    if (rel < 0) rel = 0;
    if (rel > track_len) rel = track_len;
    int range = c->slider_max - c->slider_min;
    int new_pos = c->slider_min + (rel * range + track_len / 2) / track_len;
    c->slider_pos = new_pos;

    u32 code = is_drag ? 5 /* TB_THUMBTRACK */ : 8 /* TB_ENDTRACK */;
    u32 wp = (c->slider_pos << 16) | code;
    if (dlg->proc) {
        guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0114 /* WM_HSCROLL */, wp, c->hwnd });
    }
}

/* ---- Event hook ------------------------------------------------------------------------ */

static int dlg_event_hook(const SDL_Event *e)
{
    if (dlg_stack_depth <= 0) return 0;
    struct Dialog *dlg = &dlg_stack[dlg_stack_depth - 1];
    if (!dlg->active) return 0;
    if (key_capture && (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP)) {
        if (e->type == SDL_KEYUP) key_capture = 0;
        return 0;
    }

    switch (e->type) {
        case SDL_MOUSEMOTION: {
            cur_mouse_x = e->motion.x;
            cur_mouse_y = e->motion.y;
            if (dlg->drag_slider) {
                update_slider_pos_from_mouse(dlg, dlg->drag_slider, cur_mouse_x, 1);
            }
            /* Update hovered item in open combobox */
            for (int i = 0; i < dlg->num_controls; i++) {
                struct Control *c = &dlg->controls[i];
                if (c->type == CTRL_COMBOBOX && c->is_open) {
                    int ly = c->screen_y + 22;
                    int lh = c->open_h > 22 ? c->open_h - 22 : 80;
                    if (cur_mouse_x >= c->screen_x && cur_mouse_x < c->screen_x + c->w &&
                        cur_mouse_y >= ly && cur_mouse_y < ly + lh) {
                        int row = (cur_mouse_y - ly - 2) / 16;
                        c->hovered_item = c->scroll_pos + row;
                    } else {
                        c->hovered_item = -1;
                    }
                }
            }
            return 1;
        }

        case SDL_MOUSEBUTTONDOWN: {
            if (e->button.button != SDL_BUTTON_LEFT) return 1;
            cur_mouse_x = e->button.x;
            cur_mouse_y = e->button.y;

            /* Check if clicked inside open combobox dropdown */
            for (int i = 0; i < dlg->num_controls; i++) {
                struct Control *c = &dlg->controls[i];
                if (c->type == CTRL_COMBOBOX && c->is_open) {
                    int ly = c->screen_y + 22;
                    int lh = c->open_h > 22 ? c->open_h - 22 : 80;
                    if (cur_mouse_x >= c->screen_x && cur_mouse_x < c->screen_x + c->w &&
                        cur_mouse_y >= ly && cur_mouse_y < ly + lh) {
                        int row = (cur_mouse_y - ly - 2) / 16;
                        int idx = c->scroll_pos + row;
                        if (idx >= 0 && idx < c->item_count) {
                            combo_select(c, idx);
                            c->is_open = 0;
                            if (dlg->proc) {
                                u32 wp = (1 /* CBN_SELCHANGE */ << 16) | (c->id & 0xFFFF);
                                guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, wp, c->hwnd });
                            }
                        }
                        return 1;
                    }
                    /* Clicked outside: close it */
                    c->is_open = 0;
                }
            }

            /* Hit test dialog controls */
            for (int i = 0; i < dlg->num_controls; i++) {
                struct Control *c = &dlg->controls[i];
                if (!c->visible || !c->enabled) continue;

                int ch = (c->type == CTRL_COMBOBOX) ? 22 : c->h;
                if (cur_mouse_x >= c->screen_x && cur_mouse_x < c->screen_x + c->w &&
                    cur_mouse_y >= c->screen_y && cur_mouse_y < c->screen_y + ch) {

                    dlg->focused_ctrl = i;

                    if (c->type == CTRL_BUTTON) {
                        if (c->btn_type == BTN_PUSH || c->btn_type == BTN_DEFPUSH) {
                            c->is_pressed = 1;
                            dlg->pressed_btn = c;
                        } else {
                            trigger_button_click(cur_cpu, dlg, c);
                        }
                    } else if (c->type == CTRL_STATIC) {
                        trigger_static_click(cur_cpu, dlg, c);
                    } else if (c->type == CTRL_COMBOBOX) {
                        c->is_open = !c->is_open;
                        c->hovered_item = -1;
                    } else if (c->type == CTRL_TRACKBAR) {
                        dlg->drag_slider = c;
                        update_slider_pos_from_mouse(dlg, c, cur_mouse_x, 1);
                    } else if (c->type == CTRL_LISTBOX) {
                        int row = (cur_mouse_y - c->screen_y - 2) / 16;
                        int idx = c->scroll_pos + row;
                        if (idx >= 0 && idx < c->item_count) {
                            c->cur_sel = idx;
                            u32 code = (e->button.clicks > 1) ? 2 /* LBN_DBLCLK */ : 1 /* LBN_SELCHANGE */;
                            if (dlg->proc) {
                                guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, (code << 16) | (c->id & 0xFFFF), c->hwnd });
                            }
                        }
                    } else if (c->type == CTRL_EDIT) {
                        SDL_StartTextInput();
                    }
                    return 1;
                }
            }
            return 1;
        }

        case SDL_MOUSEBUTTONUP: {
            if (e->button.button != SDL_BUTTON_LEFT) return 1;
            cur_mouse_x = e->button.x;
            cur_mouse_y = e->button.y;

            if (dlg->drag_slider) {
                update_slider_pos_from_mouse(dlg, dlg->drag_slider, cur_mouse_x, 0);
                dlg->drag_slider = NULL;
            }

            if (dlg->pressed_btn) {
                struct Control *pb = dlg->pressed_btn;
                pb->is_pressed = 0;
                dlg->pressed_btn = NULL;
                if (cur_mouse_x >= pb->screen_x && cur_mouse_x < pb->screen_x + pb->w &&
                    cur_mouse_y >= pb->screen_y && cur_mouse_y < pb->screen_y + pb->h) {
                    trigger_button_click(cur_cpu, dlg, pb);
                }
            }
            return 1;
        }

        case SDL_MOUSEWHEEL: {
            for (int i = 0; i < dlg->num_controls; i++) {
                struct Control *c = &dlg->controls[i];
                if ((c->type == CTRL_COMBOBOX && c->is_open) || c->type == CTRL_LISTBOX) {
                    int vis = (c->type == CTRL_COMBOBOX) ? ((c->open_h - 22) / 16) : (c->h / 16);
                    if (vis < 1) vis = 1;
                    if (e->wheel.y > 0 && c->scroll_pos > 0) c->scroll_pos--;
                    else if (e->wheel.y < 0 && c->scroll_pos + vis < c->item_count) c->scroll_pos++;
                }
            }
            return 1;
        }

        case SDL_KEYDOWN: {
            SDL_Keycode k = e->key.keysym.sym;
            Uint16 mod = e->key.keysym.mod;

            if (k == SDLK_ESCAPE) {
                if (dlg->is_msgbox) {
                    /* Esc chooses Cancel, or No if there is no Cancel */
                    u32 res = 1;
                    if (dlg->msgbox_type == 1 || dlg->msgbox_type == 3 || dlg->msgbox_type == 5) res = 2; /* IDCANCEL */
                    else if (dlg->msgbox_type == 4) res = 7; /* IDNO */
                    dlg->ended = 1;
                    dlg->result = res;
                    return 1;
                }
                if (dlg_log_enabled()) plat_log("dlg 0x%x notify WM_COMMAND: IDCANCEL (2)", dlg->hwnd);
                if (dlg->proc) {
                    guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, 2 /* IDCANCEL */, 0 });
                    /* the game's dialogs mostly ignore IDCANCEL and only react to their own
                       buttons: then Esc presses "Cancel" (or "Resume" on the pause bar) */
                    for (int i = 0; !dlg->ended && i < dlg->num_controls; i++) {
                        struct Control *b = &dlg->controls[i];
                        if (b->type == CTRL_BUTTON && (b->btn_type == BTN_PUSH || b->btn_type == BTN_DEFPUSH) &&
                            b->visible && b->enabled && (!strcasecmp(b->text, "Cancel") || !strncasecmp(b->text, "Resume", 6))) {
                            trigger_button_click(cur_cpu, dlg, b);
                            break;
                        }
                    }
                } else {
                    dlg->ended = 1;
                    dlg->result = 2;
                }
                return 1;
            }

            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                if (dlg->is_msgbox) {
                    /* Enter chooses focused button, otherwise first button */
                    u32 res = 1;
                    if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                        res = dlg->controls[dlg->focused_ctrl].id;
                    } else if (dlg->num_controls > 0) {
                        res = dlg->controls[0].id;
                    }
                    dlg->ended = 1;
                    dlg->result = res;
                    return 1;
                }

                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_BUTTON && (fc->btn_type == BTN_PUSH || fc->btn_type == BTN_DEFPUSH)) {
                        trigger_button_click(cur_cpu, dlg, fc);
                        return 1;
                    }
                }
                for (int i = 0; i < dlg->num_controls; i++) {
                    if (dlg->controls[i].type == CTRL_BUTTON && dlg->controls[i].btn_type == BTN_DEFPUSH &&
                        dlg->controls[i].visible && dlg->controls[i].enabled) {
                        trigger_button_click(cur_cpu, dlg, &dlg->controls[i]);
                        return 1;
                    }
                }
                if (dlg_log_enabled()) plat_log("dlg 0x%x notify WM_COMMAND: IDOK (1)", dlg->hwnd);
                if (dlg->proc) {
                    guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, 1 /* IDOK */, 0 });
                } else {
                    dlg->ended = 1;
                    dlg->result = 1;
                }
                return 1;
            }

            if (k == SDLK_TAB) {
                focus_next(dlg, (mod & KMOD_SHIFT) ? -1 : 1);
                return 1;
            }

            if (k == SDLK_DOWN) {
                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_COMBOBOX || fc->type == CTRL_LISTBOX) {
                        if (fc->cur_sel + 1 < fc->item_count) {
                            fc->cur_sel++;
                            u32 code = (fc->type == CTRL_COMBOBOX) ? 1 : 1;
                            if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, (code << 16) | (fc->id & 0xFFFF), fc->hwnd });
                        }
                        return 1;
                    }
                }
                focus_next(dlg, 1);
                return 1;
            }

            if (k == SDLK_UP) {
                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_COMBOBOX || fc->type == CTRL_LISTBOX) {
                        if (fc->cur_sel > 0) {
                            fc->cur_sel--;
                            u32 code = (fc->type == CTRL_COMBOBOX) ? 1 : 1;
                            if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, (code << 16) | (fc->id & 0xFFFF), fc->hwnd });
                        }
                        return 1;
                    }
                }
                focus_next(dlg, -1);
                return 1;
            }

            if (k == SDLK_LEFT || k == SDLK_RIGHT) {
                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_TRACKBAR) {
                        int range = fc->slider_max - fc->slider_min;
                        int step = range / 10;
                        if (step < 1) step = 1;
                        if (k == SDLK_LEFT) fc->slider_pos -= step;
                        else fc->slider_pos += step;
                        if (fc->slider_pos < fc->slider_min) fc->slider_pos = fc->slider_min;
                        if (fc->slider_pos > fc->slider_max) fc->slider_pos = fc->slider_max;
                        u32 wp = (fc->slider_pos << 16) | 8 /* TB_ENDTRACK */;
                        if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0114, wp, fc->hwnd });
                        return 1;
                    } else if (fc->type == CTRL_COMBOBOX) {
                        if (k == SDLK_LEFT && fc->cur_sel > 0) {
                            fc->cur_sel--;
                            if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, (1 << 16) | (fc->id & 0xFFFF), fc->hwnd });
                        } else if (k == SDLK_RIGHT && fc->cur_sel + 1 < fc->item_count) {
                            fc->cur_sel++;
                            if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, (1 << 16) | (fc->id & 0xFFFF), fc->hwnd });
                        }
                        return 1;
                    } else if (fc->type == CTRL_BUTTON && dlg->is_msgbox) {
                        focus_next(dlg, (k == SDLK_LEFT) ? -1 : 1);
                        return 1;
                    }
                }
            }

            if (k == SDLK_SPACE) {
                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_BUTTON) {
                        trigger_button_click(cur_cpu, dlg, fc);
                    }
                }
                return 1;
            }

            if (k == SDLK_BACKSPACE) {
                if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                    struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                    if (fc->type == CTRL_EDIT || is_edit_combo(fc)) {
                        int len = (int)strlen(fc->text);
                        if (fc->replace_on_type) { fc->text[0] = 0; fc->cursor_pos = len = 0; fc->replace_on_type = 0; }
                        if (fc->cursor_pos > 0 && len > 0) {
                            memmove(&fc->text[fc->cursor_pos - 1], &fc->text[fc->cursor_pos], len - fc->cursor_pos + 1);
                            fc->cursor_pos--;
                            if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, ((fc->type == CTRL_EDIT ? 0x300u /* EN_CHANGE */ : 5u /* CBN_EDITCHANGE */) << 16) | (fc->id & 0xFFFF), fc->hwnd });
                        }
                    }
                }
                return 1;
            }

            return 1;
        }

        case SDL_TEXTINPUT: {
            if (dlg->focused_ctrl >= 0 && dlg->focused_ctrl < dlg->num_controls) {
                struct Control *fc = &dlg->controls[dlg->focused_ctrl];
                if (fc->type == CTRL_EDIT || is_edit_combo(fc)) {
                    if (fc->replace_on_type) { fc->text[0] = 0; fc->cursor_pos = 0; fc->replace_on_type = 0; }
                    int len = (int)strlen(fc->text);
                    int ins_len = (int)strlen(e->text.text);
                    if (len + ins_len < (int)sizeof(fc->text) - 1) {
                        memmove(&fc->text[fc->cursor_pos + ins_len], &fc->text[fc->cursor_pos], len - fc->cursor_pos + 1);
                        memcpy(&fc->text[fc->cursor_pos], e->text.text, ins_len);
                        fc->cursor_pos += ins_len;
                        if (dlg->proc) guest_call(cur_cpu, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0111, ((fc->type == CTRL_EDIT ? 0x300u /* EN_CHANGE */ : 5u /* CBN_EDITCHANGE */) << 16) | (fc->id & 0xFFFF), fc->hwnd });
                    }
                }
            }
            return 1;
        }

        default:
            break;
    }
    return 1;
}

/* ---- PE Resource parsing --------------------------------------------------------------- */

static u32 read_utf16_str(u32 addr, char *dst, size_t max)
{
    size_t i = 0;
    while (1) {
        u16 ch = rd16(addr);
        addr += 2;
        if (!ch) break;
        if (i + 1 < max) {
            dst[i++] = (ch < 256) ? (char)ch : '?';
        }
    }
    if (max > 0) dst[i] = '\0';
    return addr;
}

static int find_pe_dialog_resource(u32 tmpl, u32 *out_addr, u32 *out_size)
{
    u32 pe_hdr = 0x400000 + rd32(0x40003c);
    u32 rsrc_rva = rd32(pe_hdr + 136);
    u32 rsrc_size = rd32(pe_hdr + 140);
    if (!rsrc_rva || !rsrc_size) return 0;
    u32 rsrc_base = 0x400000 + rsrc_rva;

    /* Level 1: Type (RT_DIALOG = 5) */
    u16 num_named = rd16(rsrc_base + 12);
    u16 num_id = rd16(rsrc_base + 14);
    u32 type_dir = 0;
    for (int i = 0; i < num_named + num_id; i++) {
        u32 entry = rsrc_base + 16 + i * 8;
        u32 name = rd32(entry);
        u32 offset = rd32(entry + 4);
        if (!(name & 0x80000000) && name == 5 /* RT_DIALOG */) {
            type_dir = rsrc_base + (offset & 0x7FFFFFFF);
            break;
        }
    }
    if (!type_dir) return 0;

    /* Level 2: Name / Resource ID */
    num_named = rd16(type_dir + 12);
    num_id = rd16(type_dir + 14);
    u32 lang_dir = 0;
    for (int i = 0; i < num_named + num_id; i++) {
        u32 entry = type_dir + 16 + i * 8;
        u32 name = rd32(entry);
        u32 offset = rd32(entry + 4);
        if (tmpl <= 0xFFFF) {
            if (!(name & 0x80000000) && name == tmpl) {
                lang_dir = rsrc_base + (offset & 0x7FFFFFFF);
                break;
            }
        } else {
            if (name & 0x80000000) {
                u32 str_addr = rsrc_base + (name & 0x7FFFFFFF);
                u16 slen = rd16(str_addr);
                char res_name[128];
                int k;
                for (k = 0; k < slen && k < 127; k++) {
                    u16 wc = rd16(str_addr + 2 + k * 2);
                    res_name[k] = (wc < 256) ? (char)wc : '?';
                }
                res_name[k] = '\0';
                if (!strcasecmp(res_name, GSTR(tmpl))) {
                    lang_dir = rsrc_base + (offset & 0x7FFFFFFF);
                    break;
                }
            }
        }
    }
    if (!lang_dir) return 0;

    /* Level 3: Language (take the first entry) */
    u32 data_entry_offset = rd32(lang_dir + 16 + 4);
    if (data_entry_offset & 0x80000000) return 0;
    u32 data_entry = rsrc_base + data_entry_offset;
    u32 dlg_rva = rd32(data_entry + 0);
    u32 dlg_sz = rd32(data_entry + 4);

    *out_addr = 0x400000 + dlg_rva;
    *out_size = dlg_sz;
    return 1;
}

static int parse_dialog_template(struct Dialog *dlg, u32 addr, u32 size)
{
    (void)size;
    u16 w0 = rd16(addr);
    u16 w1 = rd16(addr + 2);
    int is_ex = (w0 == 1 && w1 == 0xFFFF);
    u16 item_count = 0;
    u32 cur = addr;
    char dummy[128];

    if (is_ex) {
        dlg->ex_style = rd32(cur + 8);
        dlg->style = rd32(cur + 12);
        item_count = rd16(cur + 16);
        dlg->w = (s16)rd16(cur + 22);
        dlg->h = (s16)rd16(cur + 24);
        cur += 26;

        u16 mw = rd16(cur);
        if (mw == 0x0000) cur += 2;
        else if (mw == 0xFFFF) cur += 4;
        else cur = read_utf16_str(cur, dummy, sizeof(dummy));

        u16 cw = rd16(cur);
        if (cw == 0x0000) cur += 2;
        else if (cw == 0xFFFF) cur += 4;
        else cur = read_utf16_str(cur, dummy, sizeof(dummy));

        cur = read_utf16_str(cur, dlg->title, sizeof(dlg->title));

        if (dlg->style & 0x40 /* DS_SETFONT */) {
            cur += 6; /* pointsize(2), weight(2), italic(1), charset(1) */
            cur = read_utf16_str(cur, dummy, sizeof(dummy));
        }
    } else {
        dlg->style = rd32(cur + 0);
        dlg->ex_style = rd32(cur + 4);
        item_count = rd16(cur + 8);
        dlg->w = (s16)rd16(cur + 14);
        dlg->h = (s16)rd16(cur + 16);
        cur += 18;

        u16 mw = rd16(cur);
        if (mw == 0x0000) cur += 2;
        else if (mw == 0xFFFF) cur += 4;
        else cur = read_utf16_str(cur, dummy, sizeof(dummy));

        u16 cw = rd16(cur);
        if (cw == 0x0000) cur += 2;
        else if (cw == 0xFFFF) cur += 4;
        else cur = read_utf16_str(cur, dummy, sizeof(dummy));

        cur = read_utf16_str(cur, dlg->title, sizeof(dlg->title));

        if (dlg->style & 0x40 /* DS_SETFONT */) {
            cur += 2; /* pointsize(2) */
            cur = read_utf16_str(cur, dummy, sizeof(dummy));
        }
    }

    if (item_count > MAX_CONTROLS) item_count = MAX_CONTROLS;
    dlg->num_controls = item_count;

    for (int i = 0; i < item_count; i++) {
        cur = (cur + 3) & ~3u;
        struct Control *c = &dlg->controls[i];
        memset(c, 0, sizeof(*c));
        c->hwnd = dlg->hwnd + (i + 1);

        if (is_ex) {
            c->ex_style = rd32(cur + 4);
            c->style = rd32(cur + 8);
            c->x = (s16)rd16(cur + 12);
            c->y = (s16)rd16(cur + 14);
            c->w = (s16)rd16(cur + 16);
            c->h = (s16)rd16(cur + 18);
            c->id = rd32(cur + 20);
            cur += 24;
        } else {
            c->style = rd32(cur + 0);
            c->ex_style = rd32(cur + 4);
            c->x = (s16)rd16(cur + 8);
            c->y = (s16)rd16(cur + 10);
            c->w = (s16)rd16(cur + 12);
            c->h = (s16)rd16(cur + 14);
            c->id = (u16)rd16(cur + 16);
            cur += 18;
        }

        u16 cw = rd16(cur);
        if (cw == 0xFFFF) {
            u16 atom = rd16(cur + 2);
            cur += 4;
            switch (atom) {
                case 0x80: c->type = CTRL_BUTTON; break;
                case 0x81: c->type = CTRL_EDIT; break;
                case 0x82: c->type = CTRL_STATIC; break;
                case 0x83: c->type = CTRL_LISTBOX; break;
                case 0x84: c->type = CTRL_SCROLLBAR; break;
                case 0x85: c->type = CTRL_COMBOBOX; break;
                default:   c->type = CTRL_STATIC; break;
            }
        } else {
            char cls_name[64];
            cur = read_utf16_str(cur, cls_name, sizeof(cls_name));
            if (!strcasecmp(cls_name, "msctls_trackbar32")) c->type = CTRL_TRACKBAR;
            else if (!strcasecmp(cls_name, "BUTTON")) c->type = CTRL_BUTTON;
            else if (!strcasecmp(cls_name, "EDIT")) c->type = CTRL_EDIT;
            else if (!strcasecmp(cls_name, "STATIC")) c->type = CTRL_STATIC;
            else if (!strcasecmp(cls_name, "LISTBOX")) c->type = CTRL_LISTBOX;
            else if (!strcasecmp(cls_name, "COMBOBOX")) c->type = CTRL_COMBOBOX;
            else c->type = CTRL_STATIC;
        }

        u16 tw = rd16(cur);
        if (tw == 0xFFFF) {
            u16 ord = rd16(cur + 2);
            cur += 4;
            snprintf(c->text, sizeof(c->text), "%u", ord);
        } else {
            cur = read_utf16_str(cur, c->text, sizeof(c->text));
        }

        u16 extra = rd16(cur);
        cur += 2;
        if (extra > 0) cur += extra;

        if (c->type == CTRL_BUTTON) {
            c->btn_type = c->style & 0x0F;
        } else if (c->type == CTRL_TRACKBAR) {
            c->slider_min = 0;
            c->slider_max = 100;
            c->slider_pos = 0;
        } else if (c->type == CTRL_COMBOBOX || c->type == CTRL_LISTBOX) {
            c->cur_sel = -1;
            c->hovered_item = -1;
        } else if (c->type == CTRL_EDIT) {
            c->cursor_pos = (int)strlen(c->text);
        }

        c->visible = (c->style & 0x10000000 /* WS_VISIBLE */) != 0;
        c->enabled = (c->style & 0x08000000 /* WS_DISABLED */) == 0;
    }

    return 1;
}

static void layout_dialog(struct Dialog *dlg)
{
    const u16 *px = NULL;
    int scr_w = 640, scr_h = 480, is565 = 1;
    win_last_frame(&px, &scr_w, &scr_h, &is565);
    if (scr_w <= 0 || scr_h <= 0) { scr_w = 640; scr_h = 480; is565 = 1; }

    int dlu_w = dlg->w;
    int dlu_h = dlg->h;

    int pw = dlu_w * 3 / 2;
    int ph = dlu_h * 13 / 8;

    dlg->header_h = (dlg->title[0] != '\0') ? 22 : 0;
    int total_h = ph + dlg->header_h;

    int max_w = scr_w - 16;
    int max_h = scr_h - 16;
    double scale = 1.0;
    if (pw > max_w || total_h > max_h) {
        double sx = (double)max_w / pw;
        double sy = (double)max_h / total_h;
        scale = (sx < sy) ? sx : sy;
        pw = (int)(pw * scale);
        ph = (int)(ph * scale);
        dlg->header_h = (int)(dlg->header_h * scale);
        total_h = ph + dlg->header_h;
    }

    dlg->w = pw;
    dlg->h = total_h;
    dlg->x = (scr_w - pw) / 2;
    dlg->y = (scr_h - total_h) / 2;

    for (int i = 0; i < dlg->num_controls; i++) {
        struct Control *c = &dlg->controls[i];
        int cx = (int)(c->x * 3 / 2 * scale);
        int cy = (int)(c->y * 13 / 8 * scale);
        int cw = (int)(c->w * 3 / 2 * scale);
        int ch = (int)(c->h * 13 / 8 * scale);

        c->screen_x = dlg->x + cx;
        c->screen_y = dlg->y + dlg->header_h + cy;
        c->w = cw;
        c->h = ch;
        c->open_h = ch;
    }
}

/* ---- Exported Dialog API (dialog.h) ---------------------------------------------------- */

int dlg_enable(u32 hwnd, int enable)
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);
    if (ctrl) {
        int was_disabled = !ctrl->enabled;
        ctrl->enabled = enable ? 1 : 0;
        return was_disabled;
    }
    return 0;
}

int dlg_show(u32 hwnd, int cmd)
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);
    if (ctrl) {
        int was_visible = ctrl->visible;
        ctrl->visible = (cmd != 0);
        return was_visible;
    }
    return 0;
}

void dlg_set_text(u32 hwnd, const char *s)
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);
    if (ctrl) {
        strncpy(ctrl->text, s ? s : "", sizeof(ctrl->text) - 1);
        ctrl->text[sizeof(ctrl->text) - 1] = '\0';
        return;
    }
    dlg = find_dialog(hwnd);
    if (dlg) {
        strncpy(dlg->title, s ? s : "", sizeof(dlg->title) - 1);
        dlg->title[sizeof(dlg->title) - 1] = '\0';
        return;
    }
}

void dlg_get_rect(u32 hwnd, int r[4])
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);
    if (ctrl) {
        r[0] = ctrl->screen_x;
        r[1] = ctrl->screen_y;
        r[2] = ctrl->screen_x + ctrl->w;
        r[3] = ctrl->screen_y + ctrl->h;
        return;
    }
    dlg = find_dialog(hwnd);
    if (dlg) {
        r[0] = dlg->x;
        r[1] = dlg->y;
        r[2] = dlg->x + dlg->w;
        r[3] = dlg->y + dlg->h;
        return;
    }
    r[0] = r[1] = r[2] = r[3] = 0;
}

void dlg_set_pos(u32 hwnd, int x, int y, int w, int h, u32 flags)
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);
    if (ctrl) {
        if (!(flags & 2 /* SWP_NOMOVE */)) {
            ctrl->screen_x = x;
            ctrl->screen_y = y;
        }
        if (!(flags & 1 /* SWP_NOSIZE */)) {
            ctrl->w = w;
            ctrl->h = h;
        }
        if (flags & 0x0040 /* SWP_SHOWWINDOW */) ctrl->visible = 1;
        if (flags & 0x0080 /* SWP_HIDEWINDOW */) ctrl->visible = 0;
        return;
    }
    dlg = find_dialog(hwnd);
    if (dlg) {
        if (!(flags & 2)) {
            int dx = x - dlg->x;
            int dy = y - dlg->y;
            dlg->x = x;
            dlg->y = y;
            for (int i = 0; i < dlg->num_controls; i++) {
                dlg->controls[i].screen_x += dx;
                dlg->controls[i].screen_y += dy;
            }
        }
        if (!(flags & 1)) {
            dlg->w = w;
            dlg->h = h;
        }
        return;
    }
}

void dlg_destroy(Cpu *c, u32 hwnd)
{
    (void)c;
    struct Dialog *dlg = find_dialog(hwnd);
    if (dlg) {
        dlg->ended = 1;
        dlg->result = 0;
    }
}

u32 dlg_send(Cpu *c, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    struct Dialog *dlg = NULL;
    struct Control *ctrl = find_ctrl(hwnd, &dlg);

    if (ctrl) {
        if (dlg_log_enabled()) plat_log("dlg_send ctrl 0x%x (id %u) msg 0x%04x wp 0x%x lp 0x%x", hwnd, ctrl->id, msg, wp, lp);
        switch (msg) {
            case 0x000C: /* WM_SETTEXT */
                if (lp) {
                    strncpy(ctrl->text, GSTR(lp), sizeof(ctrl->text) - 1);
                    ctrl->text[sizeof(ctrl->text) - 1] = '\0';
                } else {
                    ctrl->text[0] = '\0';
                }
                return 1;

            case 0x000D: /* WM_GETTEXT */
                if (lp && wp > 0) {
                    u32 len = (u32)strlen(ctrl->text);
                    if (len > wp - 1) len = wp - 1;
                    memcpy(G(lp), ctrl->text, len);
                    ((char *)G(lp))[len] = '\0';
                    return len;
                }
                return 0;

            case 0x000E: /* WM_GETTEXTLENGTH */
                return (u32)strlen(ctrl->text);

            case 0x000A: /* WM_ENABLE */
                ctrl->enabled = wp ? 1 : 0;
                return 0;

            case 0x00F0: /* BM_GETCHECK */
            case 0x00F2: /* BM_GETSTATE */
                return (u32)ctrl->check_state;

            case 0x00F1: /* BM_SETCHECK */
                ctrl->check_state = (int)wp;
                return 0;

            case 0x00F3: /* BM_SETSTATE */
                ctrl->is_pressed = wp ? 1 : 0;
                return 0;

            case 0x00F5: /* BM_CLICK */
                trigger_button_click(c, dlg, ctrl);
                return 0;

            /* Combobox / Listbox shared equivalents */
            case 0x0143: /* CB_ADDSTRING */
            case 0x0180: /* LB_ADDSTRING */
                if (ctrl->item_count < MAX_ITEMS && lp) {
                    int idx = ctrl->item_count++;
                    strncpy(ctrl->items[idx].text, GSTR(lp), sizeof(ctrl->items[0].text) - 1);
                    ctrl->items[idx].data = 0;
                    if (dlg_log_enabled()) plat_log("dlg_send CB_ADDSTRING id %u: added '%s' at idx %d", ctrl->id, ctrl->items[idx].text, idx);
                    return (u32)idx;
                }
                return (u32)-1;

            case 0x014A: /* CB_INSERTSTRING */
            case 0x0181: /* LB_INSERTSTRING */ {
                int idx = (int)wp;
                if (idx < 0 || idx >= ctrl->item_count) idx = ctrl->item_count;
                if (ctrl->item_count < MAX_ITEMS && lp) {
                    memmove(&ctrl->items[idx + 1], &ctrl->items[idx], (ctrl->item_count - idx) * sizeof(struct ListItem));
                    strncpy(ctrl->items[idx].text, GSTR(lp), sizeof(ctrl->items[0].text) - 1);
                    ctrl->items[idx].data = 0;
                    ctrl->item_count++;
                    return (u32)idx;
                }
                return (u32)-1;
            }

            case 0x0144: /* CB_DELETESTRING */
            case 0x0182: /* LB_DELETESTRING */ {
                int idx = (int)wp;
                if (idx >= 0 && idx < ctrl->item_count) {
                    memmove(&ctrl->items[idx], &ctrl->items[idx + 1], (ctrl->item_count - 1 - idx) * sizeof(struct ListItem));
                    ctrl->item_count--;
                    if (ctrl->cur_sel == idx) ctrl->cur_sel = -1;
                    else if (ctrl->cur_sel > idx) ctrl->cur_sel--;
                    return (u32)ctrl->item_count;
                }
                return (u32)-1;
            }

            case 0x014B: /* CB_RESETCONTENT */
            case 0x0184: /* LB_RESETCONTENT */
                ctrl->item_count = 0;
                ctrl->cur_sel = -1;
                ctrl->is_open = 0;
                ctrl->scroll_pos = 0;
                return 0;

            case 0x0146: /* CB_GETCOUNT */
            case 0x018B: /* LB_GETCOUNT */
                return (u32)ctrl->item_count;

            case 0x0147: /* CB_GETCURSEL */
            case 0x0188: /* LB_GETCURSEL */
                return (u32)ctrl->cur_sel;

            case 0x014E: /* CB_SETCURSEL */
            case 0x0186: /* LB_SETCURSEL */ {
                int idx = (int)wp;
                if (idx == -1) { ctrl->cur_sel = -1; return (u32)-1; }
                if (idx >= 0 && idx < ctrl->item_count) {
                    combo_select(ctrl, idx);
                    return (u32)idx;
                }
                return (u32)-1;
            }

            case 0x0148: /* CB_GETLBTEXT */
            case 0x0189: /* LB_GETTEXT */ {
                int idx = (int)wp;
                if (idx >= 0 && idx < ctrl->item_count && lp) {
                    strcpy((char *)G(lp), ctrl->items[idx].text);
                    return (u32)strlen(ctrl->items[idx].text);
                }
                return (u32)-1;
            }

            case 0x0149: /* CB_GETLBTEXTLEN */
            case 0x018A: /* LB_GETTEXTLEN */ {
                int idx = (int)wp;
                if (idx >= 0 && idx < ctrl->item_count) {
                    return (u32)strlen(ctrl->items[idx].text);
                }
                return (u32)-1;
            }

            case 0x0150: /* CB_GETITEMDATA */
            case 0x0199: /* LB_GETITEMDATA */ {
                int idx = (int)wp;
                if (idx >= 0 && idx < ctrl->item_count) {
                    return ctrl->items[idx].data;
                }
                return (u32)-1;
            }

            case 0x0151: /* CB_SETITEMDATA */
            case 0x019A: /* LB_SETITEMDATA */ {
                int idx = (int)wp;
                if (idx >= 0 && idx < ctrl->item_count) {
                    ctrl->items[idx].data = lp;
                    return 1;
                }
                return (u32)-1;
            }

            case 0x014C: /* CB_FINDSTRING */
            case 0x018F: /* LB_FINDSTRING */ {
                const char *str = lp ? GSTR(lp) : "";
                size_t slen = strlen(str);
                int start = (int)wp + 1;
                if (start < 0 || start >= ctrl->item_count) start = 0;
                for (int i = 0; i < ctrl->item_count; i++) {
                    int k = (start + i) % ctrl->item_count;
                    if (!strncasecmp(ctrl->items[k].text, str, slen)) return (u32)k;
                }
                return (u32)-1;
            }

            case 0x0158: /* CB_FINDSTRINGEXACT */
            case 0x01A2: /* LB_FINDSTRINGEXACT */ {
                const char *str = lp ? GSTR(lp) : "";
                int start = (int)wp + 1;
                if (start < 0 || start >= ctrl->item_count) start = 0;
                for (int i = 0; i < ctrl->item_count; i++) {
                    int k = (start + i) % ctrl->item_count;
                    if (!strcasecmp(ctrl->items[k].text, str)) return (u32)k;
                }
                return (u32)-1;
            }

            case 0x014D: /* CB_SELECTSTRING */ {
                u32 found = dlg_send(c, hwnd, 0x014C, wp, lp);
                if (found != (u32)-1) { ctrl->cur_sel = (int)found; return found; }
                return (u32)-1;
            }

            case 0x014F: /* CB_SHOWDROPDOWN */
                ctrl->is_open = wp ? 1 : 0;
                return 1;

            /* Trackbar / Slider messages */
            case 0x0400: /* TBM_GETPOS */
                return (u32)ctrl->slider_pos;
            case 0x0401: /* TBM_GETRANGEMIN */
                return (u32)ctrl->slider_min;
            case 0x0402: /* TBM_GETRANGEMAX */
                return (u32)ctrl->slider_max;
            case 0x0405: /* TBM_SETPOS */
                ctrl->slider_pos = (int)lp;
                if (ctrl->slider_pos < ctrl->slider_min) ctrl->slider_pos = ctrl->slider_min;
                if (ctrl->slider_pos > ctrl->slider_max) ctrl->slider_pos = ctrl->slider_max;
                return 0;
            case 0x0406: /* TBM_SETRANGE */
                ctrl->slider_min = (int)(lp & 0xFFFF);
                ctrl->slider_max = (int)((lp >> 16) & 0xFFFF);
                if (ctrl->slider_pos < ctrl->slider_min) ctrl->slider_pos = ctrl->slider_min;
                if (ctrl->slider_pos > ctrl->slider_max) ctrl->slider_pos = ctrl->slider_max;
                return 0;
            case 0x0407: /* TBM_SETRANGEMIN */
                ctrl->slider_min = (int)lp;
                return 0;
            case 0x0408: /* TBM_SETRANGEMAX */
                ctrl->slider_max = (int)lp;
                return 0;
            case 0x0414: /* TBM_SETTICFREQ */
                ctrl->slider_tic_freq = (int)wp;
                return 0;
            case 0x0415: /* TBM_SETPAGESIZE */
                ctrl->slider_page_size = (int)lp;
                return 0;
            case 0x0417: /* TBM_SETLINESIZE */
                ctrl->slider_line_size = (int)lp;
                return 0;

            default:
                if (dlg_log_enabled()) {
                    plat_log("dlg_send ctrl 0x%x msg 0x%x wp 0x%x lp 0x%x", hwnd, msg, wp, lp);
                }
                return 0;
        }
    }

    dlg = find_dialog(hwnd);
    if (dlg) {
        if (dlg->proc) {
            return guest_call(c ? c : cur_cpu, dlg->proc, 4, (u32[]){ hwnd, msg, wp, lp });
        }
    }

    return 0;
}

/* ---- Dialog Imports -------------------------------------------------------------------- */

static u32 imp_MessageBoxA(Cpu *c)
{
    u32 parent = ARG(0);
    u32 text_p = ARG(1);
    u32 cap_p = ARG(2);
    u32 type = ARG(3);

    const char *text = text_p ? GSTR(text_p) : "";
    const char *caption = cap_p ? GSTR(cap_p) : "Error";

    plat_log("MessageBoxA(hwnd 0x%x, type 0x%x): [%s] %s", parent, type, caption, text);

    if (dlg_stack_depth >= MAX_OPEN_DLGS) {
        return 1;
    }

    struct Dialog *dlg = &dlg_stack[dlg_stack_depth++];
    memset(dlg, 0, sizeof(*dlg));
    dlg->active = 1;
    dlg->is_msgbox = 1;
    dlg->msgbox_type = (int)(type & 0x0F);
    dlg->hwnd = DLG_HWND_BASE | ((dlg_stack_depth - 1) << 12);
    strncpy(dlg->title, caption, sizeof(dlg->title) - 1);
    dlg->header_h = 22;

    /* Word wrap text (max 360 px width -> 51 chars at 7 px adv) */
    const char *p = text;
    int num_lines = 0;
    while (*p && num_lines < 16) {
        while (*p == ' ') p++;
        if (!*p) break;
        int line_len = 0;
        const char *line_start = p;
        const char *last_space = NULL;
        while (*p && *p != '\n' && line_len < 50) {
            if (*p == ' ') last_space = p;
            p++;
            line_len++;
        }
        if (*p == '\n') {
            int len = (int)(p - line_start);
            if (len > 127) len = 127;
            strncpy(dlg->msgbox_lines[num_lines], line_start, len);
            dlg->msgbox_lines[num_lines][len] = '\0';
            num_lines++;
            p++;
        } else if (!*p) {
            int len = (int)(p - line_start);
            if (len > 127) len = 127;
            strncpy(dlg->msgbox_lines[num_lines], line_start, len);
            dlg->msgbox_lines[num_lines][len] = '\0';
            num_lines++;
        } else {
            if (last_space && last_space > line_start) {
                int len = (int)(last_space - line_start);
                if (len > 127) len = 127;
                strncpy(dlg->msgbox_lines[num_lines], line_start, len);
                dlg->msgbox_lines[num_lines][len] = '\0';
                num_lines++;
                p = last_space + 1;
            } else {
                int len = line_len;
                if (len > 127) len = 127;
                strncpy(dlg->msgbox_lines[num_lines], line_start, len);
                dlg->msgbox_lines[num_lines][len] = '\0';
                num_lines++;
            }
        }
    }
    if (num_lines == 0) {
        dlg->msgbox_lines[0][0] = '\0';
        num_lines = 1;
    }
    dlg->msgbox_num_lines = num_lines;

    struct { const char *label; int id; } btns[3];
    int nbtns = 0;
    u32 btn_style = type & 0x0F;
    if (btn_style == 1) {
        btns[0].label = "OK";     btns[0].id = 1;
        btns[1].label = "Cancel"; btns[1].id = 2;
        nbtns = 2;
    } else if (btn_style == 3) {
        btns[0].label = "Yes";    btns[0].id = 6;
        btns[1].label = "No";     btns[1].id = 7;
        btns[2].label = "Cancel"; btns[2].id = 2;
        nbtns = 3;
    } else if (btn_style == 4) {
        btns[0].label = "Yes";    btns[0].id = 6;
        btns[1].label = "No";     btns[1].id = 7;
        nbtns = 2;
    } else if (btn_style == 5) {
        btns[0].label = "Retry";  btns[0].id = 4;
        btns[1].label = "Cancel"; btns[1].id = 2;
        nbtns = 2;
    } else {
        btns[0].label = "OK";     btns[0].id = 1;
        nbtns = 1;
    }

    int max_line_w = 0;
    for (int i = 0; i < num_lines; i++) {
        int lw = (int)strlen(dlg->msgbox_lines[i]) * 7;
        if (lw > max_line_w) max_line_w = lw;
    }
    int title_w = (int)strlen(caption) * 7;
    if (title_w > max_line_w) max_line_w = title_w;

    int btn_w = 70, btn_h = 22, btn_gap = 16;
    int total_btns_w = nbtns * btn_w + (nbtns - 1) * btn_gap;
    int content_w = (max_line_w > total_btns_w) ? max_line_w : total_btns_w;
    int box_w = content_w + 48;
    if (box_w < 260) box_w = 260;
    if (box_w > 440) box_w = 440;

    int text_h = num_lines * 14;
    int box_h = dlg->header_h + 16 + text_h + 16 + btn_h + 16;

    const u16 *px = NULL;
    int scr_w = 640, scr_h = 480, is565 = 1;
    win_last_frame(&px, &scr_w, &scr_h, &is565);
    if (scr_w <= 0 || scr_h <= 0) { scr_w = 640; scr_h = 480; is565 = 1; }

    dlg->x = (scr_w - box_w) / 2;
    dlg->y = (scr_h - box_h) / 2;
    dlg->w = box_w;
    dlg->h = box_h;

    int btns_start_x = dlg->x + (box_w - total_btns_w) / 2;
    int btns_y = dlg->y + box_h - 16 - btn_h;

    dlg->num_controls = nbtns;
    for (int i = 0; i < nbtns; i++) {
        struct Control *cbtn = &dlg->controls[i];
        memset(cbtn, 0, sizeof(*cbtn));
        cbtn->hwnd = dlg->hwnd + (i + 1);
        cbtn->id = btns[i].id;
        cbtn->type = CTRL_BUTTON;
        cbtn->btn_type = (i == 0) ? BTN_DEFPUSH : BTN_PUSH;
        cbtn->screen_x = btns_start_x + i * (btn_w + btn_gap);
        cbtn->screen_y = btns_y;
        cbtn->w = btn_w;
        cbtn->h = btn_h;
        cbtn->enabled = 1;
        cbtn->visible = 1;
        strncpy(cbtn->text, btns[i].label, sizeof(cbtn->text) - 1);
    }
    dlg->focused_ctrl = 0;

    int (*prev_hook)(const SDL_Event *) = win_event_hook;
    win_event_hook = dlg_event_hook;

    while (!dlg->ended) {
        plat_pump();
        render_all_dialogs();
        SDL_Delay(15);
    }

    win_event_hook = prev_hook;
    u32 result = dlg->result;
    dlg->active = 0;
    dlg_stack_depth--;

    plat_log("MessageBoxA: result %u", result);
    return result;
}

static u32 imp_DialogBoxParamA(Cpu *c)
{
    u32 hinst = ARG(0);
    u32 tmpl = ARG(1);
    u32 parent = ARG(2);
    u32 proc = ARG(3);
    u32 param = ARG(4);
    (void)hinst; (void)parent;

    if (tmpl <= 0xFFFF) {
        plat_log("DialogBoxParamA: dialog resource ID #%u (proc 0x%08x, param 0x%x)", tmpl, proc, param);
    } else {
        plat_log("DialogBoxParamA: dialog template '%s' (proc 0x%08x, param 0x%x)", GSTR(tmpl), proc, param);
    }

    if (dlg_stack_depth >= MAX_OPEN_DLGS) {
        plat_log("DialogBoxParamA: max dialog stack depth reached!");
        return 1;
    }

    struct Dialog *dlg = &dlg_stack[dlg_stack_depth++];
    memset(dlg, 0, sizeof(*dlg));
    dlg->active = 1;
    dlg->res_id = (tmpl <= 0xFFFF) ? tmpl : 0;
    dlg->proc = proc;
    dlg->init_param = param;
    dlg->hwnd = DLG_HWND_BASE | ((dlg_stack_depth - 1) << 12);

    u32 tmpl_addr = 0, tmpl_size = 0;
    if (!find_pe_dialog_resource(tmpl, &tmpl_addr, &tmpl_size)) {
        plat_log("DialogBoxParamA: template %u not found in PE resources", tmpl);
        dlg->active = 0;
        dlg_stack_depth--;
        return 1;
    }

    if (!parse_dialog_template(dlg, tmpl_addr, tmpl_size)) {
        plat_log("DialogBoxParamA: failed to parse dialog template %u", tmpl);
        dlg->active = 0;
        dlg_stack_depth--;
        return 1;
    }

    layout_dialog(dlg);

    dlg->focused_ctrl = -1;
    for (int i = 0; i < dlg->num_controls; i++) {
        if (is_focusable(&dlg->controls[i])) {
            dlg->focused_ctrl = i;
            break;
        }
    }
    u32 first_ctrl_hwnd = (dlg->focused_ctrl >= 0) ? dlg->controls[dlg->focused_ctrl].hwnd : 0;

    if (dlg->proc) {
        if (dlg_log_enabled()) {
            plat_log("dlg 0x%x call WM_INITDIALOG (first_ctrl 0x%x, param 0x%x)", dlg->hwnd, first_ctrl_hwnd, param);
        }
        guest_call(c, dlg->proc, 4, (u32[]){ dlg->hwnd, 0x0110 /* WM_INITDIALOG */, first_ctrl_hwnd, param });
    }
    /* Preferences (dialog 101): "Processor Select" only picks 1998 CPU-specific renderer loops,
       meaningless for recompiled code: hide it and keep it on AutoDetect (item 0) */
    if (dlg->res_id == 101) {
        struct Control *label = find_ctrl_by_id(dlg, 1111), *combo = find_ctrl_by_id(dlg, 1023);
        struct Control *ctl = find_ctrl_by_id(dlg, 1099), *audio = find_ctrl_by_id(dlg, 1100);
        if (label && combo && ctl && audio) {
            int up = ctl->screen_y - label->screen_y;  /* move the two buttons into the gap */
            label->visible = combo->visible = 0;
            combo->cur_sel = 0;
            ctl->screen_y -= up;
            audio->screen_y -= up;
        }
    }
    /* the pause bar's first button is Abort: start on Resume so Enter cannot end the mission */
    for (int i = 0; i < dlg->num_controls; i++)
        if (is_focusable(&dlg->controls[i]) && !strncasecmp(dlg->controls[i].text, "Resume", 6)) {
            dlg->focused_ctrl = i;
            break;
        }

    int (*prev_hook)(const SDL_Event *) = win_event_hook;
    win_event_hook = dlg_event_hook;

    while (!dlg->ended) {
        plat_pump();
        render_all_dialogs();
        SDL_Delay(15);
    }

    win_event_hook = prev_hook;
    u32 res = dlg->result;
    dlg->active = 0;
    dlg_stack_depth--;

    if (dlg_log_enabled()) {
        plat_log("DialogBoxParamA: tmpl %u finished with result %d", tmpl, (int)res);
    }
    return res;
}

static u32 imp_EndDialog(Cpu *c)
{
    u32 hdlg = ARG(0);
    u32 res = ARG(1);
    struct Dialog *dlg = find_dialog(hdlg);
    if (dlg) {
        dlg->ended = 1;
        dlg->result = res;
    }
    if (dlg_log_enabled()) {
        plat_log("EndDialog(hdlg 0x%x, result %d)", hdlg, (int)res);
    }
    return 1;
}

static u32 imp_GetDlgItem(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    if (dlg_log_enabled()) plat_log("GetDlgItem(dlg 0x%x, id %d) -> ctrl 0x%x", dlg_hwnd, id, ctrl ? ctrl->hwnd : 0);
    return ctrl ? ctrl->hwnd : 0;
}

static u32 imp_SetDlgItemTextA(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    u32 str_p = ARG(2);
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    if (ctrl) {
        if (str_p) {
            strncpy(ctrl->text, GSTR(str_p), sizeof(ctrl->text) - 1);
            ctrl->text[sizeof(ctrl->text) - 1] = '\0';
        } else {
            ctrl->text[0] = '\0';
        }
        return 1;
    }
    return 0;
}

static u32 imp_GetDlgItemTextA(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    u32 buf_p = ARG(2);
    int max_cch = (int)ARG(3);
    if (!buf_p || max_cch <= 0) return 0;
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    if (ctrl) {
        int len = (int)strlen(ctrl->text);
        if (len > max_cch - 1) len = max_cch - 1;
        memcpy(G(buf_p), ctrl->text, (size_t)len);
        ((char *)G(buf_p))[len] = '\0';
        return (u32)len;
    }
    ((char *)G(buf_p))[0] = '\0';
    return 0;
}

static u32 imp_SendDlgItemMessageA(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    u32 msg = ARG(2);
    u32 wp = ARG(3);
    u32 lp = ARG(4);
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    if (ctrl) {
        return dlg_send(c, ctrl->hwnd, msg, wp, lp);
    }
    return 0;
}

static u32 imp_CheckDlgButton(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    u32 check = ARG(2);
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    if (ctrl) {
        ctrl->check_state = (int)check;
        return 1;
    }
    return 0;
}

static u32 imp_IsDlgButtonChecked(Cpu *c)
{
    u32 dlg_hwnd = ARG(0);
    int id = (int)ARG(1);
    struct Dialog *dlg = find_dialog(dlg_hwnd);
    struct Control *ctrl = find_ctrl_by_id(dlg, id);
    return ctrl ? (u32)ctrl->check_state : 0;
}

/* ---- Import table ---------------------------------------------------------------------- */

const struct ImpDef imp_dialog[] = {
    IMPORT(MessageBoxA, 4),
    IMPORT(DialogBoxParamA, 5),
    IMPORT(EndDialog, 2),
    IMPORT(GetDlgItem, 2),
    IMPORT(SetDlgItemTextA, 3),
    IMPORT(GetDlgItemTextA, 4),
    IMPORT(SendDlgItemMessageA, 5),
    IMPORT(CheckDlgButton, 3),
    IMPORT(IsDlgButtonChecked, 2),
};
IMPORT_TABLE(dialog);
