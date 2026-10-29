/*
 * rsx_vertex_fetch_many as it was before the blocked, streaming rewrite
 * (rsx_vertex_compact.c at 255ad438), frozen as the reference the rewrite must
 * match bit for bit: test_vertex_fetch_direct.c compares the two, and
 * tests/bench/vertex_cache times them side by side. Only renamed (legacy_*,
 * all static) so it links beside the real one; include it in one file.
 */
#ifndef PS3RECOMP_RSX_VERTEX_FETCH_LEGACY_H
#define PS3RECOMP_RSX_VERTEX_FETCH_LEGACY_H

#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"


#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if !defined(RSX_NO_SIMD)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#endif

#if !defined(__has_builtin)
#define __has_builtin(x) 0
#endif
#if !defined(__GNUC__) && !__has_builtin(__builtin_expect)
#define __builtin_expect(cond, val) (cond)
#endif

#if !defined(RSX_NO_SIMD)
#if defined(__SSSE3__) || defined(__AVX2__)
#define LEGACY_HAVE_SSSE3 1
#endif
#if defined(__SSE4_1__) || defined(__AVX2__)
#define LEGACY_HAVE_SSE41 1
#endif
#if defined(__F16C__) || (defined(_MSC_VER) && defined(__AVX2__))
#define LEGACY_HAVE_F16C 1
#endif
#endif

static inline float legacy_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static inline float legacy_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}
/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int legacy_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            rsx_dsp_resolve_vertex_offset(plan->base_offset,
                                          fetch->desc.offset) +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

#if defined(LEGACY_HAVE_SSSE3)
static inline __m128i legacy_load_16(const u8* p)
{
    return _mm_loadu_si128((const __m128i*)p);
}

static inline __m128i legacy_load_12(const u8* p)
{
    u64 lo;
    u32 hi;
    memcpy(&lo, p, 8);
    memcpy(&hi, p + 8, 4);
    return _mm_unpacklo_epi64(_mm_cvtsi64_si128((s64)lo), _mm_cvtsi32_si128((s32)hi));
}

static inline __m128i legacy_load_8(const u8* p)
{
    u64 v;
    memcpy(&v, p, 8);
    return _mm_cvtsi64_si128((s64)v);
}

static inline __m128i legacy_load_6(const u8* p)
{
    u32 lo;
    u16 hi;
    memcpy(&lo, p, 4);
    memcpy(&hi, p + 4, 2);
    return _mm_cvtsi64_si128((s64)((u64)lo | ((u64)hi << 32)));
}

static inline __m128i legacy_load_4(const u8* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i legacy_load_3(const u8* p)
{
    u32 v = 0;
    memcpy(&v, p, 3);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i legacy_load_2(const u8* p)
{
    u16 v;
    memcpy(&v, p, 2);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i legacy_load_1(const u8* p)
{
    return _mm_cvtsi32_si128((s32)*p);
}

static inline void legacy_decode_f32_simd(const u8* src, float* out, u32 n)
{
    const __m128i w1 = _mm_set_epi32(0x3f800000, 0, 0, 0);
    switch (n) {
    case 4: {
        const __m128i m4 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_shuffle_epi8(legacy_load_16(src), m4)));
        break;
    }
    case 3: {
        const __m128i m3 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(legacy_load_12(src), m3), w1)));
        break;
    }
    case 2: {
        const __m128i m2 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, -1,-1,-1,-1, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(legacy_load_8(src), m2), w1)));
        break;
    }
    case 1: {
        const __m128i m1 = _mm_setr_epi8(3,2,1,0, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(legacy_load_4(src), m1), w1)));
        break;
    }
    }
}
#endif

#if defined(LEGACY_HAVE_F16C) && defined(LEGACY_HAVE_SSSE3) && defined(LEGACY_HAVE_SSE41)
static inline void legacy_decode_f16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = legacy_load_8(src); break;
    case 3: raw = legacy_load_6(src); break;
    case 2: raw = legacy_load_4(src); break;
    case 1: raw = legacy_load_2(src); break;
    default: return;
    }
    __m128i h16 = _mm_shuffle_epi8(raw, shuf);
    /* F16C _mm_cvtph_ps flushes/normalizes denormals and sets the quiet bit on NaNs.
     * Scalar legacy_be_f16 flushes all exp==0 to zero and preserves NaN payload
     * without quieting. Check if any active component has exp==0 or exp==31 with man!=0;
     * if so, fall back to the exact scalar converter for bit-exact results. */
    __m128i exp = _mm_and_si128(_mm_srli_epi16(h16, 10), _mm_set1_epi16(0x1F));
    __m128i is_special = _mm_or_si128(_mm_cmpeq_epi16(exp, _mm_setzero_si128()),
                                     _mm_cmpeq_epi16(exp, _mm_set1_epi16(0x1F)));
    __m128i man = _mm_and_si128(h16, _mm_set1_epi16(0x3FF));
    __m128i bad = _mm_and_si128(is_special, _mm_cmpgt_epi16(man, _mm_setzero_si128()));
    static const int valid_masks[5] = {0, 0x03, 0x0F, 0x3F, 0xFF};
    if (__builtin_expect((_mm_movemask_epi8(bad) & valid_masks[n]) != 0, 0)) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        for (u32 c = 0; c < n; c++)
            out[c] = legacy_be_f16(src + c * 2);
        return;
    }
    __m128 f = _mm_cvtph_ps(h16);
    switch (n) {
    case 4: _mm_storeu_ps(out, f); break;
    case 3: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0x8)); break;
    case 2: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xC)); break;
    case 1: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xE)); break;
    }
}
#endif

#if defined(LEGACY_HAVE_SSE41)
static inline void legacy_decode_unorm8_simd(const u8* src, float* out, u32 n)
{
    const __m128 div255 = _mm_set1_ps(255.0f);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = legacy_load_4(src); break;
    case 3: raw = legacy_load_3(src); break;
    case 2: raw = legacy_load_2(src); break;
    case 1: raw = legacy_load_1(src); break;
    default: return;
    }
    __m128i i32 = _mm_cvtepu8_epi32(raw);
    __m128 f = _mm_div_ps(_mm_cvtepi32_ps(i32), div255);
    switch (n) {
    case 4: _mm_storeu_ps(out, f); break;
    case 3: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0x8)); break;
    case 2: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xC)); break;
    case 1: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xE)); break;
    }
}

static inline void legacy_decode_uint8_simd(const u8* src, float* out, u32 n)
{
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = legacy_load_4(src); break;
    case 3: raw = legacy_load_3(src); break;
    case 2: raw = legacy_load_2(src); break;
    case 1: raw = legacy_load_1(src); break;
    default: return;
    }
    __m128i i32 = _mm_cvtepu8_epi32(raw);
    __m128 f = _mm_cvtepi32_ps(i32);
    switch (n) {
    case 4: _mm_storeu_ps(out, f); break;
    case 3: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0x8)); break;
    case 2: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xC)); break;
    case 1: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xE)); break;
    }
}
#endif

#if defined(LEGACY_HAVE_SSSE3) && defined(LEGACY_HAVE_SSE41)
static inline void legacy_decode_snorm16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 div32767 = _mm_set1_ps(32767.0f);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = legacy_load_8(src); break;
    case 3: raw = legacy_load_6(src); break;
    case 2: raw = legacy_load_4(src); break;
    case 1: raw = legacy_load_2(src); break;
    default: return;
    }
    __m128i s16 = _mm_shuffle_epi8(raw, shuf);
    __m128i i32 = _mm_cvtepi16_epi32(s16);
    __m128 f = _mm_div_ps(_mm_cvtepi32_ps(i32), div32767);
    switch (n) {
    case 4: _mm_storeu_ps(out, f); break;
    case 3: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0x8)); break;
    case 2: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xC)); break;
    case 1: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xE)); break;
    }
}

static inline void legacy_decode_sint16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = legacy_load_8(src); break;
    case 3: raw = legacy_load_6(src); break;
    case 2: raw = legacy_load_4(src); break;
    case 1: raw = legacy_load_2(src); break;
    default: return;
    }
    __m128i s16 = _mm_shuffle_epi8(raw, shuf);
    __m128i i32 = _mm_cvtepi16_epi32(s16);
    __m128 f = _mm_cvtepi32_ps(i32);
    switch (n) {
    case 4: _mm_storeu_ps(out, f); break;
    case 3: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0x8)); break;
    case 2: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xC)); break;
    case 1: _mm_storeu_ps(out, _mm_blend_ps(f, w1_f, 0xE)); break;
    }
}
#endif

#define LEGACY_FETCH_LOOP_EXPAND(call_decode)                                      \
    if (plain) {                                                                \
        switch (n) {                                                            \
        case 4: {                                                               \
            u8* out_ptr = out + slot * 16u;                                     \
            for (u32 i = 0; i < count; i++, out_ptr += stride) {               \
                float* v = (float*)out_ptr;                                     \
                const u32 element =                                             \
                    (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;     \
                if (__builtin_expect(element - first > span, 0)) {              \
                    if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))  \
                        return 0;                                               \
                    continue;                                                   \
                }                                                               \
                const u8* src = base + (u64)(element - first) * fstride;        \
                call_decode(src, v, 4);                                         \
            }                                                                   \
            break;                                                              \
        }                                                                       \
        case 3: {                                                               \
            u8* out_ptr = out + slot * 16u;                                     \
            for (u32 i = 0; i < count; i++, out_ptr += stride) {               \
                float* v = (float*)out_ptr;                                     \
                const u32 element =                                             \
                    (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;     \
                if (__builtin_expect(element - first > span, 0)) {              \
                    if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))  \
                        return 0;                                               \
                    continue;                                                   \
                }                                                               \
                const u8* src = base + (u64)(element - first) * fstride;        \
                call_decode(src, v, 3);                                         \
            }                                                                   \
            break;                                                              \
        }                                                                       \
        case 2: {                                                               \
            u8* out_ptr = out + slot * 16u;                                     \
            for (u32 i = 0; i < count; i++, out_ptr += stride) {               \
                float* v = (float*)out_ptr;                                     \
                const u32 element =                                             \
                    (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;     \
                if (__builtin_expect(element - first > span, 0)) {              \
                    if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))  \
                        return 0;                                               \
                    continue;                                                   \
                }                                                               \
                const u8* src = base + (u64)(element - first) * fstride;        \
                call_decode(src, v, 2);                                         \
            }                                                                   \
            break;                                                              \
        }                                                                       \
        default: {                                                              \
            u8* out_ptr = out + slot * 16u;                                     \
            for (u32 i = 0; i < count; i++, out_ptr += stride) {               \
                float* v = (float*)out_ptr;                                     \
                const u32 element =                                             \
                    (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;     \
                if (__builtin_expect(element - first > span, 0)) {              \
                    if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))  \
                        return 0;                                               \
                    continue;                                                   \
                }                                                               \
                const u8* src = base + (u64)(element - first) * fstride;        \
                call_decode(src, v, 1);                                         \
            }                                                                   \
            break;                                                              \
        }                                                                       \
        }                                                                       \
    } else {                                                                    \
        u8* out_ptr = out + slot * 16u;                                         \
        for (u32 i = 0; i < count; i++, out_ptr += stride) {                   \
            float* v = (float*)out_ptr;                                         \
            const u32 element = rsx_vertex_element_index(                       \
                refs[i].vertex_id, refs[i].base_index,                          \
                fetch->desc.frequency, modulo);                                 \
            if (__builtin_expect(element - first > span, 0)) {                  \
                if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))      \
                    return 0;                                                   \
                continue;                                                       \
            }                                                                   \
            const u8* src = base + (u64)(element - first) * fstride;            \
            call_decode(src, v, n);                                             \
        }                                                                       \
    }

#define LEGACY_SCALAR_LOOP(decode_expr)                                            \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                           \
            if (!legacy_fetch_attr_value(plan, attr, &refs[i], v))          \
                return 0;                                                       \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode_expr; }                            \
    }

static int rsx_vertex_fetch_many_legacy(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        if (!type || !fetch->desc.size) {   /* no array: every vertex reads the default */
            for (u32 i = 0; i < count; i++)
                memcpy(out + (size_t)i * stride + slot * 16u, fetch->default_value, 16);
            continue;
        }
        if (!base ||
            type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!legacy_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
#if defined(LEGACY_HAVE_SSSE3)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_f32_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = legacy_be_f32(src + c * 4))
#endif
            break;
        case RSX_VTX_TYPE_HALF:
#if defined(LEGACY_HAVE_F16C) && defined(LEGACY_HAVE_SSSE3) && defined(LEGACY_HAVE_SSE41)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_f16_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = legacy_be_f16(src + c * 2))
#endif
            break;
        case RSX_VTX_TYPE_UNORM8:
#if defined(LEGACY_HAVE_SSE41)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_unorm8_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = src[c] / 255.0f)
#endif
            break;
        case RSX_VTX_TYPE_UINT8:
#if defined(LEGACY_HAVE_SSE41)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_uint8_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = (float)src[c])
#endif
            break;
        case RSX_VTX_TYPE_SNORM16:
#if defined(LEGACY_HAVE_SSSE3) && defined(LEGACY_HAVE_SSE41)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_snorm16_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
#endif
            break;
        case RSX_VTX_TYPE_SINT16:
#if defined(LEGACY_HAVE_SSSE3) && defined(LEGACY_HAVE_SSE41)
            LEGACY_FETCH_LOOP_EXPAND(legacy_decode_sint16_simd)
#else
            LEGACY_SCALAR_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
#endif
            break;
        default:   /* RSX_VTX_TYPE_CMP32: one packed word, whatever the size */
            /* Inside the stable range, decode from the known source instead of
             * resolving each element again (packed normals are common). */
            for (u32 i = 0; i < count; i++) {
                float* v = (float*)(out + (size_t)i * stride + slot * 16u);
                const u32 element = plain
                    ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)
                    : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,
                                               fetch->desc.frequency, modulo);
                if (element - first > span ||
                    !rsx_vertex_decode_element(type, fetch->desc.size,
                                               base + (u64)(element - first) * fstride, v)) {
                    if (!legacy_fetch_attr_value(plan, attr, &refs[i], v)) return 0;
                }
            }
            break;
        }
    }
    return 1;
}
#undef LEGACY_FETCH_LOOP_EXPAND
#undef LEGACY_SCALAR_LOOP

#endif /* PS3RECOMP_RSX_VERTEX_FETCH_LEGACY_H */
