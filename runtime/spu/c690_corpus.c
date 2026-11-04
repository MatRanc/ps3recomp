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
    C69C_JOB_END   = 8,
    /* PS3RECOMP_C690_TAIL_CAPTURE: command-boundary stage pairs. IN is the
     * local store + GPRs at the handler entry, OUT is the same at the next
     * command handler (or the job end). The payload adds a tail meta block
     * and a 16-byte-line write bitmap in front of the usual meta/LS/GPRs. */
    C69C_TAIL_IN   = 9,
    C69C_TAIL_OUT  = 10
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

/* ---- PS3RECOMP_C690_TAIL_CAPTURE --------------------------------------- *
 * Stage-level corpus for the GPU geometric tail. Off by default and separate
 * from PS3RECOMP_C690_CORPUS so it can be enabled for one meter run without
 * turning on the per-job DMA capture. It is driven from c690_stats'
 * command-boundary hooks (indirect dispatch + SPU_DIRECT), so it needs no
 * edit to the generated lifted module and it captures the whole command,
 * including direct-call bodies such as the 0xBB80 path. */
#define C690_TAIL_KEYS         64
#define C690_TAIL_META_WORDS   24
#define C690_TAIL_LINE_SHIFT   4
#define C690_TAIL_LINES        (SPU_LS_SIZE >> C690_TAIL_LINE_SHIFT)
#define C690_TAIL_BITMAP_BYTES (C690_TAIL_LINES >> 3)
#define C690_TAIL_UNKNOWN_KEY  0xFFFFFFFFu

typedef struct {
    uint32_t key;
    uint32_t used;
    unsigned long long calls;
    unsigned long long ns;
    unsigned long long pairs;
} c690_tail_key;

static c690_tail_key s_tail_keys[C690_TAIL_KEYS];

typedef struct {
    int active;
    int sampled;           /* snapshot taken; only sampled commands are kept */
    int op;
    uint32_t stage_lsa;
    uint32_t entry_pc;
    uint32_t key;
    uint32_t key_lsa;
    uint32_t seq;
    uint32_t gpr3, gpr4, gpr5, gpr6;
    uint32_t cmd[8];
    uint32_t ret_pc;       /* 0x2A: caller link; close only on the real return */
    uint64_t entry_ns;
    uint8_t in_ls[SPU_LS_SIZE];
    uint8_t in_gpr[C690_CORPUS_GPR_SIZE];
    uint8_t bitmap[C690_TAIL_BITMAP_BYTES];
} c690_tail_run;

static SPU_THREAD_LOCAL c690_tail_run* t_tail;
static SPU_THREAD_LOCAL FILE* t_tail_f;
int g_c690_tail_on = 0;
static int s_tail_on;
static char s_tail_dir[1024];
static char s_tail_trigger[1024];
static int s_tail_trigger_seen;
static uint64_t s_tail_trigger_last_ns;
static uint64_t s_tail_hist_last_ns;
static unsigned s_tail_pairs = 8;
static unsigned s_tail_stride = 1;
static unsigned long long s_tail_seq;
static unsigned long long s_tail_files;
static unsigned long long s_tail_entries;
static unsigned long long s_tail_pairs_written;
static unsigned long long s_tail_snapshots;
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

    /* Tail capture is its own trigger; it does not require the per-job corpus
     * or the DMA spool. */
    const char* td = getenv("PS3RECOMP_C690_TAIL_CAPTURE");
    if (td && *td) {
        snprintf(s_tail_dir, sizeof s_tail_dir, "%s", td);
        const char* tn = getenv("PS3RECOMP_C690_TAIL_PAIRS");
        if (tn && *tn) { long v = strtol(tn, NULL, 0); if (v > 0) s_tail_pairs = (unsigned)v; }
        const char* ts = getenv("PS3RECOMP_C690_TAIL_STRIDE");
        if (ts && *ts) { long v = strtol(ts, NULL, 0); if (v > 0) s_tail_stride = (unsigned)v; }
        const char* tt = getenv("PS3RECOMP_C690_TAIL_TRIGGER");
        if (tt && *tt) snprintf(s_tail_trigger, sizeof s_tail_trigger, "%s", tt);
        s_tail_on = 1;
        g_c690_tail_on = 1;
#if defined(_WIN32)
        _mkdir(s_tail_dir);
#else
        mkdir(s_tail_dir, 0777);
#endif
        fprintf(stderr, "[c690-tail] enabled dir=%s pairs_per_key=%u stride=%u\n",
                s_tail_dir, s_tail_pairs, s_tail_stride);
        fflush(stderr);
    }

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

/* ---- tail-stage capture (PS3RECOMP_C690_TAIL_CAPTURE) ------------------- */

static uint32_t c690_tail_be32(const uint8_t* ls, uint32_t off)
{
    const uint32_t a = off & SPU_LS_MASK & ~3u;
    return ((uint32_t)ls[a] << 24) | ((uint32_t)ls[a + 1] << 16) |
           ((uint32_t)ls[a + 2] << 8) | (uint32_t)ls[a + 3];
}

static int c690_tail_is_stage_op(int op)
{
    /* Every tail command is captured by its own function-entry/exit wrapper,
     * not at the command boundary. The parser can reach a handler through a
     * direct known-target dispatch the resolver never sees, and the boundary
     * OUT also carries parser bookkeeping (the +2 count word at 0x1F0F0), so
     * a boundary pair is not always one command's own input/output. */
    (void)op;
    return 0;
}

/* The 0x13080 sub-selector targets. r6 at the target entry carries the
 * selector in the low three bits and the A730/BB80 format bit in bit 4. */
static int c690_tail_sel_lsa(uint32_t lsa)
{
    return lsa == 0x13178u || lsa == 0x13140u ||
           lsa == 0x131A0u || lsa == 0x131F8u;
}

static c690_tail_key* c690_tail_key_slot(uint32_t key, int create)
{
    for (;;) {
        for (unsigned i = 0; i < C690_TAIL_KEYS; i++) {
            if (__atomic_load_n(&s_tail_keys[i].used, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&s_tail_keys[i].key, __ATOMIC_RELAXED) == key)
                return &s_tail_keys[i];
        }
        if (!create) return NULL;
        for (unsigned i = 0; i < C690_TAIL_KEYS; i++) {
            uint32_t zero = 0;
            if (__atomic_load_n(&s_tail_keys[i].used, __ATOMIC_RELAXED))
                continue;
            if (__atomic_compare_exchange_n(&s_tail_keys[i].used, &zero, 1u, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                __atomic_store_n(&s_tail_keys[i].key, key, __ATOMIC_RELAXED);
                return &s_tail_keys[i];
            }
        }
        return NULL;   /* key table full: count in the caller's fallback */
    }
}

static FILE* c690_tail_spool_open(void)
{
    if (t_tail_f) return t_tail_f;
    const unsigned long long id =
        __atomic_fetch_add(&s_tail_files, 1ull, __ATOMIC_RELAXED);
    char path[1200];
    snprintf(path, sizeof path, "%s/tail_%03llu.bin", s_tail_dir, id);
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    char magic[8];
    memcpy(magic, C690_STAGE_MAGIC, sizeof magic);
    uint32_t ver = 1;
    fwrite(magic, sizeof magic, 1, f);
    fwrite(&ver, sizeof ver, 1, f);
    t_tail_f = f;
    return f;
}

static void c690_tail_write_image(FILE* f, uint32_t type,
                                  const c690_tail_run* s,
                                  const struct spu_context* ctx, uint64_t now)
{
    uint32_t meta[C690_TAIL_META_WORDS];
    memset(meta, 0, sizeof meta);
    meta[0] = SPU_LS_SIZE;
    meta[1] = C690_CORPUS_GPR_SIZE;
    meta[2] = (type == C69C_TAIL_IN) ? s->entry_pc : ctx->pc;
    meta[3] = (uint32_t)ctx->image_id;
    meta[4] = (uint32_t)s->op;
    meta[5] = s->key;
    meta[6] = s->key_lsa;
    meta[7] = s->seq;
    meta[8] = s->gpr3;
    meta[9] = s->gpr4;
    meta[10] = s->gpr5;
    meta[11] = s->gpr6;
    for (int i = 0; i < 8; i++) meta[12 + i] = s->cmd[i];
    meta[20] = (uint32_t)s->entry_ns;
    meta[21] = (uint32_t)(s->entry_ns >> 32);
    meta[22] = (uint32_t)now;
    meta[23] = (uint32_t)(now >> 32);

    const uint32_t payload = (uint32_t)sizeof meta + C690_TAIL_BITMAP_BYTES +
                             SPU_LS_SIZE + C690_CORPUS_GPR_SIZE;
    const uint32_t frame = c690_frame_flips();
    const uint64_t ea = ((uint64_t)s->seq << 32) | (frame & 0xFFFFFFFFu);
    uint32_t hdr[6] = { type, payload, (uint32_t)ea, (uint32_t)(ea >> 32),
                        s->stage_lsa, (uint32_t)ctx->spu_id };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(meta, sizeof meta, 1, f);
    fwrite(s->bitmap, 1, C690_TAIL_BITMAP_BYTES, f);
    const uint8_t* img_ls = (type == C69C_TAIL_IN) ? s->in_ls : ctx->ls;
    const uint8_t* img_gpr = (type == C69C_TAIL_IN) ? s->in_gpr
                                                    : (const uint8_t*)ctx->gpr;
    fwrite(img_ls, 1, SPU_LS_SIZE, f);
    fwrite(img_gpr, 1, C690_CORPUS_GPR_SIZE, f);
}

/* A trigger file (PS3RECOMP_C690_TAIL_TRIGGER) turns the capture on when it
 * appears, so a run can start collecting only once the pitch meter is up.
 * Checked at most every 200 ms; once seen it stays armed. */
static int c690_tail_armed(uint64_t now)
{
    if (!s_tail_trigger[0]) return 1;
    if (__atomic_load_n(&s_tail_trigger_seen, __ATOMIC_RELAXED)) return 1;
    const uint64_t last = __atomic_load_n(&s_tail_trigger_last_ns, __ATOMIC_RELAXED);
    if (last && now - last < 200000000ull) return 0;
    __atomic_store_n(&s_tail_trigger_last_ns, now, __ATOMIC_RELAXED);
    if (c690_access(s_tail_trigger, F_OK) != 0) return 0;
    __atomic_store_n(&s_tail_trigger_seen, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "[c690-tail] trigger seen; sampling\n");
    fflush(stderr);
    return 1;
}

static void c690_tail_write_hist(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/tail_hist.txt", s_tail_dir);
    FILE* f = fopen(path, "w");
    unsigned long long calls = 0, ns = 0;
    for (unsigned i = 0; i < C690_TAIL_KEYS; i++) {
        if (!__atomic_load_n(&s_tail_keys[i].used, __ATOMIC_RELAXED)) continue;
        calls += __atomic_load_n(&s_tail_keys[i].calls, __ATOMIC_RELAXED);
        ns += __atomic_load_n(&s_tail_keys[i].ns, __ATOMIC_RELAXED);
    }
    if (f) fprintf(f, "key calls ns pairs\n");
    for (unsigned i = 0; i < C690_TAIL_KEYS; i++) {
        if (!__atomic_load_n(&s_tail_keys[i].used, __ATOMIC_RELAXED)) continue;
        const uint32_t key = __atomic_load_n(&s_tail_keys[i].key, __ATOMIC_RELAXED);
        const unsigned long long c =
            __atomic_load_n(&s_tail_keys[i].calls, __ATOMIC_RELAXED);
        const unsigned long long n =
            __atomic_load_n(&s_tail_keys[i].ns, __ATOMIC_RELAXED);
        const unsigned long long p =
            __atomic_load_n(&s_tail_keys[i].pairs, __ATOMIC_RELAXED);
        if (f)
            fprintf(f, "0x%08X %llu %llu %llu\n", key, c, n, p);
    }
    if (f) fclose(f);
    if (calls)
        fprintf(stderr, "[c690-tail] hist pairs=%llu snapshots=%llu "
                        "calls=%llu time=%.3fms\n",
                s_tail_pairs_written, s_tail_snapshots, calls,
                (double)ns / 1e6);
    fflush(stderr);
}

static void c690_tail_maybe_hist(uint64_t now)
{
    const uint64_t last = __atomic_load_n(&s_tail_hist_last_ns, __ATOMIC_RELAXED);
    if (last && now - last < 2000000000ull) return;
    if (__atomic_exchange_n(&s_tail_hist_last_ns, now, __ATOMIC_RELAXED) == last)
        c690_tail_write_hist();
}

static void c690_tail_close(const struct spu_context* ctx, uint64_t now)
{
    c690_tail_run* s = t_tail;
    if (!s || !s->active) return;
    s->active = 0;

    const uint32_t key = s->key;
    c690_tail_key* k = c690_tail_key_slot(key, 1);
    if (k) {
        __atomic_fetch_add(&k->calls, 1ull, __ATOMIC_RELAXED);
        if (now > s->entry_ns)
            __atomic_fetch_add(&k->ns, now - s->entry_ns, __ATOMIC_RELAXED);
    }
    c690_tail_maybe_hist(now);
    if (s->sampled != 1) return;

    unsigned long long pairs = k ? __atomic_load_n(&k->pairs, __ATOMIC_RELAXED)
                                 : 0ull;
    if (!k || pairs >= s_tail_pairs) return;
    __atomic_fetch_add(&k->pairs, 1ull, __ATOMIC_RELAXED);

    memset(s->bitmap, 0, sizeof s->bitmap);
    for (uint32_t line = 0; line < C690_TAIL_LINES; line++) {
        const uint8_t* a = s->in_ls + (line << C690_TAIL_LINE_SHIFT);
        const uint8_t* b = ctx->ls + (line << C690_TAIL_LINE_SHIFT);
        if (memcmp(a, b, 1u << C690_TAIL_LINE_SHIFT) != 0)
            s->bitmap[line >> 3] |= (uint8_t)(1u << (line & 7u));
    }
    FILE* f = c690_tail_spool_open();
    if (!f) return;
    c690_tail_write_image(f, C69C_TAIL_IN, s, ctx, now);
    c690_tail_write_image(f, C69C_TAIL_OUT, s, ctx, now);
    fflush(f);
    __atomic_fetch_add(&s_tail_pairs_written, 1ull, __ATOMIC_RELAXED);
}

/* True while this format key still needs a pair. The key slot is created so a
 * key that is never sampled is still counted for the histogram; the cap check
 * itself only reads the pair counter, before any LS snapshot. */
static int c690_tail_key_below_cap(uint32_t key)
{
    c690_tail_key* k = c690_tail_key_slot(key, 1);
    if (!k) return 0;
    return __atomic_load_n(&k->pairs, __ATOMIC_RELAXED) < s_tail_pairs;
}

static void c690_tail_snapshot(const struct spu_context* ctx, c690_tail_run* s)
{
    for (int i = 0; i < 8; i++)
        s->cmd[i] = c690_tail_be32(ctx->ls, s->gpr3 + 0x150u + (uint32_t)i * 4u);
    memcpy(s->in_ls, ctx->ls, SPU_LS_SIZE);
    memcpy(s->in_gpr, ctx->gpr, C690_CORPUS_GPR_SIZE);
    s->sampled = 1;
    __atomic_fetch_add(&s_tail_snapshots, 1ull, __ATOMIC_RELAXED);
}

/* `key` is the format key when it is already known at entry (every wrapper
 * passes it) and C690_TAIL_UNKNOWN_KEY for a command-boundary 0x21, whose key
 * only appears when the sub-selector body is entered. In that last case the
 * snapshot is deferred to that entry, so the cap is still checked first. */
static void c690_tail_open(const struct spu_context* ctx, uint32_t lsa,
                           int op, uint64_t now, uint32_t key)
{
    if (!t_tail) {
        t_tail = (c690_tail_run*)calloc(1, sizeof *t_tail);
        if (!t_tail) return;
    }
    c690_tail_run* s = t_tail;
    s->active = 1;
    s->op = op;
    s->stage_lsa = lsa;
    s->entry_pc = ctx->pc;
    s->entry_ns = now;
    s->seq = (uint32_t)__atomic_fetch_add(&s_tail_seq, 1ull, __ATOMIC_RELAXED);
    s->gpr3 = ctx->gpr[3]._u32[0];
    s->gpr4 = ctx->gpr[4]._u32[0];
    s->gpr5 = ctx->gpr[5]._u32[0];
    s->gpr6 = ctx->gpr[6]._u32[0];
    s->ret_pc = (op == 0x2A) ? (ctx->gpr[0]._u32[0] & SPU_LS_MASK) : 0;

    s->key = key;
    s->key_lsa = (key != C690_TAIL_UNKNOWN_KEY && op == 0x21) ? lsa : 0;
    s->sampled = 0;

    const unsigned long long entry =
        __atomic_fetch_add(&s_tail_entries, 1ull, __ATOMIC_RELAXED);
    if (s_tail_stride > 1u &&
        (entry % (unsigned long long)s_tail_stride) != 0)
        return;
    if (key == C690_TAIL_UNKNOWN_KEY) {
        s->sampled = 2;             /* snapshot when the selector key is known */
        return;
    }
    if (!c690_tail_key_below_cap(key)) return;
    c690_tail_snapshot(ctx, s);
}

void c690_corpus_tail_job_begin(const struct spu_context* ctx)
{
    if (!g_c690_tail_on) return;
    if (!c690_tail_armed(c690_corpus_now_ns())) return;
    if (t_tail && t_tail->active)
        c690_tail_close(ctx, c690_corpus_now_ns());
}

void c690_corpus_tail_trace(const struct spu_context* ctx, uint32_t lsa,
                            int op, uint64_t now)
{
    if (!g_c690_tail_on || !ctx) return;
    if (!c690_tail_armed(now)) {
        if (t_tail && t_tail->active) t_tail->active = 0;
        return;
    }
    if (op >= 0) {
        /* Top-level command boundary: the previous command has finished. */
        if (t_tail && t_tail->active) c690_tail_close(ctx, now);
        if (c690_tail_is_stage_op(op)) {
            const uint32_t key = (op == 0x21) ? C690_TAIL_UNKNOWN_KEY
                                              : (uint32_t)op;
            c690_tail_open(ctx, lsa, op, now, key);
        }
        return;
    }
    if (!t_tail || !t_tail->active) return;
    if (t_tail->op == 0x21 && c690_tail_sel_lsa(lsa)) {
        const uint32_t g6 = ctx->gpr[6]._u32[0];
        const uint32_t key = (g6 & 0x1Fu) | ((lsa & 0xFFFFFu) << 8);
        t_tail->key = key;
        t_tail->key_lsa = lsa;
        if (t_tail->sampled == 2) {
            if (c690_tail_key_below_cap(key))
                c690_tail_snapshot(ctx, t_tail);
            else
                t_tail->sampled = 0;
        }
    }
}

/* Tail commands are wrapped at the function body so each pair is exactly one
 * command, even when the entry is a direct known-target dispatch the resolver
 * cannot see. The entry closes any open command (its true exit is this entry).
 * The exit closes the pair only when the handler really returned to its
 * caller: an interrupt trampoline return resumes inside the body, so the pair
 * stays open and the next command boundary closes it at the real exit. */
void c690_corpus_tail_cmd_enter(const struct spu_context* ctx, uint32_t lsa,
                                uint32_t op)
{
    if (!g_c690_tail_on || !ctx) return;
    const uint64_t now = c690_corpus_now_ns();
    if (!c690_tail_armed(now)) return;
    if (t_tail && t_tail->active) c690_tail_close(ctx, now);
    /* The wrapper knows the format key before the snapshot: the 0x21 key is
     * (sub-selector LSA << 8) | (r6 & 0x1f); everything else keys on its
     * opcode. Passing it in lets the pair cap be checked first. */
    uint32_t key = (uint32_t)op;
    if (op == 0x21) {
        const uint32_t g6 = ctx->gpr[6]._u32[0];
        key = (g6 & 0x1Fu) | ((lsa & 0xFFFFFu) << 8);
    }
    c690_tail_open(ctx, lsa, op, now, key);
}

void c690_corpus_tail_cmd_exit(const struct spu_context* ctx)
{
    if (!g_c690_tail_on || !ctx) return;
    if (!t_tail || !t_tail->active) return;
    if (t_tail->ret_pc && (ctx->pc & SPU_LS_MASK) != t_tail->ret_pc) return;
    c690_tail_close(ctx, c690_corpus_now_ns());
}

/* 0x2A (LS 0x10F78) function entry/exit. */
void c690_corpus_tail_2a_enter(const struct spu_context* ctx)
{
    c690_corpus_tail_cmd_enter(ctx, 0x10F78u, 0x2Au);
}

void c690_corpus_tail_2a_exit(const struct spu_context* ctx)
{
    if (!g_c690_tail_on || !ctx) return;
    if (!t_tail || !t_tail->active || t_tail->op != 0x2A) return;
    c690_corpus_tail_cmd_exit(ctx);
}

static void c690_tail_report(void)
{
    if (!s_tail_on) return;
    c690_tail_write_hist();
    for (unsigned i = 0; i < C690_TAIL_KEYS; i++) {
        if (!__atomic_load_n(&s_tail_keys[i].used, __ATOMIC_RELAXED)) continue;
        const uint32_t key = __atomic_load_n(&s_tail_keys[i].key, __ATOMIC_RELAXED);
        const unsigned long long c =
            __atomic_load_n(&s_tail_keys[i].calls, __ATOMIC_RELAXED);
        const unsigned long long n =
            __atomic_load_n(&s_tail_keys[i].ns, __ATOMIC_RELAXED);
        const unsigned long long p =
            __atomic_load_n(&s_tail_keys[i].pairs, __ATOMIC_RELAXED);
        fprintf(stderr, "[c690-tail] key=0x%08X calls=%llu time=%.3fms "
                        "pairs=%llu\n", key, c, (double)n / 1e6, p);
    }
    if (t_tail_f) { fclose(t_tail_f); t_tail_f = NULL; }
    if (t_tail) { free(t_tail); t_tail = NULL; }
    fflush(stderr);
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
    if (!s_on) { c690_tail_report(); return; }
    /* The spool files live across jobs; close them here before finish_tail
     * clears t_run. */
    if (t_run.stage_f) { fclose(t_run.stage_f); t_run.stage_f = NULL; }
    if (t_run.cull_f) { fclose(t_run.cull_f); t_run.cull_f = NULL; }
    c690_corpus_finish_tail();
    c690_tail_report();
}
#endif
