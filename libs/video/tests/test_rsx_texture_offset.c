/*
 * ps3recomp - texture offsets are 31 bits
 *
 * NV4097_SET_TEXTURE_OFFSET (0x1A00 + unit*0x20) and the vertex
 * texture offset (0x0900 + unit*0x20) carry a 31-bit offset; bit 31 is not an
 * address bit (RPCS3 masks it in RSXTexture.cpp offset()). A title binding a
 * main-memory plane as 0x82A22900 / 0x82B74100 got an IO lookup that missed
 * and a texture read from unwritten memory when the offset was decoded whole.
 *
 * Dispatcher only: no GPU, no host graphics API.
 *   gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *       -o /tmp/test_rsx_texture_offset libs/video/tests/test_rsx_texture_offset.c \
 *       libs/video/rsx_dispatch.c
 */
#include "rsx_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

int main(void)
{
    rsx_dispatch_sink sink = {0};
    rsx_dispatch* rsx = (rsx_dispatch*)calloc(1, sizeof(rsx_dispatch));
    rsx_dispatch_init(rsx, &sink);

    /* Fragment unit 2 with bit 31 set: main memory (format & 3 == 2), 8-bit plane. */
    rsx_dispatch_method(rsx, 0x1A00 + 2 * 0x20, 0x82A22900u);
    rsx_dispatch_method(rsx, 0x1A04 + 2 * 0x20, 0x00018102u);
    rsx_dsp_texture t;
    rsx_dsp_get_texture(rsx, 2, &t);
    CHECK(t.offset == 0x02A22900u, "fragment unit 2 offset 0x82A22900 decodes to 0x02A22900 (got 0x%08X)", t.offset);
    CHECK(t.location == RSX_LOCATION_MAIN, "fragment unit 2 location stays main memory (got %u)", t.location);

    /* The other swap buffer, and a local-memory unit whose offset has bit 31 clear: unchanged. */
    rsx_dispatch_method(rsx, 0x1A00 + 2 * 0x20, 0x82B74100u);
    rsx_dsp_get_texture(rsx, 2, &t);
    CHECK(t.offset == 0x02B74100u, "fragment unit 2 offset 0x82B74100 decodes to 0x02B74100 (got 0x%08X)", t.offset);
    rsx_dispatch_method(rsx, 0x1A00, 0x0438000u);
    rsx_dispatch_method(rsx, 0x1A04, 0x00018101u);
    rsx_dsp_get_texture(rsx, 0, &t);
    CHECK(t.offset == 0x0438000u && t.location == RSX_LOCATION_LOCAL,
          "fragment unit 0 local offset 0x00438000 unchanged (got 0x%08X loc %u)", t.offset, t.location);

    /* Vertex texture unit 1 with bit 31 set. */
    rsx_dispatch_method(rsx, 0x0900 + 1 * 0x20, 0x80100000u);
    rsx_dispatch_method(rsx, 0x0904 + 1 * 0x20, 0x00018102u);
    rsx_dsp_vertex_texture vt;
    rsx_dsp_get_vertex_texture(rsx, 1, &vt);
    CHECK(vt.offset == 0x00100000u, "vertex unit 1 offset 0x80100000 decodes to 0x00100000 (got 0x%08X)", vt.offset);

    /* tex_wgen: a unit's count moves only on a write that changes one of
     * its registers (its 8-word block or its TEX_SIZE1), never another's. */
    u32 w2 = rsx->tex_wgen[2], w3 = rsx->tex_wgen[3];
    rsx_dispatch_method(rsx, 0x1A00 + 2 * 0x20, 0x82B74100u);           /* same value */
    CHECK(rsx->tex_wgen[2] == w2, "rewriting a unit's offset unchanged leaves tex_wgen");
    rsx_dispatch_method(rsx, 0x1A14 + 2 * 0x20, 0x12345678u);           /* filter */
    CHECK(rsx->tex_wgen[2] == w2 + 1 && rsx->tex_wgen[3] == w3,
          "a changed filter bumps that unit's tex_wgen only");
    rsx_dispatch_method(rsx, 0x1840 + 2 * 4, 0x00100400u);              /* TEX_SIZE1 */
    rsx_dispatch_method(rsx, 0x1840 + 2 * 4, 0x00100400u);
    CHECK(rsx->tex_wgen[2] == w2 + 2, "TEX_SIZE1 counts for its unit, once per change");
    rsx_dispatch_method(rsx, 0x1A1C + 15 * 0x20, 1u);                   /* last unit's border */
    CHECK(rsx->tex_wgen[15] == 1, "the last unit's border colour counts");
    u32 before[RSX_DSP_NUM_TEXTURES];
    memcpy(before, rsx->tex_wgen, sizeof before);
    rsx_dispatch_method(rsx, 0x0904 + 1 * 0x20, 0x00028102u);           /* vertex unit */
    rsx_dispatch_method(rsx, 0x1840 - 4, 7u);                           /* below TEX_SIZE1 */
    rsx_dispatch_method(rsx, 0x1880, 7u);                               /* past it */
    rsx_dispatch_method(rsx, 0x1C00, 7u);                               /* past the unit blocks */
    CHECK(!memcmp(before, rsx->tex_wgen, sizeof before),
          "vertex-texture and neighbouring registers leave fragment units alone");
    u32 seed[0x1A00 / 4] = {0};
    rsx_dispatch_seed_registers(rsx, seed, 0x1A00 / 4);
    CHECK(rsx->tex_wgen[2] == w2 + 3 && rsx->tex_wgen[7] == 1, "seeding the register file bumps every unit");

    free(rsx);
    if (g_failures) { printf("%d check(s) FAILED\n", g_failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
