# Future Cop: L.A.P.D. — static recompilation

A native port of *Future Cop: L.A.P.D.* (EA, 1998, Windows) built by **static
recompilation**: every function of the original `FCopLAPD.exe` is translated
mechanically from x86 machine code into C, compiled with a modern compiler, and linked
against a new platform layer that replaces Win32 and DirectX with SDL2.

The game's own code runs natively: no emulator, and the original EXE is not executed.
It runs on Linux and is playable: menus, both game modes (Crime War and Precinct Assault),
two-player split screen, sound, music and videos.

![First native frame](docs/first_native_frame.png)

**No game code or data is in this repository.** You need your own copy of the game (the
original CD). The translated C is generated at build time from your `FCopLAPD.exe` and is
never committed.

## Building on Linux

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

```sh
./recomp/build.sh
```

The first build downloads SDL2's source and builds it as a static library (about 30
seconds), then translates the EXE into C and compiles it (about 20 seconds on a multi-core
machine). Later builds only recompile what changed. The result is **`./fcop`**: one
executable that needs nothing but the C library, so it runs on other Linux systems without
installing SDL2. (`SYSTEM_SDL=1 ./recomp/build.sh` links the system's SDL2 instead;
`DEBUG=1` keeps debug information.)

### 5. Play

Put `fcop` in any folder and start it. It keeps everything next to itself:

```
fcop
game_files/     the files from your CD (only FCopLAPD/ and Video/ are needed)
user_info/      your settings, key bindings and saved games
```

On the first start, when `game_files/` is empty, the game installs itself from your CD
image: it uses an image (`.iso`, or `.bin` + `.cue`) lying next to `fcop`, one given on the
command line (`./fcop "Future Cop.iso"`), or asks you to pick one (with the desktop's file
chooser, zenity or kdialog). It copies the 360 MB the game needs and never asks again. You
can also copy the whole CD into `game_files/` yourself.

If `fcop`'s folder is read-only, `user_info` goes to `~/.local/share/Electronic Arts/Future Cop/`
instead. Settings from older versions (`registry.txt`, `controls.txt`, `save.cfg` in the game
folder) are copied into `user_info/` on the first start.

## Windows

Not supported yet: the platform layer is SDL2-based and portable, but the Windows build
has not been set up or tested. It is on the list below.

## Playing

- **Menus** use the mouse. Skip videos with Esc, Space, Enter or a click.
- **Default controls** are the numeric keypad: 8 forward, 5 back, 4 / 6 turn, Space gun,
  Alt heavy weapon, Ctrl special weapon, Z jump, 2 action, 0 change target, 7 / 9 jink,
  M map, Tab camera, Esc pause. The arrow keys also move and turn. Change them in
  **Preferences > Controller Configuration**.
- **Gamepads** (any controller SDL recognises) are offered to the game as joysticks (tested with a simulated pad so far).
- **Preferences**: resolution (320x240 up to 1600x1200) and Fullscreen apply when the next
  mission starts; the menus always run at 640x480, as in the original. **Alt+Enter**
  switches between fullscreen and a window at any time; the window can be resized.

Optional environment variables:

| Variable | Effect |
|---|---|
| `FCOP_SKIP_VIDEOS=1` | Skip all videos (intro, briefings) |
| `FCOP_WINDOW_H=<pixels>` | Starting window height (default: 3/4 of the screen) |
| `FCOP_GAME_DIR=<folder>` | Use the game files in this folder instead of `game_files/` (same as giving the folder on the command line) |

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
- [x] Resolutions up to 1600x1200, fullscreen, resizable window
- [x] One self-contained executable that installs the game files from the CD image
- [ ] Windows and Android builds

## Third-party code

- [font8x8](https://github.com/dhepper/font8x8) (public domain): the dialog font
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT): MPEG-1 video and MP2 audio decoding
