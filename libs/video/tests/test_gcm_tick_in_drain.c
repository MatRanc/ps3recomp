/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_tick_in_drain.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_tick_in_drain && /tmp/test_gcm_tick_in_drain
 *
 * PS3RECOMP_TICK_IN_DRAIN: a flip may be completed from inside a drain pass
 * only once its commands have executed (the direct/marker pending bit) and
 * its image has been queued by the present path (the presented sequence).
 * Without the present guard this is the FLIP_ON_DONE failure: the guest
 * advances and blocks on the next buffer's ref hand-off.
 *
 * PS3RECOMP_FLIP_FENCE_DONE: the image's own sequence is carried from the
 * request through the take, and the completion waits for a fence newer than
 * the one the request saw (the next frame's fence).
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
int rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_note_flip(uint32_t buffer_id) { (void)buffer_id; }
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
    s_flip_req_us = gcm_us_now();
    s_flip_direct_pending = 1;
}

int main(void)
{
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;
    setenv("PS3RECOMP_FLIP_FENCE_DONE", "0", 1);
    setenv("PS3RECOMP_FLIP_TICK_OWN_STATE", "0", 1);
    setenv("GCM_FLIPCB_ONTICK", "1", 1);

    /* The take records the pending image's request sequence, not whatever the
     * request counter has advanced to since (a ResetFlipStatus or the next
     * request). */
    request_direct();
    s_flip_pending_seq = s_flip_req_seq;
    s_flip_pending = 1;
    s_flip_req_seq += 3;                 /* a newer frame moved the counter */
    assert(cellGcm_take_flip_pending() == 1);
    assert(s_flip_presented_seq == s_flip_pending_seq);
    assert(s_flip_presented_seq != s_flip_req_seq);

    /* The same identity for a FIFO marker: the sequence queued with the
     * marker is the one the drain puts in the pending image. */
    request_direct();
    const u32 marker_req = s_flip_req_seq;
    s_fifo_flips_queued = s_fifo_flips_reached = 0;
    s_flip_marker_req_seq[s_fifo_flips_reached % GCM_FLIP_MARKER_N] = marker_req;
    s_flip_pending_seq =
        s_flip_marker_req_seq[s_fifo_flips_reached % GCM_FLIP_MARKER_N];
    s_flip_pending = 1;
    s_flip_req_seq += 2;                 /* the next frame moved the counter */
    assert(cellGcm_take_flip_pending() == 1);
    assert(s_flip_presented_seq == marker_req);

    /* Executed and presented: the checkpoint is the completion edge. */
    setenv("PS3RECOMP_TICK_IN_DRAIN", "1", 1);
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

    /* Fence-side completion (the marker-side design for an immediate flip):
     * the previous image completes only once a fence newer than its request
     * is published. */
    setenv("PS3RECOMP_FLIP_FENCE_DONE", "1", 1);
    request_direct();
    s_flip_pending_seq = s_flip_req_seq;
    s_flip_pending = 1;
    assert(cellGcm_take_flip_pending() == 1);
    s_flip_req_ref = 100;
    cellGcmFlipFenceCheckpoint(100);     /* the fence it already saw */
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();                   /* the tick must not race the fence */
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmFlipFenceCheckpoint(101);     /* the next frame's fence */
    ppu_gcm_pump();
    assert(flips == 4 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    cellGcmFlipFenceCheckpoint(102);     /* already completed: no re-fire */
    ppu_gcm_pump();
    assert(flips == 4);
    setenv("PS3RECOMP_FLIP_FENCE_DONE", "0", 1);

    /* PS3RECOMP_FLIP_TICK_OWN_STATE: the tick completes a direct flip by our
     * own request state even after SetPrepareFlip clobbered the visible
     * status to DONE. request -> prepare -> tick completes exactly once. */
    setenv("PS3RECOMP_FLIP_TICK_OWN_STATE", "1", 1);
    s_display_buffer_set[0] = 1;
    unsigned before = flips;
    request_direct();                        /* the direct request */
    assert(s_flip_direct_pending == 1);
    cellGcmSetPrepareFlip(NULL, 0);          /* the title prepares ahead */
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 1);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    assert(s_flip_direct_pending == 0);

    /* No double completion: a second tick with no request does nothing. */
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 1);

    /* A request older than the guest's first 17 ms wait is not completed
     * (the guest has left on its own fallback; this is what keeps the meter
     * a no-op). The pending request stays outstanding. */
    request_direct();
    s_flip_req_us = gcm_us_now() - 20000ull;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 1);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    assert(s_flip_direct_pending == 1);

    /* Inside the bound it completes, once. */
    s_flip_req_us = gcm_us_now() - 16999ull;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 2);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    assert(s_flip_direct_pending == 0);

    /* A flip queued in the command stream has no direct-pending bit. The
     * own-state gate must fall through to the FIFO-reached half, or the tick
     * would never complete it while the key is on. */
    request_direct();
    s_flip_direct_pending = 0;               /* this one went into the FIFO */
    s_fifo_flips_reached = s_fifo_flips_completed + 1;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 3);
    assert(s_fifo_flips_completed == s_fifo_flips_reached);
    cellGcmTickFlip();                       /* no double completion */
    ppu_gcm_pump();
    assert(flips == before + 3);

    /* Key value 2: the guest accepts a signal during its second 17 ms wait,
     * so the bound is its whole deadline (2 x 16683 us = 33366 us). */
    setenv("PS3RECOMP_FLIP_TICK_OWN_STATE", "2", 1);
    request_direct();
    s_flip_req_us = gcm_us_now() - 20000ull; /* missed by the first bound */
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 4);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    assert(s_flip_direct_pending == 0);

    /* But not after the guest's abandon point: a completion past
     * DAT_011203f8 << 1 advances the consumer the guest is no longer waiting
     * on, which is the KEEP_STATUS failure mode. */
    request_direct();
    s_flip_req_us = gcm_us_now() - 33367ull;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 4);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    assert(s_flip_direct_pending == 1);
    s_flip_req_us = gcm_us_now() - 33365ull; /* still inside the second wait */
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 5);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    assert(s_flip_direct_pending == 0);

    /* Key value 1 keeps the first-wait bound: the same 20 ms request is not
     * completed. */
    setenv("PS3RECOMP_FLIP_TICK_OWN_STATE", "1", 1);
    request_direct();
    s_flip_req_us = gcm_us_now() - 20000ull;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 5);
    assert(s_flip_direct_pending == 1);
    s_flip_direct_pending = 0;               /* drop the stale request */

    /* No request outstanding: the tick completes nothing even though a stale
     * request stamp is still around and the status is WAITING. */
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_us = gcm_us_now();
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == before + 5);
    setenv("PS3RECOMP_FLIP_TICK_OWN_STATE", "0", 1);

    puts("GCM tick-in-drain / fence checks passed");
    return 0;
}
