# Future Cop: L.A.P.D. — static recompilation

A native port of *Future Cop: L.A.P.D.* (EA, 1998, Windows) built by **static
recompilation**: every function of the original `FCopLAPD.exe` is translated
mechanically from x86 machine code into C, compiled with a modern compiler, and linked
against a new platform layer that replaces Win32 and DirectX with SDL2.

The game's own code runs natively: no emulator, and the original EXE is not executed.
It runs on Linux and Windows and is playable: menus, both game modes (Crime War and Precinct
Assault), two-player split screen, sound, music and videos.

![First native frame](docs/first_native_frame.png)

**No game code or data is in this repository.** You need your own copy of the game (the
original CD). The translated C is generated at build time from your `FCopLAPD.exe` and is
never committed.

## Building

Both versions are built on Linux: `fcop` for Linux, and `fcop.exe` for Windows, which is
cross-compiled (on Windows itself, build inside [WSL](https://learn.microsoft.com/windows/wsl/install)).
Steps 1 to 3 are the same for both.

### 1. Install the tools

You need gcc, CMake, curl, Python 3, and the SDL2 development package (it brings in the
X11, Wayland and audio headers that the built-in SDL2 is compiled against). **gcc 15 or
newer** is recommended (it guarantees the tail calls the translated code relies on).

| Distribution | Command |
|---|---|
| Ubuntu / Debian | `sudo apt install build-essential cmake curl libsdl2-dev python3 python3-venv` |
| Fedora | `sudo dnf install gcc cmake curl SDL2-devel python3` |
| Arch | `sudo pacman -S gcc cmake curl sdl2 python` |

### 2. Get the code and the Python packages

```sh
git clone <this repository> future-cop-recomp
cd future-cop-recomp
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

### 3. Copy the game files

The build translates your own `FCopLAPD.exe`, so it needs the game files: copy the CD (at
least its `FCopLAPD` and `Video` folders) into `game_files/`. From a CD image, for example:

```sh
7z x -ogame_files "/path/to/Future Cop.iso" FCopLAPD Video
```

### 4. Build

For Linux:

```sh
./recomp/build.sh
```

The first build downloads SDL2's source and builds it as a static library (about 30
seconds), then translates the EXE into C and compiles it (about 20 seconds on a multi-core
machine). Later builds only recompile what changed. The result is **`./fcop`**: one
executable that needs nothing but the C library, so it runs on other Linux systems without
installing SDL2. (`SYSTEM_SDL=1 ./recomp/build.sh` links the system's SDL2 instead;
`DEBUG=1` keeps debug information.)

For Windows (64-bit Windows 10 or 11):

```sh
TARGET=windows ./recomp/build.sh
```

The first Windows build downloads the [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
cross-compiler (about 80 MB, into `build/`; nothing is installed on your system) and builds
SDL2 for Windows. The result is **`fcop.exe`**: one executable that needs only Windows' own
system files, no installer and no extra DLLs. Copy it to the Windows PC.

Both builds can live in the same folder: they share `game_files/` and, if you like,
`user_info/`.

### 5. Play

Put `fcop` (or `fcop.exe`) in any folder and start it. It keeps everything next to itself:

```
fcop
game_files/     the files from your CD (only FCopLAPD/ and Video/ are needed)
user_info/      your settings, key bindings and saved games
```

On the first start, when `game_files/` is empty, the game installs itself from your CD
image: it uses an image (`.iso`, or `.bin` + `.cue`) lying next to `fcop`, one given on the
command line (`./fcop "Future Cop.iso"`), or asks you to pick one (on Linux with the
desktop's file chooser, zenity or kdialog; on Windows with the usual Open dialog; you can
also drop the image onto the window). It copies the 360 MB the game needs and never asks again. You
can also copy the whole CD into `game_files/` yourself.

If the game's folder is read-only, `user_info` goes to `~/.local/share/Electronic Arts/Future Cop/`
(Linux) or `%APPDATA%\Electronic Arts\Future Cop\` (Windows) instead. On Windows the log
is written to `user_info/fcop.log`, which helps when reporting a problem. Settings from older versions (`registry.txt`, `controls.txt`, `save.cfg` in the game
folder) are copied into `user_info/` on the first start.

## Playing

- **Menus** use the mouse. Skip videos with Esc, Space, Enter or a click.
- **Default controls** are the numeric keypad: 8 forward, 5 back, 4 / 6 turn, Space gun,
  Alt heavy weapon, Ctrl special weapon, Z jump, 2 action, 0 change target, 7 / 9 jink,
  M map, Tab camera, Esc pause. The arrow keys also move and turn. Change them in
  **Preferences > Controller Configuration**.
- **Gamepads** (any controller SDL recognises) are offered to the game as joysticks (tested with a simulated pad so far).
- **Preferences**: resolution (320x240 up to 1280x960) and Fullscreen apply when the next
  mission starts; the menus always run at 640x480, as in the original. **Alt+Enter**
  switches between fullscreen and a window at any time; the window can be resized.
- **Frame rate**: the original game runs missions at 37.5 frames per second. **Preferences >
  Frame Rate** raises that (up to 90 by default, matched to your screen's refresh rate so
  motion stays even), up to Unlimited, or back to the original 37.5. The game plays at the
  same speed either way. **Show FPS Counter** shows the frame rate in missions.
- **CRT Filter** (Preferences) gives the picture the look of an old monitor: scanlines,
  a phosphor pattern and slightly darker corners. It only changes how the picture looks.

Optional environment variables:

| Variable | Effect |
|---|---|
| `FCOP_SKIP_VIDEOS=1` | Skip all videos (intro, briefings) |
| `FCOP_WINDOW_H=<pixels>` | Starting window height (default: 3/4 of the screen) |
| `FCOP_GAME_DIR=<folder>` | Use the game files in this folder instead of `game_files/` (same as giving the folder on the command line) |
| `FCOP_FPS_MAX=<n>` | Mission frame rate cap for this run, overriding Preferences (`0` unlimited, `37` the original) |

## How it works

| Part | What it does |
|---|---|
| `recomp/x86c.py` | Translates every function in the EXE into C (`build/recomp/gen/`, a few seconds), including the software renderer's self-modifying code |
| `recomp/rt/` | The guest CPU state and memory model (`cpu.h`) and runtime helpers |
| `recomp/platform/` | The Win32 / DirectDraw / DirectInput / DirectSound / DirectShow functions the game imports, implemented on SDL2 against guest memory; the game's dialog boxes are drawn in its own style |
| `recomp/difftest.py` | Runs each original function in the Unicorn emulator and as recompiled C from the same random states, and compares the results |
| `recomp/plat_test.py` | Builds and runs the game headless with scripted input, for automated testing (screenshots, sound levels) |
| `data/functions.json` | Function boundaries of the EXE (from a Ghidra analysis) |

Details and milestones: [docs/RECOMP_PLAN.md](docs/RECOMP_PLAN.md).

## Status

- [x] Whole EXE recompiles and links; 1,586 functions verified identical to the original in differential tests
- [x] All 200 imported Windows functions have an implementation
- [x] Playable missions with keyboard, mouse and gamepad, sound, music and videos
- [x] The game's dialogs (preferences, controller configuration, audio, pause, load/save, quit) redrawn in the game's style
- [x] Resolutions up to 1280x960, fullscreen, resizable window
- [x] Missions above the original 37.5 fps (up to unlimited) at the same game speed
- [x] One self-contained executable that installs the game files from the CD image
- [x] Windows build (cross-compiled; tested in a Windows virtual machine)
- [ ] Android build

## Third-party code

- [SDL2](https://libsdl.org) (zlib licence) and, for the Windows build,
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw): downloaded at build time, not included

- [font8x8](https://github.com/dhepper/font8x8) (public domain): the dialog font
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT): MPEG-1 video and MP2 audio decoding
