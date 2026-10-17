/* Dialog boxes and message boxes (dialog.c): the game's own dialogs from its RT_DIALOG
 * resources, drawn by us over the last game frame in the game's style. Windows the game
 * gets from here (dialogs and their controls) are not windows of u32_win.c: the USER32
 * window functions there hand them over with the calls below. */
#ifndef FCOP_DIALOG_H
#define FCOP_DIALOG_H
#include "host.h"

int dlg_owns(u32 hwnd);                                    /* a dialog or one of its controls */
u32 dlg_send(Cpu *c, u32 hwnd, u32 msg, u32 wp, u32 lp);   /* SendMessageA / PostMessageA */
int dlg_enable(u32 hwnd, int enable);                      /* EnableWindow: returns 1 if it was disabled */
int dlg_show(u32 hwnd, int cmd);                           /* ShowWindow: returns 1 if it was visible */
void dlg_set_text(u32 hwnd, const char *s);                /* SetWindowTextA */
void dlg_get_rect(u32 hwnd, int r[4]);                     /* GetWindowRect: left, top, right, bottom (game pixels) */
void dlg_set_pos(u32 hwnd, int x, int y, int w, int h, u32 flags);  /* SetWindowPos (SWP_NOMOVE 2, SWP_NOSIZE 1 honoured) */
void dlg_destroy(Cpu *c, u32 hwnd);                        /* DestroyWindow */

#endif
