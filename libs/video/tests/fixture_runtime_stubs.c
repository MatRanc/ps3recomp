/* Weak stubs for fixtures that link rsx_draw_engine.c without the runtime.
 *
 * rsx_draw_engine.c's PS3RECOMP_BINK_DMA_LOG path references the runtime's
 * g_spu_put_hook and ps3_ms_now. A fixture that does not link the runtime (or
 * spu_channels.c) only needs the symbols to exist; the diagnostic path is
 * gated by the environment and never runs in a fixture. Weak definitions are
 * overridden by a fixture's own strong definitions. */
#include <stdint.h>

__attribute__((weak)) void (*g_spu_put_hook)(uint32_t ea, uint32_t size);

__attribute__((weak)) unsigned long long ps3_ms_now(void);
unsigned long long ps3_ms_now(void) { return 0ull; }
