/*
 * ps3recomp - corpus capture for the c690 SPU geometry job.
 *
 * PS3RECOMP_C690_CORPUS=<dir> enables a game-data capture: for every sampled
 * c690 job it records the job descriptor, every DMA GET (with its guest EA)
 * and PUT (with its guest EA), the local-store image at the entry and exit of
 * the tree/cull stage (LS 0xFDA0), and the command opcode histogram.
 *
 * This is measurement-only. It never changes a dispatch or a transfer; every
 * hook is a predicted-not-taken branch on g_c690_corpus_on when disabled.
 */
#ifndef SPU_C690_CORPUS_H
#define SPU_C690_CORPUS_H

#include <stdint.h>

struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when PS3RECOMP_C690_CORPUS names a directory. Read-only after the
 * first hook; the DMA fast path tests it before calling out of line. */
extern int g_c690_corpus_on;

/* Job boundaries, called from the c690_stats hooks. */
void c690_corpus_job_begin(const struct spu_context* ctx, uint32_t desc_ea,
                           uint32_t frame);
void c690_corpus_job_end(const struct spu_context* ctx);

/* One completed MFC transfer. `src` is the bytes that move: the guest image
 * for a GET (before it is copied in), the local-store image for a PUT. */
void c690_corpus_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint64_t ea, uint32_t size, const uint8_t* src);

/* Command opcode dispatched by the c690 parser (0x10700). */
void c690_corpus_note_op(const struct spu_context* ctx, unsigned op);

/* Stage function boundaries (0xFDA0). The lifted module calls these from a
 * wrapper around the generated body; recursive entries are folded so only
 * the outermost call is recorded. */
void c690_corpus_stage_enter(const struct spu_context* ctx, uint32_t stage_lsa);
void c690_corpus_stage_exit(const struct spu_context* ctx, uint32_t stage_lsa);

/* Cull stage boundaries (0x25/0x26 -> 0x132E0, 0x2C -> 0x12C08), captured to
 * cull_NNN.bin when PS3RECOMP_C690_CULL_PAIRS is set. Same pair format as the
 * tree stage; the entry GPRs carry r4 and stage_lsa identifies the family. */
void c690_corpus_cull_enter(const struct spu_context* ctx, uint32_t stage_lsa);
void c690_corpus_cull_exit(const struct spu_context* ctx, uint32_t stage_lsa);

/* PS3RECOMP_C690_TAIL_CAPTURE=<dir>: stage-level corpus for the GPU tail.
 * At each top-level c690 command boundary (0x21, 0x25, 0x26, 0x2A) it records
 * the full local store + GPRs at handler entry (IN) and at the next command
 * handler / job end (OUT), the command opcode, the 0x21 sub-selector key
 * (selector + A730/BB80 bit) and the 128 command payload words it can reach.
 * A 16-byte-line write bitmap is derived from the IN/OUT diff. N pairs per
 * format key are kept (PS3RECOMP_C690_TAIL_PAIRS, default 8); every command
 * is still counted per key so the run yields exact job/time shares.
 * PS3RECOMP_C690_TAIL_TRIGGER=<path> starts sampling when that file appears.
 * It is a dedicated file: ab_live.sh's walker capture consumes and deletes
 * CAPTURE_PREFIX.trigger, so the two must never share a path. The tail hook
 * only tests for the file and never removes it.
 * Off by default; when off the hooks are one predicted-not-taken load. */
extern int g_c690_tail_on;
void c690_corpus_tail_job_begin(const struct spu_context* ctx);
void c690_corpus_tail_trace(const struct spu_context* ctx, uint32_t lsa,
                            int op, uint64_t now_ns);
/* 0x2A (LS 0x10F78) function entry/exit wrapper. The generated lifted module
 * calls these from the wrapper installed by tools/c690_ref/patch_cull_hooks.py,
 * so two back-to-back assembler commands each close one pair even when the
 * second is a direct dispatch. */
void c690_corpus_tail_2a_enter(const struct spu_context* ctx);
void c690_corpus_tail_2a_exit(const struct spu_context* ctx);
/* Generic function-entry/exit capture for the other tail commands: 0x21 at
 * the sub-selector bodies 0x13140/0x131A0 (key from r6 there) and 0x25/0x26
 * at the cull body 0x132E0. */
void c690_corpus_tail_cmd_enter(const struct spu_context* ctx, uint32_t lsa,
                                uint32_t op);
void c690_corpus_tail_cmd_exit(const struct spu_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_CORPUS_H */
