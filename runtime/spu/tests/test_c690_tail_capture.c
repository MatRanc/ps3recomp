/*
 * Unit driver for the c690 tail-stage capture hook (c690_corpus.c).
 *
 * The Python test tools/test_c690_tail_capture.py builds this with the real
 * c690_corpus.c, runs it under a temporary PS3RECOMP_C690_TAIL_CAPTURE
 * directory, and parses the spool. It simulates the command-boundary calls
 * c690_stats makes: handler entry, the 0x21 sub-selector entry, the next
 * command handler / job end.
 *
 * Usage: test_c690_tail_capture <mode>
 *   basic  one 0x21 (sel 1), one 0x25 and one 0x2A; each writes a distinct LS
 *          line so the IN/OUT diff bitmap can be checked.
 *   keys   two 0x21 commands with different selectors, back to back, to check
 *          the per-format-key cap.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "spu_context.h"
#include "c690_corpus.h"

uint8_t* vm_base = NULL;

static void fill_ctx(spu_context* ctx, uint32_t pc)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->ls = ctx->ls_store;          /* spu_context_init does this */
    ctx->image_id = 6;
    ctx->spu_id = 2;
    ctx->pc = pc;
    memset(ctx->ls, 0x11, SPU_LS_SIZE);
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 4; j++)
            ctx->gpr[i]._u32[j] = 0x10000000u + (uint32_t)i * 16u + (uint32_t)j;
    ctx->gpr[3]._u32[0] = 0x1F000u;   /* command payload base */
}

static void write_line(spu_context* ctx, uint32_t line, uint8_t v)
{
    memset(ctx->ls + line * 16u, v, 16);
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "basic";
    static spu_context ctx;

    if (!strcmp(mode, "keys")) {
        fill_ctx(&ctx, 0x13080);
        c690_corpus_tail_job_begin(&ctx);
        /* 0x21 selector 1 */
        c690_corpus_tail_trace(&ctx, 0x13080, 0x21, 1000);
        ctx.pc = 0x13140;
        ctx.gpr[6]._u32[0] = 1;
        c690_corpus_tail_trace(&ctx, 0x13140, -1, 1100);
        write_line(&ctx, 0x1F75, 0x22);
        /* next 0x21 selector 7 closes the first and opens the second */
        ctx.pc = 0x13080;
        c690_corpus_tail_trace(&ctx, 0x13080, 0x21, 2000);
        ctx.pc = 0x131A0;
        ctx.gpr[6]._u32[0] = 7;
        c690_corpus_tail_trace(&ctx, 0x131A0, -1, 2100);
        write_line(&ctx, 0x2AF5, 0x33);
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 3000);   /* job end */
        c690_corpus_tail_job_begin(&ctx);
    } else {
        fill_ctx(&ctx, 0x13080);
        c690_corpus_tail_job_begin(&ctx);
        /* 0x21 selector 1, A730 path (r6 bit 4 clear) */
        c690_corpus_tail_trace(&ctx, 0x13080, 0x21, 1000);
        ctx.pc = 0x13140;
        ctx.gpr[6]._u32[0] = 1;
        c690_corpus_tail_trace(&ctx, 0x13140, -1, 1100);
        write_line(&ctx, 0x1F75, 0x22);
        /* next command 0x25 closes 0x21 and opens the cull */
        ctx.pc = 0x13520;
        c690_corpus_tail_trace(&ctx, 0x13520, 0x25, 2000);
        write_line(&ctx, 0x204C, 0x33);
        /* next command 0x2A closes the cull and opens the assembler */
        ctx.pc = 0x10F78;
        c690_corpus_tail_trace(&ctx, 0x10F78, 0x2A, 3000);
        write_line(&ctx, 0x2F87, 0x44);
        /* job end closes the assembler */
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 4000);
        c690_corpus_tail_job_begin(&ctx);
    }

    printf("tail_on=%d\n", g_c690_tail_on);
    return 0;
}
