/* Test harness: run one recompiled function on a prepared guest state and report
 * how it ended. Built into a shared library for recomp/difftest.py. */
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include "cpu.h"

enum { H_OK, H_TRAP, H_FAULT, H_FOREIGN, H_TIMEOUT };
#define SENTINEL 0xdead0000u
#define FOREIGN_LO 0x7f000000u
#define FOREIGN_HI 0x7f001000u

static sigjmp_buf env;
static u32 last_addr;

void rt_trap(Cpu *c, u32 addr, const char *why) { (void)c; (void)why; last_addr = addr; siglongjmp(env, H_TRAP); }

void rt_foreign(Cpu *c, u32 addr)
{
    (void)c;
    last_addr = addr;
    siglongjmp(env, addr >= FOREIGN_LO && addr < FOREIGN_HI ? H_FOREIGN : H_TRAP);
}

/* guest pages mapped on demand (zero-filled), so random pointers behave the same as
   in Unicorn, which maps them the same way */
#define PAGE 4096u
static u32 demand[256], demand_n;

static volatile int in_run;

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    if (!in_run) {  /* not ours (Python, Unicorn): crash normally */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    if (sig == SIGSEGV && (u8 *)si->si_addr >= MEM && (u8 *)si->si_addr < MEM + (1ull << 32) && demand_n < 256) {
        u32 a = (u32)((u8 *)si->si_addr - MEM) & ~(PAGE - 1);
        if (mprotect(MEM + a, PAGE, PROT_READ | PROT_WRITE) == 0) {
            demand[demand_n++] = a;
            return;
        }
    }
    siglongjmp(env, sig == SIGALRM ? H_TIMEOUT : H_FAULT);
}

u32 h_demand(u32 **pages) { *pages = demand; return demand_n; }

void h_reset(void)
{
    for (u32 k = 0; k < demand_n; k++) {
        madvise(MEM + demand[k], PAGE, MADV_DONTNEED);
        mprotect(MEM + demand[k], PAGE, PROT_NONE);
    }
    demand_n = 0;
}

int h_init(void)
{
    struct sigaction sa;
    MEM = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    return 0;
}

u8 *h_mem(void) { return MEM; }
int h_map(u32 addr, u32 size) { return mprotect(MEM + addr, size, PROT_READ | PROT_WRITE); }
u32 h_last(void) { return last_addr; }

int h_run(Cpu *c, u32 addr, int timeout_ms)
{
    struct itimerval t = {{0, 0}, {timeout_ms / 1000, (timeout_ms % 1000) * 1000}}, off = {{0, 0}, {0, 0}};
    int r = sigsetjmp(env, 1);
    if (r == 0) {
        in_run = 1;
        setitimer(ITIMER_REAL, &t, NULL);
        c->r[4] -= 4;
        wr32(c->r[4], SENTINEL);
        dispatch(c, addr);
    }
    setitimer(ITIMER_REAL, &off, NULL);
    in_run = 0;
    return r;
}

#ifdef RECOMP_TRACE
#define TRACE_MAX (1u << 20)
static u32 trace[TRACE_MAX][9];
static u32 trace_n;
void rt_trace(u32 a, u32 eax, u32 ecx, u32 edx, u32 ebx, u32 esp, u32 ebp, u32 esi, u32 edi)
{
    if (trace_n < TRACE_MAX) {
        u32 *t = trace[trace_n++];
        t[0] = a; t[1] = eax; t[2] = ecx; t[3] = edx; t[4] = ebx; t[5] = esp; t[6] = ebp; t[7] = esi; t[8] = edi;
    }
}
u32 *h_trace(u32 *n) { *n = trace_n; trace_n = 0; return &trace[0][0]; }
#endif
unsigned h_cpu_size(void) { return sizeof(Cpu); }
