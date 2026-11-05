/*
 * Unit test for the c690 0x2A GPU binding: the 32-byte A/B classifier, the
 * record-store suppression, the exact SPU fallback and the ring-PUT mapping.
 * Runs the in-tree c690_gpu_2a.c, not the lifted module.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "spu_context.h"
#include "c690_gpu_2a.h"
#include "ps3_live_ctl.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

#define R3       0x1F000u
#define OUT      0x2F000u
#define N        8u
#define A0       0x20000u
#define A1       0x21000u
#define A2       0x22000u
#define A3       0x23000u
#define RING_LSA 0x2F000u
#define RING_EA  0x11700000u
#define RING_SIZE 0x2000u

static uint8_t s_rec[N][32];

static void be32w(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void be16w(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void seed_prefix(uint8_t* ls, uint32_t base, uint32_t off, uint32_t k)
{
    for (int b = 0; b < 16; b++)
        ls[base + off + b] = (uint8_t)(0x40 + (k * 13u + (uint32_t)b) % 64u);
}

static void build_ls(spu_context* ctx, int format, uint32_t* out_offs)
{
    memset(ctx->ls, 0, SPU_LS_SIZE);
    const uint32_t r3 = R3;
    const uint32_t arr[4] = { A0, A1, A2, A3 };
    for (int i = 0; i < 4; i++) {
        be32w(ctx->ls + r3 + 0x260u + (uint32_t)i * 4u, arr[i]);
        for (uint32_t k = 0; k < N; k++) {
            out_offs[k] = format ? k * 16u : (k * 48u + 16u);
            seed_prefix(ctx->ls, arr[i], out_offs[k], k + (uint32_t)i * 7u);
        }
    }
    be32w(ctx->ls + r3 + 0xD0u, OUT);
    if (!format) {
        const uint32_t list = 0x18000u;
        be32w(ctx->ls + r3 + 0x150u, list);
        for (uint32_t k = 0; k < N; k++)
            be16w(ctx->ls + list + 2u * k, (uint16_t)out_offs[k]);
    }
    for (uint32_t k = 0; k < N * 32u; k++)
        ctx->ls[OUT + k] = (uint8_t)(0xA0u + k);
    ctx->gpr[3]._u32[0] = r3;
}

static int s_sink_calls;
static c690_gpu_2a_job s_job;
static int sink_cb(c690_gpu_2a_job* j)
{
    s_job = *j;
    s_sink_calls++;
    return 1;
}

/* The simulated 0x2A body: the cursor advances to OUT+N*32 (0xEA70 does this
 * before the pack loop), then each record gets a +0 and a +16 store.  The
 * record prefix is the source array's bytes, as the real gather produces. */
static void run_body(spu_context* ctx, const uint32_t* offs)
{
    be32w(ctx->ls + R3 + 0xD0u, OUT + N * 32u);
    for (uint32_t k = 0; k < N; k++) {
        uint8_t* rec = s_rec[k];
        memcpy(rec, ctx->ls + A0 + offs[k], 12);
        for (int b = 12; b < 16; b++) rec[b] = (uint8_t)(0x10u + k + b);
        for (int b = 16; b < 28; b++) rec[b] = (uint8_t)(0x30u + k + b);
        for (int b = 28; b < 32; b++) rec[b] = (uint8_t)(0xE0u + k + b);
        memcpy(ctx->ls + OUT + k * 32u, rec, 32);
    }
}

static void free_job(void)
{
    free(s_job.offs); free(s_job.arrays); free(s_job.seed);
    memset(&s_job, 0, sizeof s_job);
}

int main(void)
{
    setenv("PS3RECOMP_C690_GPU_2A", "1", 1);
    ps3_live_ctl_refresh_env();
    c690_gpu_2a_set_sink(sink_cb);

    /* --- format A: suppress, classify, map to the ring EA --- */
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = 6;
    uint32_t offs[N];
    build_ls(&ctx, 0, offs);
    uint8_t before[SPU_LS_SIZE];
    memcpy(before, ctx.ls, sizeof before);
    c690_gpu_2a_enter(&ctx);
    run_body(&ctx, offs);
    c690_gpu_2a_exit(&ctx);
    /* The lifted output is untouched (the binding is an overlay). */
    for (uint32_t k = 0; k < N; k++)
        CHECK(!memcmp(ctx.ls + OUT + k*32u, s_rec[k], 32));
    CHECK(s_sink_calls == 0);
    c690_gpu_2a_put(&ctx, RING_LSA, RING_EA, RING_SIZE);
    CHECK(s_sink_calls == 1);
    CHECK(s_job.count == N);
    CHECK(s_job.ea == RING_EA + (OUT - RING_LSA));
    CHECK(s_job.stride == 32);
    for (uint32_t k = 0; k < N; k++) CHECK(s_job.offs[k] == offs[k]);
    for (uint32_t k = 0; k < N; k++)
        CHECK(!memcmp(s_job.seed + k*32u, s_rec[k], 32));
    free_job();

    /* --- format B (identity) --- */
    s_sink_calls = 0;
    spu_context_init(&ctx, 0);
    ctx.image_id = 6;
    build_ls(&ctx, 1, offs);
    c690_gpu_2a_enter(&ctx);
    run_body(&ctx, offs);
    c690_gpu_2a_exit(&ctx);
    c690_gpu_2a_put(&ctx, RING_LSA, RING_EA, RING_SIZE);
    CHECK(s_sink_calls == 1);
    CHECK(s_job.count == N);
    for (uint32_t k = 0; k < N; k++) CHECK(s_job.offs[k] == k * 16u);
    free_job();

    /* --- records that no longer match the source: exact fallback --- */
    s_sink_calls = 0;
    spu_context_init(&ctx, 0);
    ctx.image_id = 6;
    build_ls(&ctx, 1, offs);
    c690_gpu_2a_enter(&ctx);
    run_body(&ctx, offs);
    ctx.ls[A0] ^= 0xFF;      /* source moved after the gather */
    c690_gpu_2a_exit(&ctx);
    for (uint32_t k = 0; k < N; k++)
        CHECK(!memcmp(ctx.ls + OUT + k*32u, s_rec[k], 32));
    c690_gpu_2a_put(&ctx, RING_LSA, RING_EA, RING_SIZE);
    CHECK(s_sink_calls == 0);

    if (s_fail) { fprintf(stderr, "%d failure(s)\n", s_fail); return 1; }
    printf("c690_gpu_2a: PASS (format A, format B, exact fallback)\n");
    return 0;
}
