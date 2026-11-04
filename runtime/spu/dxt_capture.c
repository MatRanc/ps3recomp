/*
 * dxt_capture.c — corpus capture for the WWS DXT job (SPURS workload 0).
 *
 * Enabled with PS3RECOMP_DXT_CAPTURE=<dir>. Off by default: no allocation, no
 * file, no code path change when the variable is unset. Only the policy-module
 * dispatches for wid 0 (Wws_Job/DXTJob) arm the per-thread DMA spool, so the
 * capture never sees the mesh/next-synth/MP3 workloads.
 *
 * One file per SPURS lane: <dir>/dxt_lane<lane>_<n>.bin
 *
 *   char   magic[8]   "DXTCP01\0"
 *   u32    version    1
 *   u32    lane
 *   u32    reserved
 *   u64    start_ns   CLOCK_MONOTONIC at first capture on this lane
 *
 * followed by records, each
 *
 *   u32 type, u32 size, u64 ea, u32 lsa, u32 aux, <size bytes>
 *
 * Types:
 *   1 RUN_BEGIN  policy dispatch entered (wid 0); payload is dxt_run_begin
 *   2 GET        bytes read from guest EA `ea` into LS `lsa`
 *   3 PUT        bytes written from LS `lsa` to guest EA `ea`
 *   4 RUN_END    policy dispatch returned; payload is dxt_run_end
 *   5 CODE       a DMA GET whose payload is a WWS job module (C0DEC0DE header);
 *                ea/size are the code DMA source, lsa the code buffer base,
 *                aux the matched lifted image id (0 = no lifted module), and
 *                the payload is the first 0x40 bytes of the module.
 *   6 PARAM      a 0x30-byte DXT parameter block found inside a GET payload;
 *                ea is the guest address of the block, lsa its local-store
 *                address, payload is the 0x30 bytes.
 *
 * The GET/PUT payloads are the raw transfer (guest byte order); the offline
 * report reconstructs the job parameters, the input/output block pairs and
 * the code-module identity from them.
 *
 * Env:
 *   PS3RECOMP_DXT_CAPTURE=<dir>          enable, capture directory
 *   PS3RECOMP_DXT_CAPTURE_RUNS=N         max sampled wid-0 dispatches (4000)
 *   PS3RECOMP_DXT_CAPTURE_STRIDE=N       capture every Nth dispatch (1)
 *   PS3RECOMP_DXT_CAPTURE_MAX_MB=N       total spool cap in MiB (180)
 *   PS3RECOMP_DXT_CAPTURE_TRIGGER=<path> start when this file exists
 */
#include "dxt_capture.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define dxt_access _access
#define dxt_mkdir(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <unistd.h>
#define dxt_access access
#define dxt_mkdir(d) mkdir((d), 0777)
#endif

#ifndef F_OK
#define F_OK 0
#endif

int g_dxt_capture_on = 0;

#define DXT_CAPTURE_MAGIC    "DXTCP01"
#define DXT_CAPTURE_VERSION  1u
#define DXT_CAPTURE_REC_HDR  24u
#define DXT_CAPTURE_BEGIN_LS 0x120u
#define DXT_CAPTURE_CODE_PFX 0x40u

enum {
    DXTC_RUN_BEGIN = 1,
    DXTC_GET       = 2,
    DXTC_PUT       = 3,
    DXTC_RUN_END   = 4,
    DXTC_CODE      = 5,
    DXTC_PARAM     = 6
};

typedef struct {
    uint32_t wid, spu_num, image_id, pc;
    uint32_t code_base, ls_len, reserved0, reserved1;
    uint64_t wkl_data, ns;
    uint8_t  ls[DXT_CAPTURE_BEGIN_LS];
} dxt_run_begin;

typedef struct {
    uint32_t pc, status, polls, exited, code_base, reserved;
    uint64_t ns;
} dxt_run_end;

typedef struct {
    FILE* f;
    int active;
    int depth;
    uint32_t lane;
    unsigned long long begin_log, code_log, param_log;
    uint64_t bytes;
} dxt_run;

static SPU_THREAD_LOCAL dxt_run t_run;

static int s_inited, s_on, s_stop;
static char s_dir[1024], s_trigger[1024];
static unsigned s_max_runs = 4000;
static unsigned s_stride = 1;
static unsigned s_max_mb = 180;
static unsigned long long s_run_seq, s_runs, s_bytes, s_files;
static unsigned long long s_codes, s_params;
static int s_trigger_seen;

static uint32_t dxt_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t dxt_le32(const uint8_t* p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  | (uint32_t)p[0];
}

static uint64_t dxt_now_ns(void)
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

static void dxt_capture_init(void)
{
    if (__atomic_exchange_n(&s_inited, 1, __ATOMIC_RELAXED)) return;
    const char* d = getenv("PS3RECOMP_DXT_CAPTURE");
    if (!d || !*d) return;
    snprintf(s_dir, sizeof s_dir, "%s", d);
    const char* t = getenv("PS3RECOMP_DXT_CAPTURE_TRIGGER");
    if (t && *t) snprintf(s_trigger, sizeof s_trigger, "%s", t);
    const char* r = getenv("PS3RECOMP_DXT_CAPTURE_RUNS");
    if (r && *r) { long v = strtol(r, NULL, 0); if (v > 0) s_max_runs = (unsigned)v; }
    const char* s = getenv("PS3RECOMP_DXT_CAPTURE_STRIDE");
    if (s && *s) { long v = strtol(s, NULL, 0); if (v > 0) s_stride = (unsigned)v; }
    const char* m = getenv("PS3RECOMP_DXT_CAPTURE_MAX_MB");
    if (m && *m) { long v = strtol(m, NULL, 0); if (v > 0) s_max_mb = (unsigned)v; }
    dxt_mkdir(s_dir);
    s_on = 1;
    g_dxt_capture_on = 1;
    fprintf(stderr, "[dxt-capture] enabled dir=%s runs=%u stride=%u max_mb=%u trigger=%s\n",
            s_dir, s_max_runs, s_stride, s_max_mb,
            s_trigger[0] ? s_trigger : "(none)");
    fflush(stderr);
}

static int dxt_triggered(void)
{
    if (!s_trigger[0]) return 1;
    if (__atomic_load_n(&s_trigger_seen, __ATOMIC_RELAXED)) return 1;
    if (dxt_access(s_trigger, F_OK) != 0) return 0;
    __atomic_store_n(&s_trigger_seen, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "[dxt-capture] trigger seen; sampling\n");
    fflush(stderr);
    return 1;
}

static int dxt_reserve(uint64_t n)
{
    const uint64_t limit = (uint64_t)s_max_mb * 1024ull * 1024ull;
    unsigned long long cur = __atomic_load_n(&s_bytes, __ATOMIC_RELAXED);
    for (;;) {
        if ((uint64_t)cur + n > limit) {
            if (!__atomic_exchange_n(&s_stop, 1, __ATOMIC_RELAXED)) {
                fprintf(stderr, "[dxt-capture] %.1f MiB cap reached; capture stopped\n",
                        (double)cur / (1024.0 * 1024.0));
                fflush(stderr);
            }
            return 0;
        }
        if (__atomic_compare_exchange_n(&s_bytes, &cur, cur + n, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
}

static void dxt_rec(dxt_run* r, uint32_t type, uint64_t ea, uint32_t lsa,
                    uint32_t aux, const void* data, uint32_t size)
{
    if (!r->f) return;
    uint32_t hdr[6];
    hdr[0] = type;
    hdr[1] = size;
    hdr[2] = (uint32_t)ea;
    hdr[3] = (uint32_t)(ea >> 32);
    hdr[4] = lsa;
    hdr[5] = aux;
    fwrite(hdr, sizeof hdr, 1, r->f);
    if (size && data) fwrite(data, 1, size, r->f);
    r->bytes += DXT_CAPTURE_REC_HDR + size;
}

static void dxt_open_lane(dxt_run* r, uint32_t lane)
{
    if (r->f) return;
    const unsigned long long id = __atomic_fetch_add(&s_files, 1ull, __ATOMIC_RELAXED);
    char path[1200];
    snprintf(path, sizeof path, "%s/dxt_lane%u_%03llu.bin", s_dir, lane, id);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[dxt-capture] cannot open %s\n", path);
        fflush(stderr);
        return;
    }
    char magic[8];
    memcpy(magic, DXT_CAPTURE_MAGIC, sizeof magic);
    uint32_t ver = DXT_CAPTURE_VERSION, lane32 = lane, pad = 0;
    uint64_t start = dxt_now_ns();
    fwrite(magic, sizeof magic, 1, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&lane32, sizeof lane32, 1, f);
    fwrite(&pad, sizeof pad, 1, f);
    fwrite(&start, sizeof start, 1, f);
    r->f = f;
    r->lane = lane;
    fprintf(stderr, "[dxt-capture] lane %u spool %s\n", lane, path);
    fflush(stderr);
}

/* A WWS job module: four ila words (top byte 0x42/0x43), entry/image size,
 * then the C0DEC0DE marker at +0x20. The same shape as extract_jobmods.py. */
static int dxt_looks_like_code(const uint8_t* p, uint32_t size)
{
    if (size < 0x30) return 0;
    if (p[0] != 0x42 && p[0] != 0x43) return 0;
    if (p[0x20] != 0xC0 || p[0x21] != 0xDE ||
        p[0x22] != 0xC0 || p[0x23] != 0xDE) return 0;
    const uint32_t entry = dxt_be32(p + 0x10);
    const uint32_t imgsz = dxt_be32(p + 0x14);
    if (!imgsz || imgsz > 0x40000u || entry >= imgsz) return 0;
    return 1;
}

/* The 0x30-byte DXT parameter block built by GpDxtCompressor::CompressImage
 * (it sits at +0x70 of the 0xA0-byte WWS job source):
 *   0x00 source EA, 0x04 width, 0x08 height,
 *   0x10..0x1c the channel swizzle permutation (one of three accepted input
 *   formats), 0x20 output EA, 0x24 output code 1/3/5 (DXT1/3/5), 0x28 mip
 *   count, 0x2c scratch. Everything is big-endian except the four swizzle
 *   words, which the PPU writes host-endian (little-endian bytes). */
static int dxt_looks_like_param(const uint8_t* p)
{
    const uint32_t src = dxt_be32(p + 0x00);
    const uint32_t w   = dxt_be32(p + 0x04);
    const uint32_t h   = dxt_be32(p + 0x08);
    const uint32_t s0  = dxt_le32(p + 0x10);
    const uint32_t s1  = dxt_le32(p + 0x14);
    const uint32_t s2  = dxt_le32(p + 0x18);
    const uint32_t s3  = dxt_le32(p + 0x1c);
    const uint32_t out = dxt_be32(p + 0x24);
    if (!src || !w || w > 2048u || !h || h > 8192u) return 0;
    if (out != 1u && out != 3u && out != 5u) return 0;
    if (s0 != 0x03000102u && s0 != 0x01000203u && s0 != 0x03020100u) return 0;
    if (s1 != s0 + 0x04040404u || s2 != s0 + 0x08080808u || s3 != s0 + 0x0c0c0c0cu)
        return 0;
    return 1;
}

void dxt_capture_policy_begin(const struct spu_context* ctx, uint32_t wid,
                              uint64_t wkl_data, uint32_t spu_num)
{
    if (!g_dxt_capture_on) return;
    if (t_run.depth++ != 0) return;
    t_run.active = 0;
    if (!s_on || !ctx || wid != 0) return;
    if (__atomic_load_n(&s_stop, __ATOMIC_RELAXED)) return;
    if (!dxt_triggered()) return;

    const unsigned long long seq = __atomic_fetch_add(&s_run_seq, 1ull, __ATOMIC_RELAXED);
    if (s_stride > 1u && (seq % (unsigned long long)s_stride) != 0) return;
    if (__atomic_load_n(&s_runs, __ATOMIC_RELAXED) >= s_max_runs) {
        __atomic_store_n(&s_stop, 1, __ATOMIC_RELAXED);
        return;
    }
    dxt_open_lane(&t_run, spu_num);
    if (!t_run.f) return;
    if (!dxt_reserve(sizeof(dxt_run_begin))) return;

    dxt_run_begin b;
    memset(&b, 0, sizeof b);
    b.wid = wid;
    b.spu_num = spu_num;
    b.image_id = (uint32_t)ctx->image_id;
    b.pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    b.code_base = dxt_be32(ctx->ls + 0x1320);
    b.ls_len = DXT_CAPTURE_BEGIN_LS;
    b.wkl_data = wkl_data;
    b.ns = dxt_now_ns();
    memcpy(b.ls, ctx->ls + 0x100, DXT_CAPTURE_BEGIN_LS);

    dxt_rec(&t_run, DXTC_RUN_BEGIN, 0, 0, 0, &b, (uint32_t)sizeof b);
    __atomic_fetch_add(&s_runs, 1ull, __ATOMIC_RELAXED);
    t_run.active = 1;
    if (t_run.begin_log++ < 8) {
        fprintf(stderr, "[dxt-capture] RUN lane=%u wkl=0x%llX image=%d pc=0x%05X "
                        "codeBase(LS1320)=0x%05X seq=%llu\n",
                spu_num, (unsigned long long)wkl_data, ctx->image_id, b.pc,
                b.code_base, seq);
        fflush(stderr);
    }
}

void dxt_capture_policy_end(const struct spu_context* ctx)
{
    if (!g_dxt_capture_on) return;
    if (t_run.depth == 0) return;
    if (--t_run.depth != 0) return;
    if (!t_run.active) return;
    t_run.active = 0;
    if (!t_run.f) return;
    if (!dxt_reserve(sizeof(dxt_run_end))) return;

    dxt_run_end e;
    memset(&e, 0, sizeof e);
    e.pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    e.status = ctx->status;
    e.code_base = dxt_be32(ctx->ls + 0x1320);
    e.ns = dxt_now_ns();
    dxt_rec(&t_run, DXTC_RUN_END, 0, 0, 0, &e, (uint32_t)sizeof e);
    fflush(t_run.f);
}

void dxt_capture_dma(const struct spu_context* ctx, int is_put, uint32_t lsa,
                     uint64_t ea, uint32_t size, const uint8_t* data)
{
    (void)ctx;
    if (!t_run.active || !t_run.f || !data || !size) return;
    if (__atomic_load_n(&s_stop, __ATOMIC_RELAXED)) return;

    /* A larger transfer still names its source; keep a bounded prefix so the
     * code-jump header and the first input bytes are never lost. */
    uint32_t stored = size;
    if (stored > 0x4000u) stored = 0x4000u;
    if (!dxt_reserve(DXT_CAPTURE_REC_HDR + (uint64_t)stored)) return;
    dxt_rec(&t_run, is_put ? DXTC_PUT : DXTC_GET, ea, lsa & SPU_LS_MASK, size,
            data, stored);

    if (is_put) return;

    if (dxt_looks_like_code(data, stored)) {
        extern int spu_overlay_match_sig(const uint8_t hdr[16]);
        const int img = spu_overlay_match_sig(data);
        const uint32_t pfx = stored < DXT_CAPTURE_CODE_PFX ? stored : DXT_CAPTURE_CODE_PFX;
        if (dxt_reserve(DXT_CAPTURE_REC_HDR + pfx)) {
            dxt_rec(&t_run, DXTC_CODE, ea, lsa & SPU_LS_MASK, (uint32_t)img, data, pfx);
            __atomic_fetch_add(&s_codes, 1ull, __ATOMIC_RELAXED);
            if (t_run.code_log++ < 16) {
                fprintf(stderr, "[dxt-capture] CODE lane=%u ea=0x%08X size=0x%X "
                                "lsa=0x%05X image=%d hdr=%02X%02X%02X%02X%02X%02X%02X%02X"
                                "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                        t_run.lane, (uint32_t)ea, size, lsa & SPU_LS_MASK, img,
                        data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7],
                        data[8],data[9],data[10],data[11],data[12],data[13],data[14],data[15]);
                fflush(stderr);
            }
        }
    }

    /* The parameter block travels inside the job source GET; scan every GET
     * payload at 4-byte alignment for it. */
    if (stored >= 0x30u) {
        for (uint32_t off = 0; off + 0x30u <= stored; off += 4u) {
            if (!dxt_looks_like_param(data + off)) continue;
            if (dxt_reserve(DXT_CAPTURE_REC_HDR + 0x30u)) {
                dxt_rec(&t_run, DXTC_PARAM, ea + off, (lsa + off) & SPU_LS_MASK,
                        0, data + off, 0x30);
                __atomic_fetch_add(&s_params, 1ull, __ATOMIC_RELAXED);
                if (t_run.param_log++ < 8) {
                    fprintf(stderr, "[dxt-capture] PARAM lane=%u ea=0x%08X "
                                    "src=0x%08X w=%u h=%u swz=0x%08X outEA=0x%08X "
                                    "out=%u mip=%u scratch=0x%08X\n",
                            t_run.lane, (uint32_t)(ea + off), dxt_be32(data + off),
                            dxt_be32(data + off + 4), dxt_be32(data + off + 8),
                            dxt_le32(data + off + 0x10), dxt_be32(data + off + 0x20),
                            dxt_be32(data + off + 0x24),
                            dxt_be32(data + off + 0x28), dxt_be32(data + off + 0x2c));
                    fflush(stderr);
                }
            }
        }
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void dxt_capture_ctor(void)
{
    dxt_capture_init();
}
__attribute__((destructor)) static void dxt_capture_dtor(void)
{
    if (s_on)
        fprintf(stderr, "[dxt-capture] done: runs=%llu runs_sampled=%llu codes=%llu "
                        "params=%llu bytes=%.1f MiB files=%llu\n",
                s_run_seq, s_runs, s_codes, s_params,
                (double)__atomic_load_n(&s_bytes, __ATOMIC_RELAXED) / (1024.0 * 1024.0),
                s_files);
}
#endif
