/*
 * ps3recomp - one background worker with a bounded FIFO for pipeline work.
 *
 * PS3RECOMP_PIPE_ASYNC moves HLSL -> SPIR-V translation and
 * vkCreateGraphicsPipelines off the RSX walker (docs/pipeline-async.md).
 * This file is only the thread and the queue: the caller owns the job
 * payload and the synchronisation of whatever the job touches.
 *
 * push() never blocks on the worker and never drops a job: when the ring is
 * full it returns -1 and the caller keeps ownership. destroy() stops the
 * worker, joins it, and hands every queued job back through `drop` so a
 * caller can free the payload; a job the worker is already running finishes
 * first.
 */
#ifndef PS3RECOMP_VK_PIPE_QUEUE_H
#define PS3RECOMP_VK_PIPE_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_pipe_queue vk_pipe_queue;

/* Runs on the worker thread for each job. */
typedef void (*vk_pipe_queue_fn)(void* job, void* user);

/* Create the worker (0-length name skips naming the thread). Returns NULL if
 * the thread could not be started. */
vk_pipe_queue* vk_pipe_queue_create(vk_pipe_queue_fn run, void* user,
                                    unsigned capacity, const char* name);

/* Queue `job`; 0 queued, -1 when the queue is full or shut down. The caller
 * keeps ownership on failure. */
int vk_pipe_queue_push(vk_pipe_queue* q, void* job);

/* Jobs waiting to be run (not counting one the worker has taken). */
unsigned vk_pipe_queue_pending(vk_pipe_queue* q);

/* Stop and join the worker, then call `drop` on every job still queued.
 * `drop` may be NULL. The queue is freed; do not use it afterwards. */
void vk_pipe_queue_destroy(vk_pipe_queue* q, void (*drop)(void* job));

#ifdef __cplusplus
}
#endif

#endif
