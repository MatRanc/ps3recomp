/* dxt_capture.h — env-gated corpus capture for the WWS DXT job (workload 0).
 *
 * Off by default: with PS3RECOMP_DXT_CAPTURE unset nothing is allocated, no
 * file is opened, and the DMA path costs one predictable branch. See
 * dxt_capture.c for the record format and docs/dxt-native-step2.md for the
 * capture that uses it.
 */
#ifndef DXT_CAPTURE_H
#define DXT_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* Fast gate for spu_dma.h: nonzero only while a capture directory is armed. */
extern int g_dxt_capture_on;

/* Called around one policy-module run. Only wid 0 (Wws_Job/DXTJob) arms the
 * per-thread DMA spool. Cheap when the gate is off. */
void dxt_capture_policy_begin(const struct spu_context* ctx, uint32_t wid,
                              uint64_t wkl_data, uint32_t spu_num);
void dxt_capture_policy_end(const struct spu_context* ctx);

/* Called from the MFC transfer paths (spu_dma.h) with the payload the
 * transfer carries: guest memory for a GET, local store for a PUT. */
void dxt_capture_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint64_t ea, uint32_t size, const uint8_t* data);

#ifdef __cplusplus
}
#endif

#endif /* DXT_CAPTURE_H */
