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
 * in several per-lane copies. When the fix is enabled the hook doubles the
 * two x fields for the duration of the lifted cull, so the cull tests the
 * same 2x1 sample grid the renderer draws. The y fields are untouched because
 * the target has one vertical sample per pixel. The lifted body is unchanged
 * and runs bit-exactly when the switch is off; the cull output is an index
 * list, so this changes only which triangles are kept.
 *
 * Switches (env and live control file):
 *   PS3RECOMP_C690_CULL_FIX=1     apply to 0x25/0x26 (0x132E0)
 *   PS3RECOMP_C690_CULL_FIX_2C=1  apply to 0x2C (0x12C08)
 *
 * STATUS: experimental / not a safe fix yet. Offline replay of 400 meter cull
 * pairs shows that scaling the screen-space x transform changes the kept-index
 * list for 35/400 invocations, but it also drops triangles on many others
 * because the cull has a separate fixed bound. Do not enable this for normal
 * play; it exists to reproduce and measure the mechanism. See
 * docs/edge-native-step3.md.
 *
 * The switch is deliberately off by default.
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

/* Patch the screen transform in ctx->ls before the lifted cull. Returns 1
 * when at least one block was patched; pair with c690_cull_fix_leave(). */
int c690_cull_fix_enter(struct spu_context* ctx, int family);

/* Restore the saved transform words. */
void c690_cull_fix_leave(struct spu_context* ctx);

/* Counters for the report. */
typedef struct {
    unsigned long long calls;
    unsigned long long patched;
    unsigned long long no_block;
    unsigned long long blocks;
} c690_cull_fix_counters;

void c690_cull_fix_get_counters(c690_cull_fix_counters* out);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_CULL_FIX_H */
