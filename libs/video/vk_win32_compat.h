/* pthread, dlopen and monotonic-clock shim shared by the Vulkan video sources.
 * Windows gets Win32 stand-ins for the calls those sources make; everything else
 * keeps the POSIX headers. Included by rsx_vulkan_backend.c and vk_pipe_queue.c. */
#ifndef VK_WIN32_COMPAT_H
#define VK_WIN32_COMPAT_H

#include <errno.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#  include "ps3emu/ps3_trace.h"   /* ps3_trace_now(): the clock timed waits compare against */
#endif

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * Win32 stand-ins for the POSIX calls this file makes: the mutex / condvar /
 * thread subset (13 functions), dlopen, and a monotonic clock. Declared here so
 * the call sites below stay unchanged. Only the calls in this file are covered.
 * -------------------------------------------------------------------------*/
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
/* SRWLOCK rather than CRITICAL_SECTION: SRWLOCK_INIT is all-zero, so the
 * PTHREAD_MUTEX_INITIALIZER statics below need no runtime init call. */
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
typedef int pthread_condattr_t;
#  define CLOCK_MONOTONIC 1
typedef void* (*vk_thread_fn)(void*);
#  define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t* m, const void* attr)
{ (void)attr; InitializeSRWLock(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t* m)
{ (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t* m)
{ AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t* m)
{ ReleaseSRWLockExclusive(m); return 0; }
static inline int pthread_cond_init(pthread_cond_t* c, const void* attr)
{ (void)attr; InitializeConditionVariable(c); return 0; }
static inline int pthread_cond_destroy(pthread_cond_t* c)
{ (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m)
{ return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1; }
static inline int pthread_condattr_init(pthread_condattr_t* a)
{ *a = 0; return 0; }
static inline int pthread_condattr_setclock(pthread_condattr_t* a, int clock_id)
{ (void)a; (void)clock_id; return 0; }
static inline int pthread_condattr_destroy(pthread_condattr_t* a)
{ (void)a; return 0; }
/* The deadline is absolute on ps3_trace_now()'s clock (QueryPerformanceCounter
 * on Windows), so the relative timeout is recomputed against that same clock. */
static inline int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                         const struct timespec* ts)
{
    const uint64_t now = ps3_trace_now();
    const uint64_t end = (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
    const DWORD ms = end > now ? (DWORD)((end - now + 999999ull) / 1000000ull) : 0;
    if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : -1;
}
static inline int nanosleep(const struct timespec* ts, struct timespec* rem)
{
    (void)rem;
    const DWORD ms = (DWORD)((uint64_t)ts->tv_sec * 1000ull + (uint64_t)ts->tv_nsec / 1000000ull);
    Sleep(ms ? ms : 1);
    return 0;
}
static inline int pthread_cond_signal(pthread_cond_t* c)
{ WakeConditionVariable(c); return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t* c)
{ WakeAllConditionVariable(c); return 0; }

struct vk_thread_start { vk_thread_fn fn; void* arg; };
static DWORD WINAPI vk_thread_trampoline(LPVOID p)
{
    struct vk_thread_start s = *(struct vk_thread_start*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static inline int pthread_create(pthread_t* t, const void* attr, vk_thread_fn fn, void* arg)
{
    (void)attr;
    struct vk_thread_start* s = (struct vk_thread_start*)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, vk_thread_trampoline, s, 0, NULL);
    if (!h) { free(s); return -1; }
    *t = h;
    return 0;
}
static inline int pthread_join(pthread_t t, void** ret)
{
    (void)ret;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

/* Monotonic nanoseconds, the Win32 stand-in for clock_gettime(CLOCK_MONOTONIC). */
static inline int pthread_setname_np(pthread_t t, const char* name)
{ (void)t; (void)name; return 0; }

static inline unsigned long long vk_mono_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#  include <dlfcn.h>
#  include <pthread.h>
static inline unsigned long long vk_mono_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
#endif

#endif /* VK_WIN32_COMPAT_H */
