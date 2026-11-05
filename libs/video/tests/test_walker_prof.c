/*
 * ps3recomp - the walker profiler's drain sampling ([walker-prof])
 *
 * The profiler samples whole drains to keep the per-method clock reads off
 * seven drains in eight. The bug this fixture covers: the sample used to be
 * chosen from the *flip* counter, so a sampled drain could be an empty pass
 * while the window's draws were handled by unsampled drains. The report then
 * printed engine=0.000 next to draws>0 and under-counted the phases. These
 * checks drive the real begin/dispatch/drain-end sequence with synthetic
 * drains and assert that a report never pairs a non-zero draw count with a
 * zero engine time, and that idle windows still report.
 *
 *   gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *       -I libs/video/tests -o /tmp/test_walker_prof libs/video/tests/test_walker_prof.c \
 *       libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c \
 *       libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c \
 *       libs/video/rsx_fp_decompiler.c -lm
 */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static unsigned failures;

static void check(int ok, const char* fmt, ...)
{
    printf("[%s] ", ok ? "PASS" : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    failures += !ok;
}

static void prof_reset(int stride)
{
    s_wprof = 1;
    s_wp2 = 0;
    s_wprof_stride = stride;
    s_wprof_pass = 0;
    s_wprof_drain_seq = 0;
    s_wp_drains_total = s_wp_drains_prof = 0;
    memset(s_wp_ns, 0, sizeof s_wp_ns);
    memset(s_wp_n, 0, sizeof s_wp_n);
    s_wp_drain_ns = s_wp_dispatch_ns = 0;
    s_wp_draws = s_wp_draws_prof = 0;
    s_wp_flips = 0;
    s_wp_frames = 0;
    s_wp_flip = 0;
    s_wp_t0_ms = 0;
    s_wp_last_at_ns = 0;
    s_wp_main_draws = s_wp_mesh_draws = 0;
    s_wp_main_ns = s_wp_mesh_ns = 0;
    s_wp_pipe_create_ns = s_wp_pipe_create_n = 0;
}

/* One FIFO drain as the walker drives it. Draws are counted on every drain
 * (the profiler prints an exact draw count); the dispatch and drain timers
 * only on the sampled ones. */
static void sim_drain(unsigned draws, unsigned long long dispatch_ns,
                      unsigned long long drain_ns)
{
    const int pass = rsx_wprof_drain_begin();
    if (pass) {
        rsx_wprof_note_dispatch(dispatch_ns);
        s_wp_draws_prof += draws;
    }
    s_wp_draws += draws;
    if (pass) rsx_wprof_note_drain(drain_ns);
    rsx_wprof_drain_end(pass);
}

static void sim_flip(void)
{
    rsx_wprof_note_flip();
}

/* --- stderr capture ----------------------------------------------------- */

static int  g_saved_fd = -1;
static FILE* g_cap;
static char g_buf[64 * 1024];

static void cap_begin(void)
{
    fflush(stderr);
    g_saved_fd = dup(STDERR_FILENO);
    g_cap = tmpfile();
    if (!g_cap || g_saved_fd < 0) { failures++; return; }
    dup2(fileno(g_cap), STDERR_FILENO);
}

static const char* cap_end(void)
{
    fflush(stderr);
    dup2(g_saved_fd, STDERR_FILENO);
    close(g_saved_fd);
    g_saved_fd = -1;
    fseek(g_cap, 0, SEEK_SET);
    size_t n = fread(g_buf, 1, sizeof g_buf - 1, g_cap);
    g_buf[n] = 0;
    fclose(g_cap);
    g_cap = NULL;
    return g_buf;
}

/* Count reports, and reports with draws>0 while engine==0. */
static void parse_reports(const char* buf, int* lines, int* bad, double* draws_max,
                          double* engine_max)
{
    *lines = *bad = 0;
    *draws_max = *engine_max = 0.0;
    const char* p = buf;
    while ((p = strstr(p, "[walker-prof]"))) {
        const char* eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        double draws = -1.0, engine = -1.0;
        const char* d = strstr(p, "draws=");
        const char* e = strstr(p, "engine=");
        if (d && d < eol) draws = atof(d + 6);
        if (e && e < eol) engine = atof(e + 7);
        if (draws >= 0.0 && engine >= 0.0) {
            (*lines)++;
            if (draws > 0.0 && engine == 0.0) (*bad)++;
            if (draws > *draws_max) *draws_max = draws;
            if (engine > *engine_max) *engine_max = engine;
        }
        p = eol;
    }
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    /* 1. The reported failure: after a clean report, the next sampled drain
     *    is empty while the window's draws were handled by unsampled drains.
     *    The report must wait for a sampled drain with draws instead of
     *    printing engine=0 with draws>0. */
    {
        prof_reset(4);
        cap_begin();
        sim_flip();
        sim_drain(10, 1000, 2000);               /* seq 0, sampled -> report */
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(10, 1000, 2000);               /* seq 4, sampled -> report */
        sim_drain(10, 1000, 2000); sim_flip();    /* seq 5..7: unsampled draws */
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(0, 0, 10);                     /* seq 8, sampled, empty: defer */
        sim_drain(10, 1000, 2000); sim_flip();
        sim_drain(10, 1000, 2000);
        sim_drain(10, 1000, 2000);
        sim_drain(5, 500, 1000);                 /* seq 12, sampled, has draws */
        const char* buf = cap_end();
        int lines, bad; double dmax, emax;
        parse_reports(buf, &lines, &bad, &dmax, &emax);
        check(lines == 3, "empty sampled drain defers; a later sampled drain reports (%d)", lines);
        check(bad == 0, "no report pairs draws>0 with engine=0 (%d bad)", bad);
        check(dmax > 0.0 && emax > 0.0, "the emitted reports carry draws and engine time (%.1f, %.3f)",
              dmax, emax);
    }

    /* 2. An idle window must still report (draws=0, engine=0 is consistent). */
    {
        prof_reset(4);
        cap_begin();
        for (int i = 0; i < 5; i++) { sim_drain(0, 0, 0); sim_flip(); }
        const char* buf = cap_end();
        int lines, bad; double dmax, emax;
        parse_reports(buf, &lines, &bad, &dmax, &emax);
        check(lines == 1, "an idle sampled drain still reports (%d)", lines);
        check(bad == 0, "an idle line is not counted as a bad pair (%d)", bad);
        check(dmax == 0.0 && emax == 0.0, "the idle line has draws=0 and engine=0");
    }

    /* 3. Sampling is per drain: with stride 2 every second drain reports,
     *    independently of how many flips land in between. */
    {
        prof_reset(2);
        cap_begin();
        sim_flip();                       /* the first report needs a boundary */
        for (int i = 0; i < 6; i++) {
            sim_drain(7, 700, 1400);
            sim_flip();
        }
        const char* buf = cap_end();
        int lines, bad; double dmax, emax;
        parse_reports(buf, &lines, &bad, &dmax, &emax);
        check(lines == 3, "stride 2 over six drains reports three times (%d)", lines);
        check(bad == 0, "every stride-2 report keeps engine and draws consistent (%d)", bad);
        check(emax > 0.0 && dmax > 0.0, "stride-2 reports carry engine time and draws");
    }

    /* 4. A longer mixed sequence: zero-draw and draw-bearing drains, flips on
     *    a different rhythm. The invariant must hold for every line. */
    {
        prof_reset(8);
        cap_begin();
        unsigned seed = 12345;
        for (int i = 0; i < 200; i++) {
            seed = seed * 1664525u + 1013904223u;
            const unsigned draws = (seed >> 28) < 2 ? 0u : 25u;
            sim_drain(draws, draws ? draws * 100ull : 0ull, draws ? draws * 200ull : 0ull);
            if ((seed & 3u) == 0) sim_flip();
        }
        const char* buf = cap_end();
        int lines, bad; double dmax, emax;
        parse_reports(buf, &lines, &bad, &dmax, &emax);
        check(lines > 0, "the mixed sequence reports (%d)", lines);
        check(bad == 0, "200 mixed drains never print draws>0 with engine=0 (%d)", bad);
    }

    free(vm_base);
    printf("[walker_prof] %s (%u failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
