/* Windows host: the few POSIX calls MinGW lacks. Empty on other hosts. */
#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#include <windows.h>
#include "os_win.h"

void *os_reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }

int os_commit(void *p, size_t n) { return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != NULL; }

int setenv(const char *name, const char *val, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, val);
}

int unsetenv(const char *name) { return _putenv_s(name, ""); }

int fsync(int fd) { return _commit(fd); }
#endif

#ifdef _WIN32
/* FCOP_PROF="start_ms:duration_ms[:file]": a sampling profiler for the thread that calls
   os_prof_start (the game's main thread). Every millisecond it suspends that thread, records
   its call stack (unwound with the x64 unwind tables) and resumes it; at the end it writes
   the stacks and the loaded modules to the file (default build/prof.txt), for
   recomp/prof_report.py. */
#include <stdio.h>
#include <psapi.h>

#define PROF_DEPTH 48
static HANDLE prof_thread;
static unsigned prof_start_ms, prof_dur_ms;
static char prof_file[260] = "build/prof.txt";

static DWORD WINAPI prof_run(LPVOID arg)
{
    DWORD64 (*buf)[PROF_DEPTH + 1];
    unsigned cap, n = 0, k;
    DWORD t0;
    FILE *f;
    HMODULE mods[512];
    DWORD need;
    (void)arg;
    cap = prof_dur_ms + 1000;
    if (!(buf = calloc(cap, sizeof *buf))) return 0;
    timeBeginPeriod(1);
    Sleep(prof_start_ms);
    t0 = GetTickCount();
    while (GetTickCount() - t0 < prof_dur_ms && n < cap) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(prof_thread) != (DWORD)-1) {
            if (GetThreadContext(prof_thread, &ctx)) {
                unsigned d = 0;
                while (d < PROF_DEPTH && ctx.Rip) {
                    DWORD64 base, est = 0;
                    PVOID hd;
                    PRUNTIME_FUNCTION fn;
                    buf[n][1 + d++] = ctx.Rip;
                    fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (fn) {
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &est, NULL);
                    } else {  /* leaf function: the return address is on top of the stack */
                        if (!ctx.Rsp) break;
                        ctx.Rip = *(DWORD64 *)ctx.Rsp;
                        ctx.Rsp += 8;
                    }
                }
                buf[n][0] = d;
                n++;
            }
            ResumeThread(prof_thread);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    if (!(f = fopen(prof_file, "w"))) return 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        for (k = 0; k < need / sizeof mods[0] && k < 512; k++) {
            MODULEINFO mi;
            char name[MAX_PATH];
            if (!GetModuleInformation(GetCurrentProcess(), mods[k], &mi, sizeof mi)) continue;
            if (!GetModuleFileNameA(mods[k], name, sizeof name)) continue;
            fprintf(f, "M %llx %lx %s\n", (unsigned long long)(size_t)mi.lpBaseOfDll, mi.SizeOfImage, name);
        }
    }
    for (k = 0; k < n; k++) {
        unsigned d;
        fputc('S', f);
        for (d = 0; d < buf[k][0]; d++) fprintf(f, " %llx", (unsigned long long)buf[k][1 + d]);
        fputc('\n', f);
    }
    fclose(f);
    free(buf);
    fprintf(stderr, "profile: %u samples written to %s\n", n, prof_file);
    return 0;
}

void os_prof_start(const char *spec)
{
    char file[260];
    file[0] = 0;
    if (sscanf(spec, "%u:%u:%259s", &prof_start_ms, &prof_dur_ms, file) < 2) return;
    if (file[0]) snprintf(prof_file, sizeof prof_file, "%s", file);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_thread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(CreateThread(NULL, 0, prof_run, NULL, 0, NULL));
}
#endif
