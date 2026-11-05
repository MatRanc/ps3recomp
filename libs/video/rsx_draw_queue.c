/*
 * ps3recomp - SPSC draw-record ring (see rsx_draw_queue.h).
 *
 * Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
 * contributors, MIT.
 */
#include "rsx_draw_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define RQ_PAUSE() _mm_pause()
#else
#define RQ_PAUSE() ((void)0)
#endif

#include <stdatomic.h>

struct rsx_record_queue {
    uint8_t* buf;
    uint32_t cap;

    /* Producer-owned write cursor and the cursor it last published. The
     * published cursor and the read cursor are C11 atomics: the release store
     * of `pub` publishes the record bytes and the consumer's acquire load of
     * it is the other half of the hand-off (and what TSAN models). */
    _Alignas(64) uint64_t w;
    _Alignas(64) _Atomic uint64_t pub;
    /* Consumer-owned read cursor. */
    _Alignas(64) _Atomic uint64_t r;
    _Alignas(64) _Atomic int stop;

    pthread_t th;
    int started;
    rsx_record_fn exec;
    void* ctx;

    uint64_t records, bytes, full, syncs;
    uint64_t executed;
};

static inline uint8_t* rq_at(rsx_record_queue* q, uint64_t pos)
{
    return q->buf + (size_t)(pos % q->cap);
}

rsx_record_queue* rsx_record_queue_new(uint32_t cap)
{
    if (cap < 4096u) cap = 4096u;
    cap = (cap + (RSX_RECORD_ALIGN - 1u)) & ~(RSX_RECORD_ALIGN - 1u);
    rsx_record_queue* q = (rsx_record_queue*)calloc(1, sizeof *q);
    if (!q) return NULL;
    q->buf = (uint8_t*)malloc(cap);
    if (!q->buf) { free(q); return NULL; }
    q->cap = cap;
    return q;
}

void rsx_record_queue_destroy(rsx_record_queue* q)
{
    if (!q) return;
    rsx_record_queue_stop(q);
    free(q->buf);
    free(q);
}

static void rq_publish(rsx_record_queue* q)
{
    atomic_store_explicit(&q->pub, q->w, memory_order_release);
}

static uint64_t rq_load(_Atomic uint64_t* p, int acquire)
{
    return atomic_load_explicit(p, acquire ? memory_order_acquire
                                           : memory_order_relaxed);
}

static void rq_sleep(void)
{
    /* Yield, never a timed sleep: a sync call right after a publish must not
     * wait a timer tick for the worker to notice it. */
    sched_yield();
}

static void* rq_worker(void* arg)
{
    rsx_record_queue* q = (rsx_record_queue*)arg;
    uint64_t r = 0;
    for (;;) {
        /* Acquire on every iteration: that load is the hand-off for the
         * record bytes the producer wrote before its release store. Publish
         * `r` after each record so the producer can reuse the slot. */
        const uint64_t pub = rq_load(&q->pub, 1);
        if (r != pub) {
            const rsx_record_hdr* h = (const rsx_record_hdr*)rq_at(q, r);
            uint32_t size = h->size;
            if (size == RSX_RECORD_WRAP) {
                /* The producer padded to the end of the ring. */
                r += (uint32_t)(q->cap - r % q->cap);
            } else {
                if (q->exec) q->exec(q->ctx, (const uint8_t*)h + RSX_RECORD_HDR,
                                     size - RSX_RECORD_HDR);
                r += size;
                q->executed++;
            }
            atomic_store_explicit(&q->r, r, memory_order_release);
            continue;
        }
        if (atomic_load_explicit(&q->stop, memory_order_acquire)) {
            /* Execute whatever was published before the stop. */
            if (r == rq_load(&q->pub, 1)) break;
            continue;
        }
        /* Idle: spin for a short while, then sleep. */
        for (int i = 0; i < 2000 && r == rq_load(&q->pub, 0); i++) RQ_PAUSE();
        if (r == rq_load(&q->pub, 1) && !atomic_load_explicit(&q->stop, memory_order_relaxed))
            rq_sleep();
    }
    return NULL;
}

int rsx_record_queue_start(rsx_record_queue* q, rsx_record_fn exec, void* ctx)
{
    if (!q || q->started) return 0;
    q->exec = exec;
    q->ctx = ctx;
    if (pthread_create(&q->th, NULL, rq_worker, q) != 0) return -1;
    q->started = 1;
    return 0;
}

void rsx_record_queue_stop(rsx_record_queue* q)
{
    if (!q || !q->started) return;
    rq_publish(q);
    atomic_store_explicit(&q->stop, 1, memory_order_release);
    pthread_join(q->th, NULL);
    q->started = 0;
}

int rsx_record_queue_push(rsx_record_queue* q, const void* rec, uint32_t size)
{
    if (!q || !rec || !q->started) return 0;
    const uint32_t need = rsx_record_need(size);
    if (need > q->cap) return 0;

    rq_publish(q);
    uint64_t pos = q->w % q->cap;
    if (pos + need > q->cap) {
        /* Pad to the end so the consumer sees one contiguous record. */
        const uint32_t pad = (uint32_t)(q->cap - pos);
        const uint64_t end = q->w + pad + need;
        if (end - rq_load(&q->r, 1) > q->cap) {
            q->full++;
            return 0;
        }
        rsx_record_hdr* wh = (rsx_record_hdr*)rq_at(q, q->w);
        wh->size = RSX_RECORD_WRAP;
        wh->pad = 0;
        q->w += pad;
        pos = 0;
    }
    if (q->w + need - rq_load(&q->r, 1) > q->cap) {
        q->full++;
        return 0;
    }
    rsx_record_hdr* h = (rsx_record_hdr*)rq_at(q, q->w);
    h->size = need;
    h->pad = 0;
    memcpy((uint8_t*)h + RSX_RECORD_HDR, rec, size);
    q->w += need;
    q->records++;
    q->bytes += size;
    rq_publish(q);
    return 1;
}

void rsx_record_queue_sync(rsx_record_queue* q)
{
    if (!q || !q->started) return;
    rq_publish(q);
    q->syncs++;
    const uint64_t upto = q->w;
    int spins = 0;
    while (rq_load(&q->r, 1) < upto) {
        if (++spins < 20000) { RQ_PAUSE(); continue; }
        spins = 0;
        sched_yield();
    }
}

void rsx_record_queue_get_stats(rsx_record_queue* q, rsx_record_queue_stats* out)
{
    if (!q || !out) return;
    memset(out, 0, sizeof *out);
    out->records = q->records;
    out->bytes = q->bytes;
    out->full = q->full;
    out->syncs = q->syncs;
    out->executed = q->executed;
    out->cap = q->cap;
    out->used = (uint32_t)(q->w - rq_load(&q->r, 0));
}
