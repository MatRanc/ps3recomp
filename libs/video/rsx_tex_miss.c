/*
 * ps3recomp - dummy-texture census (see rsx_tex_miss.h)
 *
 * Compiled into rsx_draw_engine.c (like rsx_wcb.c) so every build of the
 * engine gets it, and standalone by its unit test. It has no engine
 * dependencies on purpose.
 */
#include "rsx_tex_miss.h"

#include <stdlib.h>
#include <string.h>

#define RSX_TEX_MISS_KEYS 2048u
#define RSX_TEX_MISS_HASH 4096u
#define RSX_TEX_MISS_TOP  10u
#define RSX_TEX_MISS_EVERY 60u

typedef struct {
    unsigned       used;
    unsigned       reason;
    uint32_t       location, offset, format, w, h, pitch, mips, cube;
    uint64_t       fp_hash;
    uint32_t       pipeline;
    unsigned long long count;
} rsx_tex_miss_key;

static int   s_state = -1;          /* -1 = read the env, 0 off, 1 on */
static unsigned long long s_run[RSX_TEX_MISS_REASONS];
static unsigned long long s_run_total;
static unsigned       s_frame[RSX_TEX_MISS_REASONS];
static unsigned       s_frame_total;
static rsx_tex_miss_key s_keys[RSX_TEX_MISS_KEYS];
static unsigned       s_nkeys;
static unsigned       s_hash[RSX_TEX_MISS_HASH];   /* key index + 1 */
static unsigned       s_dropped_keys;
static unsigned       s_frames_since_top = RSX_TEX_MISS_EVERY;

const char* rsx_tex_miss_reason_name(unsigned reason)
{
    switch (reason) {
    case RSX_TEX_MISS_FORMAT:  return "format";
    case RSX_TEX_MISS_SURFACE: return "surface";
    case RSX_TEX_MISS_ADDRESS: return "address";
    case RSX_TEX_MISS_SIZE:    return "size";
    default:                   return "other";
    }
}

static int rsx_tex_miss_env(void)
{
    const char* e = getenv("PS3RECOMP_TEX_MISS_LOG");
    return e && *e && strcmp(e, "0") != 0;
}

int rsx_tex_miss_enabled(void)
{
    if (s_state < 0) s_state = rsx_tex_miss_env();
    return s_state;
}

void rsx_tex_miss_set_enabled(int on)
{
    s_state = on ? 1 : 0;
}

void rsx_tex_miss_reset(void)
{
    memset(s_run, 0, sizeof s_run);
    memset(s_frame, 0, sizeof s_frame);
    memset(s_keys, 0, sizeof s_keys);
    memset(s_hash, 0, sizeof s_hash);
    s_run_total = 0;
    s_frame_total = 0;
    s_nkeys = 0;
    s_dropped_keys = 0;
    s_frames_since_top = RSX_TEX_MISS_EVERY;
}

static uint32_t rsx_tex_miss_hash_key(const rsx_tex_miss_key* k)
{
    uint64_t h = 1469598103934665603ull;
    const uint32_t v[9] = { k->reason, k->location, k->offset, k->format,
                            k->w, k->h, k->pitch, k->mips, k->cube };
    const unsigned char* p = (const unsigned char*)v;
    for (size_t i = 0; i < sizeof v; i++) h = (h ^ p[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (RSX_TEX_MISS_HASH - 1u);
}

static int rsx_tex_miss_key_equal(const rsx_tex_miss_key* a,
                                  const rsx_tex_miss_key* b)
{
    return a->reason == b->reason && a->location == b->location &&
           a->offset == b->offset && a->format == b->format &&
           a->w == b->w && a->h == b->h && a->pitch == b->pitch &&
           a->mips == b->mips && a->cube == b->cube;
}

void rsx_tex_miss_note(unsigned reason, uint32_t location, uint32_t offset,
                       uint32_t format, uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t mips, uint32_t cube,
                       uint64_t fp_hash, uint32_t pipeline)
{
    if (!rsx_tex_miss_enabled()) return;
    if (reason >= RSX_TEX_MISS_REASONS) reason = RSX_TEX_MISS_OTHER;

    s_frame[reason]++;
    s_frame_total++;
    s_run[reason]++;
    s_run_total++;

    rsx_tex_miss_key want;
    memset(&want, 0, sizeof want);
    want.used = 1; want.reason = reason;
    want.location = location; want.offset = offset; want.format = format;
    want.w = w; want.h = h; want.pitch = pitch;
    want.mips = mips; want.cube = cube;

    uint32_t slot = rsx_tex_miss_hash_key(&want);
    for (unsigned probe = 0; probe < RSX_TEX_MISS_HASH; probe++) {
        const unsigned idx = s_hash[slot];
        if (!idx) {
            if (s_nkeys >= RSX_TEX_MISS_KEYS) { s_dropped_keys++; return; }
            s_keys[s_nkeys] = want;
            s_keys[s_nkeys].count = 1;
            s_hash[slot] = s_nkeys + 1u;
            s_nkeys++;
            break;
        }
        if (rsx_tex_miss_key_equal(&s_keys[idx - 1u], &want)) {
            s_keys[idx - 1u].fp_hash = fp_hash;
            s_keys[idx - 1u].pipeline = pipeline;
            s_keys[idx - 1u].count++;
            return;
        }
        slot = (slot + 1u) & (RSX_TEX_MISS_HASH - 1u);
    }
}

static unsigned rsx_tex_miss_top(FILE* out, unsigned max)
{
    if (!s_nkeys) return 0;
    unsigned shown = 0;
    unsigned used[RSX_TEX_MISS_TOP];
    for (unsigned n = 0; n < max && n < RSX_TEX_MISS_TOP; n++) {
        unsigned best = RSX_TEX_MISS_KEYS;
        for (unsigned i = 0; i < s_nkeys; i++) {
            int seen = 0;
            for (unsigned k = 0; k < shown; k++) if (used[k] == i) { seen = 1; break; }
            if (seen) continue;
            if (best == RSX_TEX_MISS_KEYS || s_keys[i].count > s_keys[best].count)
                best = i;
        }
        if (best == RSX_TEX_MISS_KEYS) break;
        used[shown++] = best;
        const rsx_tex_miss_key* k = &s_keys[best];
        char tag[24] = "";
        if (k->format >> 8) snprintf(tag, sizeof tag, " kind=%u", k->format >> 8);
        fprintf(out, "[tex-miss] top %u: %s loc=%u addr=0x%08x fmt=0x%02x %ux%u "
                     "pitch=%u mips=%u cube=%u%s n=%llu fp=0x%016llx pipe=%u\n",
                shown, rsx_tex_miss_reason_name(k->reason), k->location,
                k->offset, k->format & 0xFFu, k->w, k->h, k->pitch,
                k->mips, k->cube, tag, k->count,
                (unsigned long long)k->fp_hash, k->pipeline);
    }
    if (s_dropped_keys)
        fprintf(out, "[tex-miss] top: %u distinct keys not listed (table full)\n",
                s_dropped_keys);
    return shown;
}

unsigned rsx_tex_miss_frame(FILE* out, unsigned frame_no)
{
    if (!rsx_tex_miss_enabled()) return 0;
    if (!out) out = stderr;
    const unsigned total = s_frame_total;
    if (total) {
        fprintf(out, "[tex-miss] frame=%u misses=%u format=%u surface=%u "
                     "address=%u size=%u other=%u total=%llu\n",
                frame_no, total, s_frame[RSX_TEX_MISS_FORMAT],
                s_frame[RSX_TEX_MISS_SURFACE], s_frame[RSX_TEX_MISS_ADDRESS],
                s_frame[RSX_TEX_MISS_SIZE], s_frame[RSX_TEX_MISS_OTHER],
                s_run_total);
        if (s_frames_since_top >= RSX_TEX_MISS_EVERY) {
            rsx_tex_miss_top(out, RSX_TEX_MISS_TOP);
            s_frames_since_top = 0;
        }
    }
    s_frames_since_top++;
    memset(s_frame, 0, sizeof s_frame);
    s_frame_total = 0;
    return total;
}

void rsx_tex_miss_flush(FILE* out)
{
    if (!rsx_tex_miss_enabled() || !s_run_total) return;
    if (!out) out = stderr;
    fprintf(out, "[tex-miss] end: misses=%llu format=%llu surface=%llu "
                 "address=%llu size=%llu other=%llu\n",
            s_run_total, s_run[RSX_TEX_MISS_FORMAT], s_run[RSX_TEX_MISS_SURFACE],
            s_run[RSX_TEX_MISS_ADDRESS], s_run[RSX_TEX_MISS_SIZE],
            s_run[RSX_TEX_MISS_OTHER]);
    rsx_tex_miss_top(out, RSX_TEX_MISS_TOP);
}

unsigned rsx_tex_miss_run_count(unsigned reason)
{
    return reason < RSX_TEX_MISS_REASONS ? (unsigned)s_run[reason] : 0;
}

unsigned long long rsx_tex_miss_total(void)
{
    return s_run_total;
}
