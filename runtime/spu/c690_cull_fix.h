/*
 * Native cull sample-grid fix.
 *
 * The title's ICE geometry job runs the screen-space triangle culls at the
 * 1280x720 logical viewport, while its scene target is 2560x720 (2x1 AA).
 * A triangle that covers one of the two horizontal samples can still have a
 * sub-pixel extent at 1280 and is dropped, which shows as pin-holes around
 * the eyes on character meshes.
 *
 * The cull's screen transform is a four-float block in local store:
 *   { scale_x, offset_x, scale_y, offset_y }
 * in several per-lane copies.  The block that the cull actually consumes is
 * the highest-address copy, next to the vertex batch.  Scaling only that one
 * changes the kept list (it is the sample-grid lever), but it also drops
 * triangles on some batches: the cull has a fixed 720p screen bound that is
 * not in this block.  The safe fix is therefore a union:
 *
 *   1. run the lifted cull at the sample grid (2x x) and keep its list;
 *   2. restore the game's input state, run the lifted cull unpatched;
 *   3. append the sample-grid triangles the original list does not contain.
 *
 * The original list is never changed or reordered, so the kept-index set is a
 * superset of the game's.  The lifted body stays bit-exact; the switch is off
 * by default and every hook is a predicted-not-taken branch when disabled.
 *
 * Switches (env and live control file):
 *   PS3RECOMP_C690_CULL_FIX=1     union fix for 0x25/0x26 (0x132E0)
 *   PS3RECOMP_C690_CULL_FIX_2C=1  old single-grid patch for 0x2C (0x12C08)
 *
 * See docs/edge-holes-fix.md (mlb16-recomp) and docs/edge-native-step3.md.
 */
#ifndef SPU_C690_CULL_FIX_H
#define SPU_C690_CULL_FIX_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* True when a cull hook must be consulted (switch on, or a live control file
 * exists so a switch can turn on while the game runs). */
int c690_cull_fix_maybe(void);

/* True when the fix is active for this family (1 = 0x25/0x26, 2 = 0x2C). */
int c690_cull_fix_enabled(int family);

/* Legacy single-grid entry: patch the screen transform in ctx->ls before the
 * lifted cull.  Returns 1 when at least one block was patched; pair with
 * c690_cull_fix_leave().  Only used for family 2. */
int c690_cull_fix_enter(struct spu_context* ctx, int family);

/* Restore the saved transform words. */
void c690_cull_fix_leave(struct spu_context* ctx);

/* Union driver used by the 0x25/0x26 wrapper.
 *
 *   mode = c690_cull_fix_begin(ctx, family);
 *   if (mode == 1) {
 *       body(ctx);                 // sample-grid run
 *       c690_cull_fix_mid(ctx);    // save its list, restore game input
 *       body(ctx);                 // the game's run, unpatched
 *       c690_cull_fix_end(ctx);    // append the sample-grid-only triangles
 *   } else if (mode == 2) {
 *       body(ctx);                 // single-grid patch (family 2)
 *       c690_cull_fix_leave(ctx);
 *   } else {
 *       body(ctx);                 // switch off / no block
 *   }
 *
 * Returns 0 (nothing to do), 1 (union), or 2 (single-grid patch). */
int c690_cull_fix_begin(struct spu_context* ctx, int family);
void c690_cull_fix_mid(struct spu_context* ctx);
void c690_cull_fix_end(struct spu_context* ctx);

/* Counters for the report. */
typedef struct {
    unsigned long long calls;
    unsigned long long patched;
    unsigned long long no_block;
    unsigned long long blocks;
    /* union (0x25/0x26) */
    unsigned long long union_runs;
    unsigned long long union_added_tris;
    unsigned long long union_cap_hits;
    unsigned long long union_odd_tail;
} c690_cull_fix_counters;

void c690_cull_fix_get_counters(c690_cull_fix_counters* out);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_CULL_FIX_H */
