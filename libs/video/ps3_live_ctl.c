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
    "PS3RECOMP_WALKER_WAKE",
    "EXP_NO_DRAW",
    "EXP_FAST_WAKE",
    "EXP_TICK_US",
};

static _Atomic int s_val[PS3_LIVE_COUNT];
/* 1 once the control file has named the key; from then on the file wins. */
static _Atomic int s_file_set[PS3_LIVE_COUNT];
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
    }
}

int ps3_live_ctl_get(int key)
{
    if (key < 0 || key >= PS3_LIVE_COUNT) return 0;
    pthread_once(&s_once, init_once);
    return atomic_load_explicit(&s_val[key], memory_order_relaxed);
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
    return us > 0 ? us : 16000;      /* 16 ms: the historical tick */
}
