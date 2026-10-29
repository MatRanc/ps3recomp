/*
 * Is a content-validated vertex cache worth building? The per-draw vertex
 * work it would skip is rsx_vertex_fetch_many (big-endian guest bytes ->
 * host float4s); what it must do instead, every draw, is prove the guest
 * bytes that fetch would read are unchanged. This measures both, on the real
 * rsx_vertex_fetch_plan_prepare / rsx_vertex_fetch_many, for draws shaped like
 * a retail frame's: interleaved strides 16-48 (plus one split-stream layout),
 * 100-5000 unique references after the remap, float / half / ubyte formats.
 *
 *   fetch     rsx_vertex_fetch_many into a reused (hot) scratch: what a hit skips
 *   prepare   rsx_vertex_fetch_plan_prepare: paid by both paths (it finds the spans)
 *   xxh src   XXH64 of the merged source spans the fetch reads
 *   cmp src   memcmp of those spans against a stored copy
 *   refs      XXH64 / memcmp of the remapped references (8 bytes each)
 *   out       memcpy of the converted vertices: a hit hands the cached copy
 *             to the backend, which copies it into its vertex ring
 *
 * "hot" repeats one draw (everything in L1/L2); "cold" scrubs the caches and
 * walks 16-256 distinct copies of the draw, so the guest bytes and the stored
 * copy come from DRAM, as most of a frame's do. "xxh x" / "cmp x" are fetch
 * over the validation that would replace it.
 *
 * The frame section runs 750 such draws back to back (log-uniform 100-5000
 * references, a random layout each) the way the engine does -- references
 * written, prepare, vertices produced, the backend's copy into a 64 MB ring --
 * uncached, and as a best-case cache hit (one candidate, found for free,
 * matching) validated three ways: XXH64, memcmp against a stored copy, and a
 * bare read of the same bytes, which no validation of them can beat. A hit's
 * vertices come from a stored copy that is cold by the time the draw comes
 * round again; the uncached path's come from a reused, hot scratch buffer.
 *
 * The direct section runs the same frame the way the engine can hand it to a
 * backend: converted into scratch and copied into the ring (the copy), or
 * converted straight into the ring (draw_fill), each with the converter as it
 * was before the rewrite (rsx_vertex_fetch_legacy.h) and as it is now. The
 * rows run interleaved, best of many, so a noisy host moves them together.
 *
 * Build and run (from the repo root): tests/bench/vertex_cache/run.sh
 * (--frame skips the per-draw table, --direct runs only the direct section).
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include "rsx_vertex_fetch_legacy.h"   /* libs/video/tests: the converter before the rewrite */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__SSE2__)
#include <immintrin.h>
#endif

typedef struct src_span {
    const u8* p;
    u32 bytes;
} src_span;

/* What a cache would hash per draw: XXH64 (the reference algorithm). A faster
 * hash only moves the "xxh" columns toward the "floor" row of the frame table.
 * ---- XXH64 --------------------------------------------------------------- */

#define XP1 0x9E3779B185EBCA87ull
#define XP2 0xC2B2AE3D27D4EB4Full
#define XP3 0x165667B19E3779F9ull
#define XP4 0x85EBCA77C2B2AE63ull
#define XP5 0x27D4EB2F165667C5ull

static inline u64 xxh_rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static inline u64 xxh_rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u32 xxh_rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 xxh_round(u64 acc, u64 in)
{
    acc += in * XP2;
    acc = xxh_rotl(acc, 31);
    return acc * XP1;
}
static inline u64 xxh_merge(u64 acc, u64 v)
{
    acc ^= xxh_round(0, v);
    return acc * XP1 + XP4;
}

static u64 xxh64(const void* data, u32 bytes, u64 seed)
{
    const u8* p = (const u8*)data;
    const u8* const end = p + bytes;
    u64 h;
    if (bytes >= 32) {
        u64 v1 = seed + XP1 + XP2, v2 = seed + XP2, v3 = seed, v4 = seed - XP1;
        const u8* const limit = end - 32;
        do {
            v1 = xxh_round(v1, xxh_rd64(p));
            v2 = xxh_round(v2, xxh_rd64(p + 8));
            v3 = xxh_round(v3, xxh_rd64(p + 16));
            v4 = xxh_round(v4, xxh_rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) + xxh_rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + XP5;
    }
    h += bytes;
    for (; p + 8 <= end; p += 8) {
        h ^= xxh_round(0, xxh_rd64(p));
        h = xxh_rotl(h, 27) * XP1 + XP4;
    }
    if (p + 4 <= end) {
        h ^= (u64)xxh_rd32(p) * XP1;
        h = xxh_rotl(h, 23) * XP2 + XP3;
        p += 4;
    }
    for (; p < end; p++) {
        h ^= (*p) * XP5;
        h = xxh_rotl(h, 11) * XP1;
    }
    h ^= h >> 33;
    h *= XP2;
    h ^= h >> 29;
    h *= XP3;
    h ^= h >> 32;
    return h;
}

/* The guest (or inline) bytes a prepared plan's fetch reads, as disjoint
 * ascending spans: each used attribute's stable span, overlapping ones
 * (interleaved arrays) merged. -1 when an enabled attribute has no stable
 * span, so its fetch would resolve element by element.
 * ---- spans --------------------------------------------------------------- */

static int source_spans(const rsx_vertex_fetch_plan* plan,
                        src_span spans[RSX_DSP_NUM_VERTEX_ATTR])
{
    const u8* lo[RSX_DSP_NUM_VERTEX_ATTR];
    const u8* hi[RSX_DSP_NUM_VERTEX_ATTR];
    u32 n = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
        if (!f->desc.type || !f->desc.size)
            continue;                       /* the default: in the key */
        if (!f->stable_base || !f->elem_size)
            return -1;
        const u8* a = f->stable_base;
        const u8* b = a + (u64)(f->stable_last - f->stable_first) * f->stride + f->elem_size;
        /* Insert in start order; interleaved arrays overlap and merge below. */
        u32 i = n++;
        while (i && lo[i - 1] > a) { lo[i] = lo[i - 1]; hi[i] = hi[i - 1]; i--; }
        lo[i] = a;
        hi[i] = b;
    }
    u32 out = 0;
    for (u32 i = 0; i < n; i++) {
        if (out && lo[i] <= spans[out - 1].p + spans[out - 1].bytes) {
            const u8* e = spans[out - 1].p + spans[out - 1].bytes;
            if (hi[i] > e)
                spans[out - 1].bytes = (u32)(hi[i] - spans[out - 1].p);
            continue;
        }
        spans[out].p = lo[i];
        spans[out].bytes = (u32)(hi[i] - lo[i]);
        out++;
    }
    return (int)out;
}


static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static u64 s_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (u32)(s_rng >> 16);
}

/* ---- synthetic guest memory ---------------------------------------------- */

#define GUEST_BYTES (240u << 20)   /* under the 28-bit vertex offset mask */
static u8* s_guest;

static const u8* bench_guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user; (void)location;
    if ((u64)offset + min_bytes > GUEST_BYTES) return NULL;
    return s_guest + offset;
}

static volatile u64 s_sink;
static u8* s_scrub;
#define SCRUB_BYTES (96u << 20)    /* about 3x this host's L3 */
static void scrub_caches(void)
{
    u64 acc = 0;
    for (u32 i = 0; i < SCRUB_BYTES; i += 64) { s_scrub[i]++; acc += s_scrub[i]; }
    s_sink += acc;
}

/* ---- draw shapes ---------------------------------------------------------- */

typedef struct { u32 attr, type, size, off, stride; } bench_attr;
typedef struct {
    const char* name;
    u32 n_attr;
    bench_attr a[8];
    u32 bytes_per_vertex;   /* guest bytes per vertex over all streams */
} bench_layout;

static const bench_layout s_layouts[] = {
    { "s16 f3+ub4", 2, {
        { 0, RSX_VTX_TYPE_FLOAT, 3, 0, 16 }, { 3, RSX_VTX_TYPE_UNORM8, 4, 12, 16 } }, 16 },
    { "s24 f3+h2+ub4+ub4", 4, {
        { 0, RSX_VTX_TYPE_FLOAT, 3, 0, 24 }, { 8, RSX_VTX_TYPE_HALF, 2, 12, 24 },
        { 2, RSX_VTX_TYPE_UNORM8, 4, 16, 24 }, { 3, RSX_VTX_TYPE_UNORM8, 4, 20, 24 } }, 24 },
    { "s32 f3+f3+f2", 3, {
        { 0, RSX_VTX_TYPE_FLOAT, 3, 0, 32 }, { 2, RSX_VTX_TYPE_FLOAT, 3, 12, 32 },
        { 8, RSX_VTX_TYPE_FLOAT, 2, 24, 32 } }, 32 },
    { "s48 7 attrs mixed", 7, {
        { 0, RSX_VTX_TYPE_FLOAT, 3, 0, 48 }, { 2, RSX_VTX_TYPE_HALF, 4, 12, 48 },
        { 14, RSX_VTX_TYPE_HALF, 4, 20, 48 }, { 8, RSX_VTX_TYPE_FLOAT, 2, 28, 48 },
        { 9, RSX_VTX_TYPE_HALF, 2, 36, 48 }, { 3, RSX_VTX_TYPE_UNORM8, 4, 40, 48 },
        { 1, RSX_VTX_TYPE_UNORM8, 4, 44, 48 } }, 48 },
    /* Separate streams: offsets are multiples of the stream size, set per draw. */
    { "split f3|ub4|h2", 3, {
        { 0, RSX_VTX_TYPE_FLOAT, 3, 0, 12 }, { 2, RSX_VTX_TYPE_UNORM8, 4, 1, 4 },
        { 8, RSX_VTX_TYPE_HALF, 2, 2, 4 } }, 20 },
};
#define N_LAYOUTS (sizeof s_layouts / sizeof s_layouts[0])
static const u32 s_counts[] = { 100, 500, 1000, 2500, 5000 };
#define N_COUNTS (sizeof s_counts / sizeof s_counts[0])

static void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* Plausible component values: floats in +-100, halves always normal (no
 * denormal/NaN fallback), random bytes. */
static void fill_vertices(const bench_layout* L, u32 guest_off, u32 verts)
{
    for (u32 k = 0; k < L->n_attr; k++) {
        const bench_attr* a = &L->a[k];
        const u32 base = guest_off + (L->a[k].stride == L->bytes_per_vertex ? a->off
                                       : a->off * verts * 16u);
        for (u32 v = 0; v < verts; v++) {
            u8* p = s_guest + base + v * a->stride;
            for (u32 c = 0; c < a->size; c++) {
                if (a->type == RSX_VTX_TYPE_FLOAT) {
                    float f = (float)(int)(rnd() % 20000u - 10000) * 0.01f;
                    u32 w; memcpy(&w, &f, 4); put_be32(p + c * 4, w);
                } else if (a->type == RSX_VTX_TYPE_HALF) {
                    const u16 h = (u16)(((rnd() & 1u) << 15) | ((1u + rnd() % 29u) << 10) | (rnd() & 0x3FFu));
                    p[c * 2] = (u8)(h >> 8); p[c * 2 + 1] = (u8)h;
                } else {
                    p[c] = (u8)rnd();
                }
            }
        }
    }
}

/* A 32-wide grid mesh's triangle list, remapped exactly as dc_fetch remaps
 * it: `want` unique references in first-use order. */
static u32 make_refs(rsx_vertex_ref* refs, rsx_vertex_remap* remap, u32 want)
{
    const u32 W = 32, H = (want + W - 1) / W;
    u32 n = 0;
    for (u32 y = 0; y + 1 < H; y++)
        for (u32 x = 0; x + 1 < W; x++) {
            const u32 i0 = y * W + x, i1 = i0 + 1, i2 = i0 + W, i3 = i2 + 1;
            const u32 tri[6] = { i0, i2, i1, i1, i2, i3 };
            for (u32 t = 0; t < 6; t++) { refs[n].vertex_id = tri[t]; refs[n].base_index = 0; n++; }
        }
    u32 unique = n;
    rsx_vertex_remap_build(remap, refs, n, &unique);
    return unique;
}

static void plan_draw(rsx_vertex_fetch_plan* plan, const bench_layout* L, u32 guest_off, u32 verts)
{
    memset(plan, 0, sizeof *plan);
    u32 mask = 0;
    for (u32 k = 0; k < L->n_attr; k++) mask |= 1u << L->a[k].attr;
    rsx_vertex_layout_plan_init(&plan->layout, mask);
    plan->guest_ptr = bench_guest_ptr;
    for (u32 attr = 0; attr < 16; attr++) {
        plan->attr[attr].default_value[3] = 1.0f;
    }
    for (u32 k = 0; k < L->n_attr; k++) {
        const bench_attr* a = &L->a[k];
        rsx_vertex_fetch_attr* f = &plan->attr[a->attr];
        f->desc.type = a->type;
        f->desc.size = a->size;
        f->desc.stride = a->stride;
        f->desc.location = RSX_LOCATION_LOCAL;
        f->desc.offset = guest_off + (a->stride == L->bytes_per_vertex ? a->off : a->off * verts * 16u);
        f->elem_size = rsx_vertex_attrib_size(a->type, a->size);
        f->stride = a->stride;
    }
}

static u32 guest_span(const bench_layout* L, u32 verts)
{
    return L->n_attr && L->a[0].stride == L->bytes_per_vertex
        ? verts * L->bytes_per_vertex : L->n_attr * verts * 16u;
}

/* ---- primitive timings --------------------------------------------------- */

typedef struct {
    rsx_vertex_fetch_plan plan;
    u8* copy;          /* stored source spans (cmp variant) */
    u8* out;           /* the cache entry's converted vertices */
    rsx_vertex_ref* refs_copy;
} bench_inst;

enum { OP_PREPARE, OP_FETCH, OP_XXH_SRC, OP_CMP_SRC, OP_XXH_REFS, OP_CMP_REFS, OP_OUT, N_OPS };
static const char* s_op_names[N_OPS] = { "prepare", "fetch", "xxh src", "cmp src", "xxh refs", "cmp refs", "out" };

static u8* s_scratch;   /* dc.verts: reused every draw, so hot */
static rsx_vertex_ref* s_refs;

static void run_op(int op, bench_inst* in, u32 n)
{
    const rsx_vertex_fetch_plan* plan = &in->plan;
    switch (op) {
    case OP_PREPARE: rsx_vertex_fetch_plan_prepare(&in->plan, s_refs, n); break;
    case OP_FETCH: rsx_vertex_fetch_many(plan, s_refs, n, s_scratch); break;
    case OP_XXH_SRC: {
        src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
        const u32 ns = source_spans(plan, sp);
        u64 h = 0;
        for (u32 i = 0; i < ns; i++) h = xxh64(sp[i].p, sp[i].bytes, h);
        s_sink += h;
        break; }
    case OP_CMP_SRC: {
        src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
        const u32 ns = source_spans(plan, sp);
        const u8* c = in->copy;
        int d = 0;
        for (u32 i = 0; i < ns; i++) { d |= memcmp(sp[i].p, c, sp[i].bytes); c += sp[i].bytes; }
        s_sink += (u64)d;
        break; }
    case OP_XXH_REFS: s_sink += xxh64(s_refs, n * 8u, 0); break;
    case OP_CMP_REFS: s_sink += (u64)memcmp(s_refs, in->refs_copy, n * 8u); break;
    case OP_OUT: memcpy(s_scratch, in->out, (size_t)n * plan->layout.stride); break;
    }
}

/* ns per call: hot = one instance repeated, cold = K instances after a scrub. */
static double time_op(int op, bench_inst* inst, u32 k_inst, u32 n, int cold)
{
    double best = 1e30;
    for (int trial = 0; trial < (cold ? 5 : 9); trial++) {
        if (cold) scrub_caches();
        u32 reps = cold ? k_inst : 0;
        double t0 = now_us(), t1;
        if (cold) {
            for (u32 i = 0; i < k_inst; i++) run_op(op, &inst[i], n);
            t1 = now_us();
        } else {
            run_op(op, &inst[0], n);   /* warm */
            t0 = now_us();
            do { run_op(op, &inst[0], n); reps++; t1 = now_us(); } while (t1 - t0 < 2000.0);
        }
        const double ns = (t1 - t0) * 1e3 / reps;
        if (ns < best) best = ns;
    }
    return best;
}

static void bench_primitives(void)
{
    printf("Per draw, ns (min of trials). ratio = fetch / (xxh src + xxh refs); cmp ratio likewise.\n");
    printf("%-20s %5s %4s | %9s %9s | %9s %9s %9s %9s %9s | %6s %6s\n",
           "layout", "refs", "mode", "prepare", "fetch", "xxh src", "cmp src", "xxh refs",
           "cmp refs", "out", "xxh x", "cmp x");
    for (u32 li = 0; li < N_LAYOUTS; li++) {
        const bench_layout* L = &s_layouts[li];
        for (u32 ci = 0; ci < N_COUNTS; ci++) {
            rsx_vertex_remap remap; memset(&remap, 0, sizeof remap);
            const u32 n = make_refs(s_refs, &remap, s_counts[ci]);
            u32 verts = 0;
            for (u32 i = 0; i < n; i++) if (s_refs[i].vertex_id + 1 > verts) verts = s_refs[i].vertex_id + 1;
            const u32 span = (guest_span(L, verts) + 4095u) & ~4095u;
            u32 k = (u32)((64u << 20) / (span * 3u + 1u));
            if (k > 256) k = 256;
            if (k < 16) k = 16;
            bench_inst* inst = (bench_inst*)calloc(k, sizeof *inst);
            for (u32 i = 0; i < k; i++) {
                const u32 off = i * span;
                fill_vertices(L, off, verts);
                plan_draw(&inst[i].plan, L, off, verts);
                rsx_vertex_fetch_plan_prepare(&inst[i].plan, s_refs, n);
                const u32 out_bytes = n * inst[i].plan.layout.stride;
                inst[i].out = (u8*)malloc(out_bytes);
                rsx_vertex_fetch_many(&inst[i].plan, s_refs, n, inst[i].out);
                src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
                const u32 ns = source_spans(&inst[i].plan, sp);
                u32 tot = 0;
                for (u32 s = 0; s < ns; s++) tot += sp[s].bytes;
                inst[i].copy = (u8*)malloc(tot);
                for (u32 s = 0, at = 0; s < ns; s++) { memcpy(inst[i].copy + at, sp[s].p, sp[s].bytes); at += sp[s].bytes; }
                inst[i].refs_copy = (rsx_vertex_ref*)malloc(n * 8u);
                memcpy(inst[i].refs_copy, s_refs, n * 8u);
            }
            for (int cold = 0; cold < 2; cold++) {
                double t[N_OPS];
                for (int op = 0; op < N_OPS; op++) t[op] = time_op(op, inst, k, n, cold);
                printf("%-20s %5u %4s | %9.0f %9.0f | %9.0f %9.0f %9.0f %9.0f %9.0f | %6.2f %6.2f\n",
                       L->name, n, cold ? "cold" : "hot", t[OP_PREPARE], t[OP_FETCH],
                       t[OP_XXH_SRC], t[OP_CMP_SRC], t[OP_XXH_REFS], t[OP_CMP_REFS], t[OP_OUT],
                       t[OP_FETCH] / (t[OP_XXH_SRC] + t[OP_XXH_REFS]),
                       t[OP_FETCH] / (t[OP_CMP_SRC] + t[OP_CMP_REFS]));
            }
            for (u32 i = 0; i < k; i++) { free(inst[i].copy); free(inst[i].out); free(inst[i].refs_copy); }
            free(inst);
            rsx_vertex_remap_destroy(&remap);
        }
    }
    (void)s_op_names;
}

/* ---- a frame ------------------------------------------------------------- */

#define FRAME_DRAWS 750u
typedef struct {
    const bench_layout* L;
    u32 n, verts, guest_off;
    rsx_vertex_ref* refs;
    rsx_vertex_fetch_plan plan;
    u8* out;            /* a hit's stored vertices */
    u8* copy;           /* the cmp variant's stored source bytes */
    rsx_vertex_ref* refs_copy;
    u64 h_refs, h_src;
} frame_draw;

static frame_draw s_fd[FRAME_DRAWS];
static u8* s_ring;
#define RING_BYTES (64u << 20)

enum { F_UNCACHED, F_XXH, F_CMP, F_FLOOR, N_FMODES };
static const char* s_fmode[N_FMODES] = {
    "uncached (prepare + fetch)",
    "hit, XXH64 refs + source",
    "hit, memcmp refs + source vs copy",
    "hit, floor: read refs + source once",
};

/* Reads every byte once and does nothing else: no validation of `bytes` can
 * be cheaper than this. */
static u64 read_floor(const u8* p, u32 bytes)
{
    u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0, v;
    u32 i = 0;
    for (; i + 32 <= bytes; i += 32) {
        memcpy(&v, p + i, 8); a0 |= v; memcpy(&v, p + i + 8, 8); a1 |= v;
        memcpy(&v, p + i + 16, 8); a2 |= v; memcpy(&v, p + i + 24, 8); a3 |= v;
    }
    for (; i < bytes; i++) a0 |= p[i];
    return a0 | a1 | a2 | a3;
}

/* One frame of the engine's vertex path: per draw the index walk writes the
 * references into dc.refs (modelled as a copy), prepare runs, the vertices are
 * converted -- or a hit is validated -- and the backend copies them into its
 * ring. A hit is the best case: one candidate, found for free, and it matches.
 * Returns {us total, us without the ring copy}. */
static void run_frame(int mode, double* total, double* vertex_only)
{
    u64 ring_at = 0;
    double ring = 0;
    const double t0 = now_us();
    for (u32 d = 0; d < FRAME_DRAWS; d++) {
        frame_draw* fd = &s_fd[d];
        memcpy(s_refs, fd->refs, fd->n * 8u);
        rsx_vertex_fetch_plan_prepare(&fd->plan, s_refs, fd->n);
        const u32 out_bytes = fd->n * fd->plan.layout.stride;
        const u8* verts = fd->out;
        src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
        int ns = 0;
        if (mode != F_UNCACHED) ns = source_spans(&fd->plan, sp);
        if (mode == F_UNCACHED) {
            rsx_vertex_fetch_many(&fd->plan, s_refs, fd->n, s_scratch);
            verts = s_scratch;
        } else if (mode == F_XXH) {
            u64 h = 0;
            for (int i = 0; i < ns; i++) h = xxh64(sp[i].p, sp[i].bytes, h);
            if (xxh64(s_refs, fd->n * 8u, 0) != fd->h_refs || h != fd->h_src) abort();
        } else if (mode == F_CMP) {
            const u8* c = fd->copy;
            if (memcmp(s_refs, fd->refs_copy, fd->n * 8u)) abort();
            for (int i = 0; i < ns; i++) { if (memcmp(sp[i].p, c, sp[i].bytes)) abort(); c += sp[i].bytes; }
        } else {
            u64 acc = read_floor((const u8*)s_refs, fd->n * 8u);
            for (int i = 0; i < ns; i++) acc |= read_floor(sp[i].p, sp[i].bytes);
            s_sink += acc;
        }
        const double r0 = now_us();
        if (ring_at + out_bytes > RING_BYTES) ring_at = 0;
        memcpy(s_ring + ring_at, verts, out_bytes);
        ring_at += (out_bytes + 15u) & ~15u;
        ring += now_us() - r0;
    }
    *total = now_us() - t0;
    *vertex_only = *total - ring;
}

/* ---- direct conversion into the ring -------------------------------------- */

enum { D_COPY_LEGACY, D_DIRECT_LEGACY, D_COPY, D_DIRECT_PLAIN, D_DIRECT, D_FLOOR, D_FLOOR_READ, N_DMODES };
static const char* s_dmode[N_DMODES] = {
    "copy:   legacy fetch -> scratch -> ring",
    "direct: legacy fetch -> ring",
    "copy:   fetch -> scratch -> ring",
    "direct: fetch -> ring",
    "direct: fetch_stream -> ring (draw_fill)",
    "floor:  prepare + stream the ring bytes",
    "floor:  ... + read the source spans",
};

/* The frame of bench_frame (already set up in s_fd), with the vertices either
 * converted into the hot scratch and copied into the ring, or converted
 * straight into the ring. Returns us. The caches are scrubbed first: a
 * frame's guest arrays are read once a frame, and the rest of the frame (and
 * of the title) runs in between; on a host whose last-level cache holds the
 * 25 MB of them, a frame streamed past the cache would otherwise find them
 * all still there. */
static double run_direct(int mode)
{
    scrub_caches();
    u64 ring_at = 0;
    const int legacy = mode == D_COPY_LEGACY || mode == D_DIRECT_LEGACY;
    const int direct = mode != D_COPY_LEGACY && mode != D_COPY;
    const double t0 = now_us();
    for (u32 d = 0; d < FRAME_DRAWS; d++) {
        frame_draw* fd = &s_fd[d];
        memcpy(s_refs, fd->refs, fd->n * 8u);
        rsx_vertex_fetch_plan_prepare(&fd->plan, s_refs, fd->n);
        const u32 out_bytes = fd->n * fd->plan.layout.stride;
        if (ring_at + out_bytes > RING_BYTES) ring_at = 0;
        u8* dst = direct ? s_ring + ring_at : s_scratch;
        if (mode == D_FLOOR || mode == D_FLOOR_READ) {
            /* What no conversion into the ring can beat: its bytes written,
             * and the guest bytes it converts read. */
            if (mode == D_FLOOR_READ) {
                src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
                const int ns = source_spans(&fd->plan, sp);
                u64 acc = 0;
                for (int i = 0; i < ns; i++) acc |= read_floor(sp[i].p, sp[i].bytes);
                s_sink += acc;
            }
#if defined(__SSE2__)
            const __m128i z = _mm_set1_epi32((int)d);
            for (u32 q = 0; q < out_bytes / 16u; q++) _mm_stream_si128((__m128i*)dst + q, z);
#else
            memset(dst, (int)d, out_bytes);
#endif
            ring_at += (out_bytes + 15u) & ~15u;
            continue;
        }
        const int ok = legacy ? rsx_vertex_fetch_many_legacy(&fd->plan, s_refs, fd->n, dst)
                     : mode == D_DIRECT ? rsx_vertex_fetch_many_stream(&fd->plan, s_refs, fd->n, dst)
                     : rsx_vertex_fetch_many(&fd->plan, s_refs, fd->n, dst);
        if (!ok) abort();
        if (!direct) memcpy(s_ring + ring_at, s_scratch, out_bytes);
        ring_at += (out_bytes + 15u) & ~15u;
    }
#if defined(__SSE2__)
    _mm_sfence();
#endif
    return now_us() - t0;
}

static void bench_direct(void)
{
    /* VTX_BENCH_PROFILE=<row>: that row alone, 200 times, for a profiler. */
    const char* prof = getenv("VTX_BENCH_PROFILE");
    if (prof) {
        const int m = atoi(prof);
        if (m < 0 || m >= N_DMODES) return;
        double t = 0;
        for (int i = 0; i < 200; i++) t += run_direct(m);
        printf("\n%s: %.2f ms a frame\n", s_dmode[m], t / 200 / 1e3);
        return;
    }
    double best[N_DMODES];
    for (int m = 0; m < N_DMODES; m++) best[m] = 1e30;
    for (int trial = 0; trial < 40; trial++)
        for (int m = 0; m < N_DMODES; m++) {
            const double t = run_direct(m);
            if (t < best[m]) best[m] = t;
        }
    /* Every conversion wrote the same bytes into the ring. */
    static u8* want;
    if (!want) want = (u8*)malloc(RING_BYTES);
    for (int m = D_COPY_LEGACY; m <= D_DIRECT; m++) {
        memset(s_ring, 0, RING_BYTES);
        run_direct(m);
        if (m == D_COPY_LEGACY) memcpy(want, s_ring, RING_BYTES);
        else if (memcmp(want, s_ring, RING_BYTES)) { printf("ring differs in mode %d\n", m); abort(); }
    }
    printf("\nDirect: the frame above from cold caches, ms (best of 40, interleaved)\n");
    for (int m = 0; m < N_DMODES; m++)
        printf("  %-40s %8.2f ms   %.2fx\n", s_dmode[m], best[m] / 1e3, best[D_COPY_LEGACY] / best[m]);
}

static int s_direct_only;
static void bench_frame(void)
{
    /* Log-uniform reference counts in 100..5000, a random layout per draw,
     * each draw its own guest arrays. */
    u64 src_total = 0, out_total = 0, refs_total = 0;
    u32 guest_at = 0;
    rsx_vertex_remap remap; memset(&remap, 0, sizeof remap);
    for (u32 d = 0; d < FRAME_DRAWS; d++) {
        frame_draw* fd = &s_fd[d];
        fd->L = &s_layouts[rnd() % N_LAYOUTS];
        const double u = (double)(rnd() % 10000u) / 10000.0;
        u32 want = (u32)(100.0 * __builtin_exp(u * __builtin_log(50.0)));
        fd->refs = (rsx_vertex_ref*)malloc((size_t)(want + 64) * 6u * 8u);
        fd->n = make_refs(fd->refs, &remap, want);
        fd->verts = 0;
        for (u32 i = 0; i < fd->n; i++)
            if (fd->refs[i].vertex_id + 1 > fd->verts) fd->verts = fd->refs[i].vertex_id + 1;
        const u32 span = (guest_span(fd->L, fd->verts) + 255u) & ~255u;
        fd->guest_off = guest_at;
        guest_at += span;
        fill_vertices(fd->L, fd->guest_off, fd->verts);
        plan_draw(&fd->plan, fd->L, fd->guest_off, fd->verts);
        rsx_vertex_fetch_plan_prepare(&fd->plan, fd->refs, fd->n);
        fd->out = (u8*)malloc((size_t)fd->n * fd->plan.layout.stride);
        rsx_vertex_fetch_many(&fd->plan, fd->refs, fd->n, fd->out);
        src_span sp[RSX_DSP_NUM_VERTEX_ATTR];
        const int ns = source_spans(&fd->plan, sp);
        u32 tot = 0;
        fd->h_src = 0;
        for (int i = 0; i < ns; i++) { tot += sp[i].bytes; fd->h_src = xxh64(sp[i].p, sp[i].bytes, fd->h_src); }
        fd->copy = (u8*)malloc(tot);
        for (int i = 0, at = 0; i < ns; i++) { memcpy(fd->copy + at, sp[i].p, sp[i].bytes); at += (int)sp[i].bytes; }
        fd->refs_copy = (rsx_vertex_ref*)malloc((size_t)fd->n * 8u);
        memcpy(fd->refs_copy, fd->refs, (size_t)fd->n * 8u);
        fd->h_refs = xxh64(fd->refs, fd->n * 8u, 0);
        src_total += tot;
        out_total += (u64)fd->n * fd->plan.layout.stride;
        refs_total += fd->n;
    }
    rsx_vertex_remap_destroy(&remap);
    printf("\nFrame: %u draws, %llu unique refs, %.1f MB source spans, %.1f MB converted\n",
           FRAME_DRAWS, (unsigned long long)refs_total, src_total / 1048576.0, out_total / 1048576.0);
    if (s_direct_only) return;
    printf("  %-38s %10s %10s   %s\n", "", "frame us", "vertex us", "(vertex = without the backend's ring copy)");
    double base_t = 0, base_v = 0;
    for (int mode = 0; mode < N_FMODES; mode++) {
        double bt = 1e30, bv = 1e30;
        for (int i = 0; i < 15; i++) {
            double t, v;
            run_frame(mode, &t, &v);
            if (t < bt) bt = t;
            if (v < bv) bv = v;
        }
        if (mode == F_UNCACHED) { base_t = bt; base_v = bv; }
        printf("  %-38s %10.0f %10.0f   %.2fx / %.2fx\n", s_fmode[mode], bt, bv, base_t / bt, base_v / bv);
    }
}

int main(int argc, char** argv)
{
    s_guest = (u8*)calloc(1, GUEST_BYTES);
    s_scrub = (u8*)calloc(1, SCRUB_BYTES);
    s_scratch = (u8*)malloc(8u << 20);
    s_refs = (rsx_vertex_ref*)malloc((size_t)64 * 1024 * 8);
    s_ring = (u8*)malloc(RING_BYTES);
    if (!s_guest || !s_scrub || !s_scratch || !s_refs || !s_ring) return 1;
    memset(s_ring, 0, RING_BYTES);
    const int frame_only = argc > 1 && !strcmp(argv[1], "--frame");
    s_direct_only = argc > 1 && !strcmp(argv[1], "--direct");
    if (!frame_only && !s_direct_only) bench_primitives();
    bench_frame();
    bench_direct();
    return 0;
}
