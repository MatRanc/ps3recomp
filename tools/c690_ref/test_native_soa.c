/*
 * Offline gate for the native c690 normal/tangent kernels
 * (runtime/spu/c690_native_soa.c) against the lifted module they replace.
 *
 * Input: 0xFDA0 stage captures (<job>_in.bin = 256 KiB LS + 128 GPRs). 0xFDA0
 * runs as captured; 0xFA58 / 0xFCB8 / 0xFA20 have no captures of their own and
 * are driven with arguments derived from the same image (positions, index
 * list, output buffer). Linked with test_native_screen.c for the replay
 * runtime (its main is renamed away). PFX and VARIANT pick the float twin.
 *
 * Build/run: tools/c690_ref/run_native_soa_test.sh <stage dir>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>

#include "spu_context.h"
#include "c690_native_soa.h"

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define FN(lsa) CAT(PFX, spu_func_0000##lsa##_body)
void FN(FA20)(spu_context*); void FN(FA58)(spu_context*);
void FN(FCB8)(spu_context*); void FN(FDA0)(spu_context*);

/* Module hooks the screen-test runtime does not carry. */
int g_c690_stats = 0;
void c690_stats_on_direct(const spu_context* c, uint32_t lsa) { (void)c; (void)lsa; }

#define GPR_BYTES (128u * 16u)
static spu_context ctx;
static uint8_t in_ls[SPU_LS_SIZE], nat_ls[SPU_LS_SIZE];
static u128 in_gpr[128], arg_gpr[128];

static double now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void set_args(uint32_t lsa)
{
    memcpy(arg_gpr, in_gpr, GPR_BYTES);
#define W(n) arg_gpr[n]._u32[0]
    const uint32_t pos = W(3), out = W(5), ntri = W(6), ntail = W(7), idx = W(8);
    switch (lsa) {
    case 0xFA58: W(3) = pos; W(4) = out; W(5) = ntri; W(6) = ntail; W(7) = idx; arg_gpr[8] = in_gpr[9]; break;
    case 0xFCB8: W(3) = 300; W(4) = pos; break;
    case 0xFA20: W(3) = 300; W(4) = out; break;
    }
#undef W
}

static int run_one(const char* name, uint32_t lsa, void (*body)(spu_context*), int reps,
                   double* t_lift, double* t_nat)
{
    set_args(lsa);
    const uint32_t sp = arg_gpr[1]._u32[0] & SPU_LS_MASK, dead = sp - 0x60u;

    memcpy(ctx.ls_store, in_ls, SPU_LS_SIZE);
    memcpy(ctx.gpr, arg_gpr, GPR_BYTES);
    ctx.host_depth = 1;
    body(&ctx);

    memcpy(nat_ls, in_ls, SPU_LS_SIZE);
    memcpy(ctx.gpr, arg_gpr, GPR_BYTES);
    if (!c690_soa_run_ls(&ctx, nat_ls, lsa, VARIANT)) {
        printf("%s 0x%05X: native declined the layout\n", name, lsa);
        return 1;
    }
    size_t bad = 0, first = 0, changed = 0;
    for (size_t i = 0; i < SPU_LS_SIZE; i++) {
        if (i >= dead && i < sp) continue;
        if (ctx.ls_store[i] != in_ls[i]) changed++;
        if (ctx.ls_store[i] != nat_ls[i] && !bad++) first = i;
    }

    /* Timing: back-to-back arms on the same image (the LS is not restored
     * between calls; the work per call does not depend on the values). */
    double t0 = now_us();
    for (int r = 0; r < reps; r++) { memcpy(ctx.gpr, arg_gpr, GPR_BYTES); body(&ctx); }
    double t1 = now_us();
    for (int r = 0; r < reps; r++) { memcpy(ctx.gpr, arg_gpr, GPR_BYTES); c690_soa_run_ls(&ctx, ctx.ls_store, lsa, VARIANT); }
    double t2 = now_us();
    *t_lift += (t1 - t0) / reps; *t_nat += (t2 - t1) / reps;

    printf("%s 0x%05X: changed=%zu differ=%zu", name, lsa, changed, bad);
    if (bad) printf(" first=0x%05zX", first);
    printf("  lifted %.2f us native %.2f us (%.1fx)\n", (t1 - t0) / reps, (t2 - t1) / reps, (t1 - t0) / (t2 - t1));
    return bad != 0 || (changed == 0 && lsa != 0xFA20);
}

extern int g_c690_soa_force;
extern volatile int g_spu_direct;
extern SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context*);
void spu_drain_call(spu_context* ctx, uint32_t return_pc);
typedef struct { uint32_t addr; void (*fn)(spu_context*); } reg_ent_t;
extern const reg_ent_t g_c690_reg[];
extern const int g_c690_reg_n;

static void (*find_fn(uint32_t addr))(spu_context*)
{
    addr &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++)
        if (g_c690_reg[i].addr == addr) return g_c690_reg[i].fn;
    return NULL;
}

static int fn_is_loop_exit(uint32_t lsa)
{
    return lsa == 0x76B8u || lsa == 0x7998u || lsa == 0x7C18u || lsa == 0x8160u ||
           lsa == 0xA798u || lsa == 0xB268u || lsa == 0xB830u;
}

static void run_fn_exec(uint32_t lsa, void (*fn)(spu_context*), uint64_t dirty_mask)
{
    for (uint64_t m = dirty_mask; m; m &= m - 1) {
        const unsigned p = (unsigned)__builtin_ctzll(m);
        memcpy(ctx.ls_store + (size_t)p * 4096u, in_ls + (size_t)p * 4096u, 4096u);
    }
    memcpy(ctx.gpr, in_gpr, GPR_BYTES);
    ctx.host_depth = 0; ctx.status = 0; ctx.stop_code = 0;
    if (fn_is_loop_exit(lsa)) {
        g_spu_direct = 0;
        ctx.drain_ret_pc = ~0u;
        ctx.pc = lsa;
        do {
            g_spu_trampoline_fn = NULL;
            fn(&ctx);
        } while ((ctx.pc & SPU_LS_MASK) == lsa);
        g_spu_trampoline_fn = NULL;
        g_spu_direct = 1;
    } else {
        const uint32_t ret_pc = in_gpr[0]._u32[0] & SPU_LS_MASK;
        g_spu_direct = 1;
        ctx.drain_ret_pc = ret_pc;
        ctx.pc = lsa;
        g_spu_trampoline_fn = spu_indirect_branch;
        spu_drain_call(&ctx, ret_pc);
        g_spu_trampoline_fn = NULL;
    }
}

static void run_fn_once(uint32_t lsa, void (*fn)(spu_context*))
{
    run_fn_exec(lsa, fn, ~0ULL);
}

static volatile uint8_t g_copy_sink;

static int replay_fn(int argc, char** argv, uint32_t lsa)
{
    void (*fn)(spu_context*) = find_fn(lsa);
    if (!fn) { fprintf(stderr, "0x%05X not in reg table\n", lsa); return 2; }
    int fail = 0, n = 0;
    double tl = 0, tn = 0;
    size_t tot_chg = 0;
    for (int a = 0; a < argc; a++) {
        char ls_p[512], gpr_p[512];
        snprintf(ls_p, sizeof ls_p, "%s/ls.bin", argv[a]);
        snprintf(gpr_p, sizeof gpr_p, "%s/gpr.bin", argv[a]);
        FILE* fl = fopen(ls_p, "rb"); FILE* fg = fopen(gpr_p, "rb");
        if (!fl || !fg || fread(in_ls, 1, SPU_LS_SIZE, fl) != SPU_LS_SIZE ||
            fread(in_gpr, 1, GPR_BYTES, fg) != GPR_BYTES) {
            fprintf(stderr, "%s: unreadable\n", argv[a]);
            if (fl) fclose(fl);
            if (fg) fclose(fg);
            return 2;
        }
        fclose(fl); fclose(fg);
        const uint32_t sp = in_gpr[1]._u32[0] & SPU_LS_MASK;
        const uint32_t dead = fn_is_loop_exit(lsa) ? sp : (sp > 0x2000u ? sp - 0x2000u : 0);

        g_c690_soa_force = 0;
        run_fn_once(lsa, fn);
        memcpy(nat_ls, ctx.ls_store, SPU_LS_SIZE);
        u128 gpr0[128];
        memcpy(gpr0, ctx.gpr, GPR_BYTES);
        const uint32_t pc0 = ctx.pc;

        size_t changed = 0;
        for (size_t i = 0; i < SPU_LS_SIZE; i++)
            if ((i < dead || i >= sp) && nat_ls[i] != in_ls[i]) changed++;
        tot_chg += changed;

        g_c690_soa_force = 1;
        run_fn_once(lsa, fn);
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < SPU_LS_SIZE; i++)
            if ((i < dead || i >= sp) && nat_ls[i] != ctx.ls_store[i] && !bad++) first = i;
        int gpr_bad = -1;
        const int check_all_gpr = fn_is_loop_exit(lsa);
        for (int r = 0; r < 128; r++) {
            if (!check_all_gpr && r >= 8 && r < 80) continue;
            if (memcmp(&gpr0[r], &ctx.gpr[r], 16) != 0) { gpr_bad = r; break; }
        }
        if (bad || gpr_bad != -1 || (pc0 & SPU_LS_MASK) != (ctx.pc & SPU_LS_MASK)) {
            printf("fn 0x%05X %s: differ=%zu first=0x%05zX gpr=%d pc=%05X/%05X\n",
                   lsa, argv[a], bad, first, gpr_bad, pc0, ctx.pc);
            fail = 1;
        }

        uint64_t dirty_mask = 0;
        for (int p = 0; p < 64; p++) {
            const uint32_t p0 = (uint32_t)p * 4096u, p1 = p0 + 4096u;
            if ((p1 > dead && p0 < sp) || memcmp(nat_ls + p0, in_ls + p0, 4096u) != 0)
                dirty_mask |= (1ULL << p);
        }
        enum { REPS = 1000 };
        /* Warmup + copy baseline over dirty pages */
        g_c690_soa_force = 0; run_fn_once(lsa, fn);
        g_c690_soa_force = 1; run_fn_once(lsa, fn);
        memcpy(ctx.ls_store, in_ls, SPU_LS_SIZE);
        double tc0 = now_us();
        for (int r = 0; r < REPS; r++) {
            for (uint64_t m = dirty_mask; m; m &= m - 1) {
                const unsigned p = (unsigned)__builtin_ctzll(m);
                memcpy(ctx.ls_store + (size_t)p * 4096u, in_ls + (size_t)p * 4096u, 4096u);
            }
            memcpy(ctx.gpr, in_gpr, GPR_BYTES);
            g_copy_sink = ctx.ls_store[r & 255];
        }
        double tc1 = now_us();
        double t0 = now_us();
        g_c690_soa_force = 0;
        for (int r = 0; r < REPS; r++) run_fn_exec(lsa, fn, dirty_mask);
        double t1 = now_us();
        g_c690_soa_force = 1;
        for (int r = 0; r < REPS; r++) run_fn_exec(lsa, fn, dirty_mask);
        double t2 = now_us();
        double copy_us = (tc1 - tc0);
        double l_us = (t1 - t0 - copy_us) / REPS;
        double n_us = (t2 - t1 - copy_us) / REPS;
        if (l_us < 0.01) l_us = 0.01;
        if (n_us < 0.01) n_us = 0.01;
        tl += l_us; tn += n_us; n++;
    }
    g_c690_soa_force = -1;
    printf("variant %d fn 0x%05X: %d captures, avg_chg=%zuB, lifted %.2f us native %.2f us (%.2fx) %s\n",
           VARIANT, lsa, n, n ? tot_chg / n : 0, tl / n, tn / n, tl / tn, fail ? "FAIL" : "PASS");
    return fail;
}

/* Whole-command replay: a capture of the LS + GPRs at a command handler's
 * entry (0x13080, the 0x21 command) run through the lifted module with the
 * kernels off and on. Everything outside the stack below the entry frame must
 * match. Uses the screen test's dispatcher (spu_indirect_branch). */
static void run_cmd(uint32_t entry)
{
    memcpy(ctx.ls_store, in_ls, SPU_LS_SIZE);
    memcpy(ctx.gpr, in_gpr, GPR_BYTES);
    ctx.host_depth = 0; ctx.pc = entry; ctx.status = 0; ctx.stop_code = 0;
    g_spu_trampoline_fn = NULL;
    spu_indirect_branch(&ctx);
    if (g_spu_trampoline_fn == spu_indirect_branch &&
        (ctx.pc & SPU_LS_MASK) != (ctx.gpr[0]._u32[0] & SPU_LS_MASK)) {
        g_spu_trampoline_fn = NULL;
        spu_indirect_branch(&ctx);
    }
    g_spu_trampoline_fn = NULL;
}
static int replay_cmds(int argc, char** argv, uint32_t entry)
{
    int fail = 0, n = 0;
    double tl = 0, tn = 0;
    for (int a = 0; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f || fread(in_ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE || fread(in_gpr, 1, GPR_BYTES, f) != GPR_BYTES) {
            fprintf(stderr, "%s: unreadable\n", argv[a]); return 2; }
        fclose(f);
        const uint32_t sp = in_gpr[1]._u32[0] & SPU_LS_MASK;
        g_c690_soa_force = 0; run_cmd(entry);
        memcpy(nat_ls, ctx.ls_store, SPU_LS_SIZE);
        const uint32_t pc0 = ctx.pc;
        g_c690_soa_force = 1; run_cmd(entry);
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < SPU_LS_SIZE; i++)
            if ((i < sp - 0x2000u || i >= sp) && nat_ls[i] != ctx.ls_store[i] && !bad++) first = i;
        if (bad || pc0 != ctx.pc) {
            printf("cmd %s: differ=%zu first=0x%05zX pc %05X/%05X\n", argv[a], bad, first, pc0, ctx.pc);
            fail = 1;
        }
        enum { REPS = 200 };
        double t0 = now_us();
        g_c690_soa_force = 0; for (int r = 0; r < REPS; r++) run_cmd(entry);
        double t1 = now_us();
        g_c690_soa_force = 1; for (int r = 0; r < REPS; r++) run_cmd(entry);
        double t2 = now_us();
        tl += (t1 - t0) / REPS; tn += (t2 - t1) / REPS; n++;
    }
    g_c690_soa_force = -1;
    printf("variant %d command 0x%05X: %d captures, lifted %.2f us native-kernels %.2f us per command (%.2fx) %s\n",
           VARIANT, entry, n, tl / n, tn / n, tl / tn, fail ? "FAIL" : "PASS");
    return fail;
}

int main(int argc, char** argv)
{
    int fail = 0;
    double tl[4] = {0}, tn[4] = {0};
    static const struct { uint32_t lsa; void (*body)(spu_context*); } fns[4] = {
        { 0xFDA0, FN(FDA0) }, { 0xFA58, FN(FA58) }, { 0xFCB8, FN(FCB8) }, { 0xFA20, FN(FA20) } };
    memset(&ctx, 0, sizeof ctx);
    ctx.ls = ctx.ls_store;
    ctx.image_id = 6;
    if (VARIANT) _mm_setcsr(_mm_getcsr() | 0x8040u);   /* the twins run FTZ|DAZ */
    if (argc > 3 && !strcmp(argv[1], "--fn")) {
        if (VARIANT) _mm_setcsr(_mm_getcsr() | 0x8040u);
        return replay_fn(argc - 3, argv + 3, (uint32_t)strtoul(argv[2], NULL, 16));
    }
    if (argc > 2 && !strcmp(argv[1], "--cmd")) {
        if (VARIANT) _mm_setcsr(_mm_getcsr() | 0x8040u);
        return replay_cmds(argc - 3, argv + 3, (uint32_t)strtoul(argv[2], NULL, 16));
    }
    for (int a = 1; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f || fread(in_ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE || fread(in_gpr, 1, GPR_BYTES, f) != GPR_BYTES) {
            fprintf(stderr, "%s: unreadable\n", argv[a]); return 2; }
        fclose(f);
        const char* name = strrchr(argv[a], '/'); name = name ? name + 1 : argv[a];
        for (int k = 0; k < 4; k++) fail |= run_one(name, fns[k].lsa, fns[k].body, 2000, &tl[k], &tn[k]);
    }
    for (int k = 0; k < 4; k++)
        printf("variant %d 0x%05X mean: lifted %.2f us native %.2f us ratio %.1fx\n", VARIANT, fns[k].lsa,
               tl[k] / (argc - 1), tn[k] / (argc - 1), tl[k] / tn[k]);
    printf("RESULT variant %d: %s\n", VARIANT, fail ? "FAIL" : "PASS");
    return fail;
}
