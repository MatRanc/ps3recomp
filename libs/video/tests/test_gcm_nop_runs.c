/* The FIFO walker passes over runs of NOP words at once (cellGcmSys.c).
 *
 * A run of zero words is skipped in one step, up to put or the end of the
 * 1 MB IO page. The methods around the runs must still arrive in order with
 * their data, a run must stop at put when the zeros go on past it (and the
 * next pass must carry on from there), and a run that reaches the end of an
 * IO page must not skip what the next page holds.
 *
 * The walk's own bookkeeping must be what a word-at-a-time walk leaves: the
 * trace ring holds the last GCM_TRACE_N words the walker read, NOPs
 * included, and a park whose JUMP was overwritten with NOPs is forgotten.
 *
 * -DENGINE_ON builds it with the register-file draw engine on, the methods
 * arriving through rsx_draw_engine_method(s). Run that build with
 * TCONST_FIFO=1 as well: that switch keeps NOPs on the per-word path, so
 * both walks are checked against the same expectations.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_nop_runs.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_nop_runs
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
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
/* Every method the walker delivers, in order. */
#define PROBE_METHOD 0x1EE0u
static u32 seen_m[16], seen_d[16], seen_n;
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
/* cellGcmSys asks whether a vtable backend is registered (#466). */
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (seen_n < 16) { seen_m[seen_n] = method; seen_d[seen_n] = data; }
    seen_n++;
    return 0;
}
#ifndef ENGINE_ON
#define ENGINE_ON 0
#endif
int  rsx_draw_engine_enabled(void) { return ENGINE_ON; }
void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (seen_n < 16) { seen_m[seen_n] = method & 0x1FFCu; seen_d[seen_n] = arg; }
    seen_n++;
}
void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count)
{
    for (u32 i = 0; i < count; i++, src += 4)
        rsx_draw_engine_method(method + i * step,
                               ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3]);
}
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

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00200000u     /* two IO pages */
#define CMD_SIZE  0x00010000u
#define PAGE2     0x00100000u     /* IO offset of the second page */

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

/* The words a word-at-a-time walk reads at the top of its loop (headers,
 * NOPs, JUMPs; not method data), in order: what the trace ring must hold. */
static u32 e_off[512], e_w[512], e_n;
static void visit(u32 io, u32 w) { e_off[e_n] = io; e_w[e_n] = w; e_n++; }
static void visit_nops(u32 from, u32 to) { for (u32 io = from; io < to; io += 4) visit(io, 0); }
static void check_trace(const char* what)
{
    int ok = s_tr_i == e_n;
    for (u32 j = 0; j < GCM_TRACE_N && j < e_n && ok; j++) {
        const u32 k = (s_tr_i - 1u - j) % GCM_TRACE_N;
        ok = s_tr_off[k] == e_off[e_n - 1u - j] && s_tr_w[k] == e_w[e_n - 1u - j];
    }
    if (!ok) printf("  trace: %u words read, %u expected; last 0x%08X/0x%08X, expected 0x%08X/0x%08X\n",
                    s_tr_i, e_n, s_tr_off[(s_tr_i - 1u) % GCM_TRACE_N], s_tr_w[(s_tr_i - 1u) % GCM_TRACE_N],
                    e_off[e_n - 1u], e_w[e_n - 1u]);
    check(ok, what);
}

/* One method, count 1, subchannel 0. */
static u32 put_probe(u32 io, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, arg);
    visit(io, (1u << 18) | PROBE_METHOD);
    return io + 8;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* Pass 1: probe 0, 50 NOPs, a two-word incrementing run, 3 NOPs, and put
     * five words into the zeros that follow. */
    printf("  engine %s, TCONST_FIFO %s\n", ENGINE_ON ? "on" : "off", getenv("TCONST_FIFO") ? "set" : "unset");
    u32 io = put_probe(0, 0);
    visit_nops(io, io + 50 * 4);
    io += 50 * 4;
    gw32(IO_ADDR + io, (2u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, 1);
    gw32(IO_ADDR + io + 8, 2);
    visit(io, (2u << 18) | PROBE_METHOD);
    io += 12 + 3 * 4;
    const u32 put1 = io + 5 * 4;
    visit_nops(io - 3 * 4, put1);

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put1);
    cellGcm_rsx_process_fifo();

    check(seen_n == 3, "pass 1: three method words delivered");
    check(seen_m[0] == PROBE_METHOD && seen_d[0] == 0, "pass 1: probe before the NOP run");
    check(seen_m[1] == PROBE_METHOD && seen_d[1] == 1 &&
          seen_m[2] == PROBE_METHOD + 4 && seen_d[2] == 2, "pass 1: the run after the NOPs, in order");
    check(s_fifo_getoff == put1, "pass 1: the NOP run stops at put, inside the zeros");
    check_trace("pass 1: the trace ring holds every word read, NOPs included");

    /* Pass 2: more zeros, then probe 3. */
    const u32 p3 = put1 + 6 * 4;
    visit_nops(put1, p3);
    const u32 put2 = put_probe(p3, 3);
    vm_write32(ctrl + 0, put2);
    cellGcm_rsx_process_fifo();
    check(seen_n == 4 && seen_m[3] == PROBE_METHOD && seen_d[3] == 3, "pass 2: carries on from put through the zeros");
    check(s_fifo_getoff == put2, "pass 2: get reaches put");

    /* Pass 3: JUMP near the end of the first IO page, zeros to its end, and
     * probe 5 as the first word of the second page. */
    const u32 tail = PAGE2 - 0x40;
    gw32(IO_ADDR + put2, 0x20000000u | tail);
    visit(put2, 0x20000000u | tail);
    visit_nops(tail, PAGE2);
    const u32 put3 = put_probe(PAGE2, 5);
    vm_write32(ctrl + 0, put3);
    cellGcm_rsx_process_fifo();
    check(seen_n == 5 && seen_m[4] == PROBE_METHOD && seen_d[4] == 5,
          "pass 3: the first word of the next IO page is not skipped");
    check(s_fifo_getoff == put3, "pass 3: get reaches put");
    check_trace("pass 3: the trace ring holds the JUMP and the NOPs to the page end");

    /* Pass 4: a self-JUMP park at put3, with put past it. */
    const u32 put4 = put3 + 0x200;   /* 128 words: longer than the trace ring */
    gw32(IO_ADDR + put3, 0x20000000u | put3);
    visit(put3, 0x20000000u | put3);
    vm_write32(ctrl + 0, put4);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == put3 && park_off == put3, "pass 4: the walker parks on the self-JUMP");

    /* Pass 5: the park is overwritten with zeros, as far as put. */
    gw32(IO_ADDR + put3, 0);
    visit_nops(put3, put4);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == put4, "pass 5: get runs through the zeros to put");
    check(park_off == 0, "pass 5: a park overwritten with NOPs is forgotten");
    check_trace("pass 5: the trace ring holds the last NOPs of a run longer than it");

    printf("[gcm_nop_runs] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
