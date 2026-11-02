#ifndef REPLAY_RSX_ENGINE_CORE_H
#define REPLAY_RSX_ENGINE_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_engine_replay rsx_engine_replay;

typedef struct {
    uint64_t draws;
    uint64_t vertices;
    uint64_t read_hits;
    uint64_t read_misses;
    uint32_t words;
    uint64_t frame_hash;   /* output gate: everything the backend received */
} rsx_engine_replay_stats;

rsx_engine_replay* rsx_engine_replay_create(const char* prefix, char* err, size_t errlen);
int rsx_engine_replay_run_frame(rsx_engine_replay* r, rsx_engine_replay_stats* out);
void rsx_engine_replay_destroy(rsx_engine_replay* r);

#ifdef __cplusplus
}
#endif
#endif
