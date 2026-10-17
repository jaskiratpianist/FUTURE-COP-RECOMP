# Task: implement Windows API functions for the recompiled Future Cop

The game (FCopLAPD.exe, Windows 98, 1998) has been mechanically translated to C. It runs
natively on Linux, but every Windows function it imports must be provided by us. You
implement the functions listed for you, **in your one file only**:

{ASSIGNMENT}

## How the platform layer works (read `recomp/platform/host.h` first)

- The game runs on a 32-bit **guest** address space: guest address `p` is host memory
  `MEM + p`. Every pointer the game passes or receives is a guest address (`u32`).
  - `G(p)`: host pointer for guest address p. `GSTR(p)`: a guest C string as `const char *`.
  - `rd8/rd16/rd32(p)`, `wr8/wr16/wr32(p, v)` read and write guest memory.
  - Never give the game a host pointer. Guest memory comes from `galloc(size)` (zeroed),
    `gfree`, `grealloc`, `gsize`, `gstrdup`, `gstrcpy`.
- An implementation looks like this:

  ```c
  /* DWORD GetTickCount(void) */
  static u32 imp_GetTickCount(Cpu *c) { (void)c; return (u32)(SDL_GetTicks()); }
  /* BOOL QueryPerformanceCounter(LARGE_INTEGER *out) */
  static u32 imp_QueryPerformanceCounter(Cpu *c)
  {
      u64 t = now_ns();
      wr32(ARG(0), (u32)t);
      wr32(ARG(0) + 4, (u32)(t >> 32));
      return 1;
  }
  const struct ImpDef imp_k32_sync[] = {
      IMPORT(GetTickCount, 0),
      IMPORT(QueryPerformanceCounter, 1),
  };
  IMPORT_TABLE(k32_sync);
  ```

  `ARG(n)` is the n-th 32-bit argument. The number in `IMPORT(name, n)` is how many 32-bit
  argument slots the function has (Win32 functions are stdcall: the callee removes them).
  Use **-1** for cdecl/varargs functions (`wsprintfA`). The return value goes to eax; for
  a 64-bit result also set `c->r[2]` (edx). Functions returning nothing return 0.
- Handles: `handle_new(kind, host_ptr)`, `handle_get(h, kind)`, `handle_free(h)`.
  `GetStdHandle` returns `HSTD_INPUT/OUTPUT/ERROR`. Set `last_error` on failure.
- Paths: the game uses Windows paths relative to its directory or with a drive letter.
  `host_path(GSTR(p), buf, sizeof buf)` maps them to a real file inside the game
  directory (case-insensitive).
- Calling back into game code (thread entry points, window procedures, callbacks):
  `guest_call(cpu, guest_addr, nargs, args)`. Each guest thread needs its own `Cpu`
  (host memory, zeroed, `cw = 0x27f`), its own guest stack (e.g. 1 MB from `galloc`,
  `r[4]` = its top minus 64), and its own thread information block (`fs_base`: 0x1000 bytes
  from galloc; dword 0 = 0xffffffff, dword 4 = stack top, dword 8 = stack bottom,
  dword 0x18 = its own address).
- `plat_log("...")` logs to stderr. Log a `warn:` line for anything you cannot do exactly.

## Rules

- Edit **only** your assigned file. Do not change `host.h`, `core.c`, other files or the
  generated code. If you need a helper, make it `static` in your file.
- Implement real behaviour from the Win32 documentation (Windows 98 semantics), not empty
  stubs, unless the function is genuinely meaningless here (e.g. `SetHandleCount`); then
  return what Windows would return for success.
- Plain C (C99), POSIX and SDL2 (`#include <SDL.h>`) are available. Use pthreads for
  threads and synchronisation. No other libraries.
- Allowed command, run it as often as you like:
  `.venv/bin/python -I recomp/plat_test.py`  (add `--api` to log every Windows call)
  It builds everything and runs the game for 15 s. It is normal that the game stops at
  an "unimplemented import" that belongs to another worker's file; make sure the build
  succeeds and that none of **your** functions is the cause of a crash.
- Read files (host.h, win.h, cpu.h, the Win32 docs you know) with your file-reading tool, never
  with shell commands like `cat`, `ls` or `grep`.
- Every other shell command and all MCP tools are blocked; do not use them. If an action
  is denied, continue with the next step.

## Final reply

One line per function: `Name done` or `Name partial: <what is missing>`.
