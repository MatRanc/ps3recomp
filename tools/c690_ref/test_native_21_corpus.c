/*
 * Offline corpus gate for the native c690 0x21 emit command.
 *
 *   test_native_21_corpus <capture dir> [iters]
 *
 * The capture dir holds the pairs written by PS3RECOMP_C690_21_CAPTURE:
 * pNNN_in.bin is the full LS + 128 GPRs at the 0x13080 entry.  The exit half
 * of that capture was recorded before the dispatcher's pending indirect branch
 * ran, so this harness does not use it: for every input it runs the lifted
 * 0x13080 body (with the pending trampoline drained), then the native stage
 * from the same input, and compares the full 256 KiB LS, all 128 GPRs and the
 * return PC.
 *
 * Build/run: tools/c690_ref/run_native_21_test.sh <capture dir>
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "c690_native_21.h"

volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = -1;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p,
                       uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;

static long g_dispatch_n;
static long g_max_dispatch = 5000000;
static jmp_buf g_abort_jb;
static int g_abort_jmp;

static spu_fn lookup_fn(uint32_t addr)
{
    addr &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++)
        if (g_c690_reg[i].addr == addr) return g_c690_reg[i].fn;
    return NULL;
}

void spu_indirect_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    spu_fn fn = lookup_fn(pc);
    if (++g_dispatch_n > g_max_dispatch) {
        fprintf(stderr, "[gate] dispatch budget exceeded pc=0x%05X\n", pc);
        exit(2);
    }
    if (!fn) {
        fprintf(stderr, "[gate] no lifted code at 0x%05X\n", pc);
        if (g_abort_jmp) longjmp(g_abort_jb, 1);
        exit(3);
    }
    fn(ctx);
}

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch &&
                (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
                g_spu_trampoline_fn = NULL;
                return;
            }
            g_spu_trampoline_fn = NULL;
            tf(ctx);
        }
        if ((ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) return;
        spu_fn fn = lookup_fn(ctx->pc);
        if (fn) { g_spu_trampoline_fn = spu_indirect_branch; continue; }
        fprintf(stderr, "[gate] drain lost code ret=0x%05X pc=0x%05X\n",
                return_pc & SPU_LS_MASK, ctx->pc & SPU_LS_MASK);
        if (g_abort_jmp) longjmp(g_abort_jb, 1);
        exit(3);
    }
}

void spu_img_restore(spu_context* ctx, int32_t saved_img) { ctx->image_id = saved_img; }
void spu_stop(spu_context* ctx) { (void)ctx; }
u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; u128 z; memset(&z,0,sizeof z); return z; }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 0; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v) { (void)ctx; (void)ch; (void)v; }
void spu_fncap_capture(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

/* c690 corpus/cull hooks the generated module may reference. */
int g_c690_corpus_on, g_c690_tail_on;
void c690_corpus_cull_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_cull_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_tail_cmd_enter(const spu_context* c, uint32_t a, uint32_t b) { (void)c; (void)a; (void)b; }
void c690_corpus_tail_cmd_exit(const spu_context* c) { (void)c; }
void c690_corpus_stage_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_stage_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
int c690_cull_fix_maybe(void) { return 0; }
int c690_cull_fix_begin(spu_context* c, int m) { (void)c; (void)m; return 0; }
void c690_cull_fix_mid(spu_context* c) { (void)c; }
void c690_cull_fix_end(spu_context* c) { (void)c; }
void c690_cull_fix_leave(spu_context* c) { (void)c; }
int c690_cull_bypass_mode(void) { return 0; }
int c690_native_cull_maybe(void) { return 0; }
int c690_native_cull_enter(spu_context* c) { (void)c; return 0; }
void c690_native_cull_leave(spu_context* c) { (void)c; }
int c690_native_screen_try(spu_context* c, uint32_t a, void (*f)(spu_context*))
{ (void)c; (void)a; (void)f; return 0; }
int c690_native_2a_try(spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }

static void run_lifted(spu_context* ctx)
{
    spu_fn fn = lookup_fn(0x13080u);
    if (!fn) { fprintf(stderr, "[gate] no lifted 0x13080\n"); exit(3); }
    fn(ctx);
    if (g_spu_trampoline_fn == spu_indirect_branch &&
        (ctx->pc & SPU_LS_MASK) != (ctx->gpr[0]._u32[0] & SPU_LS_MASK)) {
        g_spu_trampoline_fn = NULL;
        spu_indirect_branch(ctx);
    }
    g_spu_trampoline_fn = NULL;
}

static uint32_t rd_be32(const uint8_t* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <capture dir> [iters]\n", argv[0]); return 1; }
    const char* dir = argv[1];
    const int iters = argc > 2 ? atoi(argv[2]) : 1;
    setenv("PS3RECOMP_C690_NATIVE_21", "1", 1);

    DIR* d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    unsigned char* in = malloc(SPU_LS_SIZE + 128 * 16);
    unsigned char* ref_ls = malloc(SPU_LS_SIZE);
    unsigned char* ref_gpr = malloc(128 * 16);
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.ls = malloc(SPU_LS_SIZE);
    if (!in || !ref_ls || !ref_gpr || !ctx.ls) return 1;
    ctx.image_id = 6;

    int pairs = 0, bad = 0, skipped = 0;
    double sum_lift = 0, sum_nat = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "p", 1) || strstr(de->d_name, "_in.bin") == NULL) continue;
        char path[640];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        size_t got = fread(in, 1, SPU_LS_SIZE + 128 * 16, f);
        fclose(f);
        if (got != SPU_LS_SIZE + 128 * 16) { skipped++; continue; }
        pairs++;

        double tl = 0, tn = 0;
        struct timespec a, b;
        int r = 0;
        g_abort_jmp = 1;
        if (setjmp(g_abort_jb)) { skipped++; g_abort_jmp = 0; continue; }
        for (int it = 0; it < iters; it++) {
            memcpy(ctx.ls, in, SPU_LS_SIZE);
            memcpy(ctx.gpr, in + SPU_LS_SIZE, 128 * 16);
            ctx.steps = 1; ctx.host_depth = 0; ctx.pc = 0x13080u;
            ctx.status = 0; ctx.stop_code = 0; g_spu_trampoline_fn = NULL;
            g_dispatch_n = 0;
            clock_gettime(CLOCK_MONOTONIC, &a);
            run_lifted(&ctx);
            clock_gettime(CLOCK_MONOTONIC, &b);
            tl += (b.tv_sec-a.tv_sec)*1e9 + (b.tv_nsec-a.tv_nsec);
        }
        g_abort_jmp = 0;
        memcpy(ref_ls, ctx.ls, SPU_LS_SIZE);
        memcpy(ref_gpr, ctx.gpr, 128 * 16);
        uint32_t ref_pc = ctx.pc & SPU_LS_MASK, ref_stop = ctx.stop_code, ref_st = ctx.status;

        for (int it = 0; it < iters; it++) {
            memcpy(ctx.ls, in, SPU_LS_SIZE);
            memcpy(ctx.gpr, in + SPU_LS_SIZE, 128 * 16);
            ctx.steps = 1; ctx.host_depth = 0; ctx.pc = 0x13080u;
            ctx.status = 0; ctx.stop_code = 0; g_spu_trampoline_fn = NULL;
            clock_gettime(CLOCK_MONOTONIC, &a);
            r = c690_native_21_try(&ctx, 0x13080u);
            clock_gettime(CLOCK_MONOTONIC, &b);
            tn += (b.tv_sec-a.tv_sec)*1e9 + (b.tv_nsec-a.tv_nsec);
        }
        tl /= iters; tn /= iters;

        long lsd = 0; for (unsigned i = 0; i < SPU_LS_SIZE; i++) if (ctx.ls[i] != ref_ls[i]) lsd++;
        long gd = 0; for (int i = 0; i < 128; i++) if (memcmp(&ctx.gpr[i], ref_gpr + i*16, 16)) gd++;
        int pcok = ((ctx.pc & SPU_LS_MASK) == ref_pc) && (ctx.stop_code == ref_stop) && (ctx.status == ref_st);
        if (r != 1 || lsd || gd || !pcok) bad++;
        sum_lift += tl; sum_nat += tn;
        printf("PAIR %s ret=%d ls=%ld gpr=%ld pc=%d lift_ns=%.0f native_ns=%.0f\n",
               de->d_name, r, lsd, gd, pcok, tl, tn);
    }
    closedir(d);
    printf("pairs=%d bad=%d skipped=%d mean_lift_us=%.2f mean_native_us=%.2f speedup=%.2f\n",
           pairs, bad, skipped, sum_lift / (pairs ? pairs : 1) / 1000.0,
           sum_nat / (pairs ? pairs : 1) / 1000.0,
           sum_nat > 0 ? sum_lift / sum_nat : 0.0);
    return bad ? 1 : 0;
}
