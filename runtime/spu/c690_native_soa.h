/* Native c690 normal/tangent regeneration; see c690_native_soa.c. */
#ifndef SPU_C690_NATIVE_SOA_H
#define SPU_C690_NATIVE_SOA_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the generated wrapper of lsa. 1 = handled (natively, or in shadow
 * mode by running body and comparing); 0 = the wrapper runs body itself. */
int c690_soa_try(struct spu_context* ctx, uint32_t lsa, int variant,
                 void (*body)(struct spu_context*));

/* Offline gate: run lsa's kernel on ls with ctx's argument registers. */
int c690_soa_run_ls(const struct spu_context* ctx, uint8_t* ls, uint32_t lsa, int variant);

#ifdef __cplusplus
}
#endif

#endif
