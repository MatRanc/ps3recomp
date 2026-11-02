/*
 * Render-target readback to guest memory ("write colour buffers", WCB).
 *
 *   sync      (PS3RECOMP_RSX_WCB=1): dirty offscreen targets are copied at a
 *             sync point; display buffers and clean targets are not.
 *   ondemand  (=2): a sync point only protects the target's guest pages. A
 *             target the guest never touches is never copied; the first touch
 *             from another thread faults, waits for the engine thread to copy
 *             it, and reads the rendered bytes -- never stale, never partial.
 *   async     (=3): a target the guest read recently gets its copy queued at
 *             the sync point; a later read collects exactly that copy (the
 *             sync point's bytes), even if the target was drawn again since.
 *
 * Guest memory is a real mmap here, so the protection and the faults are the
 * real ones, and the reads come from a second thread as a PPU or SPU's would.
 */
#define main engine_retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; return 0; }

static unsigned checks, failures;
#define T(c, msg) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---- the backend: every pixel of a readback is `s_pixel` (R,G,B,A) ------- */

static u8  s_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };
static u32 s_readback_calls, s_last_readback_surface;
static u32 s_async_issued, s_async_fetched, s_async_released;
static struct { int used; u8 px[4]; u32 w, h; } s_tickets[8];

static void fill(void* out, u32 w, u32 h, u32 pitch, const u8 px[4])
{
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) memcpy((u8*)out + (size_t)r * pitch + c * 4u, px, 4);
}

static void test_readback_hook(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{
    (void)u; (void)x; (void)y;
    s_readback_calls++;
    s_last_readback_surface = s;
    fill(out, w, h, pitch, s_pixel);
}

/* The queued copy captures the target as it is when queued. */
static u32 test_async(void* u, u32 s, u32 x, u32 y, u32 w, u32 h)
{
    (void)u; (void)s; (void)x; (void)y;
    for (u32 i = 0; i < 8; i++)
        if (!s_tickets[i].used) {
            s_tickets[i].used = 1;
            memcpy(s_tickets[i].px, s_pixel, 4);
            s_tickets[i].w = w; s_tickets[i].h = h;
            s_async_issued++;
            return i + 1;
        }
    return 0;
}

static int test_async_fetch(void* u, u32 t, void* out, u32 pitch)
{
    (void)u;
    if (!t || t > 8 || !s_tickets[t - 1].used) return -1;
    fill(out, s_tickets[t - 1].w, s_tickets[t - 1].h, pitch, s_tickets[t - 1].px);
    s_tickets[t - 1].used = 0;
    s_async_fetched++;
    return 0;
}

static void test_async_release(void* u, u32 t)
{
    (void)u;
    if (t && t <= 8) s_tickets[t - 1].used = 0;
    s_async_released++;
}

/* ---- guest memory and a reader thread ----------------------------------- */

static u8* guest(u32 local_off) { return vm_base + GUEST_LOCAL_EA + local_off; }

typedef struct {
    const volatile u8* p;
    size_t len;
    u8 first[4], last[4];
    int uniform;           /* every 4-byte pixel equal to the first */
    volatile int done;
} reader;

static void* reader_main(void* arg)
{
    reader* r = (reader*)arg;
    memcpy(r->first, (const void*)r->p, 4);          /* faults if armed */
    r->uniform = 1;
    for (size_t o = 0; o < r->len; o += 4)
        if (memcmp((const void*)(r->p + o), r->first, 4)) { r->uniform = 0; break; }
    memcpy(r->last, (const void*)(r->p + r->len - 4), 4);
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Read [p, p + len) on another thread while this one -- the engine's -- keeps
 * servicing, as the FIFO walker does between methods. */
static int read_on_other_thread(reader* r, const u8* p, size_t len)
{
    memset(r, 0, sizeof *r);
    r->p = p; r->len = len;
    pthread_t t;
    if (pthread_create(&t, NULL, reader_main, r)) return 0;
    for (int i = 0; i < 50000 && !__atomic_load_n(&r->done, __ATOMIC_ACQUIRE); i++) {
        rsx_draw_engine_wcb_service();
        usleep(100);
    }
    const int ok = __atomic_load_n(&r->done, __ATOMIC_ACQUIRE);
    if (ok) pthread_join(t, NULL);
    return ok;
}

static int is_guest_px(const u8 g4[4], const u8 px[4])
{
    /* Host R,G,B,A is stored as the guest's A8R8G8B8 bytes. */
    return g4[0] == px[3] && g4[1] == px[0] && g4[2] == px[1] && g4[3] == px[2];
}

static void bind_target(u32 offset, u32 w, u32 h)
{
    m(M_SURFACE_FORMAT, 0x148);            /* A8R8G8B8 */
    m(M_COLOR_A_PITCH, 1024);
    m(M_SURFACE_CLIP_H, w << 16);
    m(M_SURFACE_CLIP_V, h << 16);
    m(M_COLOR_A_OFFSET, offset);
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
}

static void set_px(u8 r, u8 g_, u8 b, u8 a) { s_pixel[0] = r; s_pixel[1] = g_; s_pixel[2] = b; s_pixel[3] = a; }

static rsx_draw_engine_wcb_stats stats(void)
{
    rsx_draw_engine_wcb_stats s;
    rsx_draw_engine_wcb_stats_get(&s);
    return s;
}

static rsx_draw_backend wcb_backend(int async)
{
    rsx_draw_backend be = g_stub_backend;
    be.readback = test_readback_hook;
    if (async) {
        be.readback_async = test_async;
        be.readback_async_fetch = test_async_fetch;
        be.readback_async_release = test_async_release;
    }
    return be;
}

static void reset_counts(void)
{
    s_readback_calls = s_async_issued = s_async_fetched = s_async_released = 0;
    memset(s_tickets, 0, sizeof s_tickets);
}

/* ---- 1. sync: the original behaviour ------------------------------------- */

static void test_sync_mode(void)
{
    reset_counts();
    rsx_draw_engine_wcb_set_mode(RSX_WCB_SYNC);
    rsx_draw_backend be = wcb_backend(0);
    engine_up_with(&be);

    const u32 offscreen_off = 0x40000u;
    set_px(0x11, 0x22, 0x33, 0x44);
    bind_target(offscreen_off, 256, 256);
    draw_triangle();

    u32 slot = eng_current_surface();
    T(slot != ENG_INVALID, "offscreen surface slot allocated");
    T(g.surfaces[slot].guest_dirty == 1, "surface marked guest_dirty after draw");
    T(s_readback_calls == 0, "no readback issued before sync");
    u8* guest_ptr = guest(offscreen_off);
    T(memcmp(guest_ptr, "\x44\x11\x22\x33", 4) != 0, "guest memory empty before flush");

    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 1, "sync: readback invoked exactly once during flush");
    T(s_last_readback_surface == g.surfaces[slot].handle, "sync: readback targeted offscreen surface");
    T(g.surfaces[slot].guest_dirty == 0, "sync: guest_dirty cleared after flush");
    T(is_guest_px(guest_ptr, s_pixel), "sync: guest memory holds the rendered texels as A8R8G8B8");
    T(stats().sync_copies == 1 && stats().copied == 1 && stats().bytes == 1024u * 256u,
      "sync: counters report one copy of 256 KB");

    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 1, "sync: redundant flush is a no-op");

    const u32 display_off = 0x80000u;
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, display_off, 1024, 256, 256);
    m(M_COLOR_A_OFFSET, display_off);
    draw_triangle();
    const u32 disp_slot = eng_current_surface();
    T(disp_slot != ENG_INVALID && g.surfaces[disp_slot].guest_dirty == 1, "display target dirty after draw");
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 1, "sync: display scanout targets are not flushed");
    T(g.surfaces[disp_slot].guest_dirty == 1, "sync: display target dirty state retained");
    engine_down();
    rsx_draw_engine_set_display_buffer(0, 0, 0, 0, 0, 0);
}

/* ---- 2. ondemand ----------------------------------------------------------- */

static void test_ondemand_mode(void)
{
    reset_counts();
    rsx_draw_engine_wcb_set_mode(RSX_WCB_ONDEMAND);
    rsx_draw_backend be = wcb_backend(0);
    engine_up_with(&be);
    rsx_wcb_counters m0;
    rsx_wcb_counters_get(&m0);

    /* (a) A target only the GPU uses: armed once, never copied. */
    const u32 shadow_off = 0x100000u;
    bind_target(shadow_off, 256, 256);
    for (int frame = 0; frame < 4; frame++) {
        draw_triangle();
        rsx_draw_engine_flush_dirty_to_guest();     /* SET_REFERENCE */
        rsx_draw_engine_flush_dirty_to_guest();     /* SEMAPHORE_RELEASE */
    }
    const u32 shadow = eng_current_surface();
    T(s_readback_calls == 0, "ondemand: a target nobody reads is never read back");
    T(g.surfaces[shadow].wcb_pending == 1 && rsx_wcb_is_pending(shadow), "ondemand: its pages stay armed");
    T(stats().armed == 1 && stats().copied == 0, "ondemand: armed once across 8 sync points, 0 copies");
    T(stats().marked_dirty == 1, "ondemand: one dirty transition (it was never copied, so never clean)");

    /* (b) Copy on read: another thread touches the pages. */
    const u32 faces_off = 0x40000u;
    set_px(0x10, 0x20, 0x30, 0x40);
    bind_target(faces_off, 256, 256);
    draw_triangle();
    const u32 faces = eng_current_surface();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 0 && g.surfaces[faces].wcb_pending, "ondemand: sync point arms, does not copy");
    reader r;
    T(read_on_other_thread(&r, guest(faces_off), 1024u * 256u), "ondemand: reader thread finished");
    T(s_readback_calls == 1 && s_last_readback_surface == g.surfaces[faces].handle,
      "ondemand: the read caused exactly one readback, of that target");
    T(is_guest_px(r.first, s_pixel) && is_guest_px(r.last, s_pixel) && r.uniform,
      "ondemand: the reader saw the rendered bytes over the whole target, none stale");
    T(!g.surfaces[faces].wcb_pending && g.surfaces[faces].guest_dirty == 0,
      "ondemand: target clean and its pages open after the copy");
    T(g.surfaces[shadow].wcb_pending, "ondemand: the unread target is still armed");
    rsx_wcb_counters m1;
    rsx_wcb_counters_get(&m1);
    T(m1.faults > m0.faults && m1.requests > m0.requests && m1.waits > m0.waits,
      "ondemand: the module counted the fault, the request and the wait");
    T(stats().demand_copies == 1 && stats().waits >= 1, "ondemand: engine stats report it");

    /* A second read of a clean target costs nothing. */
    T(read_on_other_thread(&r, guest(faces_off), 1024u * 256u) && s_readback_calls == 1,
      "ondemand: reading a clean target again does not copy");

    /* (c) Drawn again: the next sync point re-arms, the next read sees the new
     * bytes -- never the previous copy. */
    set_px(0x55, 0x66, 0x77, 0x88);
    draw_triangle();
    T(g.surfaces[faces].guest_dirty == 1 && !g.surfaces[faces].wcb_pending,
      "ondemand: drawn after the copy: dirty, but no sync point yet");
    rsx_draw_engine_flush_dirty_to_guest();
    T(g.surfaces[faces].wcb_pending, "ondemand: re-armed at the next sync point");
    T(read_on_other_thread(&r, guest(faces_off), 1024u * 256u), "ondemand: second reader finished");
    T(is_guest_px(r.first, s_pixel) && r.uniform, "ondemand: second read sees the new bytes, not the old copy");
    T(s_readback_calls == 2, "ondemand: one readback per guest-visible change");

    /* (c') Several threads reading at once (SPU jobs over a crowd sheet):
     * each sees the whole rendered target, none a partial copy. */
    set_px(0x5A, 0x5B, 0x5C, 0x5D);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    {
        enum { N = 4 };
        reader rs[N];
        pthread_t th[N];
        for (int i = 0; i < N; i++) {
            memset(&rs[i], 0, sizeof rs[i]);
            rs[i].p = guest(faces_off) + (size_t)i * 64u * 1024u;   /* different pages */
            rs[i].len = 1024u * 256u - (size_t)i * 64u * 1024u;
            pthread_create(&th[i], NULL, reader_main, &rs[i]);
        }
        int all = 0;
        for (int k = 0; k < 50000 && !all; k++) {
            rsx_draw_engine_wcb_service();
            usleep(100);
            all = 1;
            for (int i = 0; i < N; i++) all &= __atomic_load_n(&rs[i].done, __ATOMIC_ACQUIRE);
        }
        int good = all;
        for (int i = 0; i < N && all; i++) {
            pthread_join(th[i], NULL);
            good &= rs[i].uniform && is_guest_px(rs[i].first, s_pixel);
        }
        /* Only where the copy is written through the protection: the
         * reopen-then-write fallback lets a concurrent reader see it half
         * written (docs/wcb-async.md, "Risks"), which this check catches. */
        if (rsx_wcb_forced_store())
            T(good, "ondemand: 4 concurrent readers each saw the whole rendered target");
        else
            T(all, "ondemand (reopen fallback): 4 concurrent readers finished");
        T(s_readback_calls == 3, "ondemand: 4 concurrent readers, one copy");
    }

    /* (c'') Nobody services the request: the reader is not hung forever. It
     * gets the stale bytes after the timeout, the counter says so, and the
     * next sync point arms the target again. */
    {
        const unsigned long long to0 = stats().timeouts;
        set_px(0x31, 0x32, 0x33, 0x34);
        draw_triangle();
        rsx_draw_engine_flush_dirty_to_guest();
        memset(&r, 0, sizeof r);
        r.p = guest(faces_off); r.len = 4096;
        pthread_t th;
        pthread_create(&th, NULL, reader_main, &r);
        for (int k = 0; k < 400 && !__atomic_load_n(&r.done, __ATOMIC_ACQUIRE); k++) usleep(5000);
        T(__atomic_load_n(&r.done, __ATOMIC_ACQUIRE), "ondemand: an unserviced reader gives up after the timeout");
        pthread_join(th, NULL);
        T(stats().timeouts == to0 + 1 && !is_guest_px(r.first, s_pixel),
          "ondemand: the timeout is counted, and it is the stale bytes that were read");
        T(s_readback_calls == 3, "ondemand: no copy happened");
        rsx_draw_engine_flush_dirty_to_guest();
        T(g.surfaces[faces].wcb_pending && rsx_wcb_is_pending(faces), "ondemand: re-armed after the timeout");
        T(read_on_other_thread(&r, guest(faces_off), 1024u * 256u) && is_guest_px(r.first, s_pixel),
          "ondemand: and the next read gets the rendered bytes");
        T(s_readback_calls == 4, "ondemand: one copy for it");
        s_readback_calls = 2;    /* keep the counts below as they were */
    }

    /* (d) The engine's own stream-ordered read (an NV3089 transfer out of the
     * target) resolves it inline on this thread. */
    set_px(1, 2, 3, 4);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(g.surfaces[faces].wcb_pending, "ondemand: armed before the transfer");
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, faces_off, 1024, 16, 16);
    T(!g.surfaces[faces].wcb_pending && s_readback_calls == 3, "ondemand: transfer resolved it with one readback");
    T(is_guest_px(guest(faces_off + 1024u * 255u), s_pixel), "ondemand: the whole target was copied, not just the rect");

    /* (e) A fault on the engine thread itself resolves inline too. */
    set_px(9, 8, 7, 6);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    u8 px[4];
    memcpy(px, (const void*)guest(faces_off + 4096), 4);
    T(is_guest_px(px, s_pixel) && s_readback_calls == 4, "ondemand: engine-thread fault resolved inline");

    /* (f) Two small targets sharing a host page: touching one never opens
     * the page with the other's bytes stale. */
    const u32 a_off = 0x200000u, b_off = a_off + 3u * 1024u;
    set_px(0xA1, 0xA2, 0xA3, 0xA4);
    bind_target(a_off, 256, 3);
    draw_triangle();
    const u32 sa = eng_current_surface();
    bind_target(b_off, 256, 3);
    draw_triangle();
    const u32 sb = eng_current_surface();
    rsx_draw_engine_flush_dirty_to_guest();
    T(g.surfaces[sa].wcb_pending && g.surfaces[sb].wcb_pending, "ondemand: both small targets armed");
    const u32 before = s_readback_calls;
    T(read_on_other_thread(&r, guest(a_off), 3u * 1024u), "ondemand: shared-page reader finished");
    T(!g.surfaces[sa].wcb_pending && !g.surfaces[sb].wcb_pending && s_readback_calls == before + 2,
      "ondemand: both targets on the touched page were copied before it opened");
    T(is_guest_px(guest(b_off), s_pixel), "ondemand: the neighbour's bytes are the rendered ones");

    /* The per-window counter line prints and resets without disturbing the
     * cumulative counters. */
    {
        const rsx_draw_engine_wcb_stats before_tick = stats();
        s_wcb.stats_every = 1;
        rsx_draw_engine_present();
        s_wcb.stats_every = 0;
        T(stats().copied == before_tick.copied && s_wcb.win.copied == 0,
          "stats: window line printed and reset, cumulative kept");
    }

    /* (g) Display buffers are presented, not read back: never armed. */
    const u32 display_off = 0x300000u;
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, display_off, 1024, 256, 256);
    bind_target(display_off, 256, 256);
    draw_triangle();
    const u32 disp = eng_current_surface();
    rsx_draw_engine_flush_dirty_to_guest();
    T(!g.surfaces[disp].wcb_pending, "ondemand: display buffer not armed");
    rsx_draw_engine_set_display_buffer(0, 0, 0, 0, 0, 0);

    /* (h) Shutdown copies what is still armed and reopens every page. */
    const u32 before_down = s_readback_calls;
    engine_down();
    T(s_readback_calls == before_down + 1, "ondemand: shutdown copied the one still-armed target");
    T(rsx_wcb_pending_count() == 0, "ondemand: no page left protected");
    T(is_guest_px(guest(shadow_off), s_pixel), "ondemand: guest memory readable and holds the GPU-only target's bytes");
}

/* ---- 3. async ---------------------------------------------------------------- */

static void test_async_mode(void)
{
    reset_counts();
    rsx_draw_engine_wcb_set_mode(RSX_WCB_ASYNC);
    rsx_draw_backend be = wcb_backend(1);
    engine_up_with(&be);
    reader r;

    const u32 off = 0x40000u, cold_off = 0x100000u;

    /* A target never read is never queued. */
    bind_target(cold_off, 256, 256);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_async_issued == 0 && s_readback_calls == 0, "async: a target never read gets no queued copy");

    /* First read of a target: on demand, synchronous. */
    set_px(0xC0, 0xC1, 0xC2, 0xC3);
    bind_target(off, 256, 256);
    draw_triangle();
    const u32 s = eng_current_surface();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_async_issued == 0, "async: no history yet, nothing queued");
    T(read_on_other_thread(&r, guest(off), 1024u * 256u) && is_guest_px(r.first, s_pixel),
      "async: first read served on demand");
    T(s_readback_calls == 1 && g.surfaces[s].wcb_reads == 1, "async: the read is remembered");

    /* Now hot: the next sync point queues its copy, and a read collects it
     * with no synchronous readback -- even though the target was drawn again
     * after the sync point (the label's bytes, as sync mode would give). */
    const u8 at_sync[4] = { 0xD0, 0xD1, 0xD2, 0xD3 };
    set_px(at_sync[0], at_sync[1], at_sync[2], at_sync[3]);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_async_issued == 1 && g.surfaces[s].wcb_ticket && s_readback_calls == 1,
      "async: hot target's copy queued at the sync point, no stall");
    set_px(0xE0, 0xE1, 0xE2, 0xE3);
    draw_triangle();                             /* the next frame's draw */
    T(read_on_other_thread(&r, guest(off), 1024u * 256u), "async: reader finished");
    T(is_guest_px(r.first, at_sync) && r.uniform, "async: the reader got the sync point's bytes, whole");
    T(s_async_fetched == 1 && s_readback_calls == 1, "async: served from the queued copy, no readback");
    T(g.surfaces[s].guest_dirty == 1, "async: drawn since the copy, so still dirty");

    /* The next sync point queues the newer bytes; the read gets them. */
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_async_issued == 2, "async: newer bytes queued at the next sync point");
    T(read_on_other_thread(&r, guest(off), 1024u * 256u) && is_guest_px(r.first, s_pixel),
      "async: the next read sees the newer bytes");
    T(g.surfaces[s].guest_dirty == 0, "async: clean once the copy matches the last draw");

    /* An engine-thread (stream-ordered) read wants the target as it is now:
     * the queued copy is dropped, the surface read back. */
    const u8 queued[4] = { 0xF0, 0xF1, 0xF2, 0xF3 };
    set_px(queued[0], queued[1], queued[2], queued[3]);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_async_issued == 3, "async: queued again");
    set_px(0x01, 0x02, 0x03, 0x04);
    draw_triangle();
    const u32 rb = s_readback_calls;
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, off, 1024, 16, 16);
    T(s_readback_calls == rb + 1 && is_guest_px(guest(off), s_pixel),
      "async: stream-ordered read took the current bytes");
    T(stats().async_discarded >= 1 && s_async_released >= 1, "async: the unused queued copy was released");

    /* A queued copy superseded before anyone read it is released. */
    set_px(0x21, 0x22, 0x23, 0x24);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    const unsigned long long disc = stats().async_discarded;
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(stats().async_discarded == disc + 1, "async: superseded copy released, not leaked");

    const rsx_draw_engine_wcb_stats st = stats();
    T(st.async_issued == s_async_issued && st.async_fetched == s_async_fetched,
      "async: engine stats agree with the backend's ticket counts");
    engine_down();
    int leaked = 0;
    for (int i = 0; i < 8; i++) leaked |= s_tickets[i].used;
    T(!leaked, "async: shutdown released every ticket");
}

/* ---- 4. off: nothing at all --------------------------------------------- */

static void test_off_mode(void)
{
    reset_counts();
    rsx_draw_engine_wcb_set_mode(RSX_WCB_OFF);
    rsx_draw_backend be = wcb_backend(1);
    engine_up_with(&be);
    bind_target(0x40000u, 256, 256);
    draw_triangle();
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 0 && s_async_issued == 0 && rsx_wcb_pending_count() == 0,
      "off: no copy, nothing armed");
    engine_down();
}

int main(void)
{
    /* Page-aligned and really mapped, so mprotect and the faults are real. */
    vm_base = (u8*)mmap(NULL, GUEST_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { perror("mmap"); return 1; }
    ppu_vm_size = GUEST_BYTES;
    /* Short, so the unserviced-reader case below finishes quickly; every
     * other read here is serviced within a fraction of a millisecond. */
    setenv("PS3RECOMP_RSX_WCB_TIMEOUT_MS", "300", 0);

    test_sync_mode();
    test_off_mode();
    test_ondemand_mode();
    memset(vm_base, 0, GUEST_BYTES);
    test_async_mode();

    printf("test_color_readback_guest: checks=%u failures=%u (forced store: %s)\n", checks, failures,
           rsx_wcb_forced_store() ? "yes" : "no");
    munmap(vm_base, GUEST_BYTES);
    return failures ? 1 : 0;
}
