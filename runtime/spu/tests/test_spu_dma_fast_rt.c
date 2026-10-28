/* The runtime half of the DMA fast-path fixture (tools/test_spu_dma_fast.py).
 *
 * Links the real channel and MFC code (spu_channels.c, spu_dma.h) over a 1 MiB
 * synthetic guest memory, and reports the state a transfer can change: the
 * guest memory, each context's local store and MFC/channel fields. The channel
 * traffic itself comes from test_spu_dma_fast_ops.c, written the way lifted code
 * is. Only external observation and title hooks are stubbed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
uint32_t ppu_vm_size;
#include "spu_channels.c"   /* via -I: this tree's or --against's runtime/spu */

spu_context fx_a, fx_b;

static uint64_t fx_hash(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a */
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

void fx_init(void)
{
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < sizeof guest; i++) { x = x * 1664525u + 1013904223u; guest[i] = (uint8_t)(x >> 24); }
    spu_context_init(&fx_a, 0); fx_a.image_id = 5; fx_a.spu_id = 0;
    spu_context_init(&fx_b, 0); fx_b.image_id = 6; fx_b.spu_id = 1;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
        fx_a.ls[i] = (uint8_t)(i * 7u + 3u);
        fx_b.ls[i] = (uint8_t)(i * 13u + 5u);
    }
}

void fx_poke(uint32_t ea, const void* p, uint32_t n) { memcpy(guest + ea, p, n); }

/* Everything a step can leave behind, on one line. */
void fx_report(const char* label, spu_context* c)
{
    const mfc_engine* m = mfc_for(c);
    printf("%s: guest=%016llX ls=%016llX done=%08X tagstat=%08X tagmask=%08X atomic=%u ev=%08X"
           " resv=%d/%08X stallmask=%08X stallstat=%08X\n",
           label, (unsigned long long)fx_hash(guest, sizeof guest),
           (unsigned long long)fx_hash(c->ls, SPU_LS_SIZE), m->tag_completed, c->mfc_tag_status,
           c->mfc_tag_mask, c->atomic_stat, c->event_status, c->resv_valid, c->resv_ea,
           c->list_stall_mask, c->list_stall_stat);
}

/* Which paths this run took, on a "path:" line the driver checks per mode and
 * leaves out of the comparison. */
void fx_paths(void)
{
#ifndef FIXTURE_OLD_RUNTIME
    printf("path: dma_fast=%d wrch_slow=%d\n", mfc_submit_fast_ok(&fx_a, MFC_GET_CMD), g_spu_wrch_slow);
#else
    printf("path: old runtime\n");
#endif
}

void fx_release(void)
{
    spu_coh_unregister(&fx_a); spu_coh_unregister(&fx_b);
    spu_mfc_release(&fx_a); spu_mfc_release(&fx_b);
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
