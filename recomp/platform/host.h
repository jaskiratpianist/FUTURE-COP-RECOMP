/* Platform layer for the recompiled game: the Win32 / DirectX functions it imports,
 * implemented natively against guest memory.
 *
 * Rules for import implementations (see docs/PLATFORM_TASK.md):
 * - Signature: u32 imp_<Name>(Cpu *c). The return value goes to eax.
 * - Arguments: ARG(0), ARG(1), ... are the guest stack slots (32-bit each).
 * - Pointers are GUEST addresses (u32). Use G(p) / GSTR(p) to get host pointers.
 *   Never hand a host pointer to the game; allocate guest memory with galloc().
 * - Register each function in its file's table with IMPORT(name, nargs): nargs is
 *   the number of 32-bit argument slots (stdcall pops them); use -1 for cdecl/varargs.
 */
#ifndef FCOP_HOST_H
#define FCOP_HOST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#define ARG(n) rd32(c->r[4] + 4 + 4 * (n))
#define G(p) ((void *)(MEM + (u32)(p)))
#define GSTR(p) ((const char *)(MEM + (u32)(p)))

typedef u32 (*ImpFn)(Cpu *c);
struct ImpDef { const char *name; int nargs; ImpFn fn; };
#define IMPORT(name, nargs) { #name, nargs, imp_##name }

/* guest memory */
u32  galloc(u32 size);                 /* zeroed, 16-byte aligned; 0 on failure */
u32  galloc_aligned(u32 size, u32 align);
void gfree(u32 p);
u32  gsize(u32 p);
u32  grealloc(u32 p, u32 size);
u32  gstrdup(const char *s);
void gstrcpy(u32 dst, const char *s, u32 max);  /* copy a host string into guest memory, NUL-terminated */

/* events usable from host code (k32_sync.c): a handle the game can wait on */
u32  sync_event_new(int manual, int initial);
void sync_event_set(u32 h, int on);

/* thunks: guest-callable addresses that run host code (imports, COM methods, GetProcAddress) */
u32 thunk_new(const char *name, int nargs, ImpFn fn);

/* calling guest code from the host (window procedures, thread entry points, callbacks):
   pushes the arguments, runs the function, restores esp, returns eax */
u32 guest_call(Cpu *c, u32 addr, int nargs, const u32 *args);

/* COM objects: a guest object whose first dword points at a guest vtable of thunks.
   host_data is any host-side state, found again with com_host(this). */
u32   com_new(const struct ImpDef *methods, int n, void *host_data);
void *com_host(u32 obj);

/* handles (files, events, threads, heaps, ...): small integers from a shared table */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_HEAP, H_FIND, H_OTHER };
u32   handle_new(int kind, void *data);
void *handle_get(u32 h, int kind);      /* NULL if h is not of that kind */
void  handle_free(u32 h);

/* GetLastError / SetLastError value, per thread */
extern __thread u32 last_error;

/* GetStdHandle results (real handles from handle_new() start at 64) */
#define HSTD_INPUT  4u
#define HSTD_OUTPUT 8u
#define HSTD_ERROR  12u

/* the current thread's guest CPU, for host code that needs it (callbacks) */
extern __thread Cpu *cur_cpu;

/* game directory (argv[1]); host_path() maps a guest path like "C:\\FCOP\\x.dat" or
   "missions\\a.mis" to a host path inside it, case-insensitively */
extern char game_dir[];
const char *host_path(const char *guest_path, char *buf, size_t n);

void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* per-DLL tables (kernel32.c, user32.c, ...) */
extern const struct ImpDef imp_k32_proc[], imp_k32_mem[], imp_k32_file[], imp_k32_sync[], imp_u32_win[], imp_u32_misc[], imp_gdi32[], imp_advapi32[],
    imp_ole32[], imp_winmm[], imp_ddraw[], imp_dsound[], imp_dinput[], imp_comctl32[], imp_dialog[];
extern const int imp_k32_proc_n, imp_k32_mem_n, imp_k32_file_n, imp_k32_sync_n, imp_u32_win_n, imp_u32_misc_n, imp_gdi32_n, imp_advapi32_n, imp_ole32_n,
    imp_winmm_n, imp_ddraw_n, imp_dsound_n, imp_dinput_n, imp_comctl32_n, imp_dialog_n;

#define IMPORT_TABLE(dll) const int imp_##dll##_n = sizeof imp_##dll / sizeof imp_##dll[0]

#endif
