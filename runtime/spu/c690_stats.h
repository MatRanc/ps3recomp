/*
 * ps3recomp - measurement-only counters for the c690 SPU job module.
 *
 * PS3RECOMP_C690_STATS=1 enables them. They are off by default and change no
 * dispatch decision: every call site is a single
 * `if (__builtin_expect(g_c690_stats, 0))` around an out-of-line hook.
 *
 * Hooks (see c690_stats.c for the model and the exclusions):
 *   c690_stats_on_lookup()  spu_lifted_lookup(), for indirect dispatch, the
 *                           c690 job entry/end command, and handler opcodes.
 *   c690_stats_on_direct()  SPU_DIRECT (spu_context.h), for direct lifted
 *                           function entries that never reach the resolver.
 *
 * Only contexts with image_id == 6 (the c690 job image) are counted.
 */
#ifndef SPU_C690_STATS_H
#define SPU_C690_STATS_H

#include <stdint.h>

/* spu_context is defined by spu_context.h; the hooks only read image_id/spu_id. */
struct spu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* Set once at startup from PS3RECOMP_C690_STATS=1. Read-only afterwards. */
extern int g_c690_stats;

/* PS3RECOMP_C690_GAP=1: inter-job turnover breakdown (measurement only). */
extern int g_c690_gap;
uint64_t c690_gap_now(void);
void c690_gap_on_lookup(const struct spu_context* ctx, uint32_t lsa, uint64_t now);
void c690_gap_on_direct(const struct spu_context* ctx, uint32_t lsa, uint64_t now);
void c690_gap_on_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint32_t size);
void c690_gap_on_atomic(const struct spu_context* ctx, int kind, int ok);
void c690_gap_on_overlay(int kind, int ok);

/* PS3RECOMP_C690_OP21=1: measure the 0x21 emit/ring command only
 * (handler LS 0x13080). Per call it splits the inclusive time across the
 * handler's sub-selector targets (0x13178/0x13140/0x131A0/0x131F8/0x13240/
 * 0x13248/0x13250/0x13288), and counts the DMA bytes and time, GETLLAR/PUTLLC,
 * tag-status reads, decrementer reads and trampoline steps made while the
 * command is current. It also splits the command window by its first
 * sub-selector (0x13140 sel1, 0x131A0 sel7) and reports a per-LS function
 * table inside each window, plus the all-class synchronous DMA (copy) time
 * charged to the current command. Measurement only; off by default. */
extern int g_c690_op21;
void c690_op21_dma_begin(const struct spu_context* ctx, int is_put, uint32_t size);
void c690_op21_dma_end(const struct spu_context* ctx);
void c690_op21_on_slow(const struct spu_context* ctx, int is_put, uint32_t size);
void c690_op21_on_atomic(const struct spu_context* ctx, int kind, int ok);
void c690_op21_on_tagstat(const struct spu_context* ctx);
void c690_op21_on_dec(const struct spu_context* ctx);
void c690_op21_on_rchatom(const struct spu_context* ctx, int stat);

/* PS3RECOMP_C690_HASH=<stride>: sample every stride-th c690 job and fold the
 * bytes of its DMA GETs (input) and PUTs (output) into FNV-1a hashes. Zero
 * when off; the DMA fast path tests it with one predicted-not-taken branch
 * before calling c690_stats_on_transfer(). */
extern int g_c690_hash_on;

/* A lifted target is about to be entered through spu_lifted_lookup(). `lsa`
 * is the LS target; NULL ctx and other images are ignored. */
void c690_stats_on_lookup(const struct spu_context* ctx, uint32_t lsa);

/* A lifted target is about to be entered through SPU_DIRECT(). */
void c690_stats_on_direct(const struct spu_context* ctx, uint32_t lsa);

/* One completed MFC transfer for the current c690 job. `is_put` = data leaves
 * LS for guest EA (output), otherwise it arrives in LS (input). Read-only. */
void c690_stats_on_transfer(const struct spu_context* ctx, int is_put,
                            uint32_t lsa, uint64_t ea, uint32_t size);

/* Print the accumulated report. Registered with atexit() when enabled; also
 * safe to call explicitly (it does nothing when the counters are off). */
void c690_stats_report(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_C690_STATS_H */
