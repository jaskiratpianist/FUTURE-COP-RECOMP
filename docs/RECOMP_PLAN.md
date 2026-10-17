# Static recompilation plan

Goal: a native build (Linux and Windows first, then Android) that runs the game's own code,
translated mechanically from `FCopLAPD.exe` into C, with Win32/DirectX/Glide replaced by
an SDL + OpenGL platform layer. Matched decompiled functions replace their generated
versions over time. Decompilation continues, but at lower priority.

The generated C is derived from the retail EXE, so it is never committed: it is produced
at build time from the user's own copy, the same way the game data is.

## Census (data/functions.json, linear disassembly)

- About 185K instructions in 2,037 functions (1,761 game, 276 MSVC CRT). The ~100 real mnemonics
  are mostly integer ops, about 30 x87, plus `rep movs/stos`, `repne scasb`, and a little MMX
  (`movq`, `emms`) and `cpuid`/`rdtsc`. (Junk such as `arpl`/`das` comes from data inside code.)
- Indirect control flow: about 560 `call reg` and 305 `call [mem]` (COM vtables, function
  pointers), and 172 `jmp [mem]` (switch tables, import thunks).
- Renderers in the EXE: software, Direct3D, and 3dfx Glide (`glide2x.dll`, loaded at run time).
  Modes are 640x480, 800x600 and 1024x768.

## Design

- **Guest memory:** reserve one 4 GB region and address it as `base + (u32)addr`. Game
  pointers stay 32-bit, so this works on 64-bit hosts and ARM. The EXE's sections are
  loaded at their original addresses, and the stack and heap live in the same region.
- **CPU state:** one context per thread, holding the registers, the flags (computed lazily,
  only where a `jcc`/`setcc`/`adc` reads them) and the x87 stack.
- **Functions:** one C function per original function. Direct `call` becomes a C call.
  Indirect calls go through `dispatch(addr)`, a table from address to function. Switch tables
  become `switch` statements.
- **Imports and COM:** each imported API is a host function at a reserved thunk address.
  DirectDraw, DirectInput and DirectSound objects are host objects whose guest vtables
  point at thunk addresses.
- **Rendering:** v1 shows the software renderer's frame buffer through SDL. Then comes a
  Glide 2.x implementation on OpenGL, for any resolution and texture replacement.
- **Verification:** differential testing. Run a function in the Unicorn x86 emulator and as
  generated C from the same state, and compare registers and memory. For whole-game bugs,
  trace against the original running under `loader/`.

## Milestones

1. ✅ Recompiler core (`recomp/x86c.py`): the generated C compiles and passes the Unicorn
   differential tests (1,523 functions identical; the rest can't be tested in isolation).
2. ✅ The whole EXE recompiles, compiles and links into a native executable.
3. ✅ Platform layer for all 200 imports (written by Gemini workers, partly reviewed).
   The game boots to the front-end menu (docs/first_native_frame.png).
4. ⬜ Menu input works, a mission starts and is playable, with sound. Intro movies (DirectShow) skipped for now.
5. ⬜ Glide 2.x on OpenGL: high resolution, filtering, texture replacement.
6. ⬜ Features: native quit, preferences menu (controls, graphics, resolution).
7. ⬜ 64-bit cleanliness (guest memory already keeps pointers 32-bit), Windows and Android builds.
