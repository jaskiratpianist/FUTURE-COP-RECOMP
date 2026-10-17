# Task: other resolutions and fullscreen (a virtual desktop)

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2;
the Windows functions it imports are implemented in `recomp/platform/`. Read
`recomp/platform/host.h` and `recomp/platform/win.h` first (with your file-reading tool).

## The problem

The player picks a resolution (320x240 ... 1600x1200) and Fullscreen in the game's
Preferences dialog; the game saves them in our registry (`registry.txt` in the game folder)
and uses them when a mission starts. The menus always run at 640x480.

- **Windowed** (FullScreen off): on Windows, DirectDraw's primary surface is the *whole
  desktop*. The game sizes its window to the chosen resolution and draws into the primary
  surface at the window's position (it adds the client origin; `ClientToScreen` is the
  identity here, the window sits at 0,0). Our platform claims the desktop is 640x480 (GetSystemMetrics,
  SystemParametersInfoA SPI_GETWORKAREA, GetDeviceCaps HORZRES/VERTRES, DirectDraw's default
  display size `dd->width/height`), so the primary surface is 640x480 and at 800x600 the game
  writes past its end and crashes (the registry is set to 800x600 windowed right now, so
  starting a mission crashes: that is your first test).
- **Fullscreen**: the game calls `SetCooperativeLevel` with DDSCL_FULLSCREEN (1) |
  DDSCL_EXCLUSIVE (0x10) and `SetDisplayMode(w, h, bpp)`; the primary surface then has the
  mode's size. We ignore both: nothing becomes fullscreen.

## What to implement

1. **Virtual desktop of DESKTOP_W x DESKTOP_H** (win.h, 1600x1200) everywhere the desktop
   size is reported: GetSystemMetrics SM_CXSCREEN/SM_CYSCREEN (and SM_CXFULLSCREEN/
   SM_CYFULLSCREEN, SM_CXMAXIMIZED...), SPI_GETWORKAREA, GetDeviceCaps HORZRES/VERTRES,
   DirectDraw `GetDisplayMode` and the default `dd->width/height` (ddraw.c around line 1798)
   while no display mode is set. Keep 16-bit 565 as the desktop format.
2. **Windowed presenting**: the primary surface is desktop-sized. When the primary is
   presented (the two `win_present` calls in ddraw.c), present only the main window's
   client rectangle: `main_win->x, y, w, h` (win.h `struct Win`), clipped to the surface,
   i.e. pass `pixels + y * pitch + x * 2` with width w and height h and the surface pitch.
   If there is no main window yet, present 640x480 from 0,0.
3. **Display modes**: `SetDisplayMode(w, h, bpp)` sets `dd->width/height/bpp` (surfaces
   created afterwards, including the primary, use it) and the whole primary is presented;
   `RestoreDisplayMode` returns to the desktop size. EnumDisplayModes must list all the modes
   the game offers: 320x240 400x300 480x360 512x384 640x480 800x600 960x720 1024x768
   1120x840 1280x960 1600x1200 (16 bit, and 8 bit if the code already lists 8 bit modes).
4. **Fullscreen**: implement `win_set_fullscreen(int on)` in u32_win.c with
   `SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)` (remember the
   state if the SDL window does not exist yet and apply it when it is created). Call it from
   DirectDraw `SetCooperativeLevel`: DDSCL_FULLSCREEN set means on, DDSCL_NORMAL (8) means
   off. The picture keeps its aspect ratio (u32_win.c already calls
   `SDL_RenderSetLogicalSize` with the frame size, which letterboxes and maps mouse
   coordinates back to game pixels: keep that). Also let **Alt+Enter** toggle fullscreen in
   u32_win.c's SDL event handling (host only: do not pass that key press to the game).
5. **Window size** (windowed): the SDL window follows the presented frame size (640x480 in
   menus, the chosen resolution in missions), but never larger than the usable display
   (`SDL_GetDisplayUsableBounds`): scale it down keeping the aspect ratio. Make the SDL
   window resizable (`SDL_WINDOW_RESIZABLE`); after the player resizes it, keep their size.
6. Log one line when the display mode, fullscreen state or presented size changes.

## Files

You may edit `recomp/platform/ddraw.c`, `recomp/platform/u32_win.c` and
`recomp/platform/gdi_misc.c` (GetDeviceCaps). Nothing else (not win.h, host.h, core.c,
dialog.c). Read the code you change carefully first: these files were written by another
worker and work today at 640x480; keep the menus, dialogs and the 640x480 missions working.

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 32 --input "9000:click@484,303" --png build/shots/mission.png

starts the first mission at 9 s (registry: 800x600 windowed, so it must not crash and the
PNG must be an 800x600 mission picture). More inputs (game pixels, ms):

- Preferences: `9000:click@156,246`; the resolution list opens with `10500:click@200,142`,
  its items are about 16 px apart starting near y 169 (320x240) — 640x480 is at y 233,
  800x600 at y 249, 1024x768 at y 281; the Fullscreen check box is at `352,148`; OK is at
  `150,347`. After OK the settings are saved when the game exits (end of the test run), so
  change them in one run and start a mission in the next one.
- Fullscreen check: after a fullscreen run the log must show your fullscreen line.
- `12000:Return`-style key names; `--png` saves the last frame shown.

Check: 640x480 windowed (menus, mission), 800x600 windowed, 1024x768 windowed, 800x600
fullscreen. **Finish with the registry set back to 640x480, FullScreen off** (use the
Preferences dialog in a last test run).

## Rules

- Plain C99 + SDL2 + POSIX. Helpers `static`. No empty stubs.
- Read files with your file-reading tool, never with shell commands. Every shell command
  except the test command is blocked; if an action is denied, continue.

## Final reply

What works, what is partial, and the tests you ran.
