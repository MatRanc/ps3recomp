/* The generate-controls-for-insertion helpers (cbd, chd, cwd, cdd, cbx, chx, cwx, cdx)
 * against the forms they replaced, over every insert position 0-15 and random registers.
 *
 *   cc -std=gnu17 -O2 -msse4.1 -Iinclude -Iruntime/spu \
 *      runtime/spu/tests/test_spu_insertion_controls.c -o t && ./t
 *
 * Built without -msse4.1 (or on arm64) it checks the scalar / NEON forms the same way.
 * Each result must equal, bit for bit:
 *   - spu_c*_ref, the previous helper (the base pattern patched in memory, spu_c*_pos);
 *   - a byte loop written from the ISA's definition, in SPU byte order and mapped
 *     through SPU_W (SPU byte P is host byte P ^ 3);
 * and, as shufb's control, insert the preferred slot's byte/halfword/word/doubleword of
 * one register at that position of another. Only the low four bits of the address (rA
 * word 0 plus the immediate or rB word 0) may matter: the other lanes and the upper bits
 * are random. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)s_rng; }
static u128 rv(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rnd(); return r; }

static int s_fail, s_checks;
static void cmp(const char* name, const char* against, int pos, u128 a, u128 b, u128 got, u128 want)
{
    s_checks++;
    if (memcmp(&got, &want, 16) == 0) return;
    if (s_fail++ < 20)
        printf("FAIL %s vs %s pos=%d a=%08X.. b=%08X..\n  got  %08X %08X %08X %08X\n  want %08X %08X %08X %08X\n",
               name, against, pos, a._u32[0], b._u32[0], got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

/* ISA definition: control byte t = 0x10 + t (rB's byte t), except the inserted element's
 * bytes, which select rA's preferred slot -- for an n-byte element at n-aligned position p,
 * control byte p + k = (4 - n for n < 4, else 0) + k. */
static u128 isa_control(uint32_t addr, int n)
{
    const int p = (int)(addr & 15u) & ~(n - 1), first = n < 4 ? 4 - n : 0;
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    for (int k = 0; k < n; k++) r._u8[SPU_W(p + k)] = (uint8_t)(first + k);
    return r;
}

/* The control used as shufb's: the element of `value` lands in `into` at position p. */
static u128 isa_insert(u128 value, u128 into, uint32_t addr, int n)
{
    const int p = (int)(addr & 15u) & ~(n - 1), first = n < 4 ? 4 - n : 0;
    u128 r = into;
    for (int k = 0; k < n; k++) r._u8[SPU_W(p + k)] = value._u8[SPU_W(first + k)];
    return r;
}

typedef u128 (*ctl_d)(u128, int);
typedef u128 (*ctl_x)(u128, u128);

int main(void)
{
    static const struct { const char* name; ctl_d fast, ref; int n; } d[] = {
        {"cbd", spu_cbd, spu_cbd_ref, 1}, {"chd", spu_chd, spu_chd_ref, 2},
        {"cwd", spu_cwd, spu_cwd_ref, 4}, {"cdd", spu_cdd, spu_cdd_ref, 8},
    };
    static const struct { const char* name; ctl_x fast, ref; int n; } x[] = {
        {"cbx", spu_cbx, spu_cbx_ref, 1}, {"chx", spu_chx, spu_chx_ref, 2},
        {"cwx", spu_cwx, spu_cwx_ref, 4}, {"cdx", spu_cdx, spu_cdx_ref, 8},
    };
    enum { ROUNDS = 4000 };
    for (int pos = 0; pos < 16; pos++) {
        for (int r = 0; r < ROUNDS; r++) {
            const u128 value = rv(), into = rv();
            /* d-form: rA word 0 random, the 7-bit signed immediate random, then rA's low
             * nibble set so that the sum's is `pos`. Round 0 also takes the wrap at 2^32. */
            const int i7 = (int)(rnd() % 128u) - 64;
            u128 a = rv();
            if (r == 0) a._u32[0] = 0xFFFFFFF0u;
            a._u32[0] = (a._u32[0] & ~15u) | (((uint32_t)pos - (uint32_t)i7) & 15u);
            const uint32_t addr_d = a._u32[0] + (uint32_t)i7;
            /* x-form: rA and rB word 0 random, rB's low nibble set the same way. */
            u128 xa = rv(), xb = rv();
            if (r == 1) { xa._u32[0] = 0xFFFFFFFFu; xb._u32[0] = 0x7FFFFFF0u; }
            xb._u32[0] = (xb._u32[0] & ~15u) | (((uint32_t)pos - xa._u32[0]) & 15u);
            const uint32_t addr_x = xa._u32[0] + xb._u32[0];
            if ((addr_d & 15u) != (uint32_t)pos || (addr_x & 15u) != (uint32_t)pos) {
                puts("FAIL: fixture address arithmetic");
                return 1;
            }
            for (int k = 0; k < 4; k++) {
                const u128 got = d[k].fast(a, i7);
                cmp(d[k].name, "old helper", pos, a, a, got, d[k].ref(a, i7));
                cmp(d[k].name, "ISA control", pos, a, a, got, isa_control(addr_d, d[k].n));
                cmp(d[k].name, "ISA insert", pos, value, into, spu_shufb(value, into, got),
                    isa_insert(value, into, addr_d, d[k].n));
                const u128 gx = x[k].fast(xa, xb);
                cmp(x[k].name, "old helper", pos, xa, xb, gx, x[k].ref(xa, xb));
                cmp(x[k].name, "ISA control", pos, xa, xb, gx, isa_control(addr_x, x[k].n));
                cmp(x[k].name, "ISA insert", pos, value, into, spu_shufb(value, into, gx),
                    isa_insert(value, into, addr_x, x[k].n));
            }
        }
    }
    printf("insertion controls (%s): %d checks, %d failed -- cbd chd cwd cdd cbx chx cwx cdx, positions 0-15\n",
#if SPU_X86
           "SSE4.1",
#elif defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
           "NEON",
#else
           "scalar",
#endif
           s_checks, s_fail);
    return s_fail ? 1 : 0;
}
