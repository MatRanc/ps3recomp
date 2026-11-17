/*
 * Native c690 normal/tangent regeneration (see c690_soa_kernels.inc).
 *
 * PS3RECOMP_C690_SOA (live key): 0 off, 1 native, 2 shadow (run the lifted
 * body, run the native kernel on a copy, compare the local store).
 *
 * The generated wrappers (tools/c690_ref/patch_native_soa.py) call
 * c690_soa_try(ctx, lsa, variant, body); variant is the float twin the wrapper
 * was compiled in (0 base, 1 ftz, 2 approx), so the kernel matches the lifted
 * code it replaces bit for bit in the local store. Registers r2..r79 and the
 * callee's dead stack frame are not reproduced: both are volatile in the SPU
 * ABI and the callers (0x10190) reload what they need.
 */
#include "c690_native_soa.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__FMA__) && defined(__SSSE3__)

#include <immintrin.h>

/* Same float path as the lifted module (see c690_native_cull.c). */
#ifndef SPU_FAST_FLOAT
#define SPU_FAST_FLOAT 1
#endif
#ifndef SPU_LIFTED_FAST_LS
#define SPU_LIFTED_FAST_LS 1
#endif
#include "spu_helpers.h"

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_SOA_HAVE_LIVE_CTL 1
#  endif
#endif

#define SOA_INL static inline __attribute__((always_inline))

SOA_INL __m128i soa_m(u128 a) { return _mm_loadu_si128((const __m128i*)a._u32); }
SOA_INL u128 soa_u(__m128i v) { u128 r; _mm_storeu_si128((__m128i*)r._u32, v); return r; }
SOA_INL u128 soa_bc(uint32_t w) { return soa_u(_mm_set1_epi32((int)w)); }
#define soa_sh(a, lane) soa_u(_mm_shuffle_epi32(soa_m(a), (lane) * 0x55))
SOA_INL u128 soa_yzx(u128 a) { return soa_u(_mm_shuffle_epi32(soa_m(a), _MM_SHUFFLE(3, 0, 2, 1))); }
SOA_INL u128 soa_zxy(u128 a) { return soa_u(_mm_shuffle_epi32(soa_m(a), _MM_SHUFFLE(3, 1, 0, 2))); }
SOA_INL u128 soa_xor(u128 a, u128 b) { return soa_u(_mm_xor_si128(soa_m(a), soa_m(b))); }
SOA_INL u128 soa_sel(u128 a, u128 b, u128 m) { return soa_u(_mm_blendv_epi8(soa_m(a), soa_m(b), soa_m(m))); }
/* xyz from v, word 3 from q. */
SOA_INL u128 soa_keep_w(u128 v, u128 q) { return soa_u(_mm_blend_epi16(soa_m(v), soa_m(q), 0xC0)); }

/* Aligned quadword in the local store, big-endian words <-> host lanes. */
SOA_INL __m128i soa_bswap(void) { return _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12); }
SOA_INL u128 soa_ld(const uint8_t* ls, uint32_t a)
{
    return soa_u(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu)))), soa_bswap()));
}
SOA_INL void soa_st(uint8_t* ls, uint32_t a, u128 v)
{
    _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))), _mm_shuffle_epi8(soa_m(v), soa_bswap()));
}
SOA_INL void soa_idx(const uint8_t* ls, uint32_t a, uint32_t ix[3])
{
    const uint8_t* p = ls + (a & (SPU_LS_MASK & ~7u));
    ix[0] = (uint32_t)p[0] << 8 | p[1];
    ix[1] = (uint32_t)p[2] << 8 | p[3];
    ix[2] = (uint32_t)p[4] << 8 | p[5];
}

/* The float twins' helpers, as tools/spu_ftz_twin.py emits them into the
 * generated modules (FTZ_PREAMBLE / APPROX_PREAMBLE). Keep in step with it:
 * shadow mode reports any drift. */
SOA_INL __m128 soa_ps(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
SOA_INL u128 soa_fu(__m128 v) { u128 r; _mm_storeu_ps((float*)r._u32, v); return r; }
SOA_INL u128 soa_hw(__m128 v) { return soa_fu(_mm_andnot_ps(_mm_cmpeq_ps(v, _mm_setzero_ps()), v)); }
SOA_INL u128 hw_fs(u128 a, u128 b) { return soa_hw(_mm_sub_ps(soa_ps(a), soa_ps(b))); }
SOA_INL u128 hw_fm(u128 a, u128 b) { return soa_hw(_mm_mul_ps(soa_ps(a), soa_ps(b))); }
SOA_INL u128 hw_fma(u128 a, u128 b, u128 c)  { return soa_hw(_mm_fmadd_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL u128 hw_fms(u128 a, u128 b, u128 c)  { return soa_hw(_mm_fmsub_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL u128 hw_fnms(u128 a, u128 b, u128 c) { return soa_hw(_mm_fnmadd_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL u128 ap_fs(u128 a, u128 b) { return soa_fu(_mm_sub_ps(soa_ps(a), soa_ps(b))); }
SOA_INL u128 ap_fm(u128 a, u128 b) { return soa_fu(_mm_mul_ps(soa_ps(a), soa_ps(b))); }
SOA_INL u128 ap_fma(u128 a, u128 b, u128 c)  { return soa_fu(_mm_fmadd_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL u128 ap_fms(u128 a, u128 b, u128 c)  { return soa_fu(_mm_fmsub_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL u128 ap_fnms(u128 a, u128 b, u128 c) { return soa_fu(_mm_fnmadd_ps(soa_ps(a), soa_ps(b), soa_ps(c))); }
SOA_INL __m128 ap_zsel(__m128 x, __m128 v, uint32_t bits, int keep_sign)
{
    const __m128 d = _mm_or_ps(_mm_castsi128_ps(_mm_set1_epi32((int)bits)),
                               keep_sign ? _mm_and_ps(x, _mm_set1_ps(-0.0f)) : _mm_setzero_ps());
    return _mm_blendv_ps(v, d, _mm_cmpeq_ps(x, _mm_setzero_ps()));
}
SOA_INL u128 ap_frest(u128 a)
{
    const __m128 x = soa_ps(a);
    return soa_fu(ap_zsel(x, _mm_div_ps(_mm_set1_ps(1.0f), x),
                          0x7F800000u | (spu_frest_fraction_lut[0] & 0x7FFFFFu), 1));
}
SOA_INL u128 ap_frsqest(u128 a)
{
    const __m128 x = soa_ps(a);
    const __m128 m = _mm_andnot_ps(_mm_set1_ps(-0.0f), x);
    return soa_fu(ap_zsel(x, _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(m)),
                          0x7F800000u | (spu_frsqest_fraction_lut[0] & 0x7FFFFFu), 0));
}
SOA_INL u128 ap_fi(u128 a, u128 b)
{
    (void)a;
    const __m128i e = soa_m(b);
    const __m128i huge = _mm_cmpeq_epi32(_mm_and_si128(_mm_srli_epi32(e, 23), _mm_set1_epi32(0xFF)), _mm_set1_epi32(0xFF));
    return soa_u(_mm_andnot_si128(_mm_and_si128(huge, _mm_set1_epi32(0x3FF)), e));
}

#define K(n) soa0_##n
#define FS spu_fs
#define FM spu_fm
#define FMA spu_fma
#define FMS spu_fms
#define FNMS spu_fnms
#define FRSQEST spu_frsqest
#define FREST spu_frest
#define FI spu_fi
#include "c690_soa_kernels.inc"
#undef K
#undef FS
#undef FM
#undef FMA
#undef FMS
#undef FNMS

#define K(n) soa1_##n
#define FS hw_fs
#define FM hw_fm
#define FMA hw_fma
#define FMS hw_fms
#define FNMS hw_fnms
#include "c690_soa_kernels.inc"
#undef K
#undef FS
#undef FM
#undef FMA
#undef FMS
#undef FNMS
#undef FRSQEST
#undef FREST
#undef FI

#define K(n) soa2_##n
#define FS ap_fs
#define FM ap_fm
#define FMA ap_fma
#define FMS ap_fms
#define FNMS ap_fnms
#define FRSQEST ap_frsqest
#define FREST ap_frest
#define FI ap_fi
#include "c690_soa_kernels.inc"

#define C690_IMAGE_ID 6

static unsigned long long s_native, s_fallback, s_shadow, s_mismatch;

int g_c690_soa_force = -1;   /* offline gates: >= 0 overrides the key */

static int soa_mode(void)
{
    static int s_env = -1;
    if (__builtin_expect(g_c690_soa_force >= 0, 0)) return g_c690_soa_force;
    if (s_env < 0) {
        const char* e = getenv("PS3RECOMP_C690_SOA");
        s_env = e ? atoi(e) : 0;
    }
#ifdef C690_SOA_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_C690_SOA);
#endif
    return s_env;
}

/* Run the kernel for lsa on ls with ctx's argument registers. 0 = not a
 * layout the kernels cover (the caller runs the lifted body). */
static int soa_run(const spu_context* ctx, uint8_t* ls, uint32_t lsa, int variant)
{
#define R(n) (ctx->gpr[n]._u32[0])
#define DISPATCH(call) do { switch (variant) { \
        case 0: soa0_##call; break; case 1: soa1_##call; break; \
        case 2: soa2_##call; break; default: return 0; } } while (0)
    switch (lsa) {
    case 0xFA20u:
        if (R(4) & 0xFu) return 0;
        DISPATCH(clear(ls, R(3), R(4)));
        return 1;
    case 0xFCB8u:
        if (R(4) & 0xFu) return 0;
        DISPATCH(normalize(ls, R(3), R(4)));
        return 1;
    case 0xFA58u:
        if (((R(3) | R(4)) & 0xFu) || (R(7) & 7u) || R(6) > R(5)) return 0;
        DISPATCH(normals(ls, R(3), R(4), R(5), R(6), R(7), soa_sh(ctx->gpr[8], 0)));
        return 1;
    case 0xFDA0u:
        if (((R(3) | R(4) | R(5)) & 0xFu) || (R(8) & 7u) || R(7) > R(6)) return 0;
        DISPATCH(tangents(ls, R(3), R(4), R(5), R(6), R(7), R(8), soa_sh(ctx->gpr[9], 0)));
        return 1;
    }
    return 0;
#undef DISPATCH
#undef R
}

int c690_soa_run_ls(const struct spu_context* ctx, uint8_t* ls, uint32_t lsa, int variant)
{
    return soa_run(ctx, ls, lsa, variant);
}

static void soa_report(const char* tag)
{
    fprintf(stderr, "[c690-soa] %s native=%llu fallback=%llu shadow=%llu mismatch=%llu\n",
            tag, s_native, s_fallback, s_shadow, s_mismatch);
    fflush(stderr);
}

static void soa_shadow(spu_context* ctx, uint32_t lsa, int variant, void (*body)(spu_context*))
{
    static SPU_THREAD_LOCAL uint8_t* t_ls;
    if (!t_ls && !(t_ls = (uint8_t*)malloc(SPU_LS_SIZE))) { body(ctx); return; }
    memcpy(t_ls, ctx->ls, SPU_LS_SIZE);
    const uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
    const int ran = soa_run(ctx, t_ls, lsa, variant);
    body(ctx);
    if (!ran) { __atomic_fetch_add(&s_fallback, 1ull, __ATOMIC_RELAXED); return; }
    /* The callee's frame below sp is dead and not reproduced. */
    const uint32_t dead = sp >= 0x60u ? sp - 0x60u : 0;
    size_t bad = 0, first = 0;
    if (memcmp(t_ls, ctx->ls, dead) || memcmp(t_ls + sp, ctx->ls + sp, SPU_LS_SIZE - sp))
        for (size_t i = 0; i < SPU_LS_SIZE; i++)
            if ((i < dead || i >= sp) && t_ls[i] != ctx->ls[i]) { if (!bad++) first = i; }
    const unsigned long long n = __atomic_fetch_add(&s_shadow, 1ull, __ATOMIC_RELAXED) + 1ull;
    if (bad) {
        const unsigned long long m = __atomic_fetch_add(&s_mismatch, 1ull, __ATOMIC_RELAXED);
        if (m < 8) {
            const size_t b = first & ~(size_t)15;
            fprintf(stderr, "[c690-soa] SHADOW MISMATCH lsa=0x%05X variant=%d bytes=%zu first=0x%05zX native ",
                    lsa, variant, bad, first);
            for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", t_ls[b + i]);
            fprintf(stderr, " lifted ");
            for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", ctx->ls[b + i]);
            fprintf(stderr, "\n");
        }
    }
    if (!(n & 0xFFFull)) soa_report("shadow");
}

int c690_soa_try(spu_context* ctx, uint32_t lsa, int variant, void (*body)(spu_context*))
{
    const int mode = soa_mode();
    if (__builtin_expect(!mode, 1)) return 0;
    if (!ctx || ctx->image_id != C690_IMAGE_ID || !ctx->ls) return 0;
    if (mode == 2) { soa_shadow(ctx, lsa, variant, body); return 1; }
    if (!soa_run(ctx, ctx->ls, lsa, variant)) {
        __atomic_fetch_add(&s_fallback, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_fetch_add(&s_native, 1ull, __ATOMIC_RELAXED);
    /* What the lifted body's terminal SPU_RET publishes. */
    ctx->pc = ctx->gpr[0]._u32[0];
    if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}

__attribute__((destructor)) static void soa_dtor(void)
{
    if (s_native || s_fallback || s_shadow) soa_report("exit");
}

#else  /* no FMA/SSSE3: the lifted body always runs */

int c690_soa_try(spu_context* ctx, uint32_t lsa, int variant, void (*body)(spu_context*))
{
    (void)ctx; (void)lsa; (void)variant; (void)body;
    return 0;
}
int c690_soa_run_ls(const struct spu_context* ctx, uint8_t* ls, uint32_t lsa, int variant)
{
    (void)ctx; (void)ls; (void)lsa; (void)variant;
    return 0;
}

#endif
