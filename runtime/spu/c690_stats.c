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
#include "c690_corpus.h"
#include "spu_context.h"
#include "ps3emu/ps3_trace.h"   /* PS3RECOMP_TRACE: one c690.job span per job */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include "../platform/win32_compat.h"
#endif

/* Live A/B keys (libs/video's control file): the synthetic c690 cost probe is
 * flipped mid-run. Optional so standalone SPU harnesses still build. */
#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_HAVE_LIVE_CTL 1
#  endif
#endif

/* Guest memory, for the transfer hash (read-only). */
extern uint8_t* vm_base;

int g_c690_stats = 0;
int g_c690_hash_on = 0;      /* PS3RECOMP_C690_HASH: sample stride, 0 = off */
int g_c690_gap = 0;          /* PS3RECOMP_C690_GAP: inter-job turnover breakdown */

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

#define C690_GAP_FN_MAX 128
typedef struct {
    int img;
    uint32_t lsa;
    uint64_t ns;
    unsigned long long n;
} c690_gap_fn_ent;

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
    /* PS3RECOMP_C690_GAP: turnover counters for this lane. */
    unsigned long long gap_jobs, gap_total_ns, gap_max_ns;
    unsigned long long gap_first_ns_sum, gap_first_n;
    unsigned long long gap_ctr[12];
    unsigned gap_fn_n;
    unsigned long long gap_fn_other_ns, gap_fn_other_n;
    c690_gap_fn_ent gap_fn[C690_GAP_FN_MAX];
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
    uint64_t tr_t0;          /* PS3RECOMP_TRACE: open c690.job span, 0 when untraced */
    uint32_t tr_desc_ea;     /* the job descriptor the jm2 ABI passed in r3 */
    /* PS3RECOMP_C690_HASH: sampled-job input/output hashes. `hash_tail` is
     * set at the end command and stays until the next job on this lane, so
     * the submit epilogue's output PUTs (which happen after the end command)
     * are part of the output hash. */
    unsigned hash_active, hash_tail;
    uint64_t in_hash, out_hash;
    uint64_t in_bytes, out_bytes;
    uint32_t frame;
    /* PS3RECOMP_C690_GAP: inter-job turnover on this lane. */
    unsigned gap_active, gap_first;
    uint64_t gap_t0, gap_last, gap_first_ns;
    int gap_cur_img;
    uint32_t gap_cur_lsa;
    unsigned gap_entries;
} t_run;

/* ---- PS3RECOMP_C690_HASH: do repeated jobs produce identical bytes? ----
 * A sampled job folds every DMA GET (input) and PUT (output) into FNV-1a.
 * At the next job entry on the lane the previous job's input hash is looked
 * up in a small open-addressing table; a hit means those inputs were seen
 * before. Whether the output hash also matches is counted separately, and
 * the frame number lets the report separate same-frame repeats from
 * frame-to-frame repeats. The output side is the job body's PUTs plus the
 * submit epilogue's PUTs plus the LS 0x2F000..0x33000 output page range. */
#define C690_HASH_SLOTS (1u << 18)
typedef struct {
    uint64_t in_hash, out_hash;
    uint32_t frame;
    uint8_t  used;
} c690_hash_entry;
static c690_hash_entry s_hash_tab[C690_HASH_SLOTS];
static unsigned char s_hash_lock;
static unsigned long long s_job_seq;
static unsigned long long s_hash_samples, s_hash_dup_input, s_hash_dup_output;
static unsigned long long s_hash_dup_prev_frame, s_hash_dup_same_frame;
static unsigned long long s_hash_in_bytes, s_hash_out_bytes;

static uint64_t c690_hash_bytes(uint64_t h, const uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void c690_hash_note_job(uint64_t in_hash, uint64_t out_hash, uint32_t frame)
{
    while (__atomic_test_and_set(&s_hash_lock, __ATOMIC_ACQUIRE))
        ;   /* sampled jobs only: a few hundred per second at most */
    uint32_t i = (uint32_t)((in_hash * 0x9E3779B97F4A7C15ull) >> (64 - 18));
    c690_hash_entry* e = NULL;
    for (uint32_t p = 0; p < 8; p++) {
        c690_hash_entry* c = &s_hash_tab[(i + p) & (C690_HASH_SLOTS - 1u)];
        if (!c->used || c->in_hash == in_hash) { e = c; break; }
    }
    s_hash_samples++;
    if (e && e->used) {
        s_hash_dup_input++;
        if (e->out_hash == out_hash) s_hash_dup_output++;
        if (e->frame == frame) s_hash_dup_same_frame++;
        else                   s_hash_dup_prev_frame++;
        e->out_hash = out_hash;
        e->frame = frame;
    } else if (e) {
        e->used = 1;
        e->in_hash = in_hash;
        e->out_hash = out_hash;
        e->frame = frame;
    }
    __atomic_clear(&s_hash_lock, __ATOMIC_RELEASE);
}

/* Synthetic extra cost per job (C690_SPIN_US live key). Charged inside the
 * job wall, so the report's c690 wall moves with it and the A/B reads the
 * frame-time slope directly. */
static int c690_spin_us(void)
{
#ifdef C690_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_C690_SPIN_US);
#endif
    static int v = -1;
    if (v < 0) { const char* e = getenv("C690_SPIN_US"); v = e ? atoi(e) : 0; }
    return v;
}

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

/* ===========================================================================
 * PS3RECOMP_C690_GAP: where the time between two c690 jobs on one lane goes.
 *
 * The job span is 0x135B8..0x12AB8; everything a lane does between the end
 * command and the next entry of 0x135B8 is its job turnover: the c690 submit
 * epilogue, the WWS policy module's list/ticket handling, its descriptor and
 * code DMAs, the GETLLAR/PUTLLC claim, and the runtime's MFC/coherency work
 * inside all of those. Off by default; measurement only.
 *
 * Exclusive time is the same "last entry owns the interval" model the job
 * counters use, but across ALL lifted images (the PM is image 2) and only
 * while a gap is open. Function entries are only visible at indirect dispatch
 * (spu_lifted_lookup) and SPU_DIRECT; a direct host call inside the PM is
 * charged to the function it was entered from, so the table is a lower bound
 * for that function and the "other" bucket absorbs any overflow. The counters
 * below separate the runtime pieces that sit inside those intervals.
 * ===========================================================================*/
static unsigned int s_gap_hist[C690_WALL_BUCKETS];

static c690_thread_stats* c690_slot(void);   /* defined below */
uint64_t c690_gap_now(void) { return c690_now_ns(); }

static void c690_gap_charge(int img, uint32_t lsa, uint64_t dt)
{
    if (!dt || !lsa) return;
    c690_thread_stats* const s = c690_slot();
    for (unsigned i = 0; i < s->gap_fn_n; i++) {
        if (s->gap_fn[i].img == img && s->gap_fn[i].lsa == lsa) {
            s->gap_fn[i].ns += dt;
            s->gap_fn[i].n++;
            return;
        }
    }
    if (s->gap_fn_n < C690_GAP_FN_MAX) {
        c690_gap_fn_ent* e = &s->gap_fn[s->gap_fn_n++];
        e->img = img; e->lsa = lsa; e->ns = dt; e->n = 1;
    } else {
        s->gap_fn_other_ns += dt;
        s->gap_fn_other_n++;
    }
}

static void c690_gap_entry_at(const struct spu_context* ctx, uint32_t lsa,
                              uint64_t now)
{
    if (!t_run.gap_active || !lsa) return;
    if (t_run.gap_cur_lsa)
        c690_gap_charge(t_run.gap_cur_img, t_run.gap_cur_lsa, now - t_run.gap_last);
    t_run.gap_last = now;
    t_run.gap_cur_img = ctx ? ctx->image_id : -1;
    t_run.gap_cur_lsa = lsa;
    t_run.gap_entries++;
    if (!t_run.gap_first) {
        t_run.gap_first = 1;
        t_run.gap_first_ns = now - t_run.gap_t0;
    }
}

static void c690_gap_open(uint64_t now)
{
    t_run.gap_active = 1;
    t_run.gap_first = 0;
    t_run.gap_t0 = now;
    t_run.gap_last = now;
    t_run.gap_first_ns = 0;
    t_run.gap_cur_img = C690_IMAGE_ID;
    t_run.gap_cur_lsa = C690_JOB_END_LS;
    t_run.gap_entries = 0;
}

static void c690_gap_close(uint64_t now)
{
    if (!t_run.gap_active) return;
    if (t_run.gap_cur_lsa)
        c690_gap_charge(t_run.gap_cur_img, t_run.gap_cur_lsa, now - t_run.gap_last);
    const uint64_t dt = now - t_run.gap_t0;
    t_run.gap_active = 0;
    t_run.gap_cur_lsa = 0;
    if (!dt) return;
    c690_thread_stats* const s = c690_slot();
    unsigned us = (unsigned)(dt / 1000u);
    if (us >= C690_WALL_BUCKETS) us = C690_WALL_BUCKETS - 1u;
    __atomic_fetch_add(&s_gap_hist[us], 1u, __ATOMIC_RELAXED);
    s->gap_jobs++;
    s->gap_total_ns += dt;
    if (dt > s->gap_max_ns) s->gap_max_ns = dt;
    if (t_run.gap_first) {
        s->gap_first_ns_sum += t_run.gap_first_ns;
        s->gap_first_n++;
    }
}

/* Called from spu_lifted_lookup for every indirect dispatch (any image). The
 * c690 job entry itself closes the gap; every other entry is charged. */
void c690_gap_on_lookup(const struct spu_context* ctx, uint32_t lsa, uint64_t now)
{
    if (!g_c690_gap || !ctx) return;
    if (ctx->image_id == C690_IMAGE_ID && lsa == C690_JOB_ENTRY_LS && !t_run.job_active)
        c690_gap_close(now);
    else
        c690_gap_entry_at(ctx, lsa, now);
}

/* Called from SPU_DIRECT (tail-call entries) for any image. */
void c690_gap_on_direct(const struct spu_context* ctx, uint32_t lsa, uint64_t now)
{
    if (!g_c690_gap || !ctx) return;
    c690_gap_entry_at(ctx, lsa, now);
}

void c690_gap_on_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint32_t size)
{
    if (!g_c690_gap || !ctx || !t_run.gap_active) return;
    c690_thread_stats* const s = c690_slot();
    if (is_put) {
        s->gap_ctr[1]++; s->gap_ctr[3] += size;
    } else {
        s->gap_ctr[0]++; s->gap_ctr[2] += size;
        /* The c690 job-code module loads at LS 0x4000 and its lifted code ends
         * at 0x14840 (job entry 0x135B8). A GET landing there is a re-load. */
        if (lsa >= 0x4000u && lsa < 0x14840u) {
            s->gap_ctr[4]++; s->gap_ctr[5] += size;
        }
    }
}

void c690_gap_on_atomic(const struct spu_context* ctx, int kind, int ok)
{
    if (!g_c690_gap || !ctx || !t_run.gap_active) return;
    c690_thread_stats* const s = c690_slot();
    if (kind == 0) s->gap_ctr[6]++;
    else {
        s->gap_ctr[7]++;
        if (!ok) s->gap_ctr[8]++;
    }
}

void c690_gap_on_overlay(int kind, int ok)
{
    if (!g_c690_gap) return;
    c690_thread_stats* const s = c690_slot();
    if (kind == 0) s->gap_ctr[9]++;
    else {
        s->gap_ctr[10]++;
        if (!ok) s->gap_ctr[11]++;
    }
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
static unsigned c690_frame_flips(void);            /* defined with the report */

void c690_stats_on_lookup(const struct spu_context* ctx, uint32_t lsa)
{
    if (!ctx) return;
    if (lsa == 0) return;                    /* LS 0 is an end, not an entry */
    const uint64_t now = c690_now_ns();
    if (__builtin_expect(g_c690_gap, 0)) c690_gap_on_lookup(ctx, lsa, now);
    if (ctx->image_id != C690_IMAGE_ID) return;
    c690_thread_stats* const s = c690_slot();

    if (t_run.job_active) {
        if (lsa == C690_JOB_END_LS) {
            if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
            c690_opcode_inc(s, 0x00u);       /* the end command itself */
            if (__builtin_expect(g_c690_corpus_on, 0)) c690_corpus_job_end(ctx);
            c690_record_job(s, now - t_run.job_start_ns);
            if (t_run.hash_active) {
                /* The assembled vertex buffer lives at LS 0x2F870; fold the
                 * output page range so a job whose outputs never leave through
                 * this lane's DMA still contributes its result. */
                t_run.out_hash = c690_hash_bytes(t_run.out_hash,
                                                 ctx->ls + 0x2F000, 0x4000);
                t_run.out_bytes += 0x4000;
                /* Keep hashing PUTs until the next job on this lane: the job's
                 * output DMA is in the submit epilogue, after this command. */
                t_run.hash_tail = 1;
                t_run.frame = c690_frame_flips();
            }
            ps3_trace_end(t_run.tr_t0, PS3_TR_C690_JOB, t_run.tr_desc_ea,
                          (uint32_t)ctx->spu_id);
            /* Timeline: the note is the instant the job's slot PUT can follow,
             * i.e. the signal a walker wait is waiting for. */
            if (ps3_trace_on()) ps3_trace_note(PS3_TR_NOTE_C690_END, (uint32_t)ctx->spu_id);
            t_run.tr_t0 = 0;
            t_run.job_active = 0;
            t_run.current_lsa = 0;
            __atomic_fetch_sub(&s_active_jobs, 1u, __ATOMIC_RELAXED);
            if (__builtin_expect(g_c690_gap, 0)) c690_gap_open(now);
            c690_maybe_periodic(now);
            return;
        }
        if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
        t_run.current_lsa = lsa;
        t_run.last_ns = now;
        const int op = c690_opcode_for_lsa(lsa);
        if (op >= 0) {
            c690_opcode_inc(s, (unsigned)op);
            if (__builtin_expect(g_c690_corpus_on, 0))
                c690_corpus_note_op(ctx, (unsigned)op);
        }
        c690_maybe_periodic(now);
        return;
    }

    if (lsa == C690_JOB_ENTRY_LS) {
        /* Close the previous sampled job's output tail. */
        if (t_run.hash_active && t_run.hash_tail) {
            __atomic_fetch_add(&s_hash_in_bytes, t_run.in_bytes, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_hash_out_bytes, t_run.out_bytes, __ATOMIC_RELAXED);
            c690_hash_note_job(t_run.in_hash, t_run.out_hash, t_run.frame);
        }
        t_run.hash_active = 0;   /* a body with no tail is discarded */
        t_run.job_active = 1;
        t_run.job_start_ns = now;
        t_run.hash_tail = 0;
        if (g_c690_hash_on) {
            const unsigned long long seq =
                __atomic_fetch_add(&s_job_seq, 1ull, __ATOMIC_RELAXED);
            if (!t_run.hash_active &&
                seq % (unsigned long long)g_c690_hash_on == 0) {
                t_run.hash_active = 1;
                t_run.in_hash = 0xcbf29ce484222325ull;
                t_run.out_hash = 0xcbf29ce484222325ull;
                t_run.in_bytes = 0;
                t_run.out_bytes = 0;
            }
        }
        const int spin = c690_spin_us();
        if (spin > 0) {
            const uint64_t deadline = now + (uint64_t)spin * 1000ull;
            uint64_t t;
            do { t = c690_now_ns(); } while (t < deadline);
            t_run.job_start_ns = now;      /* wall includes the synthetic cost */
        }
        t_run.last_ns = c690_now_ns();
        t_run.current_lsa = lsa;
        t_run.tr_desc_ea = ctx->gpr[3]._u32[0];
        if (__builtin_expect(g_c690_corpus_on, 0))
            c690_corpus_job_begin(ctx, t_run.tr_desc_ea, c690_frame_flips());
        t_run.tr_t0 = ps3_trace_begin();
        __atomic_fetch_add(&s_active_jobs, 1u, __ATOMIC_RELAXED);
        if (s->spu_id == 0xFFFFFFFFu) s->spu_id = ctx->spu_id;
        c690_maybe_periodic(now);
    }
}

void c690_stats_on_direct(const struct spu_context* ctx, uint32_t lsa)
{
    if (!ctx) return;
    if (lsa != 0 && __builtin_expect(g_c690_gap, 0)) {
        /* SPU_DIRECT fires for every image; the gap tracer needs the PM's
         * (image 2) tail calls too. */
        c690_gap_on_direct(ctx, lsa, c690_now_ns());
    }
    if (ctx->image_id != C690_IMAGE_ID) return;
    if (!t_run.job_active || lsa == 0) return;
    const uint64_t now = c690_now_ns();
    c690_thread_stats* const s = c690_slot();
    if (t_run.current_lsa) c690_charge(s, t_run.current_lsa, now - t_run.last_ns);
    t_run.current_lsa = lsa;
    t_run.last_ns = now;
    c690_maybe_periodic(now);
}

void c690_stats_on_transfer(const struct spu_context* ctx, int is_put,
                            uint32_t lsa, uint64_t ea, uint32_t size)
{
    if (!ctx || ctx->image_id != C690_IMAGE_ID || !t_run.hash_active ||
        size == 0)
        return;
    if (is_put) {
        /* Output: job body and the submit epilogue that follows it. */
        t_run.out_hash = c690_hash_bytes(t_run.out_hash,
                                         ctx->ls + (lsa & SPU_LS_MASK), size);
        t_run.out_bytes += size;
    } else if (!t_run.hash_tail) {
        /* Input: only while the job body is running, not the epilogue. */
        if (!vm_base) return;
        t_run.in_hash = c690_hash_bytes(t_run.in_hash,
                                        vm_base + (uint32_t)ea, size);
        t_run.in_bytes += size;
    }
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

static unsigned c690_gap_percentile(unsigned pct)
{
    unsigned long long total = 0, cum = 0, target;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++)
        total += __atomic_load_n(&s_gap_hist[i], __ATOMIC_RELAXED);
    if (!total) return 0;
    target = (total * pct + 99u) / 100u;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++) {
        cum += __atomic_load_n(&s_gap_hist[i], __ATOMIC_RELAXED);
        if (cum >= target) return i;
    }
    return C690_WALL_BUCKETS - 1u;
}

static void c690_gap_report(void)
{
    if (!g_c690_gap) return;
    unsigned long long jobs = 0, total = 0, max = 0, fsum = 0, fn = 0;
    unsigned long long ctr[12] = {0};
    static c690_gap_fn_ent agg[C690_MAX_THREADS * 8 + 16];
    unsigned an = 0;

    const int n = __atomic_load_n(&s_thread_count, __ATOMIC_RELAXED);
    const int nslots = n < C690_MAX_THREADS ? n : C690_MAX_THREADS;
    for (int i = 0; i <= nslots; i++) {
        const c690_thread_stats* st;
        if (i < nslots) st = &s_threads[i];
        else {
            if (n <= C690_MAX_THREADS) break;
            st = &s_overflow;
        }
        jobs += st->gap_jobs; total += st->gap_total_ns;
        if (st->gap_max_ns > max) max = st->gap_max_ns;
        fsum += st->gap_first_ns_sum; fn += st->gap_first_n;
        for (unsigned k = 0; k < 12u; k++) ctr[k] += st->gap_ctr[k];
        for (unsigned k = 0; k < st->gap_fn_n; k++) {
            const c690_gap_fn_ent* e = &st->gap_fn[k];
            unsigned j = 0;
            for (; j < an; j++)
                if (agg[j].img == e->img && agg[j].lsa == e->lsa) break;
            if (j == an && an < sizeof agg / sizeof agg[0]) {
                agg[an].img = e->img; agg[an].lsa = e->lsa;
                agg[an].ns = 0; agg[an].n = 0;
                an++;
            }
            if (j < an) { agg[j].ns += e->ns; agg[j].n += e->n; }
        }
    }
    if (!jobs) {
        fprintf(stderr, "[c690-gap] no completed inter-job gaps\n");
        return;
    }
    for (unsigned i = 1; i < an; i++) {          /* insertion sort, ns desc */
        const c690_gap_fn_ent v = agg[i];
        int j = (int)i - 1;
        while (j >= 0 && agg[j].ns < v.ns) { agg[j + 1] = agg[j]; j--; }
        agg[j + 1] = v;
    }
    fprintf(stderr,
            "[c690-gap] gaps=%llu total_us=%llu mean_us=%.2f p50=%u p90=%u "
            "p99=%u max_us=%llu first_entry_us=%.2f\n",
            jobs, total / 1000ull, (double)total / (double)jobs / 1000.0,
            c690_gap_percentile(50), c690_gap_percentile(90),
            c690_gap_percentile(99), max / 1000ull,
            fn ? (double)fsum / (double)fn / 1000.0 : 0.0);
    fprintf(stderr, "[c690-gap] exclusive intervals (%llu us):\n", total / 1000ull);
    const unsigned show = an < 24u ? an : 24u;
    for (unsigned i = 0; i < show; i++) {
        const unsigned p = c690_pct_tenths(agg[i].ns, total);
        fprintf(stderr, "[c690-gap]   img=%d 0x%05X  %10llu us  %u.%u%%  n=%llu  avg=%.2f us\n",
                agg[i].img, agg[i].lsa, agg[i].ns / 1000ull, p / 10u, p % 10u,
                agg[i].n, (double)agg[i].ns / (double)agg[i].n / 1000.0);
    }
    fprintf(stderr, "[c690-gap] runtime: mfc_get=%llu (%.2f MiB) mfc_put=%llu (%.2f MiB) "
                    "code_get=%llu (%.2f MiB) getllar=%llu putllc=%llu fail=%llu "
                    "ovl_load=%llu ovl_verify=%llu ovl_bad=%llu\n",
            ctr[0], (double)ctr[2] / 1048576.0, ctr[1], (double)ctr[3] / 1048576.0,
            ctr[4], (double)ctr[5] / 1048576.0, ctr[6], ctr[7], ctr[8],
            ctr[9], ctr[10], ctr[11]);
}

/* Rolling 2 s window for A/B: a per-window delta of the job wall and the
 * inter-job gaps, so each arm's lane occupancy and gap percentiles can be read
 * straight from the log without a timeline dump. */
static unsigned long long w_jobs, w_job_ns, w_gap_jobs, w_gap_total;
static unsigned int w_gap_hist[C690_WALL_BUCKETS];
static int w_gap_init;

static unsigned c690_hist_percentile(const unsigned int* h, unsigned pct)
{
    unsigned long long total = 0, cum = 0, target;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++) total += h[i];
    if (!total) return 0;
    target = (total * pct + 99u) / 100u;
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++) {
        cum += h[i];
        if (cum >= target) return i;
    }
    return C690_WALL_BUCKETS - 1u;
}

static void c690_gap_window_report(unsigned long long jobs, unsigned long long wall_ns)
{
    if (!g_c690_gap) return;
    unsigned long long gj = 0, gt = 0;
    const int n = __atomic_load_n(&s_thread_count, __ATOMIC_RELAXED);
    const int nslots = n < C690_MAX_THREADS ? n : C690_MAX_THREADS;
    for (int i = 0; i <= nslots; i++) {
        const c690_thread_stats* st;
        if (i < nslots) st = &s_threads[i];
        else { if (n <= C690_MAX_THREADS) break; st = &s_overflow; }
        gj += st->gap_jobs; gt += st->gap_total_ns;
    }
    if (!w_gap_init) {
        w_gap_init = 1;
        w_jobs = jobs; w_job_ns = wall_ns; w_gap_jobs = gj; w_gap_total = gt;
        for (unsigned i = 0; i < C690_WALL_BUCKETS; i++)
            w_gap_hist[i] = __atomic_load_n(&s_gap_hist[i], __ATOMIC_RELAXED);
        return;
    }
    const unsigned long long d_jobs = jobs - w_jobs;
    const unsigned long long d_wall = wall_ns - w_job_ns;
    const unsigned long long d_gj = gj - w_gap_jobs;
    const unsigned long long d_gt = gt - w_gap_total;
    static unsigned int dh[C690_WALL_BUCKETS];
    for (unsigned i = 0; i < C690_WALL_BUCKETS; i++) {
        const unsigned cur = __atomic_load_n(&s_gap_hist[i], __ATOMIC_RELAXED);
        dh[i] = cur - w_gap_hist[i];
        w_gap_hist[i] = cur;
    }
    w_jobs = jobs; w_job_ns = wall_ns; w_gap_jobs = gj; w_gap_total = gt;
    if (!d_gj || !d_jobs) return;
    fprintf(stderr,
            "[c690-gap-window] jobs=%llu job_us=%llu gaps=%llu gap_us=%llu "
            "gap_p50=%u gap_p90=%u gap_p99=%u lane_busy=%.3f\n",
            d_jobs, d_wall / 1000ull, d_gj, d_gt / 1000ull,
            c690_hist_percentile(dh, 50), c690_hist_percentile(dh, 90),
            c690_hist_percentile(dh, 99),
            d_wall + d_gt ? (double)d_wall / (double)(d_wall + d_gt) : 0.0);
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
        const unsigned p99 = c690_percentile(99);
        fprintf(stderr,
                "[c690-stats] wall_us p50=%u p90=%u p99=%u mean=%.1f max=%llu%s (sum=%llu)\n",
                p50, p90, p99, (double)wall_us / (double)jobs,
                total.wall_max_ns / 1000ull,
                total.wall_max_ns / 1000ull >= C690_WALL_BUCKETS ? "+ (bucket cap)" : "",
                wall_us);
    } else {
        fprintf(stderr, "[c690-stats] wall_us p50=n/a p90=n/a p99=n/a max=n/a\n");
    }

    if (g_c690_hash_on) {
        const unsigned long long n = __atomic_load_n(&s_hash_samples, __ATOMIC_RELAXED);
        const unsigned long long di = __atomic_load_n(&s_hash_dup_input, __ATOMIC_RELAXED);
        const unsigned long long dobj = __atomic_load_n(&s_hash_dup_output, __ATOMIC_RELAXED);
        const unsigned long long dpf = __atomic_load_n(&s_hash_dup_prev_frame, __ATOMIC_RELAXED);
        const unsigned long long dsf = __atomic_load_n(&s_hash_dup_same_frame, __ATOMIC_RELAXED);
        const unsigned pdi = c690_pct_tenths(di, n);
        const unsigned pdo = c690_pct_tenths(dobj, n);
        fprintf(stderr,
                "[c690-hash] stride=%d samples=%llu in_MiB=%.2f out_MiB=%.2f "
                "dup_input=%llu (%u.%u%%) dup_output=%llu (%u.%u%%) "
                "dup_prev_frame=%llu dup_same_frame=%llu\n",
                g_c690_hash_on, n,
                (double)__atomic_load_n(&s_hash_in_bytes, __ATOMIC_RELAXED) / 1048576.0,
                (double)__atomic_load_n(&s_hash_out_bytes, __ATOMIC_RELAXED) / 1048576.0,
                di, pdi / 10u, pdi % 10u, dobj, pdo / 10u, pdo % 10u,
                dpf, dsf);
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

    c690_gap_report();
    c690_gap_window_report(jobs, wall_ns);

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
    const char* h = getenv("PS3RECOMP_C690_HASH");
    const char* c = getenv("PS3RECOMP_C690_CORPUS");
    const char* g = getenv("PS3RECOMP_C690_GAP");
    const int hash_stride = (h && *h) ? atoi(h) : 0;
    const int corpus_on = c && *c;
    const int gap_on = g && *g && strcmp(g, "0") != 0;
    if ((!e || strcmp(e, "1") != 0) && hash_stride <= 0 && !corpus_on && !gap_on) return;
    g_c690_stats = 1;                     /* job tracking also feeds the hash/corpus */
    g_c690_gap = gap_on;
    g_c690_hash_on = hash_stride > 0 ? hash_stride : 0;
    s_t0_ns = c690_now_ns();
    s_last_report_ns = s_t0_ns;
    atexit(c690_stats_report);
    fprintf(stderr, "[c690-stats] enabled image=%d clock=%s report=atexit+2s"
                    " hash_stride=%d\n",
            C690_IMAGE_ID, c690_clock_name(), g_c690_hash_on);
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
