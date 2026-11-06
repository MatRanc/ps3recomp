/*
 * PS3RECOMP_CTL: a tiny live-switch file for in-run A/B experiments.
 *
 *   PS3RECOMP_CTL=/path/to/ctl ./MyGameRecomp EBOOT.elf
 *
 * The file holds lines "NAME=value" (the names are the switches' own
 * environment variables). The walker thread calls ps3_live_ctl_poll() once per
 * drain pass; a stat() comparing mtime+size+inode is the only cost until the
 * file changes. Every applied change is logged with the flip number:
 *
 *   [live-ctl] flip=412 PS3RECOMP_WALKER_FAST=1 (was 0)
 *
 * Values are per-key atomics: one writer (the walker) and cheap readers on
 * the PPU/SPU sides. A key the file never names keeps its environment value
 * (or 0), so a run without a control file is exactly the old behaviour.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "ps3_live_ctl.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* const K_NAME[PS3_LIVE_COUNT] = {
    "PS3RECOMP_WALKER_FAST",
    "PS3RECOMP_FETCH_MEMO",
    "PS3RECOMP_PIPE_MEMO",
    "PS3RECOMP_REMAP_CACHE",
    "PS3RECOMP_WALKER_WAKE",
    "EXP_NO_DRAW",
    "EXP_FAST_WAKE",
    "EXP_TICK_US",
    "PS3RECOMP_RSX_CUBE_RT",
    "PS3RECOMP_VK_PACE",
    "PS3RECOMP_RSX_WCB_RECT",
    "PS3RECOMP_RSX_WCB_BATCH",
    "PS3RECOMP_RSX_WCB_SKIP",
    "SPU_MFC_LOOKUP_CACHE",
    "GCM_NOP_SKIP",
    "GCM_REF_IMMEDIATE",
    "PS3RECOMP_RSX_TEX_PIN",
    "C690_SPIN_US",
    "SPURS_LANE6",
    "PS3RECOMP_TEX_DIRTY",
    "PS3RECOMP_C690_NATIVE_CULL",
    "PS3RECOMP_C690_NATIVE_SHADOW",
    "PS3RECOMP_SPU_IDLE_BACKOFF",
    "PS3RECOMP_PIN_LAYOUT",
    "SPURS_PM_FAST_RESCHED",
    "SPU_CODE_DMA_MEMO",
    "PS3RECOMP_PREPARE_FLIP_RET",
    "PS3RECOMP_FLIP_ON_DONE",
    "PS3_LWCOND_REAL",
    "PS3RECOMP_AUDIO_PACE",
    "PS3RECOMP_FS_WRITE_CHECK",
    "PS3RECOMP_SPURS_TEARDOWN",
    "PS3RECOMP_GCM_OFFSET_SYNC",
    "PS3RECOMP_PREPARE_FLIP_KEEP_STATUS",
    "PS3RECOMP_C690_CULL_FIX",
    "PS3RECOMP_C690_CULL_FIX_2C",
    "PS3RECOMP_REMAP_FAST",
    "PS3RECOMP_TICK_IN_DRAIN",
    "MLB16_CULL_BYPASS",
    "PS3RECOMP_C690_NATIVE_SCREEN",
    "PS3RECOMP_C690_NATIVE_SCREEN_SHADOW",
    "SPURS_PM_FAST_ACTIVE",
    "SPURS_PM_FAST_ONE_LOCK",
    "SPURS_PM_FAST_GEN",
    "PS3RECOMP_FLIP_FENCE_DONE",
    "PS3RECOMP_DUMP_FIFO_TRIGGER",
    "SPURS_PM_NATIVE_DISPATCH",
    "PS3RECOMP_FLIP_DEADLINE_US",
    "PS3RECOMP_MEMO_INDEX",
    "PS3RECOMP_REFS_HASH_FAST",
    "PS3RECOMP_RSX_WCB_STREAM",
    "PS3RECOMP_RSX_WCB_PREFETCH",
    "PS3RECOMP_REMAP_DIRECT",
    "PS3RECOMP_WALKER_MT",
    "PS3RECOMP_WALKER_MT_WORKERS",
    "PS3RECOMP_WALKER_MT_STAGES",
    "PS3RECOMP_WALKER_MT_SNAPSHOT",
    "PS3RECOMP_WALKER_MT_TEX_RESOLVE",
    "PS3RECOMP_FLIP_TICK_OWN_STATE",
    "PS3RECOMP_ENG_TEXMEMO_LOCAL",
    "PS3RECOMP_VK_VIEW_MEMO",
    "PS3RECOMP_ENG_TEXCACHE",
    "PS3RECOMP_ENG_TARGET_BLOOM",
    "PS3RECOMP_C690_HOLES_NATIVE",
    "PS3RECOMP_FP_TXD",
    "PS3RECOMP_FP_AA",
    "PS3RECOMP_PIPE_ASYNC",
    "PS3RECOMP_SPU_FTZ",
    "PS3RECOMP_ENG_DEPTH_GPU",
    "PS3RECOMP_ENG_SHADOW_CONST",
    "PS3RECOMP_VK_PASS_KEEP",
    "PS3RECOMP_VK_DRAW_STATE",
    "PS3RECOMP_VK_VTX_PRE",
    "PS3RECOMP_VK_BATCH_CREATES",
    "PS3RECOMP_CROWD_FIX",
};

static _Atomic int s_val[PS3_LIVE_COUNT];
/* 1 once the control file has named the key; from then on the file wins. */
static _Atomic int s_file_set[PS3_LIVE_COUNT];
/* String-valued keys (PS3RECOMP_PIN_LAYOUT) keep their text here; the walker
 * is the only writer (apply_file), so no synchronisation is needed for the
 * same-thread reader in thread_affinity.c. */
#define CTL_STR_CAP 48
static char s_str[PS3_LIVE_COUNT][CTL_STR_CAP];
static _Atomic int s_str_set[PS3_LIVE_COUNT];

static int key_is_string(int i)
{
    return i == PS3_LIVE_PIN_LAYOUT;
}
static const char* s_path;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static struct {
    int      valid;
    unsigned long long mt_sec, mt_nsec;
    long long size;
    unsigned long long ino;
} s_seen;

static int parse_value(const char* s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!strcasecmp(s, "on") || !strcasecmp(s, "true") ||
        !strcasecmp(s, "yes") || !strcasecmp(s, "enabled"))
        return 1;
    if (!strcasecmp(s, "off") || !strcasecmp(s, "false") ||
        !strcasecmp(s, "no") || !strcasecmp(s, "disabled") || !*s)
        return 0;
    return (int)strtol(s, NULL, 0);
}

static void seed_env(void)
{
    for (int i = 0; i < PS3_LIVE_COUNT; i++) {
        const char* e = getenv(K_NAME[i]);
        atomic_store_explicit(&s_val[i], e ? parse_value(e) : 0,
                              memory_order_relaxed);
        atomic_store_explicit(&s_file_set[i], 0, memory_order_relaxed);
        if (key_is_string(i) && e) {
            snprintf(s_str[i], CTL_STR_CAP, "%s", e);
            atomic_store_explicit(&s_str_set[i], 1, memory_order_relaxed);
        }
    }
}

/* Parse the file into the table. `log_changes` is 0 for the initial seed,
 * so the full command line can also be reproduced from the first log line. */
static void apply_file(uint32_t flip, int log_changes)
{
    if (!s_path || !*s_path) return;

    struct stat st;
    if (stat(s_path, &st) != 0) return;
#ifdef __linux__
    const unsigned long long sec = (unsigned long long)st.st_mtim.tv_sec;
    const unsigned long long nsec = (unsigned long long)st.st_mtim.tv_nsec;
#else
    const unsigned long long sec = (unsigned long long)st.st_mtime;
    const unsigned long long nsec = 0;
#endif
    if (s_seen.valid && s_seen.mt_sec == sec && s_seen.mt_nsec == nsec &&
        s_seen.size == (long long)st.st_size &&
        s_seen.ino == (unsigned long long)st.st_ino)
        return;

    FILE* f = fopen(s_path, "r");
    if (!f) return;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;

        char* eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char* name = line;
            char* value = eq + 1;
            while (*name && isspace((unsigned char)*name)) name++;
            char* end = name + strlen(name);
            while (end > name && isspace((unsigned char)end[-1])) *--end = 0;
            while (*value && isspace((unsigned char)*value)) value++;

            for (int i = 0; i < PS3_LIVE_COUNT; i++) {
                if (strcmp(name, K_NAME[i]) != 0) continue;
                if (key_is_string(i)) {
                    const int old_set = atomic_load_explicit(&s_str_set[i], memory_order_relaxed);
                    if (log_changes && (!old_set || strcmp(s_str[i], value) != 0))
                        fprintf(stderr, "[live-ctl] flip=%u %s=%s (was %s)\n",
                                flip, K_NAME[i], value, old_set ? s_str[i] : "(unset)");
                    snprintf(s_str[i], CTL_STR_CAP, "%s", value);
                    atomic_store_explicit(&s_str_set[i], 1, memory_order_relaxed);
                    atomic_store_explicit(&s_file_set[i], 1, memory_order_relaxed);
                    break;
                }
                const int v = parse_value(value);
                const int old = atomic_load_explicit(&s_val[i], memory_order_relaxed);
                atomic_store_explicit(&s_file_set[i], 1, memory_order_relaxed);
                atomic_store_explicit(&s_val[i], v, memory_order_relaxed);
                if (log_changes && v != old)
                    fprintf(stderr, "[live-ctl] flip=%u %s=%d (was %d)\n",
                            flip, K_NAME[i], v, old);
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    s_seen.valid = 1;
    s_seen.mt_sec = sec;
    s_seen.mt_nsec = nsec;
    s_seen.size = (long long)st.st_size;
    s_seen.ino = (unsigned long long)st.st_ino;
}

static void init_once(void)
{
    seed_env();
    s_path = getenv("PS3RECOMP_CTL");
    apply_file(0, 0);
}

void ps3_live_ctl_refresh_env(void)
{
    pthread_once(&s_once, init_once);
    for (int i = 0; i < PS3_LIVE_COUNT; i++) {
        if (atomic_load_explicit(&s_file_set[i], memory_order_relaxed)) continue;
        const char* e = getenv(K_NAME[i]);
        atomic_store_explicit(&s_val[i], e ? parse_value(e) : 0,
                              memory_order_relaxed);
        if (key_is_string(i)) {
            if (e) {
                snprintf(s_str[i], CTL_STR_CAP, "%s", e);
                atomic_store_explicit(&s_str_set[i], 1, memory_order_relaxed);
            } else {
                atomic_store_explicit(&s_str_set[i], 0, memory_order_relaxed);
            }
        }
    }
}

int ps3_live_ctl_get(int key)
{
    if (key < 0 || key >= PS3_LIVE_COUNT) return 0;
    pthread_once(&s_once, init_once);
    return atomic_load_explicit(&s_val[key], memory_order_relaxed);
}

int ps3_live_ctl_file_set(int key)
{
    if (key < 0 || key >= PS3_LIVE_COUNT) return 0;
    pthread_once(&s_once, init_once);
    return atomic_load_explicit(&s_file_set[key], memory_order_relaxed);
}

const char* ps3_live_ctl_get_str(int key)
{
    if (key < 0 || key >= PS3_LIVE_COUNT) return NULL;
    pthread_once(&s_once, init_once);
    return atomic_load_explicit(&s_str_set[key], memory_order_relaxed)
               ? s_str[key] : NULL;
}

int ps3_live_ctl_enabled(void)
{
    pthread_once(&s_once, init_once);
    return s_path && *s_path;
}

void ps3_live_ctl_poll(uint32_t flip)
{
    pthread_once(&s_once, init_once);
    apply_file(flip, 1);
}

const char* ps3_live_ctl_name(int key)
{
    return (key >= 0 && key < PS3_LIVE_COUNT) ? K_NAME[key] : "?";
}

int ps3_live_ctl_tick_us(void)
{
    const int us = ps3_live_ctl_get(PS3_LIVE_EXP_TICK_US);
    if (us > 0) return us;           /* an explicit period wins */
    /* FLIP_ON_DONE leaves the tick as the vblank clock only (the flip is
     * completed by the present, see cellGcmFlipOnPresent). 16683 us is
     * 1001/60000 s, the PS3's 59.94 Hz refresh, so a title timing animation
     * from the vblank handler sees the same rate as on the console. */
    if (ps3_live_ctl_get(PS3_LIVE_FLIP_ON_DONE)) return 16683;
    return 16000;                    /* 16 ms: the historical tick */
}
