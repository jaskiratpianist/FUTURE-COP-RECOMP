/* KERNEL32.dll: k32_sync
 * Time, threads, events, mutexes, waits and handle duplication.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "host.h"

#define WAIT_OBJECT_0    0x00000000u
#define WAIT_TIMEOUT     0x00000102u
#define WAIT_FAILED      0xffffffffu
#define INFINITE         0xffffffffu

#define THREAD_PRIORITY_ERROR_RETURN 0x7fffffff

/* Synchronization structures */
typedef struct GuestEvent {
    int manual_reset;
    int signaled;
    int pulse_count;
    int waiters;
} GuestEvent;

typedef struct GuestMutex {
    u32 owner_tid;
    int recursion_count;
} GuestMutex;

typedef struct GuestThread {
    u32 handle;
    u32 tid;
    pthread_t pt;
    Cpu cpu;
    u32 start_addr;
    u32 param;
    u32 stack_lo;
    u32 stack_hi;
    u32 tib;
    int suspend_count;
    int priority;
    int done;
    u32 exit_code;
} GuestThread;

/* Global synchronization lock and condition variable */
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sync_cond = PTHREAD_COND_INITIALIZER;

static __thread GuestThread *cur_thread = NULL;
static __thread u32 this_thread_id = 0;

static GuestThread main_thread_obj;
static int main_thread_initialized = 0;
static u32 next_tid_counter = 1000;

static u32 cur_thread_tid(void)
{
    if (this_thread_id) return this_thread_id;
    if (cur_thread && cur_thread->tid) {
        this_thread_id = cur_thread->tid;
        return this_thread_id;
    }
    this_thread_id = __sync_fetch_and_add(&next_tid_counter, 1);
    return this_thread_id;
}

static GuestThread *get_cur_thread(void)
{
    if (cur_thread) return cur_thread;
    if (!main_thread_initialized) {
        main_thread_initialized = 1;
        main_thread_obj.tid = cur_thread_tid();
        main_thread_obj.pt = pthread_self();
        main_thread_obj.priority = 0;
        main_thread_obj.done = 0;
        main_thread_obj.handle = handle_new(H_THREAD, &main_thread_obj);
    }
    return &main_thread_obj;
}

/* Named objects support */
#define MAX_NAMED 64
static struct { char name[64]; u32 handle; } named_events[MAX_NAMED];
static int named_events_count = 0;

static struct { char name[64]; u32 handle; } named_mutexes[MAX_NAMED];
static int named_mutexes_count = 0;

/* Thread trampoline */
static void *thread_trampoline(void *arg)
{
    GuestThread *t = (GuestThread *)arg;
    cur_thread = t;
    cur_cpu = &t->cpu;
    this_thread_id = t->tid;

    pthread_mutex_lock(&sync_lock);
    while (t->suspend_count > 0 && !t->done) {
        pthread_cond_wait(&sync_cond, &sync_lock);
    }
    if (t->done) {
        pthread_mutex_unlock(&sync_lock);
        return NULL;
    }
    pthread_mutex_unlock(&sync_lock);

    u32 ret = guest_call(&t->cpu, t->start_addr, 1, &t->param);

    pthread_mutex_lock(&sync_lock);
    t->done = 1;
    t->exit_code = ret;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);

    return NULL;
}

/* Helper to resolve and validate a waitable handle */
static int check_handle(u32 h, int *out_kind, void **out_obj)
{
    if (h == (u32)-2 || h == (u32)-1) {
        *out_kind = H_THREAD;
        *out_obj = get_cur_thread();
        return 1;
    }
    void *obj;
    if ((obj = handle_get(h, H_EVENT)) != NULL) {
        *out_kind = H_EVENT;
        *out_obj = obj;
        return 1;
    }
    if ((obj = handle_get(h, H_MUTEX)) != NULL) {
        *out_kind = H_MUTEX;
        *out_obj = obj;
        return 1;
    }
    if ((obj = handle_get(h, H_THREAD)) != NULL) {
        *out_kind = H_THREAD;
        *out_obj = obj;
        return 1;
    }
    return 0;
}

static int is_object_signaled(int kind, void *obj, u32 cur_tid)
{
    if (kind == H_EVENT) {
        GuestEvent *ev = (GuestEvent *)obj;
        return (ev->signaled || ev->pulse_count > 0);
    } else if (kind == H_MUTEX) {
        GuestMutex *m = (GuestMutex *)obj;
        return (m->owner_tid == 0 || m->owner_tid == cur_tid);
    } else if (kind == H_THREAD) {
        GuestThread *t = (GuestThread *)obj;
        return (t->done != 0);
    }
    return 0;
}

static void take_object(int kind, void *obj, u32 cur_tid)
{
    if (kind == H_EVENT) {
        GuestEvent *ev = (GuestEvent *)obj;
        if (ev->pulse_count > 0) {
            ev->pulse_count--;
        } else if (!ev->manual_reset) {
            ev->signaled = 0;
        }
    } else if (kind == H_MUTEX) {
        GuestMutex *m = (GuestMutex *)obj;
        m->owner_tid = cur_tid;
        m->recursion_count++;
    }
}

static int check_condition(u32 count, const int *kinds, void **objs, u32 cur_tid, int wait_all, u32 *out_idx)
{
    if (wait_all) {
        for (u32 i = 0; i < count; i++) {
            if (!is_object_signaled(kinds[i], objs[i], cur_tid))
                return 0;
        }
        *out_idx = 0;
        return 1;
    } else {
        for (u32 i = 0; i < count; i++) {
            if (is_object_signaled(kinds[i], objs[i], cur_tid)) {
                *out_idx = i;
                return 1;
            }
        }
        return 0;
    }
}

static u32 do_wait(u32 count, const u32 *handles, int wait_all, u32 timeout_ms)
{
    int kinds[64];
    void *objs[64];
    u32 cur_tid = cur_thread_tid();

    pthread_mutex_lock(&sync_lock);

    for (u32 i = 0; i < count; i++) {
        if (!check_handle(handles[i], &kinds[i], &objs[i])) {
            pthread_mutex_unlock(&sync_lock);
            last_error = 6; /* ERROR_INVALID_HANDLE */
            return WAIT_FAILED;
        }
    }

    if (wait_all && count > 1) {
        for (u32 i = 0; i < count; i++) {
            for (u32 j = i + 1; j < count; j++) {
                if (handles[i] == handles[j]) {
                    pthread_mutex_unlock(&sync_lock);
                    last_error = 87; /* ERROR_INVALID_PARAMETER */
                    return WAIT_FAILED;
                }
            }
        }
    }

    u32 satisfied_idx = 0;
    if (check_condition(count, kinds, objs, cur_tid, wait_all, &satisfied_idx)) {
        if (wait_all) {
            for (u32 i = 0; i < count; i++) {
                take_object(kinds[i], objs[i], cur_tid);
            }
            pthread_mutex_unlock(&sync_lock);
            return WAIT_OBJECT_0;
        } else {
            take_object(kinds[satisfied_idx], objs[satisfied_idx], cur_tid);
            pthread_mutex_unlock(&sync_lock);
            return WAIT_OBJECT_0 + satisfied_idx;
        }
    }

    if (timeout_ms == 0) {
        pthread_mutex_unlock(&sync_lock);
        return WAIT_TIMEOUT;
    }

    for (u32 i = 0; i < count; i++) {
        if (kinds[i] == H_EVENT) {
            ((GuestEvent *)objs[i])->waiters++;
        }
    }

    int timed_out = 0;
    if (timeout_ms == INFINITE) {
        while (!check_condition(count, kinds, objs, cur_tid, wait_all, &satisfied_idx)) {
            pthread_cond_wait(&sync_cond, &sync_lock);
        }
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        while (!check_condition(count, kinds, objs, cur_tid, wait_all, &satisfied_idx)) {
            int r = pthread_cond_timedwait(&sync_cond, &sync_lock, &deadline);
            if (r == ETIMEDOUT) {
                if (!check_condition(count, kinds, objs, cur_tid, wait_all, &satisfied_idx)) {
                    timed_out = 1;
                    break;
                }
            }
        }
    }

    for (u32 i = 0; i < count; i++) {
        if (kinds[i] == H_EVENT) {
            ((GuestEvent *)objs[i])->waiters--;
        }
    }

    if (timed_out) {
        pthread_mutex_unlock(&sync_lock);
        return WAIT_TIMEOUT;
    }

    if (wait_all) {
        for (u32 i = 0; i < count; i++) {
            take_object(kinds[i], objs[i], cur_tid);
        }
        pthread_mutex_unlock(&sync_lock);
        return WAIT_OBJECT_0;
    } else {
        take_object(kinds[satisfied_idx], objs[satisfied_idx], cur_tid);
        pthread_mutex_unlock(&sync_lock);
        return WAIT_OBJECT_0 + satisfied_idx;
    }
}

/* VOID Sleep(DWORD dwMilliseconds) */
static u32 imp_Sleep(Cpu *c)
{
    u32 ms = ARG(0);
    if (ms == 0) {
        sched_yield();
    } else {
        struct timespec req;
        req.tv_sec = ms / 1000;
        req.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&req, NULL);
    }
    return 0;
}

/* DWORD SleepEx(DWORD dwMilliseconds, BOOL bAlertable) */
static u32 imp_SleepEx(Cpu *c)
{
    u32 ms = ARG(0);
    if (ms == 0) {
        sched_yield();
    } else {
        struct timespec req;
        req.tv_sec = ms / 1000;
        req.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&req, NULL);
    }
    return 0;
}

/* BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) */
static u32 imp_QueryPerformanceFrequency(Cpu *c)
{
    u32 out = ARG(0);
    if (!out) {
        last_error = 87;
        return 0;
    }
    wr32(out, 1000000u);
    wr32(out + 4, 0);
    return 1;
}

/* BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) */
static u32 imp_QueryPerformanceCounter(Cpu *c)
{
    u32 out = ARG(0);
    if (!out) {
        last_error = 87;
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 us = (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000ull;
    wr32(out, (u32)us);
    wr32(out + 4, (u32)(us >> 32));
    return 1;
}

/* DWORD GetTickCount(VOID) */
static u32 imp_GetTickCount(Cpu *c)
{
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 ms = (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
    return (u32)ms;
}

/* HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                       LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                       DWORD dwCreationFlags, LPDWORD lpThreadId) */
static u32 imp_CreateThread(Cpu *c)
{
    u32 stack_size = ARG(1);
    u32 start_addr = ARG(2);
    u32 param = ARG(3);
    u32 flags = ARG(4);
    u32 tid_ptr = ARG(5);

    if (stack_size < 1024 * 1024)
        stack_size = 1024 * 1024;
    stack_size = (stack_size + 0xfffu) & ~0xfffu;

    GuestThread *t = (GuestThread *)calloc(1, sizeof(GuestThread));
    if (!t) {
        last_error = 8;
        return 0;
    }

    u32 stack_lo = galloc(stack_size);
    if (!stack_lo) {
        free(t);
        last_error = 8;
        return 0;
    }
    u32 stack_hi = stack_lo + stack_size;

    u32 tib = galloc(0x1000);
    if (!tib) {
        gfree(stack_lo);
        free(t);
        last_error = 8;
        return 0;
    }

    u32 tid = __sync_fetch_and_add(&next_tid_counter, 1);

    wr32(tib + 0x00, 0xffffffffu);
    wr32(tib + 0x04, stack_hi);
    wr32(tib + 0x08, stack_lo);
    wr32(tib + 0x18, tib);
    wr32(tib + 0x24, tid);

    t->tid = tid;
    t->start_addr = start_addr;
    t->param = param;
    t->stack_lo = stack_lo;
    t->stack_hi = stack_hi;
    t->tib = tib;
    t->priority = 0;
    t->done = 0;
    t->exit_code = 0;
    t->suspend_count = (flags & 4 /* CREATE_SUSPENDED */) ? 1 : 0;

    t->cpu.cw = 0x27f;
    t->cpu.fs_base = tib;
    t->cpu.r[4] = stack_hi - 64;

    u32 h = handle_new(H_THREAD, t);
    t->handle = h;

    if (tid_ptr) {
        wr32(tid_ptr, tid);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (pthread_create(&t->pt, &attr, thread_trampoline, t) != 0) {
        pthread_attr_destroy(&attr);
        handle_free(h);
        gfree(tib);
        gfree(stack_lo);
        free(t);
        last_error = 8;
        return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(t->pt);

    return h;
}

/* VOID ExitThread(DWORD dwExitCode) */
static u32 imp_ExitThread(Cpu *c)
{
    u32 code = ARG(0);
    pthread_mutex_lock(&sync_lock);
    GuestThread *t = cur_thread;
    if (!t) t = get_cur_thread();
    if (t) {
        t->done = 1;
        t->exit_code = code;
    }
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);

    if (!cur_thread) {
        exit((int)code);
    }
    pthread_exit(NULL);
    return 0;
}

/* DWORD ResumeThread(HANDLE hThread) */
static u32 imp_ResumeThread(Cpu *c)
{
    u32 h = ARG(0);
    GuestThread *t = (GuestThread *)handle_get(h, H_THREAD);
    if (!t) {
        last_error = 6;
        return (u32)-1;
    }
    pthread_mutex_lock(&sync_lock);
    u32 prev = (u32)t->suspend_count;
    if (t->suspend_count > 0) {
        t->suspend_count--;
        if (t->suspend_count == 0) {
            pthread_cond_broadcast(&sync_cond);
        }
    }
    pthread_mutex_unlock(&sync_lock);
    return prev;
}

/* BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode) */
static u32 imp_TerminateThread(Cpu *c)
{
    u32 h = ARG(0);
    u32 code = ARG(1);
    GuestThread *t = (GuestThread *)handle_get(h, H_THREAD);
    if (!t) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&sync_lock);
    if (!t->done) {
        t->done = 1;
        t->exit_code = code;
        pthread_cancel(t->pt);
        pthread_cond_broadcast(&sync_cond);
    }
    pthread_mutex_unlock(&sync_lock);
    return 1;
}

/* BOOL SetThreadPriority(HANDLE hThread, int nPriority) */
static u32 imp_SetThreadPriority(Cpu *c)
{
    u32 h = ARG(0);
    int prio = (int)ARG(1);
    GuestThread *t = NULL;
    if (h == (u32)-2 || h == (u32)-1) {
        t = get_cur_thread();
    } else {
        t = (GuestThread *)handle_get(h, H_THREAD);
    }
    if (!t) {
        last_error = 6;
        return 0;
    }
    t->priority = prio;
    return 1;
}

/* int GetThreadPriority(HANDLE hThread) */
static u32 imp_GetThreadPriority(Cpu *c)
{
    u32 h = ARG(0);
    GuestThread *t = NULL;
    if (h == (u32)-2 || h == (u32)-1) {
        t = get_cur_thread();
    } else {
        t = (GuestThread *)handle_get(h, H_THREAD);
    }
    if (!t) {
        last_error = 6;
        return THREAD_PRIORITY_ERROR_RETURN;
    }
    return (u32)t->priority;
}

/* HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                       BOOL bInitialState, LPCSTR lpName) */
static u32 imp_CreateEventA(Cpu *c)
{
    u32 manual = ARG(1);
    u32 initial = ARG(2);
    u32 name_ptr = ARG(3);

    pthread_mutex_lock(&sync_lock);
    if (name_ptr && rd8(name_ptr)) {
        const char *name = GSTR(name_ptr);
        for (int i = 0; i < named_events_count; i++) {
            if (strcmp(named_events[i].name, name) == 0) {
                u32 h = named_events[i].handle;
                pthread_mutex_unlock(&sync_lock);
                last_error = 183; /* ERROR_ALREADY_EXISTS */
                return h;
            }
        }
    }

    GuestEvent *ev = (GuestEvent *)calloc(1, sizeof(GuestEvent));
    if (!ev) {
        pthread_mutex_unlock(&sync_lock);
        last_error = 8;
        return 0;
    }
    ev->manual_reset = manual ? 1 : 0;
    ev->signaled = initial ? 1 : 0;
    u32 h = handle_new(H_EVENT, ev);

    if (name_ptr && rd8(name_ptr) && named_events_count < MAX_NAMED) {
        strncpy(named_events[named_events_count].name, GSTR(name_ptr), 63);
        named_events[named_events_count].name[63] = 0;
        named_events[named_events_count].handle = h;
        named_events_count++;
    }

    pthread_mutex_unlock(&sync_lock);
    last_error = 0;
    return h;
}

/* BOOL SetEvent(HANDLE hEvent) */
static u32 imp_SetEvent(Cpu *c)
{
    u32 h = ARG(0);
    GuestEvent *ev = (GuestEvent *)handle_get(h, H_EVENT);
    if (!ev) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&sync_lock);
    ev->signaled = 1;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    return 1;
}

/* events for host code (DirectShow's media event): same objects as CreateEventA */
u32 sync_event_new(int manual, int initial)
{
    GuestEvent *ev = (GuestEvent *)calloc(1, sizeof(GuestEvent));
    if (!ev) return 0;
    ev->manual_reset = manual ? 1 : 0;
    ev->signaled = initial ? 1 : 0;
    return handle_new(H_EVENT, ev);
}

void sync_event_set(u32 h, int on)
{
    GuestEvent *ev = (GuestEvent *)handle_get(h, H_EVENT);
    if (!ev) return;
    pthread_mutex_lock(&sync_lock);
    ev->signaled = on ? 1 : 0;
    if (on) pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}

/* BOOL ResetEvent(HANDLE hEvent) */
static u32 imp_ResetEvent(Cpu *c)
{
    u32 h = ARG(0);
    GuestEvent *ev = (GuestEvent *)handle_get(h, H_EVENT);
    if (!ev) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&sync_lock);
    ev->signaled = 0;
    ev->pulse_count = 0;
    pthread_mutex_unlock(&sync_lock);
    return 1;
}

/* BOOL PulseEvent(HANDLE hEvent) */
static u32 imp_PulseEvent(Cpu *c)
{
    u32 h = ARG(0);
    GuestEvent *ev = (GuestEvent *)handle_get(h, H_EVENT);
    if (!ev) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&sync_lock);
    if (ev->waiters > 0) {
        ev->pulse_count = ev->manual_reset ? ev->waiters : 1;
        pthread_cond_broadcast(&sync_cond);
    } else {
        ev->pulse_count = 0;
    }
    ev->signaled = 0;
    pthread_mutex_unlock(&sync_lock);
    return 1;
}

/* HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner,
                       LPCSTR lpName) */
static u32 imp_CreateMutexA(Cpu *c)
{
    u32 initial_owner = ARG(1);
    u32 name_ptr = ARG(2);

    pthread_mutex_lock(&sync_lock);
    if (name_ptr && rd8(name_ptr)) {
        const char *name = GSTR(name_ptr);
        for (int i = 0; i < named_mutexes_count; i++) {
            if (strcmp(named_mutexes[i].name, name) == 0) {
                u32 h = named_mutexes[i].handle;
                pthread_mutex_unlock(&sync_lock);
                last_error = 183; /* ERROR_ALREADY_EXISTS */
                return h;
            }
        }
    }

    GuestMutex *m = (GuestMutex *)calloc(1, sizeof(GuestMutex));
    if (!m) {
        pthread_mutex_unlock(&sync_lock);
        last_error = 8;
        return 0;
    }
    if (initial_owner) {
        m->owner_tid = cur_thread_tid();
        m->recursion_count = 1;
    }
    u32 h = handle_new(H_MUTEX, m);

    if (name_ptr && rd8(name_ptr) && named_mutexes_count < MAX_NAMED) {
        strncpy(named_mutexes[named_mutexes_count].name, GSTR(name_ptr), 63);
        named_mutexes[named_mutexes_count].name[63] = 0;
        named_mutexes[named_mutexes_count].handle = h;
        named_mutexes_count++;
    }

    pthread_mutex_unlock(&sync_lock);
    last_error = 0;
    return h;
}

/* DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) */
static u32 imp_WaitForSingleObject(Cpu *c)
{
    u32 h = ARG(0);
    u32 timeout = ARG(1);
    return do_wait(1, &h, 0, timeout);
}

/* DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
                                BOOL bWaitAll, DWORD dwMilliseconds) */
static u32 imp_WaitForMultipleObjects(Cpu *c)
{
    u32 count = ARG(0);
    u32 handles_ptr = ARG(1);
    u32 wait_all = ARG(2);
    u32 timeout = ARG(3);

    if (count == 0 || count > 64) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return WAIT_FAILED;
    }
    u32 handles[64];
    for (u32 i = 0; i < count; i++) {
        handles[i] = rd32(handles_ptr + 4 * i);
    }
    return do_wait(count, handles, wait_all ? 1 : 0, timeout);
}

/* DWORD WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles,
                                  BOOL bWaitAll, DWORD dwMilliseconds,
                                  BOOL bAlertable) */
static u32 imp_WaitForMultipleObjectsEx(Cpu *c)
{
    u32 count = ARG(0);
    u32 handles_ptr = ARG(1);
    u32 wait_all = ARG(2);
    u32 timeout = ARG(3);
    /* ARG(4) = bAlertable ignored */

    if (count == 0 || count > 64) {
        last_error = 87;
        return WAIT_FAILED;
    }
    u32 handles[64];
    for (u32 i = 0; i < count; i++) {
        handles[i] = rd32(handles_ptr + 4 * i);
    }
    return do_wait(count, handles, wait_all ? 1 : 0, timeout);
}

/* BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                        HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle,
                        DWORD dwDesiredAccess, BOOL bInheritHandle,
                        DWORD dwOptions) */
static u32 imp_DuplicateHandle(Cpu *c)
{
    u32 src_h = ARG(1);
    u32 out_ptr = ARG(3);
    if (!out_ptr) {
        last_error = 87;
        return 0;
    }
    u32 res = src_h;
    if (src_h == (u32)-2) {
        GuestThread *t = get_cur_thread();
        if (t && t->handle) res = t->handle;
    }
    wr32(out_ptr, res);
    return 1;
}

const struct ImpDef imp_k32_sync[] = {
    IMPORT(Sleep, 1),
    IMPORT(SleepEx, 2),
    IMPORT(QueryPerformanceFrequency, 1),
    IMPORT(QueryPerformanceCounter, 1),
    IMPORT(GetTickCount, 0),
    IMPORT(CreateThread, 6),
    IMPORT(ExitThread, 1),
    IMPORT(ResumeThread, 1),
    IMPORT(TerminateThread, 2),
    IMPORT(SetThreadPriority, 2),
    IMPORT(GetThreadPriority, 1),
    IMPORT(CreateEventA, 4),
    IMPORT(SetEvent, 1),
    IMPORT(ResetEvent, 1),
    IMPORT(PulseEvent, 1),
    IMPORT(CreateMutexA, 3),
    IMPORT(WaitForSingleObject, 2),
    IMPORT(WaitForMultipleObjects, 4),
    IMPORT(WaitForMultipleObjectsEx, 5),
    IMPORT(DuplicateHandle, 7),
};
IMPORT_TABLE(k32_sync);
