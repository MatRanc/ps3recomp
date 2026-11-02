/*
 * ps3recomp - measurement-only counters for the c690 SPU job module.
 *
 * Enabled with PS3RECOMP_C690_STATS=1, off otherwise. Nothing here affects a
 * dispatch decision: the hook call sites test `g_c690_stats` (a predicted-not-
 * taken branch) and call out of line only when it is set.
 *
 * Model
 * -----
 * A c690 job starts when the lifted job entry LS 0x135B8 is resolved and ends
 * when the end command handler LS 0x12AB8 is resolved, on the same host thread
 * (each spu_context is pinned to one). The wall time recorded is
 *
 *     t(0x12AB8) - t(0x135B8)   on that thread,
 *
 * i.e. the c690 command-processing wall. Anything the job does after the end
 * command is resolved -- the handler's own submit/policy epilogue -- is not
 * inside the interval and is deliberately excluded.
 *
 * Command opcodes are counted at the resolver: the parser at LS 0x10700
 * dispatches each command through an indirect branch, so spu_lifted_lookup()
 * sees the handler's LS address. s_op_map below is that handler -> opcode map.
 *
 * Exclusive time is charged with a "last entry owns the interval" model: on
 * every lifted-function entry, (now - last_ts) is charged to the function that
 * was current, then current = this entry and last_ts = now. At the end command
 * the remainder is charged to current before the job is closed. Both indirect
 * entries (spu_lifted_lookup) and direct known-target entries (SPU_DIRECT in
 * spu_context.h) feed the model. The per-function sum is therefore equal to
 * the job wall by construction; the report prints that sum-check.
 *
 * Clock: clock_gettime(CLOCK_MONOTONIC) on POSIX (nanoseconds), or
 * QueryPerformanceCounter on Windows. No rdtsc is used, so the values are wall
 * nanoseconds and do not depend on the host TSC rate.
 *
 * Report: cumulative, every 2 s while c690 activity continues (printed from
 * the c690 entry/job-end hooks, so no reporting thread is needed), plus once
 * at process exit via atexit(). A run killed with SIGKILL/timeout still has
 * the latest periodic report in the log. The periodic trigger rides the hook
 * path, so if c690 activity stops, nothing changes and the last printed report
 * is already the final cumulative state.
 *
 * The model cannot follow host call returns, so time spent in a function after
 * it returns to a caller is attributed to the most recently entered function.
 * Interrupt handlers entered through the drain's direct trampoline rather than
 * SPU_DIRECT are likewise not separate entries. Both are accepted limits of
 * the two hook points the counters are allowed to use.
 */
#include "c690_stats.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include "../platform/win32_compat.h"
#endif

int g_c690_stats = 0;

#define C690_IMAGE_ID        6
#define C690_JOB_ENTRY_LS    0x135B8u
#define C690_JOB_END_LS      0x12AB8u
#define C690_HOT_N           20
#define C690_FN_COUNT        (C690_HOT_N + 1)   /* last slot is "other" */
#define C690_MAX_THREADS     64
#define C690_WALL_BUCKETS    65536u             /* 1 us buckets, up to 65.5 ms */
#define C690_REPORT_PERIOD_NS 2000000000ull     /* 2 s between periodic reports */

/* The hot set, in report order (index 0. C690_HOT_N is "other"). */
#define C690_HOT_LIST(X)      \
    X( 0, 0x6D68u) X( 1, 0x7C18u) X( 2, 0x8160u) X( 3, 0xAF98u) \
    X( 4, 0xB238u) X( 5, 0xA798u) X( 6, 0xA9B0u) X( 7, 0xD2B8u) \
    X( 8, 0xD4A8u) X( 9, 0xC98Cu) X(10, 0xFDA0u) X(11, 0xFCB8u) \
    X(12, 0xFCE4u) X(13, 0xE0A8u) X(14, 0x10F78u) X(15, 0x10C70u) \
    X(16, 0x109C0u) X(17, 0x10700u) X(18, 0x132E0u) X(19, 0x12C08u)

static const uint32_t s_hot_lsa[C690_HOT_N] = {
#define C690_HOT_ENTRY(i, a) [i] = a,
    C690_HOT_LIST(C690_HOT_ENTRY)
#undef C690_HOT_ENTRY
};

static int c690_hot_index(uint32_t lsa)
{
    switch (lsa) {
#define C690_HOT_CASE(i, a) case a: return i;
    C690_HOT_LIST(C690_HOT_CASE)
#undef C690_HOT_CASE
    default: return C690_HOT_N;
    }
}

/* Parser 0x10700 handler -> command opcode, in opcode order for the report. */
typedef struct { uint32_t lsa; unsigned op; } c690_op_map;
static const c690_op_map s_op_map[] = {
    { 0x12AB8u, 0x00u },   /* end command: also the job-end marker */
    { 0x11F58u, 0x04u },
    { 0x11DF0u, 0x07u },
    { 0x11BF8u, 0x08u },
    { 0x05000u, 0x09u },
    { 0x12478u, 0x0Au },
    { 0x12830u, 0x0Cu },
    { 0x11A60u, 0x13u },
    { 0x12948u, 0x1Fu },
    { 0x13080u, 0x21u },
    { 0x13520u, 0x25u },
    { 0x13518u, 0x26u },
    { 0x11790u, 0x27u },
    { 0x10F78u, 0x2Au },
    { 0x12DC0u, 0x2Cu },
};
#define C690_OP_MAP_N (sizeof s_op_map / sizeof s_op_map[0])

static int c690_opcode_for_lsa(uint32_t lsa)
{
    for (unsigned i = 0; i < C690_OP_MAP_N; i++)
        if (s_op_map[i].lsa == lsa) return (int)s_op_map[i].op;
    return -1;
}

typedef struct {
    /* Written only by the owning host thread while it runs jobs; read by the
     * periodic and exit reports. Relaxed 64-bit accesses are atomic on every
     * host this runs on; a torn report after a timeout is far less interesting
     * than the counters themselves. */
    unsigned long long fn_ns[C690_FN_COUNT];
    unsigned long long fn_entries[C690_FN_COUNT];
    unsigned long long opcode[256];
    unsigned long long jobs;
    unsigned long long job_ns_sum;
    unsigned long long wall_max_ns;
    unsigned spu_id;
} c690_thread_stats;

static c690_thread_stats s_threads[C690_MAX_THREADS];
static c690_thread_stats s_overflow;       /* > C690_MAX_THREADS host threads */
static int s_thread_count;
static unsigned int s_wall_hist[C690_WALL_BUCKETS];

/* Periodic reporting state. s_t0_ns is the elapsed-time origin (when the
 * counters were enabled); s_last_report_ns and s_report_busy elect one
 * reporting thread when two job ends land in the same 2 s window. */
static unsigned long long s_t0_ns;
static unsigned long long s_last_report_ns;
static int s_report_busy;
static unsigned s_active_jobs;

static SPU_THREAD_LOCAL c690_thread_stats* t_slot;
static SPU_THREAD_LOCAL struct {
    unsigned job_active;
    uint64_t job_start_ns;
    uint64_t last_ns;
    uint32_t current_lsa;
} t_run;

static uint64_t c690_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000000ull
           / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static const char* c690_clock_name(void)
{
#if defined(_WIN32)
    return "QueryPerformanceCounter";
#else
    return "clock_gettime(CLOCK_MONOTONIC)";
#endif
}

static c690_thread_stats* c690_slot(void)
{
    if (__builtin_expect(t_slot != NULL, 1)) return t_slot;
    const int i = __atomic_fetch_add(&s_thread_count, 1, __ATOMIC_RELAXED);
    t_slot = (i < C690_MAX_THREADS) ? &s_threads[i] : &s_overflow;
    t_slot->spu_id = 0xFFFFFFFFu;
    return t_slot;
}

static void c690_charge(c690_thread_stats* s, uint32_t lsa, uint64_t dt)
{
    const int i = c690_hot_index(lsa);
    s->fn_ns[i] += dt;
    s->fn_entries[i]++;
}

static void c690_opcode_inc(c690_thread_stats* s, unsigned op)
{
    if (op < 256u) s->opcode[op]++;
}

static void c690_record_job(c690_thread_stats* s, uint64_t wall_ns)
{
    unsigned us = (unsigned)(wall_ns / 1000u);
    if (us >= C690_WALL_BUCKETS) us = C690_WALL_BUCKETS - 1u;
    s->jobs++;
    s->job_ns_sum += wall_ns;
    if (wall_ns > s->wall_max_ns) s->wall_max_ns = wall_ns;
    __atomic_fetch_add(&s_wall_hist[us], 1u, __ATOMIC_RELAXED);
}

static void c690_maybe_periodic(uint64_t now_ns);   /* defined after the report */

void c690_stats_on_lookup(const struct spu_context* ctx, uint32_t lsa)
{
    if (!ctx || ctx->image_id != C690_IMAGE_ID) return;
    if (lsa == 0) return;                    /* LS 0 is an end, not an entry */
    const uint64_t now = c690_now_ns();
    c690_thread_stats* const s = c690_slot();

    if (t_run.job_active) {
        if (lsa == C690_JOB_END_LS) {
            if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
            c690_opcode_inc(s, 0x00u);       /* the end command itself */
            c690_record_job(s, now - t_run.job_start_ns);
            t_run.job_active = 0;
            t_run.current_lsa = 0;
            __atomic_fetch_sub(&s_active_jobs, 1u, __ATOMIC_RELAXED);
            c690_maybe_periodic(now);
            return;
        }
        if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
        t_run.current_lsa = lsa;
        t_run.last_ns = now;
        const int op = c690_opcode_for_lsa(lsa);
        if (op >= 0) c690_opcode_inc(s, (unsigned)op);
        c690_maybe_periodic(now);
        return;
    }

    if (lsa == C690_JOB_ENTRY_LS) {
        t_run.job_active = 1;
        t_run.job_start_ns = now;
        t_run.last_ns = now;
        t_run.current_lsa = lsa;
        __atomic_fetch_add(&s_active_jobs, 1u, __ATOMIC_RELAXED);
        if (s->spu_id == 0xFFFFFFFFu) s->spu_id = ctx->spu_id;
        c690_maybe_periodic(now);
    }
}

void c690_stats_on_direct(const struct spu_context* ctx, uint32_t lsa)
{
    if (!ctx || ctx->image_id != C690_IMAGE_ID) return;
    if (!t_run.job_active || lsa == 0) return;
    const uint64_t now = c690_now_ns();
    c690_thread_stats* const s = c690_slot();
    if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
    t_run.current_lsa = lsa;
    t_run.last_ns = now;
    c690_maybe_periodic(now);
}

static unsigned c690_pct_tenths(unsigned long long part, unsigned long long total)
{
    return total ? (unsigned)((part * 1000ull) / total) : 0u;
}

static unsigned c690_percentile(unsigned pct)
{
    unsigned long long total = 0, cum = 0, target;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++)
        total += __atomic_load_n(&s_wall_hist[i], __ATOMIC_RELAXED);
    if (!total) return 0;
    target = (total * pct + 99u) / 100u;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++) {
        cum += __atomic_load_n(&s_wall_hist[i], __ATOMIC_RELAXED);
        if (cum >= target) return i;
    }
    return C690_WALL_BUCKETS - 1u;
}

static void c690_aggregate(c690_thread_stats* dst, const c690_thread_stats* src,
                           unsigned long long* per_thread_jobs, unsigned* per_thread_spu,
                           unsigned idx)
{
    /* idx is C690_MAX_THREADS for the shared overflow slot. */
    dst->jobs += src->jobs;
    dst->job_ns_sum += src->job_ns_sum;
    if (src->wall_max_ns > dst->wall_max_ns) dst->wall_max_ns = src->wall_max_ns;
    for (unsigned i = 0; i < C690_FN_COUNT; i++) {
        dst->fn_ns[i] += src->fn_ns[i];
        dst->fn_entries[i] += src->fn_entries[i];
    }
    for (unsigned i = 0; i < 256u; i++) dst->opcode[i] += src->opcode[i];
    per_thread_jobs[idx] = src->jobs;
    per_thread_spu[idx] = src->spu_id;
}

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
/* The runtime's GCM flip counter (and, as a fallback, the PPU boot scaffold)
 * may be absent from a standalone SPU harness; weak keeps that linkable. */
extern unsigned cellGcm_flip_request_count(void) __attribute__((weak));
extern unsigned ppu_boot_frames_presented(void) __attribute__((weak));
#endif

static unsigned c690_frame_flips(void)
{
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
    unsigned f = 0;
    if (cellGcm_flip_request_count) f = cellGcm_flip_request_count();
    if (!f && ppu_boot_frames_presented) f = ppu_boot_frames_presented();
    return f;
#else
    return 0;   /* no weak-symbol spelling: frames are not derivable here */
#endif
}

static void c690_report_common(const char* tag, uint64_t now_ns)
{
    c690_thread_stats total;
    memset(&total, 0, sizeof total);
    unsigned long long per_thread_jobs[C690_MAX_THREADS + 1];
    unsigned per_thread_spu[C690_MAX_THREADS + 1];
    memset(per_thread_jobs, 0, sizeof per_thread_jobs);
    memset(per_thread_spu, 0, sizeof per_thread_spu);

    const int n = __atomic_load_n(&s_thread_count, __ATOMIC_RELAXED);
    const int nslots = n < C690_MAX_THREADS ? n : C690_MAX_THREADS;
    for (int i = 0; i < nslots; i++)
        c690_aggregate(&total, &s_threads[i], per_thread_jobs, per_thread_spu, (unsigned)i);
    if (n > C690_MAX_THREADS)
        c690_aggregate(&total, &s_overflow, per_thread_jobs, per_thread_spu,
                       C690_MAX_THREADS);

    const unsigned long long jobs = total.jobs;
    const unsigned long long wall_ns = total.job_ns_sum;
    const unsigned long long wall_us = wall_ns / 1000ull;
    const double elapsed = (s_t0_ns && now_ns > s_t0_ns)
                         ? (double)(now_ns - s_t0_ns) / 1e9 : 0.0;
    const double jobs_per_s = elapsed > 0.0 ? (double)jobs / elapsed : 0.0;

    fprintf(stderr, "[c690-stats] %s elapsed=%.1fs jobs=%llu jobs/s=%.1f "
                    "in-flight=%u clock=%s\n",
            tag, elapsed, jobs, jobs_per_s,
            __atomic_load_n(&s_active_jobs, __ATOMIC_RELAXED), c690_clock_name());

    const unsigned frames = c690_frame_flips();
    if (frames)
        fprintf(stderr, "[c690-stats] frames=%u jobs/frame=%.3f\n",
                frames, (double)jobs / (double)frames);
    else
        fprintf(stderr, "[c690-stats] jobs/frame=n/a (no flip counter linked)\n");

    if (jobs) {
        const unsigned p50 = c690_percentile(50);
        const unsigned p90 = c690_percentile(90);
        fprintf(stderr,
                "[c690-stats] wall_us p50=%u p90=%u max=%llu%s (sum=%llu)\n",
                p50, p90, total.wall_max_ns / 1000ull,
                total.wall_max_ns / 1000ull >= C690_WALL_BUCKETS ? "+ (bucket cap)" : "",
                wall_us);
    } else {
        fprintf(stderr, "[c690-stats] wall_us p50=n/a p90=n/a max=n/a\n");
    }

    for (int i = 0; i <= nslots; i++) {
        if (i == nslots) {
            if (n <= C690_MAX_THREADS) break;
            if (!per_thread_jobs[C690_MAX_THREADS]) break;
            fprintf(stderr, "[c690-stats] worker=extra spu=? jobs=%llu\n",
                    per_thread_jobs[C690_MAX_THREADS]);
            continue;
        }
        if (!per_thread_jobs[i]) continue;
        fprintf(stderr, "[c690-stats] worker=%d spu=%u jobs=%llu\n",
                i, per_thread_spu[i], per_thread_jobs[i]);
    }

    unsigned long long cmd_total = 0;
    for (unsigned i = 0; i < 256u; i++) cmd_total += total.opcode[i];
    fprintf(stderr, "[c690-stats] opcode histogram (%llu mapped commands):\n",
            cmd_total);
    for (unsigned i = 0; i < C690_OP_MAP_N; i++) {
        const unsigned op = s_op_map[i].op;
        const unsigned long long c = total.opcode[op];
        const unsigned p = c690_pct_tenths(c, cmd_total);
        fprintf(stderr, "[c690-stats]   op 0x%02X handler 0x%05X  %10llu  %u.%u%%\n",
                op, s_op_map[i].lsa, c, p / 10u, p % 10u);
    }

    unsigned long long charged = 0;
    int order[C690_FN_COUNT];
    for (int i = 0; i < C690_FN_COUNT; i++) {
        order[i] = i;
        charged += total.fn_ns[i];
    }
    for (int i = 1; i < C690_FN_COUNT; i++) {          /* insertion sort, ns desc */
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && total.fn_ns[order[j]] < total.fn_ns[v]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    fprintf(stderr, "[c690-stats] exclusive functions (%llu us charged):\n",
            charged / 1000ull);
    for (int k = 0; k < C690_FN_COUNT; k++) {
        const int i = order[k];
        if (i != C690_HOT_N && !total.fn_entries[i]) continue;
        const unsigned long long t = total.fn_ns[i];
        const unsigned p = c690_pct_tenths(t, charged);
        if (i == C690_HOT_N)
            fprintf(stderr, "[c690-stats]   other         %12llu us  %u.%u%%  entries=%llu\n",
                    t / 1000ull, p / 10u, p % 10u, total.fn_entries[i]);
        else
            fprintf(stderr, "[c690-stats]   0x%05X       %12llu us  %u.%u%%  entries=%llu\n",
                    s_hot_lsa[i], t / 1000ull, p / 10u, p % 10u,
                    total.fn_entries[i]);
    }

    if (wall_ns) {
        const long long diff_ns = (long long)charged - (long long)wall_ns;
        const double diff_pct = 100.0 * (double)diff_ns / (double)wall_ns;
        fprintf(stderr,
                "[c690-stats] sum-check: functions=%llu us wall=%llu us diff=%+.2f%% %s\n",
                charged / 1000ull, wall_us, diff_pct,
                diff_pct >= -10.0 && diff_pct <= 10.0 ? "OK (<=10%)" : "OUT OF RANGE");
    } else {
        fprintf(stderr, "[c690-stats] sum-check: no completed jobs\n");
    }
    fflush(stderr);
}

void c690_stats_report(void)
{
    if (!g_c690_stats) return;
    c690_report_common("exit", c690_now_ns());
}

static void c690_maybe_periodic(uint64_t now_ns)
{
    const unsigned long long last =
        __atomic_load_n(&s_last_report_ns, __ATOMIC_RELAXED);
    if (!last || now_ns - last < C690_REPORT_PERIOD_NS) return;
    if (__atomic_exchange_n(&s_report_busy, 1, __ATOMIC_RELAXED)) return;
    if (now_ns - __atomic_load_n(&s_last_report_ns, __ATOMIC_RELAXED) >=
        C690_REPORT_PERIOD_NS) {
        __atomic_store_n(&s_last_report_ns, now_ns, __ATOMIC_RELAXED);
        c690_report_common("periodic", now_ns);
    }
    __atomic_store_n(&s_report_busy, 0, __ATOMIC_RELAXED);
}

static int s_started;

static void c690_stats_startup(void)
{
    if (__atomic_exchange_n(&s_started, 1, __ATOMIC_RELAXED)) return;
    const char* e = getenv("PS3RECOMP_C690_STATS");
    if (!e || strcmp(e, "1") != 0) return;
    g_c690_stats = 1;
    s_t0_ns = c690_now_ns();
    s_last_report_ns = s_t0_ns;
    atexit(c690_stats_report);
    fprintf(stderr, "[c690-stats] enabled image=%d clock=%s report=atexit+2s\n",
            C690_IMAGE_ID, c690_clock_name());
}

#if defined(_MSC_VER)
static void __cdecl c690_stats_startup_c(void) { c690_stats_startup(); }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (__cdecl* c690_stats_ctor)(void) =
    c690_stats_startup_c;
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void c690_stats_ctor(void)
{
    c690_stats_startup();
}
#endif
