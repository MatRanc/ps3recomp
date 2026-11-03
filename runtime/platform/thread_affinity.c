/*
 * PS3RECOMP_PIN=1: pin the named runtime threads from the CPU topology.
 *
 * See thread_affinity.h. The point is a reproducible benchmark, not a
 * general scheduling policy: the walker owns a physical core to itself, the
 * PPU main thread gets another core of the same L3 group, and every other
 * named thread shares the remaining cores. With the switch off this file
 * costs one cached env read and does nothing.
 *
 * PS3RECOMP_PIN_LAYOUT selects how the remaining threads share those cores:
 *
 *   today  (default)  walker cpu 0, ppu main cpu 1, every other thread on
 *                     the rest, both SMT siblings of each core available.
 *   lanes             walker cpu 0, ppu main cpu 1, the five busy c690
 *                     SPURS lanes one per remaining physical core (the
 *                     unavoidable fifth pair lands on a sibling), present /
 *                     DisplayFlip / spurs kernel on the light siblings.
 *   os                walker and ppu main as today; the SPURS lanes get the
 *                     whole online mask, i.e. the OS scheduler decides.
 *
 * The layout is live-switchable through the PS3RECOMP_CTL control file
 * ("PS3RECOMP_PIN_LAYOUT=lanes"): ps3_thread_pin_poll(), called from the
 * walker's drain pass, reapplies the masks to every registered thread with
 * pthread_setaffinity_np. On a machine other than this Ryzen the topology is
 * still read from sysfs; the lane layout degrades to sharing cores if fewer
 * are available.
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

/* Live layout switch (libs/video's control file). Optional so a fixture that
 * compiles this file alone still links: the string getter is weak. */
#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define PIN_HAVE_LIVE_CTL 1
#  endif
#endif

#define MAX_CPU 64u
#define PIN_MAX_THREADS 64
#define PIN_LAYOUT_NAME_CAP 32

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int      s_pin;
static int      s_ready;
static uint64_t s_online;
static uint64_t s_walker_mask;
static uint64_t s_ppu_mask;
static uint64_t s_other_mask;
/* For the startup print only. */
static unsigned s_walker_core, s_ppu_core, s_walker_l3, s_ppu_l3;

enum {
    PIN_LAYOUT_TODAY = 0,
    PIN_LAYOUT_LANES,
    PIN_LAYOUT_OS
};

static int  s_layout = PIN_LAYOUT_TODAY;
static char s_layout_name[PIN_LAYOUT_NAME_CAP] = "today";
static char s_applied_name[PIN_LAYOUT_NAME_CAP] = "today";

static uint64_t s_lane_mask[5];
static uint64_t s_present_mask;
static uint64_t s_display_mask;
static uint64_t s_kernel_mask;
static uint64_t s_light_mask;

enum {
    PIN_ROLE_OTHER = 0,
    PIN_ROLE_WALKER,
    PIN_ROLE_MAIN,
    PIN_ROLE_LANE,
    PIN_ROLE_PRESENT,
    PIN_ROLE_DISPLAYFLIP,
    PIN_ROLE_SPURS_KERNEL
};

typedef struct {
    pthread_t pt;
    int       role;
    int       lane;
    int       used;
} pin_thread;

static pin_thread   s_threads[PIN_MAX_THREADS];
static pthread_mutex_t s_threads_lock = PTHREAD_MUTEX_INITIALIZER;

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

static uint64_t siblings_of(unsigned cpu)
{
    char path[160], buf[128];
    snprintf(path, sizeof path,
             "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
    if (!read_line(path, buf, sizeof buf)) return (uint64_t)1 << cpu;
    uint64_t sib = parse_cpu_list(buf);
    return sib ? sib : (uint64_t)1 << cpu;
}

static int layout_from_name(const char* name)
{
    if (!name || !*name) return PIN_LAYOUT_TODAY;
    if (!strcmp(name, "today") || !strcmp(name, "default") ||
        !strcmp(name, "a") || !strcmp(name, "0"))
        return PIN_LAYOUT_TODAY;
    if (!strcmp(name, "lanes") || !strcmp(name, "b") || !strcmp(name, "1"))
        return PIN_LAYOUT_LANES;
    if (!strcmp(name, "os") || !strcmp(name, "c") || !strcmp(name, "2"))
        return PIN_LAYOUT_OS;
    return PIN_LAYOUT_TODAY;
}

static const char* layout_env_name(void)
{
    return getenv("PS3RECOMP_PIN_LAYOUT");
}

/* Current layout name: the control file wins once it names the key, else the
 * environment. */
static const char* current_layout_name(void)
{
#if defined(PIN_HAVE_LIVE_CTL)
    if (ps3_live_ctl_get_str) {
        const char* s = ps3_live_ctl_get_str(PS3_LIVE_PIN_LAYOUT);
        if (s && *s) return s;
    }
#endif
    return layout_env_name();
}

/* Assign the five c690 lanes to one physical core each, plus the sibling for
 * any lane beyond the number of available cores. light_mask gets every
 * remaining logical CPU of those cores (the walker/main siblings stay out of
 * s_other_mask by construction). */
static void build_lane_layout(void)
{
    uint64_t avail = s_other_mask;
    uint64_t groups[MAX_CPU];
    unsigned ng = 0;
    for (unsigned cpu = 0; cpu < MAX_CPU && ng < MAX_CPU; cpu++) {
        if (!(avail & ((uint64_t)1 << cpu))) continue;
        uint64_t g = siblings_of(cpu) & avail;
        if (!g) g = (uint64_t)1 << cpu;
        int dup = 0;
        for (unsigned k = 0; k < ng; k++) if (groups[k] == g) { dup = 1; break; }
        if (!dup) groups[ng++] = g;
    }
    for (int i = 0; i < 5; i++) s_lane_mask[i] = 0;
    uint64_t used = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t pick = 0;
        if (i < (int)ng) {
            uint64_t g = groups[i];
            /* Prefer the group's lowest CPU; if a previous lane already took
             * it, use another CPU of the same group. */
            uint64_t free_in_group = g & ~used;
            if (free_in_group) pick = (uint64_t)1 << lowest_cpu(free_in_group);
            else pick = (uint64_t)1 << lowest_cpu(g);
        } else {
            /* An extra lane shares a core with an earlier lane. Pick a still
             * unused CPU from a group, preferring the one two lanes back (so
             * the shared pair sits on a middle lane, not lane 0). */
            int order[5], norder = 0;
            int share = (i >= 2) ? i - 2 : 0;
            order[norder++] = share;
            for (int k = 0; k < (int)ng && norder < 5; k++)
                if (k != share) order[norder++] = k;
            for (int k = 0; k < norder && !pick; k++) {
                uint64_t free_in_group = groups[order[k]] & ~used;
                if (free_in_group) pick = (uint64_t)1 << lowest_cpu(free_in_group);
            }
            if (!pick && ng) pick = (uint64_t)1 << lowest_cpu(groups[share % (int)ng]);
        }
        s_lane_mask[i] = pick;
        used |= pick;
    }
    s_light_mask = avail & ~used;
    if (!s_light_mask) s_light_mask = avail ? avail : s_other_mask;
    unsigned lc[3] = { 0, 0, 0 };
    unsigned nl = 0;
    uint64_t lm = s_light_mask;
    for (unsigned cpu = 0; cpu < MAX_CPU && nl < 3; cpu++)
        if (lm & ((uint64_t)1 << cpu)) lc[nl++] = cpu;
    s_present_mask = nl > 0 ? (uint64_t)1 << lc[0] : s_light_mask;
    s_display_mask = nl > 1 ? (uint64_t)1 << lc[1] : s_light_mask;
    s_kernel_mask  = nl > 2 ? (uint64_t)1 << lc[2] : s_light_mask;
}

static void build_layout_masks(void)
{
    if (s_layout == PIN_LAYOUT_LANES) {
        build_lane_layout();
        return;
    }
    for (int i = 0; i < 5; i++)
        s_lane_mask[i] = (s_layout == PIN_LAYOUT_OS) ? s_online : s_other_mask;
    s_present_mask = s_display_mask = s_kernel_mask = s_other_mask;
    s_light_mask = s_other_mask;
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

    const char* lname = current_layout_name();
    s_layout = layout_from_name(lname);
    snprintf(s_layout_name, sizeof s_layout_name, "%s", lname && *lname ? lname : "today");
    snprintf(s_applied_name, sizeof s_applied_name, "%s", s_layout_name);
    build_layout_masks();

    char w[160], p[160], o[160], on[160], ln[160], lt[160];
    fmt_mask(s_walker_mask, w, sizeof w);
    fmt_mask(s_ppu_mask, p, sizeof p);
    fmt_mask(s_other_mask, o, sizeof o);
    fmt_mask(s_online, on, sizeof on);
    fmt_mask(s_lane_mask[0] | s_lane_mask[1] | s_lane_mask[2] |
             s_lane_mask[3] | s_lane_mask[4], ln, sizeof ln);
    fmt_mask(s_light_mask, lt, sizeof lt);
    fprintf(stderr,
            "[pin] PS3RECOMP_PIN=1 topology online=%s: walker cpu %s "
            "(core %u, L3 group %u), ppu main cpu %s (core %u, L3 group %u), "
            "other cpus %s; layout=%s masks walker=0x%llx ppu=0x%llx other=0x%llx "
            "lanes=%s light=%s (walker/ppu siblings idle)\n",
            on, w, s_walker_core, s_walker_l3, p, s_ppu_core, s_ppu_l3, o,
            s_layout_name,
            (unsigned long long)s_walker_mask,
            (unsigned long long)s_ppu_mask,
            (unsigned long long)s_other_mask,
            ln, lt);
    fflush(stderr);
}

static uint64_t mask_for(int role, int lane)
{
    switch (role) {
    case PIN_ROLE_WALKER:        return s_walker_mask;
    case PIN_ROLE_MAIN:          return s_ppu_mask;
    case PIN_ROLE_LANE:
        if (s_layout == PIN_LAYOUT_TODAY) return s_other_mask;
        if (s_layout == PIN_LAYOUT_OS)    return s_online;
        return (lane >= 0 && lane < 5) ? s_lane_mask[lane] : s_light_mask;
    case PIN_ROLE_PRESENT:       return s_present_mask;
    case PIN_ROLE_DISPLAYFLIP:   return s_display_mask;
    case PIN_ROLE_SPURS_KERNEL:  return s_kernel_mask;
    default:                     return s_light_mask;
    }
}

static void apply_to_thread(pthread_t pt, uint64_t mask)
{
    if (!mask) return;
#if defined(_WIN32)
    (void)pt; (void)mask;   /* role pinning of running threads is POSIX-only */
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned i = 0; i < MAX_CPU; i++)
        if (mask & ((uint64_t)1 << i)) CPU_SET(i, &set);
    pthread_setaffinity_np(pt, sizeof set, &set);
#endif
}

/* Record (or update) this thread and apply the current layout.
 *
 * Only the long-lived roles are kept in the registry. A transient guest
 * thread registered as PIN_ROLE_OTHER exits while the process keeps running;
 * reapplying to its stale pthread_t later is at best a wasted syscall and at
 * worst a use-after-free inside libpthread (the observed crash when a
 * malformed layout name made the poll reapply every drain pass). OTHER
 * threads still get their mask once, at creation. */
static void register_and_apply(int role, int lane)
{
    if (!s_pin || !s_ready) return;
    pthread_t self = pthread_self();
    if (role == PIN_ROLE_OTHER) {
        apply_to_thread(self, mask_for(role, lane));
        return;
    }
    pthread_mutex_lock(&s_threads_lock);
    pin_thread* slot = NULL;
    for (int i = 0; i < PIN_MAX_THREADS; i++) {
        if (!s_threads[i].used) continue;
        if (pthread_equal(s_threads[i].pt, self)) { slot = &s_threads[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < PIN_MAX_THREADS; i++)
            if (!s_threads[i].used) { slot = &s_threads[i]; break; }
    }
    if (slot) {
        slot->used = 1;
        slot->pt = self;
        slot->role = role;
        slot->lane = lane;
    }
    pthread_mutex_unlock(&s_threads_lock);
    apply_to_thread(self, mask_for(role, lane));
}

/* Non-weak link anchor. thread_name.h reaches ps3_thread_pin_by_name through a
 * weak reference, and a weak reference does not make the linker extract this
 * archive member; templates/project/main.cpp takes the address of this symbol
 * (a strong reference) so the whole object is linked. Without it PS3RECOMP_PIN
 * was silently a no-op. */
void ps3_thread_pin_link_anchor(void) { }

void ps3_thread_pin_by_name(const char* name)
{
    pthread_once(&s_once, init_mapping);
    if (!s_pin || !s_ready) return;
    int role = PIN_ROLE_OTHER;
    if (!strcmp(name, "rsx walker"))          role = PIN_ROLE_WALKER;
    else if (!strcmp(name, "ppu main"))       role = PIN_ROLE_MAIN;
    else if (!strcmp(name, "present"))        role = PIN_ROLE_PRESENT;
    else if (!strcmp(name, "ppu DisplayFlip")) role = PIN_ROLE_DISPLAYFLIP;
    else if (!strcmp(name, "spurs kernel"))   role = PIN_ROLE_SPURS_KERNEL;
    register_and_apply(role, -1);
}

/* The SPURS physical-lane workers are all named "spurs pm"; the lane index is
 * what makes layout B possible. Called right after the name. */
void ps3_thread_pin_spurs_lane(int lane)
{
    pthread_once(&s_once, init_mapping);
    if (!s_pin || !s_ready) return;
    register_and_apply(PIN_ROLE_LANE, lane);
}

/* Called from the walker's drain pass, right after ps3_live_ctl_poll(). Reads
 * the current layout name (control file first, then env) and reapplies every
 * registered thread's mask when it changed. A handful of strcmp per flip. */
void ps3_thread_pin_poll(void)
{
    pthread_once(&s_once, init_mapping);
    if (!s_pin || !s_ready) return;
    const char* name = current_layout_name();
    if (!name || !*name) name = "today";
    /* Compare the same truncation that is stored. A raw strcmp against the
     * 32-byte copy loops forever on any longer name and re-applies every
     * registered thread on every drain pass; that is what wedged and then
     * crashed the one run that got a malformed (semicolon-joined) value. */
    char shortname[PIN_LAYOUT_NAME_CAP];
    snprintf(shortname, sizeof shortname, "%s", name);
    int layout = layout_from_name(name);
    if (layout == s_layout && !strcmp(shortname, s_applied_name)) return;

    int changed = layout != s_layout;
    s_layout = layout;
    snprintf(s_layout_name, sizeof s_layout_name, "%s", shortname);
    snprintf(s_applied_name, sizeof s_applied_name, "%s", shortname);
    if (changed) build_layout_masks();

    fprintf(stderr, "[pin] layout -> %s\n", s_layout_name);
    fflush(stderr);

    pthread_mutex_lock(&s_threads_lock);
    for (int i = 0; i < PIN_MAX_THREADS; i++) {
        if (!s_threads[i].used) continue;
        if (s_threads[i].role == PIN_ROLE_OTHER) continue;
        apply_to_thread(s_threads[i].pt,
                        mask_for(s_threads[i].role, s_threads[i].lane));
    }
    pthread_mutex_unlock(&s_threads_lock);
}
