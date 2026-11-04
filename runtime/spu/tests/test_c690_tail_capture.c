/*
 * Unit driver for the c690 tail-stage capture hook (c690_corpus.c).
 *
 * The Python test tools/test_c690_tail_capture.py builds this with the real
 * c690_corpus.c, runs it under a temporary PS3RECOMP_C690_TAIL_CAPTURE
 * directory, and parses the spool. It simulates the calls c690_stats and the
 * patched lifted module make: handler entry, the 0x21 sub-selector entry, the
 * next command handler / job end, and the 0x2A function entry/exit wrapper.
 *
 * Usage: test_c690_tail_capture <mode>
 *   basic  one 0x21 (sel 1), one 0x25 and one 0x2A (through its function
 *          wrapper); each writes a distinct LS line so the IN/OUT diff bitmap
 *          can be checked.
 *   keys   two 0x21 commands with different selectors, back to back, to check
 *          the per-format-key cap.
 *   exit   two 0x2A commands back to back, each through its own entry/exit
 *          wrapper, to check that one pair is one command even when the
 *          resolver never sees the second entry.
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
    ctx->gpr[0]._u32[0] = 0x10700u;   /* caller link (command parser) */
    ctx->gpr[3]._u32[0] = 0x1F000u;   /* command payload base */
}

static void write_line(spu_context* ctx, uint32_t line, uint8_t v)
{
    memset(ctx->ls + line * 16u, v, 16);
}

static void run_2a(spu_context* ctx, uint32_t line, uint8_t v, uint32_t t0)
{
    ctx->pc = 0x10F78;
    c690_corpus_tail_2a_enter(ctx);
    write_line(ctx, line, v);
    ctx->pc = ctx->gpr[0]._u32[0];    /* SPU_RET back to the parser */
    c690_corpus_tail_2a_exit(ctx);
    (void)t0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "basic";
    static spu_context ctx;

    if (!strcmp(mode, "keys")) {
        fill_ctx(&ctx, 0x13140);
        c690_corpus_tail_job_begin(&ctx);
        /* 0x21 selector 1, first sub-selector body */
        ctx.gpr[6]._u32[0] = 1;
        c690_corpus_tail_cmd_enter(&ctx, 0x13140, 0x21);
        write_line(&ctx, 0x1F75, 0x22);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_cmd_exit(&ctx);
        /* next 0x21 selector 7 body opens its own pair */
        ctx.pc = 0x131A0;
        ctx.gpr[6]._u32[0] = 7;
        c690_corpus_tail_cmd_enter(&ctx, 0x131A0, 0x21);
        write_line(&ctx, 0x2AF5, 0x33);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_cmd_exit(&ctx);
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 3000);   /* job end */
        c690_corpus_tail_job_begin(&ctx);
    } else if (!strcmp(mode, "exit")) {
        fill_ctx(&ctx, 0x13080);
        c690_corpus_tail_job_begin(&ctx);
        /* Two 0x2A commands back to back: the second entry is the first
         * command's true exit, and each pair must hold only its own write. */
        run_2a(&ctx, 0x2F87, 0x44, 1000);
        run_2a(&ctx, 0x2F88, 0x55, 2000);
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 3000);   /* job end */
        c690_corpus_tail_job_begin(&ctx);
    } else if (!strcmp(mode, "cap")) {
        fill_ctx(&ctx, 0x13140);
        c690_corpus_tail_job_begin(&ctx);
        /* Six selector-1 commands with PS3RECOMP_C690_TAIL_PAIRS=2: only the
         * first two may snapshot, but all six are still counted. */
        ctx.gpr[6]._u32[0] = 1;
        for (uint32_t i = 0; i < 6; i++) {
            ctx.pc = 0x13140;
            c690_corpus_tail_cmd_enter(&ctx, 0x13140, 0x21);
            write_line(&ctx, 0x1F75, (uint8_t)(0x20 + i));
            ctx.pc = ctx.gpr[0]._u32[0];
            c690_corpus_tail_cmd_exit(&ctx);
        }
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 6000);   /* job end */
        c690_corpus_tail_job_begin(&ctx);
    } else if (!strcmp(mode, "a730")) {
        fill_ctx(&ctx, 0x13140);
        c690_corpus_tail_job_begin(&ctx);
        /* A730 heavy-body leaf 0xB830 through its capture wrapper. */
        ctx.pc = 0xB830;
        c690_corpus_tail_cmd_enter(&ctx, 0xB830, 0xA7);
        write_line(&ctx, 0x2F83, 0x66);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_cmd_exit(&ctx);
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 5000);
        c690_corpus_tail_job_begin(&ctx);
    } else {
        fill_ctx(&ctx, 0x13140);
        c690_corpus_tail_job_begin(&ctx);
        /* 0x21 selector 1, A730 path (r6 bit 4 clear); the sub-selector
         * body wrapper opens the pair. */
        ctx.gpr[6]._u32[0] = 1;
        c690_corpus_tail_cmd_enter(&ctx, 0x13140, 0x21);
        write_line(&ctx, 0x1F75, 0x22);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_cmd_exit(&ctx);
        /* 0x25 at the cull body 0x132E0 (r4: 0 = 0x25). */
        ctx.pc = 0x132E0;
        ctx.gpr[4]._u32[0] = 0;
        c690_corpus_tail_cmd_enter(&ctx, 0x132E0, 0x25);
        write_line(&ctx, 0x204C, 0x33);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_cmd_exit(&ctx);
        /* 0x2A through its own function wrapper. */
        ctx.pc = 0x10F78;
        c690_corpus_tail_2a_enter(&ctx);
        write_line(&ctx, 0x2F87, 0x44);
        ctx.pc = ctx.gpr[0]._u32[0];
        c690_corpus_tail_2a_exit(&ctx);
        /* job end closes anything still open */
        c690_corpus_tail_trace(&ctx, 0x12AB8, 0, 4000);
        c690_corpus_tail_job_begin(&ctx);
    }

    printf("tail_on=%d\n", g_c690_tail_on);
    return 0;
}
