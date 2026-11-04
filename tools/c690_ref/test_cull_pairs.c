/*
 * Offline gate for the 0x25/0x26 cull union fix.
 *
 * Runs every captured cull pair from a PS3RECOMP_C690_CULL_PAIRS spool:
 *   1. the lifted body unpatched, checked bit-exactly against the capture;
 *   2. the union driver (patched run + original run + append).
 * It then requires the union's kept-index list to contain every original
 * triangle (removed == 0) and reports the added triangles.
 *
 * Build/run: tools/c690_ref/run_cull_pair_test.sh <corpus dir>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "c690_cull_fix.h"

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

#define LIST_MAX 0x10000u
typedef struct { uint16_t v[LIST_MAX]; } tri_list;

static uint32_t get_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void list_read(const uint8_t* ls, uint32_t cull_ctx, tri_list* out,
                      uint32_t* out_n)
{
    const uint32_t ptr = get_be32(ls + cull_ctx + 0x110) & SPU_LS_MASK;
    uint32_t n = get_be32(ls + cull_ctx + 0x114);
    if (n > LIST_MAX) n = LIST_MAX;
    if (ptr + (size_t)n * 2u > SPU_LS_SIZE) n = 0;
    for (uint32_t i = 0; i < n; i++) out->v[i] = get_be16(ls + ptr + 2u * i);
    *out_n = n;
}

static int tri_eq(uint16_t a, uint16_t b, uint16_t c,
                  uint16_t x, uint16_t y, uint16_t z)
{
    return a == x && b == y && c == z;
}

static int list_has(const tri_list* l, uint32_t n, uint16_t a, uint16_t b, uint16_t c)
{
    for (uint32_t i = 0; i + 2 < n; i += 3)
        if (tri_eq(l->v[i], l->v[i+1], l->v[i+2], a, b, c)) return 1;
    return 0;
}

static void canon(uint16_t* a, uint16_t* b, uint16_t* c)
{
    uint16_t t;
    if (*a > *b) { t = *a; *a = *b; *b = t; }
    if (*b > *c) { t = *b; *b = *c; *c = t; }
    if (*a > *b) { t = *a; *a = *b; *b = t; }
}

static int list_has_canon(const tri_list* l, uint32_t n, uint16_t a, uint16_t b, uint16_t c)
{
    canon(&a, &b, &c);
    for (uint32_t i = 0; i + 2 < n; i += 3) {
        uint16_t x = l->v[i], y = l->v[i+1], z = l->v[i+2];
        canon(&x, &y, &z);
        if (x == a && y == b && z == c) return 1;
    }
    return 0;
}

typedef struct {
    unsigned long long pairs;
    unsigned long long baseline_bad;
    unsigned long long inert;
    unsigned long long removed;
    unsigned long long added_tris;
    unsigned long long original_tris;
    unsigned long long ls_leak;
    unsigned long long max_added;
} stats;

static int ls_diff_outside_list(const uint8_t* base, const uint8_t* uni,
                                uint32_t ptr, uint32_t count, uint32_t count_addr)
{
    const uint32_t end = ptr + count * 2u;
    for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16) {
        if (!memcmp(base + a, uni + a, 16)) continue;
        if (a >= ptr && a < end) continue;
        if (a <= count_addr && count_addr < a + 16) continue;
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    const char* corpus = argc > 1 ? argv[1] : "/home/m/Documents/mlb16-online/recomp-work/corpus_edge3s2";
    if (argc > 2) g_max_dispatch = atol(argv[2]);
    setenv("PS3RECOMP_C690_CULL_FIX", "1", 1);

    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s/cull_", corpus);
    /* Expand the five cull_*.bin files without shelling out. */
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
    tri_list* base_list = (tri_list*)malloc(sizeof(tri_list));
    tri_list* uni_list = (tri_list*)malloc(sizeof(tri_list));
    if (!cap_ls || !ls_in || !gpr_in || !base_list || !uni_list) return 1;

    stats st; memset(&st, 0, sizeof st);

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
        uint32_t cur_seq = 0, cur_ctx = 0;
        while (off + 24 <= n) {
            const uint32_t type = rd32(blob + off);
            const uint32_t size = rd32(blob + off + 4);
            const uint64_t ea = (uint64_t)rd32(blob + off + 8) |
                                ((uint64_t)rd32(blob + off + 12) << 32);
            const uint32_t lsa = rd32(blob + off + 16);
            off += 24;
            if (off + size > n) break;
            const uint8_t* payload = blob + off;
            off += size;
            (void)lsa;
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

            /* 1. unpatched body must reproduce the capture exactly. */
            memcpy(ctx.ls, ls_in, SPU_LS_SIZE);
            memcpy(ctx.gpr, gpr_in, 128u * 16u);
            ctx.pc = 0x132E0;
            ctx.image_id = 6;
            ctx.host_depth = 0;
            g_spu_trampoline_fn = NULL;
            g_dispatch_n = 0;
            cur_ctx = ctx.gpr[3]._u32[0] & SPU_LS_MASK;
            jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0_body(&ctx);

            int bad = 0;
            for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16)
                if (memcmp(ctx.ls + a, cap_ls + a, 16) != 0) { bad = 1; break; }
            if (bad) st.baseline_bad++;

            uint32_t base_n = 0;
            list_read(ctx.ls, cur_ctx, base_list, &base_n);

            /* 2. union driver on a fresh copy of the same input. */
            memcpy(ctx.ls, ls_in, SPU_LS_SIZE);
            memcpy(ctx.gpr, gpr_in, 128u * 16u);
            ctx.pc = 0x132E0;
            ctx.image_id = 6;
            ctx.host_depth = 0;
            g_spu_trampoline_fn = NULL;
            g_dispatch_n = 0;

            const int mode = c690_cull_fix_begin(&ctx, 1);
            if (mode == 1) {
                jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0_body(&ctx);
                c690_cull_fix_mid(&ctx);
                jobmod_c690c2f7b09a_eF5B8_spu_func_000132E0_body(&ctx);
                c690_cull_fix_end(&ctx);
            } else {
                st.inert++;
                memcpy(uni_list, base_list, sizeof(*uni_list));
            }

            uint32_t uni_n = 0;
            if (mode == 1) list_read(ctx.ls, cur_ctx, uni_list, &uni_n);
            else uni_n = base_n;

            const char* dump = getenv("CULL_TEST_DUMP");
            if (dump && (uint32_t)atol(dump) == seq) {
                printf("DUMP seq=%u mode=%d ctx=0x%05X base_n=%u uni_n=%u\n",
                       seq, mode, cur_ctx, base_n, uni_n);
                printf("  base:");
                for (uint32_t i = 0; i < base_n; i++) printf(" %u", base_list->v[i]);
                printf("\n  uni :");
                for (uint32_t i = 0; i < uni_n; i++) printf(" %u", uni_list->v[i]);
                printf("\n");
            }

            /* removed must be zero: every original exact triple survives. */
            unsigned removed = 0, orig_tris = 0;
            for (uint32_t i = 0; i + 2 < base_n; i += 3) {
                orig_tris++;
                if (!list_has(uni_list, uni_n, base_list->v[i],
                              base_list->v[i+1], base_list->v[i+2]))
                    removed++;
            }
            /* added: canonical triangles in the union that the game lacks. */
            unsigned added = 0;
            for (uint32_t i = 0; i + 2 < uni_n; i += 3) {
                if (!list_has_canon(base_list, base_n, uni_list->v[i],
                                    uni_list->v[i+1], uni_list->v[i+2]))
                    added++;
            }
            const uint32_t ptr = get_be32(ctx.ls + cur_ctx + 0x110) & SPU_LS_MASK;
            if (mode == 1 && ls_diff_outside_list(ctx.ls, cap_ls, ptr, uni_n,
                                     cur_ctx + 0x114)) st.ls_leak++;

            st.pairs++;
            st.removed += removed;
            st.added_tris += added;
            st.original_tris += orig_tris;
            if (added > st.max_added) st.max_added = added;
            if (bad || removed || added || getenv("CULL_TEST_VERBOSE"))
                printf("pair %s/%u ctx=0x%05X bad=%d orig_tris=%u added=%u removed=%u\n",
                       files[fi], seq, cur_ctx, bad, orig_tris, added, removed);

            have_in = 0;
        }
        free(blob);
    }

    printf("\n=== cull union gate ===\n");
    printf("pairs                 : %llu\n", st.pairs);
    printf("baseline LS mismatches : %llu\n", st.baseline_bad);
    printf("inert pairs            : %llu\n", st.inert);
    printf("removed triangles      : %llu (must be 0)\n", st.removed);
    printf("original triangles     : %llu\n", st.original_tris);
    printf("added triangles        : %llu\n", st.added_tris);
    printf("max added per pair     : %llu\n", st.max_added);
    printf("LS changes outside list: %llu\n", st.ls_leak);

    int fail = st.baseline_bad || st.removed || st.ls_leak;
    free(cap_ls); free(ls_in); free(gpr_in); free(base_list); free(uni_list);
    return fail ? 1 : 0;
}
