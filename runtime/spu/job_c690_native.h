#ifndef SPU_JOB_C690_NATIVE_H
#define SPU_JOB_C690_NATIVE_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native C / AVX2 replacement for MLB The Show 16 SPU job module
 * c690c2f7b09a, function 0x7C18 (vertex attribute quantization and packing loop).
 *
 * Signature matches lifted SPU function: void f(spu_context* ctx).
 * Reads and writes ctx->gpr and ctx->ls exactly as the lifted code does,
 * setting ctx->pc = ctx->gpr[26]._u32[0] on completion.
 */
void native_spu_func_00007C18(spu_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPU_JOB_C690_NATIVE_H */
