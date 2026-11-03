/*
 * PS3RECOMP_CTL: live experiment switches from a small control file.
 *
 * The file is polled from the RSX walker thread (once per drain pass, which
 * is at least once per flip) and compared by mtime+size, so a benchmark can
 * flip an arm in the middle of a run without restarting the game. Lines are
 * NAME=value; names are the environment variables the switch already uses.
 *
 * Values are seeded from the environment at first use, so a run without a
 * control file behaves exactly as before. ps3_live_ctl_refresh_env() is for
 * the draw-engine init path, which tests call repeatedly with different
 * environment settings in one process.
 */
#ifndef PS3_LIVE_CTL_H
#define PS3_LIVE_CTL_H

#include <stdint.h>

/* Weak so a fixture that compiles one runtime .c file on its own (the engine
 * and FIFO-walker unit tests do) still links, and falls back to the env
 * switch. The game executable has a strong reference, which pulls the real
 * implementation out of the runtime archive. */
#if defined(__GNUC__) && !defined(_WIN32)
#define PS3_LIVE_CTL_WEAK __attribute__((weak))
#else
#define PS3_LIVE_CTL_WEAK
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_LIVE_WALKER_FAST = 0,
    PS3_LIVE_WALKER_WAKE,
    PS3_LIVE_EXP_NO_DRAW,
    PS3_LIVE_EXP_FAST_WAKE,
    PS3_LIVE_EXP_TICK_US,       /* numeric: display tick period in us */
    PS3_LIVE_CUBE_RT,           /* assemble cube render targets on the GPU */
    PS3_LIVE_PRESENT_PACE,      /* even out the window present cadence */
    PS3_LIVE_WCB_RECT,          /* WCB: resolve only the transfer's rectangle */
    PS3_LIVE_WCB_BATCH,         /* WCB: one submission for a pass's readbacks */
    PS3_LIVE_WCB_SKIP,          /* WCB: skip a copy whose serial has not moved */
    PS3_LIVE_SPU_MFC_CACHE,     /* SPU_MFC_LOOKUP_CACHE: per-thread MFC engine memo */
    PS3_LIVE_NOP_SKIP,          /* GCM_NOP_SKIP: skip NV4097 NOP (0x0100) runs */
    PS3_LIVE_GCM_REF_IMMEDIATE, /* GCM_REF_IMMEDIATE: publish SET_REFERENCE now */
    PS3_LIVE_TEX_PIN,           /* PS3RECOMP_RSX_TEX_PIN: never evict a texture bound this frame */
    PS3_LIVE_COUNT
};

/* Re-read the environment for every switch the control file has not set.
 * The draw engine calls this from init (unit tests re-init it). */
void ps3_live_ctl_refresh_env(void) PS3_LIVE_CTL_WEAK;

/* Current value of a switch (0/1). Initialises the table on first use. */
int ps3_live_ctl_get(int key) PS3_LIVE_CTL_WEAK;

/* True when PS3RECOMP_CTL names a control file. */
int ps3_live_ctl_enabled(void) PS3_LIVE_CTL_WEAK;

/* Called on the walker thread. `flip` is the guest flip count, used only in
 * the change log. Cheap when no file is configured (one cached check). */
void ps3_live_ctl_poll(uint32_t flip) PS3_LIVE_CTL_WEAK;

const char* ps3_live_ctl_name(int key) PS3_LIVE_CTL_WEAK;

/* EXP_TICK_US as an effective value (default 16000), NOT weak: the runner
 * calls this and that call is what makes the linker pull this translation
 * unit into the executable. Every other entry point is weak so a fixture that
 * compiles the walker alone still links; a weak reference does not extract an
 * archive member, so the runner's call must be strong. */
int ps3_live_ctl_tick_us(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_LIVE_CTL_H */
