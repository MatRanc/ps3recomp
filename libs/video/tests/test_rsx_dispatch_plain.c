/*
 * ps3recomp - the dispatcher's plain register stores
 *
 * rsx_dispatch_method_run stores the methods dsp_method_plain names straight
 * into the register file and hands every other one to rsx_dispatch_method.
 * That is right only while each plain method does nothing in
 * rsx_dispatch_method but count the write and store it. For every NV4097
 * method 0x0000..0x1FFC, inside BEGIN/END and outside it, with every sink
 * set and a register file of noise, this checks that a plain method leaves
 * the whole dispatcher exactly as that store does and calls no sink. It
 * prints how many methods outside the set behaved as plain in the same
 * states, so the set can be seen to be no smaller than it needs to be.
 *
 * Then random runs, incrementing and not, some wrapping past 0x1FFC: one
 * rsx_dispatch_method_run against the same words one rsx_dispatch_method at
 * a time -- the same dispatcher afterwards and the same sink calls in order.
 *
 * Then the same comparison for runs through the transform program and
 * constant upload windows, which rsx_dispatch_method_run copies in one go:
 * runs up to 48 words starting before, in and after a window, load pointers
 * at the start, the end and past the end of vp[] and constants[], and ones
 * high enough that the slot arithmetic wraps.
 *
 * Build (standalone):
 *   cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *      libs/video/tests/test_rsx_dispatch_plain.c -o /tmp/test_rsx_dispatch_plain
 */
#include "../rsx_dispatch.c"

static unsigned long long g_sig;
static unsigned g_calls;
static void note(unsigned kind, u32 a, u32 b)
{
    g_calls++;
    g_sig = (g_sig ^ (((unsigned long long)kind << 56) ^ ((unsigned long long)a << 24) ^ b)) * 0x100000001b3ull;
}
static void sk_clear(void* u, const rsx_dispatch* r, u32 mask) { note(1, mask, 0); }
static void sk_begin(void* u, const rsx_dispatch* r, u32 prim) { note(2, prim, 0); }
static void sk_end(void* u, const rsx_dispatch* r) { note(3, 0, 0); }
static void sk_arrays(void* u, const rsx_dispatch* r, u32 f, u32 c) { note(4, f, c); }
static void sk_index(void* u, const rsx_dispatch* r, u32 f, u32 c) { note(5, f, c); }
static void sk_inline(void* u, const rsx_dispatch* r, const u8* d, u32 n) { note(6, n, d[0]); }
static void sk_flip(void* u, const rsx_dispatch* r, u32 arg) { note(7, arg, 0); }
static void sk_immediate(void* u, const rsx_dispatch* r) { note(8, r->regs[0x1C00 >> 2], 0); }

static u32 g_rng = 0x9E3779B9u;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static rsx_dispatch a, b, start;

static void noise(rsx_dispatch* r, int in_begin)
{
    rsx_dispatch_sink sink = { 0 };
    sink.clear = sk_clear; sink.begin = sk_begin; sink.end = sk_end;
    sink.draw_arrays = sk_arrays; sink.draw_index_array = sk_index;
    sink.inline_array = sk_inline; sink.flip = sk_flip; sink.immediate_vertex = sk_immediate;
    rsx_dispatch_init(r, &sink);
    for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++) r->regs[i] = rnd();
    /* Load pointers inside the windows, so an upload has somewhere to land. */
    r->regs[M_VP_UPLOAD_FROM_ID >> 2] = rnd() % 400u;
    r->regs[M_VP_UPLOAD_CONST_ID >> 2] = rnd() % 400u;
    r->in_begin_end = in_begin;
    r->inline_len = in_begin ? 64u : 0u;
}

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    /* 1. Every method, two states. */
    unsigned plain = 0, plain_bad = 0, other_plain = 0;
    for (int in_begin = 0; in_begin < 2; in_begin++) {
        noise(&start, in_begin);
        for (u32 m = 0; m < 0x2000u; m += 4) {
            const u32 arg = start.regs[m >> 2] ^ 0x00010001u;     /* a change */
            a = start;
            g_calls = 0;
            rsx_dispatch_method(&a, m, arg);
            b = start;
            b.seen[m >> 2]++;
            b.regs[m >> 2] = arg;
            const int same = !g_calls && !memcmp(&a, &b, sizeof a);
            if (dsp_method_plain(m)) {
                plain += !in_begin;
                if (!same) {
                    plain_bad++;
                    printf("  method 0x%04X (in BEGIN %d) is not a plain store\n", m, in_begin);
                }
            } else if (same && in_begin) {
                other_plain++;
            }
        }
    }
    printf("  %u methods plain; %u others behaved as plain in these states\n", plain, other_plain);
    check(plain_bad == 0, "every plain method only counts and stores, in and out of BEGIN/END");

    /* 2. Runs against the same words one at a time. */
    static u8 words[16 * 4];
    int run_bad = 0;
    for (int t = 0; t < 20000 && !run_bad; t++) {
        noise(&start, (int)(rnd() & 1u));
        const u32 count = 1u + rnd() % 16u, step = (rnd() % 3u) ? 4u : 0u;
        const u32 method = (rnd() % 0x800u) * 4u;
        for (u32 i = 0; i < count * 4; i++) words[i] = (u8)rnd();
        a = start; g_sig = 0; g_calls = 0;
        rsx_dispatch_method_run(&a, method, step, words, count);
        const unsigned long long sig_a = g_sig; const unsigned calls_a = g_calls;
        b = start; g_sig = 0; g_calls = 0;
        for (u32 i = 0; i < count; i++) {
            const u8* p = words + i * 4;
            rsx_dispatch_method(&b, (method + i * step) & 0x1FFCu,
                                ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
        }
        if (sig_a != g_sig || calls_a != g_calls || memcmp(&a, &b, sizeof a)) {
            printf("  run 0x%04X x%u step %u differs\n", method, count, step);
            run_bad = 1;
        }
    }
    check(!run_bad, "a run leaves the dispatcher and the sink calls as its words one at a time");

    /* 3. Upload runs against the same words one at a time. */
    static u8 upw[48 * 4];
    static const u32 loads[] = { 0, 1, 7, 255, 400, 503, 504, 505, 510, 511, 512, 513, 539, 540,
                                 541, 543, 544, 545, 600, 0x3FFFFFEFu, 0x3FFFFFF0u, 0x3FFFFFFFu,
                                 0x40000000u, 0x40000001u, 0xFFFFFFF7u, 0xFFFFFFF8u, 0xFFFFFFF9u,
                                 0xFFFFFFFEu, 0xFFFFFFFFu };
    const u32 nloads = sizeof loads / sizeof loads[0];
    int up_bad = 0;
    unsigned up_runs = 0;
    for (int t = 0; t < 40000 && !up_bad; t++) {
        noise(&start, (int)(rnd() & 1u));
        start.regs[M_VP_UPLOAD_FROM_ID >> 2] = (rnd() & 1u) ? loads[rnd() % nloads] : rnd() % 560u;
        start.regs[M_VP_UPLOAD_CONST_ID >> 2] = (rnd() & 1u) ? loads[rnd() % nloads] : rnd() % 520u;
        const u32 count = 1u + rnd() % 48u, step = (rnd() % 8u) ? 4u : 0u;
        const u32 window = (rnd() & 1u) ? M_VP_UPLOAD_INST : M_VP_UPLOAD_CONST;
        const u32 method = window - 16u + (rnd() % 52u) * 4u;   /* 4 words before .. 4 after */
        for (u32 i = 0; i < count * 4; i++) upw[i] = (u8)rnd();
        a = start; g_sig = 0; g_calls = 0;
        rsx_dispatch_method_run(&a, method, step, upw, count);
        const unsigned long long sig_a = g_sig; const unsigned calls_a = g_calls;
        b = start; g_sig = 0; g_calls = 0;
        for (u32 i = 0; i < count; i++) {
            const u8* p = upw + i * 4;
            rsx_dispatch_method(&b, (method + i * step) & 0x1FFCu,
                                ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
        }
        up_runs++;
        if (sig_a != g_sig || calls_a != g_calls || memcmp(&a, &b, sizeof a)) {
            printf("  upload run 0x%04X x%u step %u (program load %u, constant load %u) differs\n",
                   method, count, step, start.regs[M_VP_UPLOAD_FROM_ID >> 2],
                   start.regs[M_VP_UPLOAD_CONST_ID >> 2]);
            up_bad = 1;
        }
    }
    printf("  %u upload runs\n", up_runs);
    check(!up_bad, "an upload run leaves the dispatcher as its words one at a time");

    printf("[rsx_dispatch_plain] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
