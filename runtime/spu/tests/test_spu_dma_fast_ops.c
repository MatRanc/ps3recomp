/* The channel half of the DMA fast-path fixture (tools/test_spu_dma_fast.py).
 *
 * MFC traffic written the way lifted code issues it: constant channel numbers
 * through spu_wrch/spu_rdch, declared before spu_helpers.h. Built with
 * -DSPU_WRCH_INLINE these are the inline forms (spu_wrch_fast/spu_rdch_fast),
 * without it the out-of-line calls. Every step prints what the SPU can read
 * back and the state it left (fx_report); the driver runs the same steps on
 * the fast paths, on the full paths (SPU_DMA_SLOW=1, SPU_CHHIST) and on the
 * runtime as it was before the fast paths, and requires identical output.
 *
 * Covered: GET and PUT of every legal size class, the rejected forms (each of
 * the MFC rules, a local-store overrun, an EA past 4 GiB), barriers, GETL/PUTL
 * lists with sub-quadword elements and a stall-and-notify element resumed by
 * MFC_WrListStallAck, tag status in all three update modes with the tag event,
 * and the atomic lock-line commands against a second SPU's plain PUTs (single
 * line, a span with one reserved line, a 16 KiB span). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "spu_context.h"
u128 spu_rdch(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
#include "spu_helpers.h"

extern spu_context fx_a, fx_b;
void fx_init(void);
void fx_poke(uint32_t ea, const void* p, uint32_t n);
void fx_report(const char* label, spu_context* c);
void fx_paths(void);
void fx_release(void);

static void dma(spu_context* c, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t tag)
{
    spu_wrch(c, MFC_LSA, spu_splat_u32(lsa));
    spu_wrch(c, MFC_EAH, spu_splat_u32((uint32_t)(ea >> 32)));
    spu_wrch(c, MFC_EAL, spu_splat_u32((uint32_t)ea));
    spu_wrch(c, MFC_Size, spu_splat_u32(size));
    spu_wrch(c, MFC_TagID, spu_splat_u32(tag));
    spu_wrch(c, MFC_Cmd, spu_splat_u32(cmd));
}

static uint32_t tagstat(spu_context* c, uint32_t mask, uint32_t update)
{
    spu_wrch(c, MFC_WrTagMask, spu_splat_u32(mask));
    spu_wrch(c, MFC_WrTagUpdate, spu_splat_u32(update));
    return spu_rdch(c, MFC_RdTagStat)._u32[0];
}

static void step(const char* what, spu_context* c, uint32_t tag)
{
    char label[96];
    snprintf(label, sizeof label, "%s tag=%u", what, tag);
    const uint32_t st = tagstat(c, 1u << tag, 2);
    printf("%s rdtagstat=%08X rdtagmask=%08X\n", label, st, spu_rdch(c, MFC_RdTagMask)._u32[0]);
    fx_report(label, c);
}

static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* A list of n elements at LS `at`: {size | stall << 31, eal}. */
static void list(spu_context* c, uint32_t at, const uint32_t* sizes, const uint32_t* eas, int n, int stall_at)
{
    for (int i = 0; i < n; i++) {
        be32(c->ls + at + 8u * i, sizes[i] | (i == stall_at ? 0x80000000u : 0u));
        be32(c->ls + at + 8u * i + 4, eas[i]);
    }
}

int main(void)
{
    fx_init();
    spu_context* a = &fx_a;
    spu_context* b = &fx_b;

    /* GET and PUT: the small naturally aligned sizes and multiples of 16 up
     * to 16 KiB, LSA and EA sharing their low four bits. */
    static const uint32_t sizes[] = {1, 2, 4, 8, 16, 32, 48, 128, 256, 1024, 4096, 0x3FF0, 0x4000};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const uint32_t n = sizes[i], off = n < 16 ? (n * 3u) & 15u & ~(n - 1u) : 0;
        char w[32];
        dma(a, MFC_GET_CMD, 0x10000u + i * 0x40u + off, 0x20000u + i * 0x4440u + off, n, i);
        snprintf(w, sizeof w, "get %u", n); step(w, a, i);
        dma(a, MFC_PUT_CMD, 0x20000u + i * 0x30u + off, 0x60000u + i * 0x4410u + off, n, i + 13);
        snprintf(w, sizeof w, "put %u", n); step(w, a, i + 13);
    }
    /* The fenced/barrier forms and an upper-halfword class ID. */
    dma(a, MFC_GETF_CMD, 0x30000, 0x21000, 0x80, 3);      step("getf", a, 3);
    dma(a, MFC_PUTB_CMD, 0x30100, 0x61000, 0x80, 4);      step("putb", a, 4);
    dma(a, 0x00010040u, 0x30200, 0x21100, 0x40, 5);       step("get class", a, 5);

    /* Rejected: size 0, over 16 KiB, >= 16 and not a multiple of 16, LSA/EA
     * misaligned, a PUT into the low 64 KiB; a local-store overrun; an EA
     * range past 4 GiB. None may move data or fail to complete its tag. */
    dma(a, MFC_GET_CMD, 0x31000, 0x22000, 0, 6);          step("get size 0", a, 6);
    dma(a, MFC_GET_CMD, 0x31000, 0x22000, 0x4010, 7);     step("get 16K+16", a, 7);
    dma(a, MFC_GET_CMD, 0x31000, 0x22000, 24, 8);         step("get 24", a, 8);
    dma(a, MFC_GET_CMD, 0x31004, 0x22008, 16, 9);         step("get misaligned", a, 9);
    dma(a, MFC_PUT_CMD, 0x31000, 0x8000, 16, 10);         step("put low 64K", a, 10);
    dma(a, MFC_GET_CMD, 0x3FFF0, 0x22000, 0x20, 11);      step("get LS overrun", a, 11);
    dma(a, MFC_GET_CMD, 0x31000, 0xFFFFFF00u, 0x200, 12); step("get past 4G", a, 12);
    dma(a, MFC_PUT_CMD, 0x31000, 0xFFFFFF00u, 0x200, 12); step("put past 4G", a, 12);

    /* Barriers complete their tag. */
    dma(a, MFC_BARRIER_CMD, 0, 0, 0, 14); step("barrier", a, 14);
    dma(a, MFC_EIEIO_CMD, 0, 0, 0, 15);   step("eieio", a, 15);
    dma(a, MFC_SYNC_CMD, 0, 0, 0, 16);    step("sync", a, 16);

    /* Lists: elements of 16, 128, 4 (its EA's byte offset, a whole LS
     * quadword) and 2048 bytes; the list address goes in MFC_EAL. */
    {
        static const uint32_t ls_[] = {16, 128, 4, 2048}, ea_[] = {0x23000, 0x23100, 0x23204, 0x23800};
        list(a, 0x32000, ls_, ea_, 4, -1);
        dma(a, MFC_GETL_CMD, 0x33000, 0x32000, 4 * 8, 17); step("getl", a, 17);
        static const uint32_t pe_[] = {0x63000, 0x63100, 0x63208, 0x63800};
        list(a, 0x32100, ls_, pe_, 4, -1);
        dma(a, MFC_PUTL_CMD, 0x33000, 0x32100, 4 * 8, 18); step("putl", a, 18);
        /* Stall-and-notify after element 1: the tag stays incomplete, the SN
         * event and RdListStallStat say so, and the ack runs the rest. */
        list(a, 0x32200, ls_, ea_, 4, 1);
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0x2));
        dma(a, MFC_GETL_CMD, 0x34000, 0x32200, 4 * 8, 19); step("getl stalled", a, 19);
        /* A plain GET in the parked list's tag group completes, the group not. */
        dma(a, MFC_GET_CMD, 0x34800, 0x23400, 0x40, 19);  step("get behind stall", a, 19);
        const uint32_t ev = spu_rdch(a, SPU_RdEventStat)._u32[0];
        const uint32_t st1 = spu_rdch(a, MFC_RdListStallStat)._u32[0];
        const uint32_t st2 = spu_rdch(a, MFC_RdListStallStat)._u32[0];
        printf("stall: rdevent=%08X liststall=%08X liststall2=%08X\n", ev, st1, st2);
        spu_wrch(a, SPU_WrEventAck, spu_splat_u32(0x2));
        spu_wrch(a, MFC_WrListStallAck, spu_splat_u32(19)); step("getl resumed", a, 19);
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0));
    }

    /* Tag status: immediate, any and all over a mask with an incomplete tag
     * (the stall above, re-parked), and the tag-update event. */
    {
        static const uint32_t ls_[] = {16, 16}, ea_[] = {0x24000, 0x24010};
        list(a, 0x32300, ls_, ea_, 2, 0);
        dma(a, MFC_GETL_CMD, 0x35000, 0x32300, 2 * 8, 20);
        for (uint32_t u = 0; u < 3; u++)
            printf("tagwait upd=%u -> %08X\n", u, tagstat(a, (1u << 20) | (1u << 3) | (1u << 4), u));
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0x1));
        printf("tag event: upd0 ev=%08X", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        tagstat(a, 1u << 3, 0);
        printf(" -> %08X", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        tagstat(a, 1u << 3, 1);
        printf(" upd1 -> %08X\n", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        spu_wrch(a, SPU_WrEventAck, spu_splat_u32(0x3));
        spu_wrch(a, MFC_WrListStallAck, spu_splat_u32(20));
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0));
        step("tag modes", a, 20);
    }

    /* Atomic lock line. */
    {
        const uint32_t line = 0x50000;
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0x400));
        dma(a, MFC_GETLLAR_CMD, 0x36000, line, 128, 0);
        printf("getllar: atomic=%u\n", spu_rdch(a, MFC_RdAtomicStat)._u32[0]);
        a->ls[0x36000] ^= 0xFF;
        dma(a, MFC_PUTLLC_CMD, 0x36000, line, 128, 0);
        printf("putllc clean: atomic=%u\n", spu_rdch(a, MFC_RdAtomicStat)._u32[0]);
        fx_report("putllc clean", a);
        /* B PUTs the line A holds: A loses it, takes LR, and its PUTLLC fails.
         * A's state is reported before A reads SPU_RdEventStat, whose poll
         * would find the changed line by itself: the PUT has to tell it. */
        dma(a, MFC_GETLLAR_CMD, 0x36000, line, 128, 0);
        dma(b, MFC_PUT_CMD, 0x1000, line + 0x20, 0x20, 2); step("b put into a's line", b, 2);
        fx_report("a before its poll", a);
        printf("a: ev=%08X\n", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        a->ls[0x36001] ^= 0xFF;
        dma(a, MFC_PUTLLC_CMD, 0x36000, line, 128, 0);
        printf("putllc lost: atomic=%u\n", spu_rdch(a, MFC_RdAtomicStat)._u32[0]);
        fx_report("putllc lost", a);
        spu_wrch(a, SPU_WrEventAck, spu_splat_u32(0x400));
        dma(a, MFC_PUTLLUC_CMD, 0x36000, line, 128, 0);
        printf("putlluc: atomic=%u\n", spu_rdch(a, MFC_RdAtomicStat)._u32[0]);
        fx_report("putlluc", a);
        /* B PUTs 512 bytes covering A's reserved line and three free ones. */
        dma(a, MFC_GETLLAR_CMD, 0x36000, line + 0x100, 128, 0);
        dma(b, MFC_PUT_CMD, 0x2000, line + 0x80, 0x200, 3); step("b put span", b, 3);
        fx_report("a before its poll", a);
        printf("a: ev=%08X\n", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        spu_wrch(a, SPU_WrEventAck, spu_splat_u32(0x400));
        /* ...and a 16 KiB PUT over a reserved line in its middle. */
        dma(a, MFC_GETLLAR_CMD, 0x36000, line + 0x2000, 128, 0);
        dma(b, MFC_PUT_CMD, 0x4000, line, 0x4000, 4); step("b put 16K", b, 4);
        fx_report("a before its poll", a);
        printf("a: ev=%08X\n", spu_rdch(a, SPU_RdEventStat)._u32[0]);
        spu_wrch(a, SPU_WrEventAck, spu_splat_u32(0x400));
        /* A PUT of its own reserved line keeps the reservation's owner told. */
        dma(a, MFC_GETLLAR_CMD, 0x36000, line + 0x3000, 128, 0);
        dma(a, MFC_PUT_CMD, 0x37000, line + 0x3000, 0x80, 5); step("a put own line", a, 5);
        dma(a, MFC_PUTLLC_CMD, 0x36000, line + 0x3000, 128, 0);
        printf("putllc after own put: atomic=%u\n", spu_rdch(a, MFC_RdAtomicStat)._u32[0]);
        spu_wrch(a, SPU_WrEventMask, spu_splat_u32(0));
        fx_report("atomic end", a);
        fx_report("atomic end", b);
    }

    /* The same transfer a few times (below the repeat watchdog's limit). */
    for (int i = 0; i < 40; i++) dma(a, MFC_GET_CMD, 0x38000, 0x25000, 0x100, 21);
    step("repeats", a, 21);

    fx_report("final", a);
    fx_report("final", b);
    fx_paths();
    fx_release();
    return 0;
}
