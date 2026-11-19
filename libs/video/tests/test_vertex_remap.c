/* rsx_vertex_remap_build: unique references in first-use order and an
 * occurrence map back to them, the same whichever path takes a draw (the
 * direct map for one base index over a small range, the identity for a
 * sequential run, the hash otherwise), checked against a naive reference on
 * random draws of every shape.
 *
 *   cc -I libs/video -I include libs/video/tests/test_vertex_remap.c \
 *      libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c (ctest: vertex_remap)
 */
#include "rsx_vertex_compact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_seed = 12345u;
static u32 rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8; }

/* O(n^2) reference: first occurrence of each (vertex_id, base_index). */
static u32 naive(const rsx_vertex_ref* in, u32 n, rsx_vertex_ref* uniq, u32* occ)
{
    u32 u = 0;
    for (u32 i = 0; i < n; i++) {
        u32 j = 0;
        while (j < u && !(uniq[j].vertex_id == in[i].vertex_id && uniq[j].base_index == in[i].base_index)) j++;
        if (j == u) uniq[u++] = in[i];
        occ[i] = j;
    }
    return u;
}

int main(void)
{
    rsx_vertex_remap remap;
    memset(&remap, 0, sizeof remap);
    enum { MAXN = 3000 };
    static rsx_vertex_ref refs[MAXN], orig[MAXN], uniq[MAXN];
    static u32 occ[MAXN];
    int fails = 0;
    for (int iter = 0; iter < 4000; iter++) {
        const u32 n = 2 + rnd() % (MAXN - 2);
        const int shape = iter % 5;
        const u32 base = rnd() % 4 ? 0u : rnd();
        const u32 lo = rnd() % 4 ? rnd() % 1000u : rnd();
        for (u32 i = 0; i < n; i++) {
            u32 v, b = base;
            switch (shape) {
            case 0: v = lo + i; break;                                /* sequential run */
            case 1: v = lo + rnd() % (n / 3 + 1); break;              /* indexed mesh */
            case 2: v = lo + rnd() % 2000000u; break;                 /* wide range: hash */
            case 3: v = lo + rnd() % 64u; b = base + (rnd() % 2); break; /* two bases: hash */
            default: v = (i % 7 == 3) ? lo + i - 1 : lo + i; break;   /* almost sequential */
            }
            refs[i].vertex_id = v; refs[i].base_index = b;
        }
        memcpy(orig, refs, n * sizeof *refs);
        u32 unique = 0;
        if (!rsx_vertex_remap_build(&remap, refs, n, &unique)) { printf("build failed\n"); return 1; }
        const u32 want = naive(orig, n, uniq, occ);
        int bad = unique != want;
        for (u32 i = 0; !bad && i < want; i++)
            bad = refs[i].vertex_id != uniq[i].vertex_id || refs[i].base_index != uniq[i].base_index;
        for (u32 i = 0; !bad && i < n; i++)
            bad = rsx_vertex_remap_index(&remap, i) != occ[i];
        if (bad && fails++ < 5)
            printf("FAIL iter %d shape %d n %u: unique %u want %u\n", iter, shape, n, unique, want);
    }
    rsx_vertex_remap_destroy(&remap);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
