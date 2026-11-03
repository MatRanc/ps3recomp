/*
 * Offline differential test for the c690 tree/cull stage reference.
 *
 * Input:  <in.bin>  = 262144 bytes of local store followed by 2048 bytes of
 *                     ctx->gpr, captured at the entry of LS 0xFDA0.
 * Output: <out.bin> = the same image captured when the lifted 0xFDA0 call
 *                     returned.
 *
 * The reference is tools/c690_ref/ref_c690_stage.c. It must be compiled with
 * the same SPU float/LS options as the game's lifted module:
 *
 *   clang -O2 -march=native -mavx2 -mfma -ffp-contract=off \
 *         -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *         -I runtime/spu -I include \
 *         -o test_ref_c690_stage test_ref_c690_stage.c
 *
 * Exit: 0 = full local store *and* all 128 GPRs bit-identical, 1 = mismatch.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"

/* Runtime symbols the SPU headers/helpers reference but the reference stage
 * never executes (no channels, no dispatch, no LS watch). */
uint8_t* vm_base;
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p,
                       uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
}
unsigned* spu_ls_watch_list(int* n) { *n = 0; return NULL; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_stop(spu_context* ctx) { (void)ctx; }

#include "ref_c690_stage.c"

#define GPR_BYTES (128u * 16u)

static int read_image(const char* path, uint8_t* ls, uint8_t* gpr)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fread(ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE ||
        fread(gpr, 1, GPR_BYTES, f) != GPR_BYTES) {
        fprintf(stderr, "%s: short image\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <stage_in.bin> <stage_out.bin>\n", argv[0]);
        return 2;
    }

    static spu_context ctx;
    static uint8_t in_gpr[GPR_BYTES], want_ls[SPU_LS_SIZE], want_gpr[GPR_BYTES];
    memset(&ctx, 0, sizeof ctx);
    ctx.ls = ctx.ls_store;

    if (read_image(argv[1], ctx.ls_store, in_gpr)) return 2;
    if (read_image(argv[2], want_ls, want_gpr)) return 2;
    memcpy(ctx.gpr, in_gpr, GPR_BYTES);

    /* Capture-time context: image 6, no pending interrupt. The captured
     * return PC is 0x103AC; the entry itself is 0xFDA0. */
    ctx.image_id = 6;
    ctx.int_enable = 0;
    ctx.event_status = 0;
    ctx.pc = 0x103AC;

    jobmod_c690c2f7b09a_eF5B8_spu_func_0000FDA0_body(&ctx);

    size_t ls_bad = 0, gpr_bad = 0;
    size_t first_bad = (size_t)-1;
    int first_reg = -1;
    for (size_t i = 0; i < SPU_LS_SIZE; i++)
        if (ctx.ls[i] != want_ls[i]) {
            if (first_bad == (size_t)-1) first_bad = i;
            ls_bad++;
        }
    for (unsigned r = 0; r < 128; r++)
        if (memcmp(&ctx.gpr[r], want_gpr + r * 16, 16) != 0) {
            if (first_reg < 0) first_reg = (int)r;
            gpr_bad++;
        }

    printf("LS bytes differ=%zu", ls_bad);
    if (ls_bad) printf(" first=0x%05zX", first_bad);
    printf("; GPRs differ=%zu", gpr_bad);
    if (gpr_bad) printf(" first=r%d", first_reg);
    printf("\n");

    if (ls_bad && first_bad != (size_t)-1) {
        size_t base = first_bad & ~(size_t)15;
        for (size_t i = base; i < base + 64 && i + 16 <= SPU_LS_SIZE; i += 16) {
            printf("  LS 0x%05zX got ", i);
            for (int b = 0; b < 16; b++) printf("%02X", ctx.ls[i + b]);
            printf(" want ");
            for (int b = 0; b < 16; b++) printf("%02X", want_ls[i + b]);
            printf("\n");
        }
    }
    return (ls_bad || gpr_bad) ? 1 : 0;
}
