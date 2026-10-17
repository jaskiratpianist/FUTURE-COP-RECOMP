# Future Cop: L.A.P.D. — static recompilation

A native port of *Future Cop: L.A.P.D.* (EA, 1998, Windows) built by **static
recompilation**: every function of the original `FCopLAPD.exe` is translated
mechanically from x86 machine code into C, compiled with a modern compiler, and linked
against a new platform layer that replaces Win32 and DirectX with SDL2.

The game's own code runs natively: no emulator, and the original EXE is not executed.
It currently boots to the front-end menu on Linux.

![First native frame](docs/first_native_frame.png)

**No game code or data is in this repository.** You need your own copy of the game. The
translated C is generated at build time from your `FCopLAPD.exe` and is never committed.

## Building (Linux)

Requirements: gcc, SDL2 development files (`libsdl2-dev`), Python 3.

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# extract the game so that FCopLAPD.exe is somewhere under game_files/
./recomp/build.sh
build/recomp/fcop game_files/<folder with FCopLAPD.exe>
```

## How it works

| Part | What it does |
|---|---|
| `recomp/x86c.py` | Translates every function in the EXE into C (`build/recomp/gen/`, about 4 s), including the software renderer's self-modifying code |
| `recomp/rt/` | The guest CPU state and memory model (`cpu.h`) and runtime helpers |
| `recomp/platform/` | The Win32 / DirectDraw / DirectInput / DirectSound functions the game imports, implemented on SDL2 against guest memory |
| `recomp/difftest.py` | Runs each original function in the Unicorn emulator and as recompiled C from the same random states, and compares the results |
| `data/functions.json` | Function boundaries of the EXE (from a Ghidra analysis) |

Details and milestones: [docs/RECOMP_PLAN.md](docs/RECOMP_PLAN.md).

## Status

- [x] Whole EXE recompiles and links; 1,523 functions verified identical to the original in differential tests
- [x] All 200 imported Windows functions have an implementation
- [x] Boots to the front-end menu
- [ ] Playable missions with input and sound
- [ ] OpenGL renderer (Glide) for high resolutions and texture replacement
- [ ] Preferences menu with control remapping and resolution settings
- [ ] Windows and Android builds
