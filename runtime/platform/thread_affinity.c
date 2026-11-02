/*
 * PS3RECOMP_PIN=1: pin the named runtime threads from the CPU topology.
 *
 * See thread_affinity.h. The point is a reproducible benchmark, not a
 * general scheduling policy: the walker owns a physical core to itself, the
 * PPU main thread gets another core of the same L3 group, and every other
 * named thread shares the remaining cores. With the switch off this file
 * costs one cached env read and does nothing.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "thread_affinity.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_compat.h"

#define MAX_CPU 64u

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int      s_pin;
static int      s_ready;
static uint64_t s_online;
static uint64_t s_walker_mask;
static uint64_t s_ppu_mask;
static uint64_t s_other_mask;
/* For the startup print only. */
static unsigned s_walker_core, s_ppu_core, s_walker_l3, s_ppu_l3;

/* "0-3,6,8-11" -> mask */
static uint64_t parse_cpu_list(const char* s)
{
    uint64_t m = 0;
    while (s && *s) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (!isdigit((unsigned char)*s)) break;
        unsigned a = 0, b = 0;
        while (isdigit((unsigned char)*s)) a = a * 10 + (unsigned)(*s++ - '0');
        b = a;
        if (*s == '-') {
            s++;
            b = 0;
            while (isdigit((unsigned char)*s)) b = b * 10 + (unsigned)(*s++ - '0');
        }
        for (unsigned i = a; i <= b && i < MAX_CPU; i++) m |= (uint64_t)1 << i;
        while (*s && *s != ',') s++;
    }
    return m;
}

static int read_line(const char* path, char* out, size_t cap)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) { fclose(f); return 0; }
    fclose(f);
    char* p = out + strlen(out);
    while (p > out && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;
    return 1;
}

static unsigned lowest_cpu(uint64_t m)
{
    for (unsigned i = 0; i < MAX_CPU; i++)
        if (m & ((uint64_t)1 << i)) return i;
    return 0;
}

static void fmt_mask(uint64_t m, char* out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < MAX_CPU; ) {
        if (!(m & ((uint64_t)1 << i))) { i++; continue; }
        unsigned j = i;
        while (j + 1 < MAX_CPU && (m & ((uint64_t)1 << (j + 1)))) j++;
        int r;
        if (j > i)
            r = snprintf(out + n, n < cap ? cap - n : 0,
                         "%s%u-%u", n ? "," : "", i, j);
        else
            r = snprintf(out + n, n < cap ? cap - n : 0,
                         "%s%u", n ? "," : "", i);
        if (r > 0) n += (size_t)r;
        i = j + 1;
    }
    if (!n) snprintf(out, cap, "-");
}

static void init_mapping(void)
{
    const char* e = getenv("PS3RECOMP_PIN");
    s_pin = e && *e && strcmp(e, "0") != 0;
    if (!s_pin) return;

    char buf[256];
    if (!read_line("/sys/devices/system/cpu/online", buf, sizeof buf) ||
        !(s_online = parse_cpu_list(buf)))
        s_online = (uint64_t)1 << 0 | (uint64_t)1 << 1;

    /* Distinct physical cores, in CPU order. */
    uint64_t cores[MAX_CPU];
    unsigned ncore = 0;
    uint64_t core_l3[MAX_CPU];
    unsigned core_cpu[MAX_CPU];
    for (unsigned cpu = 0; cpu < MAX_CPU && ncore < MAX_CPU; cpu++) {
        if (!(s_online & ((uint64_t)1 << cpu))) continue;
        char path[160];
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
        if (!read_line(path, buf, sizeof buf)) buf[0] = 0;
        uint64_t sib = parse_cpu_list(buf);
        if (!sib) sib = (uint64_t)1 << cpu;
        int dup = 0;
        for (unsigned k = 0; k < ncore; k++) if (cores[k] == sib) { dup = 1; break; }
        if (dup) continue;
        cores[ncore] = sib;
        core_cpu[ncore] = lowest_cpu(sib);
        /* L3 group: scan the cpu's cache dirs for level 3. */
        uint64_t l3 = s_online;
        for (unsigned idx = 0; idx < 8; idx++) {
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%u/cache/index%u/level", cpu, idx);
            if (!read_line(path, buf, sizeof buf)) continue;
            if (atoi(buf) != 3) continue;
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%u/cache/index%u/shared_cpu_list",
                     cpu, idx);
            if (read_line(path, buf, sizeof buf)) {
                uint64_t g = parse_cpu_list(buf);
                if (g) l3 = g;
            }
            break;
        }
        core_l3[ncore] = l3;
        ncore++;
        if (ncore >= 2) break;   /* the first two cores are all this needs */
    }

    if (!ncore) {
        s_walker_mask = (uint64_t)1 << 0;
        s_ppu_mask    = (uint64_t)1 << 1;
        s_other_mask  = s_online & ~(s_walker_mask | s_ppu_mask);
        s_ready = 1;
    } else {
        s_walker_core = core_cpu[0];
        s_walker_l3 = (unsigned)lowest_cpu(core_l3[0]);
        s_walker_mask = (uint64_t)1 << s_walker_core;
        unsigned pick = 0;
        for (unsigned k = 1; k < ncore; k++) {
            if (core_l3[k] == core_l3[0]) { pick = k; break; }
        }
        if (!pick) pick = ncore > 1 ? 1 : 0;
        s_ppu_core = core_cpu[pick];
        s_ppu_l3 = (unsigned)lowest_cpu(core_l3[pick]);
        s_ppu_mask = (uint64_t)1 << s_ppu_core;
        s_other_mask = s_online & ~(cores[0] | cores[pick]);
        if (!s_other_mask) s_other_mask = s_online & ~(s_walker_mask | s_ppu_mask);
        s_ready = 1;
    }

    char w[160], p[160], o[160], on[160];
    fmt_mask(s_walker_mask, w, sizeof w);
    fmt_mask(s_ppu_mask, p, sizeof p);
    fmt_mask(s_other_mask, o, sizeof o);
    fmt_mask(s_online, on, sizeof on);
    fprintf(stderr,
            "[pin] PS3RECOMP_PIN=1 topology online=%s: walker cpu %s "
            "(core %u, L3 group %u), ppu main cpu %s (core %u, L3 group %u), "
            "other cpus %s; masks walker=0x%llx ppu=0x%llx other=0x%llx "
            "(walker/ppu siblings idle)\n",
            on, w, s_walker_core, s_walker_l3, p, s_ppu_core, s_ppu_l3, o,
            (unsigned long long)s_walker_mask,
            (unsigned long long)s_ppu_mask,
            (unsigned long long)s_other_mask);
    fflush(stderr);
}

void ps3_thread_pin_by_name(const char* name)
{
    pthread_once(&s_once, init_mapping);
    if (!s_pin || !s_ready) return;
    uint64_t mask;
    if (!strcmp(name, "rsx walker"))      mask = s_walker_mask;
    else if (!strcmp(name, "ppu main"))   mask = s_ppu_mask;
    else                                  mask = s_other_mask;
    ps3_thread_apply_affinity_self(mask);
}
