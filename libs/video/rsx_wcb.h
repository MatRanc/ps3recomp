/*
 * ps3recomp - guest-memory coherence for render targets read by the CPU/SPU
 *
 * "Write Color Buffers" (WCB) on demand. The draw engine renders on the host
 * GPU, so the bytes RSX would have left in guest memory exist only there. A
 * title that reads a rendered target from the PPU or an SPU (MLB The Show 16's
 * generated faces, crowd sprite sheets) sees whatever guest memory last held.
 * Copying every dirty target back at every sync point is correct but costs a
 * GPU stall and a PCIe copy per target, and most targets (shadow maps, the
 * G-buffer, post chains) are only ever sampled again by the GPU.
 *
 * This module is the read-tracking half. At a sync point the engine hands it
 * the host range of a target the guest may now read; the module takes all
 * access to those pages away (PROT_NONE / PAGE_NOACCESS). Nothing is copied.
 * The first guest access to one of those pages -- a PPU load or store, an SPU
 * DMA, a runtime HLE call touching the buffer -- faults, and the faulting
 * thread asks the owner thread (the one that drives the draw engine and the
 * host GPU API) to copy that target into guest memory, waits for it, and
 * retries the access. A target nobody touches is never copied.
 *
 * The module works purely in HOST addresses and knows nothing about render
 * targets: a region is a cookie and a page span. The engine supplies the
 * copy (rsx_wcb_resolve_fn).
 *
 * Ordering. A reader may never see a stale or partially written copy:
 *   - the pages stay inaccessible to every thread until the copy is complete;
 *   - on Linux the copy is written THROUGH the protection, via /proc/self/mem
 *     (rsx_wcb_store), and the pages are reopened only afterwards, so no
 *     thread can observe a half-written target;
 *   - where that writer is unavailable (Windows, macOS, a kernel that forbids
 *     it) the pages are reopened just before the copy and a concurrent reader
 *     on another thread could see it half written. rsx_wcb_forced_store()
 *     reports which applies; docs/wcb-async.md lists it as a risk.
 *
 * Threads. Only the owner thread may call the resolve function (the host GPU
 * API is single-threaded here). A fault on the owner thread resolves inline;
 * a fault on any other thread is a request the owner services from
 * rsx_wcb_service(), which the FIFO walker calls between methods and in its
 * idle wait. A request that is not serviced within the timeout reopens the
 * pages with stale contents and is counted: a hang is worse than a wrong
 * texel, and the counter says it happened.
 */
#ifndef RSX_WCB_H
#define RSX_WCB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Regions the module tracks at once: every colour and depth target of the
 * engine with room to spare. */
#define RSX_WCB_MAX_REGIONS 256

/* Why the resolve function runs. */
#define RSX_WCB_REASON_FAULT    0   /* a guest access faulted on the region   */
#define RSX_WCB_REASON_EXPLICIT 1   /* rsx_wcb_access on the owner thread     */
#define RSX_WCB_REASON_REQUEST  2   /* another thread's fault or access       */
#define RSX_WCB_REASON_FLUSH    3   /* rsx_wcb_resolve_all (shutdown, eviction) */

/* Copy the region `cookie` into guest memory with rsx_wcb_store. Runs on the
 * owner thread only, never with the module's lock held. The module releases
 * the region (reopens the pages) when it returns, whether the copy worked or
 * not. */
typedef void (*rsx_wcb_resolve_fn)(void* user, uint32_t cookie, int reason);

typedef struct rsx_wcb_counters {
    uint64_t protected_regions;  /* regions armed (rsx_wcb_protect succeeded) */
    uint64_t protect_failures;   /* the host refused the protection           */
    uint64_t released;           /* regions released, any reason              */
    uint64_t faults;             /* guest faults that landed on a region      */
    uint64_t late_faults;        /* faults on a page reopened meanwhile       */
    uint64_t resolves;           /* resolve-function calls                    */
    uint64_t inline_resolves;    /*   ... of which on the owner thread at once */
    uint64_t requests;           /*   ... of which another thread waited for   */
    uint64_t waits;              /* times a non-owner thread blocked           */
    uint64_t wait_ns;            /* total time blocked                         */
    uint64_t wait_max_ns;        /* longest single block                       */
    uint64_t timeouts;           /* requests given up on (stale data)          */
} rsx_wcb_counters;

/* Set up (idempotent). `wake` nudges the owner thread out of its idle wait;
 * it may be NULL. Installs the fault handler on first use. Returns 0. */
int  rsx_wcb_init(rsx_wcb_resolve_fn fn, void* user, void (*wake)(void));
/* Reopen every page and forget every region, without copying. */
void rsx_wcb_shutdown(void);
/* The calling thread becomes the owner: it runs the resolve function. */
void rsx_wcb_set_owner_thread(void);
int  rsx_wcb_is_owner_thread(void);

/* Arm [host, host + len): take away all access to the host pages covering it
 * until the region is released. A cookie already armed is left as it is.
 * Returns 0 when armed (or already armed), -1 when the host refused, in
 * which case the caller must copy now instead. */
int  rsx_wcb_protect(uint32_t cookie, void* host, size_t len);
int  rsx_wcb_is_pending(uint32_t cookie);
uint32_t rsx_wcb_pending_count(void);
/* Forget a region and reopen the pages no other region still covers. Does
 * not copy. */
void rsx_wcb_release(uint32_t cookie);

/* Write guest bytes while their pages may still be protected. Owner thread,
 * from inside the resolve function. Returns 0 on success. */
int  rsx_wcb_store(void* host_dst, const void* src, size_t n);
/* 1 when rsx_wcb_store writes through the protection (no reopen window). */
int  rsx_wcb_forced_store(void);

/* Make [host, host + len) safe to access from the calling thread: resolve
 * every armed region whose pages it touches (inline on the owner thread,
 * by request elsewhere). Cheap when nothing is armed. */
void rsx_wcb_access(const void* host, size_t len);
/* Resolve one armed region now (owner thread). Returns 1 if it was armed. */
int  rsx_wcb_resolve_cookie(uint32_t cookie, int reason);
/* Resolve every armed region now (owner thread). */
void rsx_wcb_resolve_all(int reason);
/* Owner thread: resolve regions other threads asked for. Returns how many. */
int  rsx_wcb_service(void);
/* Nonzero when a request is waiting for the owner. One relaxed load. */
int  rsx_wcb_requests_pending(void);

/* The fault handler's core, for hosts that dispatch faults themselves:
 * returns 1 when the access should be retried, 0 when the fault is not
 * ours. */
int  rsx_wcb_on_fault(uintptr_t addr);

void rsx_wcb_counters_get(rsx_wcb_counters* out);

#ifdef __cplusplus
}
#endif

#endif /* RSX_WCB_H */
