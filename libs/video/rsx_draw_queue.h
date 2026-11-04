/*
 * ps3recomp - single-producer/single-consumer byte ring for the RSX draw
 * record worker (PS3RECOMP_WALKER_MT).
 *
 * The engine (producer) copies each immutable draw packet into the ring and
 * publishes it; one worker thread (consumer) executes the packet's backend
 * calls in the same order the guest issued them. The shape follows the MIT
 * inFAMOUS 2 port's rsx_be_thread.c: a single ordered queue, copied payloads
 * held until the backend has consumed them, and synchronous calls that drain
 * the queue before touching the backend themselves.
 *
 * Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
 * contributors, MIT.
 */
#ifndef PS3RECOMP_RSX_DRAW_QUEUE_H
#define PS3RECOMP_RSX_DRAW_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_record_queue rsx_record_queue;

/* Execute one record. The pointer is valid only for the call. */
typedef void (*rsx_record_fn)(void* ctx, const void* rec, uint32_t size);

/* A ring of `cap` bytes (rounded up to 16). NULL on allocation failure. */
rsx_record_queue* rsx_record_queue_new(uint32_t cap);
void rsx_record_queue_destroy(rsx_record_queue* q);

/* Start/stop the consumer thread. exec runs on it. */
int  rsx_record_queue_start(rsx_record_queue* q, rsx_record_fn exec, void* ctx);
void rsx_record_queue_stop(rsx_record_queue* q);

/* Copy one record and make it visible to the consumer. Returns 0 when the
 * record is larger than the ring or the ring has no room; the caller either
 * drains and retries or executes the record inline. */
int  rsx_record_queue_push(rsx_record_queue* q, const void* rec, uint32_t size);

/* Wait until every published record has been executed. */
void rsx_record_queue_sync(rsx_record_queue* q);

/* Statistics, producer side when called before stop. */
typedef struct {
    uint64_t records, bytes, full, syncs, executed;
    uint32_t cap, used;
} rsx_record_queue_stats;
void rsx_record_queue_get_stats(rsx_record_queue* q, rsx_record_queue_stats* out);

/* ---- the record header -------------------------------------------------- */

#define RSX_RECORD_ALIGN 16u
#define RSX_RECORD_HDR   8u
#define RSX_RECORD_WRAP  0xFFFFFFFFu
typedef struct {
    uint32_t size;   /* total bytes including this header, or RSX_RECORD_WRAP */
    uint32_t pad;
} rsx_record_hdr;

static inline uint32_t rsx_record_need(uint32_t payload)
{
    return (RSX_RECORD_HDR + payload + (RSX_RECORD_ALIGN - 1u)) &
           ~(RSX_RECORD_ALIGN - 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_DRAW_QUEUE_H */
