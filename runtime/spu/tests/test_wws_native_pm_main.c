/* Differential test: lifted WWS PM ticket claim vs wws_pm_native_claim().
 *
 * The generated wwsjob PM module is compiled as the oracle; the native path in
 * runtime/spu/wws_native_pm.h is a transliteration of the same SPU chain with
 * the register file held in host locals. Both run on byte-identical initial
 * local store and main memory; after each case the test compares LS, main
 * memory and the result registers. Built and run by
 * run_wws_native_pm_test.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"

/* ---- channel / MFC model ------------------------------------------------ */
#define MEM_SIZE (1u << 20)
static uint8_t g_mem[MEM_SIZE];
static uint8_t g_mem_init[MEM_SIZE];
static uint8_t g_mem_lifted[MEM_SIZE];
static uint8_t g_mem_native[MEM_SIZE];
static uint32_t g_resv = 0xFFFFFFFFu;
static uint32_t g_atomic = 0;
static int g_fail_next_putllc = 0;
static uint32_t g_tag_status = 0;
static unsigned g_n_llar, g_n_putllc, g_n_get, g_n_tagupd;

volatile int g_spu_wrch_slow = 1;
int g_c690_stats = 0;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;

static uint32_t mem_addr(uint32_t ea) { return ea & (MEM_SIZE - 1u); }

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    const uint32_t x = value._u32[0];
    switch (channel) {
    case MFC_LSA: ctx->mfc_lsa = x; break;
    case MFC_EAH: ctx->mfc_eah = x; break;
    case MFC_EAL: ctx->mfc_eal = x; break;
    case MFC_Size: ctx->mfc_size = x; break;
    case MFC_TagID: ctx->mfc_tag = x & 0x1F; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = x; break;
    case MFC_WrTagUpdate: g_tag_status = ctx->mfc_tag_mask; g_n_tagupd++; break;
    case MFC_Cmd: {
        const uint32_t lsa = ctx->mfc_lsa & (SPU_LS_SIZE - 1u);
        const uint32_t ea = mem_addr(ctx->mfc_eal);
        switch (x) {
        case 0xD0: /* GETLLAR: 128 bytes into LS, take the reservation */
            g_n_llar++;
            memcpy(ctx->ls + lsa, g_mem + ea, 128);
            g_resv = ctx->mfc_eal;
            g_atomic = 0;
            break;
        case 0xB4: /* PUTLLC */
            g_n_putllc++;
            if (g_fail_next_putllc) {
                g_fail_next_putllc = 0;
                g_atomic = 1;
            } else if (g_resv == ctx->mfc_eal) {
                memcpy(g_mem + ea, ctx->ls + lsa, 128);
                g_resv = 0xFFFFFFFFu;
                g_atomic = 0;
            } else {
                g_atomic = 1;
            }
            break;
        default: { /* GET/PUT */
            if (!(x & 0x20u)) g_n_get++;
            uint32_t sz = ctx->mfc_size;
            if (sz > SPU_LS_SIZE - lsa) sz = SPU_LS_SIZE - lsa;
            if (sz > MEM_SIZE - ea) sz = MEM_SIZE - ea;
            if (x & 0x20u) memcpy(g_mem + ea, ctx->ls + lsa, sz);   /* PUT */
            else           memcpy(ctx->ls + lsa, g_mem + ea, sz);   /* GET */
            break;
        }
        }
        break;
    }
    default: break;
    }
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    (void)ctx;
    switch (channel) {
    case MFC_RdAtomicStat: return spu_make_preferred_u32(g_atomic);
    case MFC_RdTagStat:    return spu_make_preferred_u32(g_tag_status);
    default:               return spu_make_preferred_u32(0);
    }
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; return 1; }
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_drain_call(spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; }
void spu_img_restore(spu_context* ctx, int image_id) { (void)ctx; (void)image_id; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void c690_stats_on_direct(const struct spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; }
void spu_mfc_cmd(spu_context* ctx, uint32_t cmd) { (void)ctx; (void)cmd; }
void spu_mfc_tag_update(spu_context* ctx, uint32_t v) { (void)ctx; (void)v; }

/* The generated PM module (renamed claim body + wrapper). */
void wwsjob_pm_spu_func_000023A8(spu_context* ctx);

static spu_context g_ctx_a, g_ctx_b;

/* ---- fixture ------------------------------------------------------------ */
static void make_mem(uint32_t seed)
{
    uint32_t x = seed * 2246822519u + 7u;
    for (uint32_t i = 0; i < MEM_SIZE; i++) { x = x * 1664525u + 1013904223u; g_mem[i] = (uint8_t)(x >> 24); }

    /* The list line at EA 0x20000. word2 selects the lane row, +0x30 is the
     * row control word and +0x40 the first lane's cursor row. */
    const uint32_t ea = 0x20000;
    memset(g_mem + ea, 0, 128);
    uint32_t *l = (uint32_t*)(g_mem + ea);
    l[2] = 0;                 /* lane row selector -> LS 0xC0 */
    l[12] = 0x00000004;       /* row control word */
    l[16] = 0x00000000;       /* cursor row */
    l[17] = 0x11111111;
    memcpy(g_mem_init, g_mem, MEM_SIZE);
}

static void make_state(spu_context* c, uint32_t seed, int tag_path, int early_out)
{
    memset(c, 0, sizeof *c);
    spu_context_init(c, 0);
    c->image_id = 6;
    c->policy_mode = 1;

    uint32_t x = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) { x = x * 1664525u + 1013904223u; c->ls[i] = (uint8_t)(x >> 24); }

    memcpy(c->ls + 0x80, g_mem_init + 0x20000, 128);
    memcpy(c->ls + 0x14B0, c->ls + 0x80, 16);
    u128 line = spu_ls_read128(c, 0x80);
    u128 lane = spu_rotqbyi(line, 8);
    u128 rowoff = spu_shli(lane, 4);
    u128 row = spu_ls_read128(c, rowoff._u32[0] + 0xC0);
    u128 ctrl = spu_ls_read128(c, 0xB0);
    u128 coloff = spu_rotqby(ctrl, spu_shli(lane, 2));
    u128 row_base = spu_andi(spu_a(spu_shli(spu_and(spu_fsmbi(0x3000), row), 3), coloff), -128);
    if (tag_path) row_base = spu_il(0x1234);
    spu_ls_write128(c, 0x14A0, row_base);
    spu_ls_write128(c, 0x14D0, spu_il(0));
    spu_ls_write128(c, 0xD00, spu_il(early_out ? 0 : 0x00010001));

    c->gpr[1] = spu_il(0x3FF0);
    c->gpr[3] = spu_il(0x20000);
    c->gpr[4] = spu_il(2);
    c->gpr[6] = spu_il(0x3030);
}

static void reset_mfc(int fail_once)
{
    g_resv = 0xFFFFFFFFu;
    g_atomic = 0;
    g_tag_status = 0;
    g_fail_next_putllc = fail_once;
}

static int run_case(const char* name, uint32_t seed, int tag_path, int early_out,
                    int fail_once)
{
    make_mem(seed);
    make_state(&g_ctx_a, seed, tag_path, early_out);
    make_state(&g_ctx_b, seed, tag_path, early_out);
    int fail = 0;

    memcpy(g_mem, g_mem_init, MEM_SIZE);
    reset_mfc(0);
    g_n_llar = g_n_putllc = g_n_get = g_n_tagupd = 0;
    setenv("PS3RECOMP_WWS_NATIVE_PM", "0", 1);
    g_spu_trampoline_fn = NULL;
    wwsjob_pm_spu_func_000023A8(&g_ctx_a);
    /* The non-clang SPU_DIRECT path returns through g_spu_trampoline_fn; the
     * runtime's drain loop follows it. Do the same here. */
    while (g_spu_trampoline_fn) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        fn(&g_ctx_a);
    }
    memcpy(g_mem_lifted, g_mem, MEM_SIZE);

    memcpy(g_mem, g_mem_init, MEM_SIZE);
    reset_mfc(fail_once);
    g_n_llar = g_n_putllc = g_n_get = g_n_tagupd = 0;
    setenv("PS3RECOMP_WWS_NATIVE_PM", "1", 1);
    g_spu_trampoline_fn = NULL;
    wwsjob_pm_spu_func_000023A8(&g_ctx_b);
    memcpy(g_mem_native, g_mem, MEM_SIZE);

    if (memcmp(g_ctx_a.ls, g_ctx_b.ls, SPU_LS_SIZE) != 0) {
        uint32_t i = 0;
        while (i < SPU_LS_SIZE && g_ctx_a.ls[i] == g_ctx_b.ls[i]) i++;
        printf("  LS differs at 0x%05X: lifted %02X native %02X\n",
               i, g_ctx_a.ls[i], g_ctx_b.ls[i]);
        fail = 1;
    }
    if (memcmp(g_mem_lifted, g_mem_native, MEM_SIZE) != 0) {
        uint32_t i = 0;
        while (i < MEM_SIZE && g_mem_lifted[i] == g_mem_native[i]) i++;
        printf("  main memory differs at 0x%05X: lifted %02X native %02X\n",
               i, g_mem_lifted[i], g_mem_native[i]);
        fail = 1;
    }
    for (int r = 3; r <= 5; r++)
        if (memcmp(&g_ctx_a.gpr[r], &g_ctx_b.gpr[r], 16) != 0) {
            printf("  gpr%d differs\n", r);
            fail = 1;
        }
    if (g_n_llar == 0) { printf("  no GETLLAR in this case (vacuous)\n"); fail = 1; }
    if (g_n_putllc == 0 && !early_out && !tag_path) { printf("  no PUTLLC in this case (vacuous)\n"); fail = 1; }
    printf("%s %s (pc lifted 0x%05X native 0x%05X; llar=%u putllc=%u get=%u tagupd=%u)\n",
           fail ? "FAIL" : "PASS", name, g_ctx_a.pc, g_ctx_b.pc,
           g_n_llar, g_n_putllc, g_n_get, g_n_tagupd);
    return fail;
}

static double bench_claim(int native, int iters)
{
    struct timespec t0, t1;
    setenv("PS3RECOMP_WWS_NATIVE_PM", native ? "1" : "0", 1);
    make_mem(11);
    make_state(&g_ctx_a, 11, 0, 0);
    memcpy(g_mem, g_mem_init, MEM_SIZE);
    reset_mfc(0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        g_spu_trampoline_fn = NULL;
        wwsjob_pm_spu_func_000023A8(&g_ctx_a);
        while (g_spu_trampoline_fn) {
            void (*fn)(spu_context*) = g_spu_trampoline_fn;
            g_spu_trampoline_fn = NULL;
            fn(&g_ctx_a);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
            (double)(t1.tv_nsec - t0.tv_nsec)) / (double)iters;
}

int main(void)
{
    int fail = 0;
    fail |= run_case("claim-base",         1, 0, 0, 0);
    fail |= run_case("claim-index1",       2, 0, 0, 0);
    fail |= run_case("claim-tag-path",     3, 1, 0, 0);
    fail |= run_case("claim-early-out",    4, 0, 1, 0);
    fail |= run_case("claim-putllc-retry", 5, 0, 0, 1);
    if (!fail) {
        const int n = 200000;
        double lifted = bench_claim(0, n);
        double native = bench_claim(1, n);
        printf("claim cost: lifted %.1f ns, native %.1f ns, %.2fx\n",
               lifted, native, lifted / native);
    }
    printf("wws native PM claim: %s\n", fail ? "FAILED" : "all cases match");
    return fail ? 1 : 0;
}
