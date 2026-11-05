#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "vk_pipe_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct vk_pipe_queue {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       thread;
    int             started;
    int             quit;
    vk_pipe_queue_fn run;
    void*           user;
    void**          ring;
    unsigned        cap;
    unsigned        head;
    unsigned        tail;
    unsigned        count;
};

static void* vk_pipe_queue_main(void* arg)
{
    vk_pipe_queue* q = (vk_pipe_queue*)arg;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (!q->count && !q->quit)
            pthread_cond_wait(&q->cv, &q->mu);
        if (q->quit) {
            /* Jobs still queued belong to destroy(), which hands them back
             * through its drop callback. */
            pthread_mutex_unlock(&q->mu);
            break;
        }
        void* job = q->ring[q->head];
        q->head = (q->head + 1u) % q->cap;
        q->count--;
        pthread_mutex_unlock(&q->mu);

        if (job && q->run) q->run(job, q->user);
    }
    return NULL;
}

vk_pipe_queue* vk_pipe_queue_create(vk_pipe_queue_fn run, void* user,
                                    unsigned capacity, const char* name)
{
    if (!capacity) return NULL;
    vk_pipe_queue* q = (vk_pipe_queue*)calloc(1, sizeof *q);
    if (!q) return NULL;
    q->ring = (void**)calloc(capacity, sizeof *q->ring);
    if (!q->ring) { free(q); return NULL; }
    q->run = run;
    q->user = user;
    q->cap = capacity;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->ring); free(q); return NULL;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->mu); free(q->ring); free(q); return NULL;
    }
    if (pthread_create(&q->thread, NULL, vk_pipe_queue_main, q) != 0) {
        pthread_cond_destroy(&q->cv);
        pthread_mutex_destroy(&q->mu);
        free(q->ring); free(q);
        return NULL;
    }
#ifdef __linux__
    if (name && *name) pthread_setname_np(q->thread, name);
#else
    (void)name;
#endif
    q->started = 1;
    return q;
}

int vk_pipe_queue_push(vk_pipe_queue* q, void* job)
{
    if (!q) return -1;
    pthread_mutex_lock(&q->mu);
    if (q->quit || q->count >= q->cap) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->ring[q->tail] = job;
    q->tail = (q->tail + 1u) % q->cap;
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

unsigned vk_pipe_queue_pending(vk_pipe_queue* q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->mu);
    const unsigned n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

void vk_pipe_queue_destroy(vk_pipe_queue* q, void (*drop)(void* job))
{
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    q->quit = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    if (q->started) pthread_join(q->thread, NULL);

    for (;;) {
        pthread_mutex_lock(&q->mu);
        if (!q->count) { pthread_mutex_unlock(&q->mu); break; }
        void* job = q->ring[q->head];
        q->head = (q->head + 1u) % q->cap;
        q->count--;
        pthread_mutex_unlock(&q->mu);
        if (job && drop) drop(job);
    }
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    free(q->ring);
    free(q);
}
