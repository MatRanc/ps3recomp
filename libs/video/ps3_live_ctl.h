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
    PS3_LIVE_FETCH_MEMO,        /* PS3RECOMP_FETCH_MEMO: fetch plan/remap memo */
    PS3_LIVE_PIPE_MEMO,         /* PS3RECOMP_PIPE_MEMO: pipeline answer memo */
    PS3_LIVE_REMAP_CACHE,       /* PS3RECOMP_REMAP_CACHE: dedup memo by ref list */
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
    PS3_LIVE_C690_SPIN_US,      /* C690_SPIN_US: synthetic extra c690 job cost (ceiling probe) */
    PS3_LIVE_SPURS_LANE6,       /* SPURS_LANE6: let the mesh workloads use physical lane 5 */
    PS3_LIVE_TEX_DIRTY,         /* PS3RECOMP_TEX_DIRTY: texture pages by write generation */
    PS3_LIVE_C690_NATIVE_CULL,  /* PS3RECOMP_C690_NATIVE_CULL: native 0xFDA0 stage */
    PS3_LIVE_C690_NATIVE_SHADOW,/* PS3RECOMP_C690_NATIVE_SHADOW: run+compare both */
    PS3_LIVE_SPU_IDLE_BACKOFF,  /* PS3RECOMP_SPU_IDLE_BACKOFF: idle SPU poll back-off */
    PS3_LIVE_PIN_LAYOUT,        /* PS3RECOMP_PIN_LAYOUT: named affinity layout */
    PS3_LIVE_SPURS_PM_FAST_RESCHED, /* SPURS_PM_FAST_RESCHED: re-run a still-ready PM without the kernel round trip */
    PS3_LIVE_SPU_CODE_DMA_MEMO, /* SPU_CODE_DMA_MEMO: skip a code GET already resident byte-identical */
    PS3_LIVE_PREPARE_FLIP_RET,  /* cellGcmSetPrepareFlip returns the display buffer id */
    PS3_LIVE_FLIP_ON_DONE,      /* complete a flip when its present is queued, not on the display tick */
    PS3_LIVE_LWCOND_REAL,       /* PS3_LWCOND_REAL: host condition variables with real timeouts */
    PS3_LIVE_AUDIO_PACE,        /* PS3RECOMP_AUDIO_PACE: 5333 us audio period on the host clock */
    PS3_LIVE_FS_WRITE_CHECK,    /* PS3RECOMP_FS_WRITE_CHECK: error on a short/failed cellFsWrite */
    PS3_LIVE_SPURS_TEARDOWN,    /* PS3RECOMP_SPURS_TEARDOWN: real Shutdown/Wait drain */
    PS3_LIVE_GCM_OFFSET_SYNC,   /* PS3RECOMP_GCM_OFFSET_SYNC: publish guest offset tables on map/unmap */
    PS3_LIVE_COUNT
};

/* Re-read the environment for every switch the control file has not set.
 * The draw engine calls this from init (unit tests re-init it). */
void ps3_live_ctl_refresh_env(void) PS3_LIVE_CTL_WEAK;

/* Current value of a switch (0/1). Initialises the table on first use. */
int ps3_live_ctl_get(int key) PS3_LIVE_CTL_WEAK;

/* Current value of a string-valued switch (PS3RECOMP_PIN_LAYOUT), or NULL.
 * The returned buffer is written only by the walker thread's poll, so callers
 * on the walker may use it directly; every other caller copies it. */
const char* ps3_live_ctl_get_str(int key) PS3_LIVE_CTL_WEAK;

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
