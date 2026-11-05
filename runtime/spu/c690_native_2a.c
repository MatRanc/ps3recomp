/*
 * Native c690 0x2A field gather/pack.  See c690_native_2a.h and
 * docs/edge-native-2a.md.
 *
 * The four lifted passes are entry-hooked by
 * tools/c690_ref/patch_native_2a.py.  The hook runs only when the live key is
 * on and every argument matches a decoded layout; otherwise the lifted body
 * runs byte-for-byte.  The driver around the passes (index pre-pass,
 * descriptor parse, allocator, cursor) is untouched, so this replaces the
 * pack/gather compute, not the command.
 */
#include "c690_native_2a.h"
#include "spu_context.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_2A_HAVE_LIVE_CTL 1
#  endif
#elif defined(__GNUC__)
#  include "ps3_live_ctl.h"
#  define C690_2A_HAVE_LIVE_CTL 1
#endif

/* The c690 job keeps its command context at this Local Store address; the
 * descriptor pointer sits at +0x2D0 and the output cursor at +0xD0 (the same
 * fields 0x10F78 reads).  All 192 meter captures use 0x1F000. */
#define C690_2A_CTX_LSA   0x1F000u
#define C690_2A_DESC_LSA  (C690_2A_CTX_LSA + 0x2D0u)
#define C690_2A_CURSOR_LSA (C690_2A_CTX_LSA + 0xD0u)

/* Pass entry addresses. */
#define C690_2A_PASS_POS    0x7840u
#define C690_2A_PASS_S16    0x7AC0u
#define C690_2A_PASS_F16    0x7540u
#define C690_2A_PASS_NORMAL 0x7FC0u

enum {                      /* values are the lifted pass's r22 kind */
    C2A_FIELD_POS = 0,
    C2A_FIELD_F16 = 1,
    C2A_FIELD_S16 = 2,
    C2A_FIELD_NORMAL = 4
};

/* Decoded record classes and their field layout (edge-gpu-2a-classes.md).
 * Each entry is (pass entry, field offset, field bytes, components, scale). */
typedef struct {
    uint32_t desc0;
    uint32_t stride;
    int      nfields;
    struct { uint32_t pass, off, len, comps; float scale; int kind; } f[7];
} c690_2a_class;

static const c690_2a_class s_classes[] = {
    /* position-first 32-byte */
    { 0x007C0004u, 32, 4, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS },
        { C690_2A_PASS_NORMAL, 12,  4, 3, 0.0f,      C2A_FIELD_NORMAL },
        { C690_2A_PASS_S16,    16,  8, 4, 32768.0f,  C2A_FIELD_S16 },
        { C690_2A_PASS_F16,    24,  4, 2, 0.0f,      C2A_FIELD_F16 } } },
    { 0x00940004u, 32, 4, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS },
        { C690_2A_PASS_NORMAL, 12,  4, 3, 0.0f,      C2A_FIELD_NORMAL },
        { C690_2A_PASS_S16,    16,  8, 4, 32768.0f,  C2A_FIELD_S16 },
        { C690_2A_PASS_F16,    24,  4, 2, 0.0f,      C2A_FIELD_F16 } } },
    /* 20-byte: pos + f16 uv + 2 x s16 (1/8192) */
    { 0x00C40008u, 20, 3, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS },
        { C690_2A_PASS_F16,    12,  4, 2, 0.0f,      C2A_FIELD_F16 },
        { C690_2A_PASS_S16,    16,  4, 2, 8192.0f,   C2A_FIELD_S16 } } },
    /* position-only 12-byte stream (the 48-byte family is four in a row) */
    { 0x00600008u, 12, 1, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS } } },
    /* 28-byte position-first */
    { 0x00900004u, 28, 4, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS },
        { C690_2A_PASS_NORMAL, 12,  4, 3, 0.0f,      C2A_FIELD_NORMAL },
        { C690_2A_PASS_S16,    16,  8, 4, 32768.0f,  C2A_FIELD_S16 },
        { C690_2A_PASS_F16,    24,  4, 2, 0.0f,      C2A_FIELD_F16 } } },
    /* 44-byte */
    { 0x011C0004u, 44, 7, {
        { C690_2A_PASS_POS,     0, 12, 3, 0.0f,      C2A_FIELD_POS },
        { C690_2A_PASS_NORMAL, 12,  4, 3, 0.0f,      C2A_FIELD_NORMAL },
        { C690_2A_PASS_S16,    16,  4, 2, 1024.0f,   C2A_FIELD_S16 },
        { C690_2A_PASS_S16,    20,  8, 4, 32768.0f,  C2A_FIELD_S16 },
        { C690_2A_PASS_S16,    28,  4, 2, 16384.0f,  C2A_FIELD_S16 },
        { C690_2A_PASS_S16,    32,  6, 3, 1024.0f,   C2A_FIELD_S16 },
        { C690_2A_PASS_S16,    38,  6, 3, 1024.0f,   C2A_FIELD_S16 } } },
};

/* ---- live key ---------------------------------------------------------- */

static void report(const char* tag);

static int s_env_read;
static int s_env_on;

static void read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char* s = getenv("PS3RECOMP_C690_NATIVE_2A");
    s_env_on = s && *s && s[0] != '0';
}

static void atexit_report(void)
{
    if (s_env_on) report("final");
}

int c690_native_2a_enabled(void)
{
    read_env();
    if (s_env_on) {
        static int reg;
        if (!reg) { reg = 1; atexit(atexit_report); }
    }
#ifdef C690_2A_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) {
        const int v = ps3_live_ctl_get(PS3_LIVE_C690_NATIVE_2A);
        if (ps3_live_ctl_file_set && ps3_live_ctl_file_set(PS3_LIVE_C690_NATIVE_2A))
            return v != 0;
        if (v) return 1;
    }
#endif
    return s_env_on;
}

/* ---- counters ---------------------------------------------------------- */

static unsigned long long s_try, s_native, s_fallback, s_records;
static unsigned long long s_class_native, s_class_fallback;
static unsigned long long s_last_report_ns;

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void report(const char* tag)
{
    const unsigned long long classes = s_class_native + s_class_fallback;
    fprintf(stderr,
            "[c690-native2a] %s pass=%llu native=%llu fallback=%llu "
            "classes=%llu class_native=%llu class_fallback=%llu "
            "fallback_share=%.3f records=%llu\n",
            tag, s_try, s_native, s_fallback, classes, s_class_native,
            s_class_fallback,
            classes ? (double)s_class_fallback / (double)classes : 0.0,
            s_records);
    fflush(stderr);
}

static void maybe_report(void)
{
    const unsigned long long now = now_ns();
    if (s_last_report_ns == 0) { s_last_report_ns = now; return; }
    if (now - s_last_report_ns >= 2000000000ull) {
        s_last_report_ns = now;
        report("progress");
    }
}

/* ---- byte helpers ------------------------------------------------------ */

static uint32_t be32(const uint8_t* p)
{
    uint32_t w;
    memcpy(&w, p, sizeof w);
    return __builtin_bswap32(w);
}

static uint16_t be16(const uint8_t* p)
{
    uint16_t w;
    memcpy(&w, p, sizeof w);
    return (uint16_t)__builtin_bswap16(w);
}

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void put_be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static float ld_f32(const uint8_t* p)
{
    const uint32_t w = be32(p);
    float v;
    memcpy(&v, &w, sizeof v);
    return v;
}

/* ---- pack rules (edge-gpu-2a-classes.md §2) ---------------------------- */

static int enc_fixed(float v, float scale, int lo, int hi)
{
    const double d = (double)v * (double)scale;
    int t;
    if (!(d == d)) return 0;                 /* NaN: not on this corpus */
    if (d <= (double)lo) return lo;
    if (d >= (double)hi + 1.0) return hi;
    t = (int)d - 1;
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    return t;
}

static uint16_t f16_bits(float v)
{
    uint32_t f;
    memcpy(&f, &v, sizeof f);
    const uint32_t sign = (f >> 16) & 0x8000u;
    const int e = (int)((f >> 23) & 0xFFu) - 127;
    if (e < -14) return (uint16_t)sign;                 /* denormal -> signed zero */
    if (e > 15) return (uint16_t)(sign | 0x7C00u);      /* inf/overflow */
    return (uint16_t)(sign | ((uint32_t)(e + 15) << 10) | ((f >> 13) & 0x3FFu));
}

static uint32_t pack_normal(const uint8_t* e)
{
    const int x = enc_fixed(ld_f32(e + 0), 1024.0f, -1024, 1023);
    const int y = enc_fixed(ld_f32(e + 4), 1024.0f, -1024, 1023);
    const int z = enc_fixed(ld_f32(e + 8), 512.0f, -512, 511);
    return ((uint32_t)x & 0x7FFu) | (((uint32_t)y & 0x7FFu) << 11) |
           (((uint32_t)z & 0x3FFu) << 22);
}

/* ---- pass identification ---------------------------------------------- */

static const c690_2a_class* find_class(uint32_t desc0, uint32_t stride)
{
    for (unsigned i = 0; i < sizeof s_classes / sizeof s_classes[0]; i++)
        if (s_classes[i].desc0 == desc0 && s_classes[i].stride == stride)
            return &s_classes[i];
    return NULL;
}

/* ---- index-list mode cache ---------------------------------------------
 *
 * The four passes of one 0x2A command share (end cursor, count, list, stride).
 * The u16 offset table scan is O(count), so scan once on the first pass of an
 * invocation and reuse it for the other three.  A mismatching tuple always
 * rescans, so a wrapped ring cannot reuse a stale answer. */
static SPU_THREAD_LOCAL uint32_t t_scan_end, t_scan_list, t_scan_count, t_scan_stride;
static SPU_THREAD_LOCAL int t_scan_have, t_scan_valid;

static int index_list_valid(const uint8_t* ls, uint32_t list, uint32_t count)
{
    if ((list & 1u) || list + 2u * count > SPU_LS_SIZE) return 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t v = be16(ls + list + 2u * k);
        if ((v & 15u) || v >= 0x8000u) return 0;
    }
    return 1;
}

static int have_index_list(const uint8_t* ls, uint32_t end, uint32_t list,
                           uint32_t count, uint32_t stride, int force)
{
    if (!force && t_scan_have && t_scan_end == end && t_scan_list == list &&
        t_scan_count == count && t_scan_stride == stride)
        return t_scan_valid;
    t_scan_have = 1;
    t_scan_end = end;
    t_scan_list = list;
    t_scan_count = count;
    t_scan_stride = stride;
    t_scan_valid = index_list_valid(ls, list, count);
    return t_scan_valid;
}

/* ---- native pass ------------------------------------------------------- */

int c690_native_2a_try(struct spu_context* ctx, uint32_t addr)
{
    if (!ctx || !ctx->ls) return 0;
    if (__builtin_expect(!c690_native_2a_enabled(), 0)) return 0;

    const uint8_t* ls = ctx->ls;
    const uint32_t src = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t dst = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    const uint32_t stride = ctx->gpr[5]._u32[0] & 0xFFFFu;
    const uint32_t comps = ctx->gpr[6]._u32[0] & 0xFFu;
    const uint32_t list = ctx->gpr[7]._u32[0] & SPU_LS_MASK;
    const uint32_t count = ctx->gpr[11]._u32[0] & 0xFFFFu;
    const uint32_t kind = ctx->gpr[22]._u32[0] & 0xFFu;

    s_try++;

    const uint32_t cursor = be32(ls + C690_2A_CURSOR_LSA);
    const uint32_t desc_ptr = be32(ls + C690_2A_DESC_LSA) & SPU_LS_MASK;

    if (count == 0 || count > 4096 || stride == 0 || cursor < 0x2F000u ||
        cursor > SPU_LS_SIZE || desc_ptr < 0x4000u || desc_ptr > SPU_LS_SIZE - 4u)
        goto fallback;

    const uint32_t desc0 = be32(ls + desc_ptr);
    const c690_2a_class* cls = find_class(desc0, stride);
    if (!cls) goto fallback;

    /* Locate this pass in the class layout; the output end cursor gives the
     * record-0 base, so the field offset is exact even for misaligned fields. */
    const uint32_t bytes = count * stride;
    if (bytes > cursor) goto fallback;
    const uint32_t ob = cursor - bytes;
    if (dst < ob || dst >= cursor) goto fallback;
    const uint32_t off = dst - ob;

    unsigned fi;
    for (fi = 0; fi < cls->nfields; fi++) {
        if (cls->f[fi].pass == addr && cls->f[fi].off == off &&
            (uint32_t)cls->f[fi].kind == kind && cls->f[fi].comps == comps)
            break;
    }
    if (fi >= cls->nfields) goto fallback;
    {
        const uint32_t eoff = cls->f[fi].off;
        const uint32_t elen = cls->f[fi].len;
        const uint32_t ecomp = cls->f[fi].comps;
        const int ekind = cls->f[fi].kind;
        const float scale = cls->f[fi].scale;

        if (off != eoff || kind != (uint32_t)ekind) goto fallback;
        if (comps != ecomp) goto fallback;
        if (dst + (count - 1u) * stride + elen > SPU_LS_SIZE) goto fallback;

        /* The index list is a u16 byte-offset table when every entry is a
         * 16-byte-aligned offset inside the source region; otherwise the
         * driver used the implicit identity list (edge-gpu-2a-32b.md §3).
         * Scan on the position pass (always first for these classes); the
         * other passes reuse it. */
        const int have_list = have_index_list(ls, cursor, list, count, stride,
                                              addr == C690_2A_PASS_POS);

        for (uint32_t k = 0; k < count; k++) {
            const uint32_t ent = have_list ? (uint32_t)(be16(ls + list + 2u * k) >> 4) : k;
            const uint32_t sa = src + ent * 16u;
            uint8_t* dp = ctx->ls + dst + k * stride;
            if (sa + 16u > SPU_LS_SIZE) goto fallback;
            switch (ekind) {
            case C2A_FIELD_POS:
                memcpy(dp, ls + sa, 12);
                break;
            case C2A_FIELD_NORMAL:
                put_be32(dp, pack_normal(ls + sa));
                break;
            case C2A_FIELD_S16: {
                uint32_t n = ecomp;
                for (uint32_t c = 0; c < n; c++) {
                    int v = enc_fixed(ld_f32(ls + sa + 4u * c), scale, -32768, 32767);
                    put_be16(dp + 2u * c, (uint16_t)v);
                }
                break;
            }
            case C2A_FIELD_F16: {
                uint16_t u = f16_bits(ld_f32(ls + sa + 0));
                uint16_t v = f16_bits(ld_f32(ls + sa + 4));
                put_be16(dp + 0, u);
                put_be16(dp + 2, v);
                break;
            }
            default:
                goto fallback;
            }
        }

        s_native++;
        s_records += count;
        if (addr == C690_2A_PASS_POS) s_class_native++;
        maybe_report();

        /* Same terminal contract as the lifted pass's SPU_RET. */
        ctx->pc = ctx->gpr[0]._u32[0];
        if (ctx->host_depth == 0)
            g_spu_trampoline_fn = spu_indirect_branch;
        return 1;
    }

fallback:
    s_fallback++;
    if (addr == C690_2A_PASS_POS) s_class_fallback++;
    maybe_report();
    return 0;
}
