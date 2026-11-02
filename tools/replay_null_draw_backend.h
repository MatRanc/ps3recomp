/* A null rsx_draw_backend for offline replay: every resource is a fake
 * handle and every draw is counted. The engine runs its real fetch/remap,
 * texture decode/hash, pipeline key and bind work; only the host API is
 * absent. That is the "drain minus Vulkan recording" measurement. */
#ifndef REPLAY_NULL_DRAW_BACKEND_H
#define REPLAY_NULL_DRAW_BACKEND_H

#include "rsx_draw_engine.h"

const rsx_draw_backend* replay_null_backend(void);
unsigned long long replay_null_draws(void);
unsigned long long replay_null_draw_vertices(void);
void replay_null_reset(void);

#endif
