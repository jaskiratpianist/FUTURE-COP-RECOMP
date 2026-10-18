# Task: addons (replacement textures) and a "Use Addons" preference

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2;
the Windows functions it imports are implemented in `recomp/platform/`. Read
`recomp/platform/host.h` and `recomp/platform/k32_file.c` first, with your file-reading tool.

**Read only files inside this repository** (reading anything outside ends your session).
**Run the test command in the foreground and wait for its output**, never as a background
task. After every test, append one line (script, OK/PROBLEM, what you saw) to
`recomp/platform/addons_progress.txt`: your session may end without warning.

## Goal

Players put remastered textures in an `addons` folder (never in the game folder); the game
uses them when "Use Addons" is on in Preferences. A test addon already exists:
`addons/X1-Alpha-HD/` (the player robot), see below.

## Facts

- Textures live in the mission files (`<game dir>/missions/<name>`, e.g. `M1A1`), which the
  game reads with `CreateFileA` + `ReadFile` / `SetFilePointer` (k32_file.c). A mission file
  is a stream of chunks; every chunk is `tag (4 bytes) + size (u32 LE, the whole chunk)`.
  Tags are stored byte-reversed: the file starts with `LRTC` ("CTRL"). Chunks:
  - `COHS` ("SHOC"): bytes 16..19 hold a sub-tag. `RDHS` ("SHDR") = a resource header:
    bytes 24..27 the type, byte-reversed (`pmbC` = "Cbmp", a texture page), bytes 28..31 the
    resource id (u32), bytes 32..35 the resource size (u32). `TADS` ("SDAT") = a piece of
    the current resource's data: bytes 20..size-1 of the chunk, appended in file order until
    the resource size is reached.
  - `LLIF` ("FILL") and any chunk with a size < 8: padding; continue at the next `COHS`
    found after it.
  The Python reference is `tools/fcop_textures.py` (`chunks()`): match it exactly.
- A Cbmp payload is 132720 bytes; its **last 131072 bytes** are the page: 256x256 pixels,
  16-bit little endian, 555 (`(r>>3)<<10 | (g>>3)<<5 | (b>>3)`), row by row.
- An addon replaces rectangles of pages: `addons/<AddonName>/textures/<x>_<y>_<w>_<h>_<hash>.png`
  (decimal x y w h, hash = 8 hex digits). `hash` is the FNV-1a 32-bit hash (offset
  0x811c9dc5, prime 0x01000193, over bytes) of the **original** pixels of that rectangle:
  for each row y..y+h-1, the bytes of pixels x..x+w-1 (2 bytes each), rows concatenated
  (`region_pixels` + `fnv1a` in tools/fcop_textures.py). Every page in every mission file
  whose rectangle hashes to that value gets the replacement. The PNG may be bigger than the
  rectangle (the test addon's are 4x: 960x672 for 240x168): scale it down to w x h with a
  box filter (average the source pixels covering each target pixel); if smaller, scale up
  with nearest. Pixels with alpha < 128 keep the original pixel.
- Decode PNGs with `recomp/platform/third_party/stb_image.h` (`#define STB_IMAGE_IMPLEMENTATION`
  in addons.c only; `stbi_load(path, &w, &h, &n, 4)`, `stbi_image_free`).

## What to build

1. `recomp/platform/addons.c` + `addons.h`:
   - `int addons_enabled(void)` / `void addons_set_enabled(int on)`: the "Use Addons"
     setting, a DWORD value `UseAddons` (default 1 when missing) in our registry under
     `HKEY_LOCAL_MACHINE\Software\Electronic Arts\Future Cop\Settings`, through two helpers
     you add to gdi_misc.c (below). The environment variable `FCOP_ADDONS=0` or `1`
     overrides the setting (for tests).
   - The addons folder: `$FCOP_ADDONS_DIR` if set, else `addons` in the current directory,
     else `<game_dir>/../../addons` (`game_dir` is in host.h). Scan it once, at the first
     mission file opened: every subfolder (sorted by name; a later addon wins where two
     replace the same rectangle) and its `textures/*.png` with a valid name; ignore others.
     Log one line per addon found (folder name, number of textures).
   - `u8 *addons_patch_file(const char *host_path, u32 *len)`: if addons are enabled, there
     are replacements, and the file starts with `LRTC`: read the whole file, walk the
     chunks, and for every Cbmp page apply each matching replacement (decode each PNG once
     and cache the w x h 555 result). Write the changed pixels back into the right bytes
     of the file buffer (the page is spread over several SDAT chunks: keep, for every
     resource, the list of (file offset, length) pieces of its payload). Return a malloc'd
     buffer with the whole patched file and its length, or NULL if nothing matched or
     addons are off. Log `addons: <file name>: <n> regions patched`.
2. `k32_file.c`: in `CreateFileA`, for files opened for reading only (GENERIC_READ,
   OPEN_EXISTING), call `addons_patch_file`; if it returns a buffer, the handle reads from
   that memory instead of the real file: ReadFile, SetFilePointer (all move methods),
   GetFileSize (if implemented anywhere: check), GetFileType and CloseHandle (frees the
   buffer) must work on it exactly like on a real file. Other files: unchanged.
3. `gdi_misc.c`: add exported helpers (declare them in host.h is not allowed: declare them
   in addons.h and define them in gdi_misc.c):
   `int reg_host_get_dword(const char *key_path, const char *value, u32 *out)` (1 if found)
   and `void reg_host_set_dword(const char *key_path, const char *value, u32 v)` (creates
   the key, saves registry.txt), using the existing registry code and its mutex.
4. `dialog.c`, the Preferences dialog (resource id 101, `dlg->res_id == 101`): add a host
   check box "Use Addons" (control id 0xF001, never sent to the game's dialog procedure)
   below "Texture Coloring" (that one is at dialog units x 156, y 34, 113 x 9: put the new
   one at y 49, same size; convert like the template controls). Checked state from
   `addons_enabled()` when the dialog opens. When the game's OK button (id 1001) is clicked,
   call `addons_set_enabled(checked)` before passing the click on; Cancel (id 1002) or Esc
   changes nothing. Look at how dialog.c already hides the Processor Select controls for
   dialog 101 (search `res_id == 101`) and follow the same style. The setting applies to
   mission files opened afterwards (the next mission).

## Files

You may create `addons.c`, `addons.h`, `addons_progress.txt` in recomp/platform and edit
`k32_file.c`, `gdi_misc.c` (the two helpers only) and `dialog.c` (the check box only).

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 32 --input "9000:click@484,303" --png build/shots/addon_on.png

starts the first mission (M1A1) with addons on (default): the log must show the addon scan
and `addons: M1A1: ... regions patched`; the robot in the PNG (seen from behind, bottom
centre) should look more saturated and smoother. Compare with addons off through the
preference itself: `--input "9000:click@156,246 10500:click@352,188 12000:click@150,347 14000:click@484,303"`
(open Preferences, untick Use Addons, OK, START) must show no patch line and the original
robot. Find the exact check box position from a `--png` screenshot of the Preferences
dialog (`--seconds 12 --input "9000:click@156,246"`). The setting is saved in
registry.txt, so **finish with Use Addons ticked again**.

## Rules

- Plain C99 + SDL2 + POSIX (+ stb_image). Helpers `static`. No empty stubs.
- Every shell command except the test command is blocked; if an action is denied, continue.

## Final reply

What works, what is partial, and the tests you ran.
