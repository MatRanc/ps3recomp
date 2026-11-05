/*
 * Native c690 0x21 emit command (whole command).
 *
 * The lifted handler 0x13080 selects a sub-selector and then runs a large
 * call/tail-call graph (format setup -> transform/pack/normalize -> emit).
 * c690_native_21_try() replaces the whole command when the live key is on:
 * runtime/spu/c690_native_21_stage.inc is a flattened transliteration of the
 * reachable lifted blocks (same LS/SPU helpers), so the output is the lifted
 * output by construction.  Any unsupported entry/target hands control back to
 * the lifted code with the exact register state.
 *
 * PS3RECOMP_C690_NATIVE_21 is a live key, default 0.
 * PS3RECOMP_C690_21_CAPTURE=<dir> records <dir>/pNNN_{in,out}.bin pairs for the
 * offline gate (entry/exit full LS+GPR); it is inert unless set.
 */
#ifndef C690_NATIVE_21_H
#define C690_NATIVE_21_H

#include <stdint.h>

struct spu_context;

/* Returns 1 when the command ran natively (or handed the rest to the lifted
 * resolver), 0 when the caller must run the lifted 0x13080 body. */
int c690_native_21_try(struct spu_context* ctx, uint32_t entry);

/* Corpus capture hooks, called by the generated wrapper around 0x13080. */
int  c690_native_21_capture_begin(struct spu_context* ctx, uint32_t entry);
void c690_native_21_capture_end(struct spu_context* ctx);

int c690_native_21_enabled(void);

#endif /* C690_NATIVE_21_H */
