/*
 * Microbenchmark for RSX vertex decode:
 * Realistic draws:
 * - 1,000-vertex draws
 * - float3 positions, stride 32
 * - half2 UVs
 * - unorm8x4 colors
 * - snorm16x4 normals
 *
 * Compares:
 * 1. Original scalar fetch_many
 * 2. New SIMD fetch_many (attribute-major)
 * 3. SIMD fetch_many (vertex-major / interleaved writes)
 *
 * Reports speedup per type and in total.
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include "rsx_dispatch.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#define NUM_VERTS 1000
#define NUM_ITERS 10000

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t rng = 0x123456789ABCDEF0ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

/* Original scalar reference implementations for benchmarking */
static float scalar_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float scalar_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0) out = sign;
    else if (exp == 31) out = sign | 0x7F800000u | (man << 13);
    else out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

#define SCALAR_FETCH_LOOP(decode)                                               \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) continue;                                   \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

static int rsx_vertex_fetch_many_scalar(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    const u32 stride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            SCALAR_FETCH_LOOP(v[c] = scalar_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
            SCALAR_FETCH_LOOP(v[c] = scalar_be_f16(src + c * 2))
            break;
        case RSX_VTX_TYPE_UNORM8:
            SCALAR_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_SNORM16:
            SCALAR_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        default:
            break;
        }
    }
    return 1;
}
#undef SCALAR_FETCH_LOOP

/* Vertex-major (interleaved) SIMD writer for the realistic 4-attribute draw */
#if defined(__AVX2__) && defined(__F16C__)
static void rsx_vertex_fetch_many_interleaved_realistic(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    (void)plan;
    const rsx_vertex_fetch_attr* f_pos = &plan->attr[0];
    const rsx_vertex_fetch_attr* f_uv  = &plan->attr[8];
    const rsx_vertex_fetch_attr* f_col = &plan->attr[3];
    const rsx_vertex_fetch_attr* f_nrm = &plan->attr[2];

    const u8* base_pos = f_pos->stable_base;
    const u8* base_uv  = f_uv->stable_base;
    const u8* base_col = f_col->stable_base;
    const u8* base_nrm = f_nrm->stable_base;

    const __m128i m_f32 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, -1,-1,-1,-1);
    const __m128i w1_i = _mm_set_epi32(0x3f800000, 0, 0, 0);
    const __m128i m_f16 = _mm_setr_epi8(1,0, 3,2, -1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    const __m128 div255 = _mm_set1_ps(255.0f);
    const __m128 div32767 = _mm_set1_ps(32767.0f);
    const __m128i m_s16 = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);

    for (u32 i = 0; i < count; i++) {
        const u32 element = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
        float* v = (float*)(out + (size_t)i * 64u);

        // 1. Position (float3, stride 32)
        const u8* src_pos = base_pos + (u64)element * 32u;
        u64 lo; u32 hi;
        memcpy(&lo, src_pos, 8); memcpy(&hi, src_pos + 8, 4);
        __m128i vpos = _mm_set_epi32(0, (s32)hi, (s32)(lo >> 32), (s32)lo);
        _mm_storeu_ps(v + 0, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(vpos, m_f32), w1_i)));

        // 2. UV (half2, stride 4)
        const u8* src_uv = base_uv + (u64)element * 4u;
        u32 raw_uv; memcpy(&raw_uv, src_uv, 4);
        __m128i huv = _mm_shuffle_epi8(_mm_cvtsi32_si128((s32)raw_uv), m_f16);
        _mm_storeu_ps(v + 4, _mm_blend_ps(_mm_cvtph_ps(huv), w1_f, 0xC));

        // 3. Color (unorm8x4, stride 4)
        const u8* src_col = base_col + (u64)element * 4u;
        u32 raw_col; memcpy(&raw_col, src_col, 4);
        __m128 fcol = _mm_div_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((s32)raw_col))), div255);
        _mm_storeu_ps(v + 8, fcol);

        // 4. Normal (snorm16x4, stride 8)
        const u8* src_nrm = base_nrm + (u64)element * 8u;
        u64 raw_nrm; memcpy(&raw_nrm, src_nrm, 8);
        __m128i snrm = _mm_shuffle_epi8(_mm_cvtsi64_si128((s64)raw_nrm), m_s16);
        __m128 fnrm = _mm_div_ps(_mm_cvtepi32_ps(_mm_cvtepi16_epi32(snrm)), div32767);
        _mm_storeu_ps(v + 12, fnrm);
    }
}
#endif

static void init_plan_attr(rsx_vertex_fetch_plan* plan, u32 attr, u32 type, u32 size, u32 stride, const u8* data) {
    rsx_vertex_fetch_attr* f = &plan->attr[attr];
    f->desc.type = type;
    f->desc.size = size;
    f->desc.stride = stride;
    f->desc.frequency = 0;
    f->elem_size = rsx_vertex_attrib_size(type, size);
    f->stride = stride ? stride : f->elem_size;
    f->stable_base = data;
    f->stable_first = 0;
    f->stable_last = NUM_VERTS - 1;
    for (int i = 0; i < 4; i++) f->default_value[i] = 0.0f;
    f->default_value[3] = 1.0f;
}

int main(void) {
    static u8 pos_data[NUM_VERTS * 32];
    for (u32 i = 0; i < NUM_VERTS; i++) {
        float x = (float)i * 0.1f, y = (float)i * 0.2f, z = (float)i * 0.3f;
        u32 ux, uy, uz;
        memcpy(&ux, &x, 4); memcpy(&uy, &y, 4); memcpy(&uz, &z, 4);
        u8* p = pos_data + i * 32;
        p[0] = ux >> 24; p[1] = ux >> 16; p[2] = ux >> 8; p[3] = ux;
        p[4] = uy >> 24; p[5] = uy >> 16; p[6] = uy >> 8; p[7] = uy;
        p[8] = uz >> 24; p[9] = uz >> 16; p[10] = uz >> 8; p[11] = uz;
    }

    static u8 uv_data[NUM_VERTS * 4];
    for (u32 i = 0; i < NUM_VERTS; i++) {
        u16 u = 0x3C00 + (i & 0x1FF);
        u16 v = 0x3800 + (i & 0x1FF);
        u8* p = uv_data + i * 4;
        p[0] = u >> 8; p[1] = u & 0xFF;
        p[2] = v >> 8; p[3] = v & 0xFF;
    }

    static u8 color_data[NUM_VERTS * 4];
    for (u32 i = 0; i < NUM_VERTS * 4; i++) color_data[i] = (u8)rnd();

    static u8 normal_data[NUM_VERTS * 8];
    for (u32 i = 0; i < NUM_VERTS; i++) {
        for (int c = 0; c < 4; c++) {
            s16 val = (s16)(rnd() % 65536);
            normal_data[i * 8 + c * 2] = (u8)(val >> 8);
            normal_data[i * 8 + c * 2 + 1] = (u8)(val & 0xFF);
        }
    }

    static rsx_vertex_ref refs[NUM_VERTS];
    for (u32 i = 0; i < NUM_VERTS; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
    }

    static u8 out_scalar[NUM_VERTS * 64];
    static u8 out_simd[NUM_VERTS * 64];

    printf("=================================================================================\n");
    printf("RSX Vertex Fetch Microbenchmark (%d draws x %d verts = %d verts total)\n",
           NUM_ITERS, NUM_VERTS, NUM_ITERS * NUM_VERTS);
    printf("=================================================================================\n");

    struct {
        const char* name;
        u32 attr_idx;
        u32 type;
        u32 size;
        u32 stride;
        const u8* data;
    } tests[] = {
        {"float3 pos (stride 32)",     0, RSX_VTX_TYPE_FLOAT,  3, 32, pos_data},
        {"half2 UV (stride 4)",         8, RSX_VTX_TYPE_HALF,   2, 4,  uv_data},
        {"unorm8x4 color (stride 4)",   3, RSX_VTX_TYPE_UNORM8, 4, 4,  color_data},
        {"snorm16x4 normal (stride 8)", 2, RSX_VTX_TYPE_SNORM16,4, 8,  normal_data},
    };

    printf("%-27s | %-15s | %-15s | %s\n", "Attribute Type", "Scalar (Old)", "SIMD (New)", "Speedup");
    printf("----------------------------+-----------------+-----------------+---------\n");

    for (int t = 0; t < 4; t++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << tests[t].attr_idx);
        init_plan_attr(&plan, tests[t].attr_idx, tests[t].type, tests[t].size, tests[t].stride, tests[t].data);

        // Warmup & check match
        rsx_vertex_fetch_many_scalar(&plan, refs, NUM_VERTS, out_scalar);
        rsx_vertex_fetch_many(&plan, refs, NUM_VERTS, out_simd);
        if (memcmp(out_scalar, out_simd, (size_t)NUM_VERTS * plan.layout.stride) != 0) {
            printf("ERROR: Mismatch between scalar and SIMD output on %s!\n", tests[t].name);
        }

        // Time scalar
        double t0 = now_sec();
        for (int it = 0; it < NUM_ITERS; it++) {
            rsx_vertex_fetch_many_scalar(&plan, refs, NUM_VERTS, out_scalar);
        }
        double dt_scalar = (now_sec() - t0) * 1000.0;

        // Time SIMD
        t0 = now_sec();
        for (int it = 0; it < NUM_ITERS; it++) {
            rsx_vertex_fetch_many(&plan, refs, NUM_VERTS, out_simd);
        }
        double dt_simd = (now_sec() - t0) * 1000.0;

        double speedup = dt_scalar / dt_simd;
        printf("%-27s | %6.2f ms (%4.2fns) | %6.2f ms (%4.2fns) | %5.2fx\n",
               tests[t].name,
               dt_scalar, dt_scalar * 1e6 / (NUM_ITERS * NUM_VERTS),
               dt_simd, dt_simd * 1e6 / (NUM_ITERS * NUM_VERTS),
               speedup);
    }

    printf("----------------------------+-----------------+-----------------+---------\n");

    // Combined 4-attribute draw
    rsx_vertex_fetch_plan full_plan;
    memset(&full_plan, 0, sizeof full_plan);
    u32 mask = (1u << 0) | (1u << 8) | (1u << 3) | (1u << 2);
    rsx_vertex_layout_plan_init(&full_plan.layout, mask);
    init_plan_attr(&full_plan, 0, RSX_VTX_TYPE_FLOAT, 3, 32, pos_data);
    init_plan_attr(&full_plan, 8, RSX_VTX_TYPE_HALF, 2, 4, uv_data);
    init_plan_attr(&full_plan, 3, RSX_VTX_TYPE_UNORM8, 4, 4, color_data);
    init_plan_attr(&full_plan, 2, RSX_VTX_TYPE_SNORM16, 4, 8, normal_data);

    // Verify bit-exact
    rsx_vertex_fetch_many_scalar(&full_plan, refs, NUM_VERTS, out_scalar);
    rsx_vertex_fetch_many(&full_plan, refs, NUM_VERTS, out_simd);
    if (memcmp(out_scalar, out_simd, (size_t)NUM_VERTS * full_plan.layout.stride) != 0) {
        printf("ERROR: Mismatch in combined draw!\n");
    }

    double t0 = now_sec();
    for (int it = 0; it < NUM_ITERS; it++) {
        rsx_vertex_fetch_many_scalar(&full_plan, refs, NUM_VERTS, out_scalar);
    }
    double dt_scalar_total = (now_sec() - t0) * 1000.0;

    t0 = now_sec();
    for (int it = 0; it < NUM_ITERS; it++) {
        rsx_vertex_fetch_many(&full_plan, refs, NUM_VERTS, out_simd);
    }
    double dt_simd_attr_major = (now_sec() - t0) * 1000.0;

#if defined(__AVX2__) && defined(__F16C__)
    t0 = now_sec();
    for (int it = 0; it < NUM_ITERS; it++) {
        rsx_vertex_fetch_many_interleaved_realistic(&full_plan, refs, NUM_VERTS, out_simd);
    }
    double dt_simd_vtx_major = (now_sec() - t0) * 1000.0;
#endif

    printf("%-27s | %6.2f ms (%4.2fns) | %6.2f ms (%4.2fns) | %5.2fx\n",
           "TOTAL (4 attrs, 64B out)",
           dt_scalar_total, dt_scalar_total * 1e6 / (NUM_ITERS * NUM_VERTS),
           dt_simd_attr_major, dt_simd_attr_major * 1e6 / (NUM_ITERS * NUM_VERTS),
           dt_scalar_total / dt_simd_attr_major);
    printf("=================================================================================\n");

#if defined(__AVX2__) && defined(__F16C__)
    printf("\nWrite-order comparison on 4-attribute draw (64B vertex):\n");
    printf("  Attribute-major SIMD:          %6.2f ms (%4.2f ns/vert, %5.1f Mverts/s)\n",
           dt_simd_attr_major, dt_simd_attr_major * 1e6 / (NUM_ITERS * NUM_VERTS),
           (NUM_ITERS * NUM_VERTS / (dt_simd_attr_major / 1000.0)) / 1e6);
    printf("  Vertex-major (interleaved):    %6.2f ms (%4.2f ns/vert, %5.1f Mverts/s)\n",
           dt_simd_vtx_major, dt_simd_vtx_major * 1e6 / (NUM_ITERS * NUM_VERTS),
           (NUM_ITERS * NUM_VERTS / (dt_simd_vtx_major / 1000.0)) / 1e6);
    printf("  Interleaved write advantage:   %.2fx faster than attribute-major (%.1f%% time reduction)\n",
           dt_simd_attr_major / dt_simd_vtx_major,
           100.0 * (1.0 - dt_simd_vtx_major / dt_simd_attr_major));
#endif

    return 0;
}
