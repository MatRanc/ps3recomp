/*
 * Native c690 screen cull (0x25/0x26 command, LS 0xD080 subtree).
 * See c690_native_screen.h and docs/edge-native-step4.md.
 */
#include "c690_native_screen.h"
#include "c690_holes.h"
#include "c690_cull_fix.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>
#include <sys/stat.h>

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_SCR_HAVE_LIVE_CTL 1
#  endif
#elif defined(__GNUC__)
#  include "ps3_live_ctl.h"
#  define C690_SCR_HAVE_LIVE_CTL 1
#endif

/* Match the lifted module's path: SPU_FAST_FLOAT + the hardware FTZ/DAZ mode
 * (see c690_native_cull.c). */
#ifndef SPU_FAST_FLOAT
#define SPU_FAST_FLOAT 1
#endif
#ifndef SPU_LIFTED_FAST_LS
#define SPU_LIFTED_FAST_LS 1
#endif
#define SPU_FF_FTZ_HW 1
#include "spu_helpers.h"

/* Channel ABI used by the subtree (spu_channels.c).  The lifted module inlines
 * the MFC parameter latching; the out-of-line form is exact and this subtree
 * issues no MFC command (edge-native-step3.md: 0 channel ops per cull). */
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);

#define C690_SCR_GPR_BYTES (128u * 16u)
#define C690_SCR_LIST_MAX  0x10000u
#define C690_SCR_MAX_BLOCKS 8

static unsigned long long s_native_calls, s_mode1_calls;
static unsigned long long s_shadow_calls, s_shadow_mismatches;
static unsigned long long s_shadow_ls_bad, s_shadow_gpr_bad;
static unsigned long long s_lifted_ns, s_native_ns;
static unsigned long long s_holes_calls, s_holes_runs, s_holes_added;
static unsigned long long s_holes_cap_skipped, s_holes_skip_setup;
static unsigned long long s_holes_fallback, s_holes_ns;
static int s_env_read;
static int s_env_mode, s_env_shadow, s_env_holes;
static int s_dumped;

static SPU_THREAD_LOCAL spu_context* t_alt;      /* own LS, shadow native run */
static SPU_THREAD_LOCAL uint8_t* t_entry_ls;     /* shadow entry image */
static SPU_THREAD_LOCAL uint8_t* t_entry_gpr;
static SPU_THREAD_LOCAL unsigned t_entry_mxcsr;
static SPU_THREAD_LOCAL int t_holes_depth;

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

static int c690_scr_live_get(int key, int fallback)
{
#ifdef C690_SCR_HAVE_LIVE_CTL
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled())
        return ps3_live_ctl_get(key);
#endif
    (void)key;
    return fallback;
}

static void c690_scr_read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char* m = getenv("PS3RECOMP_C690_NATIVE_SCREEN");
    const char* s = getenv("PS3RECOMP_C690_NATIVE_SCREEN_SHADOW");
    const char* h = getenv("PS3RECOMP_C690_HOLES_NATIVE");
    s_env_mode = m && *m ? atoi(m) : 0;
    s_env_shadow = s && *s && s[0] != '0';
    s_env_holes = h && *h && h[0] != '0';
    if (s_env_mode || s_env_shadow || s_env_holes) {
        fprintf(stderr, "[c690-screen] enabled mode=%d shadow=%d holes=%d\n",
                s_env_mode, s_env_shadow, s_env_holes);
        fflush(stderr);
    }
}

static int c690_scr_mode(void)
{
    c690_scr_read_env();
#ifdef C690_SCR_HAVE_LIVE_CTL
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled()) {
        const int v = ps3_live_ctl_get(PS3_LIVE_C690_NATIVE_SCREEN);
        if (v >= 0 && v <= 2) return v;
    }
#endif
    return s_env_mode >= 0 && s_env_mode <= 2 ? s_env_mode : 0;
}

static int c690_scr_shadow(void)
{
    c690_scr_read_env();
#ifdef C690_SCR_HAVE_LIVE_CTL
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled())
        return ps3_live_ctl_get(PS3_LIVE_C690_NATIVE_SCREEN_SHADOW) != 0;
#endif
    return s_env_shadow;
}

static int c690_scr_holes(void)
{
    c690_scr_read_env();
#ifdef C690_SCR_HAVE_LIVE_CTL
    if (ps3_live_ctl_get && ps3_live_ctl_enabled && ps3_live_ctl_enabled())
        return ps3_live_ctl_get(PS3_LIVE_C690_HOLES_NATIVE) != 0;
#endif
    return s_env_holes;
}

int c690_native_screen_holes_active(void)
{
    return c690_scr_holes();
}

int c690_native_screen_enabled(void)
{
    return c690_scr_mode() != 0 || c690_scr_shadow() || c690_scr_holes();
}

int c690_native_screen_mode(void)
{
    return c690_scr_mode();
}

static void c690_scr_report(const char* tag)
{
    fprintf(stderr, "[c690-screen] %s native=%llu mode1=%llu shadow=%llu "
                    "mismatches=%llu ls_bad=%llu gpr_bad=%llu "
                    "lifted_ns=%llu native_ns=%llu "
                    "holes_calls=%llu holes_runs=%llu holes_added=%llu "
                    "holes_cap_skipped=%llu holes_skip_setup=%llu "
                    "holes_fallback=%llu holes_ns=%llu\n",
            tag, s_native_calls, s_mode1_calls, s_shadow_calls,
            s_shadow_mismatches, s_shadow_ls_bad, s_shadow_gpr_bad,
            s_lifted_ns, s_native_ns, s_holes_calls, s_holes_runs,
            s_holes_added, s_holes_cap_skipped, s_holes_skip_setup,
            s_holes_fallback, s_holes_ns);
    fflush(stderr);
}

void c690_native_screen_get_counters(c690_native_screen_counters* out)
{
    if (!out) return;
    out->native_calls = s_native_calls;
    out->mode1_calls = s_mode1_calls;
    out->shadow_calls = s_shadow_calls;
    out->shadow_mismatches = s_shadow_mismatches;
    out->shadow_ls_bad = s_shadow_ls_bad;
    out->shadow_gpr_bad = s_shadow_gpr_bad;
    out->union_added_tris = 0;
    out->union_cap_hits = 0;
    out->union_skipped = 0;
    out->lifted_ns = s_lifted_ns;
    out->native_ns = s_native_ns;
    out->holes_calls = s_holes_calls;
    out->holes_runs = s_holes_runs;
    out->holes_added_tris = s_holes_added;
    out->holes_cap_skipped = s_holes_cap_skipped;
    out->holes_skip_setup = s_holes_skip_setup;
    out->holes_fallback = s_holes_fallback;
    out->holes_ns = s_holes_ns;
}

/* The subtree body (generated). */
#include "c690_native_screen_stage.inc"

static void c690_scr_run_stage_mx(spu_context* ctx, unsigned entry_mxcsr)
{
    const unsigned cur = _mm_getcsr();
    _mm_setcsr(entry_mxcsr | 0x8040u);   /* FTZ | DAZ, same as the native stage */
    c690_native_screen_stage(ctx);
    _mm_setcsr(cur);
    /* The flattened subtree's terminal SPU_RET publishes r0.  The lifted
     * bracket around 0xD080 is a host call (host_depth > 0), so it returns to
     * the caller's drain rather than the indirect-branch trampoline. */
    ctx->pc = ctx->gpr[0]._u32[0];
}

static void c690_scr_run_stage(spu_context* ctx)
{
    c690_scr_run_stage_mx(ctx, _mm_getcsr());
}

void c690_native_screen_run_once(spu_context* ctx)
{
    if (!ctx || !ctx->ls) return;
    c690_scr_run_stage(ctx);
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static spu_context* alt_context(void)
{
    if (t_alt) return t_alt;
    spu_context* p = (spu_context*)calloc(1, sizeof *p);
    uint8_t* ls = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!p || !ls) { free(p); free(ls); return NULL; }
    p->ls = ls;
    t_alt = p;
    return p;
}

static int shadow_alloc(void)
{
    if (t_entry_ls && t_entry_gpr) return 1;
    if (!t_entry_ls) t_entry_ls = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!t_entry_gpr) t_entry_gpr = (uint8_t*)malloc(C690_SCR_GPR_BYTES);
    return t_entry_ls && t_entry_gpr;
}

/* ---- fused native holes pass (0x25/0x26) ------------------------------
 *
 * The 0x132E0 caller passes the cull three things in r3/r4/r5:
 *   r3  pointer to the u16 index buffer (candidates in, kept list out)
 *   r4  pointer to a 16-byte stack slot; word 0 is the candidate count and
 *       word 2 is overwritten by 0xD080 with the kept *triangle* count
 *   r5  end of the index buffer (the first byte of the next allocation)
 * After 0xD080 returns the caller reads word 2, multiplies it by 3 and writes
 * that as +0x114 (the kept index count), then writes +0x118 and +0xA0.  The
 * fused pass therefore leaves the caller's write untouched (the game-grid
 * result, exactly as the lifted run) and merges the sample-only triangles
 * into the list/count afterwards, in c690_cull_fix_end().  That is the same
 * output as the proven union driver, but the command body runs once.
 *
 * The sample evaluation uses a private context (+0x1E0 cap from the caller)
 * so the accepted game-grid LS/GPR state is never snapshot/restored. */

#define C690_HOLES_MAX_TRIS (C690_SCR_LIST_MAX / 3u)

static SPU_THREAD_LOCAL uint32_t t_holes_ptr;
static SPU_THREAD_LOCAL uint32_t t_holes_sample_tris;
static SPU_THREAD_LOCAL const uint8_t* t_holes_sample_ls;
static SPU_THREAD_LOCAL c690_tri_set t_tri_set;

/* Run the game-grid stage on the real context and stash the sample-grid list
 * for the caller-side merge.  Returns 0 when the caller must run the lifted
 * bracket (bad layout / no scratch), 1 when the native game result is final. */
static int run_holes(spu_context* ctx)
{
    t_holes_sample_tris = 0;
    t_holes_sample_ls = NULL;
    if (t_holes_depth) return 0;      /* nested cull: let the lifted path run */

    const uint32_t ptr = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t slot = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    const uint32_t end = ctx->gpr[5]._u32[0] & SPU_LS_MASK;
    if (!ptr || (ptr & 0xFu) || (slot & 0xFu) || slot + 16u > SPU_LS_SIZE ||
        (end & 0xFu) || end <= ptr || end > SPU_LS_SIZE) {
        __atomic_fetch_add(&s_holes_skip_setup, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    /* Slot word 0 is the candidate *triangle* count (word 2 is the kept
     * triangle count); the caller's +0x114 holds index counts. */
    const uint32_t cand = be32(ctx->ls + slot);
    const uint32_t cap_indices = (end - ptr) / 2u;
    if (cand > cap_indices / 3u || cand > C690_HOLES_MAX_TRIS) {
        __atomic_fetch_add(&s_holes_skip_setup, 1ull, __ATOMIC_RELAXED);
        return 0;
    }

    const long long t0 = now_ns();
    __atomic_fetch_add(&s_holes_calls, 1ull, __ATOMIC_RELAXED);
    t_holes_ptr = ptr;

    spu_context* alt = alt_context();
    int sample_ok = 0;
    /* No transform block means the sample-grid run is identical to the game
     * run (the union is inert), so skip the LS copy entirely. */
    if (alt && alt->ls &&
        c690_cull_fix_probe_transform(ctx, c690_cull_fix_holes_family())) {
        memcpy(alt->ls, ctx->ls, SPU_LS_SIZE);
        memcpy(alt->gpr, ctx->gpr, C690_SCR_GPR_BYTES);
        alt->pc = ctx->pc;
        alt->image_id = ctx->image_id;
        alt->status = ctx->status;
        alt->stop_code = ctx->stop_code;
        alt->host_depth = 0;
        const unsigned mx = _mm_getcsr();
        if (c690_cull_fix_patch_2x(alt)) {
            c690_scr_run_stage_mx(alt, mx);
            const uint32_t n = be32(alt->ls + slot + 8);
            if (n <= cap_indices / 3u) {
                t_holes_sample_tris = n;
                t_holes_sample_ls = alt->ls;
                sample_ok = 1;
            }
            c690_cull_fix_leave(alt);
            __atomic_fetch_add(&s_holes_runs, 1ull, __ATOMIC_RELAXED);
        }
    }

    /* The accepted result: the exact game-grid run on the real context. */
    t_holes_depth = 1;
    c690_scr_run_stage(ctx);
    t_holes_depth = 0;
    __atomic_fetch_add(&s_native_calls, 1ull, __ATOMIC_RELAXED);
    if (!sample_ok)
        __atomic_fetch_add(&s_holes_fallback, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_holes_ns, (unsigned long long)(now_ns() - t0),
                       __ATOMIC_RELAXED);
    return 1;
}

/* Merge the stashed sample-grid list into the game list after the caller has
 * written its count.  Canonical vertex-set membership, sample-list order, and
 * the caller's +0x1E0 buffer cap, exactly like the lifted union driver. */
uint32_t c690_native_screen_holes_merge(spu_context* ctx, uint32_t ptr,
                                        uint32_t cnt_indices,
                                        uint32_t cap_indices)
{
    const uint32_t sample_tris = t_holes_sample_tris;
    const uint8_t* sample_ls = t_holes_sample_ls;
    t_holes_sample_tris = 0;
    t_holes_sample_ls = NULL;
    if (!sample_tris || !sample_ls || ptr != t_holes_ptr ||
        (cnt_indices % 3u) != 0u)
        return 0;
    const uint32_t game_tris = cnt_indices / 3u;
    if (game_tris > cap_indices / 3u) return 0;

    unsigned cap_skipped = 0;
    const uint32_t added = c690_holes_merge_lists(
        ctx->ls, sample_ls, ptr, game_tris, sample_tris, cap_indices,
        &t_tri_set, &cap_skipped);
    if (cap_skipped)
        __atomic_fetch_add(&s_holes_cap_skipped, (unsigned long long)cap_skipped,
                           __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_holes_added, (unsigned long long)added, __ATOMIC_RELAXED);
    return added;
}

/* The lifted call bracket: 0x132E0 does host_depth++ / image save, calls
 * 0xD080, then spu_drain_call() waits for the SPU_RET. */
static void call_lifted(spu_context* ctx, uint32_t ret_pc,
                        c690_native_screen_lifted_fn lifted)
{
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, ret_pc);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
}

/* Exact lift replacement (mode 0). */
static void run_mode0(spu_context* ctx)
{
    c690_scr_run_stage(ctx);
    __atomic_fetch_add(&s_native_calls, 1ull, __ATOMIC_RELAXED);
}

/* ---- shadow comparison ------------------------------------------------ */

static void shadow_dump(spu_context* ctx)
{
    const char* dump = getenv("PS3RECOMP_C690_NATIVE_DUMP");
    if (!dump || !*dump) return;
    mkdir(dump, 0777);
    char path[1200];
    FILE* f;
    snprintf(path, sizeof path, "%s/screen_mismatch_entry.bin", dump);
    f = fopen(path, "wb");
    if (f) { fwrite(t_entry_ls, 1, SPU_LS_SIZE, f);
             fwrite(t_entry_gpr, 1, C690_SCR_GPR_BYTES, f); fclose(f); }
    snprintf(path, sizeof path, "%s/screen_mismatch_lifted.bin", dump);
    f = fopen(path, "wb");
    if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
             fwrite(ctx->gpr, 1, C690_SCR_GPR_BYTES, f); fclose(f); }
}

static void shadow_compare(spu_context* ctx)
{
    spu_context* alt = alt_context();
    if (!alt || !shadow_alloc()) return;
    memcpy(alt->ls, t_entry_ls, SPU_LS_SIZE);
    memcpy(alt->gpr, t_entry_gpr, C690_SCR_GPR_BYTES);
    const long long t0 = now_ns();
    c690_scr_run_stage_mx(alt, t_entry_mxcsr);
    const long long t1 = now_ns();
    __atomic_fetch_add(&s_native_ns, (unsigned long long)(t1 - t0), __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_shadow_calls, 1ull, __ATOMIC_RELAXED);

    size_t ls_bad = 0, first = (size_t)-1;
    for (size_t i = 0; i < SPU_LS_SIZE; i++) {
        if (alt->ls[i] != ctx->ls[i]) {
            if (first == (size_t)-1) first = i;
            ls_bad++;
        }
    }
    unsigned gpr_bad = 0, first_gpr = 128;
    for (unsigned r = 0; r < 128; r++) {
        if (memcmp(&alt->gpr[r], &ctx->gpr[r], 16) != 0) {
            if (first_gpr == 128) first_gpr = r;
            gpr_bad++;
        }
    }
    const unsigned nat_pc = alt->gpr[0]._u32[0];
    const unsigned pc_bad = (nat_pc != ctx->pc) ? 1u : 0u;
    if (ls_bad || gpr_bad || pc_bad) {
        __atomic_fetch_add(&s_shadow_mismatches, 1ull, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_shadow_ls_bad, (unsigned long long)ls_bad, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_shadow_gpr_bad, (unsigned long long)gpr_bad, __ATOMIC_RELAXED);
        if (!s_dumped) {
            s_dumped = 1;
            shadow_dump(ctx);
            fprintf(stderr, "[c690-screen] SHADOW MISMATCH ls_bytes=%zu first=0x%05zX "
                            "gprs=%u first=r%u pc_native=0x%05X pc_lifted=0x%05X\n",
                    ls_bad, first, gpr_bad, first_gpr, nat_pc, ctx->pc);
            if (first != (size_t)-1) {
                const size_t base = first & ~(size_t)15;
                fprintf(stderr, "[c690-screen]  LS 0x%05zX native ", base);
                for (int b = 0; b < 16; b++) fprintf(stderr, "%02X", alt->ls[base + b]);
                fprintf(stderr, " lifted ");
                for (int b = 0; b < 16; b++) fprintf(stderr, "%02X", ctx->ls[base + b]);
                fprintf(stderr, "\n");
            }
            if (first_gpr < 128) {
                fprintf(stderr, "[c690-screen]  r%u native %08X%08X%08X%08X lifted %08X%08X%08X%08X\n",
                        first_gpr, alt->gpr[first_gpr]._u32[0], alt->gpr[first_gpr]._u32[1],
                        alt->gpr[first_gpr]._u32[2], alt->gpr[first_gpr]._u32[3],
                        ctx->gpr[first_gpr]._u32[0], ctx->gpr[first_gpr]._u32[1],
                        ctx->gpr[first_gpr]._u32[2], ctx->gpr[first_gpr]._u32[3]);
            }
            fflush(stderr);
        }
    }
}

int c690_native_screen_try(spu_context* ctx, uint32_t ret_pc,
                           c690_native_screen_lifted_fn lifted)
{
    if (!ctx || !ctx->ls || !lifted) return 0;
    const int mode = c690_scr_mode();
    const int shadow = c690_scr_shadow();
    const int holes = c690_scr_holes();
    if (!mode && !shadow && !holes) return 0;

    if (shadow) {
        if (!shadow_alloc()) return 0;
        t_entry_mxcsr = _mm_getcsr();
        memcpy(t_entry_ls, ctx->ls, SPU_LS_SIZE);
        memcpy(t_entry_gpr, ctx->gpr, C690_SCR_GPR_BYTES);
        const long long t0 = now_ns();
        call_lifted(ctx, ret_pc, lifted);
        const long long t1 = now_ns();
        __atomic_fetch_add(&s_lifted_ns, (unsigned long long)(t1 - t0), __ATOMIC_RELAXED);
        shadow_compare(ctx);        /* native is always mode 0 in shadow */
        if ((s_shadow_calls & 0xFFull) == 0ull) c690_scr_report("shadow-progress");
        return 1;
    }

    /* Fused holes pass: one 0x132E0 body pass.  The D080 hook runs the game
     * grid on the real context and the sample grid on a private copy; the
     * caller then writes its kept count and c690_cull_fix_end appends the
     * sample-only triangles and bumps +0x114. */
    if (holes) {
        const int r = run_holes(ctx);
        if ((s_holes_calls & 0x3FFull) == 0ull) c690_scr_report("holes-progress");
        return r;
    }

    /* Mode 2 runs the exact stage here; c690_cull_fix's union driver (enabled
     * by the same key) wraps the command and does the 2x/original append. */
    run_mode0(ctx);
    if ((s_native_calls & 0x3FFull) == 0ull) c690_scr_report("native-progress");
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void c690_screen_dtor(void)
{
    if (!s_native_calls && !s_shadow_calls && !s_holes_calls) return;
    c690_scr_report("exit");
}
#endif
