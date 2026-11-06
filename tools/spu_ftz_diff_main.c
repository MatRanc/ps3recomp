/* Offline differential harness for the PS3RECOMP_SPU_FTZ twin (SPU codegen v3).
 *
 * One executable links ONE generated module (the base or its `_ftz` twin) and
 * replays a captured entry state; the Python driver (tools/spu_ftz_diff.py)
 * builds the two executables and compares their final 256 KiB local store and
 * all 128 GPRs bit for bit. `SPU_DIFF_REG_FN` names the module's registration
 * function; `SPU_DIFF_FTZ=1` sets MXCSR FTZ|DAZ for the run, which is what the
 * runtime does when the live key is on.
 *
 * Stubs cover the c690 native/capture hooks (all default off; the harness runs
 * the lifted code) and the few runtime symbols a lifted module references.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#include "spu_context.h"
#include "spu_helpers.h"

#ifndef SPU_DIFF_REG_FN
#error "compile with -DSPU_DIFF_REG_FN=<module>_spu_recomp_register"
#endif
extern void SPU_DIFF_REG_FN(void);

volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;
int g_spu_ls_watch_n = 0;
int g_c690_stats = 0;
int g_c690_corpus_on = 0;
int g_c690_tail_on = 0;

void c690_stats_on_direct(const struct spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; }
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)a; (void)w; (void)p; (void)pc; (void)lr; }
unsigned* spu_ls_watch_list(int* n) { *n = 0; return NULL; }
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_img_restore(spu_context* ctx, int32_t saved) { ctx->image_id = saved; }
void spu_fncap_capture(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }

/* c690 native / corpus hooks: all off in this harness. */
int c690_cull_bypass_mode(void) { return 0; }
int c690_cull_fix_maybe(void) { return 0; }
int c690_cull_fix_begin(spu_context* c, int f) { (void)c; (void)f; return 0; }
void c690_cull_fix_mid(spu_context* c) { (void)c; }
void c690_cull_fix_end(spu_context* c) { (void)c; }
void c690_cull_fix_leave(spu_context* c) { (void)c; }
int c690_native_cull_maybe(void) { return 0; }
void c690_native_cull_enter(spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_native_cull_leave(spu_context* c) { (void)c; }
int c690_native_screen_try(spu_context* c, uint32_t p, void (*f)(spu_context*))
{ (void)c; (void)p; (void)f; return 0; }
int c690_native_21_try(spu_context* c, uint32_t p) { (void)c; (void)p; return 0; }
int c690_native_2a_try(spu_context* c, uint32_t p) { (void)c; (void)p; return 0; }
void c690_native_21_capture_begin(spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_native_21_capture_end(spu_context* c) { (void)c; }
void c690_corpus_cull_enter(const spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_corpus_cull_exit(const spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_corpus_stage_enter(const spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_corpus_stage_exit(const spu_context* c, uint32_t p) { (void)c; (void)p; }
void c690_corpus_tail_cmd_enter(const spu_context* c, uint32_t p, uint32_t l)
{ (void)c; (void)p; (void)l; }
void c690_corpus_tail_cmd_exit(const spu_context* c) { (void)c; }
void native_spu_func_00007C18(spu_context* ctx) { (void)ctx; }

/* Minimal indirect-dispatch service for the module's own registry. */
typedef void (*spu_fn)(spu_context*);
static spu_fn g_lookup(uint32_t a);
void spu_drain_call(spu_context* ctx, uint32_t ret)
{
    (void)ret;
    unsigned long g = 0;
    while (g_spu_trampoline_fn && g++ < 4000000) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        if (f == spu_indirect_branch) { f = g_lookup(ctx->pc); if (!f) break; }
        f(ctx);
    }
}
static spu_fn g_tab[8192];
static uint32_t g_addr[8192];
static int g_n;
void spu_register_function(uint32_t addr, void (*fn)(spu_context*))
{ if (g_n < 8192) { g_addr[g_n] = addr & SPU_LS_MASK; g_tab[g_n] = fn; g_n++; } }
void spu_register_function_var(uint32_t addr, void (*fn)(spu_context*), int variant)
{ (void)variant; spu_register_function(addr, fn); }
static spu_fn g_lookup(uint32_t a)
{ a &= SPU_LS_MASK; for (int i = 0; i < g_n; i++) if (g_addr[i] == a) return g_tab[i]; return NULL; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

#define GPR_BYTES (128u * 16u)

static int rd(const char* path, uint8_t* ls, uint8_t* gpr)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fread(ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE ||
        fread(gpr, 1, GPR_BYTES, f) != GPR_BYTES) {
        fprintf(stderr, "short %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.bin|capdir> <entry> [out.bin] [maxdisp] [--hash-only]\n",
                argv[0]);
        return 2;
    }
    static spu_context ctx;
    static uint8_t in_gpr[GPR_BYTES], want_ls[SPU_LS_SIZE], want_gpr[GPR_BYTES];
    char p[1024];
    spu_context_init(&ctx, 0);
    const char* in = argv[1];
    const uint32_t entry = (uint32_t)strtoul(argv[2], NULL, 0);
    const char* outp = argc > 3 && argv[3][0] && strcmp(argv[3], "-") ? argv[3] : NULL;
    const unsigned long cap = argc > 4 ? strtoul(argv[4], NULL, 0) : 2000000;

    if (strstr(in, ".bin")) {
        if (rd(in, ctx.ls_store, in_gpr)) return 2;
    } else {
        snprintf(p, sizeof p, "%s/ls.bin", in);
        FILE* f = fopen(p, "rb");
        if (!f) { perror(p); return 2; }
        if (fread(ctx.ls_store, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE) return 2;
        fclose(f);
        snprintf(p, sizeof p, "%s/gpr.bin", in);
        f = fopen(p, "rb");
        if (!f) { perror(p); return 2; }
        if (fread(in_gpr, 1, GPR_BYTES, f) != GPR_BYTES) return 2;
        fclose(f);
    }
    memcpy(ctx.gpr, in_gpr, GPR_BYTES);
    if (outp && rd(outp, want_ls, want_gpr)) return 2;

    SPU_DIFF_REG_FN();
    spu_fn fn = g_lookup(entry);
    if (!fn) { fprintf(stderr, "no entry 0x%X\n", entry); return 2; }

    const char* ftz = getenv("SPU_DIFF_FTZ");
    if (ftz && ftz[0] == '1') _mm_setcsr(_mm_getcsr() | 0x8040u);

    ctx.image_id = 6; ctx.int_enable = 0; ctx.event_status = 0; ctx.event_mask = 0;
    ctx.steps = 1; ctx.pc = entry; ctx.status = 0; ctx.stop_code = 0;
    g_spu_trampoline_fn = NULL;

    unsigned long disp = 0, ent = 0;
    fn(&ctx); ent++;
    while (g_spu_trampoline_fn && disp < cap) {
        spu_fn f = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        disp++;
        if (f == spu_indirect_branch) { f = g_lookup(ctx.pc); if (!f) break; }
        f(&ctx); ent++;
    }

    size_t ls_diff = 0, gpr_diff = 0;
    if (outp) {
        for (size_t i = 0; i < SPU_LS_SIZE; i++) if (ctx.ls_store[i] != want_ls[i]) ls_diff++;
        for (size_t i = 0; i < GPR_BYTES; i++) if (((uint8_t*)ctx.gpr)[i] != want_gpr[i]) gpr_diff++;
    }
    printf("pc=0x%05X ent=%lu disp=%lu ls_diff=%zu gpr_diff=%zu ls_hash=%016llx gpr_hash=%016llx\n",
           ctx.pc & SPU_LS_MASK, ent, disp, ls_diff, gpr_diff,
           (unsigned long long)fnv(ctx.ls_store, SPU_LS_SIZE),
           (unsigned long long)fnv(ctx.gpr, GPR_BYTES));
    if (getenv("SPU_DIFF_WRITE")) {
        FILE* f = fopen(getenv("SPU_DIFF_WRITE"), "wb");
        if (f) { fwrite(ctx.ls_store, 1, SPU_LS_SIZE, f); fwrite(ctx.gpr, 1, GPR_BYTES, f); fclose(f); }
    }
    return (outp && (ls_diff || gpr_diff)) ? 1 : 0;
}

/* Channel/MFC stubs: the float-bound kernels do not use them, but the module
 * must link. */
#ifdef spu_rdch
#undef spu_rdch
#endif
#ifdef spu_wrch
#undef spu_wrch
#endif
u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; u128 z; memset(&z, 0, sizeof z); return z; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v) { (void)ctx; (void)ch; (void)v; }
void spu_mfc_cmd(spu_context* ctx, uint32_t cmd) { (void)ctx; (void)cmd; }
void spu_mfc_tag_update(spu_context* ctx, uint32_t v) { (void)ctx; (void)v; }
volatile int g_spu_wrch_slow = 0;
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 0; }
