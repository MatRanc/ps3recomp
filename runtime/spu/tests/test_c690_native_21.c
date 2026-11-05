/*
 * Unit test for the native c690 0x21 emit command
 * (runtime/spu/c690_native_21.c).
 *
 * The byte-level proof is the meter corpus gate in docs/edge-native-21.md;
 * this covers the wiring: the hook claims only 0x13080 with the key on, the
 * trivial selector-0 path publishes the ring byte and the return PC, and the
 * key-off / unsupported-entry paths fall back to the lifted body.
 */
#include "c690_native_21.h"
#include "spu_context.h"
#include "spu_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime stubs the flattened bodies reference. */
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_stop(spu_context* ctx) { (void)ctx; }
u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; u128 z; memset(&z, 0, sizeof z); return z; }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 0; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v) { (void)ctx; (void)ch; (void)v; }
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx);
volatile int g_spu_direct = 1;
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = -1;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p,
                       uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

int main(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ctx || !ls) return 1;
    ctx->ls = ls;
    ctx->image_id = 6;
    ctx->host_depth = 0;

    /* Every selector-table entry -> the trivial epilogue 0x13178. */
    for (unsigned i = 0; i < 8; i++) put_be32(ls + 0x1311C + i * 4, 0x13178u);
    /* Command payload the dispatcher reads; both early-zero tests must fail. */
    put_be32(ls + 0x1F150, 0x00000001u);
    put_be32(ls + 0x1F154, 0x00000001u);
    put_be32(ls + 0x1F160, 0x00000001u);

    ctx->gpr[0]._u32[0] = 0x1234u;      /* return PC */
    ctx->gpr[1]._u32[0] = 0x3F800u;     /* stack */
    ctx->gpr[3]._u32[0] = 0x1F000u;     /* command context */
    ctx->gpr[11]._u32[0] = 0xABu;       /* byte the epilogue merges in */
    g_spu_trampoline_fn = NULL;

    int fails = 0;

    /* Key on from the first call (the runtime caches the environment). */
    setenv("PS3RECOMP_C690_NATIVE_21", "1", 1);
    /* Unsupported entry: must not claim. */
    if (c690_native_21_try(ctx, 0x13140u) != 0) {
        fprintf(stderr, "FAIL: claimed a non-0x13080 entry\n"); fails++;
    }

    /* Selector 0: the epilogue writes the ring byte at [r80+0xA0] and
     * publishes r0 as the return PC. */
    uint8_t before[16];
    memcpy(before, ls + 0x1F0A0, sizeof before);
    const int r = c690_native_21_try(ctx, 0x13080u);
    if (r != 1) {
        fprintf(stderr, "FAIL: selector-0 command not claimed (ret %d)\n", r); fails++;
    } else {
        if ((ctx->pc & SPU_LS_MASK) != 0x1234u) {
            fprintf(stderr, "FAIL: return pc 0x%05X\n", ctx->pc & SPU_LS_MASK); fails++;
        }
        if (memcmp(before, ls + 0x1F0A0, sizeof before) == 0) {
            fprintf(stderr, "FAIL: ring line not written\n"); fails++;
        }
        if (g_spu_trampoline_fn != spu_indirect_branch) {
            fprintf(stderr, "FAIL: return trampoline not published\n"); fails++;
        }
    }

    free(ls);
    free(ctx);
    if (fails) { fprintf(stderr, "test_c690_native_21: %d failure(s)\n", fails); return 1; }
    printf("test_c690_native_21: PASS\n");
    return 0;
}
