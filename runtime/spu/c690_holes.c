/* Conservative cull-union merge.  See c690_holes.h. */
#include "c690_holes.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint32_t tri_hash64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (uint32_t)k;
}

static uint64_t tri_key(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t lo = a, mid = b, hi = c, t;
    if (lo > mid) { t = lo; lo = mid; mid = t; }
    if (mid > hi) { t = mid; mid = hi; hi = t; }
    if (lo > mid) { t = lo; lo = mid; mid = t; }
    return ((uint64_t)lo << 32) | ((uint64_t)mid << 16) | (uint64_t)hi;
}

int c690_tri_set_init(c690_tri_set* set, uint32_t tris)
{
    if (!set) return 0;
    uint32_t want = 64;
    while (want < tris * 2u && want < (1u << 20)) want <<= 1;
    if (set->keys && set->cap >= want) {
        memset(set->keys, 0, (size_t)set->cap * sizeof *set->keys);
        return 1;
    }
    uint64_t* p = (uint64_t*)realloc(set->keys, (size_t)want * sizeof *p);
    if (!p) return 0;
    set->keys = p;
    set->cap = want;
    memset(p, 0, (size_t)want * sizeof *p);
    return 1;
}

void c690_tri_set_free(c690_tri_set* set)
{
    if (!set) return;
    free(set->keys);
    set->keys = NULL;
    set->cap = 0;
}

/* 0 is the empty marker; stored keys are key+1 (the canonical key is 48-bit,
 * so key+1 is never 0). */
void c690_tri_set_insert(c690_tri_set* set, uint16_t a, uint16_t b, uint16_t c)
{
    if (!set || !set->keys || !set->cap) return;
    const uint64_t v = tri_key(a, b, c) + 1u;
    uint32_t i = tri_hash64(v) & (set->cap - 1u);
    while (set->keys[i] && set->keys[i] != v) i = (i + 1u) & (set->cap - 1u);
    set->keys[i] = v;
}

int c690_tri_set_has(const c690_tri_set* set, uint16_t a, uint16_t b, uint16_t c)
{
    if (!set || !set->keys || !set->cap) return 0;
    const uint64_t v = tri_key(a, b, c) + 1u;
    uint32_t i = tri_hash64(v) & (set->cap - 1u);
    while (set->keys[i]) {
        if (set->keys[i] == v) return 1;
        i = (i + 1u) & (set->cap - 1u);
    }
    return 0;
}

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint32_t c690_holes_merge_lists(uint8_t* game_ls, const uint8_t* sample_ls,
                                uint32_t ptr, uint32_t game_tris,
                                uint32_t sample_tris, uint32_t cap_indices,
                                c690_tri_set* set, unsigned* cap_skipped)
{
    if (cap_skipped) *cap_skipped = 0;
    if (!game_ls || !sample_ls || !set) return 0;
    if (game_tris > cap_indices / 3u) return 0;
    if (!c690_tri_set_init(set, game_tris + sample_tris)) return 0;

    for (uint32_t i = 0; i < game_tris; i++) {
        c690_tri_set_insert(set,
            be16(game_ls + ptr + 6u * i),
            be16(game_ls + ptr + 6u * i + 2u),
            be16(game_ls + ptr + 6u * i + 4u));
    }

    uint32_t out = game_tris, added = 0, skipped = 0;
    for (uint32_t i = 0; i < sample_tris; i++) {
        const uint16_t a = be16(sample_ls + ptr + 6u * i);
        const uint16_t b = be16(sample_ls + ptr + 6u * i + 2u);
        const uint16_t c = be16(sample_ls + ptr + 6u * i + 4u);
        if (c690_tri_set_has(set, a, b, c)) continue;
        if ((out + 1u) * 3u > cap_indices) {
            skipped++;
            continue;
        }
        put16(game_ls + ptr + 6u * out, a);
        put16(game_ls + ptr + 6u * out + 2u, b);
        put16(game_ls + ptr + 6u * out + 4u, c);
        c690_tri_set_insert(set, a, b, c);
        out++;
        added++;
    }
    if (cap_skipped) *cap_skipped = skipped;
    return added;
}
