// The handful of Win32 calls this project uses, on every platform.
//
// The search driver, the solvers and the estimator were written against
// <windows.h> directly -- CreateThread, critical sections, Interlocked*, two
// clocks -- so nothing here built outside Windows. That ruled out exactly the
// machines serious seedfinding runs on: rented Linux boxes, cloud instances,
// distributed volunteers. On Windows this is <windows.h> and nothing else; on
// POSIX it maps the same names onto pthreads, C11 atomics builtins and
// clock_gettime, so the calling code stays byte-for-byte as it was.
#pragma once

#ifdef _WIN32
#include <windows.h>
#else

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef int64_t  LONG64;
typedef uint64_t ULONGLONG;
typedef void    *LPVOID;
typedef int      BOOL;
#define WINAPI
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#define INFINITE 0xFFFFFFFFu

typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwNumberOfProcessors; } SYSTEM_INFO;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

// ---- threads: a HANDLE owns its pthread; Wait joins, CloseHandle frees.
typedef struct PlatThread {
    pthread_t t;
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
    int joined;
} *HANDLE;

static void *plat_thread_tramp(void *p)
{
    HANDLE h = (HANDLE)p;
    h->fn(h->arg);
    return NULL;
}

static inline HANDLE CreateThread(void *attr, size_t stack, LPTHREAD_START_ROUTINE fn,
                                  LPVOID arg, DWORD flags, DWORD *tid)
{
    (void)attr; (void)stack; (void)flags; (void)tid;
    HANDLE h = (HANDLE)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fn = fn; h->arg = arg;
    if (pthread_create(&h->t, NULL, plat_thread_tramp, h) != 0) { free(h); return NULL; }
    return h;
}

static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms;
    if (h && !h->joined) { pthread_join(h->t, NULL); h->joined = 1; }
    return 0;
}

static inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms)
{
    (void)all;
    for (DWORD i = 0; i < n; i++) WaitForSingleObject(hs[i], ms);
    return 0;
}

static inline BOOL CloseHandle(HANDLE h)
{
    if (!h) return FALSE;
    WaitForSingleObject(h, INFINITE);   // every caller has waited already
    free(h);
    return TRUE;
}

// ---- locks
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_init(c, NULL); }
static inline void EnterCriticalSection(CRITICAL_SECTION *c)      { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c)      { pthread_mutex_unlock(c); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c)     { pthread_mutex_destroy(c); }

// ---- atomics (Win32 Interlocked* return the NEW value, as these do)
#define InterlockedIncrement(p)    __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p)  __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedAdd64(p, v)     __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v)  __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

// ---- clocks
static inline void Sleep(DWORD ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static inline ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000);
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

// ---- system
static inline void GetSystemInfo(SYSTEM_INFO *si)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = (DWORD)(n > 0 ? n : 1);
}

#endif // _WIN32
