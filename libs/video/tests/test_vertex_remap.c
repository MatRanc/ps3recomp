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
#include <string.h>

int main(void)
{
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

    rsx_vertex_remap_destroy(&remap);
    puts("vertex remap: ok");
    return 0;
}
