/* KERNEL32.dll: memory, TLS, interlocked, critical sections */
#include <pthread.h>
#include "host.h"

/* ---- Heaps --------------------------------------------------------------------------- */

/* HANDLE HeapCreate(DWORD flOptions, DWORD dwInitialSize, DWORD dwMaximumSize) */
static u32 imp_HeapCreate(Cpu *c)
{
    (void)c;
    /* Dummy heap handle; all memory comes from the shared guest heap */
    return handle_new(H_HEAP, NULL);
}

/* BOOL HeapDestroy(HANDLE hHeap) */
static u32 imp_HeapDestroy(Cpu *c)
{
    u32 h = ARG(0);
    if (h) handle_free(h);
    return 1;
}

/* LPVOID HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD dwBytes) */
static u32 imp_HeapAlloc(Cpu *c)
{
    (void)c;
    u32 flags = ARG(1);
    u32 bytes = ARG(2);
    (void)flags;
    /* galloc already zeroes allocated memory, satisfying HEAP_ZERO_MEMORY (8) */
    u32 p = galloc(bytes);
    if (!p) {
        last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    return p;
}

/* BOOL HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) */
static u32 imp_HeapFree(Cpu *c)
{
    (void)c;
    u32 p = ARG(2);
    if (p) {
        gfree(p);
    }
    return 1;
}

/* LPVOID HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, DWORD dwBytes) */
static u32 imp_HeapReAlloc(Cpu *c)
{
    (void)c;
    u32 flags = ARG(1);
    u32 p = ARG(2);
    u32 bytes = ARG(3);
    if (!p) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    u32 cur_sz = gsize(p);
    if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (cur_sz >= bytes) return p;
        last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u32 q = grealloc(p, bytes);
    if (!q) {
        last_error = 8;
        return 0;
    }
    return q;
}

/* DWORD HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) */
static u32 imp_HeapSize(Cpu *c)
{
    (void)c;
    u32 p = ARG(2);
    u32 sz = gsize(p);
    if (!p || !sz) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return (u32)-1;
    }
    return sz;
}

/* ---- Virtual Memory ------------------------------------------------------------------ */

/* LPVOID VirtualAlloc(LPVOID lpAddress, DWORD dwSize, DWORD flAllocationType, DWORD flProtect) */
static u32 imp_VirtualAlloc(Cpu *c)
{
    u32 addr = ARG(0);
    u32 size = ARG(1);
    u32 type = ARG(2);
    u32 prot = ARG(3);
    (void)type; (void)prot;
    if (!size) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (addr) {
        /* If an address is provided, return it directly */
        return addr;
    }
    u32 p = galloc_aligned(size, 4096);
    if (!p) {
        last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    return p;
}

/* BOOL VirtualFree(LPVOID lpAddress, DWORD dwSize, DWORD dwFreeType) */
static u32 imp_VirtualFree(Cpu *c)
{
    u32 addr = ARG(0);
    u32 size = ARG(1);
    u32 type = ARG(2);
    (void)size;
    if (!addr) {
        last_error = 87;
        return 0;
    }
    if (type & 0x8000) { /* MEM_RELEASE */
        gfree(addr);
    }
    return 1;
}

/* BOOL VirtualProtect(LPVOID lpAddress, DWORD dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) */
static u32 imp_VirtualProtect(Cpu *c)
{
    u32 addr = ARG(0);
    u32 size = ARG(1);
    u32 new_prot = ARG(2);
    u32 old_prot = ARG(3);
    (void)addr; (void)size; (void)new_prot;
    if (old_prot) {
        wr32(old_prot, 0x40); /* PAGE_EXECUTE_READWRITE */
    }
    return 1;
}

/* ---- Global Memory ------------------------------------------------------------------- */

/* HGLOBAL GlobalAlloc(UINT uFlags, DWORD dwBytes) */
static u32 imp_GlobalAlloc(Cpu *c)
{
    u32 flags = ARG(0);
    u32 bytes = ARG(1);
    (void)flags;
    u32 p = galloc(bytes);
    if (!p) {
        last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    return p;
}

/* HGLOBAL GlobalFree(HGLOBAL hMem) */
static u32 imp_GlobalFree(Cpu *c)
{
    (void)c;
    u32 p = ARG(0);
    if (p) {
        gfree(p);
    }
    return 0; /* Win32 GlobalFree returns NULL on success */
}

/* LPVOID GlobalLock(HGLOBAL hMem) */
static u32 imp_GlobalLock(Cpu *c)
{
    (void)c;
    return ARG(0);
}

/* BOOL GlobalUnlock(HGLOBAL hMem) */
static u32 imp_GlobalUnlock(Cpu *c)
{
    (void)c;
    last_error = 0; /* NO_ERROR */
    return 0;
}

/* HGLOBAL GlobalHandle(LPCVOID pMem) */
static u32 imp_GlobalHandle(Cpu *c)
{
    (void)c;
    return ARG(0);
}

/* ---- Thread Local Storage ------------------------------------------------------------ */

#define TLS_SLOTS 64
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static u8 tls_used[TLS_SLOTS];
static __thread u32 tls_slots[TLS_SLOTS];

/* DWORD TlsAlloc(void) */
static u32 imp_TlsAlloc(Cpu *c)
{
    (void)c;
    pthread_mutex_lock(&tls_lock);
    for (u32 i = 0; i < TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&tls_lock);
    last_error = 14; /* ERROR_OUTOFMEMORY */
    return 0xffffffffu; /* TLS_OUT_OF_INDEXES */
}

/* LPVOID TlsGetValue(DWORD dwTlsIndex) */
static u32 imp_TlsGetValue(Cpu *c)
{
    (void)c;
    u32 idx = ARG(0);
    if (idx >= TLS_SLOTS) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    last_error = 0;
    return tls_slots[idx];
}

/* BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue) */
static u32 imp_TlsSetValue(Cpu *c)
{
    (void)c;
    u32 idx = ARG(0);
    u32 val = ARG(1);
    if (idx >= TLS_SLOTS) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    tls_slots[idx] = val;
    return 1;
}

/* ---- Interlocked Operations ---------------------------------------------------------- */

/* LONG InterlockedIncrement(LPLONG lpAddend) */
static u32 imp_InterlockedIncrement(Cpu *c)
{
    (void)c;
    u32 p = ARG(0);
    if (!p) return 0;
    s32 res = __atomic_add_fetch((s32 *)G(p), 1, __ATOMIC_SEQ_CST);
    return (u32)res;
}

/* LONG InterlockedDecrement(LPLONG lpAddend) */
static u32 imp_InterlockedDecrement(Cpu *c)
{
    (void)c;
    u32 p = ARG(0);
    if (!p) return 0;
    s32 res = __atomic_sub_fetch((s32 *)G(p), 1, __ATOMIC_SEQ_CST);
    return (u32)res;
}

/* ---- Critical Sections --------------------------------------------------------------- */

#define CS_MAX 4096
static pthread_mutex_t cs_table[CS_MAX];
static u8 cs_in_use[CS_MAX];
static pthread_mutex_t cs_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static u32 cs_get_or_alloc(u32 cs_addr)
{
    pthread_mutex_lock(&cs_alloc_lock);
    u32 idx = rd32(cs_addr);
    if (idx >= 1 && idx < CS_MAX && cs_in_use[idx]) {
        pthread_mutex_unlock(&cs_alloc_lock);
        return idx;
    }
    for (u32 i = 1; i < CS_MAX; i++) {
        if (!cs_in_use[i]) {
            cs_in_use[i] = 1;
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_table[i], &attr);
            pthread_mutexattr_destroy(&attr);
            wr32(cs_addr, i);
            pthread_mutex_unlock(&cs_alloc_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&cs_alloc_lock);
    plat_log("out of critical sections");
    abort();
}

/* void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection) */
static u32 imp_InitializeCriticalSection(Cpu *c)
{
    (void)c;
    u32 cs_addr = ARG(0);
    if (!cs_addr) return 0;
    pthread_mutex_lock(&cs_alloc_lock);
    u32 old_idx = rd32(cs_addr);
    if (old_idx >= 1 && old_idx < CS_MAX && cs_in_use[old_idx]) {
        pthread_mutex_destroy(&cs_table[old_idx]);
        cs_in_use[old_idx] = 0;
    }
    memset(G(cs_addr), 0, 24);
    for (u32 i = 1; i < CS_MAX; i++) {
        if (!cs_in_use[i]) {
            cs_in_use[i] = 1;
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_table[i], &attr);
            pthread_mutexattr_destroy(&attr);
            wr32(cs_addr, i);
            pthread_mutex_unlock(&cs_alloc_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&cs_alloc_lock);
    plat_log("out of critical sections");
    abort();
}

/* void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection) */
static u32 imp_EnterCriticalSection(Cpu *c)
{
    (void)c;
    u32 cs_addr = ARG(0);
    if (!cs_addr) return 0;
    u32 idx = cs_get_or_alloc(cs_addr);
    pthread_mutex_lock(&cs_table[idx]);
    return 0;
}

/* void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection) */
static u32 imp_LeaveCriticalSection(Cpu *c)
{
    (void)c;
    u32 cs_addr = ARG(0);
    if (!cs_addr) return 0;
    u32 idx = rd32(cs_addr);
    if (idx >= 1 && idx < CS_MAX && cs_in_use[idx]) {
        pthread_mutex_unlock(&cs_table[idx]);
    }
    return 0;
}

/* void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection) */
static u32 imp_DeleteCriticalSection(Cpu *c)
{
    (void)c;
    u32 cs_addr = ARG(0);
    if (!cs_addr) return 0;
    pthread_mutex_lock(&cs_alloc_lock);
    u32 idx = rd32(cs_addr);
    if (idx >= 1 && idx < CS_MAX && cs_in_use[idx]) {
        pthread_mutex_destroy(&cs_table[idx]);
        cs_in_use[idx] = 0;
        wr32(cs_addr, 0);
    }
    pthread_mutex_unlock(&cs_alloc_lock);
    return 0;
}

/* ---- Import Table -------------------------------------------------------------------- */

const struct ImpDef imp_k32_mem[] = {
    IMPORT(HeapCreate, 3),
    IMPORT(HeapDestroy, 1),
    IMPORT(HeapAlloc, 3),
    IMPORT(HeapFree, 3),
    IMPORT(HeapReAlloc, 4),
    IMPORT(HeapSize, 3),
    IMPORT(VirtualAlloc, 4),
    IMPORT(VirtualFree, 3),
    IMPORT(VirtualProtect, 4),
    IMPORT(GlobalAlloc, 2),
    IMPORT(GlobalFree, 1),
    IMPORT(GlobalLock, 1),
    IMPORT(GlobalUnlock, 1),
    IMPORT(GlobalHandle, 1),
    IMPORT(TlsAlloc, 0),
    IMPORT(TlsGetValue, 1),
    IMPORT(TlsSetValue, 2),
    IMPORT(InterlockedIncrement, 1),
    IMPORT(InterlockedDecrement, 1),
    IMPORT(InitializeCriticalSection, 1),
    IMPORT(EnterCriticalSection, 1),
    IMPORT(LeaveCriticalSection, 1),
    IMPORT(DeleteCriticalSection, 1),
};
IMPORT_TABLE(k32_mem);
