/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * An SPU that issues GETLLAR holds a reservation on a 128-byte line and then
 * spins, waiting for someone else to change it. On real hardware a store by
 * ANY other processor to that line kills the reservation and raises
 * SPU_EVENT_LR on the reserving SPU, which is how a parked SPURS idle service
 * learns that the PPU has posted work. Emulated, the two halves live on
 * different host threads with nothing between them, so the SPU half alone gets
 * PUTLLC right (it compares the line before committing) while the PPU half
 * never tells anyone it wrote. The service sleeps through the signal.
 *
 * This is the missing half. Every line an SPU reserves is marked in a bitmap;
 * the PPU store paths (runtime/ppu/ppu_memory.h and runtime/ppu/ppu_loader.cpp)
 * check the bitmap, and on a hit commit the store under the same lock-line lock
 * the SPU's GETLLAR/PUTLLC transaction takes, then notify. That does two things
 * at once: the store can no longer land inside a PUTLLC's compare/commit window
 * (where it would be silently reverted), and the reserving SPU gets its event.
 *
 * The bitmap check is a shift and a load against a global that stays zero until
 * an SPU actually reserves something, so a title that never runs SPU code pays
 * one predictable branch per store.
 */

#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU external event: lock-line reservation lost. */
#define SPU_EVENT_LR  0x400u

/* The lock-line lock, striped by line. Every lock-line transaction, SPU and
 * PPU alike -- the SPU's GETLLAR snapshot and PUTLLC compare-and-commit, a
 * PPU store or stwcx to a line some SPU has reserved, an MFC PUT -- holds the
 * stripe of each 128-byte line it writes (spu_lockline_mask_ea/_range), so
 * transactions on different lines run in parallel and those on one line
 * serialize exactly as before. The critical sections are two 128-byte memcpy
 * and a memcmp.
 *
 * spu_lockline_lock/unlock take EVERY stripe: the old global lock, for the
 * operations that must exclude all line traffic (removing a context from the
 * holder list, the watch list) and for callers that do not name an address.
 * Several stripes are always taken in ascending order, so no two holders
 * deadlock. Not recursive: a holder must not take any stripe again. */
#define SPU_LL_STRIPES 64
void     spu_lockline_lock(void);
void     spu_lockline_unlock(void);
uint64_t spu_lockline_mask_ea(uint32_t ea);
uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len);
void     spu_lockline_lock_mask(uint64_t mask);
void     spu_lockline_unlock_mask(uint64_t mask);

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* GETLLAR fast path: publish ctx's reservation of ea WITHOUT the lock-line
 * lock, when ctx is already a registered holder and ea's line already marked.
 * Returns 0 when the caller must take the lock and call spu_coh_reserve
 * instead. On 1 the caller copies the line, also without the lock. */
int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Expects the stripe of `ea`'s line held, so that the store this is reporting and the
 * reservation state it clears are seen by an SPU's PUTLLC as one transaction. */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);

/* Scoped host waiters for real writes to a guest 128-byte line. Add and
 * remove run under every stripe (spu_lockline_lock); callbacks run under the
 * written line's stripe. A watch remains caller-owned
 * until remove returns; callbacks must not re-enter coherence or alter watches.
 * They may wake a host waiter, but must not manufacture guest state. */
typedef struct spu_coh_watch {
    uint32_t line;
    void (*wake)(void* opaque);
    void* opaque;
    struct spu_coh_watch* next;
} spu_coh_watch;
void spu_coh_watch_add(spu_coh_watch* watch, uint32_t ea,
                       void (*wake)(void*), void* opaque);
void spu_coh_watch_remove(spu_coh_watch* watch);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
