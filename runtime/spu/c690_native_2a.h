/*
 * Native c690 0x2A field gather/pack (position-first record classes).
 *
 * The lifted 0x2A assembler (0x10F78) builds the SPU index pre-pass and then
 * runs four per-field passes over the record list:
 *
 *   0x7840  position copy      (pos.xyz f32, exact)
 *   0x7FC0  normal 11:11:10    (3 floats)
 *   0x7AC0  fixed s16 x 2/3/4  (scale from the record layout)
 *   0x7540  f16 x 2            (u, v)
 *
 * Each pass receives everything it needs in GPRs: r3 source array base,
 * r4 destination (record 0 + field offset), r5 record stride, r7 index list
 * (u16 byte offsets, or not a list -> identity k*16), r11 record count,
 * r6 component count and r22 field kind.  The native pass reads the same
 * arguments, gathers the same source entries and writes only the field bytes
 * with the decoded pack rules; the driver (descriptor parse, index pre-pass,
 * allocator, cursor update) stays lifted, so its LS/register side effects are
 * unchanged.  Bytes the field pass does not own (the SPU-written attribute
 * word at +28..31) are already correct in LS when the pass runs.
 *
 * Live key: PS3RECOMP_C690_NATIVE_2A (default 0).  Unknown record classes
 * (attribute-first 0x00A00004, the odd 12-byte descriptor) and any argument
 * that does not match the decoded layouts fall back to the lifted pass.
 *
 * Exactness (edge-exactness.md policy): positions/source bytes exact, packed
 * normal/s16/f16 <= 1 LSB.  See docs/edge-native-2a.md.
 */
#ifndef C690_NATIVE_2A_H
#define C690_NATIVE_2A_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* True when the live key or the environment switch is on. */
int c690_native_2a_enabled(void);

/* Called at the entry of one of the four field passes (addr is its LS
 * address).  Returns 1 when the native pass ran and published the return PC;
 * 0 when the caller must run the lifted body. */
int c690_native_2a_try(struct spu_context* ctx, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* C690_NATIVE_2A_H */
