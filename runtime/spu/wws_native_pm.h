/* wws_native_pm.h -- native WWS job-manager (PM) hot sections.
 *
 * Included by the generated, game-derived wwsjob PM module (LS base 0xA00,
 * image 6) through tools/c690_ref/patch_wws_pm_native.py. The native paths are
 * transliterations of the lifted SPU code with the SPU register file kept in
 * host locals instead of ctx->gpr; every helper, DMA channel write, LS access
 * and control-flow edge is the same, so the guest-visible LS and main-memory
 * effects are identical. Opt in with PS3RECOMP_WWS_NATIVE_PM=1 (default 0).
 *
 * Header-only on purpose: the runtime's sources are globbed by CMake at
 * configure time, and a new translation unit would force a reconfigure. The
 * generated PM module is the only consumer in the game build; the unit test
 * (runtime/spu/tests/run_wws_native_pm_test.sh) includes the same header.
 *
 * Claim  (lifted LS 0x23A8 -> 0x23BC -> 0x2460/0x2498/0x24C0/0x250C/0x2518/
 *        0x2534): the WWS ticket claim. GETLLAR the list line into LS 0x80,
 *        pick the lane row at LS 0xC0 + (line lane index << 4), compute the
 *        next ticket, PUTLLC the line back, retry on a failed reservation.
 *        Reads/writes LS 0x14A0/0x14B0/0x14D0 caches and LS 0xD00 job state.
 */
#pragma once
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdlib.h>
#include <string.h>

/* PS3RECOMP_WWS_NATIVE_PM: 0/off by default. Cached once per process. */
static int g_wws_native_pm_state = -1;
static int wws_native_pm_enabled(void)
{
#ifdef WWS_NATIVE_PM_TEST
    /* Unit test: the key is read per call so one process can run the lifted
     * body and the native body on the same initial state. */
    const char* e = getenv("PS3RECOMP_WWS_NATIVE_PM");
    return (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
#else
    if (g_wws_native_pm_state < 0) {
        const char* e = getenv("PS3RECOMP_WWS_NATIVE_PM");
        g_wws_native_pm_state = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return g_wws_native_pm_state;
#endif
}

/* Native replacement for wwsjob_pm_spu_func_000023A8 (the WWS ticket claim).
 * Entry registers: r3 = list line EA, r4 = 2 * job index, r6 = return link.
 * Results (as the lifted chain): r3/r4/r5, ctx->pc = r6. */
static void wws_pm_native_claim(spu_context* ctx)
{
    /* Every r[] entry read below is assigned first (r50-r54 in the entry
     * block; r55-r73 before their first use), so no bulk zeroing is needed. */
    u128 r1, r3, r4, r5;
    u128 r50, r51, r52, r53, r54, r55, r56, r57, r58, r59, r60, r61, r62, r63,
         r64, r65, r66, r67, r68, r69, r70, r71, r72, r73;
    r1 = ctx->gpr[1];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];

            r50 = spu_ila(0x80);
            r51 = spu_fsmbi(0x0);
            r52 = spu_ori(r3, 0);
            r53 = spu_fsmbi(0x3000);
            r54 = spu_a(r4, r4);
            goto L_23BC;
    L_23BC:
            spu_wrch(ctx, MFC_LSA, r50);
            r3 = spu_il(208);
            spu_wrch(ctx, MFC_EAL, r52);
            r55 = spu_il(128);
            spu_wrch(ctx, MFC_Cmd, r3);
            r3 = spu_ila(0xD00);
            r4 = spu_rdch(ctx, MFC_RdAtomicStat);
            r56 = spu_ls_read128(ctx, r50._u32[0] + 0x0);
            r4 = spu_ls_read128(ctx, r50._u32[0] + 0x30);
            r5 = spu_rotqbyi(r56, 8);
            r57 = spu_ls_read128(ctx, 0x14B0);
            r58 = spu_shli(r5, 4);
            r59 = spu_chd(r1, 0);
            r60 = spu_shli(r5, 2);
            r60 = spu_rotqby(r4, r60);
            r4 = spu_ceq(r57, r56);
            r57 = spu_ls_read128(ctx, r58._u32[0] + 0xC0);
            r61 = spu_il(8);
            r62 = spu_ls_read128(ctx, 0x14A0);
            r63 = spu_il(64);
            r64 = spu_chd(r54, 4);
            r65 = spu_il(256);
            r66 = spu_ai(r54, 2);
            r67 = spu_il(2);
            r68 = spu_rotqbyi(r4, 12);
            r4 = spu_and(r53, r57);
            r69 = spu_roti(r57, 16);
            r70 = spu_shli(r4, 3);
            r66 = spu_rotqby(r57, r66);
            r71 = spu_il(180);
            r72 = spu_ahi(r4, 1);
            r73 = spu_clgth(r66, r4);
            r60 = spu_a(r70, r60);
            r66 = spu_selb(r66, r4, r73);
            r70 = spu_andi(r60, -128);
            r73 = spu_clgth(r69, r72);
            r62 = spu_ceq(r70, r62);
            r60 = spu_andi(r60, 127);
            r57 = spu_selb(r57, r72, r53);
            if (r68._u32[0] == 0) goto L_2534;
            if (r62._u32[0] == 0) goto L_2534;
            goto L_2460;
    L_2534:
            spu_wrch(ctx, MFC_LSA, r3);
            spu_wrch(ctx, MFC_EAL, r70);
            spu_wrch(ctx, MFC_Size, r55);
            spu_wrch(ctx, MFC_TagID, r61);
            spu_wrch(ctx, MFC_Cmd, r63);
            spu_wrch(ctx, MFC_WrTagMask, r65);
            spu_wrch(ctx, MFC_WrTagUpdate, r67);
            spu_ls_write128(ctx, 0x14A0, r70);
            spu_ls_write128(ctx, 0x14B0, r56);
            r55 = spu_rdch(ctx, MFC_RdTagStat);
            goto L_2460;
    L_2460:
            r55 = spu_selb(r72, r69, r73);
            r3 = spu_ls_read128(ctx, r3._u32[0] + r60._u32[0]);
            r55 = spu_shufb(r55, r57, r59);
            r3 = spu_rotqby(r3, r60);
            r57 = spu_ls_read128(ctx, 0x14D0);
            r59 = spu_ceqhi(r3, 3);
            r60 = spu_ceqhi(r3, 4);
            r62 = spu_rotqbyi(r3, 4);
            r68 = spu_ceqhi(r3, 1);
            if (r3._u16[0] == 0) goto L_250C;
            if (r59._u16[0] != 0) goto L_2498;
            if (r60._u16[0] != 0) goto L_2518;
            if (r68._u16[0] == 0) goto L_24C0;
            r55 = spu_shufb(r66, r55, r64);
            goto L_2498;
    L_2498:
            spu_ls_write128(ctx, r58._u32[0] + 0xC0, r55);
            spu_wrch(ctx, MFC_LSA, r50);
            spu_wrch(ctx, MFC_EAL, r52);
            spu_wrch(ctx, MFC_Cmd, r71);
            r55 = spu_rdch(ctx, MFC_RdAtomicStat);
            if (r68._u16[0] == 0) goto L_23BC;
            r55 = spu_andi(r55, 1);
            if (r55._u32[0] != 0) goto L_23BC;
            spu_ls_write128(ctx, 0x14D0, r51);
            goto ret6;
    L_250C:
            spu_ls_write128(ctx, 0x14A0, r51);
            spu_ls_write128(ctx, 0x14D0, r51);
            goto ret6;
    L_2518:
            r57 = spu_fsmbi(0xF);
            r56 = spu_a(r57, r56);
            r57 = spu_fsmbi(0xF0);
            r59 = spu_rotqbyi(r62, 8);
            r56 = spu_selb(r56, r59, r57);
            spu_ls_write128(ctx, r50._u32[0] + 0x0, r56);
            goto L_2498;
    L_24C0:
            r56 = spu_ceq(r57, r62);
            r57 = spu_ila(0x14C0);
            r59 = spu_andi(r62, -16);
            if (r56._u32[0] != 0) goto L_2498;
            r56 = spu_il(16);
            spu_ls_write128(ctx, 0x14D0, r62);
            spu_wrch(ctx, MFC_LSA, r57);
            spu_wrch(ctx, MFC_EAL, r59);
            spu_wrch(ctx, MFC_Size, r56);
            spu_wrch(ctx, MFC_TagID, r61);
            spu_wrch(ctx, MFC_Cmd, r63);
            spu_wrch(ctx, MFC_WrTagMask, r65);
            spu_wrch(ctx, MFC_WrTagUpdate, r67);
            r56 = spu_rdch(ctx, MFC_RdTagStat);
            r56 = spu_ls_read128(ctx, r57._u32[0] + 0x0);
            r57 = spu_ai(r62, 14);
            r56 = spu_rotqby(r56, r57);
            if (r56._u16[0] == 0) goto L_2498;
            r3 = spu_il(0);
            goto L_250C;
ret6:
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->pc = ctx->gpr[6]._u32[0];
}
