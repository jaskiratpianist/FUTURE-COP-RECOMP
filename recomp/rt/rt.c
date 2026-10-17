/* Runtime support for recompiled code: dispatch, x87 helpers, odd instructions. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "cpu.h"

u8 *MEM;

struct entry { u32 addr; void (*fn)(Cpu *); };
extern const struct entry recomp_table[];
extern const unsigned recomp_table_len;

/* called for any guest address that is not the start of a recompiled function:
   import thunks, COM methods and anything unexpected (the platform layer overrides it) */
__attribute__((weak)) void rt_foreign(Cpu *c, u32 addr)
{
    rt_trap(c, addr, "call to unknown address");
}

/* guest thread turns: the platform layer implements them (core.c); tests run one thread */
int rt_poll;
__attribute__((weak)) void rt_yield(Cpu *c) { (void)c; }

void dispatch(Cpu *c, u32 addr)
{
    unsigned lo = 0, hi = recomp_table_len;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (recomp_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < recomp_table_len && recomp_table[lo].addr == addr)
        recomp_table[lo].fn(c);
    else
        rt_foreign(c, addr);
}

__attribute__((weak)) void rt_trap(Cpu *c, u32 addr, const char *why)
{
    (void)c;
    fprintf(stderr, "recomp: trap at 0x%08x: %s\n", addr, why);
    abort();
}

void rt_unimpl(Cpu *c, u32 addr, const char *what)
{
    char buf[128];
    snprintf(buf, sizeof buf, "unimplemented: %s", what);
    rt_trap(c, addr, buf);
}

void rt_fcom(Cpu *c, double a, double b)
{
    c->sw &= ~0x4500;  /* C0 C2 C3 */
    if (a != a || b != b) c->sw |= 0x4500;
    else if (a < b) c->sw |= 0x0100;
    else if (a == b) c->sw |= 0x4000;
}

void rt_fxam(Cpu *c)
{
    double x = ST(0);
    u16 cc;
    switch (fpclassify(x)) {
    case FP_NAN: cc = 0x0100; break;
    case FP_INFINITE: cc = 0x0500; break;
    case FP_ZERO: cc = 0x4000; break;
    case FP_SUBNORMAL: cc = 0x4400; break;
    default: cc = 0x0400; break;
    }
    if (signbit(x)) cc |= 0x0200;
    c->sw = (c->sw & ~0x4700) | cc;
}

double rt_frnd(Cpu *c, double x)
{
    switch ((c->cw >> 10) & 3) {
    case 0: return nearbyint(x);  /* host default is round-to-nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}

s64 rt_fist(Cpu *c, double x)
{
    double r = rt_frnd(c, x);
    if (r != r || r >= 9223372036854775808.0 || r < -9223372036854775808.0)
        return INT64_MIN;
    return (s64)r;
}

double rt_ld80(u32 a)
{
    u64 man = rd64(a);
    u16 se = rd16(a + 8);
    int e = se & 0x7fff;
    double v;
    if (e == 0 && man == 0) v = 0.0;
    else if (e == 0x7fff) v = (man << 1) ? NAN : INFINITY;
    else v = ldexp((double)man, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

void rt_st80(u32 a, double x)
{
    u64 man = 0;
    u16 se = signbit(x) ? 0x8000 : 0;
    double m = fabs(x);
    if (m != m) { man = 0xC000000000000000ull; se |= 0x7fff; }
    else if (isinf(m)) { man = 0x8000000000000000ull; se |= 0x7fff; }
    else if (m != 0.0) {
        int e;
        double f = frexp(m, &e);           /* m = f * 2^e, 0.5 <= f < 1 */
        man = (u64)ldexp(f, 64);
        se |= (u16)(e - 1 + 16383);
    }
    wr64(a, man);
    wr16(a + 8, se);
}

void rt_cpuid(Cpu *c)
{
    /* a Pentium MMX: family 5, model 4, MMX in edx bit 23 */
    if (c->r[0] == 0) {
        c->r[0] = 1;
        c->r[3] = 0x756e6547; c->r[2] = 0x49656e69; c->r[1] = 0x6c65746e;  /* GenuineIntel */
    } else {
        c->r[0] = 0x543; c->r[3] = 0; c->r[1] = 0; c->r[2] = 0x008001bf;
    }
}

void rt_rdtsc(Cpu *c)
{
    struct timespec ts;
    u64 t;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t = (u64)ts.tv_sec * 200000000ull + (u64)ts.tv_nsec / 5;  /* a 200 MHz clock */
    c->r[0] = (u32)t;
    c->r[2] = (u32)(t >> 32);
}

/* hardware ports (the game polls the gameport at 0x201): nothing is connected */
u32 rt_in(Cpu *c, u32 port, int size) { (void)c; (void)port; return size == 1 ? 0xff : 0xffffffffu; }
void rt_out(Cpu *c, u32 port, u32 v, int size) { (void)c; (void)port; (void)v; (void)size; }
