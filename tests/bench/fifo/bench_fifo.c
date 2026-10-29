/* FIFO walker replay benchmark: ns per method word through
 * cellGcm_rsx_process_fifo, and a record of everything it did.
 *
 * One synthetic frame (~300k method words, a realistic mix: 90% register
 * writes in runs of 1-16, incrementing and non-incrementing, on subchannels
 * 0 and 1; 10% methods with side effects -- draws, clears, immediate and
 * inline vertices, NV406E semaphores, back-end labels, SET_REFERENCE,
 * GET_REPORT, notifies, a user command -- plus JUMPs, CALL/RET and a flip)
 * is written into IO memory and replayed through the real walker
 * (cellGcmSys.c, included below) into the real dispatcher (rsx_dispatch.c,
 * linked). The graphics backends are stubbed: every callback they would get
 * is recorded instead.
 *
 * Printed, for comparing two builds of the walker (run_fifo_bench.sh):
 *   calls  -- the number of handler calls and a hash over their sequence:
 *             every dispatcher sink callback and every rsx_process_method
 *             call, with its arguments, in order. With --verify each sink
 *             callback also hashes the whole dispatcher register file,
 *             write counts, transform program and constants as they were at
 *             that call, so a store moved across a draw shows up.
 *   state  -- a hash of the final dispatcher state, the label window, the
 *             fence queue, the report values and the walker's own counters.
 *   ns     -- best frame of the run, per method word and per header.
 *
 * bench_fifo [--engine|--legacy|--vtable] [--verify] [--frames N]
 *   --engine  the register-file draw engine is on and no vtable backend is
 *             registered (macOS Metal, Vulkan with guest programs: the
 *             default hosts). This is the path the sampler profiled.
 *   --legacy  the engine on and rsx_state fed too (PS3RECOMP_FIFO_LEGACY=1).
 *   --vtable  the engine is off; every method feeds rsx_state.
 *
 * Built by run_fifo_bench.sh against two source trees: GCM_SYS_C names the
 * cellGcmSys.c to include, BENCH_BASE marks a tree from before the run API.
 */
#include GCM_SYS_C
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

/* ---- the record ---------------------------------------------------------- */

static u64 g_hash = 0xcbf29ce484222325ull, g_calls;
static int g_verify, g_engine_on;
static rsx_dispatch g_rsx;

static inline void mix(u64 v)
{
    g_hash = (g_hash ^ v) * 0x100000001b3ull;
    g_hash ^= g_hash >> 29;
}
static u64 hash_words(u64 h, const void* p, size_t bytes)
{
    const u8* q = (const u8*)p;
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        u64 v; memcpy(&v, q + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull + (h >> 31);
    }
    return h;
}
static void call(u32 kind, u32 a, u32 b)
{
    g_calls++;
    mix(((u64)kind << 56) ^ ((u64)a << 24) ^ b);
}
/* The dispatcher's state as a sink callback sees it. */
static void call_state(void)
{
    if (!g_verify) return;
    u64 h = hash_words(0, g_rsx.regs, sizeof g_rsx.regs);
    h = hash_words(h, g_rsx.seen, sizeof g_rsx.seen);
    h = hash_words(h, g_rsx.vp, sizeof g_rsx.vp);
    h = hash_words(h, g_rsx.constants, sizeof g_rsx.constants);
    h = hash_words(h, g_rsx.tex_wgen, sizeof g_rsx.tex_wgen);
    mix(h ^ ((u64)g_rsx.inline_len << 32) ^ (u64)g_rsx.in_begin_end);
}

static void sk_clear(void* u, const rsx_dispatch* r, u32 mask) { (void)u; (void)r; call(1, mask, 0); call_state(); }
static void sk_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; call(2, prim, 0); call_state(); }
static void sk_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; call(3, 0, 0); call_state(); }
static void sk_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{ (void)u; (void)r; call(4, first, count); call_state(); }
static void sk_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{ (void)u; (void)r; call(5, first, count); call_state(); }
static void sk_inline(void* u, const rsx_dispatch* r, const u8* data, u32 bytes)
{ (void)u; (void)r; call(6, bytes, (u32)hash_words(0, data, bytes)); call_state(); }
static void sk_flip(void* u, const rsx_dispatch* r, u32 arg) { (void)u; (void)r; call(7, arg, 0); call_state(); }
static void sk_immediate(void* u, const rsx_dispatch* r) { (void)u; (void)r; call(8, 0, 0); call_state(); }

/* ---- the walker's consumers ---------------------------------------------- */

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (method == 0x50u) g_rsx_last_reference = data;    /* as rsx_commands.c */
    call(16, method, data);
    return 0;
}
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_draw_engine_enabled(void) { return g_engine_on; }
/* rsx_draw_engine.c's own bodies, on the bench's dispatcher. */
void rsx_draw_engine_method(u32 method, u32 arg)
{
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g_rsx, m, arg);
}
#ifndef BENCH_BASE
void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count)
{
    rsx_dispatch_method_run(&g_rsx, method, step, src, count);
}
#endif
void rsx_draw_engine_note_flip(u32 buffer_id) { call(17, buffer_id, 0); }
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
void rsx_raise_user_cmd(uint32_t arg) { call(18, arg, 0); }

/* ---- the frame ----------------------------------------------------------- */

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u      /* four IO pages */
#define CMD_SIZE  0x00080000u
#define FRAME_IO  0x00080000u      /* the frame, after the context's own ring */
#define SUB_IO    0x003F0000u      /* the CALL target */
#define FRAME_WORDS 300000u

static uint32_t g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static u32 g_rng = 0x2545F491u;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static u32 g_io;                                  /* write cursor */
static u32 n_words, n_hdrs, n_se_words;           /* method words, headers, side-effect words */
static void emit(u32 w) { vm_write32(IO_ADDR + g_io, w); g_io += 4; }
static void method(u32 subch, u32 m, int ni, u32 count, const u32* args, int se)
{
    emit(((ni ? 2u : 0u) << 29) | (count << 18) | (subch << 13) | m);
    for (u32 i = 0; i < count; i++) emit(args[i]);
    n_hdrs++; n_words += count;
    if (se) n_se_words += count;
}
static void one(u32 m, u32 v, int se) { method(0, m, 0, 1, &v, se); }

/* Register blocks a frame rewrites: first method, words. */
static const u32 k_blocks[][2] = {
    {0x0200, 12}, {0x0280, 4}, {0x0304, 3}, {0x0310, 6}, {0x032C, 10}, {0x0370, 1},
    {0x0A00, 2}, {0x0A20, 8}, {0x0A6C, 3}, {0x08C0, 2}, {0x08E4, 1}, {0x0900, 16},
    {0x1680, 16}, {0x1738, 2}, {0x1740, 16}, {0x1830, 4}, {0x1840, 16}, {0x1A00, 16},
    {0x1A20, 8}, {0x1A40, 8}, {0x1D60, 1}, {0x1D8C, 2}, {0x1FF0, 2}, {0x1940, 16},
    {0x1C00, 16}, {0x0A80, 16}, {0x1E40, 8}, {0x1500, 12}, {0x1DAC, 2}, {0x0B40, 16},
};

static void reg_run(void)
{
    u32 args[16];
    const u32 subch = (rnd() % 10u) == 0 ? 1u : 0u;
    const u32 r = rnd() % 20u;
    if (r == 0) {                                 /* transform constants */
        u32 id = rnd() % 480u;
        method(subch, 0x1EFC, 0, 1, &id, 0);
        for (u32 i = 0; i < 16; i++) args[i] = rnd();
        method(subch, 0x1F00, 0, 16, args, 0);
        return;
    }
    if (r == 1) {                                 /* vertex program microcode */
        u32 id = rnd() % 500u;
        method(subch, 0x1E9C, 0, 1, &id, 0);
        for (u32 i = 0; i < 16; i++) args[i] = rnd();
        method(subch, 0x0B80, 0, 16, args, 0);
        return;
    }
    const u32* b = k_blocks[rnd() % (sizeof k_blocks / sizeof k_blocks[0])];
    const int ni = (rnd() % 7u) == 0;
    u32 count = 1u + rnd() % 16u;
    u32 first = b[0];
    if (!ni) {
        if (count > b[1]) count = b[1];
        first += 4u * (rnd() % (b[1] - count + 1u));
    } else {
        first += 4u * (rnd() % b[1]);
    }
    for (u32 i = 0; i < count; i++) args[i] = rnd();
    method(subch, first, ni, count, args, 0);
}

static u32 g_label_val;
static void side_effect(void)
{
    u32 args[16];
    switch (rnd() % 12u) {
    case 0: case 1: case 2:                       /* array draw */
        one(0x1808, 5, 1);
        { const u32 n = 1u + rnd() % 4u;
          for (u32 i = 0; i < n; i++) args[i] = ((rnd() % 255u) << 24) | (rnd() & 0xFFFFu);
          method(0, 0x1814, 1, n, args, 1); }
        one(0x1808, 0, 1);
        break;
    case 3: case 4:                               /* indexed draw */
        args[0] = rnd() & 0xFFFFF0u; args[1] = 0x10u;
        method(0, 0x181C, 0, 2, args, 0);
        one(0x1808, 4, 1);
        one(0x1824, ((rnd() % 255u) << 24) | (rnd() & 0xFFFFu), 1);
        one(0x1808, 0, 1);
        break;
    case 5:                                       /* clear */
        one(0x1D90, rnd(), 0);
        one(0x1D94, 0xF3, 1);
        break;
    case 6:                                       /* immediate vertices, attr 0 */
        one(0x1808, 6, 1);
        for (int v = 0; v < 3; v++) {
            for (u32 i = 0; i < 4; i++) args[i] = rnd();
            method(0, 0x1C00, 0, 4, args, 1);
        }
        one(0x1808, 0, 1);
        break;
    case 7:                                       /* inline array */
        one(0x1808, 5, 1);
        for (u32 i = 0; i < 12; i++) args[i] = rnd();
        method(0, 0x1818, 1, 12, args, 1);
        one(0x1808, 0, 1);
        break;
    case 8: {                                     /* NV406E release, then an acquire of it */
        const u32 off = (rnd() % 256u) * 16u;
        g_label_val++;
        one(0x64, off, 1); one(0x6C, g_label_val, 1);
        one(0x64, off, 1); one(0x68, g_label_val, 1);
        break; }
    case 9:                                       /* back-end labels, reference */
        one(0x1A4, 0x66626660u, 1);
        one(0x1D6C, (rnd() % 256u) * 16u, 1);
        one(0x1D70, rnd(), 1);
        args[0] = (rnd() % 256u) * 16u; args[1] = 0x66616661u; args[2] = rnd();
        method(0, 0x1D6C, 0, 3, args, 1);         /* offset, 0x1D70, 0x1D74 */
        one(0x50, rnd(), 1);
        break;
    case 10:                                      /* report, notify */
        one(0x1A8, 0xFEED0000u, 1);
        one(0x1800, (1u << 24) | ((rnd() % 64u) * 16u), 1);
        one(0x180, 0x66604200u, 1);
        one(0x104, 0, 1);
        break;
    case 11: {                                    /* user command (0xEB00 is subch 7, 0x0B00) */
        const u32 v = rnd();
        method(7, 0x0B00, 0, 1, &v, 1);
        break; }
    }
}

/* A CALL target: a few register writes, then RET. */
static void build_sub(void)
{
    const u32 save = g_io;
    g_io = SUB_IO;
    u32 args[8];
    for (u32 i = 0; i < 8; i++) args[i] = 0x3F800000u + i;
    method(0, 0x0A20, 0, 8, args, 0);
    emit(0x00020000u);
    g_io = save;
}

static u32 build_frame(void)
{
    g_io = FRAME_IO;
    build_sub();
    while (n_words < FRAME_WORDS) {
        const u32 r = rnd() % 64u;
        if (r == 0) {                             /* JUMP over a few words that are not commands */
            const u32 tgt = g_io + 4u + 8u * 4u;
            emit(0x20000000u | tgt);
            for (int i = 0; i < 8; i++) emit(0xDEAD0000u | (u32)i);
        } else if (r == 1) {
            emit(SUB_IO | 2u);                    /* CALL */
        } else if (n_se_words * 10u < n_words && (r & 3u) == 0)
            side_effect();
        else
            reg_run();
    }
    emit(GCM_FLIP_MARKER | 1u);
    return g_io;
}

static u64 state_hash(void)
{
    u64 h = hash_words(0, g_rsx.regs, sizeof g_rsx.regs);
    h = hash_words(h, g_rsx.seen, sizeof g_rsx.seen);
    h = hash_words(h, g_rsx.vp, sizeof g_rsx.vp);
    h = hash_words(h, g_rsx.constants, sizeof g_rsx.constants);
    h = hash_words(h, g_rsx.tex_wgen, sizeof g_rsx.tex_wgen);
    h = hash_words(h, vm_base + GCM_LABEL_GUEST_BASE, 0x10000u);
    for (u32 i = 0; i < CELL_GCM_MAX_REPORT_COUNT; i++) h = (h ^ s_report_data[i].value) * 0x100000001b3ull;
    const u32 w[] = { s_fifo_getoff, s_sema_offset, s_fifo_back_jumps, s_flip_request_count,
                      s_fifo_flips_reached, s_report_dma, g_rsx_last_reference, s_ref_qtail,
                      (u32)g_rsx.in_begin_end, g_rsx.inline_len, g_rsx.inline_dropped, 0 };
    return hash_words(h, w, sizeof w);
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(int argc, char** argv)
{
    int frames = 40;
    const char* cfg = "engine";
    g_engine_on = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine")) g_engine_on = 1, cfg = "engine";
        else if (!strcmp(argv[i], "--legacy")) g_engine_on = 1, cfg = "legacy", setenv("PS3RECOMP_FIFO_LEGACY", "1", 1);
        else if (!strcmp(argv[i], "--vtable")) g_engine_on = 0, cfg = "vtable";
        else if (!strcmp(argv[i], "--verify")) g_verify = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--engine|--legacy|--vtable] [--verify] [--frames N]\n", argv[0]); return 2; }
    }
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("guest VM alloc failed"); return 1; }
    ppu_vm_size = vm_size;

    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear = sk_clear; sink.begin = sk_begin; sink.end = sk_end;
    sink.draw_arrays = sk_arrays; sink.draw_index_array = sk_index;
    sink.inline_array = sk_inline; sink.flip = sk_flip; sink.immediate_vertex = sk_immediate;
    rsx_dispatch_init(&g_rsx, &sink);

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const u32 put = build_frame();

    double best = 1e30;
    for (int f = 0; f < frames; f++) {
        s_fifo_getoff = FRAME_IO;
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
        const double t0 = now_ns();
        for (int pass = 0; s_fifo_getoff != put && pass < 64; pass++) cellGcm_rsx_process_fifo();
        const double t = now_ns() - t0;
        if (s_fifo_getoff != put) { printf("frame %d: walker stopped at 0x%08X, put 0x%08X\n", f, s_fifo_getoff, put); return 1; }
        if (t < best) best = t;
        /* The fences the ticker would publish. */
        while (s_ref_qhead != s_ref_qtail) { mix(s_ref_q[s_ref_qhead % GCM_REF_QLEN]); s_ref_qhead++; }
    }
    printf("config %s%s frames %d\n", cfg, g_verify ? " verify" : "", frames);
    printf("frame %u method words, %u headers, %.1f%% side-effect words, %u bytes\n",
           n_words, n_hdrs, 100.0 * n_se_words / n_words, put - FRAME_IO);
    printf("calls %llu %016llx\n", (unsigned long long)g_calls, (unsigned long long)g_hash);
    printf("state %016llx\n", (unsigned long long)state_hash());
    printf("time best frame %.0f us: %.2f ns/word %.2f ns/header\n",
           best / 1e3, best / n_words, best / n_hdrs);
    free(vm_base);
    return 0;
}
