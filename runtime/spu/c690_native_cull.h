/*
 * Native replacement for the c690 tree/cull stage (LS 0xFDA0).
 *
 * The lifted stage is a 45-block SPU state machine. c690_native_stage.inc is
 * a bit-exact flattening of those blocks (tools/c690_ref/gen_native_stage.py):
 * the register file stays in locals for the whole traversal instead of being
 * saved/reloaded through ctx->gpr at every continuation. The SPU arithmetic
 * and local-store helpers are unchanged; the one deviation is that the native
 * module runs under MXCSR FTZ|DAZ and so only canonicalises the sign of zero
 * results (the lifted module masks every operand). Both are proven equal on
 * the captured corpus (docs/edge-native-step2.md).
 *
 * Switches (env and live control file):
 *   PS3RECOMP_C690_NATIVE_CULL=1    run the native stage instead of the lifted one
 *   PS3RECOMP_C690_NATIVE_SHADOW=1  run both, compare, use the lifted result
 *
 * Fallback: any entry whose layout is not the validated c690 cull layout calls
 * the lifted stage. Fallbacks are counted and reported at exit.
 */
#ifndef SPU_C690_NATIVE_CULL_H
#define SPU_C690_NATIVE_CULL_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* True when either switch is on (live control file wins over the env). */
int c690_native_cull_enabled(void);

/* True when the hook must be consulted: a switch is on, or a live control
 * file exists so a switch could turn on in the middle of a lifted stage. */
int c690_native_cull_maybe(void);

/* Called at the outermost 0xFDA0 entry, before the lifted body. Returns 1
 * when the native stage ran and the caller must not execute the body. In
 * shadow mode it stashes the entry image and returns 0. Recursive entries
 * return 0 immediately; pair the call with c690_native_cull_leave(). */
int c690_native_cull_enter(struct spu_context* ctx);

/* Called after the lifted body on the outermost entry. In shadow mode it
 * re-runs the native kernel on the stashed entry image and compares the full
 * local store and all 128 GPRs with the lifted result. */
void c690_native_cull_leave(struct spu_context* ctx);

/* Counters (native runs, fallbacks, shadow comparisons/mismatches). */
typedef struct {
    unsigned long long native_calls;
    unsigned long long fallbacks;
    unsigned long long shadow_calls;
    unsigned long long shadow_mismatches;
    unsigned long long shadow_ls_bytes_bad;
    unsigned long long shadow_gpr_bad;
} c690_native_cull_counters;

void c690_native_cull_get_counters(c690_native_cull_counters* out);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_NATIVE_CULL_H */
