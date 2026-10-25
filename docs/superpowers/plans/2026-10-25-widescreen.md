# Widescreen (Hor+) and native resolution

Goal: missions fill the whole screen at the display's real resolution and aspect (16:9,
21:9, ...), with more of the world visible at the sides (Hor+), nothing stretched, and the HUD
undistorted. Both hardware renderers (OpenGL/THRASH and 3Dfx/Glide). Menus and videos stay 4:3.

## How the game draws (found 2026-10-25)

- Everything is drawn in a fixed virtual 4:3 screen of 2560x1920 units (int16). The list
  walkers convert to pixels per resolution index: 0x4393e0 (Glide), 0x456270 (THRASH), via
  switch tables 0x43a168 / 0x456e00 (index 9 = 1280x960: units / 2).
- Projection is integer, PS1 style: `sx = cx + x * 4096 / z`, `sy = 960 - y * 4096 / z`,
  cx = 0x500 (0x280 per view in split screen, flag 0x511f54). Clip flags against 0..2*cx
  and 0..0x780. Three shared functions:
  - 0x453740(out, x, y, z): terrain vertex (called from 0x4534f0, 5x5 blocks from 0x453360)
  - 0x457340(points, flags, n): object points {x, y, z, _} s16, 8 bytes each, in place
  - 0x457140(p): sprite {x, y, z, _, w, h} s16
  Other `0x10000000` divisions (0x426630, 0x43ae70, 0x443fe0) not checked yet.
- Display list: 0x38-byte records, bump-allocated from 0x499b0c (pointer 0x511dc0), sorted
  into depth buckets by 0x40ca10. Buckets are chained by dummy nodes (type byte 0), farthest
  first. Bucket 0 (depth key 0) is the HUD; its node is `[0x499ad0]` and it is drawn last.
- Record: +0 next, +4 type (bits 0-2: 1..4 = polygon with that many vertices in 2560x1920
  units, 5 = sprite in 640x480 units, 6 = sprite in game pixels; 0x80 = textured, sprite size
  from the texture rectangle), +7 texture page, +8.. UV bytes, +0x28.. s16 x/y pairs.
  Page 0xd with type 6 is the font (special colour handling in both walkers, keyed on the
  original type), so records must not be rewritten into other types.

## Approach (spike works, OpenGL, `FCOP_WIDE=<aspect>`)

- Scale camera-space x by k = (4/3) / aspect before each projection (wrappers of 0x453740,
  0x457340, 0x457140; sprite width too). The game's 2560 virtual width then holds the wider
  view, and every clip constant stays valid.
- The renderer stretches 3D frames over the whole drawable.
- HUD: the wrappers of 0x4393e0 / 0x456270 call the walker twice: the list cut before the
  bucket-0 node, then the bucket-0 part with the 4:3 viewport (`glp_hud`). The walker's `ret`
  pops the return address, so esp is reset before the second call.
- Code: recomp/platform/widescreen.c, x86c.py `WRAPPED`, gl_present.c `glp_hud`.

## Tasks

1. [x] Terrain at the edges: at 21:9 ground was missing at the sides. Cause: 0x452880 picks a
   5x5 block grid around the point the camera looks at, 0x452a20 fills per-block masks of
   visible 4x4 cells (0x50dc80, read by 0x453360 and the object pass 0x4507f0) with the
   sphere-vs-frustum test 0x452fd0(p, r) (`|x|*16 <= 5z + ..`, the 4:3 view). Fix: wrapper
   scales p.x by k. 21:9 ground complete in the start scene. Still to check in play: the 5x5
   grid itself may be too small at extreme aspects (32:9) or camera angles.
2. [x] Object culling at the edges: objects use 0x4572a0(p, r), the same sphere test with
   p {x, y, z} s16 (callers 0x4280f0, 0x430bc0, 0x4373e0, 0x447de0); wrapper scales p.x by
   k. Other near-plane checks (0x4437d0, 0x47ee00) are unrelated. 32:9 start scene: ground,
   walls, lamps and objects complete to both edges. Not yet checked while moving.
3. [x] Aspect from the real drawable: `widescreen_update()` (after every THRASH present and at
   setvideomode) sets k from `glp_drawable_aspect()` and `glp_set_wide()`, so projection and
   renderer change together between frames (window resize, Alt+Enter). 4:3 or narrower:
   k = 1, pillarbox as before. Env `FCOP_WIDE=0` turns it off, `FCOP_WIDE=<aspect>` forces
   one. Only THRASH calls it so far (Glide: task 4).
4. [x] Glide path: at the 1280x960 setting the game opens 1280x1024 and stretches its 4:3
   picture to it (y * 1.0667, 0x48466c, in the walker 0x4393e0). So the 3D frame fills the
   drawable like THRASH, and the HUD pass uses a 4:3 viewport (`glp_viewport_43`), not the
   screen's 5:4. glide_gl.c calls `widescreen_update()` at grSstWinOpen and grBufferSwap.
   Tested 16:9: correct. Known, the game's own: type-6 text (ammo numbers) is in unscaled
   pixels, so at 1280x1024 it sits about 60 px above its icons (same without widescreen).
5. [x] Preferences (hardware mode): dialog.c hides the game's resolution combo (kept on its
   "1280x960" item, so the game draws at index 9) and shows ours (0x7E04): "Desktop (WxH)"
   plus the display's SDL modes; REG_DWORD PortResolution = w << 16 | h (0 = desktop).
   Box "Widescreen" (0x7E05, REG_DWORD PortWidescreen, default on) between Fullscreen and
   Filtering. OK applies both at once (`win_display_apply`, `widescreen_enable`).
   gl_present.c draws into an FBO of the render size (`glp_set_render_size`; fullscreen: the
   chosen size, else the drawable's) and blits it scaled (aspect kept, GL_LINEAR) on swap;
   `picture_rect` (mouse) follows. Windowed: the window gets the chosen size
   (`window_size_for`). Tested fullscreen: 1920x1080 on a 2560x1440 display renders 1920x1080
   in 16:9; Widescreen off gives the 4:3 pillarbox. Windowed sizes not tested yet.
6. [x] Checks (16:9 fullscreen, OpenGL and Glide):
   - Target markers: 0x435330 projects the target and emits a sprite (page 8) with depth
     key 0, so it was drawn in the HUD pass at a 4:3 position. Wrapper moves each marker's
     centre to cx + (x - cx) / k right after emission (size kept).
   - Split screen: the game draws each player's list in its own walker call, starting with
     control records from 0x40ca10 (type bits 0-2 = 0; 0x10 = x origin, 0 or 0x140 in 640
     units; 0x08 = extent). The HUD call now gets them chained in front of bucket 0, and draws
     with `glp_hud(2|3)`: the 4:3 HUD's left/right half centred in, and scissored to, that
     player's half of the screen. Full-height HUD records (type 0x35 divider) are moved like
     markers. The right player's ammo numbers sit too far left: the game's own (CLAUDE.md).
   - Esc pause bar: black background as before (known, the game clears the screen).
   - Menus and dialogs stay 4:3 (16-bit frames, `glp_show16`).
   `FCOP_WIDE_LOG=1` logs full-height HUD records.
7. [x] Clean-up (one `to_screen` for markers and bars; type-6 pixel units from the game's
   resolution), CLAUDE.md, docs/THRASH_NOTES.md and GLIDE_NOTES.md updated. `FCOP_WIDE` stays
   as a test switch.
8. [x] HUD at the screen edges (single player): `draw_hud_anchored` splits bucket 0 into
   left / centre / right records by the centre of their extent (centre band 1280 +- 160 units)
   and draws each group with a left-aligned, centred or right-aligned 4:3 viewport
   (`glp_hud(4|1|5)`). Font glyphs in a row (page 0xd, type 6) are classified together so
   numbers and words stay whole; records placed by `to_screen` (markers) stay centred.
   `FCOP_HUD_EDGES=0` keeps the whole HUD centred. Split screen keeps one HUD per half.
   Tested 16:9: Crime War (OpenGL) left HUD at the left edge; Precinct Assault (Glide) scores
   and the second bar at the right edge.

## Follow-up: vertex precision (done)

The walkers rounded every vertex to the game's resolution (x / 2 at 1280x960). Now the
resolution's jump-table entry points at the walker's plain path during our calls (0x4566fb
THRASH, 0x4397cd Glide), so vertices arrive in 2560x1920 units; type-6 text stays in game pixels
and is drawn in its own call. The control record's origin is rescaled per call (x * 4 for units,
x * gw / 640 for text), which also fixes the game's split-screen text offset (the plain path adds
origin / 4). THRASH clips polygons itself against a rectangle from 0x469d90, wrapped to scale it.
Measured: 42 % of vertex x values are odd 2560 units (impossible before). Tested OpenGL and
Glide, single and split screen, 16:9 and 4:3.
