/*
 * POSIX threading, sched, time and fs calls the runtime makes, mapped onto
 * Win32 on Windows. Include it in place of <pthread.h>, <sched.h> and
 * <unistd.h> in any file that uses these calls. On every other host it only
 * pulls in the POSIX headers, so Linux and macOS build exactly as before.
 *
 * Timed waits take an absolute deadline on the clock_gettime(CLOCK_MONOTONIC)
 * base. On Windows that base is QueryPerformanceCounter, the same base as
 * ps3_trace_now(), so deadlines from either side compare correctly.
 */
#ifndef PS3EMU_POSIX_WIN32_H
#define PS3EMU_POSIX_WIN32_H

#include <errno.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  include <stdlib.h>
#  include <string.h>
#  include <sys/stat.h>

#  ifndef CLOCK_MONOTONIC
#    define CLOCK_MONOTONIC 1
#  endif

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
typedef int pthread_condattr_t;
typedef INIT_ONCE pthread_once_t;
typedef void* (*ps3_thread_fn)(void*);

#  define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#  define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
#  define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
#  define mkdir(path, mode) _mkdir(path)
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp

/* Monotonic nanoseconds. Seconds and remainder are split: counter * 1e9
 * overflows after a few minutes. */
static inline unsigned long long ps3_mono_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)(c.QuadPart / f.QuadPart) * 1000000000ull +
           (unsigned long long)(c.QuadPart % f.QuadPart) * 1000000000ull / (unsigned long long)f.QuadPart;
}

static inline int clock_gettime(int clock_id, struct timespec* ts)
{
    const unsigned long long ns = ps3_mono_ns();
    (void)clock_id;
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

static inline int nanosleep(const struct timespec* ts, struct timespec* rem)
{
    DWORD ms;
    (void)rem;
    if (ts->tv_sec == 0) {
        if (ts->tv_nsec == 0) {
            SwitchToThread();
            return 0;
        }
        if (ts->tv_nsec < 1000000L) {
            /* Sub-millisecond sleep: avoid 1-15ms scheduler penalty on Windows */
            YieldProcessor();
            return 0;
        }
    }
    ms = (DWORD)((uint64_t)ts->tv_sec * 1000ull + (uint64_t)ts->tv_nsec / 1000000ull);
    Sleep(ms ? ms : 1);
    return 0;
}

static inline int sched_yield(void)
{
    SwitchToThread();
    return 0;
}

static inline pthread_t pthread_self(void)
{
    return GetCurrentThread();
}

static inline int pthread_equal(pthread_t t1, pthread_t t2)
{
    if (t1 == t2) return 1;
    return GetThreadId(t1) == GetThreadId(t2);
}

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
static inline int pthread_cond_signal(pthread_cond_t* c)
{ WakeConditionVariable(c); return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t* c)
{ WakeAllConditionVariable(c); return 0; }
static inline int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                         const struct timespec* ts)
{
    const unsigned long long now = ps3_mono_ns();
    const unsigned long long end = (unsigned long long)ts->tv_sec * 1000000000ull + (unsigned long long)ts->tv_nsec;
    const DWORD ms = end > now ? (DWORD)((end - now + 999999ull) / 1000000ull) : 0;
    if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : -1;
}
static inline int pthread_condattr_init(pthread_condattr_t* a)
{ *a = 0; return 0; }
static inline int pthread_condattr_setclock(pthread_condattr_t* a, int clock_id)
{ (void)a; (void)clock_id; return 0; }
static inline int pthread_condattr_destroy(pthread_condattr_t* a)
{ (void)a; return 0; }

static BOOL CALLBACK ps3_once_thunk(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    (void)once; (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}
static inline int pthread_once(pthread_once_t* once, void (*fn)(void))
{
    return InitOnceExecuteOnce(once, ps3_once_thunk, (PVOID)fn, NULL) ? 0 : -1;
}

struct ps3_thread_start { ps3_thread_fn fn; void* arg; };
static DWORD WINAPI ps3_thread_trampoline(LPVOID p)
{
    struct ps3_thread_start s = *(struct ps3_thread_start*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static inline int pthread_create(pthread_t* t, const void* attr, ps3_thread_fn fn, void* arg)
{
    struct ps3_thread_start* s;
    HANDLE h;
    (void)attr;
    s = (struct ps3_thread_start*)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    h = CreateThread(NULL, 0, ps3_thread_trampoline, s, 0, NULL);
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
static inline int pthread_setname_np(pthread_t t, const char* name)
{ (void)t; (void)name; return 0; }

#else
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>
#endif

#endif /* PS3EMU_POSIX_WIN32_H */
