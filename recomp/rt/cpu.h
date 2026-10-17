/* Guest CPU state and memory for statically recompiled FCopLAPD.exe code.
 *
 * Guest memory is one 4 GB region: guest address a lives at MEM + a, so game pointers
 * stay 32-bit on any host. Generated functions keep the registers and flags in C locals
 * (LOAD on entry and after calls, SYNC before calls and returns), so the host compiler
 * can drop flag computations nobody reads.
 */
#ifndef FCOP_RECOMP_CPU_H
#define FCOP_RECOMP_CPU_H
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

typedef struct Cpu {
    u32 r[8];          /* eax ecx edx ebx esp ebp esi edi */
    u32 fs_base;       /* guest address of the thread information block */
    u32 df;            /* direction flag */
    double st[8];      /* x87 registers, ST(i) = st[(top + i) & 7] */
    u32 top;
    u16 sw, cw;        /* x87 status word (without TOP) and control word */
    u64 mm[8];         /* MMX registers (aliasing with x87 is not modelled) */
    u32 fl[5];         /* CF ZF SF OF PF, only passed along on jumps between functions */
    s64 sti[8];        /* exact value of a register loaded by fild m64 (doubles lose bits), */
    u8 stx[8];         /* valid while stx is set: the FPU memcpy idiom fild/fistp qword */
} Cpu;

extern u8 *MEM;

static inline u8  rd8(u32 a)  { return MEM[a]; }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, MEM + a, 2); return v; }
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, MEM + a, 4); return v; }
static inline u64 rd64(u32 a) { u64 v; memcpy(&v, MEM + a, 8); return v; }
static inline float  rdf32(u32 a) { float v;  memcpy(&v, MEM + a, 4); return v; }
static inline double rdf64(u32 a) { double v; memcpy(&v, MEM + a, 8); return v; }
static inline void wr8(u32 a, u8 v)   { MEM[a] = v; }
static inline void wr16(u32 a, u16 v) { memcpy(MEM + a, &v, 2); }
static inline void wr32(u32 a, u32 v) { memcpy(MEM + a, &v, 4); }
static inline void wr64(u32 a, u64 v) { memcpy(MEM + a, &v, 8); }
static inline void wrf32(u32 a, float v)  { memcpy(MEM + a, &v, 4); }
static inline void wrf64(u32 a, double v) { memcpy(MEM + a, &v, 8); }

#define PAR(x) (!__builtin_parity((u8)(x)))

#define LOAD do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; \
                  esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)
#define SYNC do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; \
                  c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)
#define FSYNC do { c->fl[0] = CF; c->fl[1] = ZF; c->fl[2] = SF; c->fl[3] = OF; c->fl[4] = PF; } while (0)
#define REGS u32 eax, ecx, edx, ebx, esp, ebp, esi, edi; \
             u32 CF = c->fl[0], ZF = c->fl[1], SF = c->fl[2], OF = c->fl[3], PF = c->fl[4]; \
             (void)CF; (void)ZF; (void)SF; (void)OF; (void)PF

#ifdef RECOMP_TRACE  /* log every instruction with the registers before it runs */
void rt_trace(u32 a, u32 eax, u32 ecx, u32 edx, u32 ebx, u32 esp, u32 ebp, u32 esi, u32 edi);
#define TR(a) rt_trace(a, eax, ecx, edx, ebx, esp, ebp, esi, edi)
#else
#define TR(a) ((void)0)
#endif

#define ST(i) c->st[(c->top + (i)) & 7]
#define STK(i) ((c->top + (i)) & 7)
#define STW(i) (*(c->stx[STK(i)] = 0, &ST(i)))   /* ST(i) as an lvalue for a computed value */
#define FPUSH(v) do { double _v = (v); c->top = (c->top - 1) & 7; ST(0) = _v; c->stx[STK(0)] = 0; } while (0)
#define FPUSHI(v) do { s64 _v = (s64)(v); c->top = (c->top - 1) & 7; ST(0) = (double)_v; \
                       c->sti[STK(0)] = _v; c->stx[STK(0)] = 1; } while (0)
#define FPUSHR(i) do { int _k = STK(i); double _v = c->st[_k]; s64 _x = c->sti[_k]; u8 _t = c->stx[_k]; \
                       c->top = (c->top - 1) & 7; ST(0) = _v; c->sti[STK(0)] = _x; c->stx[STK(0)] = _t; } while (0)
#define FCOPY(d, s) do { int _a = STK(d), _b = STK(s); c->st[_a] = c->st[_b]; c->sti[_a] = c->sti[_b]; \
                         c->stx[_a] = c->stx[_b]; } while (0)
#define FXCH(i) do { int _a = STK(0), _b = STK(i); double _v = c->st[_a]; s64 _x = c->sti[_a]; u8 _t = c->stx[_a]; \
                     c->st[_a] = c->st[_b]; c->sti[_a] = c->sti[_b]; c->stx[_a] = c->stx[_b]; \
                     c->st[_b] = _v; c->sti[_b] = _x; c->stx[_b] = _t; } while (0)
#define FIST64(x) (c->stx[STK(0)] ? c->sti[STK(0)] : rt_fist(c, (x)))
#define FPOP() (c->top = (c->top + 1) & 7)

/* A jump from one recompiled function into another is a tail call; it must not grow the host
   stack, or loops that span functions (the software renderer's pixel loops) overflow it. */
#if defined(__has_attribute)
#if __has_attribute(musttail)
#define TAILCALL(call) do { __attribute__((musttail)) return call; } while (0)
#endif
#endif
#ifndef TAILCALL
#define TAILCALL(call) do { call; return; } while (0)  /* older compilers: rely on -O2 sibling calls */
#endif

/* runtime (rt.c) */
void dispatch(Cpu *c, u32 addr);                 /* call guest code at addr (return address pushed) */
void rt_unimpl(Cpu *c, u32 addr, const char *what);
void rt_trap(Cpu *c, u32 addr, const char *why);
void rt_fcom(Cpu *c, double a, double b);        /* sets C0/C2/C3 */
void rt_fxam(Cpu *c);
s64  rt_fist(Cpu *c, double x);                  /* round per the control word */
double rt_frnd(Cpu *c, double x);
double rt_ld80(u32 a);
void rt_st80(u32 a, double x);
void rt_cpuid(Cpu *c);
void rt_rdtsc(Cpu *c);
u32  rt_in(Cpu *c, u32 port, int size);
void rt_out(Cpu *c, u32 port, u32 v, int size);

/* Guest threads take turns, as on the single-core PCs the game was written for (it has
   races that only show up when its threads really run in parallel). rt_poll is set while
   another guest thread waits for its turn; loop heads check it, so spin loops hand over too.
   The acquire load also makes the compiler re-read guest memory after each check. */
extern int rt_poll;
void rt_yield(Cpu *c);
#define POLL do { if (__builtin_expect(__atomic_load_n(&rt_poll, __ATOMIC_ACQUIRE), 0)) rt_yield(c); } while (0)

#endif
