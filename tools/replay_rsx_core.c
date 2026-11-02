/*
 * RSX FIFO Replay Core Implementation
 * Replays dumped FIFO command streams through cellGcmSys and rsx_dispatch.
 */
#include "replay_rsx_core.h"
#include "../libs/video/cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VM environment stubs for standalone execution */
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
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }

/* Replay context tracking */
struct rsx_replay_context {
    uint32_t io_addr;
    uint32_t io_size;
    uint32_t stream_start;
    uint32_t stream_end;
    uint32_t word_count;
    int engine_mode;
    rsx_dispatch rsx;
};

static rsx_replay_context* g_active_replay_ctx = NULL;
static uint64_t g_call_hash = 0xcbf29ce484222325ull;
static uint64_t g_call_count = 0;
static uint32_t g_draw_count = 0;
static uint32_t g_clear_count = 0;
static uint32_t g_flip_count = 0;
static int g_verify_active = 0;

static inline void mix_hash(uint64_t v)
{
    g_call_hash = (g_call_hash ^ v) * 0x100000001b3ull;
    g_call_hash ^= g_call_hash >> 29;
}

static uint64_t hash_memory(uint64_t h, const void* p, size_t bytes)
{
    const uint8_t* q = (const uint8_t*)p;
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t v;
        memcpy(&v, q + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull + (h >> 31);
    }
    return h;
}

static void record_call(uint32_t kind, uint32_t a, uint32_t b)
{
    g_call_count++;
    mix_hash(((uint64_t)kind << 56) ^ ((uint64_t)a << 24) ^ b);
}

static void record_state(void)
{
    if (!g_verify_active || !g_active_replay_ctx) return;
    const rsx_dispatch* r = &g_active_replay_ctx->rsx;
    uint64_t h = hash_memory(0, r->regs, sizeof(r->regs));
    h = hash_memory(h, r->seen, sizeof(r->seen));
    h = hash_memory(h, r->vp, sizeof(r->vp));
    h = hash_memory(h, r->constants, sizeof(r->constants));
    mix_hash(h ^ ((uint64_t)r->inline_len << 32) ^ (uint64_t)r->in_begin_end);
}

static void sk_clear(void* u, const rsx_dispatch* r, u32 mask)
{
    (void)u; (void)r;
    g_clear_count++;
    record_call(1, mask, 0);
    record_state();
}

static void sk_begin(void* u, const rsx_dispatch* r, u32 prim)
{
    (void)u; (void)r;
    record_call(2, prim, 0);
    record_state();
}

static void sk_end(void* u, const rsx_dispatch* r)
{
    (void)u; (void)r;
    record_call(3, 0, 0);
    record_state();
}

static void sk_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r;
    g_draw_count++;
    record_call(4, first, count);
    record_state();
}

static void sk_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r;
    g_draw_count++;
    record_call(5, first, count);
    record_state();
}

static void sk_inline(void* u, const rsx_dispatch* r, const u8* data, u32 bytes)
{
    (void)u; (void)r;
    g_draw_count++;
    record_call(6, bytes, (u32)hash_memory(0, data, bytes));
    record_state();
}

static void sk_flip(void* u, const rsx_dispatch* r, u32 arg)
{
    (void)u; (void)r;
    g_flip_count++;
    record_call(7, arg, 0);
    record_state();
}

static void sk_immediate(void* u, const rsx_dispatch* r)
{
    (void)u; (void)r;
    record_call(8, 0, 0);
    record_state();
}

/* Walker consumers */
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (method == 0x50u) g_rsx_last_reference = data;
    record_call(16, method, data);
    return 0;
}

int rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{
    (void)state; (void)method; (void)src; (void)count;
    return 0;
}

rsx_backend* rsx_get_backend(void) { return NULL; }

int rsx_draw_engine_enabled(void)
{
    return g_active_replay_ctx ? (g_active_replay_ctx->engine_mode != 0) : 1;
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g_active_replay_ctx) return;
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g_active_replay_ctx->rsx, m, arg);
}

void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count)
{
    if (!g_active_replay_ctx || !src) return;
    const u32* words = (const u32*)src;
    for (u32 i = 0; i < count; i++) {
        u32 m = method + i * step;
        u32 m_clamped = (m >= 0xE000u) ? (m & 0xFFFCu) : (m & 0x1FFCu);
        rsx_dispatch_method(&g_active_replay_ctx->rsx, m_clamped, words[i]);
    }
}

void rsx_draw_engine_note_flip(u32 buffer_id)
{
    g_flip_count++;
    record_call(17, buffer_id, 0);
}

void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_flush_dirty_to_guest(void) {}
void rsx_draw_engine_wcb_service(void) {}
void rsx_draw_engine_wcb_set_wake(void (*wake)(void)) { (void)wake; }
void rsx_draw_engine_set_display_query(int (*fn)(u32 offset)) { (void)fn; }
void rsx_draw_engine_set_display_buffer(u32 id, u32 loc, u32 off, u32 w, u32 h, u32 p) { (void)id; (void)loc; (void)off; (void)w; (void)h; (void)p; }
/* Profiler/capture are real-engine features; this verification replay does
 * not use them. */
int  rsx_wprof_enabled(void) { return 0; }
unsigned long long rsx_wprof_now(void) { return 0; }
void rsx_wprof_note_drain(unsigned long long ns) { (void)ns; }
void rsx_wprof_note_dispatch(unsigned long long ns) { (void)ns; }
void rsx_wprof_note_flip(void) {}
void rsx_wprof_drain_end(void) {}
int  rsx_wcap_begin_if_triggered(void) { return 0; }
void rsx_wcap_word(u32 w) { (void)w; }
void rsx_wcap_flip(u32 w) { (void)w; }
void rsx_wcap_drain_end(void) {}
void rsx_wcap_note_texture(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_live_draw_set_display_buffer(u32 id, u32 loc, u32 off, u32 w, u32 h, u32 p) { (void)id; (void)loc; (void)off; (void)w; (void)h; (void)p; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { record_call(18, arg, 0); }

#ifndef _WIN32
HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, const char* name)
{
    (void)sa; (void)manual; (void)initial; (void)name;
    return (HANDLE)(uintptr_t)1;
}
BOOL SetEvent(HANDLE h) { (void)h; return 1; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
#endif

static uint32_t s_replay_heap = 0x00800000u;
static u32 replay_host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    s_replay_heap = (s_replay_heap + align - 1u) & ~(align - 1u);
    u32 r = s_replay_heap;
    s_replay_heap += size;
    return r;
}

static void replay_host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static uint64_t compute_full_state_hash(const rsx_replay_context* ctx)
{
    const rsx_dispatch* r = &ctx->rsx;
    uint64_t h = hash_memory(0, r->regs, sizeof(r->regs));
    h = hash_memory(h, r->vp, sizeof(r->vp));
    h = hash_memory(h, r->constants, sizeof(r->constants));
    if (vm_base) {
        h = hash_memory(h, vm_base + GCM_LABEL_GUEST_BASE, 0x1000u);
    }
    for (u32 i = 0; i < CELL_GCM_MAX_REPORT_COUNT; i++) {
        h = (h ^ s_report_data[i].value) * 0x100000001b3ull;
    }
    const u32 state_vec[] = {
        s_fifo_getoff, s_sema_offset, s_report_dma, g_rsx_last_reference,
        (u32)r->in_begin_end, r->inline_len, r->inline_dropped, g_draw_count,
        g_clear_count, g_flip_count, 0
    };
    h = hash_memory(h, state_vec, sizeof(state_vec));
    return h ^ g_call_hash;
}

rsx_replay_context* rsx_replay_create(uint32_t io_addr, uint32_t io_size, int engine_mode)
{
    rsx_replay_context* ctx = (rsx_replay_context*)calloc(1, sizeof(rsx_replay_context));
    if (!ctx) return NULL;

    ctx->io_addr = io_addr ? io_addr : 0x00100000u;
    ctx->io_size = io_size ? io_size : 0x00400000u;
    ctx->engine_mode = engine_mode;
    g_active_replay_ctx = ctx;

    const uint32_t vm_size = ppu_hle_inject_base + 0x100000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) {
        free(ctx);
        return NULL;
    }
    ppu_vm_size = vm_size;

    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.clear = sk_clear;
    sink.begin = sk_begin;
    sink.end = sk_end;
    sink.draw_arrays = sk_arrays;
    sink.draw_index_array = sk_index;
    sink.inline_array = sk_inline;
    sink.flip = sk_flip;
    sink.immediate_vertex = sk_immediate;
    rsx_dispatch_init(&ctx->rsx, &sink);

    u32 ctx_out = replay_host_alloc(4, 4);
    uint32_t cmd_size = 0x00080000u;
    (void)cellGcmSetupContext(ctx_out, cmd_size, ctx->io_size, ctx->io_addr, replay_host_alloc, replay_host_w32);

    g_active_replay_ctx = ctx;
    return ctx;
}

int rsx_replay_load_words(rsx_replay_context* ctx, const uint32_t* words, size_t word_count)
{
    if (!ctx || !words || !word_count) return -1;
    if (word_count * 4 > ctx->io_size) return -2;

    /* The recorded frame is placed after the initial context ring in IO space */
    ctx->stream_start = 0x00080000u;
    ctx->word_count = (uint32_t)word_count;
    ctx->stream_end = ctx->stream_start + ctx->word_count * 4;

    u32 ea = gcm_io2ea(ctx->stream_start);
    if (!ea) return -3;

    memcpy(vm_base + ea, words, word_count * 4);
    return 0;
}

int rsx_replay_run_frame(rsx_replay_context* ctx, rsx_replay_frame_stats* stats_out, int verify_each_call)
{
    if (!ctx) return -1;

    g_active_replay_ctx = ctx;
    g_verify_active = verify_each_call;
    g_call_count = 0;
    g_draw_count = 0;
    g_clear_count = 0;
    g_flip_count = 0;
    g_call_hash = 0xcbf29ce484222325ull;

    s_fifo_getoff = ctx->stream_start;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, ctx->stream_end);
    s_flip_request_count = 0;
    s_fifo_flips_reached = 0;
    s_fifo_flips_queued = 0;
    s_ref_qhead = 0;
    s_ref_qtail = 0;
    s_fifo_back_jumps = 0;
    memset(ctx->rsx.seen, 0, sizeof(ctx->rsx.seen));

    /* Drain the FIFO until caught up to put */
    for (int pass = 0; s_fifo_getoff != ctx->stream_end && pass < 256; pass++) {
        cellGcm_rsx_process_fifo();
    }

    /* Drain pending fences */
    while (s_ref_qhead != s_ref_qtail) {
        mix_hash(s_ref_q[s_ref_qhead % GCM_REF_QLEN]);
        s_ref_qhead++;
    }

    if (stats_out) {
        stats_out->total_calls = g_call_count;
        stats_out->state_hash = compute_full_state_hash(ctx);
        stats_out->words_processed = ctx->word_count;
        stats_out->draws = g_draw_count;
        stats_out->clears = g_clear_count;
        stats_out->flips = g_flip_count;
    }

    return (s_fifo_getoff == ctx->stream_end) ? 0 : -2;
}

void rsx_replay_destroy(rsx_replay_context* ctx)
{
    if (!ctx) return;
    if (g_active_replay_ctx == ctx) g_active_replay_ctx = NULL;
    if (vm_base) {
        free(vm_base);
        vm_base = NULL;
    }
    free(ctx);
}
