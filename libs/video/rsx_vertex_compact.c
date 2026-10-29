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
#define RSX_HAVE_SSSE3 1
#endif
#if defined(__SSE4_1__) || defined(__AVX2__)
#define RSX_HAVE_SSE41 1
#endif
#if defined(__F16C__) || (defined(_MSC_VER) && defined(__AVX2__))
#define RSX_HAVE_F16C 1
#endif
#endif

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
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

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (rsx_vertex_freq_plain) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (rsx_vertex_freq_plain(fetch->desc.frequency,
                                  (plan->divider_mask >> attr) & 1u)) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
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

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

#if defined(RSX_HAVE_SSSE3)
static inline __m128i rsx_load_16(const u8* p)
{
    return _mm_loadu_si128((const __m128i*)p);
}

static inline __m128i rsx_load_12(const u8* p)
{
    u64 lo;
    u32 hi;
    memcpy(&lo, p, 8);
    memcpy(&hi, p + 8, 4);
    return _mm_unpacklo_epi64(_mm_cvtsi64_si128((s64)lo), _mm_cvtsi32_si128((s32)hi));
}

static inline __m128i rsx_load_8(const u8* p)
{
    u64 v;
    memcpy(&v, p, 8);
    return _mm_cvtsi64_si128((s64)v);
}

static inline __m128i rsx_load_6(const u8* p)
{
    u32 lo;
    u16 hi;
    memcpy(&lo, p, 4);
    memcpy(&hi, p + 4, 2);
    return _mm_cvtsi64_si128((s64)((u64)lo | ((u64)hi << 32)));
}

static inline __m128i rsx_load_4(const u8* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i rsx_load_3(const u8* p)
{
    u32 v = 0;
    memcpy(&v, p, 3);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i rsx_load_2(const u8* p)
{
    u16 v;
    memcpy(&v, p, 2);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i rsx_load_1(const u8* p)
{
    return _mm_cvtsi32_si128((s32)*p);
}

static inline void rsx_decode_f32_simd(const u8* src, float* out, u32 n)
{
    const __m128i w1 = _mm_set_epi32(0x3f800000, 0, 0, 0);
    switch (n) {
    case 4: {
        const __m128i m4 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_shuffle_epi8(rsx_load_16(src), m4)));
        break;
    }
    case 3: {
        const __m128i m3 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(rsx_load_12(src), m3), w1)));
        break;
    }
    case 2: {
        const __m128i m2 = _mm_setr_epi8(3,2,1,0, 7,6,5,4, -1,-1,-1,-1, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(rsx_load_8(src), m2), w1)));
        break;
    }
    case 1: {
        const __m128i m1 = _mm_setr_epi8(3,2,1,0, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1);
        _mm_storeu_ps(out, _mm_castsi128_ps(_mm_or_si128(_mm_shuffle_epi8(rsx_load_4(src), m1), w1)));
        break;
    }
    }
}
#endif

#if defined(RSX_HAVE_F16C) && defined(RSX_HAVE_SSSE3) && defined(RSX_HAVE_SSE41)
static inline void rsx_decode_f16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = rsx_load_8(src); break;
    case 3: raw = rsx_load_6(src); break;
    case 2: raw = rsx_load_4(src); break;
    case 1: raw = rsx_load_2(src); break;
    default: return;
    }
    __m128i h16 = _mm_shuffle_epi8(raw, shuf);
    /* F16C _mm_cvtph_ps flushes/normalizes denormals and sets the quiet bit on NaNs.
     * Scalar rsx_compact_be_f16 flushes all exp==0 to zero and preserves NaN payload
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
            out[c] = rsx_compact_be_f16(src + c * 2);
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

#if defined(RSX_HAVE_SSE41)
static inline void rsx_decode_unorm8_simd(const u8* src, float* out, u32 n)
{
    const __m128 div255 = _mm_set1_ps(255.0f);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = rsx_load_4(src); break;
    case 3: raw = rsx_load_3(src); break;
    case 2: raw = rsx_load_2(src); break;
    case 1: raw = rsx_load_1(src); break;
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

static inline void rsx_decode_uint8_simd(const u8* src, float* out, u32 n)
{
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = rsx_load_4(src); break;
    case 3: raw = rsx_load_3(src); break;
    case 2: raw = rsx_load_2(src); break;
    case 1: raw = rsx_load_1(src); break;
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

#if defined(RSX_HAVE_SSSE3) && defined(RSX_HAVE_SSE41)
static inline void rsx_decode_snorm16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 div32767 = _mm_set1_ps(32767.0f);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = rsx_load_8(src); break;
    case 3: raw = rsx_load_6(src); break;
    case 2: raw = rsx_load_4(src); break;
    case 1: raw = rsx_load_2(src); break;
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

static inline void rsx_decode_sint16_simd(const u8* src, float* out, u32 n)
{
    const __m128i shuf = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, -1,-1,-1,-1,-1,-1,-1,-1);
    const __m128 w1_f = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    __m128i raw;
    switch (n) {
    case 4: raw = rsx_load_8(src); break;
    case 3: raw = rsx_load_6(src); break;
    case 2: raw = rsx_load_4(src); break;
    case 1: raw = rsx_load_2(src); break;
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

#define RSX_FETCH_LOOP_EXPAND(call_decode)                                      \
    if (plain) {                                                                \
        switch (n) {                                                            \
        case 4: {                                                               \
            u8* out_ptr = out + slot * 16u;                                     \
            for (u32 i = 0; i < count; i++, out_ptr += stride) {               \
                float* v = (float*)out_ptr;                                     \
                const u32 element =                                             \
                    (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;     \
                if (__builtin_expect(element - first > span, 0)) {              \
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))  \
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
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))  \
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
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))  \
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
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))  \
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
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))      \
                    return 0;                                                   \
                continue;                                                       \
            }                                                                   \
            const u8* src = base + (u64)(element - first) * fstride;            \
            call_decode(src, v, n);                                             \
        }                                                                       \
    }

#define RSX_SCALAR_LOOP(decode_expr)                                            \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                           \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v))          \
                return 0;                                                       \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode_expr; }                            \
    }

int rsx_vertex_fetch_many(
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
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
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
#if defined(RSX_HAVE_SSSE3)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_f32_simd)
#else
            RSX_SCALAR_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
#endif
            break;
        case RSX_VTX_TYPE_HALF:
#if defined(RSX_HAVE_F16C) && defined(RSX_HAVE_SSSE3) && defined(RSX_HAVE_SSE41)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_f16_simd)
#else
            RSX_SCALAR_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
#endif
            break;
        case RSX_VTX_TYPE_UNORM8:
#if defined(RSX_HAVE_SSE41)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_unorm8_simd)
#else
            RSX_SCALAR_LOOP(v[c] = src[c] / 255.0f)
#endif
            break;
        case RSX_VTX_TYPE_UINT8:
#if defined(RSX_HAVE_SSE41)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_uint8_simd)
#else
            RSX_SCALAR_LOOP(v[c] = (float)src[c])
#endif
            break;
        case RSX_VTX_TYPE_SNORM16:
#if defined(RSX_HAVE_SSSE3) && defined(RSX_HAVE_SSE41)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_snorm16_simd)
#else
            RSX_SCALAR_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
#endif
            break;
        case RSX_VTX_TYPE_SINT16:
#if defined(RSX_HAVE_SSSE3) && defined(RSX_HAVE_SSE41)
            RSX_FETCH_LOOP_EXPAND(rsx_decode_sint16_simd)
#else
            RSX_SCALAR_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
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
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;
                }
            }
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP_EXPAND
#undef RSX_SCALAR_LOOP

int rsx_vertex_fetch_plan_can_fail(const rsx_vertex_fetch_plan* plan)
{
    if (!plan)
        return 1;
    if (!(plan->layout.mask & 1u))
        return 0;
    /* Every element of the prepared references lies in a guest array's
     * stable span, and a known type decodes from one without failing: only
     * ATTR0 resolved element by element can fail. An INLINE_ARRAY span is the
     * stream's length, not the references' range, so a reference past it
     * fails there too. */
    const rsx_vertex_fetch_attr* fetch = &plan->attr[0];
    if (!fetch->desc.type || !fetch->desc.size)
        return 0;
    return !fetch->stable_base || plan->inline_data ||
           fetch->desc.type < RSX_VTX_TYPE_SNORM16 ||
           fetch->desc.type > RSX_VTX_TYPE_UINT8;
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;
    /* A draw_arrays or an ascending index run: every reference is already
     * unique, so skip the hash. No caller reads the table unless the unique
     * count came back smaller. */
    {
        u32 i = 1;
        while (i < count && refs[i].base_index == refs[0].base_index &&
               refs[i].vertex_id == refs[i - 1].vertex_id + 1u)
            i++;
        if (i == count)
            return 1;
    }

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
