/*
 * ps3recomp - guest-memory coherence for render targets read by the CPU/SPU
 *
 * See rsx_wcb.h for what this is and why. This file has three parts: the
 * region table (a spinlock-guarded array, small enough to scan), the page
 * protection and forced-store primitives per host, and the fault handler.
 *
 * Not compiled on its own: rsx_draw_engine.c #includes it (CMake excludes
 * it from the source glob), so every build of the engine carries it.
 *
 * Everything a faulting thread runs is kept to what is reasonable inside a
 * synchronous SIGSEGV handler: a spinlock rather than a mutex, mprotect,
 * clock_gettime, sched_yield and nanosleep for the wait. The fault is
 * synchronous -- raised by the thread's own load or store of a guest page,
 * never while it is inside this module or an allocator -- which is what makes
 * blocking in it sound in practice.
 */
#include "rsx_wcb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define WCB_TLS __declspec(thread)
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <sched.h>
  #include <signal.h>
  #include <sys/mman.h>
  #include <time.h>
  #include <unistd.h>
  #define WCB_TLS _Thread_local
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h>
  #define WCB_LOAD(p)        (*(volatile long*)(p))
  #define WCB_ADD64(p, v)    InterlockedExchangeAdd64((volatile LONG64*)(p), (LONG64)(v))
  static int wcb_cas_lock(volatile long* l) { return InterlockedCompareExchange(l, 1, 0) == 0; }
  static void wcb_unlock_word(volatile long* l) { InterlockedExchange(l, 0); }
  typedef volatile long wcb_lock_t;
#else
  #define WCB_LOAD(p)        __atomic_load_n((p), __ATOMIC_ACQUIRE)
  #define WCB_ADD64(p, v)    __atomic_fetch_add((p), (uint64_t)(v), __ATOMIC_RELAXED)
  typedef int wcb_lock_t;
  static int wcb_cas_lock(volatile int* l)
  {
      int expect = 0;
      return __atomic_compare_exchange_n(l, &expect, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }
  static void wcb_unlock_word(volatile int* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
#endif

/* ---- small host primitives ---------------------------------------------- */

static void wcb_yield(unsigned spin)
{
#ifdef _WIN32
    if (spin < 64) SwitchToThread(); else Sleep(spin < 2000 ? 0 : 1);
#else
    if (spin < 64) { sched_yield(); return; }
    struct timespec ts = { 0, spin < 2000 ? 20000 : 200000 };   /* 20 us, then 200 us */
    nanosleep(&ts, NULL);
#endif
}

static uint64_t wcb_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uintptr_t wcb_page_size(void)
{
    static uintptr_t s_page;
    if (!s_page) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s_page = si.dwPageSize ? si.dwPageSize : 4096u;
#else
        long v = sysconf(_SC_PAGESIZE);
        s_page = v > 0 ? (uintptr_t)v : 4096u;
#endif
    }
    return s_page;
}

/* Texture page-dirty tracking (PS3RECOMP_TEX_DIRTY) lives in this file too:
 * it shares the region table's page-protection state and the one fault
 * handler. These are defined at the bottom; the region code calls them while
 * it holds the lock. */
extern uint8_t* vm_base;
extern void (*volatile g_ps3_guest_write_hook)(const void* host, size_t len);
static int  td_page_wants_ro(uintptr_t page);       /* lock held */
static void td_open_note_locked(uintptr_t lo, uintptr_t hi, int ro);
static void td_forget_prot_locked(uintptr_t lo, uintptr_t hi);
int  rsx_texdirty_on_fault(uintptr_t addr);
static int td_late_fault(uintptr_t addr);
static void td_install_faults(void);

/* 0 on success. `open` restores read/write: render targets live in ordinary
 * read/write guest memory, which is the only kind the engine arms. TEX_DIRTY
 * texture pages stay read-only when reopened, so their writes keep faulting. */
static int wcb_set_access_prot(uintptr_t lo, uintptr_t hi, int prot)
{
    if (hi <= lo) return 0;
#ifdef _WIN32
    DWORD old;
    DWORD p = prot == 0 ? PAGE_NOACCESS : (prot == 1 ? PAGE_READONLY : PAGE_READWRITE);
    return VirtualProtect((void*)lo, (SIZE_T)(hi - lo), p, &old) ? 0 : -1;
#else
    return mprotect((void*)lo, (size_t)(hi - lo), prot == 0 ? PROT_NONE
                                                    : (prot == 1 ? PROT_READ
                                                                 : (PROT_READ | PROT_WRITE)));
#endif
}

static int wcb_set_access(uintptr_t lo, uintptr_t hi, int open)
{
    return wcb_set_access_prot(lo, hi, open ? 2 : 0);
}

/* ---- state -------------------------------------------------------------- */

enum { WCB_FREE = 0, WCB_PENDING, WCB_REQUESTED, WCB_RESOLVING };

typedef struct {
    uint32_t  cookie;
    int       state;
    uint32_t  gen;          /* bumped at every release: a waiter's ticket */
    uintptr_t lo, hi;       /* host-page-aligned span                      */
} wcb_region;

#define WCB_RECENT 64       /* released spans a late fault may still name  */

static struct {
    wcb_lock_t lock;
    int        inited;
    rsx_wcb_resolve_fn fn;
    void*      user;
    void     (*wake)(void);
    wcb_region r[RSX_WCB_MAX_REGIONS];
    uint32_t   n;            /* high-water mark of used slots               */
    uint32_t   pending;      /* regions not FREE                            */
    int        requested;    /* regions in WCB_REQUESTED                    */
    uint32_t   gen;
    /* The span every armed region lies in, read without the lock by
     * rsx_wcb_access: a stale value can only miss a region armed a moment
     * ago, whose access then faults and resolves the ordinary way. */
    volatile uintptr_t env_lo, env_hi;
    struct { uintptr_t lo, hi; } recent[WCB_RECENT];
    uint32_t   recent_next;
    uint64_t   timeout_ns;
    int        forced;       /* rsx_wcb_store writes through the protection */
#ifndef _WIN32
    int        mem_fd;
#endif
    rsx_wcb_counters c;
    /* Per-cookie guest wait totals for the attribution table. Only updated
     * when `attr` is on. */
    int        attr;
    struct { uint32_t cookie, n; uint64_t ns; } attr_wait[RSX_WCB_MAX_REGIONS];
} s_wcbm;

/* The owner is one thread at a time: whichever last called
 * rsx_wcb_set_owner_thread. A thread is the owner while its token matches
 * the current epoch, so a thread that once drove the engine stops resolving
 * inline the moment another takes over. */
static volatile uint32_t s_owner_epoch;
static WCB_TLS uint32_t s_owner_token;
static int wcb_owner(void)
{
    return s_owner_token != 0 && s_owner_token == WCB_LOAD(&s_owner_epoch);
}

static void wcb_lock(void)
{
    unsigned spin = 0;
    while (!wcb_cas_lock(&s_wcbm.lock)) wcb_yield(spin++ < 64 ? spin : 64);
}
static void wcb_unlock(void) { wcb_unlock_word(&s_wcbm.lock); }

static void wcb_count_max(uint64_t* p, uint64_t v)
{
    /* Counters are diagnostics: a lost maximum under a race is acceptable. */
    if (v > *p) *p = v;
}

/* ---- forced store -------------------------------------------------------- */

#ifndef _WIN32
/* Writes through /proc/self/mem ignore the page protection (the kernel uses
 * a forced user-page lookup for them), so the copy lands while every other
 * thread still faults on the pages. Probed once against a scratch page,
 * because a hardened kernel can refuse it. */
static int wcb_probe_forced(void)
{
#if defined(__linux__)
    s_wcbm.mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (s_wcbm.mem_fd < 0) return 0;
    const uintptr_t pg = wcb_page_size();
    unsigned char* p = (unsigned char*)mmap(NULL, pg, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    p[0] = 0;
    int ok = 0;
    if (mprotect(p, pg, PROT_NONE) == 0) {
        const unsigned char v = 0x5A;
        ok = pwrite(s_wcbm.mem_fd, &v, 1, (off_t)(uintptr_t)p) == 1;
        mprotect(p, pg, PROT_READ | PROT_WRITE);
        ok = ok && p[0] == 0x5A;
    }
    munmap(p, pg);
    if (!ok) { close(s_wcbm.mem_fd); s_wcbm.mem_fd = -1; }
    return ok;
#else
    s_wcbm.mem_fd = -1;
    return 0;
#endif
}
#endif

int rsx_wcb_forced_store(void) { return s_wcbm.forced; }

int rsx_wcb_store(void* host_dst, const void* src, size_t n)
{
    if (!n) return 0;
    rsx_texdirty_note_write_host(host_dst, n);   /* the /proc/self/mem path
                                                  * bypasses page protection */
#ifndef _WIN32
    if (s_wcbm.forced) {
        const unsigned char* s = (const unsigned char*)src;
        uintptr_t at = (uintptr_t)host_dst;
        while (n) {
            const ssize_t k = pwrite(s_wcbm.mem_fd, s, n, (off_t)at);
            if (k < 0) { if (errno == EINTR) continue; return -1; }
            if (k == 0) return -1;
            s += k; at += (uintptr_t)k; n -= (size_t)k;
        }
        return 0;
    }
#endif
    memcpy(host_dst, src, n);
    return 0;
}

/* ---- the region table ---------------------------------------------------- */

static int wcb_find(uint32_t cookie)
{
    for (uint32_t i = 0; i < s_wcbm.n; i++)
        if (s_wcbm.r[i].state != WCB_FREE && s_wcbm.r[i].cookie == cookie) return (int)i;
    return -1;
}

/* Is [lo, hi) covered by an armed region other than `skip`? Page granular. */
static int wcb_page_covered(uintptr_t page, int skip)
{
    for (uint32_t i = 0; i < s_wcbm.n; i++)
        if ((int)i != skip && s_wcbm.r[i].state != WCB_FREE && s_wcbm.r[i].lo <= page && page < s_wcbm.r[i].hi)
            return 1;
    return 0;
}

/* Reopen region i's pages that no other region covers, and free the slot.
 * Lock held. A page still covered by a cached texture reopens read-only so
 * the next guest store faults and charges the write to that texture. */
static void wcb_reopen_uncovered(int i)
{
    const wcb_region* r = &s_wcbm.r[i];
    const uintptr_t pg = wcb_page_size();
    uintptr_t run = 0;
    int run_prot = -1;
    for (uintptr_t p = r->lo; p < r->hi; p += pg) {
        const int keep = wcb_page_covered(p, i);
        const int prot = keep ? 0 : (td_page_wants_ro(p) ? 1 : 2);
        if (run && (!keep || prot != run_prot)) {
            wcb_set_access_prot(run, p, run_prot);
            td_open_note_locked(run, p, run_prot == 1);
            run = 0;
        }
        if (!keep && !run) { run = p; run_prot = prot; }
    }
    if (run) {
        wcb_set_access_prot(run, r->hi, run_prot);
        td_open_note_locked(run, r->hi, run_prot == 1);
    }
}

static void wcb_release_locked(int i)
{
    wcb_region* r = &s_wcbm.r[i];
    wcb_reopen_uncovered(i);
    if (r->state == WCB_REQUESTED) s_wcbm.requested--;
    s_wcbm.recent[s_wcbm.recent_next % WCB_RECENT].lo = r->lo;
    s_wcbm.recent[s_wcbm.recent_next % WCB_RECENT].hi = r->hi;
    s_wcbm.recent_next++;
    r->state = WCB_FREE;
    r->gen = ++s_wcbm.gen;
    s_wcbm.pending--;
    s_wcbm.c.released++;
    while (s_wcbm.n && s_wcbm.r[s_wcbm.n - 1].state == WCB_FREE) s_wcbm.n--;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (uint32_t k = 0; k < s_wcbm.n; k++)
        if (s_wcbm.r[k].state != WCB_FREE) {
            if (s_wcbm.r[k].lo < lo) lo = s_wcbm.r[k].lo;
            if (s_wcbm.r[k].hi > hi) hi = s_wcbm.r[k].hi;
        }
    s_wcbm.env_lo = lo;
    s_wcbm.env_hi = hi;
}

int rsx_wcb_protect(uint32_t cookie, void* host, size_t len)
{
    if (!s_wcbm.inited || !host || !len) return -1;
    const uintptr_t pg = wcb_page_size();
    const uintptr_t lo = (uintptr_t)host & ~(pg - 1u);
    const uintptr_t hi = ((uintptr_t)host + len + pg - 1u) & ~(pg - 1u);
    wcb_lock();
    if (wcb_find(cookie) >= 0) { wcb_unlock(); return 0; }
    int slot = -1;
    for (uint32_t i = 0; i < RSX_WCB_MAX_REGIONS; i++)
        if (s_wcbm.r[i].state == WCB_FREE) { slot = (int)i; break; }
    if (slot < 0 || wcb_set_access(lo, hi, 0) != 0) {
        s_wcbm.c.protect_failures++;
        wcb_unlock();
        return -1;
    }
    td_forget_prot_locked(lo, hi);      /* PROT_NONE now owns these pages */
    s_wcbm.r[slot].cookie = cookie;
    s_wcbm.r[slot].lo = lo;
    s_wcbm.r[slot].hi = hi;
    s_wcbm.r[slot].state = WCB_PENDING;
    if ((uint32_t)slot >= s_wcbm.n) s_wcbm.n = (uint32_t)slot + 1u;
    if (s_wcbm.pending == 0 || lo < s_wcbm.env_lo) s_wcbm.env_lo = lo;
    if (s_wcbm.pending == 0 || hi > s_wcbm.env_hi) s_wcbm.env_hi = hi;
    s_wcbm.pending++;
    s_wcbm.c.protected_regions++;
    wcb_unlock();
    return 0;
}

int rsx_wcb_is_pending(uint32_t cookie)
{
    wcb_lock();
    const int r = wcb_find(cookie) >= 0;
    wcb_unlock();
    return r;
}

uint32_t rsx_wcb_pending_count(void) { return (uint32_t)WCB_LOAD(&s_wcbm.pending); }

int rsx_wcb_requests_pending(void) { return WCB_LOAD(&s_wcbm.requested) != 0; }

void rsx_wcb_release(uint32_t cookie)
{
    wcb_lock();
    const int i = wcb_find(cookie);
    if (i >= 0) wcb_release_locked(i);
    wcb_unlock();
}

/* Run the resolve function for slot i, which the caller marked RESOLVING
 * (lock not held), then release it. Owner thread only. */
static void wcb_run_and_release(int i, int reason)
{
    const uint32_t cookie = s_wcbm.r[i].cookie;
    s_wcbm.c.resolves++;
    if (reason == RSX_WCB_REASON_REQUEST) s_wcbm.c.requests++;
    else s_wcbm.c.inline_resolves++;
    if (s_wcbm.fn) s_wcbm.fn(s_wcbm.user, cookie, reason);
    wcb_lock();
    if (s_wcbm.r[i].state == WCB_RESOLVING && s_wcbm.r[i].cookie == cookie) wcb_release_locked(i);
    wcb_unlock();
}

static int wcb_regions_share_page(const wcb_region* a, const wcb_region* b)
{
    return a->lo < b->hi && b->lo < a->hi;
}

static void wcb_resolve_slot(int i, int reason)
{
    if (s_wcbm.forced) { wcb_run_and_release(i, reason); return; }
    /* No way to write through the protection: the pages must be open before
     * the copy lands, and a reader on another thread can see it half written
     * in that window. A page this region shares with another armed one
     * cannot be opened without exposing that one's stale bytes, and cannot
     * stay closed without the copy faulting on this thread -- so the regions
     * sharing pages, transitively, are opened and copied together. */
    int group[RSX_WCB_MAX_REGIONS];
    int n = 0;
    wcb_lock();
    if (s_wcbm.r[i].state == WCB_RESOLVING) group[n++] = i;
    for (int k = 0; k < n; k++)
        for (uint32_t j = 0; j < s_wcbm.n; j++) {
            wcb_region* r = &s_wcbm.r[j];
            if ((r->state == WCB_PENDING || r->state == WCB_REQUESTED) &&
                wcb_regions_share_page(r, &s_wcbm.r[group[k]])) {
                if (r->state == WCB_REQUESTED) s_wcbm.requested--;
                r->state = WCB_RESOLVING;
                group[n++] = (int)j;
            }
        }
    for (int k = 0; k < n; k++) wcb_set_access(s_wcbm.r[group[k]].lo, s_wcbm.r[group[k]].hi, 1);
    wcb_unlock();
    for (int k = 0; k < n; k++)
        wcb_run_and_release(group[k], k == 0 ? reason : RSX_WCB_REASON_FLUSH);
}

/* Owner thread: resolve every armed region whose pages meet [lo, hi). */
static void wcb_resolve_span_owner(uintptr_t lo, uintptr_t hi, int reason)
{
    for (;;) {
        int pick = -1;
        wcb_lock();
        for (uint32_t i = 0; i < s_wcbm.n; i++) {
            wcb_region* r = &s_wcbm.r[i];
            if ((r->state == WCB_PENDING || r->state == WCB_REQUESTED) && r->lo < hi && lo < r->hi) {
                if (r->state == WCB_REQUESTED) s_wcbm.requested--;
                r->state = WCB_RESOLVING;
                pick = (int)i;
                break;
            }
        }
        wcb_unlock();
        if (pick < 0) return;
        wcb_resolve_slot(pick, reason);
    }
}

/* Another thread: ask the owner for every armed region meeting [lo, hi) and
 * wait until each has been released. Returns how many it waited for. */
static int wcb_request_span(uintptr_t lo, uintptr_t hi)
{
    int slots[RSX_WCB_MAX_REGIONS];
    uint32_t gens[RSX_WCB_MAX_REGIONS];
    uint32_t cookies[RSX_WCB_MAX_REGIONS];
    int n = 0;
    wcb_lock();
    for (uint32_t i = 0; i < s_wcbm.n; i++) {
        wcb_region* r = &s_wcbm.r[i];
        if (r->state != WCB_FREE && r->lo < hi && lo < r->hi) {
            if (r->state == WCB_PENDING) { r->state = WCB_REQUESTED; s_wcbm.requested++; }
            slots[n] = (int)i;
            gens[n] = r->gen;
            cookies[n] = r->cookie;
            n++;
        }
    }
    wcb_unlock();
    if (!n) return 0;
    if (s_wcbm.wake) s_wcbm.wake();
    const uint64_t t0 = wcb_now_ns();
    unsigned spin = 0;
    for (;;) {
        int left = 0;
        wcb_lock();
        for (int k = 0; k < n; k++)
            if (s_wcbm.r[slots[k]].state != WCB_FREE && s_wcbm.r[slots[k]].gen == gens[k]) left++;
        if (left && wcb_now_ns() - t0 > s_wcbm.timeout_ns) {
            /* Nobody serviced it. Open the pages with what they hold rather
             * than hang the guest; the counter and the log say so. */
            for (int k = 0; k < n; k++)
                if (s_wcbm.r[slots[k]].state != WCB_FREE && s_wcbm.r[slots[k]].gen == gens[k] &&
                    s_wcbm.r[slots[k]].state != WCB_RESOLVING)
                    wcb_release_locked(slots[k]);
            s_wcbm.c.timeouts++;
            left = 0;
            for (int k = 0; k < n; k++)
                if (s_wcbm.r[slots[k]].state != WCB_FREE && s_wcbm.r[slots[k]].gen == gens[k]) left++;
            if (!left) {
                wcb_unlock();
                static int logs;
                if (logs++ < 8)
                    fprintf(stderr, "[rsx wcb] owner thread did not service a read within %llu ms;"
                                    " guest reads stale render-target bytes\n",
                            (unsigned long long)(s_wcbm.timeout_ns / 1000000ull));
                break;
            }
            /* Mid-copy on the owner: keep waiting for it to finish. */
        }
        wcb_unlock();
        if (!left) break;
        if (s_wcbm.wake && (spin & 255u) == 255u) s_wcbm.wake();
        wcb_yield(spin++);
    }
    const uint64_t dt = wcb_now_ns() - t0;
    WCB_ADD64(&s_wcbm.c.waits, 1);
    WCB_ADD64(&s_wcbm.c.wait_ns, dt);
    wcb_count_max(&s_wcbm.c.wait_max_ns, dt);
    if (s_wcbm.attr && n) {
        wcb_lock();
        for (int k = 0; k < n; k++) {
            int e = -1;
            for (uint32_t j = 0; j < RSX_WCB_MAX_REGIONS; j++)
                if (s_wcbm.attr_wait[j].n && s_wcbm.attr_wait[j].cookie == cookies[k]) { e = (int)j; break; }
            if (e < 0)
                for (uint32_t j = 0; j < RSX_WCB_MAX_REGIONS; j++)
                    if (!s_wcbm.attr_wait[j].n) { e = (int)j; s_wcbm.attr_wait[j].cookie = cookies[k]; break; }
            if (e >= 0) {
                s_wcbm.attr_wait[e].ns += dt / (uint64_t)n;
                s_wcbm.attr_wait[e].n += 1;
            }
        }
        wcb_unlock();
    }
    return n;
}

void rsx_wcb_access(const void* host, size_t len)
{
    if (!WCB_LOAD(&s_wcbm.pending) || !host || !len) return;
    const uintptr_t pg = wcb_page_size();
    const uintptr_t lo = (uintptr_t)host & ~(pg - 1u);
    const uintptr_t hi = ((uintptr_t)host + len + pg - 1u) & ~(pg - 1u);
    if (hi <= s_wcbm.env_lo || lo >= s_wcbm.env_hi) return;
    if (wcb_owner()) wcb_resolve_span_owner(lo, hi, RSX_WCB_REASON_EXPLICIT);
    else         wcb_request_span(lo, hi);
}

int rsx_wcb_resolve_cookie(uint32_t cookie, int reason)
{
    if (!WCB_LOAD(&s_wcbm.pending)) return 0;
    wcb_lock();
    const int i = wcb_find(cookie);
    if (i >= 0 && s_wcbm.r[i].state != WCB_RESOLVING) {
        if (s_wcbm.r[i].state == WCB_REQUESTED) s_wcbm.requested--;
        s_wcbm.r[i].state = WCB_RESOLVING;
    } else {
        wcb_unlock();
        return 0;
    }
    wcb_unlock();
    wcb_resolve_slot(i, reason);
    return 1;
}

void rsx_wcb_resolve_all(int reason)
{
    if (!WCB_LOAD(&s_wcbm.pending)) return;
    wcb_resolve_span_owner(0, ~(uintptr_t)0, reason);
}

int rsx_wcb_service(void)
{
    int done = 0;
    if (!WCB_LOAD(&s_wcbm.requested)) return 0;
    for (;;) {
        int pick = -1;
        wcb_lock();
        for (uint32_t i = 0; i < s_wcbm.n; i++)
            if (s_wcbm.r[i].state == WCB_REQUESTED) {
                s_wcbm.r[i].state = WCB_RESOLVING;
                s_wcbm.requested--;
                pick = (int)i;
                break;
            }
        wcb_unlock();
        if (pick < 0) return done;
        wcb_resolve_slot(pick, RSX_WCB_REASON_REQUEST);
        done++;
    }
}

/* Take every request at once (PS3RECOMP_RSX_WCB_BATCH). The regions stay
 * RESOLVING, and therefore keep the waiting threads blocked, until the caller
 * has copied them and calls rsx_wcb_release() for each. */
uint32_t rsx_wcb_take_requests(uint32_t* cookies, uint32_t max)
{
    if (!cookies || !max || !WCB_LOAD(&s_wcbm.requested)) return 0;
    uint32_t n = 0;
    wcb_lock();
    for (uint32_t i = 0; i < s_wcbm.n && n < max; i++) {
        if (s_wcbm.r[i].state != WCB_REQUESTED) continue;
        s_wcbm.r[i].state = WCB_RESOLVING;
        s_wcbm.requested--;
        cookies[n++] = s_wcbm.r[i].cookie;
    }
    wcb_unlock();
    return n;
}

/* ---- faults --------------------------------------------------------------- */

int rsx_wcb_on_fault(uintptr_t addr)
{
    if (!s_wcbm.inited) return 0;
    const uintptr_t pg = wcb_page_size();
    const uintptr_t page = addr & ~(pg - 1u);
    wcb_lock();
    int ours = 0, actionable = 0;
    for (uint32_t i = 0; i < s_wcbm.n; i++)
        if (s_wcbm.r[i].state != WCB_FREE && s_wcbm.r[i].lo <= page && page < s_wcbm.r[i].hi) {
            ours = 1;
            if (s_wcbm.r[i].state != WCB_RESOLVING) actionable = 1;
        }
    if (ours && !actionable && wcb_owner()) {
        /* The owner touched a page whose region it is itself copying: there
         * is nothing to wait for and waiting would never end. Open the page
         * (the copy completes into it) rather than loop or die. */
        const int ro = td_page_wants_ro(page);
        wcb_set_access_prot(page, page + pg, ro ? 1 : 2);
        td_open_note_locked(page, page + pg, ro);
        wcb_unlock();
        WCB_ADD64(&s_wcbm.c.late_faults, 1);
        return 1;
    }
    if (!ours) {
        /* The region may have been released between the fault and this
         * handler taking the lock: the page is open again and the access
         * only has to be retried. A bounded number of retries per thread,
         * so a fault that is genuinely someone else's still reaches them. */
        static WCB_TLS uintptr_t last_addr;
        static WCB_TLS unsigned  last_count;
        int recent = 0;
        const uint32_t nrec = s_wcbm.recent_next < WCB_RECENT ? s_wcbm.recent_next : WCB_RECENT;
        for (uint32_t i = 0; i < nrec && !recent; i++)
            if (s_wcbm.recent[i].lo <= page && page < s_wcbm.recent[i].hi) recent = 1;
        wcb_unlock();
        if (!recent) return 0;
        if (addr == last_addr) { if (++last_count > 1000u) return 0; }
        else { last_addr = addr; last_count = 0; }
        WCB_ADD64(&s_wcbm.c.late_faults, 1);
        return 1;
    }
    wcb_unlock();
    WCB_ADD64(&s_wcbm.c.faults, 1);
    if (wcb_owner()) wcb_resolve_span_owner(page, page + pg, RSX_WCB_REASON_FAULT);
    else         wcb_request_span(page, page + pg);
    return 1;
}

#ifdef _WIN32
static LONG WINAPI wcb_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = (uintptr_t)er->ExceptionInformation[1];
    return (rsx_texdirty_on_fault(at) || rsx_wcb_on_fault(at) || td_late_fault(at))
           ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static void wcb_install_handler(void)
{
    /* First in line: a demand-commit handler would otherwise "commit" our
     * PAGE_NOACCESS page read-write and let the read through stale. */
    static int once;
    if (once) return;
    once = 1;
    AddVectoredExceptionHandler(1, wcb_veh);
}
#else
/* One saved action per signal, chained in the form it was registered, as
 * runtime/ppu/ppu_loader.cpp and the win32_compat VEH dispatcher do: the
 * three compose in any installation order. Darwin reports a PROT_NONE page
 * as SIGBUS, Linux as SIGSEGV; both are taken. */
static struct sigaction s_prev_segv, s_prev_bus;

static void wcb_chain(int sig, siginfo_t* si, void* uctx)
{
    const struct sigaction* prev = sig == SIGBUS ? &s_prev_bus : &s_prev_segv;
    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) { prev->sa_sigaction(sig, si, uctx); return; }
    if (prev->sa_handler && prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);   /* re-run the access and die as before */
}

static void wcb_signal(int sig, siginfo_t* si, void* uctx)
{
    const int saved = errno;
    if (si) {
        const uintptr_t at = (uintptr_t)si->si_addr;
        if (rsx_texdirty_on_fault(at) || rsx_wcb_on_fault(at) || td_late_fault(at)) { errno = saved; return; }
    }
    errno = saved;
    wcb_chain(sig, si, uctx);
}

static int s_handler_done;
static void wcb_install_handler(void)
{
    if (s_handler_done) return;
    s_handler_done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = wcb_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_prev_segv);
    sigaction(SIGBUS, &sa, &s_prev_bus);
}
#endif

static void td_install_faults(void)
{
    wcb_install_handler();
}

/* ---- lifecycle ----------------------------------------------------------- */

/* The runtime's hook for code that hands guest memory to the host kernel or
 * checks page state before touching it (cellFsWrite, the SPU DMA guard): it
 * calls this first so the range is coherent and accessible. */
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
/* The runtime's definition (runtime/spu/spu_vm_pagemap.c) wins wherever it is
 * linked; this one serves an engine-only test binary that does not link it. */
__attribute__((weak)) void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len);
#else
extern void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len);
#endif

int rsx_wcb_init(rsx_wcb_resolve_fn fn, void* user, void (*wake)(void))
{
    s_wcbm.fn = fn;
    s_wcbm.user = user;
    s_wcbm.wake = wake;
    if (s_wcbm.inited) return 0;
    const char* t = getenv("PS3RECOMP_RSX_WCB_TIMEOUT_MS");
    const long ms = t ? atol(t) : 0;
    s_wcbm.timeout_ns = (uint64_t)(ms > 0 ? ms : 5000) * 1000000ull;
    /* PS3RECOMP_RSX_WCB_FORCED=0: reopen-then-write even where writing
     * through the protection works (to test that path, or to rule it out). */
    const char* f = getenv("PS3RECOMP_RSX_WCB_FORCED");
#ifdef _WIN32
    (void)f;
    s_wcbm.forced = 0;
#else
    s_wcbm.forced = (f && *f == '0') ? 0 : wcb_probe_forced();
#endif
    wcb_install_handler();
    g_ps3_guest_access_hook = rsx_wcb_access;
    s_wcbm.inited = 1;
    fprintf(stderr, "[rsx wcb] on-demand render-target readback armed (%s, timeout %llu ms)\n",
            s_wcbm.forced ? "writes through the protection" : "reopen-then-write: see docs/wcb-async.md",
            (unsigned long long)(s_wcbm.timeout_ns / 1000000ull));
    return 0;
}

void rsx_wcb_shutdown(void)
{
    if (!s_wcbm.inited) return;
    wcb_lock();
    for (uint32_t i = 0; i < s_wcbm.n; i++)
        if (s_wcbm.r[i].state != WCB_FREE) wcb_release_locked((int)i);
    s_wcbm.requested = 0;
    wcb_unlock();
}

void rsx_wcb_set_owner_thread(void)
{
    if (wcb_owner()) return;
    wcb_lock();
    s_owner_token = ++s_owner_epoch;
    wcb_unlock();
}
int  rsx_wcb_is_owner_thread(void) { return wcb_owner(); }

void rsx_wcb_counters_get(rsx_wcb_counters* out)
{
    if (!out) return;
    wcb_lock();
    *out = s_wcbm.c;
    wcb_unlock();
}

void rsx_wcb_attr_set(int on)
{
    wcb_lock();
    s_wcbm.attr = on ? 1 : 0;
    memset(s_wcbm.attr_wait, 0, sizeof s_wcbm.attr_wait);
    wcb_unlock();
}

int rsx_wcb_attr_take_wait(uint32_t cookie, uint64_t* ns, uint32_t* n)
{
    int found = 0;
    if (ns) *ns = 0;
    if (n) *n = 0;
    wcb_lock();
    for (uint32_t j = 0; j < RSX_WCB_MAX_REGIONS; j++)
        if (s_wcbm.attr_wait[j].n && s_wcbm.attr_wait[j].cookie == cookie) {
            if (ns) *ns = s_wcbm.attr_wait[j].ns;
            if (n) *n = s_wcbm.attr_wait[j].n;
            s_wcbm.attr_wait[j].ns = 0;
            s_wcbm.attr_wait[j].n = 0;
            found = 1;
            break;
        }
    wcb_unlock();
    return found;
}

/* ---- texture page dirty tracking (PS3RECOMP_TEX_DIRTY) -------------------
 *
 * A 4K-guest-page write sequence. The pages a cached texture was decoded from
 * are write-protected; every write path the engine can intercept bumps the
 * sequence for its range and opens the pages, and the PPU's own inlined
 * stores fault into td_on_fault (the same handler as the WCB). A texture
 * remembers the sequence sampled just before it read the bytes; it is stale
 * when any page in its span has a newer sequence. No content hashing.
 *
 * The state lives here because it shares both the page protection primitives
 * and wcb_lock with the region table: a page can be a render target waiting
 * for a copy and a texture's source at the same time, and the two must agree
 * on which protection it has.
 */
#define TD_PAGE_SIZE_4K 4096u
#define TD_NPAGES   (1u << 20)                  /* 4 GB / 4K */
#define TD_TEX_PROT 0x01u                       /* page is PROT_READ for us */
#define TD_WCB_PROT 0x02u                       /* page is PROT_NONE for WCB */
#define TD_WAS_PROT 0x04u                       /* we have protected it before */
#define TD_RANGE_MAX 16u                        /* tracked source ranges     */

typedef struct {
    volatile int used;
    uint32_t ea, span;
    uint64_t epoch;
    uint64_t gen;                               /* seq of the last overlap   */
} td_range;

static struct {
    volatile int on;
    unsigned long long seq;                     /* monotonic write sequence */
    uint64_t* page_seq;                         /* TD_NPAGES, newest write */
    uint16_t* page_cov;                         /* texture entries covering   */
    uint8_t*  page_flags;
    uint64_t  writes, protect_calls, fault_opens, serial;
    td_range  ranges[TD_RANGE_MAX];
    uint64_t  range_epoch;                      /* bumped when ranges reset  */
    uint32_t  range_n;                          /* live entries              */
    int       failed;
} s_td;

static int td_supported(void)
{
    if (wcb_page_size() != TD_PAGE_SIZE_4K) return 0;
    return ((uintptr_t)vm_base & (TD_PAGE_SIZE_4K - 1u)) == 0;
}

static uint32_t td_index(uintptr_t host_off)
{
    const uint32_t i = (uint32_t)(host_off >> 12);
    return i < TD_NPAGES ? i : TD_NPAGES;
}

/* Open a run [lo,hi) of guest pages at `prot` (1 = read-only, 2 = read/write)
 * and update the per-page bookkeeping. Lock held. */
static void td_open_run_locked(uintptr_t lo, uintptr_t hi, int prot)
{
    wcb_set_access_prot(lo, hi, prot);
    const uint32_t a = td_index(lo - (uintptr_t)vm_base);
    const uint32_t b = td_index(hi - (uintptr_t)vm_base);
    for (uint32_t i = a; i < b && i < TD_NPAGES; i++) {
        s_td.page_flags[i] &= (uint8_t)~TD_WCB_PROT;
        if (prot == 1) s_td.page_flags[i] |= TD_TEX_PROT | TD_WAS_PROT;
        else           s_td.page_flags[i] &= (uint8_t)~TD_TEX_PROT;
    }
}

static void td_forget_prot_locked(uintptr_t lo, uintptr_t hi)
{
    if (!s_td.on || !s_td.page_flags) return;
    const uint32_t a = td_index(lo - (uintptr_t)vm_base);
    const uint32_t b = td_index(hi - (uintptr_t)vm_base);
    for (uint32_t i = a; i < b && i < TD_NPAGES; i++)
        s_td.page_flags[i] = (uint8_t)((s_td.page_flags[i] & (uint8_t)~TD_TEX_PROT) | TD_WCB_PROT);
    __atomic_add_fetch(&s_td.serial, 1, __ATOMIC_RELAXED);
}

static int td_page_wants_ro(uintptr_t page)
{
    if (!s_td.on || !s_td.page_cov) return 0;
    const uint32_t i = td_index(page - (uintptr_t)vm_base);
    return i < TD_NPAGES && s_td.page_cov[i] != 0;
}

static void td_open_note_locked(uintptr_t lo, uintptr_t hi, int ro)
{
    if (!s_td.on || !s_td.page_flags) return;
    const uint32_t a = td_index(lo - (uintptr_t)vm_base);
    const uint32_t b = td_index(hi - (uintptr_t)vm_base);
    for (uint32_t i = a; i < b && i < TD_NPAGES; i++) {
        s_td.page_flags[i] &= (uint8_t)~TD_WCB_PROT;
        if (ro) s_td.page_flags[i] |= TD_TEX_PROT | TD_WAS_PROT;
        else    s_td.page_flags[i] &= (uint8_t)~TD_TEX_PROT;
    }
}

/* Drop every tracked source range. Registration is per enable epoch, so a
 * range from before a disable/enable cycle is never mistaken for the new
 * page state. Lock held. */
static void td_ranges_reset_locked(void)
{
    for (uint32_t i = 0; i < TD_RANGE_MAX; i++)
        __atomic_store_n(&s_td.ranges[i].used, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_td.range_epoch, 1, __ATOMIC_RELEASE);
    s_td.range_n = 0;
    memset(s_td.ranges, 0, sizeof s_td.ranges);
}

/* Open the protected runs of `ea..ea+len` read/write without charging a new
 * write generation. Used by the ordered host-write path (PS3RECOMP_BINK_GREEN_FIX):
 * the caller opens the pages, copies the bytes, and only then calls
 * td_note_write_locked, so a concurrent texture bind can never sample the new
 * generation while the old bytes are still in place. Lock held. */
static void td_open_write_locked(uint32_t ea, uint32_t len)
{
    if (!s_td.on || !s_td.page_flags) return;
    if (!len) len = 1;
    uint64_t end = (uint64_t)ea + len;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    uintptr_t run = 0;
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++) {
        const int prot = (s_td.page_flags[i] & TD_TEX_PROT) != 0;
        if (prot && !run) run = (uintptr_t)vm_base + ((uintptr_t)i << 12);
        if (!prot && run) {
            td_open_run_locked(run, (uintptr_t)vm_base + ((uintptr_t)i << 12), 2);
            __atomic_add_fetch(&s_td.serial, 1, __ATOMIC_RELAXED);
            run = 0;
        }
    }
    if (run) {
        td_open_run_locked(run, (uintptr_t)vm_base + (((uintptr_t)p1 + 1) << 12), 2);
        __atomic_add_fetch(&s_td.serial, 1, __ATOMIC_RELAXED);
    }
}

/* Charge `ea..ea+len` to every page, opening protected runs. Lock held. */
static void td_note_write_locked(uint32_t ea, uint32_t len)
{
    if (!s_td.on || !s_td.page_seq) return;
    if (!len) len = 1;
    const uint64_t seq = __atomic_add_fetch(&s_td.seq, 1, __ATOMIC_RELAXED);
    uint64_t end = (uint64_t)ea + len;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++)
        s_td.page_seq[i] = seq;
    s_td.writes++;

    /* Source-range tracking (SPU_CODE_DMA_MEMO): charge the write to every
     * registered range it overlaps. Skipped entirely (one load) until the
     * memo actually registers a range, so the baseline write path is
     * unchanged when the switch is off. */
    if (__builtin_expect(s_td.range_n != 0, 0))
        for (uint32_t i = 0; i < TD_RANGE_MAX; i++) {
            td_range* r = &s_td.ranges[i];
            if (!__atomic_load_n(&r->used, __ATOMIC_ACQUIRE)) continue;
            const uint64_t rend = (uint64_t)r->ea + r->span;
            if ((uint64_t)ea < rend && end > (uint64_t)r->ea)
                __atomic_store_n(&r->gen, seq, __ATOMIC_RELEASE);
        }

    /* Open runs of pages we had write-protected. */
    td_open_write_locked(ea, len);
}

/* Protect the covered pages of [ea,ea+span) that are not already protected.
 * Lock held. */
static void td_protect_locked(uint32_t ea, uint32_t span)
{
    if (!s_td.on || !s_td.page_seq || !span) return;
    uint64_t end = (uint64_t)ea + span;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    uintptr_t run = 0;
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++) {
        const int want = s_td.page_cov[i] != 0 &&
                         !(s_td.page_flags[i] & (TD_TEX_PROT | TD_WCB_PROT));
        if (want && !run) run = (uintptr_t)vm_base + ((uintptr_t)i << 12);
        if (!want && run) {
            td_open_run_locked(run, (uintptr_t)vm_base + ((uintptr_t)i << 12), 1);
            s_td.protect_calls++;
            run = 0;
        }
    }
    if (run) {
        td_open_run_locked(run, (uintptr_t)vm_base + (((uintptr_t)p1 + 1) << 12), 1);
        s_td.protect_calls++;
    }
}

int rsx_texdirty_enabled(void) { return s_td.on; }

int rsx_texdirty_set_enabled(int on)
{
    if (!on) {
        wcb_lock();
        if (s_td.on) {
            /* Open everything still read-only. */
            uintptr_t run = 0;
            for (uint32_t i = 0; i < TD_NPAGES; i++) {
                const int prot = s_td.page_flags && (s_td.page_flags[i] & TD_TEX_PROT);
                if (prot && !run) run = (uintptr_t)vm_base + ((uintptr_t)i << 12);
                if (!prot && run) {
                    td_open_run_locked(run, (uintptr_t)vm_base + ((uintptr_t)i << 12), 2);
                    run = 0;
                }
            }
            if (run) td_open_run_locked(run, (uintptr_t)vm_base + ((uintptr_t)TD_NPAGES << 12), 2);
            if (s_td.page_cov) memset(s_td.page_cov, 0, TD_NPAGES * sizeof(uint16_t));
            if (s_td.page_flags) memset(s_td.page_flags, 0, TD_NPAGES);
            if (s_td.page_seq) memset(s_td.page_seq, 0, TD_NPAGES * sizeof(uint64_t));
            td_ranges_reset_locked();
        }
        s_td.on = 0;
        g_ps3_guest_write_hook = NULL;
        wcb_unlock();
        return 0;
    }
    if (s_td.on) return 0;
    if (!td_supported()) return -1;
    wcb_lock();
    if (!s_td.page_seq) {
        s_td.page_seq = (uint64_t*)calloc(TD_NPAGES, sizeof(uint64_t));
        s_td.page_cov = (uint16_t*)calloc(TD_NPAGES, sizeof(uint16_t));
        s_td.page_flags = (uint8_t*)calloc(TD_NPAGES, 1);
        if (!s_td.page_seq || !s_td.page_cov || !s_td.page_flags) {
            free(s_td.page_seq); free(s_td.page_cov); free(s_td.page_flags);
            s_td.page_seq = NULL; s_td.page_cov = NULL; s_td.page_flags = NULL;
            wcb_unlock();
            return -1;
        }
    } else {
        memset(s_td.page_seq, 0, TD_NPAGES * sizeof(uint64_t));
        memset(s_td.page_cov, 0, TD_NPAGES * sizeof(uint16_t));
        memset(s_td.page_flags, 0, TD_NPAGES);
    }
    __atomic_store_n(&s_td.seq, 1, __ATOMIC_RELAXED);
    s_td.writes = s_td.protect_calls = s_td.fault_opens = 0;
    td_ranges_reset_locked();
    s_td.on = 1;
    wcb_unlock();
    td_install_faults();
    return 0;
}

void rsx_texdirty_shutdown(void)
{
    rsx_texdirty_set_enabled(0);
    wcb_lock();
    free(s_td.page_seq); free(s_td.page_cov); free(s_td.page_flags);
    s_td.page_seq = NULL; s_td.page_cov = NULL; s_td.page_flags = NULL;
    wcb_unlock();
}

uint64_t rsx_texdirty_seq(void)
{
    return __atomic_load_n(&s_td.seq, __ATOMIC_RELAXED);
}

uint64_t rsx_texdirty_serial(void)
{
    return __atomic_load_n(&s_td.serial, __ATOMIC_RELAXED);
}

void rsx_texdirty_cover(uint32_t ea, uint32_t span)
{
    if (!s_td.on || !span) return;
    uint64_t end = (uint64_t)ea + span;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    wcb_lock();
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++)
        if (s_td.page_cov[i] != 0xFFFFu) s_td.page_cov[i]++;
    td_protect_locked(ea, span);
    wcb_unlock();
}

void rsx_texdirty_uncover(uint32_t ea, uint32_t span)
{
    if (!s_td.on || !span) return;
    uint64_t end = (uint64_t)ea + span;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    wcb_lock();
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++)
        if (s_td.page_cov[i]) s_td.page_cov[i]--;
    td_protect_locked(ea, span);       /* leaves uncovered pages alone */
    /* Open runs that no texture covers any more. */
    uintptr_t run = 0;
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++) {
        const int open = s_td.page_cov[i] == 0 && (s_td.page_flags[i] & TD_TEX_PROT);
        if (open && !run) run = (uintptr_t)vm_base + ((uintptr_t)i << 12);
        if (!open && run) {
            td_open_run_locked(run, (uintptr_t)vm_base + ((uintptr_t)i << 12), 2);
            run = 0;
        }
    }
    if (run) td_open_run_locked(run, (uintptr_t)vm_base + (((uintptr_t)p1 + 1) << 12), 2);
    wcb_unlock();
}

uint64_t rsx_texdirty_max_seq(uint32_t ea, uint32_t span)
{
    if (!s_td.on || !span) return 0;
    uint64_t end = (uint64_t)ea + span;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)ea >= end) return UINT64_MAX;
    const uint32_t p0 = ea >> 12, p1 = (uint32_t)((end - 1) >> 12);
    uint64_t m = 0;
    wcb_lock();
    for (uint32_t i = p0; i <= p1 && i < TD_NPAGES; i++)
        if (s_td.page_seq[i] > m) m = s_td.page_seq[i];
    wcb_unlock();
    return m;
}

void rsx_texdirty_reprotect(uint32_t ea, uint32_t span)
{
    if (!s_td.on) return;
    wcb_lock();
    td_protect_locked(ea, span);
    wcb_unlock();
}

/* Find a live tracked range for this epoch. Lock held. */
static int td_range_find_locked(uint32_t ea, uint32_t span)
{
    const uint64_t ep = __atomic_load_n(&s_td.range_epoch, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < TD_RANGE_MAX; i++) {
        td_range* r = &s_td.ranges[i];
        if (!__atomic_load_n(&r->used, __ATOMIC_ACQUIRE)) continue;
        if (__atomic_load_n(&r->epoch, __ATOMIC_RELAXED) != ep) continue;
        if (r->ea == ea && r->span == span) return (int)i;
    }
    return -1;
}

int rsx_texdirty_track_range(uint32_t ea, uint32_t span, uint64_t* gen)
{
    if (!s_td.on || !span) return -1;
    if ((uint64_t)ea + span > ((uint64_t)TD_NPAGES << 12)) return -1;

    wcb_lock();
    int idx = td_range_find_locked(ea, span);
    int is_new = 0;
    if (idx < 0) {
        for (uint32_t i = 0; i < TD_RANGE_MAX; i++)
            if (!__atomic_load_n(&s_td.ranges[i].used, __ATOMIC_ACQUIRE)) {
                idx = (int)i;
                break;
            }
        if (idx < 0) { wcb_unlock(); return -1; }
        td_range* r = &s_td.ranges[idx];
        r->ea = ea;
        r->span = span;
        __atomic_store_n(&r->epoch,
                         __atomic_load_n(&s_td.range_epoch, __ATOMIC_RELAXED),
                         __ATOMIC_RELAXED);
        __atomic_store_n(&r->gen, __atomic_load_n(&s_td.seq, __ATOMIC_RELAXED),
                         __ATOMIC_RELAXED);
        __atomic_store_n(&r->used, 1, __ATOMIC_RELEASE);
        s_td.range_n++;
        is_new = 1;
    }
    wcb_unlock();

    /* Protect the pages (a new range also establishes the coverage count).
     * A write racing this call either happened before the caller's compare,
     * which sees it, or bumps the generation the caller samples after it. */
    if (is_new) rsx_texdirty_cover(ea, span);
    else        rsx_texdirty_reprotect(ea, span);

    wcb_lock();
    idx = td_range_find_locked(ea, span);
    if (idx < 0) { wcb_unlock(); return -1; }
    td_range* r = &s_td.ranges[idx];
    const uint64_t g = __atomic_load_n(&s_td.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&r->gen, g, __ATOMIC_RELEASE);
    if (gen) *gen = g;
    wcb_unlock();
    return 0;
}

int rsx_texdirty_range_gen(uint32_t ea, uint32_t span, uint64_t* gen)
{
    if (!s_td.on || !span) return 0;
    const uint64_t ep = __atomic_load_n(&s_td.range_epoch, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < TD_RANGE_MAX; i++) {
        td_range* r = &s_td.ranges[i];
        if (!__atomic_load_n(&r->used, __ATOMIC_ACQUIRE)) continue;
        if (__atomic_load_n(&r->epoch, __ATOMIC_RELAXED) != ep) continue;
        if (r->ea != ea || r->span != span) continue;
        if (gen) *gen = __atomic_load_n(&r->gen, __ATOMIC_ACQUIRE);
        return 1;
    }
    return 0;
}

int rsx_texdirty_range_unchanged(uint32_t ea, uint32_t span, uint64_t gen)
{
    uint64_t cur;
    return rsx_texdirty_range_gen(ea, span, &cur) && cur == gen;
}

void rsx_texdirty_note_write(uint32_t ea, uint32_t len)
{
    if (!s_td.on) return;
    wcb_lock();
    td_note_write_locked(ea, len);
    wcb_unlock();
}

void rsx_texdirty_note_write_host(const void* host, size_t len)
{
    if (!s_td.on || !host || !len) return;
    const uintptr_t off = (uintptr_t)host - (uintptr_t)vm_base;
    if (off >= ((uintptr_t)TD_NPAGES << 12)) return;
    rsx_texdirty_note_write((uint32_t)off, (uint32_t)len);
}

/* PS3RECOMP_BINK_GREEN_FIX (env, default 0): order a host write as
 * open-pages -> copy -> note. The default path notes the write before the
 * bytes land, so a texture bind racing the copy can sample the new generation
 * while the old (often all-zero) bytes are still there and then store that
 * generation as up to date, pinning the stale image until some later,
 * unrelated write. */
int rsx_texdirty_write_ordered(void)
{
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("PS3RECOMP_BINK_GREEN_FIX");
        s = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return s;
}

/* Open the tracked pages of the coming host write without charging it. */
void rsx_texdirty_prepare_write_host(const void* host, size_t len)
{
    if (!s_td.on || !host || !len) return;
    const uintptr_t off = (uintptr_t)host - (uintptr_t)vm_base;
    if (off >= ((uintptr_t)TD_NPAGES << 12)) return;
    uint64_t end = (uint64_t)(uint32_t)off + len;
    if (end > ((uint64_t)TD_NPAGES << 12)) end = (uint64_t)TD_NPAGES << 12;
    if ((uint64_t)(uint32_t)off >= end) return;
    wcb_lock();
    td_open_write_locked((uint32_t)off, (uint32_t)(end - (uint64_t)(uint32_t)off));
    wcb_unlock();
}

int rsx_texdirty_on_fault(uintptr_t addr)
{
    if (!s_td.on || !s_td.page_flags) return 0;
    const uintptr_t off = addr - (uintptr_t)vm_base;
    if (off >= ((uintptr_t)TD_NPAGES << 12)) return 0;
    const uint32_t i = (uint32_t)(off >> 12);
    int ours = 0;
    wcb_lock();
    ours = (s_td.page_flags[i] & TD_TEX_PROT) != 0;
    if (ours) {
        td_note_write_locked((uint32_t)(i << 12), 1);
        s_td.fault_opens++;
    }
    wcb_unlock();
    return ours;
}

/* Last in line, after the WCB declined too: a page we protected before that
 * another thread opened between the fault and our lock (a texture released
 * or rewritten meanwhile). Retry the access. Declining it would chain to the
 * default action, which resets SIGSEGV to SIG_DFL for the whole process and
 * kills the next real fault. Bounded per thread, as in rsx_wcb_on_fault, so
 * a fault that is genuinely someone else's still reaches them. */
static int td_late_fault(uintptr_t addr)
{
    if (!s_td.on || !s_td.page_flags) return 0;
    const uintptr_t off = addr - (uintptr_t)vm_base;
    if (off >= ((uintptr_t)TD_NPAGES << 12)) return 0;
    if (!(s_td.page_flags[off >> 12] & TD_WAS_PROT)) return 0;
    static WCB_TLS uintptr_t last_addr;
    static WCB_TLS unsigned  last_count;
    if (addr == last_addr) { if (++last_count > 1000u) return 0; }
    else { last_addr = addr; last_count = 0; }
    return 1;
}

/* The runtime hook for a write into guest memory (see ps3emu/guest_access.h).
 * This one marks the pages dirty and removes our read-only protection. */
static void td_guest_write_hook(const void* host, size_t len)
{
    rsx_texdirty_note_write_host(host, len);
}

/* Set by rsx_draw_engine_init / the mode switch. Declared weak beside the
 * read hook so an engine-only test binary links without the runtime. */
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
__attribute__((weak)) void (*volatile g_ps3_guest_write_hook)(const void* host, size_t len);
#else
extern void (*volatile g_ps3_guest_write_hook)(const void* host, size_t len);
#endif

void rsx_texdirty_publish_hook(void)
{
    if (s_td.on) g_ps3_guest_write_hook = td_guest_write_hook;
}
