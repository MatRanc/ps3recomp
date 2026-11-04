/*
 * Native c690 tree/cull stage: switch, layout guard, shadow comparison.
 * See c690_native_cull.h and docs/edge-native-step2.md.
 */
#include "c690_native_cull.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include <sys/stat.h>

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_NATIVE_HAVE_LIVE_CTL 1
#  endif
#elif defined(__GNUC__)
#  include "ps3_live_ctl.h"
#  define C690_NATIVE_HAVE_LIVE_CTL 1
#endif

/* The native kernel uses the same helpers as the lifted module, with the
 * hardware denormal mode standing in for the per-operand SPU mask. */
/* This translation unit is part of the runtime library, which does not carry
 * the lifted module's SPU float defines. The native stage replaces lifted code,
 * so it must use the same float path (SPU_FAST_FLOAT + -mfma in this build);
 * otherwise last-bit rounding differs from the lifted stage. */
#ifndef SPU_FAST_FLOAT
#define SPU_FAST_FLOAT 1
#endif
#ifndef SPU_LIFTED_FAST_LS
#define SPU_LIFTED_FAST_LS 1
#endif
#define SPU_FF_FTZ_HW 1
#include "spu_helpers.h"
#include "c690_native_stage.inc"

#define C690_IMAGE_ID 6

static unsigned long long s_native_calls, s_fallbacks;
static unsigned long long s_shadow_calls, s_shadow_mismatches;
static unsigned long long s_shadow_ls_bad, s_shadow_gpr_bad;
static int s_env_read;
static int s_env_cull, s_env_shadow;
static int s_dumped;

static SPU_THREAD_LOCAL int t_depth;
static SPU_THREAD_LOCAL int t_armed;
static SPU_THREAD_LOCAL unsigned t_entry_mxcsr;
static SPU_THREAD_LOCAL uint8_t* t_entry_ls;
static SPU_THREAD_LOCAL uint8_t* t_entry_gpr;
static SPU_THREAD_LOCAL spu_context* t_nat;

static int c690_native_live_get(int key, int fallback)
{
#ifdef C690_NATIVE_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) return ps3_live_ctl_get(key) != 0;
#endif
    (void)key;
    return fallback;
}

static void c690_native_read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char* c = getenv("PS3RECOMP_C690_NATIVE_CULL");
    const char* s = getenv("PS3RECOMP_C690_NATIVE_SHADOW");
    s_env_cull = c && *c && c[0] != '0';
    s_env_shadow = s && *s && s[0] != '0';
    if (s_env_cull || s_env_shadow) {
        fprintf(stderr, "[c690-native] enabled cull=%d shadow=%d\n",
                s_env_cull, s_env_shadow);
        fflush(stderr);
    }
}

static void c690_native_report(const char* tag)
{
    fprintf(stderr, "[c690-native] %s native=%llu fallbacks=%llu shadow=%llu "
                    "mismatches=%llu ls_bytes_bad=%llu gpr_bad=%llu\n",
            tag, s_native_calls, s_fallbacks, s_shadow_calls, s_shadow_mismatches,
            s_shadow_ls_bad, s_shadow_gpr_bad);
    fflush(stderr);
}

static unsigned long long s_enable_tick;

int c690_native_cull_enabled(void)
{
    c690_native_read_env();
    const int cull = c690_native_live_get(PS3_LIVE_C690_NATIVE_CULL, s_env_cull);
    const int shadow = c690_native_live_get(PS3_LIVE_C690_NATIVE_SHADOW, s_env_shadow);
    if (cull || shadow) {
        const unsigned long long t =
            __atomic_fetch_add(&s_enable_tick, 1ull, __ATOMIC_RELAXED) + 1ull;
        if ((t & 0xFFull) == 0ull) c690_native_report("progress");
    }
    return cull || shadow;
}

int c690_native_cull_maybe(void)
{
    if (c690_native_cull_enabled()) return 1;
#ifdef C690_NATIVE_HAVE_LIVE_CTL
    if (ps3_live_ctl_enabled) return ps3_live_ctl_enabled() != 0;
#endif
    return 0;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* The captured c690 cull layout. Reject anything else: the caller then runs
 * the lifted stage (counted). */
static int c690_layout_ok(const spu_context* ctx)
{
    if (!ctx || ctx->image_id != C690_IMAGE_ID) return 0;
    const uint32_t r1 = ctx->gpr[1]._u32[0];
    const uint32_t r3 = ctx->gpr[3]._u32[0];
    const uint32_t r4 = ctx->gpr[4]._u32[0];
    const uint32_t r5 = ctx->gpr[5]._u32[0];
    const uint32_t r6 = ctx->gpr[6]._u32[0];
    const uint32_t r7 = ctx->gpr[7]._u32[0];
    const uint32_t r8 = ctx->gpr[8]._u32[0];
    /* r2 is a byte-offset stream pointer and may be unaligned; the kernel's
     * LS helpers mask every access. r3/r4/r5/r8 are 16-byte batch bases. */
    const uint32_t ptrs = r3 | r4 | r5 | r8;
    if (r1 < 0x1000u || r1 > 0x3F800u || (r1 & 0xFu)) return 0;
    if (ptrs & ~(SPU_LS_SIZE - 1u)) return 0;
    if (ptrs & 0xFu) return 0;
    if (r6 > 0x100000u || r7 > 0x100000u) return 0;
    /* Descriptor at LS 0x1330: word 0 is the job/batch count, word 1 the
     * stream count (8 in every cull layout captured so far), word 3 the
     * SPURS instance EA. Word 7 is a layout flag and is not checked. */
    const uint8_t* p = ctx->ls;
    if (!p) return 0;
    const uint32_t w1 = be32(p + 0x1334);
    const uint32_t w3 = be32(p + 0x133C);
    if (w1 != 8u) return 0;
    if (w3 != 0x01243E00u) return 0;
    return 1;
}

static int c690_shadow_alloc(void)
{
    if (t_nat) return 1;
    void* p = malloc(sizeof(spu_context) + SPU_LS_SIZE + 128u * 16u);
    uint8_t* ls = malloc(SPU_LS_SIZE);
    uint8_t* gpr = malloc(128u * 16u);
    if (!p || !ls || !gpr) {
        free(p); free(ls); free(gpr);
        return 0;
    }
    t_nat = (spu_context*)p;
    memset(t_nat, 0, sizeof *t_nat);
    t_nat->ls = ls;
    t_entry_ls = malloc(SPU_LS_SIZE);
    t_entry_gpr = gpr;
    if (!t_entry_ls) return 0;
    return 1;
}

/* Run with the rounding mode in effect at stage entry. The lifted stage runs
 * its arithmetic under that mode; the body can leave a truncate mode behind
 * (the SPU's own mode), which must not leak into the replacement. */
static void c690_native_run(spu_context* ctx, unsigned entry_mxcsr,
                            unsigned long long* calls, int live)
{
    const unsigned cur = _mm_getcsr();
    _mm_setcsr(entry_mxcsr | 0x8040u);   /* entry rounding | FTZ | DAZ */
    c690_native_stage(ctx);
    _mm_setcsr(cur);
    /* The flattened kernel keeps the final register file but, unlike the
     * lifted body's terminal SPU_RET, it must publish the return PC itself.
     * The corpus test compared LS+GPR only, which is why this was missed. */
    if (live) {
        ctx->pc = ctx->gpr[0]._u32[0];
        if (ctx->host_depth == 0)
            g_spu_trampoline_fn = spu_indirect_branch;
    }
    if (calls) __atomic_fetch_add(calls, 1ull, __ATOMIC_RELAXED);
}

int c690_native_cull_enter(spu_context* ctx)
{
    c690_native_read_env();
    const int outer = (t_depth == 0);
    t_depth++;
    if (!outer) return 0;

    const int live = c690_native_live_get(PS3_LIVE_C690_NATIVE_CULL, s_env_cull);
    const int shadow = c690_native_live_get(PS3_LIVE_C690_NATIVE_SHADOW, s_env_shadow);
    if (!live && !shadow) return 0;   /* outer lifted stage; leave() closes */
    if (shadow) {
        static int once;
        if (!once) {
            once = 1;
            fprintf(stderr, "[c690-native] shadow active (first outer entry)\n");
            fflush(stderr);
        }
    }

    if (!shadow) {
        /* Live: the native call replaces the whole body, so there is no
         * recursion through this wrapper and the depth counter stays 0. */
        if (!c690_layout_ok(ctx)) {
            __atomic_fetch_add(&s_fallbacks, 1ull, __ATOMIC_RELAXED);
            return 0;
        }
        c690_native_run(ctx, _mm_getcsr(), &s_native_calls, 1);
        if ((s_native_calls & 0xFFull) == 0ull) c690_native_report("native-progress");
        t_depth = 0;
        return 1;
    }

    /* Shadow: only the outermost entry stashes and compares. */
    t_armed = 0;
    if (!c690_layout_ok(ctx)) {
        __atomic_fetch_add(&s_fallbacks, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    if (!c690_shadow_alloc()) return 0;
    t_entry_mxcsr = _mm_getcsr();
    memcpy(t_entry_ls, ctx->ls, SPU_LS_SIZE);
    memcpy(t_entry_gpr, ctx->gpr, 128u * 16u);
    t_armed = 1;
    return 0;
}

void c690_native_cull_leave(spu_context* ctx)
{
    if (t_depth == 0) return;
    if (--t_depth != 0) return;
    if (!t_armed) return;
    t_armed = 0;
    if (!t_nat || !t_entry_ls) return;

    memcpy(t_nat->ls, t_entry_ls, SPU_LS_SIZE);
    memcpy(t_nat->gpr, t_entry_gpr, 128u * 16u);
    t_nat->image_id = ctx->image_id;
    t_nat->pc = ctx->pc;
    t_nat->int_enable = ctx->int_enable;
    t_nat->event_status = ctx->event_status;
    c690_native_run(t_nat, t_entry_mxcsr, NULL, 0);
    const unsigned long long shadow_n =
        __atomic_fetch_add(&s_shadow_calls, 1ull, __ATOMIC_RELAXED) + 1ull;
    if ((shadow_n & 0xFFull) == 0ull) c690_native_report("shadow-progress");

    size_t ls_bad = 0, first = (size_t)-1;
    for (size_t i = 0; i < SPU_LS_SIZE; i++) {
        if (t_nat->ls[i] != ctx->ls[i]) {
            if (first == (size_t)-1) first = i;
            ls_bad++;
        }
    }
    unsigned gpr_bad = 0, first_gpr = 128;
    for (unsigned r = 0; r < 128; r++) {
        if (memcmp(&t_nat->gpr[r], &ctx->gpr[r], 16) != 0) {
            if (first_gpr == 128) first_gpr = r;
            gpr_bad++;
        }
    }
    /* The lifted terminal SPU_RET publishes gpr[0] as pc; the native kernel
     * must agree on it even though the corpus gate compared GPRs only. */
    const unsigned nat_pc = t_nat->gpr[0]._u32[0];
    const unsigned pc_bad = (nat_pc != ctx->pc) ? 1u : 0u;
    if (ls_bad || gpr_bad || pc_bad) {
        __atomic_fetch_add(&s_shadow_mismatches, 1ull, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_shadow_ls_bad, (unsigned long long)ls_bad, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_shadow_gpr_bad, (unsigned long long)gpr_bad, __ATOMIC_RELAXED);
        if (!s_dumped) {
            s_dumped = 1;
            const char* dump = getenv("PS3RECOMP_C690_NATIVE_DUMP");
            if (dump && *dump) {
                mkdir(dump, 0777);
                char path[1200];
                FILE* f;
                snprintf(path, sizeof path, "%s/mismatch_entry.bin", dump);
                f = fopen(path, "wb");
                if (f) { fwrite(t_entry_ls, 1, SPU_LS_SIZE, f);
                         fwrite(t_entry_gpr, 1, 128u * 16u, f); fclose(f); }
                snprintf(path, sizeof path, "%s/mismatch_lifted.bin", dump);
                f = fopen(path, "wb");
                if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
                         fwrite(ctx->gpr, 1, 128u * 16u, f); fclose(f); }
                snprintf(path, sizeof path, "%s/mismatch_native.bin", dump);
                f = fopen(path, "wb");
                if (f) { fwrite(t_nat->ls, 1, SPU_LS_SIZE, f);
                         fwrite(t_nat->gpr, 1, 128u * 16u, f); fclose(f); }
                fprintf(stderr, "[c690-native]  dumped entry/lifted/native to %s\n", dump);
            }
            fprintf(stderr, "[c690-native] SHADOW MISMATCH ls_bytes=%zu first=0x%05zX "
                            "gprs=%u first=r%u pc_native=0x%05X pc_lifted=0x%05X\n",
                    ls_bad, first, gpr_bad, first_gpr, nat_pc, ctx->pc);
            if (first != (size_t)-1) {
                const size_t base = first & ~(size_t)15;
                fprintf(stderr, "[c690-native]  LS 0x%05zX native ", base);
                for (int b = 0; b < 16; b++) fprintf(stderr, "%02X", t_nat->ls[base + b]);
                fprintf(stderr, " lifted ");
                for (int b = 0; b < 16; b++) fprintf(stderr, "%02X", ctx->ls[base + b]);
                fprintf(stderr, "\n");
            }
            if (first_gpr < 128) {
                fprintf(stderr, "[c690-native]  r%u native %08X%08X%08X%08X lifted %08X%08X%08X%08X\n",
                        first_gpr, t_nat->gpr[first_gpr]._u32[0], t_nat->gpr[first_gpr]._u32[1],
                        t_nat->gpr[first_gpr]._u32[2], t_nat->gpr[first_gpr]._u32[3],
                        ctx->gpr[first_gpr]._u32[0], ctx->gpr[first_gpr]._u32[1],
                        ctx->gpr[first_gpr]._u32[2], ctx->gpr[first_gpr]._u32[3]);
            }
            fflush(stderr);
        }
    }
}

void c690_native_cull_get_counters(c690_native_cull_counters* out)
{
    if (!out) return;
    out->native_calls = s_native_calls;
    out->fallbacks = s_fallbacks;
    out->shadow_calls = s_shadow_calls;
    out->shadow_mismatches = s_shadow_mismatches;
    out->shadow_ls_bytes_bad = s_shadow_ls_bad;
    out->shadow_gpr_bad = s_shadow_gpr_bad;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void c690_native_cull_dtor(void)
{
    if (!s_native_calls && !s_fallbacks && !s_shadow_calls) return;
    c690_native_report("exit");
}
#endif
