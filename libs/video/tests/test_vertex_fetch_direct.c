/*
 * rsx_vertex_fetch_many and rsx_vertex_fetch_many_stream against the
 * converter they replaced (rsx_vertex_fetch_legacy.h, frozen at 255ad438) and
 * against rsx_vertex_fetch_one per vertex: every byte and the result must be
 * the same.
 *
 * Random plans -- every vertex type (and two no decoder knows), 0-7
 * components, interleaved arrays at random offsets inside their stride and
 * separate ones, frequency dividers in both modes, VERTEX_DATA_BASE_OFFSET,
 * guest memory that ends partway through an array or has a hole in it,
 * INLINE_ARRAY streams -- over random reference sets: ascending runs, random
 * indices with repeats, remapped meshes, base indices, runs across the 20-bit
 * element wrap, 0 to 5000 references so blocks end everywhere, and the prefix
 * of the prepared references the engine hands draw_fill. Guest bytes are
 * random, so every half (denormals, infinities, NaN payloads), every float
 * bit pattern (signalling NaNs included) and every byte and 16-bit value
 * turns up. The streaming variant writes 16-byte-aligned and unaligned
 * destinations; outputs start out poisoned with different bytes, so a byte
 * one path leaves unwritten shows. Guest memory ends at an inaccessible page
 * and some arrays are moved to end exactly there, so a load past the span
 * the plan mapped (a 16-byte load of the last float3) crashes the test.
 *
 * Build (from the repo root), with the runtime's flags and with -march=native:
 *   clang -std=gnu17 -O2 -ffp-contract=off -msse4.1 -I include -I libs/video \
 *     -I libs/video/tests -o /tmp/test_vertex_fetch_direct \
 *     libs/video/tests/test_vertex_fetch_direct.c libs/video/rsx_vertex_compact.c \
 *     libs/video/rsx_dispatch.c
 * (tools/test_vertex_decoders.py builds it every SIMD way.)
 */
#include "rsx_vertex_fetch_legacy.h"

#include <stdint.h>
#include <stdio.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x452821E638D01377ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 20 };
static uint8_t* guest[2];               /* two locations */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */
static uint32_t hole_lo[2], hole_hi[2]; /* and so is [lo, hi) */

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    const u64 end = (u64)offset + min_bytes;
    if (end > mapped_end[loc]) return NULL;
    if (offset < hole_hi[loc] && end > hole_lo[loc]) return NULL;
    return guest[loc] + offset;
}

static u32 pick_type(void)
{
    const u32 r = rnd() % 32;
    if (r < 8) return RSX_VTX_TYPE_FLOAT;
    if (r < 13) return RSX_VTX_TYPE_HALF;
    if (r < 18) return RSX_VTX_TYPE_UNORM8;
    if (r < 20) return 0;                      /* disabled */
    if (r < 21) return 8 + rnd() % 8;          /* no decoder */
    return 1 + rnd() % 7;
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline, const u8* inl,
                        u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    /* Mostly the few attributes a real program reads, sometimes all of them. */
    u32 mask = 0;
    if (rnd() % 8 == 0) mask = rnd() & 0xFFFFu;
    else for (u32 k = 1 + rnd() % 7; k; k--) mask |= 1u << (rnd() % 16);
    if (rnd() % 4) mask |= 1u;
    rsx_vertex_layout_plan_init(&plan->layout, mask);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 4096u : 0;
    if (rnd() % 8 == 0) plan->base_offset += (1u + rnd() % 15u) << 28;
    plan->divider_mask = (rnd() % 4 == 0) ? rnd() & 0xFFFFu : 0;
    plan->guest_ptr = guest_ptr;
    /* An interleaved stream most of the time: attributes packed at offsets in
     * one stride; otherwise each its own array. */
    const int interleaved = rnd() % 3 != 0;
    const u32 istride = 4u * (1u + rnd() % 16u);
    const u32 ibase = rnd() % (GUEST / 2);
    u32 at = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        f->desc.type = pick_type();
        f->desc.size = (rnd() % 10 == 0) ? rnd() % 8 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        if (interleaved) {
            if (at + f->elem_size > istride) at = 0;
            f->desc.stride = istride;
            f->desc.offset = ibase + at;
            at += (f->elem_size + 1u) & ~1u;
        } else {
            f->desc.stride = (rnd() % 6 == 0) ? 0 : f->elem_size + (rnd() % 4) * (1u + rnd() % 4);
            f->desc.offset = rnd() % (GUEST - 1u);
        }
        f->desc.location = rnd() & 1u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 6 == 0) ? rnd() % 6 : 0;
        for (u32 c = 0; c < 4; c++) {
            const u32 bits = rnd();
            memcpy(&f->default_value[c], &bits, 4);
        }
    }
    if (use_inline) {
        u32 packed = 0;
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = packed ? packed + (rnd() % 3) * 4u : 16u;
        plan->inline_data = inl;
        plan->inline_bytes = inl_bytes;
    }
}

/* Reference sets the engine produces. */
static u32 random_refs(rsx_vertex_ref* refs, u32 max)
{
    const u32 shape = rnd() % 6;
    u32 count = (rnd() % 8 == 0) ? rnd() % (max + 1) : rnd() % 300u;
    if (count > max) count = max;
    const u32 base = (rnd() % 4 == 0) ? rnd() % 4096u : 0;
    const u32 start = (rnd() % 6 == 0) ? 0x000FFFFFu - rnd() % 400u
                    : (rnd() % 3 == 0) ? rnd() % 20000u : 0;
    const u32 range = 1u + rnd() % (count + 64u);
    for (u32 i = 0; i < count; i++) {
        u32 v;
        switch (shape) {
        case 0: v = start + i; break;                                  /* draw_arrays */
        case 1: v = start + rnd() % range; break;                      /* repeats */
        case 2: v = start + (i ^ (rnd() % 4)); break;                  /* nearly ascending */
        case 3: v = (rnd() % 16 == 0) ? rnd() : start + rnd() % range; break;
        case 4: v = start + (i / 3) * 2 + i % 3; break;                /* a strip */
        default: v = start + (count - 1 - i); break;                   /* descending */
        }
        refs[i].vertex_id = v;
        refs[i].base_index = (rnd() % 16 == 0) ? rnd() % 64u : base;
    }
    if (shape == 1 && count > 1 && rnd() % 2) {
        /* remapped as dc_fetch remaps a triangle list */
        rsx_vertex_remap remap;
        memset(&remap, 0, sizeof remap);
        u32 unique = count;
        if (rsx_vertex_remap_build(&remap, refs, count, &unique)) count = unique;
        rsx_vertex_remap_destroy(&remap);
    }
    return count;
}

/* GUEST bytes followed by a page nothing may touch. */
static uint8_t* guarded_alloc(void)
{
#if !defined(_WIN32)
    const size_t page = 1u << 16;   /* a multiple of every page size in use */
    uint8_t* p = (uint8_t*)mmap(NULL, GUEST + page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (mprotect(p + GUEST, page, PROT_NONE)) return NULL;
    return p;
#else
    return (uint8_t*)malloc(GUEST);
#endif
}

/* Move a plain array so the span the plan will map for these references
 * ends at the last guest byte, against the guard page. */
static void end_at_guard(rsx_vertex_fetch_plan* plan, u32 attr, const rsx_vertex_ref* refs, u32 count)
{
    rsx_vertex_fetch_attr* f = &plan->attr[attr];
    if (!count || plan->inline_data || !f->elem_size ||
        !rsx_vertex_freq_plain(f->desc.frequency, (plan->divider_mask >> attr) & 1u))
        return;
    u32 last = 0;
    for (u32 i = 0; i < count; i++) {
        const u32 e = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
        if (e > last) last = e;
    }
    const u64 tail = (u64)last * f->stride + f->elem_size;
    if (tail > GUEST) return;
    f->desc.offset = ((u32)(GUEST - tail) - plan->base_offset) & 0x0FFFFFFFu;
    mapped_end[f->desc.location & 1u] = GUEST;
    hole_lo[f->desc.location & 1u] = hole_hi[f->desc.location & 1u] = 0;
}

int main(void)
{
    enum { PLANS = 30000, MAXV = 5000 };
    guest[0] = guarded_alloc();
    guest[1] = guarded_alloc();
    static rsx_vertex_ref refs[MAXV];
    static u8 inl[16384];
    const size_t out_cap = (size_t)MAXV * 256u + 64u;
    u8* out_legacy = (u8*)malloc(out_cap);
    u8* out_many = (u8*)malloc(out_cap);
    u8* out_stream_raw = (u8*)malloc(out_cap + 64u);
    u8* out_one = (u8*)malloc(out_cap);
    if (!guest[0] || !guest[1] || !out_legacy || !out_many || !out_stream_raw || !out_one) return 1;
    for (u32 i = 0; i < GUEST; i++) { guest[0][i] = (u8)rnd(); guest[1][i] = (u8)rnd(); }
    for (u32 i = 0; i < sizeof inl; i++) inl[i] = (u8)rnd();

    unsigned long long vertices = 0, bytes = 0;
    int failed_draws = 0, streamed_aligned = 0, prefixes = 0, guarded = 0;
    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++) {
            mapped_end[l] = (rnd() % 4 == 0) ? rnd() % GUEST : GUEST;
            hole_lo[l] = hole_hi[l] = 0;
            if (rnd() % 8 == 0) { hole_lo[l] = rnd() % GUEST; hole_hi[l] = hole_lo[l] + rnd() % 4096u; }
        }
        rsx_vertex_fetch_plan plan;
        const u32 count = random_refs(refs, MAXV);
        const int use_inline = rnd() % 6 == 0;
        random_plan(&plan, use_inline, inl, (u32)(rnd() % sizeof inl));
        if (rnd() % 3 == 0) {
            end_at_guard(&plan, plan.layout.count ? plan.layout.attrs[rnd() % plan.layout.count] : 0,
                         refs, count);
            guarded++;
        }
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        /* The engine converts the first `uploaded` of the prepared references. */
        u32 fetch = count;
        if (count && rnd() % 5 == 0) { fetch = rnd() % (count + 1); prefixes++; }
        const u32 stride = plan.layout.stride;
        const size_t out_bytes = (size_t)fetch * stride;

        memset(out_legacy, 0xA5, out_bytes + 16);
        memset(out_many, 0x5A, out_bytes + 16);
        const u32 misalign = (rnd() % 4 == 0) ? 4u * (1u + rnd() % 3) : 0;
        u8* out_stream = (u8*)(((uintptr_t)out_stream_raw + 15u) & ~(uintptr_t)15u) + misalign;
        memset(out_stream, 0x3C, out_bytes + 16);
        streamed_aligned += !misalign;

        const int r_legacy = rsx_vertex_fetch_many_legacy(&plan, refs, fetch, out_legacy);
        const int r_many = rsx_vertex_fetch_many(&plan, refs, fetch, out_many);
        const int r_stream = rsx_vertex_fetch_many_stream(&plan, refs, fetch, out_stream);
        int r_one = 1;
        for (u32 i = 0; i < fetch && r_one; i++)
            r_one = rsx_vertex_fetch_one(&plan, &refs[i], out_one + (size_t)i * stride);

        CHECK(r_many == r_legacy && r_stream == r_legacy && r_one == r_legacy,
              "plan %d: results legacy %d many %d stream %d one %d", n, r_legacy, r_many,
              r_stream, r_one);
        /* A failed fetch drops the draw; what it left in out is nobody's. */
        if (!r_legacy) {
            CHECK(rsx_vertex_fetch_plan_can_fail(&plan),
                  "plan %d: the fetch failed where can_fail said it cannot", n);
            failed_draws++;
            continue;
        }
        if (memcmp(out_many, out_legacy, out_bytes) || memcmp(out_stream, out_legacy, out_bytes) ||
            memcmp(out_one, out_legacy, out_bytes)) {
            size_t at = 0;
            while (at < out_bytes && out_many[at] == out_legacy[at] && out_stream[at] == out_legacy[at] &&
                   out_one[at] == out_legacy[at]) at++;
            const u32 v = (u32)(at / (stride ? stride : 1)), slot = (u32)(at % (stride ? stride : 1)) / 16u;
            const rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[slot]];
            CHECK(0, "plan %d: vertex %u of %u slot %u (attr %u type %u size %u stride %u freq %u%s) differs: "
                  "legacy %02x many %02x stream %02x one %02x", n, v, fetch, slot,
                  plan.layout.attrs[slot], f->desc.type, f->desc.size, f->stride, f->desc.frequency,
                  f->stable_base ? "" : ", no stable span",
                  out_legacy[at], out_many[at], out_stream[at], out_one[at]);
        }
        /* Not a byte past the vertices. */
        CHECK(out_many[out_bytes] == 0x5A && out_stream[out_bytes] == 0x3C,
              "plan %d: wrote past %zu bytes", n, out_bytes);
        vertices += fetch;
        bytes += out_bytes;
    }
    printf("test_vertex_fetch_direct: %d plans, %llu vertices (%.1f MB) bit-identical to the legacy "
           "converter and to fetch_one, %d failed draws matched, %d prefixes, %d aligned streams, "
           "%d arrays against the guard page, %d failure(s)\n", PLANS, vertices, bytes / 1048576.0,
           failed_draws, prefixes, streamed_aligned, guarded, fails);
    free(out_legacy); free(out_many); free(out_stream_raw); free(out_one);
    return fails ? 1 : 0;
}
