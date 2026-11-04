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
#define C690_CULL_GPR_BYTES  (128u * 16u)
#define C690_CULL_LIST_MAX   0x10000u

typedef struct {
    uint32_t addr;
    uint32_t w0, w1;      /* original big-endian words */
} c690_cull_saved;

static unsigned long long s_calls, s_patched, s_no_block, s_blocks;
static unsigned long long s_union_runs, s_union_added_tris;
static unsigned long long s_union_cap_hits, s_union_cap_skipped;
static unsigned long long s_union_odd_tail;
static unsigned long long s_union_skip_setup, s_union_skip_range;
static unsigned long long s_union_skip_realloc;
static int s_env_read;
static int s_env_fix, s_env_fix_2c;

static SPU_THREAD_LOCAL uint32_t t_blocks[C690_CULL_MAX_BLOCKS];
static SPU_THREAD_LOCAL int t_nblocks;
static SPU_THREAD_LOCAL int t_scanned;
static SPU_THREAD_LOCAL unsigned t_scan_tick;
static SPU_THREAD_LOCAL c690_cull_saved t_saved[C690_CULL_MAX_BLOCKS];
static SPU_THREAD_LOCAL int t_nsaved;

/* Union state (0x25/0x26). The LS/GPR snapshot is per lane and only
 * allocated while the switch is on. */
static SPU_THREAD_LOCAL uint8_t* t_ls_save;
static SPU_THREAD_LOCAL uint8_t* t_gpr_save;
static SPU_THREAD_LOCAL uint8_t* t_list_save;
static SPU_THREAD_LOCAL uint32_t t_list_cap;   /* realloc'd local copy */
static SPU_THREAD_LOCAL uint32_t t_list_n;
static SPU_THREAD_LOCAL uint32_t t_cull_ctx;
static SPU_THREAD_LOCAL uint32_t t_entry_ptr;   /* +0x110 at entry */
static SPU_THREAD_LOCAL uint32_t t_cap_entries; /* proven buffer capacity */
static SPU_THREAD_LOCAL uint32_t t_save_pc;
static SPU_THREAD_LOCAL uint32_t t_save_status;
static SPU_THREAD_LOCAL uint32_t t_save_stop;
static SPU_THREAD_LOCAL int t_save_image;
static SPU_THREAD_LOCAL int t_union_mode;   /* 0 none, 1 union, 2 simple */
static SPU_THREAD_LOCAL int t_union_depth;

/* The native screen key's mode 2 selects this union driver with the native
 * 0xD080 stage in place of the lifted one. */
extern int c690_native_screen_mode(void) __attribute__((weak));
static int native_screen_union(void)
{
    return c690_native_screen_mode && c690_native_screen_mode() == 2;
}

static int cull_live_get(int key, int fallback)
{
#ifdef C690_CULL_HAVE_LIVE_CTL
    /* Only trust the live table when a control file is configured.  A weak
     * link can resolve to a table that was seeded before this switch's
     * environment variable was set; in that case the env value is the only
     * source we have. */
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled())
        return ps3_live_ctl_get(key) != 0;
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
    return cull_live_get(PS3_LIVE_C690_CULL_FIX, s_env_fix) || native_screen_union();
}

/* MLB16_CULL_BYPASS: the generated c690 module calls this for every 0x25/0x26
 * and 0x2C cull dispatch. The value is a bitmask over the two families
 * (1 = both, 2 = 0x2C, 3 = 0x25/0x26); 0 is inert and is the default. The
 * control file wins while it is configured, so an A/B can flip it mid-run;
 * without PS3RECOMP_CTL it reads the environment once, as before. */
int c690_cull_bypass_mode(void)
{
    static int env = -1;
    if (env < 0) {
        const char* e = getenv("MLB16_CULL_BYPASS");
        env = e && *e ? atoi(e) : 0;
    }
#ifdef C690_CULL_HAVE_LIVE_CTL
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled())
        return ps3_live_ctl_get(PS3_LIVE_CULL_BYPASS);
#endif
    return env;
}

int c690_cull_fix_maybe(void)
{
    cull_read_env();
    if (native_screen_union()) return 1;
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

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
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

/* Patch every screen-transform copy onto the 2x1 sample grid.  The cull reads
 * the highest-address copy; the others are harmless duplicates. */
static int cull_apply_patch(struct spu_context* ctx)
{
    if (!ctx || !ctx->ls) return 0;

    /* The job context is rebuilt per job, so the block addresses move.  Rescan
     * when the cached addresses no longer look like a transform in this image
     * (and every 256 calls as a cheap drift check).  A job with no transform
     * rescans each call; that only happens while the fix is switched on. */
    int need_scan = !t_scanned || ((++t_scan_tick & 0xFFu) == 0u);
    if (!need_scan && t_nblocks) {
        int any = 0;
        for (int i = 0; i < t_nblocks; i++)
            if (looks_like_screen_xform(ctx->ls, t_blocks[i])) { any = 1; break; }
        if (!any) need_scan = 1;
    } else if (!need_scan && !t_nblocks) {
        need_scan = 1;
    }
    if (need_scan) cull_scan(ctx);
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

int c690_cull_fix_enter(struct spu_context* ctx, int family)
{
    if (!c690_cull_fix_enabled(family)) return 0;
    __atomic_fetch_add(&s_calls, 1ull, __ATOMIC_RELAXED);
    return cull_apply_patch(ctx);
}

void c690_cull_fix_leave(struct spu_context* ctx)
{
    if (!ctx || !ctx->ls) return;
    for (int i = 0; i < t_nsaved; i++) {
        put32(ctx->ls + t_saved[i].addr, t_saved[i].w0);
        put32(ctx->ls + t_saved[i].addr + 4, t_saved[i].w1);
    }
    t_nsaved = 0;
    if (t_union_mode == 2) {    /* single-grid family 2 path */
        t_union_mode = 0;
        t_union_depth = 0;
    }
}

/* Canonical triangle membership: order-insensitive, so the union does not add
 * the opposite winding of a triangle the game already keeps. */
static int list_has_tri(const uint8_t* list, uint32_t count,
                        uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t lo = a, mid = b, hi = c, t;
    if (lo > mid) { t = lo; lo = mid; mid = t; }
    if (mid > hi) { t = mid; mid = hi; hi = t; }
    if (lo > mid) { t = lo; lo = mid; mid = t; }
    for (uint32_t i = 0; i + 2 < count; i += 3) {
        uint16_t x = be16(list + 2 * i);
        uint16_t y = be16(list + 2 * (i + 1));
        uint16_t z = be16(list + 2 * (i + 2));
        uint16_t l = x, m = y, h = z;
        if (l > m) { t = l; l = m; m = t; }
        if (m > h) { t = m; m = h; h = t; }
        if (l > m) { t = l; l = m; m = t; }
        if (l == lo && m == mid && h == hi) return 1;
    }
    return 0;
}

static int union_ensure_buffers(void)
{
    if (!t_ls_save) {
        t_ls_save = (uint8_t*)malloc(SPU_LS_SIZE);
        if (!t_ls_save) return 0;
    }
    if (!t_gpr_save) {
        t_gpr_save = (uint8_t*)malloc(C690_CULL_GPR_BYTES);
        if (!t_gpr_save) return 0;
    }
    return 1;
}

/* The cull context (r3) describes the index list as:
 *   +0x110  pointer to the u16 index buffer
 *   +0x114  candidate index count (3 per triangle) the cull will consume
 *   +0x1E0  end of that buffer / start of the next allocation
 * +0x114 is a count, not a capacity: changing it by n changes the kept-index
 * count by n one-for-one on the captured pairs, and the cull reads exactly
 * that many candidates.  It writes its output back into the same buffer and
 * never past the input extent (canary probe: 0 writes beyond +0x110+2*+0x114
 * on all 400 pairs).  The bytes at +0x1E0 are the next live structure (a
 * command header, identical across frames; 7 captured buffers even end exactly
 * at another buffer's +0x110).  The proven writable extent is therefore
 * [ptr, +0x1E0); that is the capacity the union may append into. */
static int cull_list_capacity(const struct spu_context* ctx, uint32_t* out_ptr,
                              uint32_t* out_cnt, uint32_t* out_cap)
{
    const uint32_t c = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t ptr = be32(ctx->ls + c + 0x110) & SPU_LS_MASK;
    const uint32_t cnt = be32(ctx->ls + c + 0x114);
    const uint32_t end = be32(ctx->ls + c + 0x1E0) & SPU_LS_MASK;
    if (ptr == 0 || (ptr & 0xFu) || (end & 0xFu) || end <= ptr ||
        end > SPU_LS_SIZE || cnt > (end - ptr) / 2u ||
        cnt > C690_CULL_LIST_MAX)
        return 0;
    *out_ptr = ptr;
    *out_cnt = cnt;
    *out_cap = (end - ptr) / 2u;
    return 1;
}

int c690_cull_fix_begin(struct spu_context* ctx, int family)
{
    if (!ctx || !ctx->ls) return 0;
    if (!c690_cull_fix_enabled(family)) return 0;
    if (t_union_depth) return 0;    /* recursive cull: leave it to the caller */

    __atomic_fetch_add(&s_calls, 1ull, __ATOMIC_RELAXED);

    if (!union_ensure_buffers()) {
        __atomic_fetch_add(&s_union_skip_setup, 1ull, __ATOMIC_RELAXED);
        return 0;
    }

    /* The corpus snapshot is taken by the wrapper before this call, so this
     * copy is the same real input state. */
    memcpy(t_ls_save, ctx->ls, SPU_LS_SIZE);
    memcpy(t_gpr_save, ctx->gpr, C690_CULL_GPR_BYTES);
    t_save_pc = ctx->pc;
    t_save_status = ctx->status;
    t_save_stop = ctx->stop_code;
    t_save_image = ctx->image_id;
    t_cull_ctx = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    t_list_n = 0;

    uint32_t ptr, cnt, cap;
    if (!cull_list_capacity(ctx, &ptr, &cnt, &cap)) {
        __atomic_fetch_add(&s_union_skip_setup, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    t_entry_ptr = ptr;
    t_cap_entries = cap;

    t_union_depth = 1;
    if (!cull_apply_patch(ctx)) {
        t_union_depth = 0;
        return 0;
    }
    t_union_mode = (family == 2) ? 2 : 1;
    return t_union_mode;
}

void c690_cull_fix_mid(struct spu_context* ctx)
{
    if (!ctx || t_union_mode != 1) return;

    const uint32_t ptr = be32(ctx->ls + t_cull_ctx + 0x110) & SPU_LS_MASK;
    const uint32_t cnt = be32(ctx->ls + t_cull_ctx + 0x114);
    if (ptr != t_entry_ptr || cnt > t_cap_entries ||
        ptr + (size_t)cnt * 2u > SPU_LS_SIZE) {
        __atomic_fetch_add(&s_union_skip_range, 1ull, __ATOMIC_RELAXED);
    } else {
        if (cnt > t_list_cap) {
            uint8_t* p = (uint8_t*)realloc(t_list_save, (size_t)cnt * 2u);
            if (p) { t_list_save = p; t_list_cap = cnt; }
            else __atomic_fetch_add(&s_union_skip_realloc, 1ull, __ATOMIC_RELAXED);
        }
        if (t_list_save && cnt <= t_list_cap) {
            memcpy(t_list_save, ctx->ls + ptr, (size_t)cnt * 2u);
            t_list_n = cnt;
        }
    }

    /* Back to the game's input image before the unpatched run. */
    memcpy(ctx->ls, t_ls_save, SPU_LS_SIZE);
    memcpy(ctx->gpr, t_gpr_save, C690_CULL_GPR_BYTES);
    ctx->pc = t_save_pc;
    ctx->status = t_save_status;
    ctx->stop_code = t_save_stop;
    ctx->image_id = t_save_image;
    c690_cull_fix_leave(ctx);       /* t_nsaved = 0; restores the same words */
}

void c690_cull_fix_end(struct spu_context* ctx)
{
    if (!ctx || t_union_mode != 1) {
        t_union_mode = 0;
        t_union_depth = 0;
        return;
    }

    const uint32_t ptr = be32(ctx->ls + t_cull_ctx + 0x110) & SPU_LS_MASK;
    const uint32_t cnt = be32(ctx->ls + t_cull_ctx + 0x114);
    uint32_t out_cnt = cnt;

    /* The capacity is the proven buffer extent from +0x1E0, not the entry
     * count: appending stays inside this call's allocation and never reaches
     * the next live structure. */
    uint32_t cap = t_cap_entries;
    if (ptr != t_entry_ptr || cnt > cap) {
        __atomic_fetch_add(&s_union_skip_range, 1ull, __ATOMIC_RELAXED);
        cap = cnt;
    }
    if (cap > C690_CULL_LIST_MAX) cap = C690_CULL_LIST_MAX;
    if (ptr + (size_t)cap * 2u > SPU_LS_SIZE) cap = (SPU_LS_SIZE - ptr) / 2u;

    uint32_t added = 0;
    if (t_list_save && t_list_n && out_cnt <= cap) {
        if (t_list_n % 3u) __atomic_fetch_add(&s_union_odd_tail, 1ull, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i + 2 < t_list_n; i += 3) {
            const uint16_t a = be16(t_list_save + 2 * i);
            const uint16_t b = be16(t_list_save + 2 * (i + 1));
            const uint16_t c = be16(t_list_save + 2 * (i + 2));
            if (list_has_tri(ctx->ls + ptr, out_cnt, a, b, c)) continue;
            if (out_cnt + 3u > cap) {
                __atomic_fetch_add(&s_union_cap_hits, 1ull, __ATOMIC_RELAXED);
                __atomic_fetch_add(&s_union_cap_skipped, 1ull, __ATOMIC_RELAXED);
                continue;
            }
            put16(ctx->ls + ptr + 2u * out_cnt, a);
            put16(ctx->ls + ptr + 2u * (out_cnt + 1u), b);
            put16(ctx->ls + ptr + 2u * (out_cnt + 2u), c);
            out_cnt += 3u;
            added++;
        }
        put32(ctx->ls + t_cull_ctx + 0x114, out_cnt);
    }

    __atomic_fetch_add(&s_union_runs, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_union_added_tris, (unsigned long long)added, __ATOMIC_RELAXED);
    t_union_mode = 0;
    t_union_depth = 0;
}

void c690_cull_fix_get_counters(c690_cull_fix_counters* out)
{
    if (!out) return;
    out->calls = s_calls;
    out->patched = s_patched;
    out->no_block = s_no_block;
    out->blocks = s_blocks;
    out->union_runs = s_union_runs;
    out->union_added_tris = s_union_added_tris;
    out->union_cap_hits = s_union_cap_hits;
    out->union_cap_skipped = s_union_cap_skipped;
    out->union_odd_tail = s_union_odd_tail;
    out->union_skip_setup = s_union_skip_setup;
    out->union_skip_range = s_union_skip_range;
    out->union_skip_realloc = s_union_skip_realloc;
}

static void cull_fix_report(const char* tag)
{
    if (!s_calls) return;
    fprintf(stderr, "[c690-cull-fix] %s calls=%llu patched=%llu blocks=%llu "
                    "no_block=%llu union_runs=%llu added_tris=%llu "
                    "cap_hits=%llu cap_skipped=%llu odd_tail=%llu "
                    "skip_setup=%llu skip_range=%llu skip_realloc=%llu\n",
            tag, s_calls, s_patched, s_blocks, s_no_block, s_union_runs,
            s_union_added_tris, s_union_cap_hits, s_union_cap_skipped,
            s_union_odd_tail, s_union_skip_setup, s_union_skip_range,
            s_union_skip_realloc);
    fflush(stderr);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void cull_fix_dtor(void)
{
    cull_fix_report("exit");
}
#endif
