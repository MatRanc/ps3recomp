#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__clang__)
#pragma clang attribute push (__attribute__((target("avx2,fma"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC target("avx2,fma")
#endif
#include "job_c690_native.h"
#include "spu_helpers.h"
#include <immintrin.h>

#define BSWAP_MASK _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)

static inline __attribute__((always_inline)) __m128i native_ls_read(const uint8_t* ls, uint32_t addr, __m128i bswap) {
    __m128i v = _mm_loadu_si128((const __m128i*)&ls[addr & 0x3FFF0]);
    return _mm_shuffle_epi8(v, bswap);
}

static inline __attribute__((always_inline)) void native_ls_write(uint8_t* ls, uint32_t addr, __m128i v, __m128i bswap) {
    _mm_storeu_si128((__m128i*)&ls[addr & 0x3FFF0], _mm_shuffle_epi8(v, bswap));
}

static inline __attribute__((always_inline)) __m128i native_selb(__m128i a, __m128i b, __m128i mask) {
    return _mm_or_si128(_mm_and_si128(b, mask), _mm_andnot_si128(mask, a));
}

static inline __attribute__((always_inline)) __m128i native_shufb(__m128i A, __m128i B, __m128i C) {
    const __m128i idx = _mm_xor_si128(_mm_and_si128(C, _mm_set1_epi8((char)0x8F)), _mm_set1_epi8(3));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), _mm_slli_epi16(C, 3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
    r = _mm_or_si128(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F))));
    return r;
}

static inline __attribute__((always_inline)) __m256 native_fma_fast256(__m256 a, __m256 b_f, __m256 c_f) {
    const __m256i ea = _mm256_and_si256(_mm256_castps_si256(a), _mm256_set1_epi32(0x7F800000));
    __m256 fa = _mm256_andnot_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(ea, _mm256_setzero_si256())), a);
    return _mm256_fmadd_ps(fa, b_f, c_f);
}

static inline __attribute__((always_inline)) __m256i native_cflts256(__m256i x, int i8) {
    const __m256i be = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i m = _mm256_or_si256(_mm256_and_si256(x, _mm256_set1_epi32(0x7FFFFF)), _mm256_set1_epi32(0x800000));
    const __m256i sh = _mm256_add_epi32(be, _mm256_set1_epi32(23 - (i8 & 0xFF)));
    const __m256i lsh = _mm256_max_epi32(sh, _mm256_setzero_si256());
    const __m256i rsh = _mm256_max_epi32(_mm256_sub_epi32(_mm256_setzero_si256(), sh), _mm256_setzero_si256());
    __m256i v = _mm256_blendv_epi8(_mm256_srlv_epi32(m, rsh), _mm256_sllv_epi32(m, lsh),
                                   _mm256_cmpgt_epi32(sh, _mm256_set1_epi32(-1)));
    const __m256i neg = _mm256_srai_epi32(x, 31), isz = _mm256_cmpeq_epi32(be, _mm256_setzero_si256());
    v = _mm256_sub_epi32(_mm256_xor_si256(v, neg), neg);
    const __m256i sat = _mm256_xor_si256(_mm256_set1_epi32(0x7FFFFFFF), neg);
    v = _mm256_blendv_epi8(v, sat, _mm256_cmpgt_epi32(sh, _mm256_set1_epi32(7)));
    return _mm256_andnot_si256(isz, v);
}

#define ONE_ITER_BODY() do { \
    sr44 = native_selb(sr44, sr60, sr37); \
    sr46 = native_ls_read(ls, a54 + 0x0, bswap); \
    __m128i raw57 = native_ls_read(ls, a54 + off_21, bswap); \
    __m128i raw60 = native_ls_read(ls, a54 + off_22, bswap); \
    sr65 = _mm_and_si128(sr30, c_3); \
    __m128i raw61 = native_ls_read(ls, a54 + off_19, bswap); \
    native_ls_write(ls, b_68 + a27, sr48, bswap); \
    native_ls_write(ls, b_62 + a27, sr44, bswap); \
    sr69 = _mm_cmpeq_epi32(sr65, c_1); \
    __m256 in0 = _mm256_set_m128(_mm_castsi128_ps(sr59), _mm_castsi128_ps(sr57)); \
    __m256 in1 = _mm256_set_m128(_mm_castsi128_ps(sr58), _mm_castsi128_ps(sr56)); \
    __m256 fma0 = native_fma_fast256(in0, scale_256, offset_256); \
    __m256 fma1 = native_fma_fast256(in1, scale_256, offset_256); \
    __m256i cfl0 = native_cflts256(_mm256_castps_si256(fma0), 157); \
    __m256i cfl1 = native_cflts256(_mm256_castps_si256(fma1), 157); \
    __m256i shf0 = _mm256_or_si256(_mm256_shuffle_epi8(cfl0, shuf_idx256), shuf_or256); \
    __m256i shf1 = _mm256_or_si256(_mm256_shuffle_epi8(cfl1, shuf_idx256), shuf_or256); \
    sr49 = _mm256_castsi256_si128(shf0); \
    sr61 = _mm256_extracti128_si256(shf0, 1); \
    sr73 = _mm256_castsi256_si128(shf1); \
    sr60 = _mm256_extracti128_si256(shf1, 1); \
    sr58 = native_ls_read(ls, b_43 + a32, bswap); \
    sr44 = native_ls_read(ls, b_40 + a32, bswap); \
    sr46 = native_shufb(sr46, raw57, sr26); \
    sr54 = _mm_add_epi32(sr54, sr2); \
    sr70 = native_shufb(raw60, raw61, sr55); \
    native_ls_write(ls, b_63 + a27, sr36, bswap); \
    native_ls_write(ls, b_4 + a27, sr29, bswap); \
    sr29 = _mm_srli_epi32(sr46, 16); \
    sr36 = native_ls_read(ls, b_33 + a32, bswap); \
    sr71 = _mm_srli_epi32(sr70, 16); \
    sr72 = native_ls_read(ls, b_4 + a32, bswap); \
    sr27 = sr32; \
    sr32 = sr35; \
    sr39 = _mm_add_epi32(sr39, c_neg1); \
    sr57 = native_ls_read(ls, b_3 + _mm_cvtsi128_si32(sr46), bswap); \
    sr35 = _mm_add_epi32(sr35, sr17); \
    sr36 = native_selb(sr36, sr49, sr37); \
    sr48 = native_selb(sr58, sr61, sr37); \
    sr59 = native_ls_read(ls, b_3 + _mm_cvtsi128_si32(sr70), bswap); \
    sr49 = native_selb(sr64, sr49, sr28); \
    sr56 = native_ls_read(ls, b_3 + _mm_cvtsi128_si32(sr29), bswap); \
    sr46 = native_selb(sr66, sr73, sr28); \
    sr58 = native_ls_read(ls, b_3 + _mm_cvtsi128_si32(sr71), bswap); \
    sr29 = native_selb(sr72, sr73, sr37); \
    a54 += step_54; \
    a27 = a32; \
    a32 = a35; \
    a35 += step_35; \
} while (0)

void native_spu_func_00007C18(spu_context* ctx) {
    const __m128i bswap = BSWAP_MASK;
    uint8_t* ls = ctx->ls;

    __m128i sr0  = _mm_loadu_si128((const __m128i*)&ctx->gpr[0]);
    __m128i sr2  = _mm_loadu_si128((const __m128i*)&ctx->gpr[2]);
    __m128i sr3  = _mm_loadu_si128((const __m128i*)&ctx->gpr[3]);
    __m128i sr4  = _mm_loadu_si128((const __m128i*)&ctx->gpr[4]);
    __m128i sr5  = _mm_loadu_si128((const __m128i*)&ctx->gpr[5]);
    __m128i sr7  = _mm_loadu_si128((const __m128i*)&ctx->gpr[7]);
    __m128i sr9  = _mm_loadu_si128((const __m128i*)&ctx->gpr[9]);
    __m128i sr10 = _mm_loadu_si128((const __m128i*)&ctx->gpr[10]);
    __m128i sr13 = _mm_loadu_si128((const __m128i*)&ctx->gpr[13]);
    __m128i sr17 = _mm_loadu_si128((const __m128i*)&ctx->gpr[17]);
    __m128i sr19 = _mm_loadu_si128((const __m128i*)&ctx->gpr[19]);
    __m128i sr21 = _mm_loadu_si128((const __m128i*)&ctx->gpr[21]);
    __m128i sr22 = _mm_loadu_si128((const __m128i*)&ctx->gpr[22]);
    __m128i sr26 = _mm_loadu_si128((const __m128i*)&ctx->gpr[26]);
    __m128i sr27 = _mm_loadu_si128((const __m128i*)&ctx->gpr[27]);
    __m128i sr28 = _mm_loadu_si128((const __m128i*)&ctx->gpr[28]);
    __m128i sr29 = _mm_loadu_si128((const __m128i*)&ctx->gpr[29]);
    __m128i sr30 = _mm_loadu_si128((const __m128i*)&ctx->gpr[30]);
    __m128i sr31 = _mm_loadu_si128((const __m128i*)&ctx->gpr[31]);
    __m128i sr32 = _mm_loadu_si128((const __m128i*)&ctx->gpr[32]);
    __m128i sr33 = _mm_loadu_si128((const __m128i*)&ctx->gpr[33]);
    __m128i sr34 = _mm_loadu_si128((const __m128i*)&ctx->gpr[34]);
    __m128i sr35 = _mm_loadu_si128((const __m128i*)&ctx->gpr[35]);
    __m128i sr36 = _mm_loadu_si128((const __m128i*)&ctx->gpr[36]);
    __m128i sr37 = _mm_loadu_si128((const __m128i*)&ctx->gpr[37]);
    __m128i sr38 = _mm_loadu_si128((const __m128i*)&ctx->gpr[38]);
    __m128i sr39 = _mm_loadu_si128((const __m128i*)&ctx->gpr[39]);
    __m128i sr40 = _mm_loadu_si128((const __m128i*)&ctx->gpr[40]);
    __m128i sr42 = _mm_loadu_si128((const __m128i*)&ctx->gpr[42]);
    __m128i sr43 = _mm_loadu_si128((const __m128i*)&ctx->gpr[43]);
    __m128i sr44 = _mm_loadu_si128((const __m128i*)&ctx->gpr[44]);
    __m128i sr45 = _mm_loadu_si128((const __m128i*)&ctx->gpr[45]);
    __m128i sr46 = _mm_loadu_si128((const __m128i*)&ctx->gpr[46]);
    __m128i sr47 = _mm_loadu_si128((const __m128i*)&ctx->gpr[47]);
    __m128i sr48 = _mm_loadu_si128((const __m128i*)&ctx->gpr[48]);
    __m128i sr49 = _mm_loadu_si128((const __m128i*)&ctx->gpr[49]);
    __m128i sr50 = _mm_loadu_si128((const __m128i*)&ctx->gpr[50]);
    __m128i sr51 = _mm_loadu_si128((const __m128i*)&ctx->gpr[51]);
    __m128i sr53 = _mm_loadu_si128((const __m128i*)&ctx->gpr[53]);
    __m128i sr54 = _mm_loadu_si128((const __m128i*)&ctx->gpr[54]);
    __m128i sr55 = _mm_loadu_si128((const __m128i*)&ctx->gpr[55]);
    __m128i sr56 = _mm_loadu_si128((const __m128i*)&ctx->gpr[56]);
    __m128i sr57 = _mm_loadu_si128((const __m128i*)&ctx->gpr[57]);
    __m128i sr58 = _mm_loadu_si128((const __m128i*)&ctx->gpr[58]);
    __m128i sr59 = _mm_loadu_si128((const __m128i*)&ctx->gpr[59]);
    __m128i sr60 = _mm_loadu_si128((const __m128i*)&ctx->gpr[60]);
    __m128i sr61 = _mm_loadu_si128((const __m128i*)&ctx->gpr[61]);
    __m128i sr62 = _mm_loadu_si128((const __m128i*)&ctx->gpr[62]);
    __m128i sr63 = _mm_loadu_si128((const __m128i*)&ctx->gpr[63]);
    __m128i sr64 = _mm_loadu_si128((const __m128i*)&ctx->gpr[64]);
    __m128i sr65 = _mm_loadu_si128((const __m128i*)&ctx->gpr[65]);
    __m128i sr66 = _mm_loadu_si128((const __m128i*)&ctx->gpr[66]);
    __m128i sr67 = _mm_loadu_si128((const __m128i*)&ctx->gpr[67]);
    __m128i sr68 = _mm_loadu_si128((const __m128i*)&ctx->gpr[68]);
    __m128i sr69 = _mm_loadu_si128((const __m128i*)&ctx->gpr[69]);
    __m128i sr70 = _mm_loadu_si128((const __m128i*)&ctx->gpr[70]);
    __m128i sr71 = _mm_loadu_si128((const __m128i*)&ctx->gpr[71]);
    __m128i sr72 = _mm_loadu_si128((const __m128i*)&ctx->gpr[72]);
    __m128i sr73 = _mm_loadu_si128((const __m128i*)&ctx->gpr[73]);

    const __m128i c_neg1 = _mm_set1_epi32(-1);
    const __m128i c_3    = _mm_set1_epi32(3);
    const __m128i c_1    = _mm_set1_epi32(1);
    const __m128i c_2    = _mm_set1_epi32(2);

    const __m128 scale_f  = _mm_castsi128_ps(sr9);
    const __m128 offset_f = _mm_castsi128_ps(sr10);
    const __m256 scale_256  = _mm256_set_m128(scale_f, scale_f);
    const __m256 offset_256 = _mm256_set_m128(offset_f, offset_f);

    const __m128i shuf_idx128 = _mm_xor_si128(_mm_and_si128(sr34, _mm_set1_epi8((char)0x8F)), _mm_set1_epi8(3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
    const __m128i shuf_or128  = _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(sr34, 4), _mm_set1_epi8(0x0F)));
    const __m256i shuf_idx256 = _mm256_set_m128i(shuf_idx128, shuf_idx128);
    const __m256i shuf_or256  = _mm256_set_m128i(shuf_or128, shuf_or128);

    uint32_t a54 = _mm_cvtsi128_si32(sr54);
    uint32_t a27 = _mm_cvtsi128_si32(sr27);
    uint32_t a32 = _mm_cvtsi128_si32(sr32);
    uint32_t a35 = _mm_cvtsi128_si32(sr35);
    const uint32_t step_54 = _mm_cvtsi128_si32(sr2);
    const uint32_t step_35 = _mm_cvtsi128_si32(sr17);
    const uint32_t off_21 = _mm_cvtsi128_si32(sr21);
    const uint32_t off_22 = _mm_cvtsi128_si32(sr22);
    const uint32_t off_19 = _mm_cvtsi128_si32(sr19);
    const uint32_t b_68 = _mm_cvtsi128_si32(sr68);
    const uint32_t b_62 = _mm_cvtsi128_si32(sr62);
    const uint32_t b_63 = _mm_cvtsi128_si32(sr63);
    const uint32_t b_4  = _mm_cvtsi128_si32(sr4);
    const uint32_t b_43 = _mm_cvtsi128_si32(sr43);
    const uint32_t b_40 = _mm_cvtsi128_si32(sr40);
    const uint32_t b_33 = _mm_cvtsi128_si32(sr33);
    const uint32_t b_3  = _mm_cvtsi128_si32(sr3);

loc_loop:
    // Process 2 iterations (8 vertices) at once whenever >= 2 iterations remain
    if (_mm_cvtsi128_si32(sr39) >= 2 && (_mm_cvtsi128_si32(sr50) & SPU_LS_MASK) == 0x7C18) {
        ONE_ITER_BODY();
        ONE_ITER_BODY();
        if (_mm_cvtsi128_si32(sr39) != 0 && (_mm_cvtsi128_si32(sr50) & SPU_LS_MASK) == 0x7C18) {
            goto loc_loop;
        }
    } else {
        ONE_ITER_BODY();
        if (_mm_cvtsi128_si32(sr39) != 0 && (_mm_cvtsi128_si32(sr50) & SPU_LS_MASK) == 0x7C18) {
            goto loc_loop;
        }
    }

    if (_mm_cvtsi128_si32(sr39) != 0) {
        goto loc_exit_sr50;
    }

    // Tail remainder stores
    sr26 = native_selb(sr13, sr0, sr67);
    native_ls_write(ls, _mm_cvtsi128_si32(sr42) + a27, sr46, bswap);
    sr7 = _mm_add_epi32(sr7, c_2);
    native_ls_write(ls, b_4 + a27, sr29, bswap);
    sr4 = _mm_add_epi32(sr4, sr5);
    if (_mm_cvtsi128_si32(sr69) != 0 && (_mm_cvtsi128_si32(sr26) & SPU_LS_MASK) == 0x7C18) goto loc_loop;
    if (_mm_cvtsi128_si32(sr69) != 0) goto loc_exit;

    sr29 = _mm_cmpeq_epi32(sr65, c_2);
    native_ls_write(ls, _mm_cvtsi128_si32(sr45) + a27, sr49, bswap);
    sr30 = native_selb(sr51, sr60, sr28);
    native_ls_write(ls, b_33 + a27, sr36, bswap);
    sr31 = native_selb(sr44, sr60, sr37);
    if (_mm_cvtsi128_si32(sr29) != 0 && (_mm_cvtsi128_si32(sr26) & SPU_LS_MASK) == 0x7C18) goto loc_loop;
    if (_mm_cvtsi128_si32(sr29) != 0) goto loc_exit;

    sr29 = _mm_cmpeq_epi32(sr65, c_3);
    native_ls_write(ls, _mm_cvtsi128_si32(sr47) + a27, sr30, bswap);
    sr28 = native_selb(sr53, sr61, sr28);
    native_ls_write(ls, b_40 + a27, sr31, bswap);
    if (_mm_cvtsi128_si32(sr29) != 0 && (_mm_cvtsi128_si32(sr26) & SPU_LS_MASK) == 0x7C18) goto loc_loop;
    if (_mm_cvtsi128_si32(sr29) != 0) goto loc_exit;

    native_ls_write(ls, _mm_cvtsi128_si32(sr38) + a27, sr28, bswap);
    native_ls_write(ls, b_43 + a27, sr48, bswap);
    if ((_mm_cvtsi128_si32(sr26) & SPU_LS_MASK) == 0x7C18) goto loc_loop;

    uint32_t exit_pc = 0;
    exit_pc = _mm_cvtsi128_si32(sr26);
    goto loc_exit_common;

loc_exit_sr50:
    exit_pc = _mm_cvtsi128_si32(sr50);
    goto loc_exit_common;

loc_exit:
    exit_pc = _mm_cvtsi128_si32(sr26);

loc_exit_common:
    _mm_storeu_si128((__m128i*)&ctx->gpr[4], sr4);
    _mm_storeu_si128((__m128i*)&ctx->gpr[7], sr7);
    _mm_storeu_si128((__m128i*)&ctx->gpr[26], sr26);
    _mm_storeu_si128((__m128i*)&ctx->gpr[27], sr27);
    _mm_storeu_si128((__m128i*)&ctx->gpr[28], sr28);
    _mm_storeu_si128((__m128i*)&ctx->gpr[29], sr29);
    _mm_storeu_si128((__m128i*)&ctx->gpr[30], sr30);
    _mm_storeu_si128((__m128i*)&ctx->gpr[31], sr31);
    _mm_storeu_si128((__m128i*)&ctx->gpr[32], sr32);
    _mm_storeu_si128((__m128i*)&ctx->gpr[35], sr35);
    _mm_storeu_si128((__m128i*)&ctx->gpr[36], sr36);
    _mm_storeu_si128((__m128i*)&ctx->gpr[39], sr39);
    _mm_storeu_si128((__m128i*)&ctx->gpr[44], sr44);
    _mm_storeu_si128((__m128i*)&ctx->gpr[46], sr46);
    _mm_storeu_si128((__m128i*)&ctx->gpr[48], sr48);
    _mm_storeu_si128((__m128i*)&ctx->gpr[49], sr49);
    _mm_storeu_si128((__m128i*)&ctx->gpr[54], sr54);
    _mm_storeu_si128((__m128i*)&ctx->gpr[56], sr56);
    _mm_storeu_si128((__m128i*)&ctx->gpr[57], sr57);
    _mm_storeu_si128((__m128i*)&ctx->gpr[58], sr58);
    _mm_storeu_si128((__m128i*)&ctx->gpr[59], sr59);
    _mm_storeu_si128((__m128i*)&ctx->gpr[60], sr60);
    _mm_storeu_si128((__m128i*)&ctx->gpr[61], sr61);
    _mm_storeu_si128((__m128i*)&ctx->gpr[65], sr65);
    _mm_storeu_si128((__m128i*)&ctx->gpr[69], sr69);
    _mm_storeu_si128((__m128i*)&ctx->gpr[70], sr70);
    _mm_storeu_si128((__m128i*)&ctx->gpr[71], sr71);
    _mm_storeu_si128((__m128i*)&ctx->gpr[72], sr72);
    _mm_storeu_si128((__m128i*)&ctx->gpr[73], sr73);

    ctx->pc = exit_pc;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
