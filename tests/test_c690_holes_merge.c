/*
 * Unit tests for the conservative c690 cull-union merge (c690_holes.c).
 *
 * The merge is the pure half of the fused native holes pass: append the
 * sample-grid triangles the game list does not contain, never remove or
 * reorder an original entry.  No SPU state is involved.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "c690_holes.h"

static int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t list_tris(const uint8_t* ls, uint32_t ptr, uint32_t tris,
                          uint16_t* out)
{
    for (uint32_t i = 0; i < tris * 3u; i++) out[i] = get16(ls + ptr + 2u * i);
    return tris * 3u;
}

static void test_set_canonical(void)
{
    c690_tri_set set = {0};
    CHECK(c690_tri_set_init(&set, 8));
    c690_tri_set_insert(&set, 7, 3, 5);
    CHECK(c690_tri_set_has(&set, 7, 3, 5));
    CHECK(c690_tri_set_has(&set, 3, 5, 7));
    CHECK(c690_tri_set_has(&set, 5, 7, 3));
    CHECK(!c690_tri_set_has(&set, 7, 3, 6));
    c690_tri_set_free(&set);
    CHECK(set.keys == NULL && set.cap == 0);
}

static void test_merge_adds_only_sample_only(void)
{
    enum { PTR = 0x40, CAP = 64 };
    uint8_t game[1024], sample[1024];
    memset(game, 0, sizeof game);
    memset(sample, 0, sizeof sample);

    /* Game keeps (1,2,3) and (4,5,6). */
    put16(game + PTR + 0, 1); put16(game + PTR + 2, 2); put16(game + PTR + 4, 3);
    put16(game + PTR + 6, 4); put16(game + PTR + 8, 5); put16(game + PTR + 10, 6);
    /* Sample: same triangle with a different winding, a new one, its repeat. */
    put16(sample + PTR + 0, 3); put16(sample + PTR + 2, 1); put16(sample + PTR + 4, 2);
    put16(sample + PTR + 6, 9); put16(sample + PTR + 8, 8); put16(sample + PTR + 10, 7);
    put16(sample + PTR + 12, 7); put16(sample + PTR + 14, 8); put16(sample + PTR + 16, 9);

    uint8_t before[1024];
    memcpy(before, game, sizeof before);

    c690_tri_set set = {0};
    unsigned skipped = 99;
    uint32_t added = c690_holes_merge_lists(game, sample, PTR, 2, 3, CAP,
                                            &set, &skipped);
    CHECK(added == 1);
    CHECK(skipped == 0);
    CHECK(memcmp(game, before, PTR + 12) == 0);      /* originals untouched */
    uint16_t tris[16];
    CHECK(list_tris(game, PTR, 3, tris) == 9);
    CHECK(tris[6] == 9 && tris[7] == 8 && tris[8] == 7);
    c690_tri_set_free(&set);
}

static void test_merge_cap(void)
{
    enum { PTR = 0x40 };
    uint8_t game[1024], sample[1024];
    memset(game, 0, sizeof game);
    memset(sample, 0, sizeof sample);
    put16(game + PTR + 0, 1); put16(game + PTR + 2, 2); put16(game + PTR + 4, 3);
    put16(sample + PTR + 0, 7); put16(sample + PTR + 2, 8); put16(sample + PTR + 4, 9);
    put16(sample + PTR + 6, 10); put16(sample + PTR + 8, 11); put16(sample + PTR + 10, 12);

    c690_tri_set set = {0};
    unsigned skipped = 0;
    /* Room for exactly one appended triangle. */
    uint32_t added = c690_holes_merge_lists(game, sample, PTR, 1, 2, 6, &set, &skipped);
    CHECK(added == 1);
    CHECK(skipped == 1);
    c690_tri_set_free(&set);

    /* The game list itself does not fit: refuse to merge. */
    memset(game, 0, sizeof game);
    c690_tri_set set2 = {0};
    skipped = 0;
    added = c690_holes_merge_lists(game, sample, PTR, 3, 2, 6, &set2, &skipped);
    CHECK(added == 0);
    c690_tri_set_free(&set2);
}

int main(void)
{
    test_set_canonical();
    test_merge_adds_only_sample_only();
    test_merge_cap();
    if (g_fail) {
        fprintf(stderr, "%d c690 holes merge check(s) failed\n", g_fail);
        return 1;
    }
    printf("c690 holes merge: OK\n");
    return 0;
}
