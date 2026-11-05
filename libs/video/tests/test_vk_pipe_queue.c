/*
 * The pipeline-compile worker queue: FIFO order, a bounded ring, a push that
 * never touches the worker, and a destroy that stops the worker and hands
 * every unrun job back exactly once. Run it under TSan:
 *
 *   cc -std=gnu17 -O1 -g -fsanitize=thread \
 *      -Iinclude -Ilibs/video libs/video/tests/test_vk_pipe_queue.c \
 *      libs/video/vk_pipe_queue.c -pthread -o /tmp/tq && /tmp/tq
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vk_pipe_queue.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static unsigned done;
static unsigned last;
static unsigned dropped;

static void run_job(void* job, void* user)
{
    (void)user;
    pthread_mutex_lock(&mu);
    done++;
    last = (unsigned)(size_t)job;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void drop_job(void* job)
{
    (void)job;
    pthread_mutex_lock(&mu);
    dropped++;
    pthread_mutex_unlock(&mu);
}

static int wait_done(unsigned want, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mu);
    while (done < want) {
        if (pthread_cond_timedwait(&cv, &mu, &ts) != 0) break;
    }
    const int ok = done >= want;
    pthread_mutex_unlock(&mu);
    return ok;
}

typedef struct { vk_pipe_queue* q; unsigned base; } pusher_arg;

static void* pusher(void* arg)
{
    pusher_arg* a = (pusher_arg*)arg;
    for (unsigned i = 0; i < 40; i++) {
        void* job = (void*)(size_t)(a->base + i + 1u);
        while (vk_pipe_queue_push(a->q, job) != 0) usleep(100);
    }
    return NULL;
}

int main(void)
{
    vk_pipe_queue* q = vk_pipe_queue_create(run_job, NULL, 8, "pq-test");
    CHECK(q != NULL);
    CHECK(vk_pipe_queue_pending(q) == 0);

    /* FIFO within an empty queue, and a full ring refuses without dropping. */
    for (unsigned i = 1; i <= 8; i++)
        CHECK(vk_pipe_queue_push(q, (void*)(size_t)i) == 0);
    CHECK(vk_pipe_queue_push(q, (void*)(size_t)99) == -1);
    CHECK(wait_done(8, 4000));
    pthread_mutex_lock(&mu);
    CHECK(last == 8);       /* single worker: strict FIFO */
    pthread_mutex_unlock(&mu);
    CHECK(vk_pipe_queue_push(q, (void*)(size_t)9) == 0);
    CHECK(wait_done(9, 4000));

    /* Several producers, one consumer: every accepted job runs once. */
    pthread_t th[4];
    pusher_arg a[4];
    for (int i = 0; i < 4; i++) {
        a[i].q = q;
        a[i].base = 1000u + (unsigned)i * 100u;
        CHECK(pthread_create(&th[i], NULL, pusher, &a[i]) == 0);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(wait_done(169, 8000));

    pthread_mutex_lock(&mu);
    const unsigned before = done;
    pthread_mutex_unlock(&mu);
    CHECK(before == 169);

    /* Destroy stops the worker; every queued job is dropped exactly once,
     * and one the worker already took is not dropped. */
    vk_pipe_queue* q2 = vk_pipe_queue_create(run_job, NULL, 8, NULL);
    CHECK(q2 != NULL);
    for (unsigned i = 0; i < 8; i++)
        CHECK(vk_pipe_queue_push(q2, (void*)(size_t)(2000u + i)) == 0);
    vk_pipe_queue_destroy(q2, drop_job);
    pthread_mutex_lock(&mu);
    const unsigned ran = done - before;
    const unsigned total = ran + dropped;
    pthread_mutex_unlock(&mu);
    CHECK(total == 8);      /* ran + dropped, no loss and no double free */

    vk_pipe_queue_destroy(q, NULL);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("test_vk_pipe_queue: PASS\n");
    return 0;
}
