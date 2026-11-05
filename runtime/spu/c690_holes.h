/*
 * Conservative cull-union merge for the c690 0x25/0x26 screen cull.
 *
 * The game culls at the 1280x720 logical viewport while the scene target is
 * 2560x720 with 2x1 AA.  The safe holes rule is a union: keep every triangle
 * the game keeps, plus the ones the 2x1 sample grid keeps.  This file is the
 * pure list-merge half of that rule (no SPU state): it appends sample-grid
 * triangles that the game list does not contain, in sample-list order, using
 * canonical (order-insensitive) vertex-set membership and never touching the
 * original entries.
 *
 * See runtime/spu/c690_native_screen.c and docs/edge-e1-holes.md.
 */
#ifndef SPU_C690_HOLES_H
#define SPU_C690_HOLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t* keys;
    uint32_t cap;              /* power of two, 0 when not allocated */
} c690_tri_set;

/* Allocate/clear a set sized for `tris` insertions (at least 64 slots).
 * Returns 1 on success.  Reuses the existing allocation when it is large
 * enough. */
int c690_tri_set_init(c690_tri_set* set, uint32_t tris);
void c690_tri_set_free(c690_tri_set* set);

/* Canonical vertex-set membership. */
void c690_tri_set_insert(c690_tri_set* set, uint16_t a, uint16_t b, uint16_t c);
int c690_tri_set_has(const c690_tri_set* set, uint16_t a, uint16_t b, uint16_t c);

/* Merge `sample_tris` triangles from sample_ls into the game list at
 * game_ls+ptr, which currently holds `game_tris` triangles and may grow to
 * `cap_indices` u16 entries.  Lists are big-endian u16 index arrays, three
 * indices per triangle.  `set` is scratch (initialised here).  Returns the
 * number of triangles appended (0..); `*cap_skipped` receives the number of
 * sample triangles that did not fit. */
uint32_t c690_holes_merge_lists(uint8_t* game_ls, const uint8_t* sample_ls,
                                uint32_t ptr, uint32_t game_tris,
                                uint32_t sample_tris, uint32_t cap_indices,
                                c690_tri_set* set, unsigned* cap_skipped);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_HOLES_H */
