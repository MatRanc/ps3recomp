/*
 * rsx_vertex_remap_build: an ascending run of references (a draw_arrays) skips
 * the hash and comes back unchanged with every reference unique; anything else
 * still collapses duplicates and maps each occurrence to its unique vertex.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_remap libs/video/tests/test_vertex_remap.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* PS3RECOMP_REMAP_FAST is read once (the env fallback is cached); enable it
     * for the whole fixture so every case below also proves the memo exact. */
    setenv("PS3RECOMP_REMAP_FAST", "1", 1);

    rsx_vertex_remap remap;
    memset(&remap, 0, sizeof remap);
    rsx_vertex_ref refs[8];
    u32 unique = 0;

    /* Sequential: untouched, all unique. */
    for (u32 i = 0; i < 8; i++) { refs[i].vertex_id = 100 + i; refs[i].base_index = 7; }
    assert(rsx_vertex_remap_build(&remap, refs, 8, &unique) && unique == 8);
    for (u32 i = 0; i < 8; i++) assert(refs[i].vertex_id == 100 + i && refs[i].base_index == 7);

    /* Ascending but a different base mid-run: not the fast path, still all unique. */
    refs[5].base_index = 8;
    assert(rsx_vertex_remap_build(&remap, refs, 8, &unique) && unique == 8);

    /* A quad as two triangles (0 1 2 2 1 3): duplicates collapse. */
    const u32 ids[6] = { 0, 1, 2, 2, 1, 3 };
    for (u32 i = 0; i < 6; i++) { refs[i].vertex_id = ids[i]; refs[i].base_index = 0; }
    assert(rsx_vertex_remap_build(&remap, refs, 6, &unique) && unique == 4);
    const u32 want[6] = { 0, 1, 2, 2, 1, 3 };
    for (u32 i = 0; i < 6; i++) assert(rsx_vertex_remap_index(&remap, i) == want[i]);
    for (u32 i = 0; i < 4; i++) assert(refs[i].vertex_id == i);

    /* A longer list with local and distant repeats, two base indices, and a
     * non-monotonic order: the micro memo must not change any answer. The
     * expected map is the obvious O(n^2) first-use collapse. */
    enum { N = 400 };
    rsx_vertex_ref big[N], orig[N];
    u32 want_big[N], want_unique = 0;
    for (u32 i = 0; i < N; i++) {
        big[i].vertex_id = (i * 37u + (i / 11u) * 5u) % 97u;
        big[i].base_index = (i % 13u) < 9u ? 3u : 4u;
        want_big[i] = 0;
        u32 j;
        for (j = 0; j < i; j++)
            if (big[j].vertex_id == big[i].vertex_id &&
                big[j].base_index == big[i].base_index) { want_big[i] = want_big[j]; break; }
        if (j == i) want_big[i] = want_unique++;
    }
    memcpy(orig, big, sizeof orig);   /* the build compacts refs in place */
    assert(rsx_vertex_remap_build(&remap, big, N, &unique) && unique == want_unique);
    for (u32 i = 0; i < N; i++) assert(rsx_vertex_remap_index(&remap, i) == want_big[i]);
    /* Rebuilding the same list on the same object (a new draw) is identical. */
    memcpy(big, orig, sizeof big);
    assert(rsx_vertex_remap_build(&remap, big, N, &unique) && unique == want_unique);
    for (u32 i = 0; i < N; i++) assert(rsx_vertex_remap_index(&remap, i) == want_big[i]);

    rsx_vertex_remap_destroy(&remap);

    /* Incremental reference hash: folding each reference as it is appended
     * must give exactly rsx_vertex_refs_hash() of the finished list (the draw
     * engine uses the fold to skip a second pass over the list). Includes the
     * empty list and lists whose length is not a multiple of the reference
     * size's natural word count, plus a "restart cut" where the sentinel is
     * never appended. */
    {
        rsx_vertex_ref list[257];
        for (u32 n = 0; n <= 257; n += (n < 8 ? 1u : 37u)) {
            for (u32 i = 0; i < n; i++) {
                list[i].vertex_id = (i * 2654435761u) ^ (i << 7);
                list[i].base_index = (i * 40503u) ^ (i >> 3);
            }
            u64 fold = RSX_VERTEX_REFS_HASH_BASIS;
            for (u32 i = 0; i < n; i++)
                fold = rsx_vertex_refs_hash_fold(fold, list[i].vertex_id,
                                                 list[i].base_index);
            /* The trailing count is byte-wise, like the engine's tail. */
            for (u32 b = 0; b < sizeof n; b++)
                fold = (fold ^ ((const u8*)&n)[b]) * 1099511628211ull;
            assert(fold == rsx_vertex_refs_hash(list, n));
        }
        /* Restart sentinel: it is excluded from the list, so the hash must be
         * the one a shorter list would produce. */
        const u32 ids[6] = { 40, 41, 0xFFFFFFFFu, 42, 41, 43 };
        u64 fold = RSX_VERTEX_REFS_HASH_BASIS;
        u32 kept = 0;
        for (u32 i = 0; i < 6; i++)
            if (ids[i] != 0xFFFFFFFFu) {
                list[kept].vertex_id = ids[i];
                list[kept].base_index = 2;
                fold = rsx_vertex_refs_hash_fold(fold, ids[i], 2);
                kept++;
            }
        for (u32 b = 0; b < sizeof kept; b++)
            fold = (fold ^ ((const u8*)&kept)[b]) * 1099511628211ull;
        assert(fold == rsx_vertex_refs_hash(list, kept));
    }

    puts("vertex remap: ok");
    return 0;
}
