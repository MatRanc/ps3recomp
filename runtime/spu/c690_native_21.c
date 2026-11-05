/*
 * Native c690 0x21 emit command.  See c690_native_21.h and
 * docs/edge-native-21.md.
 *
 * c690_native_21_stage.inc is generated from the lifted c690 module by
 * tools/c690_ref/gen_native_21_stage.py.  It keeps the 128 SPU registers live
 * and turns every tail transfer into a goto, so the per-block ctx->gpr
 * marshalling disappears; the SPU arithmetic and LS helpers are unchanged.
 */
#include "c690_native_21.h"
#include "spu_context.h"

/* This TU is part of the runtime library, which does not carry the lifted
 * module's SPU float defines.  The native stage replaces lifted code, so it
 * must use the same float path (SPU_FAST_FLOAT + -mfma in this build). */
#ifndef SPU_FAST_FLOAT
#define SPU_FAST_FLOAT 1
#endif
#ifndef SPU_LIFTED_FAST_LS
#define SPU_LIFTED_FAST_LS 1
#endif
#include "spu_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(__has_include)
#  if __has_include("ps3_live_ctl.h")
#    include "ps3_live_ctl.h"
#    define C690_21_HAVE_LIVE_CTL 1
#  endif
#elif defined(__GNUC__)
#  include "ps3_live_ctl.h"
#  define C690_21_HAVE_LIVE_CTL 1
#endif

/* Runtime channel/stop entry points the flattened bodies call. */
void spu_stop(spu_context* ctx);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
void spu_indirect_branch(spu_context* ctx);

#include "c690_native_21_stage.inc"

/* ---- live key ---------------------------------------------------------- */

static int s_env_read;
static int s_env_on;

static void read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char* s = getenv("PS3RECOMP_C690_NATIVE_21");
    s_env_on = s && *s && s[0] != '0';
}

static void atexit_report(void);

int c690_native_21_enabled(void)
{
    read_env();
    if (s_env_on) {
        static int reg;
        if (!reg) { reg = 1; atexit(atexit_report); }
    }
#ifdef C690_21_HAVE_LIVE_CTL
    if (ps3_live_ctl_get) {
        const int v = ps3_live_ctl_get(PS3_LIVE_C690_NATIVE_21);
        if (ps3_live_ctl_file_set && ps3_live_ctl_file_set(PS3_LIVE_C690_NATIVE_21))
            return v != 0;
        if (v) return 1;
    }
#endif
    return s_env_on;
}

/* ---- counters ---------------------------------------------------------- */

static unsigned long long s_try, s_native, s_returned, s_stop, s_lift;
static unsigned long long s_report_ns;

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void report(const char* tag)
{
    fprintf(stderr, "[c690-native21] %s try=%llu native=%llu returned=%llu "
            "stop=%llu lift_dispatch=%llu\n",
            tag, s_try, s_native, s_returned, s_stop, s_lift);
    fflush(stderr);
}

static void atexit_report(void) { report("final"); }

static void maybe_report(void)
{
    const unsigned long long now = now_ns();
    if (!s_report_ns) { s_report_ns = now; return; }
    if (now - s_report_ns >= 2000000000ull) {
        s_report_ns = now;
        report("progress");
    }
}

/* ---- entry ------------------------------------------------------------- */

int c690_native_21_try(struct spu_context* ctx, uint32_t entry)
{
    if (!ctx || entry != 0x13080u) return 0;
    if (__builtin_expect(!c690_native_21_enabled(), 0)) return 0;
    s_try++;
    const int r = c690_native_21_stage(ctx, entry);
    if (r == 2) {
        /* The flattened body reached a target it does not model.  It has
         * already published the exact register state and pc; continue in the
         * lifted resolver. */
        s_lift++;
        spu_indirect_branch(ctx);
        s_native++;
        maybe_report();
        return 1;
    }
    if (r == 0) return 0;               /* unsupported entry: lifted body */
    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP) s_stop++;
    else s_returned++;
    s_native++;
    maybe_report();
    return 1;
}

/* ---- corpus capture ----------------------------------------------------
 *
 * The wrapper around 0x13080 calls capture_begin(); when it returns 1 the
 * lifted body runs and capture_end() writes the pair.  This is diagnostic
 * only and never used while the native key is on.
 */

#define C21_CAP_MAX 128

static int s_cap_init, s_cap_on;
static char s_cap_dir[480];
static char s_cap_trig[480];
static unsigned s_cap_n, s_cap_got;
static uint8_t* s_cap_ls;
static uint8_t* s_cap_gpr;
static uint32_t s_cap_pc_in;
static unsigned long long s_cap_t0;

static void cap_init(void)
{
    if (s_cap_init) return;
    s_cap_init = 1;
    const char* d = getenv("PS3RECOMP_C690_21_CAPTURE");
    if (!d || !*d) return;
    snprintf(s_cap_dir, sizeof s_cap_dir, "%s", d);
#if defined(_WIN32)
    _mkdir(s_cap_dir);
#else
    mkdir(s_cap_dir, 0777);
#endif
    const char* t = getenv("PS3RECOMP_C690_21_CAPTURE_TRIGGER");
    if (t && *t) snprintf(s_cap_trig, sizeof s_cap_trig, "%s", t);
    const char* n = getenv("PS3RECOMP_C690_21_CAPTURE_N");
    s_cap_n = n && *n ? (unsigned)strtoul(n, NULL, 0) : 96u;
    if (s_cap_n > C21_CAP_MAX) s_cap_n = C21_CAP_MAX;
    s_cap_ls = (uint8_t*)malloc(SPU_LS_SIZE);
    s_cap_gpr = (uint8_t*)malloc(sizeof(u128) * 128);
    if (s_cap_ls && s_cap_gpr) s_cap_on = 1;
}

int c690_native_21_capture_begin(struct spu_context* ctx, uint32_t entry)
{
    if (!s_cap_init) cap_init();
    if (!s_cap_on || s_cap_got >= s_cap_n) return 0;
    if (s_cap_trig[0]) {
        FILE* f = fopen(s_cap_trig, "rb");
        if (!f) return 0;
        fclose(f);
    }
    s_cap_got++;
    s_cap_pc_in = ctx->pc & SPU_LS_MASK;
    memcpy(s_cap_ls, ctx->ls, SPU_LS_SIZE);
    memcpy(s_cap_gpr, ctx->gpr, sizeof(u128) * 128);
    s_cap_t0 = now_ns();
    return 1;
}

void c690_native_21_capture_end(struct spu_context* ctx)
{
    if (!s_cap_on) return;
    const unsigned idx = s_cap_got ? s_cap_got - 1 : 0;
    const unsigned long long t1 = now_ns();
    char path[640];
    snprintf(path, sizeof path, "%s/p%03u_in.bin", s_cap_dir, idx);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(s_cap_ls, 1, SPU_LS_SIZE, f);
        fwrite(s_cap_gpr, 1, sizeof(u128) * 128, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/p%03u_out.bin", s_cap_dir, idx);
    f = fopen(path, "wb");
    if (f) {
        fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
        fwrite(ctx->gpr, 1, sizeof(u128) * 128, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/p%03u.txt", s_cap_dir, idx);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "entry=0x%05X pc_in=0x%05X pc_out=0x%05X time_ns=%llu "
                   "r4=%08X r6=%08X status=0x%X stop=0x%X\n",
                0x13080u, s_cap_pc_in, ctx->pc & SPU_LS_MASK, t1 - s_cap_t0,
                ((const u128*)s_cap_gpr)[4]._u32[0],
                ((const u128*)s_cap_gpr)[6]._u32[0],
                ctx->status, ctx->stop_code);
        fclose(f);
    }
    if (s_cap_got >= s_cap_n) {
        fprintf(stderr, "[c690-cap21] DONE %u pairs in %s\n", s_cap_got, s_cap_dir);
        fflush(stderr);
        s_cap_on = 0;
    }
}
