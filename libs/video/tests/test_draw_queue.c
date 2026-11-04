/*
 * rsx_draw_queue: SPSC record ring, ordering, wrap, full and sync/drain.
 *
 * The same ring carries the RSX draw packets when PS3RECOMP_WALKER_MT=1
 * WORKERS=1. This fixture checks the rules the engine relies on: records run
 * in push order on the worker, a sync returns only after every published
 * record has executed, a full ring refuses instead of overwriting, and a
 * record larger than the ring is refused (the engine then runs it inline).
 */
#include "rsx_draw_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mu;
    uint32_t n, bad, expected;
    uint32_t order[8192];
} exec_state;

static void exec_one(void* ctx, const void* rec, uint32_t size)
{
    exec_state* s = (exec_state*)ctx;
    if (size < 4) { pthread_mutex_lock(&s->mu); s->bad++; pthread_mutex_unlock(&s->mu); return; }
    uint32_t v;
    memcpy(&v, rec, 4);
    pthread_mutex_lock(&s->mu);
    if (v != s->expected) s->bad++;
    s->expected = v + 1;
    if (s->n < 8192) s->order[s->n] = v;
    s->n++;
    pthread_mutex_unlock(&s->mu);
}

static exec_state s_exec;

static void reset_exec(void)
{
    pthread_mutex_lock(&s_exec.mu);
    s_exec.n = s_exec.bad = s_exec.expected = 0;
    pthread_mutex_unlock(&s_exec.mu);
}

static int check(const char* what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        return 0;
    }
    return 1;
}

int main(void)
{
    memset(&s_exec, 0, sizeof s_exec);
    pthread_mutex_init(&s_exec.mu, NULL);
    int ok = 1;

    /* A ring big enough for many records: order and sync. */
    rsx_record_queue* q = rsx_record_queue_new(64u * 1024u);
    ok &= check("new", q != NULL);
    if (!q) return 1;
    ok &= check("start", rsx_record_queue_start(q, exec_one, &s_exec) == 0);

    for (uint32_t i = 0; i < 500; i++) {
        uint8_t payload[40];
        memset(payload, 0, sizeof payload);
        memcpy(payload, &i, 4);
        if (!rsx_record_queue_push(q, payload, (uint32_t)sizeof payload)) {
            fprintf(stderr, "FAIL push %u\n", i);
            ok = 0;
            break;
        }
    }
    rsx_record_queue_sync(q);
    pthread_mutex_lock(&s_exec.mu);
    ok &= check("all executed", s_exec.n == 500);
    ok &= check("in order", s_exec.bad == 0);
    pthread_mutex_unlock(&s_exec.mu);

    /* Wrap: small ring, push more than its capacity in total. */
    rsx_record_queue* w = rsx_record_queue_new(4096u);
    ok &= check("wrap new", w != NULL);
    if (w) {
        reset_exec();
        ok &= check("wrap start", rsx_record_queue_start(w, exec_one, &s_exec) == 0);
        uint32_t pushed = 0;
        for (uint32_t i = 0; i < 2000; i++) {
            uint8_t payload[48];
            memset(payload, 0, sizeof payload);
            memcpy(payload, &i, 4);
            if (!rsx_record_queue_push(w, payload, (uint32_t)sizeof payload)) {
                /* The engine drains and retries; the fixture does the same. */
                rsx_record_queue_sync(w);
                if (!rsx_record_queue_push(w, payload, (uint32_t)sizeof payload)) {
                    fprintf(stderr, "FAIL wrap push %u\n", i);
                    ok = 0;
                    break;
                }
            }
            pushed++;
        }
        rsx_record_queue_sync(w);
        pthread_mutex_lock(&s_exec.mu);
        ok &= check("wrap all executed", s_exec.n == pushed);
        ok &= check("wrap in order", s_exec.bad == 0);
        pthread_mutex_unlock(&s_exec.mu);
        rsx_record_queue_destroy(w);
    }

    /* Full: fill the ring without draining and the next push must refuse. */
    rsx_record_queue* f = rsx_record_queue_new(4096u);
    ok &= check("full new", f != NULL);
    if (f) {
        reset_exec();
        ok &= check("full start", rsx_record_queue_start(f, exec_one, &s_exec) == 0);
        uint8_t payload[256];
        memset(payload, 0, sizeof payload);
        uint32_t n = 0;
        while (rsx_record_queue_push(f, payload, (uint32_t)sizeof payload)) {
            if (++n > 100000) break;
        }
        ok &= check("full refuses", n > 0 && n < 100000);
        /* A record larger than the whole ring is always refused. */
        uint8_t* big = (uint8_t*)calloc(1, 8192u);
        ok &= check("oversize refused", big && !rsx_record_queue_push(f, big, 8192u));
        free(big);
        rsx_record_queue_sync(f);
        pthread_mutex_lock(&s_exec.mu);
        ok &= check("full drained", s_exec.n == n);
        pthread_mutex_unlock(&s_exec.mu);
        rsx_record_queue_destroy(f);
    }

    rsx_record_queue_destroy(q);
    pthread_mutex_destroy(&s_exec.mu);
    if (!ok) return 1;
    printf("test_draw_queue: order, wrap, full and sync/drain OK\n");
    return 0;
}
