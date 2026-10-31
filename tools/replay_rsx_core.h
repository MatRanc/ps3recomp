/*
 * RSX FIFO Replay Core Interface
 * Provides headless replaying of captured RSX FIFO command streams through
 * cellGcmSys and rsx_dispatch.
 */
#ifndef REPLAY_RSX_CORE_H
#define REPLAY_RSX_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_REPLAY_MAGIC 0x46585352u /* "RSXF" */

/* Optional structured header for FIFO recordings */
typedef struct {
    uint32_t magic;          /* RSX_REPLAY_MAGIC ("RSXF") */
    uint32_t version;        /* 1 */
    uint32_t io_addr;        /* Base IO address in guest memory (e.g. 0x00100000) */
    uint32_t io_size;        /* Total IO region size (e.g. 0x00400000) */
    uint32_t frame_count;    /* Number of recorded frames */
    uint32_t total_words;    /* Total 32-bit method words */
    uint32_t flags;          /* Reserved */
    uint32_t extra;          /* Reserved */
} rsx_replay_file_header;

typedef struct {
    uint64_t total_calls;
    uint64_t state_hash;
    uint32_t words_processed;
    uint32_t headers_processed;
    uint32_t draws;
    uint32_t clears;
    uint32_t flips;
} rsx_replay_frame_stats;

typedef struct rsx_replay_context rsx_replay_context;

/* Create replay context with specified IO memory window.
 * engine_mode: 0 = vtable only, 1 = register-file draw engine (default), 2 = legacy vtable + engine
 */
rsx_replay_context* rsx_replay_create(uint32_t io_addr, uint32_t io_size, int engine_mode);

/* Load method words into the replay buffer */
int rsx_replay_load_words(rsx_replay_context* ctx, const uint32_t* words, size_t word_count);

/* Run one frame through the RSX walker into the dispatcher */
int rsx_replay_run_frame(rsx_replay_context* ctx, rsx_replay_frame_stats* stats_out, int verify_each_call);

/* Destroy context and free guest memory */
void rsx_replay_destroy(rsx_replay_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_RSX_CORE_H */
