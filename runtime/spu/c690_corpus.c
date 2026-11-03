/*
 * ps3recomp - corpus capture for the c690 SPU geometry job.
 *
 * Enabled with PS3RECOMP_C690_CORPUS=<dir>. Off by default. See c690_corpus.h
 * for the hook model and docs/edge-native-step1.md for the record format.
 *
 * One file per sampled job, <dir>/job_<index>_spu<id>.c69:
 *
 *   char   magic[8]   "C69CORP1"
 *   u32    version    1
 *   u32    job_index
 *   u32    frame
 *   u32    spu_id
 *   u32    image_id
 *   u32    desc_ea
 *   u64    start_ns   CLOCK_MONOTONIC at job entry
 *
 * followed by records, each
 *
 *   u32 type, u32 size, u64 ea, u32 lsa, u32 aux, <size bytes>
 *
 * Types:
 *   1 JOB_DESC   descriptor bytes from guest EA `ea` (aux = frame)
 *   2 JOB_GPR    all 128 GPRs (aux = pc at entry)
 *   3 GET        bytes read from guest EA `ea` into LS `lsa`
 *   4 PUT        bytes written from LS `lsa` to guest EA `ea`
 *   5 STAGE_IN   stage entry image (aux = stage LSA); payload below
 *   6 STAGE_OUT  stage exit image (aux = stage LSA)
 *   7 OPCODES    256 u32 command counts (aux = 0)
 *   8 JOB_END    no payload; ea = wall ns, aux = total bytes recorded
 *
 * STAGE_IN/OUT payload: u32 ls_size, u32 gpr_size, u32 pc, u32 image, then
 * ls_size bytes of local store and gpr_size bytes of ctx->gpr.
 *
 * Env:
 *   PS3RECOMP_C690_CORPUS=<dir>          enable, capture directory
 *   PS3RECOMP_C690_CORPUS_JOBS=N         max sampled jobs (default 2000)
 *   PS3RECOMP_C690_CORPUS_STAGE_JOBS=N   jobs with stage snapshots (default 20)
 *   PS3RECOMP_C690_CORPUS_TRIGGER=<path> start sampling when this file exists
 */
#include "c690_corpus.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define c690_access _access
#else
#include <sys/stat.h>
#include <unistd.h>
#define c690_access access
#endif

#ifndef F_OK
#define F_OK 0
#endif

extern uint8_t* vm_base;

int g_c690_corpus_on = 0;

#define C690_CORPUS_MAGIC    "C69CORP1"
#define C690_CORPUS_VERSION  1u
#define C690_CORPUS_GPR_SIZE (128u * 16u)
#define C690_CORPUS_DESC_MAX 0x200u
#define C690_CORPUS_REC_HDR  24u

enum {
    C69C_JOB_DESC  = 1,
    C69C_JOB_GPR   = 2,
    C69C_GET       = 3,
    C69C_PUT       = 4,
    C69C_STAGE_IN  = 5,
    C69C_STAGE_OUT = 6,
    C69C_OPCODES   = 7,
    C69C_JOB_END   = 8
};

typedef struct {
    FILE* f;
    int active;
    int stage_done;
    int stage_depth;
    uint32_t job_index;
    uint32_t stage_max;
    uint64_t bytes;
    uint32_t opcode[256];
} c690_corpus_run;

static SPU_THREAD_LOCAL c690_corpus_run t_run;

static int s_inited;
static int s_on;
static char s_dir[1024];
static char s_trigger[1024];
static unsigned s_max_jobs = 2000;
static unsigned s_stage_jobs = 20;
static unsigned s_stride = 1;
static unsigned long long s_job_seq;
static unsigned long long s_captured;
static int s_trigger_seen;
static int s_done_written;

static uint64_t c690_corpus_now_ns(void)
{
#if defined(_WIN32)
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void c690_corpus_init(void)
{
    if (__atomic_exchange_n(&s_inited, 1, __ATOMIC_RELAXED)) return;
    const char* d = getenv("PS3RECOMP_C690_CORPUS");
    if (!d || !*d) return;
    snprintf(s_dir, sizeof s_dir, "%s", d);
    const char* t = getenv("PS3RECOMP_C690_CORPUS_TRIGGER");
    if (t && *t) snprintf(s_trigger, sizeof s_trigger, "%s", t);
    const char* j = getenv("PS3RECOMP_C690_CORPUS_JOBS");
    if (j && *j) { long v = strtol(j, NULL, 0); if (v > 0) s_max_jobs = (unsigned)v; }
    const char* st = getenv("PS3RECOMP_C690_CORPUS_STAGE_JOBS");
    if (st && *st) { long v = strtol(st, NULL, 0); if (v >= 0) s_stage_jobs = (unsigned)v; }
    const char* sd = getenv("PS3RECOMP_C690_CORPUS_STRIDE");
    if (sd && *sd) { long v = strtol(sd, NULL, 0); if (v > 0) s_stride = (unsigned)v; }
    s_on = 1;
    g_c690_corpus_on = 1;
#if defined(_WIN32)
    _mkdir(s_dir);
#else
    mkdir(s_dir, 0777);
#endif
    fprintf(stderr, "[c690-corpus] enabled dir=%s jobs=%u stage_jobs=%u stride=%u trigger=%s\n",
            s_dir, s_max_jobs, s_stage_jobs, s_stride, s_trigger[0] ? s_trigger : "(none)");
    fflush(stderr);
}

static int c690_corpus_triggered(void)
{
    if (!s_trigger[0]) return 1;
    if (__atomic_load_n(&s_trigger_seen, __ATOMIC_RELAXED)) return 1;
    if (c690_access(s_trigger, F_OK) != 0) return 0;
    __atomic_store_n(&s_trigger_seen, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "[c690-corpus] trigger seen; sampling\n");
    fflush(stderr);
    return 1;
}

static void c690_corpus_rec(FILE* f, uint32_t type, uint64_t ea, uint32_t lsa,
                            uint32_t aux, const void* data, uint32_t size)
{
    uint32_t hdr[6];
    hdr[0] = type;
    hdr[1] = size;
    hdr[2] = (uint32_t)ea;
    hdr[3] = (uint32_t)(ea >> 32);
    hdr[4] = lsa;
    hdr[5] = aux;
    fwrite(hdr, sizeof hdr, 1, f);
    if (size && data) fwrite(data, 1, size, f);
    t_run.bytes += C690_CORPUS_REC_HDR + size;
}

static void c690_corpus_image(FILE* f, uint32_t type, uint32_t stage_lsa,
                              const struct spu_context* ctx)
{
    uint32_t meta[4];
    meta[0] = SPU_LS_SIZE;
    meta[1] = C690_CORPUS_GPR_SIZE;
    meta[2] = ctx->pc;
    meta[3] = (uint32_t)ctx->image_id;
    const uint32_t payload = (uint32_t)sizeof meta + SPU_LS_SIZE + C690_CORPUS_GPR_SIZE;
    uint32_t hdr[6] = { type, payload, 0, 0, stage_lsa, stage_lsa };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(meta, sizeof meta, 1, f);
    fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
    fwrite(ctx->gpr, 1, C690_CORPUS_GPR_SIZE, f);
    t_run.bytes += C690_CORPUS_REC_HDR + payload;
}

void c690_corpus_job_begin(const struct spu_context* ctx, uint32_t desc_ea,
                           uint32_t frame)
{
    c690_corpus_init();
    if (!s_on || !ctx || t_run.active) return;
    if (!c690_corpus_triggered()) return;

    const unsigned long long raw = __atomic_fetch_add(&s_job_seq, 1ull, __ATOMIC_RELAXED);
    if (s_stride > 1u && (raw % (unsigned long long)s_stride) != 0) return;
    const unsigned long long seq = __atomic_fetch_add(&s_captured, 1ull, __ATOMIC_RELAXED);
    if (seq >= s_max_jobs) return;

    char path[1200];
    snprintf(path, sizeof path, "%s/job_%06llu_spu%u.c69", s_dir,
             seq, (unsigned)ctx->spu_id);
    FILE* f = fopen(path, "wb");
    if (!f) return;

    char magic[8];
    memcpy(magic, C690_CORPUS_MAGIC, sizeof magic);
    uint32_t hdr[6];
    hdr[0] = C690_CORPUS_VERSION;
    hdr[1] = (uint32_t)seq;
    hdr[2] = frame;
    hdr[3] = (uint32_t)ctx->spu_id;
    hdr[4] = (uint32_t)ctx->image_id;
    hdr[5] = desc_ea;
    uint64_t start = c690_corpus_now_ns();
    fwrite(magic, sizeof magic, 1, f);
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&start, sizeof start, 1, f);

    memset(&t_run, 0, sizeof t_run);
    t_run.f = f;
    t_run.active = 1;
    t_run.job_index = (uint32_t)seq;
    t_run.stage_max = s_stage_jobs;

    c690_corpus_rec(f, C69C_JOB_GPR, 0, 0, ctx->pc, ctx->gpr, C690_CORPUS_GPR_SIZE);
    if (vm_base && desc_ea) {
        c690_corpus_rec(f, C69C_JOB_DESC, desc_ea, 0, frame,
                        vm_base + (uint32_t)desc_ea, C690_CORPUS_DESC_MAX);
    }
}

void c690_corpus_note_op(const struct spu_context* ctx, unsigned op)
{
    if (!g_c690_corpus_on || !t_run.active || op >= 256u) return;
    t_run.opcode[op]++;
}

void c690_corpus_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint64_t ea, uint32_t size, const uint8_t* src)
{
    if (!t_run.active || !t_run.f || size == 0 || !src) return;
    if (size > 0x4000u) return;
    c690_corpus_rec(t_run.f, is_put ? C69C_PUT : C69C_GET, ea,
                    lsa & SPU_LS_MASK, 0, src, size);
}

void c690_corpus_stage_enter(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!g_c690_corpus_on || !t_run.active) return;
    if (t_run.stage_depth++ != 0) return;
    if (t_run.stage_done || t_run.job_index >= t_run.stage_max) return;
    c690_corpus_image(t_run.f, C69C_STAGE_IN, stage_lsa, ctx);
}

void c690_corpus_stage_exit(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!g_c690_corpus_on || !t_run.active) return;
    if (t_run.stage_depth == 0 || --t_run.stage_depth != 0) return;
    if (t_run.stage_done || t_run.job_index >= t_run.stage_max) return;
    c690_corpus_image(t_run.f, C69C_STAGE_OUT, stage_lsa, ctx);
    t_run.stage_done = 1;
}

void c690_corpus_job_end(const struct spu_context* ctx)
{
    (void)ctx;
    if (!t_run.active || !t_run.f) return;
    c690_corpus_rec(t_run.f, C69C_OPCODES, 0, 0, 0,
                    t_run.opcode, (uint32_t)sizeof t_run.opcode);
    const uint64_t wall = c690_corpus_now_ns();
    c690_corpus_rec(t_run.f, C69C_JOB_END, wall, 0, (uint32_t)t_run.bytes, NULL, 0);
    fclose(t_run.f);
    memset(&t_run, 0, sizeof t_run);
    if (!__atomic_load_n(&s_done_written, __ATOMIC_RELAXED) &&
        __atomic_load_n(&s_captured, __ATOMIC_RELAXED) >= s_max_jobs) {
        if (!__atomic_exchange_n(&s_done_written, 1, __ATOMIC_RELAXED)) {
            char path[1200];
            snprintf(path, sizeof path, "%s/DONE", s_dir);
            FILE* d = fopen(path, "w");
            if (d) { fprintf(d, "jobs=%u\n", s_max_jobs); fclose(d); }
            fprintf(stderr, "[c690-corpus] captured %u jobs; DONE; disabling c690 stats/corpus hooks\n", s_max_jobs);
            fflush(stderr);
            /* The capture is complete. Leaving PS3RECOMP_C690_STATS-style
             * clock_gettime accounting on would distort the perf window
             * that follows in the same run. */
            { extern int g_c690_stats; g_c690_stats = 0; }
            __atomic_store_n(&g_c690_corpus_on, 0, __ATOMIC_RELAXED);
        }
    }
}

/* Set g_c690_corpus_on at process start: the dispatch/DMA hooks are gated on
 * it, so it must not depend on the first hook taking the call. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void c690_corpus_ctor(void)
{
    c690_corpus_init();
}
#endif
