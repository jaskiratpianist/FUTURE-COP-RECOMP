/* Platform core: guest memory and allocator, thunks, handles, COM objects, guest calls,
 * loading the EXE and starting it. */
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "host.h"

/* guest address space layout */
#define TIB_ADDR    0x00010000u
#define STACK_LO    0x00100000u   /* main thread stack: 0x100000-0x400000 */
#define STACK_HI    0x00400000u
#define IMAGE_LO    0x00400000u
#define IMAGE_HI    0x00522000u
#define THUNK_LO    0x7f000000u
#define THUNK_N     8192u
#define ARENA_LO    0x01000000u   /* guest heap: 16 MB - 1 GB */
#define ARENA_HI    0x40000000u

char game_dir[1024];
__thread Cpu *cur_cpu;
__thread u32 last_error;

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- guest heap: first-fit free list with 16-byte headers ---------------------------- */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 arena_top = ARENA_LO;
struct blk { u32 size, next, free, magic; };   /* header just before the user block */
#define BLK(p) ((struct blk *)(MEM + (p) - 16))
#define MAGIC 0xfc0fb10cu
static u32 free_list;  /* guest address of first free user block */

u32 galloc_aligned(u32 size, u32 align)
{
    u32 p = 0, *link;
    size = (size + 15) & ~15u;
    if (align < 16) align = 16;
    pthread_mutex_lock(&heap_lock);
    if (align == 16) {
        for (link = &free_list; *link; link = &BLK(*link)->next)
            if (BLK(*link)->size >= size) {
                p = *link;
                *link = BLK(p)->next;
                break;
            }
    }
    if (!p) {
        u32 at = (arena_top + 16 + align - 1) & ~(align - 1);
        if (at + size > ARENA_HI) { pthread_mutex_unlock(&heap_lock); return 0; }
        p = at;
        arena_top = at + size;
        BLK(p)->size = size;
    }
    BLK(p)->free = 0;
    BLK(p)->magic = MAGIC;
    BLK(p)->next = 0;
    pthread_mutex_unlock(&heap_lock);
    memset(MEM + p, 0, BLK(p)->size);
    return p;
}

u32 galloc(u32 size) { return galloc_aligned(size ? size : 1, 16); }

void gfree(u32 p)
{
    if (!p || BLK(p)->magic != MAGIC || BLK(p)->free) return;
    pthread_mutex_lock(&heap_lock);
    BLK(p)->free = 1;
    BLK(p)->next = free_list;
    free_list = p;
    pthread_mutex_unlock(&heap_lock);
}

u32 gsize(u32 p) { return p && BLK(p)->magic == MAGIC ? BLK(p)->size : 0; }

u32 grealloc(u32 p, u32 size)
{
    u32 q;
    if (!p) return galloc(size);
    if (gsize(p) >= size) return p;
    q = galloc(size);
    if (q) { memcpy(MEM + q, MEM + p, gsize(p)); gfree(p); }
    return q;
}

u32 gstrdup(const char *s)
{
    u32 p = galloc((u32)strlen(s) + 1);
    if (p) strcpy((char *)G(p), s);
    return p;
}

void gstrcpy(u32 dst, const char *s, u32 max)
{
    if (!max) return;
    strncpy((char *)G(dst), s, max - 1);
    ((char *)G(dst))[max - 1] = 0;
}

static u32 missing_stub(Cpu *c);

/* ---- thunks -------------------------------------------------------------------------- */
static struct ImpDef thunks[THUNK_N];
static u32 thunk_n;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

u32 thunk_new(const char *name, int nargs, ImpFn fn)
{
    u32 k;
    pthread_mutex_lock(&thunk_lock);
    k = thunk_n++;
    pthread_mutex_unlock(&thunk_lock);
    if (k >= THUNK_N) { plat_log("out of thunks"); abort(); }
    thunks[k].name = name;
    thunks[k].nargs = nargs;
    thunks[k].fn = fn;
    return THUNK_LO + 4 * k;
}

/* ---- guest threads take turns --------------------------------------------------------------
   Only one thread runs guest code at a time (a fair ticket lock); host code runs unlocked, so a
   thread hands over its turn whenever it calls an import, and at loop heads when another
   thread is waiting (POLL in cpu.h). */
static unsigned turn_next, turn_now;
static __thread int turn_held;

static void turn_take(void)
{
    unsigned t = __atomic_fetch_add(&turn_next, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&turn_now, __ATOMIC_ACQUIRE) != t) {
        __atomic_store_n(&rt_poll, 1, __ATOMIC_RELEASE);
        sched_yield();
    }
    turn_held = 1;
}

static void turn_give(void)
{
    unsigned now;
    turn_held = 0;
    now = __atomic_add_fetch(&turn_now, 1, __ATOMIC_ACQ_REL);
    /* a waiter that arrives after this sets rt_poll again itself */
    __atomic_store_n(&rt_poll, __atomic_load_n(&turn_next, __ATOMIC_ACQUIRE) != now, __ATOMIC_RELEASE);
}

void rt_yield(Cpu *c)
{
    (void)c;
    if (!turn_held) return;
    turn_give();
    turn_take();
}

/* guest code called an address that is not recompiled code */
void rt_foreign(Cpu *c, u32 addr)
{
    u32 k = (addr - THUNK_LO) / 4, r;
    if (addr < THUNK_LO || k >= thunk_n) {
        plat_log("call to unknown address 0x%08x (esp 0x%08x, return 0x%08x)", addr, c->r[4], rd32(c->r[4]));
        abort();
    }
    if (getenv("FCOP_TRACE_API")) plat_log("api %s from 0x%08x", thunks[k].name, rd32(c->r[4]));
    if (thunks[k].fn == missing_stub) {
        plat_log("unimplemented import %s (called from 0x%08x)", thunks[k].name, rd32(c->r[4]));
        abort();
    }
    if (turn_held) {
        turn_give();
        r = thunks[k].fn(c);
        turn_take();
    } else {
        r = thunks[k].fn(c);
    }
    c->r[0] = r;
    c->r[4] += 4 + (thunks[k].nargs > 0 ? 4 * (u32)thunks[k].nargs : 0);
}

void rt_trap(Cpu *c, u32 addr, const char *why)
{
    plat_log("trap at 0x%08x: %s (eax %08x ecx %08x edx %08x esp %08x)", addr, why,
             c->r[0], c->r[1], c->r[2], c->r[4]);
    abort();
}

/* ---- calling guest code ------------------------------------------------------------------ */
#define RET_SENTINEL 0xdeadbeefu

u32 guest_call(Cpu *c, u32 addr, int nargs, const u32 *args)
{
    u32 esp = c->r[4];
    Cpu *prev = cur_cpu;
    int k, took = !turn_held;
    if (took) turn_take();
    cur_cpu = c;
    for (k = nargs - 1; k >= 0; k--) {
        c->r[4] -= 4;
        wr32(c->r[4], args[k]);
    }
    c->r[4] -= 4;
    wr32(c->r[4], RET_SENTINEL);
    dispatch(c, addr);
    c->r[4] = esp;  /* works for both stdcall and cdecl callees */
    cur_cpu = prev;
    if (took) turn_give();
    return c->r[0];
}

/* ---- COM objects ----------------------------------------------------------------------- */
/* host data by object number, in pages that never move (other threads read it unlocked) */
#define COM_PAGE 4096u
static void **com_pages[1024];
static u32 com_n;
static pthread_mutex_t com_lock = PTHREAD_MUTEX_INITIALIZER;

/* one guest vtable per interface (method table), shared by all its objects: the game creates
   surfaces and sound buffers all the time, and thunks are never freed */
static u32 com_vtable(const struct ImpDef *methods, int n)
{
    static struct { const struct ImpDef *methods; u32 vt; } vts[64];
    static int vt_n;
    int k;
    for (k = 0; k < vt_n; k++)
        if (vts[k].methods == methods) return vts[k].vt;
    if (vt_n == 64) { plat_log("out of COM vtables"); abort(); }
    vts[vt_n].methods = methods;
    vts[vt_n].vt = galloc(4 * (u32)n);
    for (k = 0; k < n; k++)
        wr32(vts[vt_n].vt + 4 * k, thunk_new(methods[k].name, methods[k].nargs, methods[k].fn));
    return vts[vt_n++].vt;
}

u32 com_new(const struct ImpDef *methods, int n, void *host_data)
{
    u32 obj = galloc(16);
    pthread_mutex_lock(&com_lock);
    wr32(obj, com_vtable(methods, n));
    if (com_n == COM_PAGE * 1024u) { plat_log("out of COM objects"); abort(); }
    if (!com_pages[com_n / COM_PAGE]) com_pages[com_n / COM_PAGE] = calloc(COM_PAGE, sizeof(void *));
    com_pages[com_n / COM_PAGE][com_n % COM_PAGE] = host_data;
    wr32(obj + 4, com_n++);
    pthread_mutex_unlock(&com_lock);
    return obj;
}

void *com_host(u32 obj) { u32 k = rd32(obj + 4); return com_pages[k / COM_PAGE][k % COM_PAGE]; }

/* ---- handles ----------------------------------------------------------------------------- */
#define HANDLE_N 4096
static struct { int kind; void *data; } handles[HANDLE_N];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

u32 handle_new(int kind, void *data)
{
    u32 h;
    pthread_mutex_lock(&handle_lock);
    for (h = 16; h < HANDLE_N; h++)
        if (handles[h].kind == H_FREE) {
            handles[h].kind = kind;
            handles[h].data = data;
            break;
        }
    pthread_mutex_unlock(&handle_lock);
    if (h == HANDLE_N) { plat_log("out of handles"); abort(); }
    return h * 4;  /* Win32 handles are multiples of 4 */
}

void *handle_get(u32 h, int kind)
{
    h /= 4;
    return h < HANDLE_N && handles[h].kind == kind ? handles[h].data : NULL;
}

void handle_free(u32 h)
{
    h /= 4;
    if (h < HANDLE_N) handles[h].kind = H_FREE;
}

/* ---- paths ------------------------------------------------------------------------------- */
/* "C:\\FCOP\\Missions\\x.mis" or "missions\\x.mis" -> "<game_dir>/missions/X.MIS" as it exists
   on disk (matching each component case-insensitively) */
const char *host_path(const char *gp, char *buf, size_t n)
{
    char tmp[1024], *part, *save = NULL;
    size_t k;
    if (gp[0] && gp[1] == ':') gp += 2;
    snprintf(tmp, sizeof tmp, "%s", gp);
    for (k = 0; tmp[k]; k++) if (tmp[k] == '\\') tmp[k] = '/';
    snprintf(buf, n, "%s", game_dir);
    for (part = strtok_r(tmp, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        DIR *d;
        struct dirent *e;
        const char *found = part;
        if (!strcmp(part, ".")) continue;
        /* the CD keeps the game in a folder "FCopLAPD" ("D:\\FCopLAPD\\Missions\\..."): that folder is game_dir */
        if (!strcmp(buf, game_dir) && !strcasecmp(part, "FCopLAPD")) continue;
        d = opendir(buf);
        if (d) {
            while ((e = readdir(d)))
                if (!strcasecmp(e->d_name, part)) { found = e->d_name; break; }
        }
        k = strlen(buf);
        snprintf(buf + k, n - k, "/%s", found);
        if (d) closedir(d);
    }
    return buf;
}

/* ---- loading and starting the EXE ------------------------------------------------------- */
struct import_ref { u32 iat; const char *dll, *name; };
extern const struct import_ref recomp_imports[];
extern const unsigned recomp_imports_n;

static const struct ImpDef *find_import(const char *dll, const char *name)
{
    static const struct { const char *dll; const struct ImpDef *t; const int *n; } dlls[] = {
        {"KERNEL32.dll", imp_k32_proc, &imp_k32_proc_n}, {"KERNEL32.dll", imp_k32_mem, &imp_k32_mem_n},
        {"KERNEL32.dll", imp_k32_file, &imp_k32_file_n}, {"KERNEL32.dll", imp_k32_sync, &imp_k32_sync_n},
        {"USER32.dll", imp_u32_win, &imp_u32_win_n}, {"USER32.dll", imp_u32_misc, &imp_u32_misc_n},
        {"GDI32.dll", imp_gdi32, &imp_gdi32_n}, {"ADVAPI32.dll", imp_advapi32, &imp_advapi32_n},
        {"ole32.dll", imp_ole32, &imp_ole32_n}, {"WINMM.dll", imp_winmm, &imp_winmm_n},
        {"DDRAW.dll", imp_ddraw, &imp_ddraw_n}, {"DSOUND.dll", imp_dsound, &imp_dsound_n},
        {"DINPUT.dll", imp_dinput, &imp_dinput_n}, {"COMCTL32.dll", imp_comctl32, &imp_comctl32_n},
    };
    unsigned k;
    int j;
    for (k = 0; k < sizeof dlls / sizeof dlls[0]; k++)
        if (!strcasecmp(dlls[k].dll, dll))
            for (j = 0; j < *dlls[k].n; j++)
                if (!strcmp(dlls[k].t[j].name, name)) return &dlls[k].t[j];
    return NULL;
}

/* imports with no implementation yet: rt_foreign reports them by name */
static char missing_names[256][96];
static int missing_n;
static u32 missing_stub(Cpu *c) { (void)c; return 0; }

static void map(u32 lo, u32 hi)
{
    if (mprotect(MEM + lo, hi - lo, PROT_READ | PROT_WRITE)) { perror("mprotect"); exit(1); }
}

static int load_exe(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *buf;
    u32 pe, nsec, opt, k;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    memcpy(&pe, buf + 0x3c, 4);
    nsec = (u32)buf[pe + 6] | (u32)buf[pe + 7] << 8;
    opt = (u32)buf[pe + 20] | (u32)buf[pe + 21] << 8;
    memcpy(MEM + IMAGE_LO, buf, 0x1000);  /* headers */
    for (k = 0; k < nsec; k++) {
        u8 *s = buf + pe + 24 + opt + 40 * k;
        u32 vsize, va, rsize, raw;
        memcpy(&vsize, s + 8, 4); memcpy(&va, s + 12, 4); memcpy(&rsize, s + 16, 4); memcpy(&raw, s + 20, 4);
        memcpy(MEM + IMAGE_LO + va, buf + raw, rsize < vsize || !vsize ? rsize : vsize);
    }
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    static Cpu c0;
    char exe[1200];
    unsigned k;
    int missing = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder containing FCopLAPD.exe>\n", argv[0]);
        return 2;
    }
    snprintf(game_dir, sizeof game_dir, "%s", argv[1]);
    MEM = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap 4GB"); return 1; }
    map(TIB_ADDR, TIB_ADDR + 0x1000);
    map(STACK_LO, STACK_HI);
    map(IMAGE_LO, IMAGE_HI);
    map(ARENA_LO, ARENA_HI);
    host_path("FCopLAPD.exe", exe, sizeof exe);  /* any letter case */
    if (load_exe(exe)) { plat_log("cannot read %s", exe); return 1; }

    for (k = 0; k < recomp_imports_n; k++) {
        const struct import_ref *r = &recomp_imports[k];
        const struct ImpDef *d = find_import(r->dll, r->name);
        if (d) {
            wr32(r->iat, thunk_new(d->name, d->nargs, d->fn));
        } else {
            snprintf(missing_names[missing_n], sizeof missing_names[0], "%s!%s", r->dll, r->name);
            wr32(r->iat, thunk_new(missing_names[missing_n], 0, missing_stub));
            missing_n++;
            missing++;
        }
    }
    plat_log("%u imports, %d not implemented yet", recomp_imports_n, missing);

    /* thread information block: SEH chain end, stack bounds, self pointer */
    wr32(TIB_ADDR + 0x00, 0xffffffffu);
    wr32(TIB_ADDR + 0x04, STACK_HI);
    wr32(TIB_ADDR + 0x08, STACK_LO);
    wr32(TIB_ADDR + 0x18, TIB_ADDR);
    c0.fs_base = TIB_ADDR;
    c0.r[4] = STACK_HI - 64;
    c0.cw = 0x27f;
    cur_cpu = &c0;
    guest_call(&c0, rd32(IMAGE_LO + rd32(IMAGE_LO + 0x3c) + 0x28) + IMAGE_LO, 0, NULL);  /* entry point */
    plat_log("entry point returned (eax %u)", c0.r[0]);
    return (int)c0.r[0];
}
