# Open issues (OpenGL renderer)

Found by the review of the OpenGL branch (0445841..b69f478) and not fixed yet. Fixed from the
same review: mouse coordinates in OpenGL mode, the dialog buffer overflow above 1024x768, the
THRASH thunk leak.

## 1. GL calls from the video thread

- Where: video.c:144. `present_video_frame` runs on a video pthread (video.c:509) and calls
  `win_last_frame` only to learn whether the format is 565. If a THRASH frame was the last thing
  shown (`thrash_frame_last` = 1), `win_last_frame` calls `glp_read_frame16`
  (u32_win.c, gl_present.c), which makes GL calls on a thread that has no current context,
  while the main thread may be drawing. It also races on `last_px` and `thrash_frame_last`.
- When: a video starts right after mission frames with no 16-bit frame in between (end of
  mission, credits, a fly-over). Can crash on some drivers, or fail silently.
- Not checked: whether the game ever starts a video in that state.
- Fix: read the GL frame back only on the context's thread (`glp_thread`); give video.c a
  cheap `win_last_format()` that never touches GL.

## 2. Hardware mode reported although OpenGL is unavailable

- Where: gdi_misc.c writes `Group` = `OpenGL`. When GL fails, `thrash_load` (thrash_gl.c)
  refuses and missions fall back to software, but `RegQueryValueExA` (gdi_misc.c) still
  answers `OpenGL` unless `FCOP_RENDERER` is set.
- Effect: the game keeps its hardware-mode UI: the Preferences dialog has no Fullscreen
  checkbox, and its resolution list comes from THRASH `about()`, which is never called in this
  case, so the list can be empty.
- When: machines without GL 3.3 (old Intel GPUs, VMs, some Linux setups).
- Fix: set a `gl_failed` flag in `ensure_window_sdl`/`glp_init` and answer `Software` for
  `Group` when it is set. Check first whether the game reads `Group` (0x45acae) before
  CreateWindowExA; if so, probe GL earlier.

## 3. Freed SDL window still used by gl_present.c

- Where: DestroyWindow of the main window (u32_win.c) destroys the SDL window, but
  `glp_window` and `glp_ctx` (gl_present.c) stay alive.
- When: any frame shown after that (a quit box drawn after DestroyWindow, or `main_win`
  reassigned to another window and shown) calls `SDL_GL_GetDrawableSize`/`SDL_GL_SwapWindow`
  on freed memory. A new window would also get no context, because `glp_init` returns early
  when `glp_ctx` is set.
- Fix: a `glp_shutdown()` called when the destroyed window is `glp_window` (delete the context,
  reset the GL object state), or rebind with `SDL_GL_MakeCurrent(new_window, ctx)` and update
  `glp_window`.
