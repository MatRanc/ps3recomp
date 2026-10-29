#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
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
#if defined(__SSE4_1__) || defined(__AVX2__)
#define RSX_HAVE_SSE41 1
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

/* ---- rsx_vertex_fetch_many ----------------------------------------------
 *
 * The draw's references go through in blocks of RSX_FETCH_BLOCK vertices.
 * Per block the plain element of every reference is worked out once and
 * shared by every ordinary attribute; then each attribute converts the whole
 * block in a loop of its own, one per format, chosen once per draw, so no
 * vertex pays a call or a branch on its format. Loads are full width (a
 * float3 is read as 16 bytes), so an element must lie where that stays inside
 * the span the plan mapped; the block's lowest and highest elements say
 * whether all of them do, and they nearly always do. A block where some do
 * not (the last element of a float3 array, or one outside the stable span)
 * runs the loop that checks each element, and the few it skips are done
 * exactly, one by one, afterwards.
 *
 * Into ordinary memory the block is converted where it lands. Into memory the
 * CPU only writes (rsx_vertex_fetch_many_stream) it is assembled in a tile on
 * the stack and written out whole with non-temporal stores, vertex after
 * vertex: attribute by attribute, each line of a write-combined GPU ring would
 * be written once per attribute, and a cached one read in first.
 *
 * Every conversion is the scalar one's, bit for bit: big-endian floats are
 * only byte-swapped; halves are rebuilt with integer operations exactly as
 * rsx_compact_be_f16 does (denormals flushed, NaN payloads kept), not with
 * F16C, which does neither; x / 255 and x / 32767 are x * 2^-8 + x * c and
 * x * 2^-15 + x * c, whose first product is exact, which gives the quotient's
 * own rounding for every 8- and 16-bit x (and with a fused multiply-add too).
 * test_vertex_fetch_direct.c holds this against the converter it replaced. */

#define RSX_FETCH_BLOCK 32u

enum {
    RSX_FETCH_DEFAULT,      /* no array: the attribute's current value       */
    RSX_FETCH_LOOP,         /* a stable span and a format loop               */
    RSX_FETCH_EXACT,        /* a stable span, decoded element by element     */
    RSX_FETCH_RESOLVE       /* no stable span, or no decoder: per element    */
};

typedef struct rsx_fetch_slot rsx_fetch_slot;
typedef u32 (*rsx_fetch_loop_fn)(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                 u8* dst, u32 ostride, u32* slow);

struct rsx_fetch_slot {
    u32 kind;
    u32 attr;
    u32 offset;             /* slot * 16 within a vertex                     */
    int plain;              /* element = (vertex_id + base_index) & 0xFFFFF  */
    u32 frequency, modulo;
    const u8* base;
    u32 first, span, fstride;
    u32 lim;                /* rel <= lim: in the span, and a full-width load
                             * at rel * fstride stays inside it              */
    rsx_fetch_loop_fn loop;     /* checks every element                      */
    rsx_fetch_loop_fn loop_all; /* every element known to be <= lim          */
#if defined(RSX_HAVE_SSE41)
    __m128i shuf;           /* byte swap, and zero for absent components     */
    __m128i fill;           /* 1.0 in w when there are fewer than 4          */
#else
    u32 fill_n;             /* components to decode                          */
#endif
};

/* An element inside the stable span, exactly as the per-element path decodes
 * it (rsx_vertex_decode_element never fails for a known type). */
static void rsx_fetch_exact(const rsx_vertex_fetch_attr* fetch, const u8* src, u8* v)
{
    float value[4];
    rsx_vertex_decode_element(fetch->desc.type, fetch->desc.size, src, value);
    memcpy(v, value, 16);
}

#if defined(RSX_HAVE_SSE41)
static inline __m128i rsx_ld32(const u8* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return _mm_cvtsi32_si128((s32)v);
}

static inline __m128i rsx_ld64(const u8* p)
{
    u64 v;
    memcpy(&v, p, 8);
    return _mm_cvtsi64_si128((s64)v);
}

/* Each format's loop comes twice: checking every element, and for a block
 * whose elements are all known to be in reach (the usual case), not. */
#if defined(_MSC_VER) && !defined(__clang__)
#define RSX_ALWAYS_INLINE __forceinline
#else
#define RSX_ALWAYS_INLINE inline __attribute__((always_inline))
#endif
#define RSX_FETCH_LOOP_BEGIN                                                    \
    const u8* const base = s->base;                                             \
    const u32 first = s->first, lim = s->lim, fs = s->fstride;                  \
    const __m128i shuf = s->shuf, fill = s->fill;                               \
    u32 ns = 0;                                                                 \
    for (u32 i = 0; i < nb; i++, dst += ostride) {                              \
        const u32 rel = elem[i] - first;                                        \
        if (checked && __builtin_expect(rel > lim, 0)) { slow[ns++] = i; continue; } \
        const u8* src = base + (size_t)rel * fs;
#define RSX_FETCH_LOOP_END                                                      \
    }                                                                           \
    return ns;
#define RSX_FETCH_LOOP_PAIR(name)                                               \
static u32 name##_checked(const rsx_fetch_slot* s, const u32* elem, u32 nb,     \
                          u8* dst, u32 ostride, u32* slow)                      \
{ return name(s, elem, nb, dst, ostride, slow, 1); }                            \
static u32 name##_all(const rsx_fetch_slot* s, const u32* elem, u32 nb,         \
                      u8* dst, u32 ostride, u32* slow)                          \
{ return name(s, elem, nb, dst, ostride, slow, 0); }

/* Full width: 16 bytes for a float, 8 for 16-bit components, 4 for bytes. */
static RSX_ALWAYS_INLINE u32 rsx_loop_f32(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                          u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128i v = _mm_loadu_si128((const __m128i*)src);
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(_mm_shuffle_epi8(v, shuf), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_f32)

/* rsx_compact_be_f16 on four halves, one per 32-bit lane (the high 16 bits
 * clear): exponent 0 is a signed zero, 31 keeps its mantissa as an infinity
 * or NaN, the rest rebias by 112. */
static inline __m128i rsx_f16_bits(__m128i h)
{
    const __m128i e = _mm_and_si128(h, _mm_set1_epi32(0x7C00));
    const __m128i rebias = _mm_set1_epi32(112 << 23);
    __m128i v = _mm_add_epi32(_mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x7FFF)), 13), rebias);
    v = _mm_add_epi32(v, _mm_and_si128(_mm_cmpeq_epi32(e, _mm_set1_epi32(0x7C00)), rebias));
    v = _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), v);
    return _mm_or_si128(v, _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x8000)), 16));
}

static RSX_ALWAYS_INLINE u32 rsx_loop_f16(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                          u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128i h = _mm_shuffle_epi8(rsx_ld64(src), shuf);
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(rsx_f16_bits(h), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_f16)

static inline __m128 rsx_div255(__m128 x)
{
    return _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(0x1p-8f)),
                      _mm_mul_ps(x, _mm_set1_ps(0x1.010102p-16f)));
}

static inline __m128 rsx_div32767(__m128 x)
{
    return _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(0x1p-15f)),
                      _mm_mul_ps(x, _mm_set1_ps(0x1.0002p-30f)));
}

static RSX_ALWAYS_INLINE u32 rsx_loop_unorm8(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                             u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128 f = rsx_div255(_mm_cvtepi32_ps(_mm_shuffle_epi8(rsx_ld32(src), shuf)));
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(_mm_castps_si128(f), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_unorm8)

static RSX_ALWAYS_INLINE u32 rsx_loop_uint8(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                            u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128 f = _mm_cvtepi32_ps(_mm_shuffle_epi8(rsx_ld32(src), shuf));
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(_mm_castps_si128(f), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_uint8)

static RSX_ALWAYS_INLINE u32 rsx_loop_snorm16(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                              u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128i v = _mm_cvtepi16_epi32(_mm_shuffle_epi8(rsx_ld64(src), shuf));
        const __m128 f = rsx_div32767(_mm_cvtepi32_ps(v));
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(_mm_castps_si128(f), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_snorm16)

static RSX_ALWAYS_INLINE u32 rsx_loop_sint16(const rsx_fetch_slot* s, const u32* elem, u32 nb,
                                             u8* dst, u32 ostride, u32* slow, const int checked)
{
    RSX_FETCH_LOOP_BEGIN
        const __m128i v = _mm_cvtepi16_epi32(_mm_shuffle_epi8(rsx_ld64(src), shuf));
        const __m128 f = _mm_cvtepi32_ps(v);
        _mm_storeu_si128((__m128i*)dst, _mm_or_si128(_mm_castps_si128(f), fill));
    RSX_FETCH_LOOP_END
}
RSX_FETCH_LOOP_PAIR(rsx_loop_sint16)
#undef RSX_FETCH_LOOP_BEGIN
#undef RSX_FETCH_LOOP_END
#undef RSX_FETCH_LOOP_PAIR

/* The shuffle that byte-swaps `n` components of `bytes` each into the low
 * end of 32-bit lanes (16-bit ones into the low half of a lane pair for
 * _mm_cvtepi16_epi32 when `pack16`), zeroing the rest. */
static __m128i rsx_fetch_shuffle(u32 bytes, u32 n, int pack16)
{
    s8 m[16];
    memset(m, -1, sizeof m);
    for (u32 c = 0; c < n; c++)
        for (u32 b = 0; b < bytes; b++)
            m[(pack16 ? c * 2u : c * 4u) + b] = (s8)(c * bytes + bytes - 1u - b);
    __m128i r;
    memcpy(&r, m, 16);
    return r;
}
#else
/* Without SSE4.1 the loops are the scalar conversions, one per format. */
#define RSX_FETCH_LOOP_SCALAR(name, decode_expr)                                \
static u32 name##_checked(const rsx_fetch_slot* s, const u32* elem, u32 nb,     \
                u8* dst, u32 ostride, u32* slow)                                \
{                                                                               \
    const u8* const base = s->base;                                             \
    const u32 first = s->first, lim = s->lim, fs = s->fstride;                  \
    const u32 n = s->fill_n;                                                    \
    u32 ns = 0;                                                                 \
    for (u32 i = 0; i < nb; i++, dst += ostride) {                              \
        const u32 rel = elem[i] - first;                                        \
        if (rel > lim) { slow[ns++] = i; continue; }                            \
        const u8* src = base + (size_t)rel * fs;                                \
        float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };                                \
        for (u32 c = 0; c < n; c++) { decode_expr; }                            \
        memcpy(dst, v, 16);                                                     \
    }                                                                           \
    return ns;                                                                  \
}
#define rsx_loop_f32_all rsx_loop_f32_checked
#define rsx_loop_f16_all rsx_loop_f16_checked
#define rsx_loop_unorm8_all rsx_loop_unorm8_checked
#define rsx_loop_uint8_all rsx_loop_uint8_checked
#define rsx_loop_snorm16_all rsx_loop_snorm16_checked
#define rsx_loop_sint16_all rsx_loop_sint16_checked
RSX_FETCH_LOOP_SCALAR(rsx_loop_f32, v[c] = rsx_compact_be_f32(src + c * 4))
RSX_FETCH_LOOP_SCALAR(rsx_loop_f16, v[c] = rsx_compact_be_f16(src + c * 2))
RSX_FETCH_LOOP_SCALAR(rsx_loop_unorm8, v[c] = src[c] / 255.0f)
RSX_FETCH_LOOP_SCALAR(rsx_loop_uint8, v[c] = (float)src[c])
RSX_FETCH_LOOP_SCALAR(rsx_loop_snorm16,
                      v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
RSX_FETCH_LOOP_SCALAR(rsx_loop_sint16,
                      v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
#undef RSX_FETCH_LOOP_SCALAR
#endif

/* How one used attribute is fetched this draw. */
static void rsx_fetch_slot_init(const rsx_vertex_fetch_plan* plan, u32 slot,
                                rsx_fetch_slot* s)
{
    const u32 attr = plan->layout.attrs[slot];
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    memset(s, 0, sizeof *s);
    s->attr = attr;
    s->offset = slot * 16u;
    s->modulo = (plan->divider_mask >> attr) & 1u;
    s->frequency = fetch->desc.frequency;
    s->plain = rsx_vertex_freq_plain(s->frequency, s->modulo);
    const u32 type = fetch->desc.type;
    if (!type || !fetch->desc.size) {
        s->kind = RSX_FETCH_DEFAULT;
        return;
    }
    if (!fetch->stable_base || type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
        s->kind = RSX_FETCH_RESOLVE;
        return;
    }
    s->base = fetch->stable_base;
    s->first = fetch->stable_first;
    s->span = fetch->stable_last - fetch->stable_first;
    s->fstride = fetch->stride;
    s->kind = RSX_FETCH_EXACT;
    const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
    u32 width = 0, bytes = 0;
    int pack16 = 0;
    switch (type) {
#define RSX_FETCH_LOOPS(name) (s->loop = name##_checked, s->loop_all = name##_all)
    case RSX_VTX_TYPE_FLOAT:   RSX_FETCH_LOOPS(rsx_loop_f32); width = 16; bytes = 4; break;
    case RSX_VTX_TYPE_HALF:    RSX_FETCH_LOOPS(rsx_loop_f16); width = 8; bytes = 2; break;
    case RSX_VTX_TYPE_UNORM8:  RSX_FETCH_LOOPS(rsx_loop_unorm8); width = 4; bytes = 1; break;
    case RSX_VTX_TYPE_UINT8:   RSX_FETCH_LOOPS(rsx_loop_uint8); width = 4; bytes = 1; break;
    case RSX_VTX_TYPE_SNORM16: RSX_FETCH_LOOPS(rsx_loop_snorm16); width = 8; bytes = 2; pack16 = 1; break;
    case RSX_VTX_TYPE_SINT16:  RSX_FETCH_LOOPS(rsx_loop_sint16); width = 8; bytes = 2; pack16 = 1; break;
#undef RSX_FETCH_LOOPS
    default:                   return;   /* CMP32: one packed word, decoded exactly */
    }
    /* The span the plan mapped ends elem_size past its last element's start;
     * a full-width load may start no later than `width` before that. */
    const u64 span_bytes = (u64)s->span * s->fstride + fetch->elem_size;
    if (span_bytes < width)
        return;
    const u64 last_wide = s->fstride ? (span_bytes - width) / s->fstride : s->span;
    s->lim = last_wide < s->span ? (u32)last_wide : s->span;
    s->kind = RSX_FETCH_LOOP;
#if defined(RSX_HAVE_SSE41)
    s->shuf = rsx_fetch_shuffle(bytes, n, pack16);
    s->fill = n < 4 ? _mm_setr_epi32(0, 0, 0, 0x3F800000) : _mm_setzero_si128();
#else
    (void)bytes; (void)pack16;
    s->fill_n = n;
#endif
}

/* Block [b, b + nb) of one attribute into dst (vertex i at dst + i * ostride).
 * `plain` holds the block's plain elements, lo..hi. 0 when an ATTR0 element
 * fails. */
static int rsx_fetch_slot_block(const rsx_vertex_fetch_plan* plan, const rsx_fetch_slot* s,
                                const rsx_vertex_ref* refs, u32 b, u32 nb,
                                const u32* plain, u32 lo, u32 hi, u8* dst, u32 ostride)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[s->attr];
    if (s->kind == RSX_FETCH_DEFAULT) {
        for (u32 i = 0; i < nb; i++)
            memcpy(dst + (size_t)i * ostride, fetch->default_value, 16);
        return 1;
    }
    if (s->kind == RSX_FETCH_RESOLVE) {
        for (u32 i = 0; i < nb; i++)
            if (!rsx_vertex_fetch_attr_value(plan, s->attr, &refs[b + i],
                                             (float*)(dst + (size_t)i * ostride)))
                return 0;
        return 1;
    }
    u32 local[RSX_FETCH_BLOCK];
    const u32* elem = plain;
    if (!s->plain) {
        lo = UINT_MAX; hi = 0;
        for (u32 i = 0; i < nb; i++) {
            const u32 e = rsx_vertex_element_index(refs[b + i].vertex_id, refs[b + i].base_index,
                                                   s->frequency, s->modulo);
            local[i] = e;
            lo = e < lo ? e : lo;
            hi = e > hi ? e : hi;
        }
        elem = local;
    }
    u32 slow[RSX_FETCH_BLOCK];
    u32 ns = nb;
    if (s->kind == RSX_FETCH_LOOP) {
        ns = lo >= s->first && hi - s->first <= s->lim
            ? s->loop_all(s, elem, nb, dst, ostride, slow)
            : s->loop(s, elem, nb, dst, ostride, slow);
    } else {
        for (u32 i = 0; i < nb; i++) slow[i] = i;
    }
    for (u32 k = 0; k < ns; k++) {
        const u32 i = slow[k];
        u8* v = dst + (size_t)i * ostride;
        const u32 rel = elem[i] - s->first;
        if (rel <= s->span)
            rsx_fetch_exact(fetch, s->base + (size_t)rel * s->fstride, v);
        else if (!rsx_vertex_fetch_attr_value(plan, s->attr, &refs[b + i], (float*)v))
            return 0;
    }
    return 1;
}

static int rsx_vertex_fetch_blocks(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out, int stream)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    const u32 nslots = plan->layout.count;
    if (!count || !nslots)
        return 1;
    rsx_fetch_slot slots[RSX_DSP_NUM_VERTEX_ATTR];
    int any_plain = 0;
    for (u32 slot = 0; slot < nslots; slot++) {
        rsx_fetch_slot_init(plan, slot, &slots[slot]);
        any_plain |= slots[slot].plain && slots[slot].kind >= RSX_FETCH_LOOP &&
                     slots[slot].kind <= RSX_FETCH_EXACT;
    }
#if defined(RSX_HAVE_SSE41)
    /* A tile of the block in cache, written out whole, in order. Each block's
     * tile sits half a page from where it goes (mod 4 KB), well clear of 4K
     * aliasing between its loads and the non-temporal stores still in flight
     * ahead of them: at some placements the copy stalled on every load and a
     * benchmark frame took 1.8x as long. */
    _Alignas(64) u8 tile_mem[RSX_FETCH_BLOCK * RSX_DSP_NUM_VERTEX_ATTR * 16u + 4096u];
    stream = stream && !((uintptr_t)out & 15u);
#else
    u8 tile_mem[1];
    stream = 0;
#endif
    u32 plain[RSX_FETCH_BLOCK];
    int ok = 1;
    for (u32 b = 0; b < count && ok; b += RSX_FETCH_BLOCK) {
        const u32 nb = count - b < RSX_FETCH_BLOCK ? count - b : RSX_FETCH_BLOCK;
        u8* const block_out = out + (size_t)b * stride;
        u8* const dst = stream
            ? tile_mem + (((uintptr_t)block_out + 2048u - (uintptr_t)tile_mem) & 4095u)
            : block_out;
        u32 lo = UINT_MAX, hi = 0;
        if (any_plain)
            for (u32 i = 0; i < nb; i++) {
                const u32 e = (refs[b + i].vertex_id + refs[b + i].base_index) & 0x000FFFFFu;
                plain[i] = e;
                lo = e < lo ? e : lo;
                hi = e > hi ? e : hi;
            }
        for (u32 slot = 0; slot < nslots; slot++) {
            const rsx_fetch_slot* s = &slots[slot];
            if (!rsx_fetch_slot_block(plan, s, refs, b, nb, plain, lo, hi, dst + s->offset, stride)) {
                ok = 0;
                break;
            }
        }
#if defined(RSX_HAVE_SSE41)
        if (stream && ok) {
            const __m128i* src = (const __m128i*)dst;
            __m128i* d = (__m128i*)block_out;
            for (u32 q = 0, nq = nb * stride / 16u; q < nq; q++)
                _mm_stream_si128(d + q, _mm_load_si128(src + q));
        }
#endif
    }
#if defined(RSX_HAVE_SSE41)
    if (stream)
        _mm_sfence();   /* ordered before whatever hands the memory to the GPU */
#endif
    return ok;
}

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    return rsx_vertex_fetch_blocks(plan, refs, count, out, 0);
}

int rsx_vertex_fetch_many_stream(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    return rsx_vertex_fetch_blocks(plan, refs, count, out, 1);
}

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
