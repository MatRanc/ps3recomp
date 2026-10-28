/* The SPU runtime's per-operation cuts, checked through the real code:
 *  - a channel read returns its value in the preferred slot, other lanes 0;
 *  - the MFC parameter channels latch through spu_wrch, and GET/PUT issued
 *    that way move the right bytes in program order, including a PUT
 *    published to another thread (data, then flag);
 *  - the indirect-branch fast path resolves like the full resolver: overlay
 *    before image, job-module overlays (id >= 200) included, resident_task
 *    cleared below the task region, the pc masked.
 * tools/test_spu_runtime_cuts.py runs it with SPU_IB_FAST unset and =0. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
uint32_t ppu_vm_size = sizeof guest;
#include "../spu_channels.c"

static spu_context spu;

/* Link stubs for the units this fixture does not exercise. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi; int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{ (void)c; (void)a; (void)b; (void)d; abort(); }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }

static void wr(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, spu_make_preferred_u32(v)); }
static void dma(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    wr(c, MFC_LSA, lsa); wr(c, MFC_EAH, 0); wr(c, MFC_EAL, ea);
    wr(c, MFC_Size, size); wr(c, MFC_TagID, tag); wr(c, MFC_Cmd, cmd);
}
static void expect_preferred(u128 r, uint32_t v)
{
    assert(r._u32[0] == v && r._u32[1] == 0 && r._u32[2] == 0 && r._u32[3] == 0);
}

/* A second SPU publishing records to the main thread: data PUT, then a
 * fenced flag PUT. The reader must never see the flag before the data. */
static spu_context producer;
#define MP_ROUNDS 20000u
static void* mp_producer(void* arg)
{
    (void)arg;
    for (uint32_t i = 1; i <= MP_ROUNDS; i++) {
        for (unsigned k = 0; k < 64; k += 4) memcpy(producer.ls + 0x2000 + k, &i, 4);
        dma(&producer, 0x20, 0x2000, 0x30000, 64, 1);        /* PUT data */
        memcpy(producer.ls + 0x2080, &i, 4);
        dma(&producer, 0x22, 0x2080, 0x30080, 16, 1);        /* PUTF flag */
        while (__atomic_load_n((uint32_t*)(guest + 0x30100), __ATOMIC_ACQUIRE) != i) {}
    }
    return NULL;
}

static int hits[4];
static void f_img(spu_context* c)  { (void)c; hits[0]++; }
static void f_ovl(spu_context* c)  { (void)c; hits[1]++; }
static void f_low(spu_context* c)  { (void)c; hits[2]++; }
static void f_job(spu_context* c)  { (void)c; hits[3]++; }

int main(void)
{
    spu_context_init(&spu, 0);
    spu.image_id = 5;

    /* spu_make_preferred_u32 and channel reads. */
    const uint32_t vals[] = { 0, 1, 0x80000001u, 0xFFFFFFFFu, 0x12345678u };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        expect_preferred(spu_make_preferred_u32(vals[i]), vals[i]);
        spu.event_mask = vals[i];
        expect_preferred(spu_rdch(&spu, SPU_RdEventMask), vals[i]);
    }
    spu.event_mask = 0;

    /* Parameter latches. */
    wr(&spu, MFC_LSA, 0x1230); wr(&spu, MFC_EAH, 7); wr(&spu, MFC_EAL, 0x40000);
    wr(&spu, MFC_Size, 0x80); wr(&spu, MFC_TagID, 0x25); wr(&spu, MFC_WrTagMask, 0x22);
    assert(spu.mfc_lsa == 0x1230 && spu.mfc_eah == 7 && spu.mfc_eal == 0x40000);
    assert(spu.mfc_size == 0x80 && spu.mfc_tag == 5 && spu.mfc_tag_mask == 0x22);

    /* GET, change, PUT elsewhere, GET back: program order holds. */
    for (unsigned i = 0; i < 256; i++) guest[0x20000 + i] = (uint8_t)(i * 7 + 1);
    dma(&spu, 0x40, 0x1000, 0x20000, 256, 2);                /* GET */
    assert(!memcmp(spu.ls + 0x1000, guest + 0x20000, 256));
    for (unsigned i = 0; i < 256; i++) spu.ls[0x1000 + i] ^= 0xA5;
    dma(&spu, 0x20, 0x1000, 0x21000, 256, 2);                /* PUT */
    dma(&spu, 0x40, 0x3000, 0x21000, 256, 2);                /* GET what was PUT */
    for (unsigned i = 0; i < 256; i++)
        assert(spu.ls[0x3000 + i] == (uint8_t)((i * 7 + 1) ^ 0xA5));
    wr(&spu, MFC_WrTagMask, 1u << 2);
    wr(&spu, MFC_WrTagUpdate, 2);                            /* all */
    expect_preferred(spu_rdch(&spu, MFC_RdTagStat), 1u << 2);

    /* Message passing from another SPU thread. */
    spu_context_init(&producer, 1);
    producer.image_id = 6;
    pthread_t th;
    assert(!pthread_create(&th, NULL, mp_producer, NULL));
    for (uint32_t i = 1; i <= MP_ROUNDS; i++) {
        while (__atomic_load_n((uint32_t*)(guest + 0x30080), __ATOMIC_ACQUIRE) != i) {}
        for (unsigned k = 0; k < 64; k += 4) {
            uint32_t d; memcpy(&d, guest + 0x30000 + k, 4);
            if (d != i) { fprintf(stderr, "round %u: flag before data (%u)\n", i, d); return 1; }
        }
        __atomic_store_n((uint32_t*)(guest + 0x30100), i, __ATOMIC_RELEASE);
    }
    pthread_join(th, NULL);

    /* Indirect branches. */
    spu_begin_image(5);
    spu_register_function(0x4100, f_img);
    spu_register_function(0x1000, f_low);
    spu_begin_image(201);   /* a WWS job-module overlay */
    spu_register_function(0x4100, f_ovl);
    spu_register_function(0x4200, f_job);

    spu.resident_ovl = 201;
    spu.pc = 0x4100 | 0x40000;                               /* high bits ignored */
    spu_indirect_branch(&spu);
    assert(hits[1] == 1 && hits[0] == 0 && spu.pc == 0x4100);
    spu.pc = 0x4200;
    spu_indirect_branch(&spu);
    assert(hits[3] == 1);
    spu.resident_ovl = 0;
    spu.pc = 0x4100;
    spu_indirect_branch(&spu);
    assert(hits[0] == 1);
    spu.resident_task = 9;                                   /* below 0x3000: cleared */
    spu.pc = 0x1000;
    spu_indirect_branch(&spu);
    assert(hits[2] == 1 && spu.resident_task == 0);
    for (int i = 0; i < 100000; i++) { spu.pc = 0x4100; spu_indirect_branch(&spu); }
    assert(hits[0] == 100001);
    assert(g_spu_trampoline_fn == 0);

    spu_mfc_release(&spu);
    spu_mfc_release(&producer);
    puts("PASS: channel reads, MFC latches and ordering, indirect-branch resolution");
    return 0;
}
