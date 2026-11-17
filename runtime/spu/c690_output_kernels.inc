/*
 * Native AVX2/SSE4.1 kernels for c690 Edge output writers 0x7998 and 0x8160.
 *
 * Included by c690_native_soa.c. Both functions are entered from 0x7AC0 / 0x7B28
 * (and their sibling blocks) and exit via spu_indirect_branch (setting ctx->pc
 * and mutating ctx->gpr[...]).
 */

SOA_INL __m128i out_ld(const uint8_t* ls, uint32_t addr, __m128i bswap)
{
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (addr & (SPU_LS_MASK & ~0xFu)))), bswap);
}

SOA_INL void out_st(uint8_t* ls, uint32_t addr, __m128i v, __m128i bswap)
{
    _mm_storeu_si128((__m128i*)(ls + (addr & (SPU_LS_MASK & ~0xFu))), _mm_shuffle_epi8(v, bswap));
}

SOA_INL __m128i out_selb(__m128i a, __m128i b, __m128i mask)
{
    return _mm_or_si128(_mm_and_si128(b, mask), _mm_andnot_si128(mask, a));
}

SOA_INL __m128i out_shufb(__m128i A, __m128i B, __m128i C)
{
    const __m128i idx = _mm_xor_si128(_mm_and_si128(C, _mm_set1_epi8((char)0x8F)), _mm_set1_epi8(3));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), _mm_slli_epi16(C, 3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
    return _mm_or_si128(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F))));
}

SOA_INL __m256i out_cflts256_142(__m256i x)
{
    const __m256i v = _mm256_cvttps_epi32(_mm256_mul_ps(_mm256_castsi256_ps(x), _mm256_set1_ps(2147483648.0f)));
    const __m256i neg = _mm256_srai_epi32(x, 31);
    const __m256i fix = _mm256_andnot_si256(neg, _mm256_cmpeq_epi32(v, _mm256_set1_epi32((int)0x80000000u)));
    return _mm256_xor_si256(v, fix);
}

static uint32_t soa_out_7998(spu_context* ctx, uint8_t* ls)
{
    const __m128i bswap = soa_bswap();
#define LDG(n) _mm_loadu_si128((const __m128i*)&ctx->gpr[n])
#define STG(n, v) _mm_storeu_si128((__m128i*)&ctx->gpr[n], (v))

    const __m128i sr0  = LDG(0),  sr2  = LDG(2),  sr3  = LDG(3),  sr5  = LDG(5);
    const __m128i sr13 = LDG(13), sr15 = LDG(15), sr16 = LDG(16), sr18 = LDG(18);
    const __m128i sr20 = LDG(20), sr21 = LDG(21), sr22 = LDG(22), sr24 = LDG(24);
    const __m128i sr32 = LDG(32), sr33 = LDG(33), sr36 = LDG(36), sr37 = LDG(37);
    const __m128i sr39 = LDG(39), sr40 = LDG(40), sr41 = LDG(41), sr43 = LDG(43);
    const __m128i sr45 = LDG(45), sr48 = LDG(48), sr50 = LDG(50), sr58 = LDG(58);
    const __m128i sr59 = LDG(59), sr60 = LDG(60), sr61 = LDG(61), sr62 = LDG(62);
    const __m128i sr64 = LDG(64);

    __m128i sr4  = LDG(4),  sr25 = LDG(25), sr26 = LDG(26), sr27 = LDG(27);
    __m128i sr28 = LDG(28), sr31 = LDG(31), sr34 = LDG(34), sr35 = LDG(35);
    __m128i sr38 = LDG(38), sr42 = LDG(42), sr44 = LDG(44), sr46 = LDG(46);
    __m128i sr47 = LDG(47), sr49 = LDG(49), sr51 = LDG(51), sr52 = LDG(52);
    __m128i sr53 = LDG(53), sr54 = LDG(54), sr55 = LDG(55), sr56 = LDG(56);
    __m128i sr57 = LDG(57), sr63 = LDG(63), sr65 = LDG(65), sr66 = LDG(66);
    __m128i sr67 = LDG(67), sr68 = LDG(68), sr69 = LDG(69), sr70 = LDG(70);
    __m128i sr71 = LDG(71);

    const uint32_t b3  = (uint32_t)_mm_cvtsi128_si32(sr3);
    const uint32_t b4  = (uint32_t)_mm_cvtsi128_si32(sr4);
    const uint32_t b32 = (uint32_t)_mm_cvtsi128_si32(sr32);
    const uint32_t b37 = (uint32_t)_mm_cvtsi128_si32(sr37);
    const uint32_t b39 = (uint32_t)_mm_cvtsi128_si32(sr39);
    const uint32_t b40 = (uint32_t)_mm_cvtsi128_si32(sr40);
    const uint32_t b41 = (uint32_t)_mm_cvtsi128_si32(sr41);
    const uint32_t b43 = (uint32_t)_mm_cvtsi128_si32(sr43);
    const uint32_t b45 = (uint32_t)_mm_cvtsi128_si32(sr45);
    const uint32_t o18 = (uint32_t)_mm_cvtsi128_si32(sr18);
    const uint32_t o20 = (uint32_t)_mm_cvtsi128_si32(sr20);
    const uint32_t o21 = (uint32_t)_mm_cvtsi128_si32(sr21);
    const uint32_t r48 = (uint32_t)_mm_cvtsi128_si32(sr48);

    const __m128i c_neg1 = _mm_set1_epi32(-1);
    uint32_t exit_pc;

    for (;;) {
        const uint32_t a52 = (uint32_t)_mm_cvtsi128_si32(sr52);
        const uint32_t a31 = (uint32_t)_mm_cvtsi128_si32(sr31);
        const uint32_t a26 = (uint32_t)_mm_cvtsi128_si32(sr26);

        sr44 = _mm_add_epi32(sr25, sr15);
        sr47 = out_ld(ls, a52 + o18, bswap);
        sr57 = _mm_cmpeq_epi32(sr22, c_neg1);
        sr49 = out_ld(ls, a52 + o21, bswap);
        sr51 = _mm_add_epi32(sr44, sr24);
        sr63 = out_ld(ls, a52 + o20, bswap);
        sr65 = out_ld(ls, a52, bswap);
        sr54 = out_shufb(sr54, sr54, sr33);
        sr66 = out_selb(sr4, sr41, sr50);
        sr53 = out_shufb(sr53, sr53, sr33);
        sr67 = out_selb(sr4, sr39, sr50);
        sr68 = out_ld(ls, b41 + a31, bswap);
        sr69 = out_selb(sr4, sr32, sr50);
        sr70 = out_ld(ls, b32 + a31, bswap);
        sr52 = _mm_add_epi32(sr2, sr52);
        sr49 = out_shufb(sr49, sr47, sr51);
        sr38 = _mm_add_epi32(sr38, c_neg1);
        sr44 = out_shufb(sr65, sr63, sr44);
        sr51 = out_selb(sr58, sr56, sr27);
        sr63 = out_ld(ls, b39 + a31, bswap);
        sr47 = out_selb(sr60, sr55, sr27);
        sr65 = out_ld(ls, b4 + a31, bswap);
        sr71 = _mm_srli_epi32(sr49, 16);
        out_st(ls, (uint32_t)_mm_cvtsi128_si32(sr66) + a26, sr46, bswap);
        sr66 = _mm_srli_epi32(sr44, 16);
        out_st(ls, (uint32_t)_mm_cvtsi128_si32(sr69) + a26, sr35, bswap);
        sr46 = out_selb(sr68, sr56, sr36);
        sr56 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(sr49), bswap);
        sr35 = out_selb(sr70, sr55, sr36);
        sr55 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(sr44), bswap);
        sr49 = out_selb(sr62, sr54, sr27);
        out_st(ls, (uint32_t)_mm_cvtsi128_si32(sr67) + a26, sr42, bswap);
        sr44 = out_selb(sr64, sr53, sr27);
        out_st(ls, b4 + a26, sr28, bswap);
        sr42 = out_selb(sr63, sr54, sr36);
        sr54 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(sr71), bswap);
        sr28 = out_selb(sr65, sr53, sr36);
        sr53 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(sr66), bswap);
        sr26 = sr31;
        sr56 = out_shufb(sr56, sr56, sr33);
        sr31 = sr34;
        sr55 = out_shufb(sr55, sr55, sr33);
        sr34 = _mm_add_epi32(sr34, sr16);

        if (_mm_cvtsi128_si32(sr38) != 0) {
            if ((r48 & SPU_LS_MASK) == 0x7998u) continue;
            exit_pc = r48;
            break;
        }

        const uint32_t a26_tail = (uint32_t)_mm_cvtsi128_si32(sr26);
        sr25 = out_selb(sr13, sr0, sr57);
        out_st(ls, b40 + a26_tail, sr44, bswap);
        sr27 = _mm_cmpeq_epi32(sr59, _mm_set1_epi32(2));
        out_st(ls, b4 + a26_tail, sr28, bswap);
        sr4 = _mm_add_epi32(sr5, sr4);
        const uint32_t r25 = (uint32_t)_mm_cvtsi128_si32(sr25);
        if (_mm_cvtsi128_si32(sr61) != 0) {
            if ((r25 & SPU_LS_MASK) == 0x7998u) continue;
            exit_pc = r25;
            break;
        }

        sr28 = _mm_cmpeq_epi32(sr59, _mm_set1_epi32(3));
        out_st(ls, b43 + a26_tail, sr47, bswap);
        out_st(ls, b32 + a26_tail, sr35, bswap);
        if (_mm_cvtsi128_si32(sr27) != 0) {
            if ((r25 & SPU_LS_MASK) == 0x7998u) continue;
            exit_pc = r25;
            break;
        }

        out_st(ls, b45 + a26_tail, sr49, bswap);
        out_st(ls, b39 + a26_tail, sr42, bswap);
        if (_mm_cvtsi128_si32(sr28) != 0) {
            if ((r25 & SPU_LS_MASK) == 0x7998u) continue;
            exit_pc = r25;
            break;
        }

        out_st(ls, b37 + a26_tail, sr51, bswap);
        out_st(ls, b41 + a26_tail, sr46, bswap);
        if ((r25 & SPU_LS_MASK) == 0x7998u) continue;
        exit_pc = r25;
        break;
    }

    STG(4, sr4);   STG(25, sr25); STG(26, sr26); STG(27, sr27); STG(28, sr28);
    STG(31, sr31); STG(34, sr34); STG(35, sr35); STG(38, sr38); STG(42, sr42);
    STG(44, sr44); STG(46, sr46); STG(47, sr47); STG(49, sr49); STG(51, sr51);
    STG(52, sr52); STG(53, sr53); STG(54, sr54); STG(55, sr55); STG(56, sr56);
    STG(57, sr57); STG(63, sr63); STG(65, sr65); STG(66, sr66); STG(67, sr67);
    STG(68, sr68); STG(69, sr69); STG(70, sr70); STG(71, sr71);
    return exit_pc;

#undef LDG
#undef STG
}

SOA_INL __m256 out_fma256_v(__m256 a, __m256 b, __m256 c, int variant)
{
    if (variant == 2) return _mm256_fmadd_ps(a, b, c);
    if (variant == 1) {
        const __m256i ea = _mm256_and_si256(_mm256_castps_si256(a), _mm256_set1_epi32(0x7F800000));
        const __m256 fa = _mm256_andnot_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(ea, _mm256_setzero_si256())), a);
        return _mm256_fmadd_ps(fa, b, c);
    }
    const __m128i lo = soa_m(spu_fma(soa_u(_mm_castps_si128(_mm256_castps256_ps128(a))),
                                     soa_u(_mm_castps_si128(_mm256_castps256_ps128(b))),
                                     soa_u(_mm_castps_si128(_mm256_castps256_ps128(c)))));
    const __m128i hi = soa_m(spu_fma(soa_u(_mm_castps_si128(_mm256_extractf128_ps(a, 1))),
                                     soa_u(_mm_castps_si128(_mm256_extractf128_ps(b, 1))),
                                     soa_u(_mm_castps_si128(_mm256_extractf128_ps(c, 1)))));
    return _mm256_set_m128(_mm_castsi128_ps(hi), _mm_castsi128_ps(lo));
}

SOA_INL uint32_t out_8160_core(spu_context* ctx, uint8_t* ls, int variant)
{
    const __m128i bswap = soa_bswap();
#define LDG(n) _mm_loadu_si128((const __m128i*)&ctx->gpr[n])
#define STG(n, v) _mm_storeu_si128((__m128i*)&ctx->gpr[n], (v))

    const __m128i sr3  = LDG(3),  sr5  = LDG(5),  sr9  = LDG(9),  sr10 = LDG(10);
    const __m128i sr18 = LDG(18), sr19 = LDG(19), sr21 = LDG(21), sr24 = LDG(24);
    const __m128i sr25 = LDG(25), sr26 = LDG(26), sr35 = LDG(35);
    const __m128i sr36 = LDG(36), sr37 = LDG(37), sr39 = LDG(39), sr41 = LDG(41);
    const __m128i sr43 = LDG(43), sr44 = LDG(44), sr45 = LDG(45), sr47 = LDG(47);
    const __m128i sr48 = LDG(48), sr49 = LDG(49), sr50 = LDG(50), sr51 = LDG(51);
    const __m128i sr52 = LDG(52), sr53 = LDG(53), sr57 = LDG(57), sr62 = LDG(62);
    const __m128i sr63 = LDG(63), sr64 = LDG(64), sr65 = LDG(65), sr66 = LDG(66);
    const __m128i sr75 = LDG(75), sr76 = LDG(76), sr77 = LDG(77);

    __m128i sr4  = LDG(4),  sr28 = LDG(28), sr29 = LDG(29), sr30 = LDG(30), sr31 = LDG(31);
    __m128i sr32 = LDG(32), sr33 = LDG(33), sr34 = LDG(34), sr38 = LDG(38);
    __m128i sr40 = LDG(40), sr42 = LDG(42), sr46 = LDG(46), sr54 = LDG(54);
    __m128i sr56 = LDG(56), sr58 = LDG(58), sr60 = LDG(60), sr67 = LDG(67);
    __m128i sr68 = LDG(68), sr69 = LDG(69), sr70 = LDG(70), sr71 = LDG(71);
    __m128i sr72 = LDG(72), sr73 = LDG(73), sr74 = LDG(74), sr78 = LDG(78);
    __m128i sr79 = LDG(79);

    const uint32_t b3  = (uint32_t)_mm_cvtsi128_si32(sr3);
    const uint32_t b4  = (uint32_t)_mm_cvtsi128_si32(sr4);
    const uint32_t b31 = (uint32_t)_mm_cvtsi128_si32(sr31);
    const uint32_t b37 = (uint32_t)_mm_cvtsi128_si32(sr37);
    const uint32_t b39 = (uint32_t)_mm_cvtsi128_si32(sr39);
    const uint32_t b41 = (uint32_t)_mm_cvtsi128_si32(sr41);
    const uint32_t b43 = (uint32_t)_mm_cvtsi128_si32(sr43);
    const uint32_t b45 = (uint32_t)_mm_cvtsi128_si32(sr45);
    const uint32_t b47 = (uint32_t)_mm_cvtsi128_si32(sr47);
    const uint32_t b62 = (uint32_t)_mm_cvtsi128_si32(sr62);
    const uint32_t b64 = (uint32_t)_mm_cvtsi128_si32(sr64);
    const uint32_t b65 = (uint32_t)_mm_cvtsi128_si32(sr65);
    const uint32_t o18 = (uint32_t)_mm_cvtsi128_si32(sr18);
    const uint32_t o19 = (uint32_t)_mm_cvtsi128_si32(sr19);
    const uint32_t o26 = (uint32_t)_mm_cvtsi128_si32(sr26);
    const uint32_t r49 = (uint32_t)_mm_cvtsi128_si32(sr49);
    const uint32_t r77 = (uint32_t)_mm_cvtsi128_si32(sr77);

    const __m128i rot_sh = _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), sr25), _mm_set1_epi32(0x3F));
    const __m256i rot_sh256 = _mm256_set_m128i(rot_sh, rot_sh);
    const __m256 scale256 = _mm256_set_m128(_mm_castsi128_ps(sr9), _mm_castsi128_ps(sr9));
    const __m256 off256   = _mm256_set_m128(_mm_castsi128_ps(sr10), _mm_castsi128_ps(sr10));
    const __m128i c_neg1  = _mm_set1_epi32(-1);
    uint32_t exit_pc;

    for (;;) {
        const uint32_t a28 = (uint32_t)_mm_cvtsi128_si32(sr28);
        const uint32_t a30 = (uint32_t)_mm_cvtsi128_si32(sr30);

        const __m256i r68_70 = _mm256_srlv_epi32(_mm256_set_m128i(sr70, sr68), rot_sh256);
        const __m256i r67_69 = _mm256_srlv_epi32(_mm256_set_m128i(sr69, sr67), rot_sh256);
        const __m128i r68 = _mm256_castsi256_si128(r68_70);
        const __m128i r56_rot = _mm256_extracti128_si256(r68_70, 1);
        const __m128i r67 = _mm256_castsi256_si128(r67_69);
        const __m128i r69 = _mm256_extracti128_si256(r67_69, 1);

        const __m128i idx0 = out_shufb(sr54, sr56, sr53);
        const __m128i idx1 = out_shufb(sr58, sr60, sr57);

        sr60 = out_selb(sr46, sr74, sr35);
        sr46 = out_ld(ls, b41 + a30, bswap);
        sr70 = out_selb(sr42, sr73, sr35);
        sr42 = out_ld(ls, b37 + a30, bswap);

        const __m128i i2 = _mm_srli_epi32(idx0, 16);
        const __m128i v0 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(idx0), bswap);
        const __m128i i3 = _mm_srli_epi32(idx1, 16);
        const __m128i v1 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(idx1), bswap);

        sr78 = out_selb(sr34, sr72, sr35);
        sr34 = out_ld(ls, b31 + a30, bswap);
        sr38 = _mm_add_epi32(sr38, sr21);
        const __m128i t72 = out_shufb(r68, r56_rot, sr52);
        sr79 = out_selb(sr29, sr71, sr35);
        const __m128i v2 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(i2), bswap);
        const __m128i v3 = out_ld(ls, b3 + (uint32_t)_mm_cvtsi128_si32(i3), bswap);
        sr29 = out_ld(ls, b4 + a30, bswap);

        const __m128i t56 = out_shufb(r68, r56_rot, sr66);
        out_st(ls, b62 + a28, sr60, bswap);
        sr40 = _mm_add_epi32(sr40, c_neg1);
        out_st(ls, b64 + a28, sr70, bswap);

        const __m256 fma01 = out_fma256_v(_mm256_set_m128(_mm_castsi128_ps(v1), _mm_castsi128_ps(v0)),
                                          scale256, off256, variant);
        const __m256 fma23 = out_fma256_v(_mm256_set_m128(_mm_castsi128_ps(v3), _mm_castsi128_ps(v2)),
                                          scale256, off256, variant);
        const __m256i cfl01 = out_cflts256_142(_mm256_castps_si256(fma01));
        const __m256i cfl23 = out_cflts256_142(_mm256_castps_si256(fma23));
        sr68 = _mm256_castsi256_si128(cfl01);
        sr70 = _mm256_extracti128_si256(cfl01, 1);
        sr67 = _mm256_castsi256_si128(cfl23);
        sr69 = _mm256_extracti128_si256(cfl23, 1);

        const __m128i t71 = out_shufb(r67, r69, sr66);
        const __m128i t67 = out_shufb(r67, r69, sr52);
        sr72 = out_selb(t72, t56, sr63);
        out_st(ls, b65 + a28, sr78, bswap);

        const uint32_t a38 = (uint32_t)_mm_cvtsi128_si32(sr38);
        sr54 = out_ld(ls, a38, bswap);
        sr56 = out_ld(ls, a38 + o18, bswap);
        sr71 = out_selb(t67, t71, sr63);
        sr58 = out_ld(ls, a38 + o19, bswap);
        sr60 = out_ld(ls, a38 + o26, bswap);

        out_st(ls, b4 + a28, sr79, bswap);
        sr28 = sr30;
        sr73 = _mm_shuffle_epi32(sr71, _MM_SHUFFLE(0, 3, 2, 1));
        sr30 = sr33;
        sr74 = _mm_shuffle_epi32(sr72, _MM_SHUFFLE(0, 3, 2, 1));
        sr33 = _mm_add_epi32(sr24, sr33);

        if (_mm_cvtsi128_si32(sr40) != 0) {
            if ((r49 & SPU_LS_MASK) == 0x8160u) continue;
            exit_pc = r49;
            break;
        }

        const uint32_t a28_tail = (uint32_t)_mm_cvtsi128_si32(sr28);
        sr30 = out_selb(sr44, sr71, sr36);
        sr29 = out_selb(sr29, sr71, sr35);
        sr32 = out_selb(sr48, sr72, sr36);
        out_st(ls, b39 + a28_tail, sr30, bswap);
        sr30 = out_selb(sr34, sr72, sr35);
        out_st(ls, b4 + a28_tail, sr29, bswap);
        sr4 = _mm_add_epi32(sr5, sr4);
        if (_mm_cvtsi128_si32(sr76) != 0) {
            if ((r77 & SPU_LS_MASK) == 0x8160u) continue;
            exit_pc = r77;
            break;
        }

        sr29 = _mm_cmpeq_epi32(sr75, _mm_set1_epi32(2));
        out_st(ls, b43 + a28_tail, sr32, bswap);
        sr32 = out_selb(sr50, sr73, sr36);
        out_st(ls, b31 + a28_tail, sr30, bswap);
        sr30 = out_selb(sr42, sr73, sr35);
        if (_mm_cvtsi128_si32(sr29) != 0) {
            if ((r77 & SPU_LS_MASK) == 0x8160u) continue;
            exit_pc = r77;
            break;
        }

        sr29 = _mm_cmpeq_epi32(sr75, _mm_set1_epi32(3));
        out_st(ls, b45 + a28_tail, sr32, bswap);
        sr31 = out_selb(sr51, sr74, sr36);
        out_st(ls, b37 + a28_tail, sr30, bswap);
        sr30 = out_selb(sr46, sr74, sr35);
        if (_mm_cvtsi128_si32(sr29) != 0) {
            if ((r77 & SPU_LS_MASK) == 0x8160u) continue;
            exit_pc = r77;
            break;
        }

        out_st(ls, b47 + a28_tail, sr31, bswap);
        out_st(ls, b41 + a28_tail, sr30, bswap);
        if ((r77 & SPU_LS_MASK) == 0x8160u) continue;
        exit_pc = r77;
        break;
    }

    STG(4, sr4);   STG(28, sr28); STG(29, sr29); STG(30, sr30); STG(31, sr31);
    STG(32, sr32); STG(33, sr33); STG(34, sr34); STG(38, sr38); STG(40, sr40);
    STG(42, sr42); STG(46, sr46); STG(54, sr54); STG(56, sr56); STG(58, sr58);
    STG(60, sr60); STG(67, sr67); STG(68, sr68); STG(69, sr69); STG(70, sr70);
    STG(71, sr71); STG(72, sr72); STG(73, sr73); STG(74, sr74); STG(78, sr78);
    STG(79, sr79);
    return exit_pc;

#undef LDG
#undef STG
}

static uint32_t soa_out_8160_v0(spu_context* ctx, uint8_t* ls) { return out_8160_core(ctx, ls, 0); }
static uint32_t soa_out_8160_v1(spu_context* ctx, uint8_t* ls) { return out_8160_core(ctx, ls, 1); }
static uint32_t soa_out_8160_v2(spu_context* ctx, uint8_t* ls) { return out_8160_core(ctx, ls, 2); }
