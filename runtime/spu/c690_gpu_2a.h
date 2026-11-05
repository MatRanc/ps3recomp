/*
 * c690 Edge 0x2A: GPU ring binding for the position-first 32-byte class.
 *
 * The SPU keeps the command parser, the index pre-pass, the record pack, the
 * cursor/allocator bookkeeping, the attribute word at record +28..31 and the
 * ring PUT, so the lifted path is byte-for-byte unchanged.  For a 0x2A
 * command whose output is the decoded 32-byte A/B contract it exports the four
 * planar source arrays and the per-record offset list.  The renderer runs the
 * offline c690_2a_32b Vulkan kernel over them; the draw engine's guest
 * resolver then serves the GPU-written range for that mesh-ring span instead
 * of the guest bytes.  Any other class runs the lifted path and nothing is
 * exported.
 *
 * The record pack itself is not skipped yet: the handler reads back the
 * records it writes, so suppressing the stores changes its own output.  The
 * next stage replaces the per-stream pack (0x10C70/0x109C0), not the stores.
 *
 * Live key: PS3RECOMP_C690_GPU_2A, default 0 (nothing runs when off).
 */
#ifndef C690_GPU_2A_H
#define C690_GPU_2A_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* One output range exported by an SPU lane.  Ownership of offs/arrays/seed
 * passes to the sink. */
typedef struct c690_gpu_2a_job {
    uint32_t ea;        /* ring EA of the first record (main memory) */
    uint32_t count;     /* records */
    uint32_t stride;    /* 32 */
    uint32_t extent;    /* bytes per source array (all four are this long) */
    uint16_t* offs;     /* count: per-record byte offset into every array */
    uint8_t* arrays;    /* 4 * extent: pos, nrm, tan, uv (SPU LS byte order) */
    uint8_t* seed;      /* count * 32: exact SPU record bytes (attr + fallback) */
    uint32_t seq;       /* diagnostics: the capture sequence number */
} c690_gpu_2a_job;

/* Renderer hook.  Called from an SPU lane thread with a fully owned job;
 * returns 1 if the callee took ownership, 0 if it did not (the caller frees).
 * The renderer runs the compute and installs the range at the next drain. */
typedef int (*c690_gpu_2a_sink_fn)(c690_gpu_2a_job* job);
void c690_gpu_2a_set_sink(c690_gpu_2a_sink_fn fn);

/* SPU hooks, called from the generated 0x10F78 wrapper. */
void c690_gpu_2a_enter(struct spu_context* ctx);
void c690_gpu_2a_exit(struct spu_context* ctx);

/* PUT hook: the job's submit epilogue copies LS [lsa, lsa+size) to EA. */
void c690_gpu_2a_put(const struct spu_context* ctx, uint32_t lsa,
                     uint64_t ea, uint32_t size);

/* Nonzero when the live key is on.  Cached after the first 0x2A entry. */
extern _Atomic int g_c690_gpu_2a_on;
int c690_gpu_2a_enabled(void);

/* Diagnostics: a counter snapshot line on stderr (every ~2 s). */
void c690_gpu_2a_stats_maybe(void);

/* Pure classifier used by the unit test.  Returns 1 and fills offs[] when the
 * records match format A (u16 list at cmd0) or format B (identity k*16). */
int c690_gpu_2a_classify(const uint8_t* ls, const uint8_t* recs, uint32_t r3,
                         uint32_t count, uint32_t cmd0, uint16_t* offs,
                         int* format_out);

#ifdef __cplusplus
}
#endif

#endif
