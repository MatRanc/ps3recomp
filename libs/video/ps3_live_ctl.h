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

#ifdef __cplusplus
}
#endif

#endif /* PS3_LIVE_CTL_H */
