/*
 * Native cull sample-grid fix. See c690_cull_fix.h.
 */
#include "c690_cull_fix.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_CULL_HAVE_LIVE_CTL 1
#  endif
#elif defined(__GNUC__)
#  include "ps3_live_ctl.h"
#  define C690_CULL_HAVE_LIVE_CTL 1
#endif

#define C690_CULL_MAX_BLOCKS 8

typedef struct {
    uint32_t addr;
    uint32_t w0, w1;      /* original big-endian words */
} c690_cull_saved;

static unsigned long long s_calls, s_patched, s_no_block, s_blocks;
static int s_env_read;
static int s_env_fix, s_env_fix_2c;

static SPU_THREAD_LOCAL uint32_t t_blocks[C690_CULL_MAX_BLOCKS];
static SPU_THREAD_LOCAL int t_nblocks;
static SPU_THREAD_LOCAL int t_scanned;
static SPU_THREAD_LOCAL unsigned t_scan_tick;
static SPU_THREAD_LOCAL c690_cull_saved t_saved[C690_CULL_MAX_BLOCKS];
static SPU_THREAD_LOCAL int t_nsaved;

static int cull_live_get(int key, int fallback)
{
#ifdef C690_CULL_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) return ps3_live_ctl_get(key) != 0;
#endif
    (void)key;
    return fallback;
}

static void cull_read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char* a = getenv("PS3RECOMP_C690_CULL_FIX");
    const char* b = getenv("PS3RECOMP_C690_CULL_FIX_2C");
    s_env_fix = a && *a && a[0] != '0';
    s_env_fix_2c = b && *b && b[0] != '0';
    if (s_env_fix || s_env_fix_2c) {
        fprintf(stderr, "[c690-cull-fix] enabled family25/26=%d family2C=%d\n",
                s_env_fix, s_env_fix_2c);
        fflush(stderr);
    }
}

int c690_cull_fix_enabled(int family)
{
    cull_read_env();
    if (family == 2)
        return cull_live_get(PS3_LIVE_C690_CULL_FIX_2C, s_env_fix_2c);
    return cull_live_get(PS3_LIVE_C690_CULL_FIX, s_env_fix);
}

int c690_cull_fix_maybe(void)
{
    cull_read_env();
    if (s_env_fix || s_env_fix_2c) return 1;
#ifdef C690_CULL_HAVE_LIVE_CTL
    if (ps3_live_ctl_enabled) return ps3_live_ctl_enabled() != 0;
#endif
    return 0;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static float as_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static int finite_f(float f)
{
    return f == f && f > -1e30f && f < 1e30f;
}

/* The title stores its screen transform as
 *   { scale_x, offset_x, scale_y, offset_y }
 * in several 16-byte aligned per-lane copies. Both axes are first-quadrant
 * viewport values (|scale| <= |offset|, 16:9-ish), which is what separates
 * them from the gameplay matrices around them. */
static int looks_like_screen_xform(const uint8_t* ls, uint32_t a)
{
    const float w0 = as_float(be32(ls + a + 0));
    const float w1 = as_float(be32(ls + a + 4));
    const float w2 = as_float(be32(ls + a + 8));
    const float w3 = as_float(be32(ls + a + 12));
    if (!finite_f(w0) || !finite_f(w1) || !finite_f(w2) || !finite_f(w3))
        return 0;
    if (w1 < 300.0f || w1 > 5000.0f || w3 < 150.0f || w3 > 3000.0f) return 0;
    const float aspect = w1 / w3;
    if (aspect < 1.60f || aspect > 1.95f) return 0;
    const float rx = w0 / w1, ry = w2 / w3;
    if (rx < 0.5f || rx > 1.0f) return 0;
    if (ry < -1.0f || ry > -0.5f) return 0;
    return 1;
}

static void cull_scan(const struct spu_context* ctx)
{
    t_scanned = 1;
    t_nblocks = 0;
    const uint8_t* ls = ctx->ls;
    if (!ls) return;
    /* The geometry job builds its screen transform in the scratch region
     * above the command buffers; restricting the scan keeps gameplay
     * matrices and vertex data from matching the heuristic. */
    for (uint32_t a = 0x10000u; a + 16u <= 0x22000u; a += 16u) {
        if (!looks_like_screen_xform(ls, a)) continue;
        if (t_nblocks >= C690_CULL_MAX_BLOCKS) break;
        t_blocks[t_nblocks++] = a;
    }
    static SPU_THREAD_LOCAL int t_logged;
    if (t_nblocks && !t_logged) {
        t_logged = 1;
        fprintf(stderr, "[c690-cull-fix] screen transform blocks=%d first=0x%05X\n",
                t_nblocks, t_blocks[0]);
        fflush(stderr);
    }
}

int c690_cull_fix_enter(struct spu_context* ctx, int family)
{
    if (!ctx || !ctx->ls) return 0;
    if (!c690_cull_fix_enabled(family)) return 0;
    __atomic_fetch_add(&s_calls, 1ull, __ATOMIC_RELAXED);

    /* Rescan every 256 calls: the job context is rebuilt per job, and a lane
     * can switch to another screen layout. */
    if (!t_scanned || (++t_scan_tick & 0xFFu) == 0u) cull_scan(ctx);
    if (!t_nblocks) {
        __atomic_fetch_add(&s_no_block, 1ull, __ATOMIC_RELAXED);
        return 0;
    }

    t_nsaved = 0;
    for (int i = 0; i < t_nblocks; i++) {
        const uint32_t a = t_blocks[i];
        if (!looks_like_screen_xform(ctx->ls, a)) continue;
        const uint32_t w0 = be32(ctx->ls + a);
        const uint32_t w1 = be32(ctx->ls + a + 4);
        const float x0 = as_float(w0), x1 = as_float(w1);
        if (!finite_f(x0) || !finite_f(x1)) continue;
        c690_cull_saved* s = &t_saved[t_nsaved++];
        s->addr = a;
        s->w0 = w0;
        s->w1 = w1;
        /* Map the logical-pixel transform x = ndc*scale + offset onto the
         * 2x1 sample grid: x_sample = 2*x. y is left alone (one sample). */
        const float n0 = x0 * 2.0f, n1 = x1 * 2.0f;
        uint32_t u0, u1;
        memcpy(&u0, &n0, sizeof u0);
        memcpy(&u1, &n1, sizeof u1);
        put32(ctx->ls + a, u0);
        put32(ctx->ls + a + 4, u1);
    }
    if (!t_nsaved) {
        __atomic_fetch_add(&s_no_block, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_fetch_add(&s_patched, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_blocks, (unsigned long long)t_nsaved, __ATOMIC_RELAXED);
    return 1;
}

void c690_cull_fix_leave(struct spu_context* ctx)
{
    if (!ctx || !ctx->ls) return;
    for (int i = 0; i < t_nsaved; i++) {
        put32(ctx->ls + t_saved[i].addr, t_saved[i].w0);
        put32(ctx->ls + t_saved[i].addr + 4, t_saved[i].w1);
    }
    t_nsaved = 0;
}

void c690_cull_fix_get_counters(c690_cull_fix_counters* out)
{
    if (!out) return;
    out->calls = s_calls;
    out->patched = s_patched;
    out->no_block = s_no_block;
    out->blocks = s_blocks;
}

static void cull_fix_report(const char* tag)
{
    if (!s_calls) return;
    fprintf(stderr, "[c690-cull-fix] %s calls=%llu patched=%llu blocks=%llu "
                    "no_block=%llu\n", tag, s_calls, s_patched, s_blocks,
            s_no_block);
    fflush(stderr);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void cull_fix_dtor(void)
{
    cull_fix_report("exit");
}
#endif
