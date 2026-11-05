/*
 * Native c690 screen cull: the 0x25/0x26 command's 0xD080 subtree.
 *
 * The lifted command handler calls 0xD080 as a subroutine; from there the
 * cull is a tail-call state machine that transforms vertices, tests them
 * against the screen/clip planes and writes the kept-index list back into the
 * caller's buffer. c690_native_screen_stage.inc is a bit-exact flattening of
 * that subtree (tools/c690_ref/gen_native_screen_stage.py): the SPU registers
 * stay live instead of being marshalled through ctx->gpr at every block.
 *
 * Switches (environment and live control file):
 *   PS3RECOMP_C690_NATIVE_SCREEN=0        off (default; the lifted call runs)
 *   PS3RECOMP_C690_NATIVE_SCREEN=1        mode 0: exact lift replacement
 *   PS3RECOMP_C690_NATIVE_SCREEN=2        mode 1: 2x1 sample-grid union
 *   PS3RECOMP_C690_NATIVE_SCREEN_SHADOW=1 run lifted, compare, keep lifted
 *
 * Mode 1 takes the entry snapshot, evaluates the cull once at the 2x sample
 * grid and once at the game's grid in the same native call, restores the
 * game's result and appends only the sample-grid triangles the original list
 * does not contain. The original list and its order are untouched; the append
 * is capped by the proven buffer extent (+0x1E0), exactly as the lifted union.
 */
#ifndef SPU_C690_NATIVE_SCREEN_H
#define SPU_C690_NATIVE_SCREEN_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*c690_native_screen_lifted_fn)(struct spu_context* ctx);

/* Replace the lifted `0xD080` call bracket. Returns 0 when the caller must run
 * the lifted bracket itself (switch off), 1 when this call handled it. In
 * shadow mode it runs the lifted function through the bracket and compares.
 * `ret_pc` is the link register the lifted call was made with. */
int c690_native_screen_try(struct spu_context* ctx, uint32_t ret_pc,
                           c690_native_screen_lifted_fn lifted);

/* True when a switch is on (live control file wins over the environment). */
int c690_native_screen_enabled(void);

/* Current mode (0 off, 1 exact, 2 sample-grid union).  The union driver lives
 * in c690_cull_fix.c; it asks this to enable itself for the same key. */
int c690_native_screen_mode(void);

typedef struct {
    unsigned long long native_calls;
    unsigned long long mode1_calls;
    unsigned long long shadow_calls;
    unsigned long long shadow_mismatches;
    unsigned long long shadow_ls_bad;
    unsigned long long shadow_gpr_bad;
    unsigned long long union_added_tris;
    unsigned long long union_cap_hits;
    unsigned long long union_skipped;
    unsigned long long lifted_ns;
    unsigned long long native_ns;
    /* fused holes pass (PS3RECOMP_C690_HOLES_NATIVE) */
    unsigned long long holes_calls;
    unsigned long long holes_runs;          /* sample-grid stage evaluations */
    unsigned long long holes_added_tris;
    unsigned long long holes_cap_skipped;
    unsigned long long holes_skip_setup;
    unsigned long long holes_fallback;
    unsigned long long holes_ns;
} c690_native_screen_counters;

void c690_native_screen_get_counters(c690_native_screen_counters* out);

/* Test hook: run the native stage once on the context (no switches). */
void c690_native_screen_run_once(struct spu_context* ctx);

/* Fused holes pass (PS3RECOMP_C690_HOLES_NATIVE).  The 0xD080 hook stashes
 * the sample-grid list; c690_cull_fix_end() calls the merge after the caller
 * has written the game-grid count.  Returns the number of triangles appended;
 * the caller adds 3*added to the kept index count. */
int c690_native_screen_holes_active(void);
uint32_t c690_native_screen_holes_merge(struct spu_context* ctx, uint32_t ptr,
                                        uint32_t cnt_indices,
                                        uint32_t cap_indices);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_NATIVE_SCREEN_H */
