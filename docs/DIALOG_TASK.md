# Task: the game's dialog boxes, drawn in the game's own style

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2.
Windows functions it imports are implemented by us in `recomp/platform/`. On Windows, the
game shows standard grey Windows dialog boxes: Preferences, Controller Configuration, Audio,
the in-mission pause bar (Esc: Resume / Abort / Audio / Controls), Load/Save game, and a
"Do You Really Want To Quit?" message box. **The game's own code drives these dialogs** (its
dialog procedure fills the controls, reacts to clicks and saves the settings). We must
provide the Windows dialog machinery and draw the dialogs so they look like part of the game.

You write **`recomp/platform/dialog.c`** (replace the placeholder that is there now). You may
also edit `recomp/platform/u32_misc.c`, but only to delete its old placeholder dialog
functions (MessageBoxA, DialogBoxParamA, EndDialog, GetDlgItem, SetDlgItemTextA,
GetDlgItemTextA, SendDlgItemMessageA, CheckDlgButton, IsDlgButtonChecked, the FakeDlgItem
code) and their lines in its `imp_u32_misc` table. Change nothing else.

## Read first (with your file-reading tool)

- `recomp/platform/host.h`: guest memory (`MEM`, `rd32`, `wr32`, `GSTR`, `galloc`), `ARG(n)`,
  `IMPORT`, `IMPORT_TABLE`, `guest_call`, `plat_log`.
- `recomp/platform/dialog.h`: functions you must export (u32_win.c calls them when the game
  uses SendMessageA, EnableWindow, ShowWindow, SetWindowTextA, SetWindowPos, GetWindowRect or
  DestroyWindow on a dialog or a control).
- `recomp/platform/win.h`: `win_last_frame`, `win_show`, `win_event_hook`, `plat_pump`.
- `recomp/platform/third_party/font8x8_basic.h`: the bitmap font to use (include it only in
  dialog.c; `font8x8_basic[ch][row]`, bit 0 of each row byte is the leftmost pixel).
- `build/dialogs.txt`: every dialog in the game, decoded: control class, id, text, position
  and size in dialog units, style. Design against these.
- `build/ref_menu.png`: the game's main menu with the Preferences dialog open (on Windows).
  Your dialogs must match the **menu behind it**, not the grey dialog.

## Imports to implement (table `imp_dialog`, end with `IMPORT_TABLE(dialog);`)

`MessageBoxA` (4 args), `DialogBoxParamA` (5), `EndDialog` (2), `GetDlgItem` (2),
`SetDlgItemTextA` (3), `GetDlgItemTextA` (4), `SendDlgItemMessageA` (5), `CheckDlgButton` (3),
`IsDlgButtonChecked` (2).

## How it must work

1. **Templates.** `DialogBoxParamA(hinst, tmpl, parent, proc, param)`: `tmpl` <= 0xffff is a
   resource id, otherwise a guest string name. Find the RT_DIALOG (type 5) resource in the
   loaded EXE in guest memory: image base 0x400000, PE header at `0x400000 + rd32(0x40003c)`,
   resource directory = data directory 2 of the optional header (RVA, add 0x400000). Walk the
   three directory levels (type, name/id, language: take the first). Parse DLGTEMPLATEEX
   (starts with words 1, 0xFFFF) and plain DLGTEMPLATE: menu, class, title, font (if DS_SETFONT
   0x40), then each item DWORD-aligned: class is 0xFFFF + atom (0x80 BUTTON, 0x81 EDIT,
   0x82 STATIC, 0x83 LISTBOX, 0x84 SCROLLBAR, 0x85 COMBOBOX) or a UTF-16 string
   (`msctls_trackbar32`), then title, then extra-data count + bytes. Strings are UTF-16LE;
   convert to Latin-1. Item ids are 16-bit in DLGTEMPLATE, 32-bit in DLGTEMPLATEEX.
2. **Geometry.** Convert dialog units to game pixels with x * 3 / 2 and y * 13 / 8; if the
   dialog would not fit in the game frame (normally 640x480, minus 8 px margin), scale
   everything down to fit. Always centre the dialog in the frame.
3. **Handles.** Give each dialog and each control a unique hwnd from your own range (for
   example 0x00DA0000 + dialog*256 + control). `dlg_owns` recognises them. Controls without
   an id (id 0xFFFF / -1) still need an hwnd.
4. **Modal loop.** Create the dialog and controls, then call the dialog procedure:
   `guest_call(c, proc, 4, (u32[]){hwnd, WM_INITDIALOG 0x110, first control hwnd, param})`.
   Then loop until EndDialog was called for this dialog: `plat_pump()` (it delivers SDL
   events to your `win_event_hook`, set it while a dialog is open and restore the previous
   hook after), draw, `win_show(...)`, `SDL_Delay(15)`. Return the EndDialog result. Dialogs
   nest: the pause bar opens Audio from inside its procedure, so keep a stack and draw all open
   dialogs (only the top one gets input). If no frame exists yet, draw on black.
   Messages to the dialog procedure always go through `guest_call` with the `Cpu *c` of the
   DialogBoxParamA call. The return value of the procedure can be ignored.
5. **Controls** (state kept by you, changed by the game's messages and by the player):
   - BUTTON, low 4 style bits: 0 push, 1 default push, 2 check box, 3 auto check box,
     4 radio, 9 auto radio, 7 group box (a frame with a title, not clickable). Clicking a push
     button or check/radio sends `WM_COMMAND` (0x111) with wParam = id | (BN_CLICKED 0 << 16),
     lParam = control hwnd. Auto check boxes toggle first; auto radio buttons check themselves
     and uncheck the others in their group (a group starts at a control with WS_GROUP
     0x00020000 and runs to the next one). Messages: BM_GETCHECK 0xF0, BM_SETCHECK 0xF1,
     BM_GETSTATE 0xF2, BM_SETSTATE 0xF3, BM_CLICK 0xF5; CheckDlgButton/IsDlgButtonChecked.
   - STATIC: text; style low bits 1 centre, 2 right; WS_BORDER 0x00800000 or SS_SUNKEN 0x1000
     draws a box; SS_CENTERIMAGE 0x200 centres vertically. With SS_NOTIFY 0x100 a click sends
     WM_COMMAND id | (STN_CLICKED 0 << 16) (the controller key boxes use this).
   - COMBOBOX (all styles behave as a drop-down list): CB_ADDSTRING 0x143, CB_DELETESTRING
     0x144, CB_GETCOUNT 0x146, CB_GETCURSEL 0x147, CB_GETLBTEXT 0x148, CB_GETLBTEXTLEN 0x149,
     CB_INSERTSTRING 0x14A, CB_RESETCONTENT 0x14B, CB_FINDSTRING 0x14C, CB_SELECTSTRING 0x14D,
     CB_SETCURSEL 0x14E, CB_SHOWDROPDOWN 0x14F, CB_GETITEMDATA 0x150, CB_SETITEMDATA 0x151,
     CB_FINDSTRINGEXACT 0x158; errors return CB_ERR (-1). The template height is the height of
     the opened list. Clicking opens the list; picking an item sets the selection and sends
     WM_COMMAND id | (CBN_SELCHANGE 1 << 16). Mouse wheel scrolls a long list.
   - LISTBOX: the LB_ equivalents (ADDSTRING 0x180, INSERTSTRING 0x181, DELETESTRING 0x182,
     RESETCONTENT 0x184, SETCURSEL 0x186, GETCURSEL 0x188, GETTEXT 0x189, GETTEXTLEN 0x18A,
     GETCOUNT 0x18B, FINDSTRING 0x18F, GETITEMDATA 0x199, SETITEMDATA 0x19A,
     FINDSTRINGEXACT 0x1A2), LBN_SELCHANGE 1 and LBN_DBLCLK 2 notifications, wheel scrolling.
   - `msctls_trackbar32` (slider): range default 0..100. TBM_GETPOS 0x400, TBM_GETRANGEMIN
     0x401, TBM_GETRANGEMAX 0x402, TBM_SETPOS 0x405 (lParam = pos), TBM_SETRANGE 0x406
     (lParam = min | max << 16), TBM_SETRANGEMIN 0x407, TBM_SETRANGEMAX 0x408, TBM_SETTICFREQ
     0x414, TBM_SETPAGESIZE 0x415, TBM_SETLINESIZE 0x417 (store, no visible effect needed).
     Dragging sends WM_HSCROLL (0x114) to the dialog procedure: wParam = TB_THUMBTRACK 5 |
     pos << 16 while dragging, then TB_ENDTRACK 8; lParam = control hwnd.
   - EDIT: one line of text; click to focus, type with SDL_TEXTINPUT (call SDL_StartTextInput
     while one has focus), Backspace; sends WM_COMMAND id | (EN_CHANGE 0x300 << 16).
   - Any control: WM_SETTEXT 0xC, WM_GETTEXT 0xD (copy into guest buffer, NUL-terminated,
     return length), WM_GETTEXTLENGTH 0xE, WM_ENABLE; disabled or hidden controls ignore
     input; hidden controls (ShowWindow SW_HIDE 0) are not drawn. Controls created with no
     WS_VISIBLE (0x10000000) start hidden; WS_DISABLED 0x08000000 starts disabled.
6. **Keyboard and gamepad-style navigation.** Esc sends WM_COMMAND IDCANCEL (2) (with
   lParam 0); Enter activates the focused push button, otherwise the default push button
   (style 1), otherwise sends IDOK (1). Tab / Down / Up move the focus between visible,
   enabled, clickable controls (in template order); Space activates the focused control;
   Left / Right change a focused slider by 1/10 of its range (with WM_HSCROLL TB_ENDTRACK)
   or a focused combo box selection.
7. **MessageBoxA(hwnd, text, caption, type)**: same look and loop. A caption bar with the
   caption, the text word-wrapped (max 360 px wide), buttons by `type & 0xF`: 0 OK (returns
   IDOK 1), 1 OK/Cancel (1/2), 3 Yes/No/Cancel (6/7/2), 4 Yes/No (6/7), 5 Retry/Cancel (4/2).
   Esc chooses Cancel, or No if there is no Cancel; Enter chooses the first button.
8. **EndDialog(hdlg, result)** marks that dialog finished (the loop returns `result`).
   `dlg_destroy` (DestroyWindow) also ends it. When a dialog closes, send nothing else.
9. Set `FCOP_DLG_LOG=1` to log: each dialog opened (id, title), every notification sent to
   the dialog procedure, EndDialog results, MessageBoxA text and answer. Always log one line
   per MessageBoxA and DialogBoxParamA call even without it.

## Look: like part of the game (see build/ref_menu.png)

Draw into a 16-bit buffer the size of the game frame (same pixel format as the last frame:
565 if `is565`, else 555) and show it with `win_show(buf, w, h, w * 2, 16, rmask, gmask,
bmask)`. Everything is drawn with your own small helpers (fill rect, rounded rect outline,
blend, text).

- Behind the dialog: the last game frame darkened to about 40 % brightness.
- Dialog panel: dark navy (about #0A1236) at 90 % opacity, with a 2 px bright blue border
  (#2E7BFF) and a 1 px darker inner line (#123A8C), slightly rounded corners. A title (if
  any) in a header strip at the top: slightly lighter navy (#16245E), text centred.
- Push buttons: **pill-shaped** like NEW GAME / QUIT in the menu: rounded ends (radius = half
  the height), dark fill (#0C1A4A), 2 px bright blue outline (#2E7BFF), text centred in
  upper case with 1 px extra letter spacing when it fits, colour #B8D4FF. Hover / focus:
  fill #18307A, outline #6FB0FF, text white. Pressed: fill #2A50C0. Disabled: outline
  #34466E, text #5A6A90.
- Check boxes and radio buttons: a 10 px square (radio: a diamond or round) with a blue
  outline; checked = filled bright cyan-blue (#4FA8FF) inner square; label to the right.
- Group boxes: 1 px blue line (#2E7BFF at 70 %) with the title over the top line.
- Combo boxes: dark field (#050A20) with blue outline, text left, a small down-pointing
  triangle on the right; the open list is a navy panel with a blue outline, the hovered or
  selected item highlighted with #1E4FC8 and white text.
- Sliders: a thin 2 px track (#2E7BFF), a pill-shaped thumb (#B8D4FF fill, blue outline);
  the part of the track left of the thumb a brighter cyan (#4FA8FF).
- Static text: #B8D4FF; boxed statics (controller keys): dark field with blue outline.
- Text: font8x8 at scale 1 with 7 px advance (glyphs use 7 columns); if a string does not
  fit its control, use a 6 px advance; if it still does not fit, clip it. Vertically centre
  text in buttons and boxes. Give text a 1 px dark shadow (#000818) for readability.

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 14 --input "9000:click@156,306" --png build/shots/quit.png

builds, starts the game, clicks QUIT in the main menu at 9 s (it calls MessageBoxA "Do You
Really Want To Quit?") and saves the last frame shown as a PNG, which you can open with
your file-reading tool to look at your work. Useful inputs (game pixels, times in ms):

- Quit box: `9000:click@156,306`, then `11000:click@<x,y of your No button>` and the game
  must keep running (the main menu returns).
- Preferences: `9000:click@156,246` (dialog 101; Controller Configuration opens 113, Audio 152).
- Pause bar in a mission: `9000:click@484,303 26000:Escape` (dialog 166), use `--seconds 32`.
- Keys: `12000:Down 12500:Return` etc. (SDL key names; `Keypad_8*2000` holds a key 2 s).

Check every dialog listed above looks right and that OK / Cancel / Esc close it. Look at
your PNGs critically (alignment, text clipping, colours) and fix what looks wrong.

## Rules

- Plain C99 + SDL2 + POSIX. No other libraries. Helpers `static` inside dialog.c.
- Read files with your file-reading tool, never with shell commands. Every shell command
  except the test command above is blocked; if an action is denied, continue.
- No empty stubs: every message listed above must really work.

## Final reply

A short list: what works, what is partial, and the test inputs you used.
