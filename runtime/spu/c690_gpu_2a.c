/*
 * c690 Edge 0x2A GPU ring binding for the position-first 32-byte class.
 * See c690_gpu_2a.h.  Live key PS3RECOMP_C690_GPU_2A, default 0.
 *
 * The lifted 0x10F78 body is untouched: the wrapper snapshots r3 at entry and
 * exports the source arrays/offsets at exit.  Guest memory keeps the exact
 * SPU records, so the GPU range is a resolver overlay, never a correctness
 * dependency.
 *
 * Clean room: own reverse engineering of the lifted c690 module and the 0x2A
 * corpus; the stage/corpus/differential method follows NHL Legacy Native
 * (MIT) with attribution.  No GPL emulator source was read.
 */
#include "spu_context.h"
#include "c690_gpu_2a.h"
#include "../../libs/video/ps3_live_ctl.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>

int g_c690_gpu_2a_on;

#define C690_GPU_2A_MAX_RECORDS 8192u
#define C690_GPU_2A_MAX_EXPORTS 8u
#define C690_GPU_2A_LS_MASK     0x3FFFFu

typedef struct c690_gpu_2a_export {
    int      valid;
    uint32_t out_base, end, count;
    uint16_t* offs;
    uint8_t* arrays;
    uint32_t extent;
    uint8_t* seed;        /* count * 32 exact SPU bytes */
    uint32_t seq;
} c690_gpu_2a_export;

typedef struct c690_gpu_2a_lane {
    int      active;
    uint32_t r3;
    uint32_t out_base;      /* entry cursor */
    uint32_t seq;
    c690_gpu_2a_export exp[C690_GPU_2A_MAX_EXPORTS];
    unsigned exp_next;
} c690_gpu_2a_lane;

/*
 * Lane state is per HOST THREAD, not per ctx->spu_id.
 *
 * The c690 mesh jobs do not run as lv2 SPU threads on this port: the SPURS
 * PM workers dispatch them through spu_workload_dispatch_job ->
 * spu_run_spurs_job, which builds a stack-local spu_context with
 * spu_context_init(&ctx, 0). Every one of the five lanes therefore reports
 * spu_id 0 (see [c690-stats] worker=N spu=0 in the live logs). Keying the
 * export state on spu_id made all five concurrent lanes share one slot: the
 * enter/exit/PUT state (r3, cursor, export ring) was written by one thread
 * while another submitted/freed the same job buffers, which is the double
 * free seen at the first key-on switch.
 *
 * A lane IS one host thread here (spu_run_spurs_job and the lifted thread
 * run the whole job, including the submit epilogue PUT, synchronously on the
 * caller). So thread-local state gives each lane its own export list with no
 * lock and no id dependence.
 */
static SPU_THREAD_LOCAL c690_gpu_2a_lane t_lane;
static c690_gpu_2a_sink_fn s_sink;
static uint32_t s_seq;

/* Diagnostics. */
static unsigned long s_stat_enter, s_stat_class, s_stat_fallback,
    s_stat_nosink, s_stat_put, s_stat_unmatched, s_stat_records,
    s_stat_fallback_records;
static unsigned long long s_stat_last_ns;

static inline uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int c690_gpu_2a_enabled(void)
{
    int on = ps3_live_ctl_get(PS3_LIVE_C690_GPU_2A) != 0;
    __atomic_store_n(&g_c690_gpu_2a_on, on, __ATOMIC_RELAXED);
    return on;
}

void c690_gpu_2a_set_sink(c690_gpu_2a_sink_fn fn)
{
    s_sink = fn;
}

static void export_free(c690_gpu_2a_export* e)
{
    free(e->offs);
    free(e->arrays);
    free(e->seed);
    memset(e, 0, sizeof *e);
}

static c690_gpu_2a_lane* lane_self(void)
{
    return &t_lane;
}

void c690_gpu_2a_enter(struct spu_context* ctx)
{
    if (!ctx) return;
    c690_gpu_2a_lane* l = lane_self();
    l->active = 0;          /* a lost exit must not confuse the next entry */
    if (!c690_gpu_2a_enabled()) return;
    l->active = 1;
    l->r3 = ctx->gpr[3]._u32[0] & C690_GPU_2A_LS_MASK;
    l->out_base = be32(ctx->ls + ((l->r3 + 0xD0u) & C690_GPU_2A_LS_MASK));
    l->seq = __atomic_add_fetch(&s_seq, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_stat_enter, 1, __ATOMIC_RELAXED);
}

/*
 * Format A: cmd[0] is a u16 byte-offset list, one entry per record.
 * Format B: the implicit identity list offs[k] = k * 16.
 * The record begins with the position triple, so compare those 12 bytes.
 */
int c690_gpu_2a_classify(const uint8_t* ls, const uint8_t* recs, uint32_t r3,
                         uint32_t count, uint32_t cmd0, uint16_t* offs,
                         int* format_out)
{
    if (!ls || !recs || !count || count > C690_GPU_2A_MAX_RECORDS) return 0;
    const uint32_t arr0 = be32(ls + ((r3 + 0x260u) & C690_GPU_2A_LS_MASK));
    if (!arr0 || arr0 >= SPU_LS_SIZE) return 0;
    uint32_t ok = 0;
    if (cmd0 && cmd0 < SPU_LS_SIZE) {
        for (uint32_t k = 0; k < count; k++) {
            const uint16_t o = be16(ls + ((cmd0 + 2u * k) & C690_GPU_2A_LS_MASK));
            offs[k] = o;
            const uint32_t a = (arr0 + o) & C690_GPU_2A_LS_MASK;
            if (a + 12u <= SPU_LS_SIZE && !memcmp(ls + a, recs + k * 32u, 12)) ok++;
        }
        if ((uint64_t)ok * 1000u >= (uint64_t)count * 999u) {
            if (format_out) *format_out = 0;
            return 1;
        }
    }
    ok = 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint16_t o = (uint16_t)(k * 16u);
        offs[k] = o;
        const uint32_t a = (arr0 + o) & C690_GPU_2A_LS_MASK;
        if (a + 12u <= SPU_LS_SIZE && !memcmp(ls + a, recs + k * 32u, 12)) ok++;
    }
    if ((uint64_t)ok * 1000u >= (uint64_t)count * 999u) {
        if (format_out) *format_out = 1;
        return 1;
    }
    return 0;
}

static void export_push(c690_gpu_2a_lane* l, c690_gpu_2a_export* e)
{
    for (unsigned i = 0; i < C690_GPU_2A_MAX_EXPORTS; i++) {
        const unsigned at = (l->exp_next + i) % C690_GPU_2A_MAX_EXPORTS;
        if (!l->exp[at].valid) {
            l->exp[at] = *e;
            l->exp_next = (at + 1u) % C690_GPU_2A_MAX_EXPORTS;
            return;
        }
    }
    const unsigned at = l->exp_next % C690_GPU_2A_MAX_EXPORTS;
    export_free(&l->exp[at]);
    l->exp[at] = *e;
    l->exp_next = (at + 1u) % C690_GPU_2A_MAX_EXPORTS;
}

void c690_gpu_2a_exit(struct spu_context* ctx)
{
    if (!ctx) return;
    c690_gpu_2a_lane* l = lane_self();
    if (!l->active) return;
    l->active = 0;
    if (!__atomic_load_n(&g_c690_gpu_2a_on, __ATOMIC_RELAXED)) return;

    const uint32_t r3 = l->r3;
    const uint32_t out_base = l->out_base;
    const uint32_t end = be32(ctx->ls + ((r3 + 0xD0u) & C690_GPU_2A_LS_MASK));
    const uint32_t delta = (end - out_base) & C690_GPU_2A_LS_MASK;
    if (!out_base || end <= out_base || (delta & 31u) ||
        delta > C690_GPU_2A_MAX_RECORDS * 32u ||
        (uint64_t)out_base + delta > SPU_LS_SIZE)
        return;
    const uint32_t count = delta / 32u;
    const uint32_t cmd0 = be32(ctx->ls + ((r3 + 0x150u) & C690_GPU_2A_LS_MASK));

    uint16_t* offs = (uint16_t*)malloc((size_t)count * sizeof(uint16_t));
    int format = -1;
    if (!offs || !c690_gpu_2a_classify(ctx->ls, ctx->ls + out_base, r3, count,
                                       cmd0, offs, &format)) {
        free(offs);
        __atomic_fetch_add(&s_stat_fallback, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_stat_fallback_records, count, __ATOMIC_RELAXED);
        return;
    }
    uint32_t maxoff = 0;
    for (uint32_t k = 0; k < count; k++)
        if (offs[k] > maxoff) maxoff = offs[k];
    uint32_t extent = format == 1 ? count * 16u : maxoff + 16u;
    if (extent > 0x10000u) extent = 0x10000u;

    uint8_t* arrays = (uint8_t*)malloc((size_t)extent * 4u);
    uint8_t* seed = (uint8_t*)malloc((size_t)count * 32u);
    if (!arrays || !seed) {
        free(offs); free(arrays); free(seed);
        __atomic_fetch_add(&s_stat_fallback, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_stat_fallback_records, count, __ATOMIC_RELAXED);
        return;
    }
    /* Source arrays: pos, nrm, tan, uv (r3+0x260 order). */
    const uint32_t arr[4] = {
        be32(ctx->ls + ((r3 + 0x260u) & C690_GPU_2A_LS_MASK)),
        be32(ctx->ls + ((r3 + 0x264u) & C690_GPU_2A_LS_MASK)),
        be32(ctx->ls + ((r3 + 0x268u) & C690_GPU_2A_LS_MASK)),
        be32(ctx->ls + ((r3 + 0x26Cu) & C690_GPU_2A_LS_MASK)),
    };
    for (int i = 0; i < 4; i++) {
        const uint32_t base = arr[i];
        const uint32_t room = base < SPU_LS_SIZE ? SPU_LS_SIZE - base : 0;
        const uint32_t n = extent < room ? extent : room;
        if (n) memcpy(arrays + (size_t)i * extent, ctx->ls + base, n);
        if (n < extent) memset(arrays + (size_t)i * extent + n, 0, extent - n);
    }
    memcpy(seed, ctx->ls + out_base, (size_t)count * 32u);

    c690_gpu_2a_export e;
    memset(&e, 0, sizeof e);
    e.valid = 1;
    e.out_base = out_base;
    e.end = end;
    e.count = count;
    e.offs = offs;
    e.arrays = arrays;
    e.extent = extent;
    e.seed = seed;
    e.seq = l->seq;
    export_push(l, &e);
    __atomic_fetch_add(&s_stat_class, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_stat_records, count, __ATOMIC_RELAXED);
}

void c690_gpu_2a_put(const struct spu_context* ctx, uint32_t lsa, uint64_t ea,
                     uint32_t size)
{
    if (!__atomic_load_n(&g_c690_gpu_2a_on, __ATOMIC_RELAXED) || !ctx || !size) return;
    c690_gpu_2a_lane* l = lane_self();
    for (unsigned i = 0; i < C690_GPU_2A_MAX_EXPORTS; i++) {
        c690_gpu_2a_export* e = &l->exp[i];
        if (!e->valid) continue;
        /* Any PUT that overlaps the record range proves the ring mapping.
         * The submit PUT can be split at the 0x4000 DMA chunk size, so the
         * whole range need not arrive in one transfer; the ring is linear, so
         * one point extrapolates the record EA. */
        if (e->out_base >= lsa + size || e->end <= lsa) continue;
        const int64_t delta = (int64_t)e->out_base - (int64_t)lsa;
        const uint32_t rec_ea = (uint32_t)((int64_t)(uint32_t)ea + delta);
        c690_gpu_2a_job job;
        memset(&job, 0, sizeof job);
        job.ea = rec_ea;
        job.count = e->count;
        job.stride = 32;
        job.extent = e->extent;
        job.offs = e->offs;
        job.arrays = e->arrays;
        job.seed = e->seed;
        job.seq = e->seq;
        int taken = s_sink ? s_sink(&job) : 0;
        if (!taken) {
            __atomic_fetch_add(&s_stat_nosink, 1, __ATOMIC_RELAXED);
            free(job.offs); free(job.arrays); free(job.seed);
        }
        e->offs = NULL; e->arrays = NULL; e->seed = NULL;
        export_free(e);
        __atomic_fetch_add(&s_stat_put, 1, __ATOMIC_RELAXED);
    }
    c690_gpu_2a_stats_maybe();
}

void c690_gpu_2a_stats_maybe(void)
{
    const uint64_t now = now_ns();
    uint64_t last = __atomic_load_n(&s_stat_last_ns, __ATOMIC_RELAXED);
    if (!last) { __atomic_store_n(&s_stat_last_ns, now, __ATOMIC_RELAXED); return; }
    if (now - last < 2000000000ull) return;
    if (!__atomic_exchange_n(&s_stat_last_ns, now, __ATOMIC_RELAXED)) return;
    fprintf(stderr,
            "[c690-gpu2a] enter=%lu class=%lu fallback=%lu nosink=%lu put=%lu "
            "unmatched=%lu records=%lu fallback_records=%lu\n",
            __atomic_load_n(&s_stat_enter, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_class, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_fallback, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_nosink, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_put, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_unmatched, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_records, __ATOMIC_RELAXED),
            __atomic_load_n(&s_stat_fallback_records, __ATOMIC_RELAXED));
}
