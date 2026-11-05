/*
 * Multithreaded ownership test for the PS3RECOMP_C690_GPU_2A binding.
 *
 * The live c690 mesh jobs do not run on lv2 SPU threads: the SPURS PM workers
 * dispatch them through spu_run_spurs_job, which builds a stack-local
 * spu_context with spu_context_init(&ctx, 0). Every lane therefore has
 * spu_id 0 ([c690-stats] worker=N spu=0). This test drives the REAL
 * runtime/spu/c690_gpu_2a.c export/PUT path with exactly that condition:
 * five concurrent "lane" threads, all with spu_id 0, feeding one "walker"
 * thread that drains a queue mirroring vk_c690_sink/vk_c690_prepare's
 * ownership.
 *
 * Input is the captured meter corpus (C69STG01 tail spools; the 0x2A pairs
 * are C69C_TAIL_IN/OUT). With no spool available the test synthesises 32-byte
 * format-A/B jobs so it still runs standalone.
 *
 * Build with ASan+UBSan (catches the double free / use-after-free) and with
 * TSan (catches the shared lane state). See tools/run_c690_gpu_2a_race.sh.
 */
#include "spu_context.h"
#include "c690_gpu_2a.h"
#include "ps3_live_ctl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANES        5
#define MAX_PAIRS    512
#define ITERATIONS   40
#define LS_SIZE      SPU_LS_SIZE
#define GPR_BYTES    (128u * 16u)
#define TAIL_META_WORDS 24
#define TAIL_BITMAP_BYTES ((LS_SIZE >> 4) >> 3)
#define TAIL_PAYLOAD (TAIL_META_WORDS * 4u + TAIL_BITMAP_BYTES + LS_SIZE + GPR_BYTES)
#define MAX_EXPORTS  8

typedef struct {
    uint32_t seq;
    uint8_t in_ls[LS_SIZE];
    uint8_t out_ls[LS_SIZE];
    uint8_t in_gpr[GPR_BYTES];
    uint8_t out_gpr[GPR_BYTES];
} tail_pair;

static tail_pair* s_pairs;
static unsigned s_npairs;

/* ---- queue mirror of vk_c690_sink (the backend owns the real one) -------- */
static pthread_mutex_t s_qmu = PTHREAD_MUTEX_INITIALIZER;
static c690_gpu_2a_job* s_q;
static unsigned s_qn, s_qcap;

static _Atomic unsigned long s_sunk, s_dropped, s_drains, s_reads;
static _Atomic int s_stop;

static void job_free(c690_gpu_2a_job* j)
{
    free(j->offs); free(j->arrays); free(j->seed);
    j->offs = NULL; j->arrays = NULL; j->seed = NULL;
}

/* The real sink also arms the resolver's pending marker; the mirror records
 * the range so the reader below exercises the same pointer lifetime. */
#define MIRROR_RANGES 128
static pthread_mutex_t s_rmu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t ea, size; const uint8_t* bytes; } s_rng[MIRROR_RANGES];
static unsigned s_rng_n;

static int mirror_sink(c690_gpu_2a_job* job)
{
    if (!job) return 0;
    pthread_mutex_lock(&s_qmu);
    if (s_qn == s_qcap) {
        unsigned cap = s_qcap ? s_qcap * 2u : 64u;
        if (cap > 1024u) cap = 1024u;
        c690_gpu_2a_job* p = (c690_gpu_2a_job*)realloc(s_q, cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&s_qmu); return 0; }
        s_q = p; s_qcap = cap;
        if (s_qn == s_qcap) {
            job_free(&s_q[0]);
            memmove(&s_q[0], &s_q[1], (s_qn - 1) * sizeof s_q[0]);
            s_qn--;
            atomic_fetch_add(&s_dropped, 1u);
        }
    }
    s_q[s_qn++] = *job;
    pthread_mutex_unlock(&s_qmu);
    atomic_fetch_add(&s_sunk, 1u);
    return 1;
}

/* ---- drain mirror of vk_c690_prepare ----------------------------------- */
static void mirror_drain(void)
{
    pthread_mutex_lock(&s_rmu);
    s_rng_n = 0;                 /* gpu_range_clear() */
    pthread_mutex_unlock(&s_rmu);

    pthread_mutex_lock(&s_qmu);
    c690_gpu_2a_job* jobs = s_q;
    const unsigned n = s_qn;
    s_q = NULL; s_qn = s_qcap = 0;
    pthread_mutex_unlock(&s_qmu);
    if (!n) { free(jobs); return; }

    /* A persistent host-visible arena, like s_vk.c690_out. The range bytes
     * the reader sees are the arena copy, never the job allocation. */
    static uint8_t* arena;
    static uint32_t arena_cap;
    uint32_t need = 0;
    for (unsigned i = 0; i < n; i++) need += jobs[i].count * 32u + 256u;
    if (need > arena_cap) {
        uint8_t* p = (uint8_t*)realloc(arena, need);
        if (!p) { for (unsigned i = 0; i < n; i++) job_free(&jobs[i]); free(jobs); return; }
        arena = p; arena_cap = need;
    }
    uint32_t at = 0;
    pthread_mutex_lock(&s_rmu);
    for (unsigned i = 0; i < n; i++) {
        c690_gpu_2a_job* j = &jobs[i];
        const uint32_t out_bytes = j->count * 32u;
        at = (at + 255u) & ~255u;
        if (at + out_bytes > arena_cap) { at = 0; }
        memcpy(arena + at, j->seed, out_bytes);
        if (s_rng_n < MIRROR_RANGES) {
            s_rng[s_rng_n].ea = j->ea;
            s_rng[s_rng_n].size = out_bytes;
            s_rng[s_rng_n].bytes = arena + at;
            s_rng_n++;
        }
        at += out_bytes;
        job_free(j);
    }
    pthread_mutex_unlock(&s_rmu);
    atomic_fetch_add(&s_drains, 1u);
    free(jobs);
}

/* ---- corpus ------------------------------------------------------------- */
static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int load_spool_file(const char* path, uint32_t* want_lo, uint32_t* want_hi)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8]; uint32_t ver = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "C69STG01", 8) != 0 ||
        fread(&ver, 4, 1, f) != 1) { fclose(f); return 0; }

    tail_pair pending;
    int have_in = 0;
    while (s_npairs < MAX_PAIRS) {
        uint32_t hdr[6];
        if (fread(hdr, sizeof hdr, 1, f) != 1) break;
        const uint32_t type = hdr[0];
        const uint32_t payload = hdr[1];
        if (payload > TAIL_PAYLOAD + 64u || payload < TAIL_META_WORDS * 4u) break;
        uint8_t* buf = (uint8_t*)malloc(payload);
        if (!buf) break;
        if (fread(buf, 1, payload, f) != payload) { free(buf); break; }
        if (type != 9 && type != 10) { free(buf); continue; }
        if (payload < TAIL_PAYLOAD) { free(buf); continue; }
        const uint32_t* meta = (const uint32_t*)buf;
        const uint32_t op = meta[4];
        const uint32_t seq = meta[7];
        if (op != 0x2Au) { free(buf); continue; }
        const uint8_t* ls = buf + TAIL_META_WORDS * 4u + TAIL_BITMAP_BYTES;
        const uint8_t* gpr = ls + LS_SIZE;
        if (type == 9) {
            pending.seq = seq;
            memcpy(pending.in_ls, ls, LS_SIZE);
            memcpy(pending.in_gpr, gpr, GPR_BYTES);
            have_in = 1;
        } else if (type == 10 && have_in && pending.seq == seq) {
            pending.out_gpr[0] = 0;
            memcpy(pending.out_ls, ls, LS_SIZE);
            memcpy(pending.out_gpr, gpr, GPR_BYTES);
            s_pairs[s_npairs++] = pending;
            have_in = 0;
        }
        free(buf);
    }
    (void)want_lo; (void)want_hi;
    fclose(f);
    return 1;
}

static void load_corpus(void)
{
    const char* dir = getenv("C690_GPU_2A_SPOOL");
    if (!dir || !*dir)
        dir = "../recomp-work/captures/gpu2a/fncap2b2w3";
    char path[1200];
    for (unsigned i = 0; i < 8 && s_npairs < MAX_PAIRS; i++) {
        snprintf(path, sizeof path, "%s/tail_%03u.bin", dir, i);
        load_spool_file(path, NULL, NULL);
    }
}

/* Synthetic fallback: a format-A 32-byte job. */
static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void synth_pair(tail_pair* p, unsigned seed)
{
    memset(p, 0, sizeof *p);
    const uint32_t r3 = 0x1F000u;
    const uint32_t out = 0x2F000u;
    const uint32_t count = 8u + (seed % 24u);
    const uint32_t arr[4] = { 0x20000u, 0x21000u, 0x22000u, 0x23000u };
    const uint32_t list = 0x18000u;
    for (int i = 0; i < 4; i++)
        put_be32(p->in_ls + r3 + 0x260u + (uint32_t)i * 4u, arr[i]);
    put_be32(p->in_ls + r3 + 0xD0u, out);
    put_be32(p->in_ls + r3 + 0x150u, list);
    for (uint32_t k = 0; k < count; k++) {
        const uint16_t o = (uint16_t)(16u + k * 32u);
        p->in_ls[list + 2u * k] = (uint8_t)(o >> 8);
        p->in_ls[list + 2u * k + 1] = (uint8_t)o;
        for (int i = 0; i < 4; i++)
            for (uint32_t b = 0; b < 16u; b++)
                p->in_ls[arr[i] + o + b] =
                    (uint8_t)(0x40 + ((seed + k * 13u + (uint32_t)i * 7u + b) % 64u));
    }
    memcpy(p->out_ls, p->in_ls, LS_SIZE);
    for (uint32_t k = 0; k < count; k++) {
        uint8_t* rec = p->out_ls + out + k * 32u;
        memcpy(rec, p->out_ls + arr[0] + (16u + k * 32u), 12);
        for (int b = 12; b < 32; b++) rec[b] = (uint8_t)(0x10u + k + (uint32_t)b + seed);
    }
    put_be32(p->out_ls + r3 + 0xD0u, out + count * 32u);
    const uint32_t r3v = r3;
    memcpy(p->in_gpr + 3 * 16, &r3v, 4);   /* gpr[3]._u32[0], native lane 0 */
    memcpy(p->out_gpr, p->in_gpr, GPR_BYTES);
}

/* ---- lane threads ------------------------------------------------------- */
typedef struct { unsigned lane; unsigned long errors; } lane_arg;

static void run_one(const tail_pair* p, unsigned idx)
{
    static SPU_THREAD_LOCAL spu_context ctx;
    spu_context_init(&ctx, 0);            /* the live job runner's id */
    ctx.image_id = 6;
    memcpy(ctx.ls, p->in_ls, LS_SIZE);
    memcpy(ctx.gpr, p->in_gpr, GPR_BYTES);

    c690_gpu_2a_enter(&ctx);

    /* the lifted body ran: the OUT snapshot is the handler's exit state */
    memcpy(ctx.ls, p->out_ls, LS_SIZE);
    memcpy(ctx.gpr, p->out_gpr, GPR_BYTES);
    c690_gpu_2a_exit(&ctx);

    /* submit epilogue PUT: LSA is the record cursor, EA a mesh-ring slot. */
    uint32_t r3 = 0;
    memcpy(&r3, p->in_gpr + 3 * 16, 4);
    r3 &= 0x3FFFFu;
    const uint32_t lsa = be32(p->in_ls + ((r3 + 0xD0u) & 0x3FFFFu));
    const uint32_t ea = 0x11800000u + (uint32_t)(idx % 2048u) * 0x40u;
    if (lsa && lsa < LS_SIZE)
        c690_gpu_2a_put(&ctx, lsa, ea, LS_SIZE - lsa);
}

static void* lane_main(void* v)
{
    lane_arg* a = (lane_arg*)v;
    for (unsigned it = 0; it < ITERATIONS; it++) {
        for (unsigned i = a->lane; i < s_npairs; i += LANES)
            run_one(&s_pairs[i], i + it);
        if (a->lane & 1u) sched_yield();
    }
    return NULL;
}

static void* walker_main(void* v)
{
    (void)v;
    for (unsigned it = 0; it < ITERATIONS * 4u; it++) {
        mirror_drain();
        /* Read every installed range while the lanes keep queueing: an
         * install-time free (the old seed fallback) shows up here. */
        pthread_mutex_lock(&s_rmu);
        for (unsigned i = 0; i < s_rng_n; i++) {
            const uint8_t* p = s_rng[i].bytes;
            if (!p) continue;
            volatile uint8_t acc = 0;
            for (uint32_t b = 0; b < s_rng[i].size; b += 16u)
                acc ^= p[b];
            (void)acc;
            atomic_fetch_add(&s_reads, 1u);
        }
        pthread_mutex_unlock(&s_rmu);
        if (atomic_load(&s_stop)) break;
        struct timespec ts = { 0, 200 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void)
{
    setenv("PS3RECOMP_C690_GPU_2A", "1", 1);
    ps3_live_ctl_refresh_env();
    c690_gpu_2a_set_sink(mirror_sink);

    s_pairs = (tail_pair*)calloc(MAX_PAIRS, sizeof *s_pairs);
    if (!s_pairs) return 2;
    load_corpus();
    const int synthetic = !s_npairs;
    if (synthetic)
        for (unsigned i = 0; i < 64; i++) synth_pair(&s_pairs[i], i), s_npairs++;

    fprintf(stderr, "[test-c690-gpu2a-race] %u pairs (%s), %u lanes\n",
            s_npairs, synthetic ? "synthetic" : "captured", LANES);

    pthread_t lanes[LANES], walker;
    lane_arg args[LANES];
    pthread_create(&walker, NULL, walker_main, NULL);
    for (unsigned i = 0; i < LANES; i++) {
        args[i].lane = i; args[i].errors = 0;
        pthread_create(&lanes[i], NULL, lane_main, &args[i]);
    }
    for (unsigned i = 0; i < LANES; i++) pthread_join(lanes[i], NULL);
    atomic_store(&s_stop, 1);
    pthread_join(walker, NULL);
    mirror_drain();     /* final drain */

    pthread_mutex_lock(&s_qmu);
    const unsigned leftover = s_qn;
    pthread_mutex_unlock(&s_qmu);
    fprintf(stderr, "[test-c690-gpu2a-race] sunk=%lu dropped=%lu drains=%lu "
            "reads=%lu leftover=%u\n",
            atomic_load(&s_sunk), atomic_load(&s_dropped),
            atomic_load(&s_drains), atomic_load(&s_reads), leftover);

    int fail = 0;
    if (s_sunk == 0) { fprintf(stderr, "FAIL: no jobs were exported\n"); fail = 1; }
    if (leftover) { fprintf(stderr, "FAIL: %u jobs left queued\n", leftover); fail = 1; }
    if (atomic_load(&s_drains) == 0) { fprintf(stderr, "FAIL: no drains ran\n"); fail = 1; }

    pthread_mutex_lock(&s_rmu);
    s_rng_n = 0;
    pthread_mutex_unlock(&s_rmu);
    free(s_pairs);
    if (fail) return 1;
    printf("c690_gpu_2a race: PASS\n");
    return 0;
}
