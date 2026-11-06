#!/usr/bin/env python3
"""Check the --fast-cfl cflts/cfltu (spu_ftz_twin.py APPROX_FAST_CFL) against spu_cflts_ref for every
i8 scale and every float exponent/sign x mantissa edge plus random words, under MXCSR FTZ|DAZ."""
import importlib.util, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("twin", os.path.join(HERE, "spu_ftz_twin.py"))
twin = importlib.util.module_from_spec(spec); spec.loader.exec_module(twin)
calls = "\n".join("    T(%d);" % i for i in range(256))
src = r'''
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include "spu_helpers.h"
static inline __m128 spu_ap_ld(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
%s
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static long bad;
static void chk(const uint32_t* w, int i8, u128 a, u128 e, u128 g, const char* n) {
    for (int l = 0; l < 4; l++) if (e._u32[l] != g._u32[l]) {
        if (bad++ < 10) printf("%%s i8=%%d in=%%08X want=%%08X got=%%08X\n", n, i8, w[l], e._u32[l], g._u32[l]);
    }
}
#define T(I) do { for (int k = 0; k < NW; k += 4) { u128 a; for (int l = 0; l < 4; l++) a._u32[l] = W[k+l]; \
    chk(W+k, I, a, spu_cflts_ref(a, I), spu_ap_cflts(a, I), "cflts"); \
    chk(W+k, I, a, spu_cfltu_ref(a, I), spu_ap_cfltu(a, I), "cfltu"); } } while (0)
enum { NW = 4 * (2 * 256 * 24 + 4096) };
static uint32_t W[NW];
int main(void) {
    int n = 0;
    for (int s = 0; s < 2; s++) for (int e = 0; e < 256; e++) {
        static const uint32_t m[24] = {0,1,2,0x7FFFFF,0x400000,0x3FFFFF,0x7FFFFE,0x555555,0x2AAAAA,0x100000,0x7FFF00,0x000100,
                                       0x7F0000,0x00FFFF,0x600000,0x200000,0x7FFFFD,0x123456,0x654321,0x4,0x8,0x10,0x20,0x40};
        for (int j = 0; j < 24; j++) for (int q = 0; q < 4; q += 4) W[n++] = (uint32_t)s << 31 | (uint32_t)e << 23 | m[j];
        while (n & 3) W[n++] = 0;
    }
    n = 4 * 2 * 256 * 24 > n ? n : n;
    for (; n < NW; n++) W[n] = rnd();
    _mm_setcsr(_mm_getcsr() | 0x8040u);
%s
    printf("bad=%%ld\n", bad); return bad != 0;
}
''' % (twin.APPROX_FAST_CFL, calls)
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, "t.c"); open(c, "w").write(src)
    cmd = ["clang", "-O2", "-march=native", "-std=gnu17", "-fno-strict-aliasing", "-fwrapv", "-mfma", "-mavx2", "-ffp-contract=off",
           "-DSPU_FAST_FLOAT", "-DSPU_LIFTED_FAST_LS", "-DSPU_WRCH_INLINE"] + \
          ["-I%s/%s" % (ROOT, p) for p in ("runtime/spu", "include", "libs/video", "runtime/ppu", "runtime/memory", "runtime/syscalls", "runtime/platform")] + \
          [c, "-o", os.path.join(d, "t"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: sys.exit(r.stderr[-3000:])
    sys.exit(subprocess.run([os.path.join(d, "t")]).returncode)
