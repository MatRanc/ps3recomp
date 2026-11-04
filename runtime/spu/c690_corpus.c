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
 * The job file stays open after the job-end command until the next job on the
 * same lane (and at process exit), so the output PUTs of the submit epilogue
 * are recorded. Stage snapshots are capped on outermost 0xFDA0 entries, not on
 * the job index: >= STAGE_PAIRS pairs spread by STAGE_GAP_MS over meter frames,
 * so every cull layout that runs in the window is represented.
 *
 * Env:
 *   PS3RECOMP_C690_CORPUS=<dir>          enable, capture directory
 *   PS3RECOMP_C690_CORPUS_JOBS=N         max sampled jobs (default 2000)
 *   PS3RECOMP_C690_CORPUS_STAGE_JOBS=N   max 0xFDA0 stage pairs (default 20;
 *                                        the historical name is kept)
 *   PS3RECOMP_C690_CORPUS_STAGE_STRIDE=N capture every Nth outermost entry
 *   PS3RECOMP_C690_CORPUS_STAGE_GAP_MS=N minimum gap between stage pairs
 *                                        (default 0 = every outer entry)
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
    FILE* stage_f;         /* per-lane stage spool, independent of job sampling */
    uint64_t stage_pair_seq;
    int stage_capture;
    int stage_depth;
    FILE* cull_f;          /* 0x25/0x26 and 0x2C cull stage spool */
    uint64_t cull_pair_seq;
    int cull_capture;
    int cull_depth;
    int active;            /* job body running */
    int tail;              /* job body done, file open for the submit epilogue */
    uint32_t job_index;
    uint64_t bytes;
    uint64_t end_ns;
    uint32_t opcode[256];
} c690_corpus_run;

static SPU_THREAD_LOCAL c690_corpus_run t_run;

static int s_inited;
static int s_on;
static char s_dir[1024];
static char s_trigger[1024];
static unsigned s_max_jobs = 2000;
static unsigned s_stage_jobs = 20;
static unsigned s_stage_stride = 1;
static unsigned s_stage_gap_ms = 0;
static unsigned s_cull_jobs = 0;      /* 0 = cull capture off */
static unsigned s_cull_stride = 1;
static unsigned s_cull_gap_ms = 0;
static unsigned s_stride = 1;
static unsigned long long s_job_seq;
static unsigned long long s_captured;
static unsigned long long s_stage_entries;
static unsigned long long s_stage_pairs;
static uint64_t s_stage_last_ns;
static unsigned long long s_stage_files;
static unsigned long long s_cull_entries;
static unsigned long long s_cull_started;
static unsigned long long s_cull_pairs;
static uint64_t s_cull_last_ns;
static unsigned long long s_cull_files;
static int s_trigger_seen;
static int s_done_written;
static int s_stop;

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
extern unsigned cellGcm_flip_request_count(void) __attribute__((weak));
extern unsigned ppu_boot_frames_presented(void) __attribute__((weak));
#endif

static unsigned c690_frame_flips(void)
{
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
    unsigned f = 0;
    if (cellGcm_flip_request_count) f = cellGcm_flip_request_count();
    if (!f && ppu_boot_frames_presented) f = ppu_boot_frames_presented();
    return f;
#else
    return 0;
#endif
}

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
    const char* ss = getenv("PS3RECOMP_C690_CORPUS_STAGE_STRIDE");
    if (ss && *ss) { long v = strtol(ss, NULL, 0); if (v > 0) s_stage_stride = (unsigned)v; }
    const char* sg = getenv("PS3RECOMP_C690_CORPUS_STAGE_GAP_MS");
    if (sg && *sg) { long v = strtol(sg, NULL, 0); if (v >= 0) s_stage_gap_ms = (unsigned)v; }
    const char* sd = getenv("PS3RECOMP_C690_CORPUS_STRIDE");
    if (sd && *sd) { long v = strtol(sd, NULL, 0); if (v > 0) s_stride = (unsigned)v; }
    const char* cj = getenv("PS3RECOMP_C690_CULL_PAIRS");
    if (cj && *cj) { long v = strtol(cj, NULL, 0); if (v > 0) s_cull_jobs = (unsigned)v; }
    const char* cs = getenv("PS3RECOMP_C690_CULL_STRIDE");
    if (cs && *cs) { long v = strtol(cs, NULL, 0); if (v > 0) s_cull_stride = (unsigned)v; }
    const char* cg = getenv("PS3RECOMP_C690_CULL_GAP_MS");
    if (cg && *cg) { long v = strtol(cg, NULL, 0); if (v >= 0) s_cull_gap_ms = (unsigned)v; }
    s_on = 1;
    g_c690_corpus_on = 1;
#if defined(_WIN32)
    _mkdir(s_dir);
#else
    mkdir(s_dir, 0777);
#endif
    fprintf(stderr, "[c690-corpus] enabled dir=%s jobs=%u stage_pairs=%u "
                    "stage_stride=%u stage_gap=%ums cull_pairs=%u "
                    "cull_stride=%u cull_gap=%ums stride=%u trigger=%s\n",
            s_dir, s_max_jobs, s_stage_jobs, s_stage_stride, s_stage_gap_ms,
            s_cull_jobs, s_cull_stride, s_cull_gap_ms, s_stride,
            s_trigger[0] ? s_trigger : "(none)");
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


static void c690_cull_spool_maybe_close(void);

static void c690_corpus_finish_tail(void)
{
    if (!t_run.f) return;
    c690_corpus_rec(t_run.f, C69C_JOB_END, t_run.end_ns, 0,
                    (uint32_t)t_run.bytes, NULL, 0);
    fclose(t_run.f);
    c690_cull_spool_maybe_close();
    /* Keep an open stage/cull spool across the memset: it is per lane and
     * lives across jobs.  The pair sequences continue in the same file. */
    FILE* keep_stage_f = t_run.stage_f;
    FILE* keep_cull_f = t_run.cull_f;
    const uint64_t keep_stage_seq = t_run.stage_pair_seq;
    const uint64_t keep_cull_seq = t_run.cull_pair_seq;
    memset(&t_run, 0, sizeof t_run);
    t_run.stage_f = keep_stage_f;
    t_run.stage_pair_seq = keep_stage_seq;
    t_run.cull_f = keep_cull_f;
    t_run.cull_pair_seq = keep_cull_seq;
    if (__atomic_load_n(&s_captured, __ATOMIC_RELAXED) >= s_max_jobs &&
        !__atomic_load_n(&s_done_written, __ATOMIC_RELAXED) &&
        !__atomic_exchange_n(&s_done_written, 1, __ATOMIC_RELAXED)) {
        char path[1200];
        snprintf(path, sizeof path, "%s/DONE", s_dir);
        FILE* d = fopen(path, "w");
        if (d) {
            fprintf(d, "jobs=%llu stage_pairs=%llu\n",
                    s_captured, __atomic_load_n(&s_stage_pairs, __ATOMIC_RELAXED));
            fclose(d);
        }
        fprintf(stderr, "[c690-corpus] captured %llu jobs, %llu stage pairs; DONE\n",
                s_captured, __atomic_load_n(&s_stage_pairs, __ATOMIC_RELAXED));
        fflush(stderr);
        /* Stop starting new jobs but keep the DMA/stage hooks live so the
         * remaining lanes can finish their open epilogue tails. */
        __atomic_store_n(&s_stop, 1, __ATOMIC_RELAXED);
    }
}

void c690_corpus_job_begin(const struct spu_context* ctx, uint32_t desc_ea,
                           uint32_t frame)
{
    c690_corpus_init();
    if (!s_on || !ctx) return;
    c690_corpus_finish_tail();          /* close this lane's previous epilogue */
    if (t_run.active) return;
    if (!c690_corpus_triggered()) return;
    if (__atomic_load_n(&s_stop, __ATOMIC_RELAXED)) return;

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

    /* The stage and cull spools are per-lane and independent of job sampling;
     * do not close or forget them when a sampled job starts on this lane. */
    FILE* keep_stage_f = t_run.stage_f;
    uint64_t keep_stage_seq = t_run.stage_pair_seq;
    FILE* keep_cull_f = t_run.cull_f;
    uint64_t keep_cull_seq = t_run.cull_pair_seq;
    memset(&t_run, 0, sizeof t_run);
    t_run.f = f;
    t_run.active = 1;
    t_run.tail = 0;
    t_run.stage_capture = 0;
    t_run.stage_depth = 0;
    t_run.stage_f = keep_stage_f;
    t_run.stage_pair_seq = keep_stage_seq;
    t_run.cull_f = keep_cull_f;
    t_run.cull_pair_seq = keep_cull_seq;
    t_run.job_index = (uint32_t)seq;

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
    if (!t_run.f || size == 0 || !src) return;
    if (is_put) {
        /* Body and the submit epilogue that follows the job-end command. */
        if (!t_run.active && !t_run.tail) return;
    } else {
        /* Inputs only while the job body runs. */
        if (!t_run.active || t_run.tail) return;
    }
    if (size > 0x4000u) return;
    c690_corpus_rec(t_run.f, is_put ? C69C_PUT : C69C_GET, ea,
                    lsa & SPU_LS_MASK, 0, src, size);
}

#define C690_STAGE_MAGIC "C69STG01"

static void c690_stage_spool_image(FILE* f, uint32_t type, uint32_t stage_lsa,
                                  uint32_t spu_id, uint64_t pair_frame,
                                  const struct spu_context* ctx)
{
    uint32_t meta[4];
    meta[0] = SPU_LS_SIZE;
    meta[1] = C690_CORPUS_GPR_SIZE;
    meta[2] = ctx->pc;
    meta[3] = (uint32_t)ctx->image_id;
    const uint32_t payload = (uint32_t)sizeof meta + SPU_LS_SIZE + C690_CORPUS_GPR_SIZE;
    uint32_t hdr[6] = { type, payload, (uint32_t)pair_frame,
                        (uint32_t)(pair_frame >> 32), stage_lsa, spu_id };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(meta, sizeof meta, 1, f);
    fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
    fwrite(ctx->gpr, 1, C690_CORPUS_GPR_SIZE, f);
    fflush(f);
}

static void c690_stage_spool_open(void)
{
    if (t_run.stage_f) return;
    const unsigned long long id =
        __atomic_fetch_add(&s_stage_files, 1ull, __ATOMIC_RELAXED);
    char path[1200];
    snprintf(path, sizeof path, "%s/stage_%03llu.bin", s_dir, id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    char magic[8];
    memcpy(magic, C690_STAGE_MAGIC, sizeof magic);
    uint32_t ver = 1;
    fwrite(magic, sizeof magic, 1, f);
    fwrite(&ver, sizeof ver, 1, f);
    t_run.stage_f = f;
}

void c690_corpus_stage_enter(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!s_on || !g_c690_corpus_on || !ctx) return;
    if (!c690_corpus_triggered()) return;
    if (t_run.stage_depth++ != 0) return;
    t_run.stage_capture = 0;
    if (__atomic_load_n(&s_stop, __ATOMIC_RELAXED)) return;
    if (__atomic_load_n(&s_stage_pairs, __ATOMIC_RELAXED) >= s_stage_jobs) return;

    /* Cap on outermost 0xFDA0 entries, not on the job index. The spool is
     * independent of the sampled job files, so every lane and every cull
     * job is eligible even when the job itself is not sampled. */
    const unsigned long long entry =
        __atomic_fetch_add(&s_stage_entries, 1ull, __ATOMIC_RELAXED);
    if (s_stage_stride > 1u && (entry % (unsigned long long)s_stage_stride) != 0)
        return;

    const uint64_t now = c690_corpus_now_ns();
    if (s_stage_gap_ms) {
        const uint64_t last = __atomic_load_n(&s_stage_last_ns, __ATOMIC_RELAXED);
        if (last && now - last < (uint64_t)s_stage_gap_ms * 1000000ull) return;
        __atomic_store_n(&s_stage_last_ns, now, __ATOMIC_RELAXED);
    }
    c690_stage_spool_open();
    if (!t_run.stage_f) return;
    const uint64_t seq = ++t_run.stage_pair_seq;
    const uint64_t frame = (uint64_t)c690_frame_flips();
    t_run.stage_capture = 1;
    c690_stage_spool_image(t_run.stage_f, C69C_STAGE_IN, stage_lsa,
                           (uint32_t)ctx->spu_id, (seq << 32) | (frame & 0xFFFFFFFFu),
                           ctx);
}

void c690_corpus_stage_exit(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!g_c690_corpus_on) return;
    if (t_run.stage_depth == 0 || --t_run.stage_depth != 0) return;
    if (!t_run.stage_capture) return;
    t_run.stage_capture = 0;
    if (!t_run.stage_f) return;
    const uint64_t seq = t_run.stage_pair_seq;
    const uint64_t frame = (uint64_t)c690_frame_flips();
    c690_stage_spool_image(t_run.stage_f, C69C_STAGE_OUT, stage_lsa,
                           (uint32_t)ctx->spu_id, (seq << 32) | (frame & 0xFFFFFFFFu),
                           ctx);
    __atomic_fetch_add(&s_stage_pairs, 1ull, __ATOMIC_RELAXED);
}

/* ---- cull stage spool (0x25/0x26 -> 0x132E0, 0x2C -> 0x12C08) ----------- *
 * Same on-disk pairs as the tree stage, in cull_NNN.bin. The capture is its
 * own trigger (PS3RECOMP_C690_CULL_PAIRS > 0) so it can run without the
 * 0xFDA0 spool; the entry image carries r4 (1 for 0x25, 0 for 0x26) and the
 * stage_lsa distinguishes the two families. */
/* Close this lane's cull spool once the global capture cap is reserved out.
 * The spool is per lane and otherwise lives across jobs; this is the
 * deterministic close, and a lane that has gone idle is closed at its next
 * job boundary. */
static void c690_cull_spool_maybe_close(void)
{
    if (!t_run.cull_f) return;
    if (__atomic_load_n(&s_cull_started, __ATOMIC_RELAXED) < s_cull_jobs) return;
    fclose(t_run.cull_f);
    t_run.cull_f = NULL;
}

static void c690_cull_spool_open(void)
{
    if (t_run.cull_f) return;
    const unsigned long long id =
        __atomic_fetch_add(&s_cull_files, 1ull, __ATOMIC_RELAXED);
    char path[1200];
    snprintf(path, sizeof path, "%s/cull_%03llu.bin", s_dir, id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    char magic[8];
    memcpy(magic, C690_STAGE_MAGIC, sizeof magic);
    uint32_t ver = 1;
    fwrite(magic, sizeof magic, 1, f);
    fwrite(&ver, sizeof ver, 1, f);
    t_run.cull_f = f;
}

void c690_corpus_cull_enter(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!s_on || !g_c690_corpus_on || !ctx || !s_cull_jobs) return;
    if (!c690_corpus_triggered()) return;
    if (t_run.cull_depth++ != 0) return;
    t_run.cull_capture = 0;
    const unsigned long long entry =
        __atomic_fetch_add(&s_cull_entries, 1ull, __ATOMIC_RELAXED);
    if (s_cull_stride > 1u && (entry % (unsigned long long)s_cull_stride) != 0)
        return;
    const uint64_t now = c690_corpus_now_ns();
    if (s_cull_gap_ms) {
        const uint64_t last = __atomic_load_n(&s_cull_last_ns, __ATOMIC_RELAXED);
        if (last && now - last < (uint64_t)s_cull_gap_ms * 1000000ull) return;
        __atomic_store_n(&s_cull_last_ns, now, __ATOMIC_RELAXED);
    }
    /* Reserve the slot at enter, not at exit: several lanes can be in the
     * cull at once, and counting completed pairs let the cap overshoot by the
     * lane count. */
    const unsigned long long slot =
        __atomic_fetch_add(&s_cull_started, 1ull, __ATOMIC_RELAXED);
    if (slot >= s_cull_jobs) return;
    c690_cull_spool_open();
    if (!t_run.cull_f) return;
    const uint64_t seq = ++t_run.cull_pair_seq;
    const uint64_t frame = (uint64_t)c690_frame_flips();
    t_run.cull_capture = 1;
    c690_stage_spool_image(t_run.cull_f, C69C_STAGE_IN, stage_lsa,
                           (uint32_t)ctx->spu_id, (seq << 32) | (frame & 0xFFFFFFFFu),
                           ctx);
}

void c690_corpus_cull_exit(const struct spu_context* ctx, uint32_t stage_lsa)
{
    if (!g_c690_corpus_on) return;
    if (t_run.cull_depth == 0 || --t_run.cull_depth != 0) return;
    if (!t_run.cull_capture) return;
    t_run.cull_capture = 0;
    if (!t_run.cull_f) return;
    const uint64_t seq = t_run.cull_pair_seq;
    const uint64_t frame = (uint64_t)c690_frame_flips();
    c690_stage_spool_image(t_run.cull_f, C69C_STAGE_OUT, stage_lsa,
                           (uint32_t)ctx->spu_id, (seq << 32) | (frame & 0xFFFFFFFFu),
                           ctx);
    __atomic_fetch_add(&s_cull_pairs, 1ull, __ATOMIC_RELAXED);
    c690_cull_spool_maybe_close();
}

void c690_corpus_job_end(const struct spu_context* ctx)
{
    (void)ctx;
    if (!t_run.active || !t_run.f) return;
    c690_corpus_rec(t_run.f, C69C_OPCODES, 0, 0, 0,
                    t_run.opcode, (uint32_t)sizeof t_run.opcode);
    t_run.end_ns = c690_corpus_now_ns();
    /* Keep the file open: the ring/vertex submit epilogue follows the
     * job-end command and its PUTs are part of the job output. It is closed
     * by c690_corpus_finish_tail() at the next job on this lane. */
    t_run.active = 0;
    t_run.tail = 1;
    t_run.stage_depth = 0;
    t_run.stage_capture = 0;
}

/* Set g_c690_corpus_on at process start: the dispatch/DMA hooks are gated on
 * it, so it must not depend on the first hook taking the call. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void c690_corpus_ctor(void)
{
    c690_corpus_init();
}
__attribute__((destructor)) static void c690_corpus_dtor(void)
{
    if (!s_on) return;
    /* The spool files live across jobs; close them here before finish_tail
     * clears t_run. */
    if (t_run.stage_f) { fclose(t_run.stage_f); t_run.stage_f = NULL; }
    if (t_run.cull_f) { fclose(t_run.cull_f); t_run.cull_f = NULL; }
    c690_corpus_finish_tail();
}
#endif
