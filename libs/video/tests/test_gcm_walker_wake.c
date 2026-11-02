/* PS3RECOMP_WALKER_WAKE: an SPU PUT on the parked word wakes the walker
 * (cellGcmSys.c).
 *
 * The present thread waits for the next `put` on its kick event with a bounded
 * timeout. A park whose slot is filled by an SPU DMA is therefore only re-read
 * when that timeout expires: measured p50 0.53 ms, p90 2.15 ms of every frame.
 * gcm_note_spu_put remembers the parked `get` and signals the kick event when
 * a PUT range contains it; a PUT anywhere else must not.
 *
 * Block A (io 0) runs a probe and parks at io 8; put is the end of block B
 * (io 0x8000). A producer thread patches the park to a JUMP to B and reports
 * the DMA. The idle wait must return in tens of milliseconds, not its 2 s
 * timeout, and the next pass must run B. A PUT outside the park must leave the
 * wait to its timeout.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_walker_wake.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_walker_wake
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* The auto-reset event the idle wait uses. The real one lives in
 * runtime/platform/win32_compat.c; the fixture links no runtime, so provide
 * the same semantics here (a signal survives until one waiter takes it). */
typedef struct { pthread_mutex_t m; pthread_cond_t c; int set; } test_event;
HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
    (void)sa; (void)manual_reset; (void)name;
    test_event* e = (test_event*)calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->set = initial_state != 0;
    return (HANDLE)e;
}
BOOL SetEvent(HANDLE h)
{
    test_event* e = (test_event*)h;
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
    return 1;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    test_event* e = (test_event*)h;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&e->m);
    while (!e->set) {
        int rc;
        if (ms == 0xFFFFFFFFu) rc = pthread_cond_wait(&e->c, &e->m);
        else                   rc = pthread_cond_timedwait(&e->c, &e->m, &ts);
        if (rc == ETIMEDOUT) break;
    }
    const int got = e->set;
    if (got) e->set = 0;                 /* auto-reset */
    pthread_mutex_unlock(&e->m);
    return got ? 0u : 258u;              /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
}
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
/* The walker's other consumers: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* Counts the probe methods the walker executed. */
#define PROBE_METHOD 0x1EE0u
static u32 seen_n;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state; (void)data;
    if (method == PROBE_METHOD) seen_n++;
    return 0;
}
int  rsx_draw_engine_enabled(void) { return 0; }
/* The walker hands vertex-program runs here (#554) and asks for a vtable
 * backend (#466); this fixture has neither. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count) { (void)method; (void)step; (void)src; (void)count; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void rsx_draw_engine_set_display_query(int (*fn)(u32)) { (void)fn; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u
#define SEG_B     0x00008000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* One method, count 1, subchannel 0. */
static u32 put_probe(u32 io, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, arg);
    return io + 8;
}

struct wake_arg { u32 park; unsigned delay_us; int covering; };

static void* wake_producer(void* arg)
{
    struct wake_arg* w = (struct wake_arg*)arg;
    usleep(w->delay_us);
    if (w->covering) {
        /* The title links B in: the self-JUMP park becomes a JUMP to B, and
         * the SPU job's DMA is what makes that word visible to the walker. */
        gw32(IO_ADDR + w->park, 0x20000000u | SEG_B);
        gcm_note_spu_put(IO_ADDR + w->park, 4);
    } else {
        gcm_note_spu_put(IO_ADDR + 0x100u, 4);
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("PS3RECOMP_WALKER_WAKE", "1", 1);   /* opt-in; see cellGcmSys.c */
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    (void)cellGcm_fifo_kick_event();          /* the wait's auto-reset event */

    /* A: probe, then a park at io 8.  B: probe; put after it. */
    const u32 park = put_probe(0, 0);
    gw32(IO_ADDR + park, 0x20000000u | park);
    const u32 put = put_probe(SEG_B, 1);

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();
    check(seen_n == 1 && s_fifo_getoff == park && s_walker_park_get == park,
          "the walker parks and remembers the parked word");

    struct wake_arg w1 = { park, 30000, 1 };
    pthread_t th;
    if (pthread_create(&th, NULL, wake_producer, &w1) != 0) {
        puts("FAIL: producer thread"); free(vm_base); return 1;
    }
    const unsigned long long t0 = ps3_ms_now();
    cellGcm_fifo_kick_wait(2000);
    const unsigned long long t1 = ps3_ms_now();
    pthread_join(th, NULL);
    check(t1 - t0 < 500, "a PUT on the parked word wakes the idle wait promptly");
    cellGcm_rsx_process_fifo();
    check(seen_n == 2 && s_fifo_getoff == put,
          "the woken walker follows the patched park to put");

    /* Re-park, and report a PUT that does not touch the parked word. */
    gw32(IO_ADDR + park, 0x20000000u | park);
    s_fifo_getoff = 0;
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == park && s_walker_park_get == park, "parked again");

    struct wake_arg w2 = { park, 10000, 0 };
    if (pthread_create(&th, NULL, wake_producer, &w2) != 0) {
        puts("FAIL: producer thread"); free(vm_base); return 1;
    }
    const unsigned long long t2 = ps3_ms_now();
    cellGcm_fifo_kick_wait(150);
    const unsigned long long t3 = ps3_ms_now();
    pthread_join(th, NULL);
    check(t3 - t2 >= 100, "a PUT elsewhere leaves the wait to its timeout");

    printf("[gcm_walker_wake] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
