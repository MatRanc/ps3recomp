/*
 * Offline gate for the native c690 screen cull (0x25/0x26, LS 0xD080 subtree).
 *
 * Runs every captured cull pair from a PS3RECOMP_C690_CULL_PAIRS spool in one
 * of four modes (argv[1]): lift, exact (native mode 0), union (native mode 1)
 * or fix (the lifted union driver).  The driver script compares full LS/GPR
 * hashes and kept-index lists across the four runs.
 *
 * Build/run: tools/c690_ref/run_native_screen_test.sh <corpus dir>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spu_context.h"
#include "c690_native_screen.h"

/* ---- minimal replay runtime ------------------------------------------- */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;

static spu_fn lookup_fn(uint32_t addr)
{
    addr &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++)
        if (g_c690_reg[i].addr == addr) return g_c690_reg[i].fn;
    return NULL;
}

static long g_dispatch_n;
static long g_max_dispatch = 5000000;

void spu_indirect_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    spu_fn fn = lookup_fn(pc);
    if (++g_dispatch_n > g_max_dispatch) {
        fprintf(stderr, "[cull-test] dispatch budget exceeded pc=0x%05X\n", pc);
        exit(2);
    }
    if (!fn) {
        fprintf(stderr, "[cull-test] no lifted code at 0x%05X\n", pc);
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
        fprintf(stderr, "[cull-test] drain lost code ret=0x%05X pc=0x%05X\n",
                return_pc & SPU_LS_MASK, ctx->pc & SPU_LS_MASK);
        exit(3);
    }
}

void spu_img_restore(spu_context* ctx, int32_t saved_img) { ctx->image_id = saved_img; }
void spu_stop(spu_context* ctx) { (void)ctx; }

static long g_ch_ops;
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];
    if (++g_ch_ops > 2000000) { fprintf(stderr, "[cull-test] channel budget\n"); exit(4); }
    switch (channel) {
    case MFC_LSA: ctx->mfc_lsa = v; break;
    case MFC_EAH: ctx->mfc_eah = v; break;
    case MFC_EAL: ctx->mfc_eal = v; break;
    case MFC_Size: ctx->mfc_size = v; break;
    case MFC_TagID: ctx->mfc_tag = v; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v; break;
    default: break;
    }
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    u128 r; r._u32[0]=r._u32[1]=r._u32[2]=r._u32[3]=0;
    (void)ctx; (void)channel;
    return r;
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; return 0; }
void spu_fncap_capture(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
unsigned long g_fncount[65536];
void __probe_fn(unsigned addr) { g_fncount[(addr >> 2) & 0xFFFFu]++; }

/* Generated module hooks not used by the direct-body test. */
int g_c690_corpus_on = 0;
void c690_corpus_cull_enter(const spu_context* c, uint32_t l) { (void)c; (void)l; }
void c690_corpus_cull_exit(const spu_context* c, uint32_t l) { (void)c; (void)l; }
void c690_corpus_stage_enter(const spu_context* c, uint32_t l) { (void)c; (void)l; }
void c690_corpus_stage_exit(const spu_context* c, uint32_t l) { (void)c; (void)l; }
int c690_native_cull_maybe(void) { return 0; }
int c690_native_cull_enter(spu_context* c) { (void)c; return 0; }
void c690_native_cull_leave(spu_context* c) { (void)c; }
void native_spu_func_00007C18(spu_context* c) { (void)c; }

void jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0_body(spu_context* ctx);

void jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0(spu_context* ctx);

/* ---- cull spool reader ------------------------------------------------- */
#define STAGE_MAGIC "C69STG01"
#define T_STAGE_IN  5u
#define T_STAGE_OUT 6u

static uint32_t rd32(const uint8_t* p)
{
    uint32_t v; memcpy(&v, p, 4); return v;
}

static uint8_t* read_file(const char* path, size_t* out_n)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = (uint8_t*)malloc((size_t)n);
    if (!p) { fprintf(stderr, "oom\n"); exit(1); }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f);
    *out_n = (size_t)n;
    return p;
}

static uint32_t get_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t hash_bytes(uint64_t h, const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

#define LIST_MAX 0x10000u
static uint16_t g_list[LIST_MAX];

static uint32_t list_read(const uint8_t* ls, uint32_t cull_ctx,
                          uint16_t* out, uint32_t* out_n)
{
    const uint32_t ptr = get_be32(ls + cull_ctx + 0x110) & SPU_LS_MASK;
    uint32_t n = get_be32(ls + cull_ctx + 0x114);
    if (n > LIST_MAX) n = LIST_MAX;
    if (ptr + (size_t)n * 2u > SPU_LS_SIZE) n = 0;
    for (uint32_t i = 0; i < n; i++) out[i] = get_be16(ls + ptr + 2u * i);
    *out_n = n;
    return ptr;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "lift";
    const char* corpus = argc > 2 ? argv[2] :
        "/home/m/Documents/mlb16-online/recomp-work/corpus_edge3s2";
    if (argc > 3) g_max_dispatch = atol(argv[3]);

    char files[64][1200];
    int nfiles = 0;
    for (int i = 0; i < 64; i++) {
        snprintf(files[nfiles], sizeof files[0], "%s/cull_%03d.bin", corpus, i);
        FILE* f = fopen(files[nfiles], "rb");
        if (!f) break;
        fclose(f);
        nfiles++;
    }
    if (!nfiles) { fprintf(stderr, "no cull_*.bin under %s\n", corpus); return 1; }

    static spu_context ctx;
    spu_context_init(&ctx, 0);
    uint8_t* cap_ls = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* ls_in = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* gpr_in = (uint8_t*)malloc(128u * 16u);
    if (!cap_ls || !ls_in || !gpr_in) return 1;

    unsigned long long pairs = 0, bad_exit = 0, calls = 0;
    unsigned long long total_ns = 0;

    for (int fi = 0; fi < nfiles; fi++) {
        size_t n = 0;
        uint8_t* blob = read_file(files[fi], &n);
        if (n < 12 || memcmp(blob, STAGE_MAGIC, 8) != 0) {
            fprintf(stderr, "%s: bad magic\n", files[fi]);
            free(blob);
            return 1;
        }
        size_t off = 12;
        int have_in = 0;
        uint32_t cur_seq = 0;
        while (off + 24 <= n) {
            const uint32_t type = rd32(blob + off);
            const uint32_t size = rd32(blob + off + 4);
            const uint64_t ea = (uint64_t)rd32(blob + off + 8) |
                                ((uint64_t)rd32(blob + off + 12) << 32);
            off += 24;
            if (off + size > n) break;
            const uint8_t* payload = blob + off;
            off += size;
            if (type != T_STAGE_IN && type != T_STAGE_OUT) continue;
            const uint32_t ls_size = rd32(payload);
            const uint32_t gpr_size = rd32(payload + 4);
            const uint32_t seq = (uint32_t)(ea >> 32);
            if (ls_size != SPU_LS_SIZE || gpr_size != 128u * 16u) continue;
            if (type == T_STAGE_IN) {
                memcpy(ls_in, payload + 16, SPU_LS_SIZE);
                memcpy(gpr_in, payload + 16 + SPU_LS_SIZE, 128u * 16u);
                cur_seq = seq;
                have_in = 1;
                continue;
            }
            if (!have_in || seq != cur_seq) continue;
            memcpy(cap_ls, payload + 16, SPU_LS_SIZE);

            memcpy(ctx.ls, ls_in, SPU_LS_SIZE);
            memcpy(ctx.gpr, gpr_in, 128u * 16u);
            ctx.pc = 0x132E0;
            ctx.image_id = 6;
            ctx.host_depth = 0;
            g_spu_trampoline_fn = NULL;
            g_dispatch_n = 0;
            const uint32_t cull_ctx = ctx.gpr[3]._u32[0] & SPU_LS_MASK;

            const long long t0 = now_ns();
            if (!strcmp(mode, "fix") || !strcmp(mode, "union"))
                jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0(&ctx);
            else
                jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0_body(&ctx);
            const long long t1 = now_ns();
            total_ns += (unsigned long long)(t1 - t0);
            calls++;

            uint64_t hls = hash_bytes(1469598103934665603ull, ctx.ls, SPU_LS_SIZE);
            uint64_t hgpr = hash_bytes(1469598103934665603ull,
                                       (const uint8_t*)ctx.gpr, 128u * 16u);
            uint32_t ln = 0;
            const uint32_t ptr = list_read(ctx.ls, cull_ctx, g_list, &ln);

            int bad = 0;
            for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16)
                if (memcmp(ctx.ls + a, cap_ls + a, 16) != 0) { bad = 1; break; }
            if (bad) bad_exit++;

            printf("PAIR %s %llu %u 0x%05X %016llX %016llX 0x%05X %u %u %u\n",
                   mode, pairs, seq, cull_ctx, (unsigned long long)hls,
                   (unsigned long long)hgpr, ctx.pc, ptr, ln, (unsigned)bad);
            printf("LIST %s %llu", mode, pairs);
            for (uint32_t i = 0; i < ln; i++) printf(" %u", g_list[i]);
            printf("\n");
            pairs++;
            have_in = 0;
        }
        free(blob);
    }

    fprintf(stderr, "[native-screen-test] mode=%s pairs=%llu calls=%llu "
                    "capture_mismatch=%llu mean_ns=%.1f\n",
            mode, pairs, calls, bad_exit,
            calls ? (double)total_ns / (double)calls : 0.0);
    free(cap_ls); free(ls_in); free(gpr_in);
    return 0;
}
