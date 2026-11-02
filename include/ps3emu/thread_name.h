/*
 * ps3_thread_name: name the calling host thread ("ppu main", "spu <group>:0",
 * "audio mix", ...), so a debugger, a profiler or a per-thread CPU table can
 * tell the guest's PPU and SPU threads and the runtime's own threads apart.
 *
 * The OS name is Windows (SetThreadDescription, looked up at run time) and
 * Linux/macOS (pthread_setname_np; glibc's prototype needs _GNU_SOURCE before
 * the first pthread.h, which a header included this late cannot promise, so
 * the Linux prototype is declared here when the libc has not exposed it).
 * Everywhere, the name is also the thread's track in a PS3RECOMP_TRACE
 * timeline (ps3_trace.h). A weak ps3_thread_pin_by_name() hook lets
 * PS3RECOMP_PIN map the name to a CPU mask without making this header depend
 * on the platform shim.
 */
#ifndef PS3EMU_THREAD_NAME_H
#define PS3EMU_THREAD_NAME_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ps3emu/ps3_trace.h"

#ifdef _WIN32
#include <windows.h>

static inline void ps3_thread_name(const char* fmt, ...)
{
    typedef HRESULT (WINAPI *set_desc_fn)(HANDLE, PCWSTR);
    static set_desc_fn set_desc;
    static int looked_up;
    if (!looked_up) {
        set_desc = (set_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                      "SetThreadDescription");
        looked_up = 1;
    }
    if (!set_desc && !ps3_trace_on()) return;
    char s[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    ps3_trace_thread_name(s);
    if (!set_desc) return;
    WCHAR w[64];
    int i = 0;
    for (; s[i] && i < 63; i++) w[i] = (WCHAR)(unsigned char)s[i];
    w[i] = 0;
    set_desc(GetCurrentThread(), w);
}
#else
#include <pthread.h>
#if defined(__linux__) && !defined(__USE_GNU)
/* glibc exports it; its prototype is hidden without _GNU_SOURCE, which the
 * translation unit may have included pthread.h without. */
extern int pthread_setname_np(pthread_t, const char*);
#endif
#if defined(__GNUC__) && !defined(_WIN32)
#ifdef __cplusplus
extern "C" {
#endif
extern void ps3_thread_pin_by_name(const char*) __attribute__((weak));
#ifdef __cplusplus
}
#endif
#endif

static inline void ps3_thread_name(const char* fmt, ...)
{
    char s[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (ps3_trace_on()) ps3_trace_thread_name(s);
#ifdef __APPLE__
    {
        char n[16];
        snprintf(n, sizeof n, "%s", s);
        pthread_setname_np(n);
    }
#elif defined(__linux__)
    {
        char n[16];   /* Linux task comm is 15 characters + NUL */
        snprintf(n, sizeof n, "%s", s);
        pthread_setname_np(pthread_self(), n);
    }
#endif
#ifdef __GNUC__
    if (ps3_thread_pin_by_name) ps3_thread_pin_by_name(s);
#endif
}
#endif

#endif /* PS3EMU_THREAD_NAME_H */
