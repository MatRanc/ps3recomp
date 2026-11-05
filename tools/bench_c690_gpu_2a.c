/*
 * Offline microbenchmark for the c690 0x2A GPU export path.
 *
 * Runs the real runtime/spu/c690_gpu_2a.c enter/classify/exit/PUT path over the
 * captured meter tail spools (C69STG01) and reports, per export:
 *   - classify time (format A/B match, the u16 offset list rebuild),
 *   - exit time (malloc + classify + the four array copies + the seed copy),
 *   - PUT time (the sink handoff; the sink frees the job),
 *   - bytes copied (4*extent + count*32 + count*2, the live packing).
 * A plain memcpy of the same bytes is timed as the copy floor.
 *
 * Usage: bench_c690_gpu_2a [spool-dir]
 */
#include "spu_context.h"
#include "c690_gpu_2a.h"
#include "ps3_live_ctl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PAIRS    512
#define LS_SIZE      SPU_LS_SIZE
#define GPR_BYTES    (128u * 16u)
#define TAIL_META_WORDS 24
#define TAIL_BITMAP_BYTES ((LS_SIZE >> 4) >> 3)
#define TAIL_PAYLOAD (TAIL_META_WORDS * 4u + TAIL_BITMAP_BYTES + LS_SIZE + GPR_BYTES)
#define REPEAT       16

typedef struct {
    uint32_t seq;
    uint8_t in_ls[LS_SIZE];
    uint8_t out_ls[LS_SIZE];
    uint8_t in_gpr[GPR_BYTES];
} tail_pair;

static tail_pair* s_pairs;
static unsigned s_npairs;

static unsigned long long s_jobs, s_records, s_bytes;

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int load_spool_file(const char* path)
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
            memcpy(pending.out_ls, ls, LS_SIZE);
            s_pairs[s_npairs++] = pending;
            have_in = 0;
        }
        free(buf);
    }
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
        load_spool_file(path);
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int sink(c690_gpu_2a_job* j)
{
    if (!j) return 0;
    s_jobs++;
    s_records += j->count;
    s_bytes += (uint64_t)j->extent * 4u + (uint64_t)j->count * 32u +
               (uint64_t)j->count * 2u;
    free(j->offs); free(j->arrays); free(j->seed);
    return 1;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    setenv("PS3RECOMP_C690_GPU_2A", "1", 1);
    ps3_live_ctl_refresh_env();
    c690_gpu_2a_set_sink(sink);

    s_pairs = (tail_pair*)calloc(MAX_PAIRS, sizeof *s_pairs);
    if (!s_pairs) return 2;
    load_corpus();
    if (!s_npairs) {
        fprintf(stderr, "bench_c690_gpu_2a: no corpus pairs (set C690_GPU_2A_SPOOL)\n");
        return 2;
    }

    static SPU_THREAD_LOCAL spu_context ctx;
    uint64_t t_class = 0, t_exit = 0, t_put = 0;
    uint64_t exports = 0, records = 0;
    uint64_t copy_floor_ns = 0;
    uint8_t* scratch = (uint8_t*)malloc(1u << 20);

    for (unsigned it = 0; it < REPEAT; it++) {
        for (unsigned i = 0; i < s_npairs; i++) {
            const tail_pair* p = &s_pairs[i];
            spu_context_init(&ctx, 0);
            ctx.image_id = 6;
            memcpy(ctx.ls, p->in_ls, LS_SIZE);
            memcpy(ctx.gpr, p->in_gpr, GPR_BYTES);

            const uint32_t r3 = ctx.gpr[3]._u32[0] & 0x3FFFFu;
            const uint32_t out_base = be32(p->in_ls + ((r3 + 0xD0u) & 0x3FFFFu));
            const uint32_t end = be32(p->out_ls + ((r3 + 0xD0u) & 0x3FFFFu));
            if (!out_base || end <= out_base) continue;
            const uint32_t count = (end - out_base) / 32u;
            const uint32_t cmd0 = be32(p->in_ls + ((r3 + 0x150u) & 0x3FFFFu));

            uint16_t* offs = (uint16_t*)malloc((size_t)count * 2u);
            int format = -1;
            uint64_t t0 = now_ns();
            const int ok = c690_gpu_2a_classify(p->out_ls, p->out_ls + out_base,
                                                r3, count, cmd0, offs, &format);
            t_class += now_ns() - t0;
            free(offs);
            if (!ok) continue;

            c690_gpu_2a_enter(&ctx);

            /* The lifted body's effect: the OUT record bytes and the advanced
             * output cursor in the LS. The source arrays stay as they were. */
            memcpy(ctx.ls + out_base, p->out_ls + out_base,
                   (size_t)count * 32u);
            put_be32(ctx.ls + ((r3 + 0xD0u) & 0x3FFFFu), end);

            t0 = now_ns();
            c690_gpu_2a_exit(&ctx);
            const uint64_t te = now_ns() - t0;
            t_exit += te;

            const uint32_t lsa = out_base;
            const uint32_t ea = 0x11800000u + (uint32_t)(i % 2048u) * 0x40u;
            t0 = now_ns();
            c690_gpu_2a_put(&ctx, lsa, ea, LS_SIZE - lsa);
            t_put += now_ns() - t0;
            exports++;
            records += count;

            /* Copy floor: the same bytes the export moves, as plain memcpy. */
            const uint32_t maxoff = 0; (void)maxoff;
            uint64_t bytes = (uint64_t)count * 16u * 4u + (uint64_t)count * 32u +
                             (uint64_t)count * 2u;
            if (bytes > (1u << 20)) bytes = 1u << 20;
            t0 = now_ns();
            memcpy(scratch, ctx.ls, bytes);
            asm volatile("" :: "r"(scratch) : "memory");
            copy_floor_ns += now_ns() - t0;
        }
    }

    printf("pairs:        %u (x%d)\n", s_npairs, REPEAT);
    printf("exports:      %llu, records %llu (%.1f/export)\n",
           (unsigned long long)exports, (unsigned long long)records,
           exports ? (double)records / exports : 0.0);
    printf("bytes/export: %.0f (4*extent + count*32 + count*2)\n",
           exports ? (double)s_bytes / exports : 0.0);
    printf("classify:     %.0f ns/export (%.2f ns/record)\n",
           exports ? (double)t_class / exports : 0.0,
           records ? (double)t_class / records : 0.0);
    printf("exit:         %.0f ns/export (%.2f ns/record)\n",
           exports ? (double)t_exit / exports : 0.0,
           records ? (double)t_exit / records : 0.0);
    printf("put:          %.0f ns/export\n",
           exports ? (double)t_put / exports : 0.0);
    printf("export total: %.0f ns/export\n",
           exports ? (double)(t_exit + t_put) / exports : 0.0);
    printf("memcpy floor: %.0f ns/export (%.1f GB/s)\n",
           exports ? (double)copy_floor_ns / exports : 0.0,
           copy_floor_ns ? (double)s_bytes / (double)copy_floor_ns : 0.0);
    free(scratch);
    free(s_pairs);
    return 0;
}
