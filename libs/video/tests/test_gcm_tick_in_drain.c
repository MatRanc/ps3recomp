/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_tick_in_drain.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_tick_in_drain && /tmp/test_gcm_tick_in_drain
 *
 * PS3RECOMP_TICK_IN_DRAIN: a flip may be completed from inside a drain pass
 * only once its commands have executed (the direct/marker pending bit) and
 * its image has been queued by the present path (the presented sequence).
 * Without the present guard this is the FLIP_ON_DONE failure: the guest
 * advances and blocks on the next buffer's ref hand-off.
 */
#include "../cellGcmSys.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
static unsigned flips;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == 0x400) flips++;
}

static void request_direct(void)
{
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    s_flip_direct_pending = 1;
}

int main(void)
{
    setenv("PS3RECOMP_TICK_IN_DRAIN", "1", 1);
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;

    /* Executed and presented: the checkpoint is the completion edge. */
    request_direct();
    s_flip_presented_seq = s_flip_req_seq;
    cellGcmFlipTickCheckpoint();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* Executed but NOT presented: nothing may complete, and the executed bit
     * must survive for the later present. */
    request_direct();
    s_flip_presented_seq = s_flip_req_seq - 1u;
    cellGcmFlipTickCheckpoint();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    assert(s_flip_direct_pending == 1);
    s_flip_presented_seq = s_flip_req_seq;
    cellGcmFlipTickCheckpoint();
    ppu_gcm_pump();
    assert(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* Switch off: the checkpoint is a no-op even when everything is ready. */
    setenv("PS3RECOMP_TICK_IN_DRAIN", "0", 1);
    request_direct();
    s_flip_presented_seq = s_flip_req_seq;
    cellGcmFlipTickCheckpoint();
    ppu_gcm_pump();
    assert(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    assert(s_flip_direct_pending == 1);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    puts("GCM tick-in-drain checks passed");
    return 0;
}
