/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"
#include "rsx_wcb.h"
/* The page-protection half of on-demand readback is its own file but not its
 * own translation unit: every build of the engine -- the runtime library and
 * the many fixtures that compile this file directly -- gets it this way. */
#include "rsx_wcb.c"
#include "rsx_tex_miss.h"
#include "rsx_tex_miss.c"
#include "rsx_draw_queue.h"
#include "ps3_live_ctl.h"
#include "ps3emu/ps3_trace.h"   /* PS3RECOMP_TIMELINE: readback done */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <time.h>
#include "ps3emu/log_first.h"
#include "../../runtime/memory/vm.h"   /* vm_commit: the capture images pages */

/* Draw-record worker barriers (defined with the packet code below). */
static void eng_mt_sync(void);

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* cellGcmSys.c: the display buffers registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;
/* cellGcmSys.c: monotonic ms, same epoch as the [flip] lines. Weak so the
 * offline engine fixtures (which compile this file alone) still link; the
 * `at=` field is then 0 and simply unused. */
extern unsigned long long ps3_ms_now(void) __attribute__((weak));
/* cellGcmSys.c: the guest flip counter (PS3RECOMP_PRESENT_TRACE). Weak so the
 * standalone engine fixtures still link. */
extern u32 cellGcm_flip_frame_count(void) __attribute__((weak));

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_PIPELINE_INDEX 16384u        /* open-addressed key index, 2x the table */
#define ENG_VP_MEMO        8
#define ENG_MAX_BATCHES    256
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_POLY_OFFSET_POINT   0x0A60
#define M_POLY_OFFSET_LINE    0x0A64
#define M_POLY_OFFSET_FILL    0x0A68
#define M_POLY_OFFSET_FACTOR  0x0A78
#define M_POLY_OFFSET_BIAS    0x0A7C

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 pitch;              /* guest bytes per row, from SET_SURFACE_PITCH */
    u32 aa_grid;            /* samples per pixel, aa_sx | aa_sy << 8          */
    rsx_be_format fmt;
    u32 handle;
    u32 feedback;          /* last immutable own-target copy, engine-owned */
    u64 color_serial;       /* g.write_serial of the last colour write, or 0 */
    u64 last_write_serial;  /* any colour write, 3D or 2D (present trace)     */
    int guest_dirty;        /* rendered since guest memory last got a copy   */
    u32 last_use;           /* g.frames when last bound, sampled or presented */
    /* PS3RECOMP_RSX_WCB ondemand/async (docs/wcb-async.md). */
    int wcb_pending;        /* guest pages protected, guest bytes stale       */
    u32 wcb_ticket;         /* queued async copy, or 0                        */
    u64 wcb_ticket_serial;  /* color_serial the ticket's copy captured        */
    u32 wcb_reads;          /* guest reads that needed a copy                 */
    u32 wcb_last_read;      /* g.frames of the last one                       */
    /* PS3RECOMP_RSX_CUBE_RT=1: the last draw that wrote this target, for the
     * cube-face census (which programs produce the six faces). */
    u64 last_writer_fp;
    u32 last_writer_pipe;
    /* PS3RECOMP_RSX_WCB_SKIP: the colour serial, and the surface-pixel
     * rectangle, the guest-memory copy was last known to hold. 0 serial = the
     * bytes are not known to be valid. */
    u64 guest_valid_serial;
    u32 guest_valid_x, guest_valid_y, guest_valid_w, guest_valid_h;
} eng_surface;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    u32 snap_w, snap_h;    /* the region that copy holds                    */
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 depth_serial;      /* g.write_serial of the last depth write or clear */
    int guest_dirty;       /* written since guest memory last got a copy     */
    u32 zfmt, pitch;       /* SURFACE_FORMAT depth field and zeta pitch then */
    u32 used_w, used_h;    /* the clip that write went through              */
    u32 conv;              /* depth_as_color of the depth at conv_serial, or 0 */
    u32 conv_w, conv_h, conv_fmt;
    u64 conv_serial;
    u32 last_use;          /* g.frames when last bound or sampled             */
    u32 ext_w, ext_h;      /* the largest clip it was bound with: past it,
                            * nothing was drawn or cleared (eng_clear_rect) */
    int wcb_pending;       /* guest pages protected (docs/wcb-async.md)       */

} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u64 sparse_hash;            /* eng_texture_sparse_hash, the per-frame check */
    u32 last_full_frame;        /* g.frames of the last full content hash */
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 depth;                  /* slices of a 3D texture, else 1 */
    u32 levels;                 /* SET_TEXTURE_FORMAT mipmap count it was built with */
    int rewritten;              /* guest bytes rewritten by the engine itself */
    /* PS3RECOMP_TEX_DIRTY: guest EA of the span, the write sequence sampled
     * just before the bytes were read, and when the pages were last
     * re-protected. */
    u32 ea;
    u64 dirty_seq;
    u64 prot_serial;
    u8  dirty_covered;
    u8  hash_valid;             /* content_hash is the paranoid-check reference */

} eng_texture;

typedef struct {
    u64 key;
    u32 handle;
    u8 fixed;
    u16 tex_used;           /* units the pixel program samples */
    u64 fp_hash;            /* eng_fp_debug_hash of the guest fragment program */
    u16 vpc_lo, vpc_n;      /* the vp_c[] rows its vertex program can read... */
    u8 vpc_full;            /* ...or any row (relative reads, built-in program) */
} eng_pipeline;

typedef struct { u32 first, count; } eng_batch;

/* A vertex program's identity hash, remembered with the exact bytes it was
 * computed from: the transform program changes only on upload, but every
 * draw re-derives the pipeline key from it. */
typedef struct {
    const u8* uc;
    u32 bytes;
    u64 seed, hash;
    u8 copy[RSX_DSP_VP_INSTR * 16u];
} eng_vp_memo;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    u16 pipeline_index[ENG_PIPELINE_INDEX];   /* pipelines[] slot + 1, 0 = empty */
    eng_vp_memo vp_memo[ENG_VP_MEMO];
    u32 vp_memo_next;
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;
    u64 write_serial;       /* orders colour and depth writes to one address */

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    rsx_fp_constant_block fp_constants;
} g;

/* PS3RECOMP_PRESENT_TRACE=1 (default off): see eng_present_trace. Declared
 * here so a 2D colour-target write only bumps color_serial while the trace is
 * on; last_write_serial is diagnostic and gates nothing else. */
static int s_ptrace = -1;

/* ==========================================================================
 * PS3RECOMP_WALKER_FAST: cut 1 (fetch plan/remap) and cut 3 (pipeline input
 * memo). A memo survives a frame boundary; it is invalidated by a change in
 * its key and, for the fetch plan, by a new guest-memory resolver
 * (s_fast_epoch). The fetch cache holds addresses and spans, never guest
 * bytes, so vertex payloads are always re-read; the reference set and the
 * program bytes are key inputs, so a changed index array or program misses.
 * ======================================================================== */

#define ENG_FETCH_MEMO 1024
#define ENG_PIPE_MEMO  512
#define ENG_REMAP_MEMO 1024
/* PS3RECOMP_MEMO_INDEX: the memo find used to be a linear scan of the whole
 * table (1024/512 slots) on every draw. A direct-mapped index answers the
 * common hit in one probe; a collision still falls back to the exact scan,
 * so the answer is the same either way. */
#define ENG_MEMO_INDEX 4096

/* One prepared fetch plan and its remap, keyed by the attribute layout, the
 * decoded VTXFMT/default state and the reference set. No guest bytes are
 * cached: only addresses, spans and the occurrence map. */
typedef struct {
    int used;
    u32 epoch;
    u64 key;
    u32 n_source, n_unique;
    int remapped;
    rsx_vertex_ref* refs;
    u32 refs_cap;
    u32* occ;
    u32 occ_cap;
    rsx_vertex_fetch_plan plan;   /* prepared against `refs` */
} eng_fetch_memo;

/* One pipeline answer, keyed by a fingerprint of the program bytes and every
 * register the pipeline decision reads. The buffered fragment constants are
 * part of the answer: sink_end copies them into the fragment constant block. */
typedef struct {
    int used;
    u32 epoch;
    u64 fingerprint;
    u32 handle;
    u8 fixed;
    u16 tex_used;
    u64 fp_hash;
    u16 vpc_lo, vpc_n;
    u8 vpc_full;
    /* The block is 32 KB padded to RSX_FP_MAX_INLINE_CONSTANTS; only `count`
     * slots are live, and copying the whole thing per draw cost more than
     * the analysis the memo replaces. */
    u32 const_count;
    u32* const_values;
    u32 const_cap;
} eng_pipe_memo;

/* One deduplicated reference answer, keyed by the source reference list. It
 * stores only the collapse result (unique prefix and occurrence map), never
 * guest bytes, so a hit is exactly what rsx_vertex_remap_build would return
 * for the same references. It is independent of the fetch plan: a draw whose
 * transform/buffer registers changed but whose references did not still hits. */
typedef struct {
    int used;
    u32 epoch;
    u64 key;
    u32 n_source, n_unique;
    int remapped;
    rsx_vertex_ref* refs;
    u32 refs_cap;
    u32* occ;
    u32 occ_cap;
} eng_remap_memo;

static int s_walker_fast;
/* The two halves of WALKER_FAST as independent switches: the fetch
 * plan/remap memo (plan item 4) and the pipeline answer memo (plan item 5).
 * WALKER_FAST=1 still turns both on; an explicit per-item value wins. */
static int s_fetch_memo;
static int s_pipe_memo;
/* PS3RECOMP_REMAP_CACHE: memoize rsx_vertex_remap_build by the reference
 * list, independent of the fetch plan (plan item 1/4). */
static int s_remap_cache;
/* PS3RECOMP_MEMO_INDEX: probe the memo through a direct-mapped index before
 * the exact linear scan. PS3RECOMP_REFS_HASH_FAST: fold the reference hash
 * into the reference-building loop instead of hashing the whole list again. */
static int s_memo_index;
static int s_refs_hash_fast;
/* EXP_NO_DRAW: live throwaway switch. When on, sink_end returns before any
 * fetch, pipeline, texture or Vulkan recording; the method stream (and so
 * fences, labels and flips) still dispatches exactly as before. */
static int s_no_draw;
static u32 s_fast_epoch = 1;
static unsigned long long s_fetch_hits, s_fetch_misses;
static unsigned long long s_pipe_hits, s_pipe_misses;
static unsigned long long s_remap_hits, s_remap_misses;
static eng_fetch_memo s_fmemo[ENG_FETCH_MEMO];
static u32 s_fmemo_next;
static eng_pipe_memo s_pmemo[ENG_PIPE_MEMO];
static u32 s_pmemo_next;
static eng_remap_memo s_rmemo[ENG_REMAP_MEMO];
static u32 s_rmemo_next;
/* slot + 1 of the last entry stored under a bucket, 0 = none. */
static u32 s_fmemo_index[ENG_MEMO_INDEX];
static u32 s_pmemo_index[ENG_MEMO_INDEX];
static u32 s_rmemo_index[ENG_MEMO_INDEX];

static inline u32 eng_memo_bucket(u64 key)
{
    /* The keys are already well mixed (FNV-1a / a 64-bit fingerprint); the
     * low bits are as good as any and the fold keeps a 64-bit key's high
     * bits from vanishing on a table smaller than 2^32. */
    return (u32)(key ^ (key >> 32)) & (ENG_MEMO_INDEX - 1u);
}

/* Remember where an entry was stored. A later collision only loses the
 * shortcut (the find falls back to its exact scan), never an answer. */
static inline void eng_memo_index_put(u32* index, u32 slot, u64 key)
{
    index[eng_memo_bucket(key)] = slot + 1u;
}

/* The direct-mapped shortcut: nonzero only when this exact entry is the one
 * the bucket last named. */
static inline u32 eng_memo_index_get(const u32* index, u64 key)
{
    return index[eng_memo_bucket(key)];
}

static void eng_fast_reset_caches(void)
{
    for (u32 i = 0; i < ENG_FETCH_MEMO; i++) s_fmemo[i].used = 0;
    for (u32 i = 0; i < ENG_PIPE_MEMO; i++) s_pmemo[i].used = 0;
    for (u32 i = 0; i < ENG_REMAP_MEMO; i++) s_rmemo[i].used = 0;
    memset(s_fmemo_index, 0, sizeof s_fmemo_index);
    memset(s_pmemo_index, 0, sizeof s_pmemo_index);
    memset(s_rmemo_index, 0, sizeof s_rmemo_index);
    s_fmemo_next = s_pmemo_next = s_rmemo_next = 0;
    s_fast_epoch = 1;
    s_fetch_hits = s_fetch_misses = 0;
    s_pipe_hits = s_pipe_misses = 0;
    s_remap_hits = s_remap_misses = 0;
}

static void eng_fast_free_caches(void)
{
    for (u32 i = 0; i < ENG_FETCH_MEMO; i++) {
        free(s_fmemo[i].refs);
        free(s_fmemo[i].occ);
    }
    for (u32 i = 0; i < ENG_PIPE_MEMO; i++)
        free(s_pmemo[i].const_values);
    for (u32 i = 0; i < ENG_REMAP_MEMO; i++) {
        free(s_rmemo[i].refs);
        free(s_rmemo[i].occ);
    }
    memset(s_fmemo, 0, sizeof s_fmemo);
    memset(s_pmemo, 0, sizeof s_pmemo);
    memset(s_rmemo, 0, sizeof s_rmemo);
    s_fmemo_next = s_pmemo_next = s_rmemo_next = 0;
}

/* Live switch setters. cellGcmSys.c owns the PS3RECOMP_CTL dependency (a
 * standalone engine fixture compiles this file without the control module),
 * and calls these once per drain pass. A switch change flushes the memos it
 * governs so the arm measures the path it names from its first draw. */
void rsx_draw_engine_set_walker_fast(int on)
{
    if (on != s_walker_fast) {
        s_walker_fast = on;
        s_fetch_memo = on;
        s_pipe_memo = on;
        eng_fast_reset_caches();
        fprintf(stderr, "[live] PS3RECOMP_WALKER_FAST=%d: fetch/pipeline memos flushed\n",
                on);
    }
}

void rsx_draw_engine_set_fetch_memo(int on)
{
    if (on != s_fetch_memo) {
        s_fetch_memo = on;
        eng_fast_reset_caches();
        fprintf(stderr, "[live] PS3RECOMP_FETCH_MEMO=%d: fetch memo flushed\n", on);
    }
}

void rsx_draw_engine_set_pipe_memo(int on)
{
    if (on != s_pipe_memo) {
        s_pipe_memo = on;
        eng_fast_reset_caches();
        fprintf(stderr, "[live] PS3RECOMP_PIPE_MEMO=%d: pipeline memo flushed\n", on);
    }
}

void rsx_draw_engine_set_remap_cache(int on)
{
    if (on != s_remap_cache) {
        s_remap_cache = on;
        eng_fast_reset_caches();
        fprintf(stderr, "[live] PS3RECOMP_REMAP_CACHE=%d: remap memo flushed\n", on);
    }
}

void rsx_draw_engine_set_memo_index(int on)
{
    if (on == s_memo_index) return;
    s_memo_index = on;
    /* An entry cached while this was off was not indexed; the exact scan
     * still finds it, so no flush is needed. Clear the stale index anyway so
     * the first probe after enabling cannot name an unrelated generation. */
    if (on) {
        memset(s_fmemo_index, 0, sizeof s_fmemo_index);
        memset(s_pmemo_index, 0, sizeof s_pmemo_index);
        memset(s_rmemo_index, 0, sizeof s_rmemo_index);
    }
}

void rsx_draw_engine_set_refs_hash_fast(int on)
{
    s_refs_hash_fast = on;
}

void rsx_draw_engine_set_no_draw(int on)
{
    s_no_draw = on;
}

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    /* FNV-1a of the source reference bytes, folded in while the list is
     * built. Equal to eng_refs_key()'s hash over the same refs. */
    u64 refs_hash;
    int refs_remapped;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;

    /* Canonical ATTR0..15 float4 snapshots for register-based vertices.
     * Bounded separately from compact GPU staging; never fetch stale arrays. */
    float (*immediate)[RSX_DSP_NUM_VERTEX_ATTR][4];
    u32 n_immediate, cap_immediate;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;
    /* The vertices are converted at the draw, into the backend's own memory
     * (draw_fill), not into verts at the fetch. */
    int fetch_deferred;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
    u32 inline_dropped_at_begin;
} dc;

/* Bumped whenever a texture unit's binding could resolve differently with the
 * same registers inside one frame: a colour or depth target created at some
 * address, a cached texture's handle replaced or evicted, or cached textures
 * marked rewritten. Drops the binding memo (sink_bind_textures). */
static u32 s_tex_gen = 1;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
    /* Cached fetch plans hold guest addresses. A new resolver can mean a new
     * mapping, so nothing prepared against the old one is valid. */
    s_fast_epoch++;
}


/* ==========================================================================
 * Walker profiler (PS3RECOMP_WALKER_PROF) and one-frame capture
 * (PS3RECOMP_WALKER_CAPTURE). Both are dormant unless their environment
 * variable is set; the off path is a cached integer compare.
 * ======================================================================== */

static int s_wprof = -1;
/* Frame sampling. PS3RECOMP_WALKER_PROF=1 keeps [walker-prof] lines and the
 * exact per-frame draw count, but fully instruments only every Nth frame
 * (PS3RECOMP_WALKER_PROF_SAMPLE, default 8, 1 = every frame). The dispatch
 * timestamps in the FIFO loop are per method, so sampling draws was not
 * enough; a sampled frame times every drain, draw and method, and the report
 * scales the phase sums by (frames since the last report / sampled frames)
 * before dividing by flips. The cost analysis is in docs/walker-remap.md. */
static int s_wprof_stride = -1;
static int s_wprof_pass = 0;
static unsigned long long s_wprof_frame_seq;
static unsigned long long s_wp_frames_total, s_wp_frames_prof;
static unsigned long long s_wp_ns[RSX_WPROF_PHASES];
static unsigned long long s_wp_n[RSX_WPROF_PHASES];
static unsigned long long s_wp_drain_ns;
static unsigned long long s_wp_dispatch_ns;
static unsigned long long s_wp_draws;
static unsigned long long s_wp_flips;
static unsigned long long s_wp_frames;
static int s_wp_flip;
/* Draw-source split: draws fed from the five SPU mesh rings vs everything
 * else, and the sink_end time each class cost. EA range is the one in
 * docs/progress.md (2026-10-27 night): five 2 MB rings at 0x11900000..0x12100000. */
#define ENG_MESH_EA_LO 0x11900000u
#define ENG_MESH_EA_HI 0x12300000u
static unsigned long long s_wp_main_draws, s_wp_mesh_draws;
static unsigned long long s_wp_main_ns, s_wp_mesh_ns;
/* Pipeline creation inside the walker (vkCreate*Pipelines), fed by the
 * Vulkan backend. The `pipe` phase includes it; this separates it. */
static unsigned long long s_wp_pipe_create_ns, s_wp_pipe_create_n;
/* Base for the `at=` field, the same clock and near the same epoch as the
 * guest [flip] lines (first flip). */
static unsigned long long s_wp_t0_ms;
static unsigned long long s_wp_last_at_ns;

/* RSX_WPROF2: a finer split of the same draw-group work than the five
 * phases above. Off by default; the off path is a cached integer test around
 * every site. Timers nest: `convert` is inside `fetch`/`draw`, `upload` is
 * inside `tex_slot`, `tex_slot` inside `tex`. The report prints each term as
 * measured and says which contain which. */
enum {
    WPROF2_LAYOUT = 0,
    WPROF2_SURFACE,        /* render state, targets, surface/viewport/zeta */
    WPROF2_FETCH_REFS,     /* arrays + index runs -> reference list */
    WPROF2_FETCH_PLAN,     /* key, remap, fetch-plan prepare, defer choice */
    WPROF2_CONVERT,        /* vertex fetch/convert into backend memory */
    WPROF2_VTXOFF,         /* eng_check_gpu_vertex_offload */
    WPROF2_PIPE_LOOKUP,    /* fingerprint + memo probe */
    WPROF2_PIPE_ANALYZE,   /* program/state analysis, texture masks */
    WPROF2_PIPE_KEY,       /* identity hash */
    WPROF2_PIPE_DIR,       /* pipeline hash-directory lookup */
    WPROF2_PIPE_DECOMPILE, /* VP/FP decompile + text patching */
    WPROF2_PIPE_CREATE,    /* be->pipeline_create (host compile) */
    WPROF2_PIPE_DEBUG,     /* eng_debug_fp */
    WPROF2_TEX_SLOT,       /* texture cache lookup/hash/decode/upload */
    WPROF2_TEX_UPLOAD,     /* texture decode + backend upload (inside slot) */
    WPROF2_TEX_BIND,       /* surface/depth scan, samplers, memo (tex - slot) */
    WPROF2_VTEX,           /* vertex texture binds */
    WPROF2_CONST,          /* VP/FP constant blocks */
    WPROF2_INDEX,          /* topology rebuild + index writer */
    WPROF2_BIND_BE,        /* backend bind calls (descriptor writes) */
    WPROF2_SURFTRACK,      /* pd_note, written-target bookkeeping */
    WPROF2_DRAW_BE,        /* backend draw recording (includes convert) */
    WPROF2_N
};
static int s_wp2 = -1;
static unsigned long long s_wp2_ns[WPROF2_N], s_wp2_n[WPROF2_N];
static unsigned long long s_wp2_draws, s_wp2_verts, s_wp2_draw_groups;
static unsigned long long s_wp2_conv_verts, s_wp2_conv_bytes;
static unsigned long long s_wp2_pipe_hits, s_wp2_pipe_gets, s_wp2_pipe_created;
static unsigned long long s_wp2_tex_slots, s_wp2_tex_uploads;
static unsigned long long s_wp2_fetch_hit, s_wp2_fetch_miss;
static unsigned long long s_wp2_remap_hit, s_wp2_remap_miss;

static int rsx_wprof2_enabled(void)
{
    if (s_wp2 < 0) {
        const char* e = getenv("PS3RECOMP_WALKER_PROF2");
        s_wp2 = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return s_wp2;
}

static inline unsigned long long wp2_t0(void)
{
    if (s_wp2 < 0) rsx_wprof2_enabled();
    return s_wp2 > 0 ? rsx_wprof_now() : 0;
}

static inline void wp2_add(int slot, unsigned long long t0)
{
    if (s_wp2 > 0 && t0) {
        s_wp2_ns[slot] += rsx_wprof_now() - t0;
        s_wp2_n[slot]++;
    }
}

int rsx_wprof_enabled(void)
{
    if (s_wprof < 0) {
        const char* e = getenv("PS3RECOMP_WALKER_PROF");
        s_wprof = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return s_wprof;
}

static int rsx_wprof_sample_stride(void)
{
    if (s_wprof_stride < 0) {
        const char* e = getenv("PS3RECOMP_WALKER_PROF_SAMPLE");
        int n = (e && *e) ? atoi(e) : 8;
        if (n < 1) n = 1;
        if (n > 4096) n = 4096;
        s_wprof_stride = n;
    }
    return s_wprof_stride;
}

int rsx_wprof_drain_begin(void)
{
    if (rsx_wprof_enabled() <= 0) { s_wprof_pass = 0; return 0; }
    /* WALKER_PROF2 has its own exact per-draw timers (not sampled); do not
     * starve it of reports. */
    if (rsx_wprof2_enabled() > 0) { s_wprof_pass = 1; return 1; }
    /* Sample whole frames, not drains: the FIFO and the flip can be handled
     * by different drains, and a per-drain decision would mix a sampled
     * drain's timers with an unsampled one's draws. The frame sequence is
     * advanced by rsx_wprof_note_flip(), so every drain of a frame agrees. */
    const int stride = rsx_wprof_sample_stride();
    s_wprof_pass = stride <= 1 ||
        (s_wprof_frame_seq % (unsigned long long)stride) == 0;
    return s_wprof_pass;
}

unsigned long long rsx_wprof_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull +
           (unsigned long long)t.tv_nsec;
}

void rsx_wprof_note_drain(unsigned long long ns)
{
    if (s_wprof > 0) s_wp_drain_ns += ns;
}

void rsx_wprof_note_dispatch(unsigned long long ns)
{
    if (s_wprof > 0) s_wp_dispatch_ns += ns;
}

void rsx_wprof_note_flip(void)
{
    if (s_wprof > 0) {
        if (!s_wp_t0_ms && ps3_ms_now) s_wp_t0_ms = ps3_ms_now();
        s_wp_flip = 1;
        s_wp_flips++;
        s_wp_frames_total++;
        if (rsx_wprof2_enabled() > 0 ||
            rsx_wprof_sample_stride() <= 1 ||
            (s_wprof_frame_seq % (unsigned long long)rsx_wprof_sample_stride()) == 0)
            s_wp_frames_prof++;
        s_wprof_frame_seq++;
    }
}

void rsx_wprof_note_pipeline_create(unsigned long long ns)
{
    if (s_wprof > 0) { s_wp_pipe_create_ns += ns; s_wp_pipe_create_n++; }
}

static void wprof_phase(int phase, unsigned long long ns)
{
    if (s_wprof > 0 && s_wprof_pass) { s_wp_ns[phase] += ns; s_wp_n[phase]++; }
}

void rsx_wprof_drain_end(int profiled)
{
    if (s_wprof <= 0 || !profiled || !s_wp_flip) return;
    /* A flip can land between two drain passes, so one report may cover
     * several flips. Divide by them: the printed values are per frame, and
     * the pitch window is many reports wide. */
    const double f = s_wp_flips ? (double)s_wp_flips : 1.0;
    /* Only every Nth drain is timed (per-method dispatch timestamps are the
     * expensive part); scale its sums back to the whole interval so the
     * printed per-frame values still describe every frame. The draw counts
     * are accumulated on every drain and are not scaled. */
    const double scale = s_wp_frames_prof
        ? (double)s_wp_frames_total / (double)s_wp_frames_prof : 1.0;
    const double drain = (double)s_wp_drain_ns / 1e6 / f * scale;
    const double disp  = (double)s_wp_dispatch_ns / 1e6 / f * scale;
    double sum = 0.0;
    for (int i = 0; i < RSX_WPROF_PHASES; i++)
        sum += (double)s_wp_ns[i] / 1e6 / f * scale;
    const double parse = drain > disp ? drain - disp : 0.0;
    const double other = disp > sum ? disp - sum : 0.0;
    /* Wall time since the previous report, per frame. The difference from the
     * walker's own drain time is the wait (idle/park/present), the split the
     * critical-path doc reports as drain vs put_wait. */
    const unsigned long long now_ns = rsx_wprof_now();
    double wait = 0.0;
    if (s_wp_last_at_ns) {
        const double wall = (double)(now_ns - s_wp_last_at_ns) / 1e6 / f;
        wait = wall > drain ? wall - drain : 0.0;
    }
    s_wp_last_at_ns = now_ns;
    const unsigned long long at_ms =
        (s_wp_t0_ms && ps3_ms_now)
            ? (unsigned long long)(ps3_ms_now() - s_wp_t0_ms) : 0;
    fprintf(stderr,
            "[walker-prof] at=%llums report=%llu flips=%llu draws=%.1f "
            "walker=%.3fms wait=%.3fms engine=%.3fms parse=%.3fms "
            "fetch=%.3f tex=%.3f pipe=%.3f bind=%.3f draw=%.3f other=%.3f "
            "| src_main=%.1f src_mesh=%.1f mainms=%.3f meshms=%.3f "
            "pipecrt=%llu pipems=%.3f | n/fetch=%.1f tex=%.1f "
            "pipe=%.1f bind=%.1f draw=%.1f\n",
            at_ms, s_wp_frames, s_wp_flips, (double)s_wp_draws / f,
            drain, wait, disp, parse,
            (double)s_wp_ns[RSX_WPROF_FETCH] / 1e6 / f * scale,
            (double)s_wp_ns[RSX_WPROF_TEX] / 1e6 / f * scale,
            (double)s_wp_ns[RSX_WPROF_PIPE] / 1e6 / f * scale,
            (double)s_wp_ns[RSX_WPROF_BIND] / 1e6 / f * scale,
            (double)s_wp_ns[RSX_WPROF_DRAW] / 1e6 / f * scale, other,
            (double)s_wp_main_draws / f, (double)s_wp_mesh_draws / f,
            (double)s_wp_main_ns / 1e6 / f * scale,
            (double)s_wp_mesh_ns / 1e6 / f * scale,
            s_wp_pipe_create_n, (double)s_wp_pipe_create_ns / 1e6 / f * scale,
            (double)s_wp_n[RSX_WPROF_FETCH] / f * scale,
            (double)s_wp_n[RSX_WPROF_TEX] / f * scale,
            (double)s_wp_n[RSX_WPROF_PIPE] / f * scale,
            (double)s_wp_n[RSX_WPROF_BIND] / f * scale,
            (double)s_wp_n[RSX_WPROF_DRAW] / f * scale);
    if (s_wp2 < 0) rsx_wprof2_enabled();
    if (s_wp2 > 0) {
        fprintf(stderr,
                "[walker-prof2] at=%llums draws=%.1f verts=%.1f "
                "conv_verts=%.1f conv_kb=%.1f "
                "layout=%.3fms surface=%.3f fetch_refs=%.3f fetch_plan=%.3f "
                "convert=%.3f vtxoff=%.3f pipe_lookup=%.3f pipe_analyze=%.3f "
                "pipe_key=%.3f pipe_dir=%.3f pipe_decompile=%.3f "
                "pipe_create=%.3f pipe_debug=%.3f tex_slot=%.3f "
                "tex_upload=%.3f tex_bind=%.3f vtex=%.3f const=%.3f "
                "index=%.3f bind_be=%.3f surftrack=%.3f draw_be=%.3f "
                "| pipe_hit=%.1f pipe_get=%.1f pipe_new=%.1f "
                "fetch_hit=%.0f fetch_miss=%.0f remap_hit=%.0f remap_miss=%.0f "
                "tex_binds=%.1f tex_loads=%.1f\n",
                at_ms, (double)s_wp2_draws / f, (double)s_wp2_verts / f,
                (double)s_wp2_conv_verts / f,
                (double)s_wp2_conv_bytes / 1024.0 / f,
                (double)s_wp2_ns[WPROF2_LAYOUT] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_SURFACE] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_FETCH_REFS] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_FETCH_PLAN] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_CONVERT] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_VTXOFF] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_LOOKUP] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_ANALYZE] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_KEY] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_DIR] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_DECOMPILE] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_CREATE] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_PIPE_DEBUG] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_TEX_SLOT] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_TEX_UPLOAD] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_TEX_BIND] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_VTEX] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_CONST] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_INDEX] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_BIND_BE] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_SURFTRACK] / 1e6 / f,
                (double)s_wp2_ns[WPROF2_DRAW_BE] / 1e6 / f,
                (double)s_wp2_pipe_hits / f, (double)s_wp2_pipe_gets / f,
                (double)s_wp2_pipe_created / f,
                (double)s_wp2_fetch_hit / f, (double)s_wp2_fetch_miss / f,
                (double)s_wp2_remap_hit / f, (double)s_wp2_remap_miss / f,
                (double)s_wp2_tex_slots / f, (double)s_wp2_tex_uploads / f);
        fflush(stderr);
        memset(s_wp2_ns, 0, sizeof s_wp2_ns);
        memset(s_wp2_n, 0, sizeof s_wp2_n);
        s_wp2_draws = s_wp2_verts = s_wp2_draw_groups = 0;
        s_wp2_conv_verts = s_wp2_conv_bytes = 0;
        s_wp2_pipe_hits = s_wp2_pipe_gets = s_wp2_pipe_created = 0;
        s_wp2_tex_slots = s_wp2_tex_uploads = 0;
        s_wp2_fetch_hit = s_wp2_fetch_miss = 0;
        s_wp2_remap_hit = s_wp2_remap_miss = 0;
    }
    fflush(stderr);
    memset(s_wp_ns, 0, sizeof s_wp_ns);
    memset(s_wp_n, 0, sizeof s_wp_n);
    s_wp_drain_ns = s_wp_dispatch_ns = s_wp_draws = s_wp_flips = 0;
    s_wp_main_draws = s_wp_mesh_draws = 0;
    s_wp_main_ns = s_wp_mesh_ns = 0;
    s_wp_pipe_create_ns = s_wp_pipe_create_n = 0;
    s_wp_frames_total = s_wp_frames_prof = 0;
    s_wp_flip = 0;
    s_wp_frames++;
}

/* ---- the capture ------------------------------------------------------- */

/* Guest main memory is the only host-backed window (runtime/memory/vm.h maps
 * the rest PROT_NONE). A page outside it cannot be imaged and is reported. */
#define WCAP_PAGE      4096u
#define WCAP_MAX_BYTES (768u * 1024u * 1024u)

typedef struct wcap_range { u32 location, offset, len, ea; } wcap_range;
typedef struct wcap_page  { u32 location, page, ea; } wcap_page;

static int  s_wcap = -1;          /* -1 unread, 0 off, 1 waiting, 2 active, 3 done */
static int  s_wcap_active;
static int  s_wcap_finish;
static int  s_wcap_wait_flip;
static int  s_wcap_start_next;
static atomic_flag s_wcap_lock = ATOMIC_FLAG_INIT;
static inline void wcap_lock(void)
{ while (atomic_flag_test_and_set_explicit(&s_wcap_lock, memory_order_acquire)) {} }
static inline void wcap_unlock(void)
{ atomic_flag_clear_explicit(&s_wcap_lock, memory_order_release); }
static int  s_wcap_polls;
static char s_wcap_prefix[448];
static rsx_wcap_state s_wcap_state;

static u32*   s_wcap_words;
static size_t s_wcap_nwords, s_wcap_cwords;

/* The replay restores the captured register file and then walks the captured
 * FIFO with *cold* engine caches, while the live capture frame ran with warm
 * ones. Two consequences break the M0 gate:
 *   - the fetch-plan memo turns a draw's span read (the note that images its
 *     guest pages) into a memo hit, so the frame never notes the vertex bytes
 *     it is about to draw; a replay of it then drops those draws as
 *     untransformable (1,278 of 1,705 backend draws, 23k read misses).
 *   - a cached texture is not re-read on every bind, but a cold replay may
 *     validate it, so its span must be imaged even if this frame only served
 *     it from the cache.
 * Flush the plan memos and note every cached texture span at the capture
 * boundary. The captured frame then performs (and notes) the same guest reads
 * a cold replay will, without changing what it renders. */
static u32 eng_texture_entry_span(const eng_texture* e);

static void wcap_note_cached_textures(void)
{
    for (u32 i = 0; i < g.n_textures; i++) {
        const eng_texture* e = &g.textures[i];
        if (!e->handle || !e->width || !e->height) continue;
        const u32 span = eng_texture_entry_span(e);
        if (span) rsx_wcap_note_texture(e->location, e->offset, span);
    }
}

static wcap_range* s_wcap_ranges;
static size_t s_wcap_nranges, s_wcap_cranges;
static wcap_page*  s_wcap_pages;
static size_t s_wcap_npages, s_wcap_cpages;

static int wcap_grow(void** p, size_t* cap, size_t need, size_t elem)
{
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap * 2 : 4096;
    while (n < need) n *= 2;
    void* q = realloc(*p, n * elem);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

static void wcap_add_range(u32 location, u32 offset, u32 len, u32 ea)
{
    if (!s_wcap_active || !len) return;
    if (!wcap_grow((void**)&s_wcap_ranges, &s_wcap_cranges,
                   s_wcap_nranges + 1, sizeof(wcap_range))) return;
    wcap_range* r = &s_wcap_ranges[s_wcap_nranges++];
    r->location = location; r->offset = offset; r->len = len; r->ea = ea;
}

void rsx_wcap_note_read(u32 location, u32 offset, u32 len, u32 ea)
{
    wcap_add_range(location, offset, len, ea);
}

void rsx_wcap_note_texture(u32 location, u32 offset, u32 len)
{
    if (!s_wcap_active || !len) return;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    wcap_add_range(location, offset, len, ea);
}

static int wcap_page_cmp(const void* a, const void* b)
{
    const wcap_page* x = (const wcap_page*)a;
    const wcap_page* y = (const wcap_page*)b;
    if (x->location != y->location) return x->location < y->location ? -1 : 1;
    if (x->page != y->page) return x->page < y->page ? -1 : 1;
    return 0;
}

/* Main memory is committed at boot; RSX local memory is demand-committed by
 * the title's own accesses (ppu_loader's fault handler). Every page the frame
 * read is therefore either already committed or legal to commit now, which is
 * what makes a local-memory image possible at all. */
static int wcap_readable(u32 ea)
{
    if (!vm_base || (u64)ea + WCAP_PAGE > 0x100000000ull) return 0;
    return vm_commit(ea, WCAP_PAGE) == 0;
}

static void wcap_missing(const char* why, u32 location, u32 offset, u32 len, u32 ea)
{
    char path[520];
    snprintf(path, sizeof path, "%s.missing", s_wcap_prefix);
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s location=%u offset=0x%08X len=%u ea=0x%08X\n",
            why, location, offset, len, ea);
    fclose(f);
}

static void wcap_write_fifo(void)
{
    char path[520];
    snprintf(path, sizeof path, "%s.fifo", s_wcap_prefix);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[walker-cap] cannot create %s\n", path); return; }
    struct {
        u32 magic, version, io_addr, io_size, frame_count, total_words, flags, extra;
    } h = { 0x46585352u, 1u, s_wcap_state.io_addr, s_wcap_state.io_size,
            1u, (u32)s_wcap_nwords, 0u, 0u };
    fwrite(&h, sizeof h, 1, f);
    for (size_t i = 0; i < s_wcap_nwords; i++) {
        u32 be = __builtin_bswap32(s_wcap_words[i]);
        fwrite(&be, 4, 1, f);
    }
    fclose(f);
}

static void wcap_write_state(void)
{
    char path[520];
    snprintf(path, sizeof path, "%s.state", s_wcap_prefix);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[walker-cap] cannot create %s\n", path); return; }
    fwrite(&s_wcap_state, sizeof s_wcap_state, 1, f);
    fclose(f);
}

static void wcap_write_mem(void)
{
    /* Every range expanded to whole pages, deduplicated by (location, page). */
    for (size_t i = 0; i < s_wcap_nranges; i++) {
        const wcap_range* r = &s_wcap_ranges[i];
        const u32 first = r->offset & ~(WCAP_PAGE - 1u);
        const u32 last  = (r->offset + r->len + WCAP_PAGE - 1u) & ~(WCAP_PAGE - 1u);
        for (u32 off = first; off < last; off += WCAP_PAGE) {
            if (!wcap_grow((void**)&s_wcap_pages, &s_wcap_cpages,
                           s_wcap_npages + 1, sizeof(wcap_page))) return;
            wcap_page* p = &s_wcap_pages[s_wcap_npages++];
            p->location = r->location;
            p->page = off;
            p->ea = r->ea ? r->ea - (r->offset - off) : 0;
        }
    }
    qsort(s_wcap_pages, s_wcap_npages, sizeof(wcap_page), wcap_page_cmp);

    /* A page is usable when its EA is in the host-backed window. Keep the
     * lowest EA for duplicate pages; two different EAs for one RSX offset in
     * one frame would already be a title bug. */
    size_t keep = 0;
    for (size_t i = 0; i < s_wcap_npages; i++) {
        if (keep && s_wcap_pages[keep - 1].location == s_wcap_pages[i].location &&
            s_wcap_pages[keep - 1].page == s_wcap_pages[i].page)
            continue;
        s_wcap_pages[keep++] = s_wcap_pages[i];
    }
    s_wcap_npages = keep;

    size_t good = 0, bytes = 0;
    for (size_t i = 0; i < s_wcap_npages; i++) {
        wcap_page* p = &s_wcap_pages[i];
        if (!wcap_readable(p->ea)) {
            wcap_missing(vm_base ? "unbacked" : "no-vm", p->location,
                         p->page, WCAP_PAGE, p->ea);
            continue;
        }
        if (bytes + WCAP_PAGE > WCAP_MAX_BYTES) {
            wcap_missing("cap", p->location, p->page, WCAP_PAGE, p->ea);
            continue;
        }
        s_wcap_pages[good++] = *p;
        bytes += WCAP_PAGE;
    }
    s_wcap_npages = good;

    char path[520];
    snprintf(path, sizeof path, "%s.mem", s_wcap_prefix);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[walker-cap] cannot create %s\n", path); return; }
    struct { u32 magic, version, count, page_bytes, data_bytes, reserved; } h = {
        0x50414357u, 1u, (u32)s_wcap_npages, WCAP_PAGE, (u32)bytes, 0u
    };
    fwrite(&h, sizeof h, 1, f);
    for (size_t i = 0; i < s_wcap_npages; i++) {
        struct { u32 location, page, len, data_off; } e = {
            s_wcap_pages[i].location, s_wcap_pages[i].page, WCAP_PAGE,
            (u32)(i * WCAP_PAGE)
        };
        fwrite(&e, sizeof e, 1, f);
    }
    for (size_t i = 0; i < s_wcap_npages; i++)
        fwrite(vm_base + s_wcap_pages[i].ea, WCAP_PAGE, 1, f);
    fclose(f);
    fprintf(stderr, "[walker-cap] wrote %s (pages=%llu, %.1f MB)\n", path,
            (unsigned long long)s_wcap_npages, (double)bytes / (1024.0 * 1024.0));
}

void rsx_wcap_word(u32 w)
{
    if (!s_wcap_active || s_wcap_finish) return;
    wcap_lock();
    if (s_wcap_active && !s_wcap_finish &&
        wcap_grow((void**)&s_wcap_words, &s_wcap_cwords,
                  s_wcap_nwords + 1, sizeof(u32)))
        s_wcap_words[s_wcap_nwords++] = w;
    wcap_unlock();
}

void rsx_wcap_flip(u32 w)
{
    /* Armed: the next drain starts a clean frame boundary. */
    if (s_wcap == 4 && s_wcap_wait_flip) {
        s_wcap_wait_flip = 0;
        s_wcap_start_next = 1;
        return;
    }
    if (!s_wcap_active || s_wcap_finish) return;
    wcap_lock();
    if (s_wcap_active && !s_wcap_finish) {
        s_wcap_finish = 1;
        if (wcap_grow((void**)&s_wcap_words, &s_wcap_cwords,
                      s_wcap_nwords + 1, sizeof(u32)))
            s_wcap_words[s_wcap_nwords++] = 0xFEAD0000u | (w & 7u);
    }
    wcap_unlock();
}

int rsx_wcap_begin_if_triggered(void)
{
    if (s_wcap < 0) {
        const char* e = getenv("PS3RECOMP_WALKER_CAPTURE");
        if (e && *e) {
            snprintf(s_wcap_prefix, sizeof s_wcap_prefix, "%s", e);
            s_wcap = 1;
        } else {
            s_wcap = 0;
        }
    }
    if (s_wcap == 1) {
        if ((s_wcap_polls++ & 0x0F) != 0) return s_wcap_active;
        char trig[520];
        snprintf(trig, sizeof trig, "%s.trigger", s_wcap_prefix);
        FILE* f = fopen(trig, "rb");
        if (!f) return s_wcap_active;
        fclose(f);
        remove(trig);
        s_wcap_wait_flip = 1;
        s_wcap = 4;
        fprintf(stderr, "[walker-cap] trigger seen; waiting for a flip boundary\n");
        fflush(stderr);
        return 0;
    }
    if (s_wcap != 4 || !s_wcap_start_next) return s_wcap_active;
    s_wcap_start_next = 0;

    memset(&s_wcap_state, 0, sizeof s_wcap_state);
    if (rsx_draw_engine_state_snapshot(&s_wcap_state) != 0) return s_wcap_active;
    /* Cold-start the frame the replay will see (see wcap_note_cached_textures). */
    s_fast_epoch++;
    wcap_note_cached_textures();
    s_wcap_state.io_addr = 0x00100000u;
    s_wcap_state.io_size = 0x00400000u;
    s_wcap_nwords = 0;
    s_wcap_nranges = 0;
    s_wcap_npages = 0;
    s_wcap_finish = 0;
    s_wcap_active = 1;
    s_wcap = 2;
    fprintf(stderr, "[walker-cap] flip boundary reached; capturing this frame\n");
    fflush(stderr);
    return 1;
}

static void wcap_finish_write(void)
{
    char missing[520];
    snprintf(missing, sizeof missing, "%s.missing", s_wcap_prefix);
    remove(missing);
    wcap_write_fifo();
    wcap_write_state();
    wcap_write_mem();
    fprintf(stderr, "[walker-cap] frame complete: %llu words, %llu read ranges\n",
            (unsigned long long)s_wcap_nwords,
            (unsigned long long)s_wcap_nranges);
    fflush(stderr);
    s_wcap_active = 0;
    s_wcap_finish = 0;
    s_wcap = 3;
}

void rsx_wcap_drain_end(void)
{
    if (s_wcap_active && s_wcap_finish) wcap_finish_write();
}

/* ---- snapshot / restore ------------------------------------------------ */

int rsx_draw_engine_state_snapshot(rsx_wcap_state* out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->magic = RSX_WCAP_MAGIC;
    out->version = 1;
    out->width = g.width;
    out->height = g.height;
    out->in_begin_end = (u32)g.rsx.in_begin_end;
    out->current_primitive = g.rsx.current_primitive;
    out->inline_len = g.rsx.inline_len;
    out->inline_dropped = g.rsx.inline_dropped;
    memcpy(out->regs, g.rsx.regs, sizeof out->regs);
    memcpy(out->vp, g.rsx.vp, sizeof out->vp);
    memcpy(out->constants, g.rsx.constants, sizeof out->constants);
    if (g.rsx.inline_len <= RSX_DSP_INLINE_MAX_BYTES)
        memcpy(out->inline_data, g.rsx.inline_data, g.rsx.inline_len);
    return 0;
}

int rsx_draw_engine_state_restore(const rsx_wcap_state* in)
{
    if (!in || in->magic != RSX_WCAP_MAGIC || in->version != 1) return -1;
    rsx_dispatch_sink sink = g.rsx.sink;
    g.rsx.in_begin_end = (int)in->in_begin_end;
    g.rsx.current_primitive = in->current_primitive;
    g.rsx.inline_len = in->inline_len;
    g.rsx.inline_dropped = in->inline_dropped;
    memcpy(g.rsx.regs, in->regs, sizeof in->regs);
    memcpy(g.rsx.vp, in->vp, sizeof in->vp);
    memcpy(g.rsx.constants, in->constants, sizeof in->constants);
    if (in->inline_len <= RSX_DSP_INLINE_MAX_BYTES)
        memcpy(g.rsx.inline_data, in->inline_data, in->inline_len);
    g.rsx.sink = sink;
    /* This path replaces the register file wholesale, bypassing
     * rsx_dispatch_method and its write counters. Bump each texture unit's
     * generation so eng_tex re-decodes instead of serving the value of the
     * previous frame's last write. (A replay restores at every frame start.) */
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) g.rsx.tex_wgen[u]++;
    return 0;
}

/* The host address of guest bytes, with no coherence step: what a copy INTO
 * guest memory for on-demand readback writes through (rsx_wcb_store). */
static const u8* eng_guest_ptr_raw(u32 location, u32 offset, u32 min_bytes)
{
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) {
        const u8* p = s_guest_reader(s_guest_user, location, offset, min_bytes);
        if (p && s_wcap_active)
            rsx_wcap_note_read(location, offset, min_bytes,
                               (u32)(uintptr_t)(p - vm_base));
        return p;
    }
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    if (s_wcap_active) rsx_wcap_note_read(location, offset, min_bytes, ea);
    return vm_base + ea;
}

/* Nonzero while ondemand/async readback has targets armed: every engine read
 * of guest memory first resolves an armed target whose pages it touches, in
 * command-stream order, rather than faulting on them. */
static int s_wcb_live;

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    const u8* p = eng_guest_ptr_raw(location, offset, min_bytes);
    if (p && s_wcb_live) rsx_wcb_access(p, min_bytes);
    return p;
}

/* PS3RECOMP_RSX_WCB_ATTR=1: why each readback ran. Every copy is counted
 * against its target and against the path that asked for it, so the log says
 * whether the walker's texture binds or the guest PPU/SPU reads cost the
 * readback time. Default off; the note path is one flag test when off. */
enum {
    ENG_WCB_TRIG_TEXBIND = 0,   /* the walker's texture bind wants guest bytes */
    ENG_WCB_TRIG_GUEST,         /* a PPU/SPU load/store faulted on the target  */
    ENG_WCB_TRIG_OTHER,         /* transfer, sync-point fallback, eviction      */
    ENG_WCB_TRIG_COUNT
};

/* The trigger of the copy currently on the stack. Only the draw engine sets
 * it, and every copy ends inside the call that set it. */
static int s_wcb_trigger = ENG_WCB_TRIG_OTHER;

typedef struct {
    int used, depth;
    u32 location, offset, fmt, w, h, pitch;
    u64 reads[ENG_WCB_TRIG_COUNT];
    u64 bytes[ENG_WCB_TRIG_COUNT];
    u64 copy_ns[ENG_WCB_TRIG_COUNT];
    u64 wait_ns;                /* guest-thread wait, taken at the flush     */
} eng_wcb_attr_row;

/* WCB mode, settings and counters (docs/wcb-async.md). `st` is cumulative;
 * `win` is the window PS3RECOMP_RSX_WCB_STATS prints and resets. */
static struct {
    int mode;                 /* -1 until read from the environment        */
    int armed_module;         /* rsx_wcb_init done                         */
    u32 hot_frames;           /* a read this recent makes a target "hot"   */
    u32 stats_every;          /* print window in frames, 0 = never         */
    u32 win_frames;
    int attr_on;              /* PS3RECOMP_RSX_WCB_ATTR                    */
    u32 attr_every, attr_frames;
    eng_wcb_attr_row attr[ENG_MAX_SURFACES + ENG_MAX_ZDEPTHS];
    void (*wake)(void);
    rsx_draw_engine_wcb_stats st, win;
    rsx_wcb_counters mod_mark;  /* module counters at the window start      */
    /* PS3RECOMP_RSX_WCB_RECT: the rectangle the transfer being resolved asked
     * for. Set on the walker thread around rsx_wcb_resolve_cookie; -1 when the
     * resolve is not a transfer's. */
    int rect_slot;
    rsx_wcb_copy_rect rect;
    /* PS3RECOMP_RSX_WCB_RECT: a rectangle-only resolve left this surface's
     * other pages stale; re-protect them once the transfer has read its
     * bytes (rsx_draw_engine_guest_read_done). */
    int partial_slot;
    /* PS3RECOMP_RSX_WCB_BATCH: this resolve is draining the request queue. */
    int batching;
} s_wcb = { .mode = -1, .rect_slot = -1, .partial_slot = -1 };

#define WCB_COUNT(field, v) do { s_wcb.st.field += (v); s_wcb.win.field += (v); } while (0)

/* The readback cuts (docs/readback-cost.md section 6): each is its own live
 * key, seeded from the environment, so a PS3RECOMP_CTL switch flips it mid-run
 * like PS3RECOMP_RSX_CUBE_RT. Off by default; a run without them behaves
 * exactly as before. */
static int eng_wcb_cut_on(int key, const char* env)
{
#if defined(__GNUC__) && !defined(_WIN32)
    if (ps3_live_ctl_get) return ps3_live_ctl_get(key);
#endif
    const char* e = getenv(env);
    return e && *e && strcmp(e, "0") != 0;
}
static int eng_wcb_rect_on(void)
{ return eng_wcb_cut_on(PS3_LIVE_WCB_RECT, "PS3RECOMP_RSX_WCB_RECT"); }
static int eng_wcb_batch_on(void)
{ return eng_wcb_cut_on(PS3_LIVE_WCB_BATCH, "PS3RECOMP_RSX_WCB_BATCH"); }
static int eng_wcb_skip_on(void)
{ return eng_wcb_cut_on(PS3_LIVE_WCB_SKIP, "PS3RECOMP_RSX_WCB_SKIP"); }
/* PS3RECOMP_RSX_WCB_STREAM: record a demand readback's image->buffer copy at
 * the end of the open draw batch and wait once, instead of flushing and
 * waiting for the batch and then submitting the copy on its own. The bytes
 * are the same (the copy is ordered after every draw before it); the cost
 * goes from two GPU round trips per readback to one. */
static int eng_wcb_stream_on(void)
{ return eng_wcb_cut_on(PS3_LIVE_WCB_STREAM, "PS3RECOMP_RSX_WCB_STREAM"); }
/* PS3RECOMP_RSX_WCB_PREFETCH: at a sync point, queue copies of the targets
 * the guest has been reading and submit the batch without waiting, so the GPU
 * runs them while the walker records the rest of the frame. The guest's read
 * then collects a copy that is already in flight instead of forcing the
 * accumulated batch to complete. */
static int eng_wcb_prefetch_on(void)
{ return eng_wcb_cut_on(PS3_LIVE_WCB_PREFETCH, "PS3RECOMP_RSX_WCB_PREFETCH"); }

static void eng_wcb_retire_surface(u32 slot);
static void eng_wcb_retire_zdepth(u32 slot);

static unsigned long long eng_wcb_now_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

/* One copy finished: count it against its target and trigger. */
static void eng_wcb_attr_note(int depth, u32 slot, u64 bytes, u64 ns)
{
    if (ps3_trace_on())
        ps3_trace_instant(PS3_TR_WCB_DONE, (uint32_t)(bytes >> 10), (uint32_t)(ns / 1000u));
    if (!s_wcb.attr_on) return;
    const u32 idx = depth ? ENG_MAX_SURFACES + slot : slot;
    if (idx >= ENG_MAX_SURFACES + ENG_MAX_ZDEPTHS) return;
    eng_wcb_attr_row* r = &s_wcb.attr[idx];
    if (!r->used) { r->used = 1; r->depth = depth; }
    if (depth) {
        const eng_zdepth* z = &g.zdepths[slot];
        r->location = z->location; r->offset = z->offset; r->fmt = z->zfmt;
        r->w = z->used_w < z->w ? z->used_w : z->w;
        r->h = z->used_h < z->h ? z->used_h : z->h;
        r->pitch = z->pitch;
    } else {
        const eng_surface* s = &g.surfaces[slot];
        r->location = s->location; r->offset = s->offset; r->fmt = (u32)s->fmt;
        r->w = s->w; r->h = s->h; r->pitch = s->pitch;
    }
    const int t = s_wcb_trigger;
    r->reads[t]++;
    r->bytes[t] += bytes;
    r->copy_ns[t] += ns;
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* The same FNV-1a over 64-bit words. The bulk inputs -- a draw's reference
 * list, a fragment program, the texture state -- are thousands of words, and
 * a byte at a time made the cache identity cost more than the work it was
 * meant to skip. */
static u64 eng_fnv1a_words(const void* data, u32 bytes, u64 hash)
{
    const u8* p = (const u8*)data;
    u32 n = bytes / 8u;
    for (u32 i = 0; i < n; i++) {
        u64 w;
        memcpy(&w, p + (size_t)i * 8u, sizeof w);
        hash ^= w;
        hash *= 1099511628211ull;
    }
    for (u32 i = n * 8u; i < bytes; i++) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/* eng_fnv1a(uc, bytes, seed) for a vertex program, reused while the bytes
 * are unchanged. A byte compare is far cheaper than the byte-serial hash. */
static u64 eng_vp_hash(const u8* uc, u32 bytes, u64 seed)
{
    if (bytes > sizeof g.vp_memo[0].copy) return eng_fnv1a(uc, bytes, seed);
    for (u32 i = 0; i < ENG_VP_MEMO; i++) {
        eng_vp_memo* m = &g.vp_memo[i];
        if (m->uc == uc && m->bytes == bytes && m->seed == seed &&
            !memcmp(m->copy, uc, bytes))
            return m->hash;
    }
    eng_vp_memo* m = &g.vp_memo[g.vp_memo_next++ % ENG_VP_MEMO];
    m->uc = uc;
    m->bytes = bytes;
    m->seed = seed;
    memcpy(m->copy, uc, bytes);
    m->hash = eng_fnv1a(uc, bytes, seed);
    return m->hash;
}

static u32 eng_pipeline_slot(u64 key)
{
    return (u32)((key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull >> 40) & (ENG_PIPELINE_INDEX - 1u);
}

/* ---- render state -------------------------------------------------------- */

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask_mrt = rsx_dsp_reg(rsx, 0x0370) & 0xFFF0u;
    rs->blend_mrt      = rsx_dsp_reg(rsx, 0x036C) & 0xEu;
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->depth_bounds_test = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE) & 1;
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(color_mask_mrt);
    ENG_HASH_FIELD(blend_mrt);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
    ENG_HASH_FIELD(depth_bounds_test);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_ptr(NULL, location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* The surface and zeta tables keep every target a title has rendered to, and
 * nothing used to take an entry out. A title that runs through enough scenes
 * fills both 64-entry tables, and a target created after that got no slot: a
 * shadow map was rendered nowhere and read back as zeros from guest memory, so
 * every shadow it casts was lost. A full table now gives up the entry used
 * longest ago, if that was before the previous frame; anything this frame or
 * the last one used stays, and with nothing older the table is too small for
 * the frame and the caller gets no slot, as before. What an evicted target
 * held and guest memory never got is gone, as it is for any address the
 * engine never saw. Returns the slot, or ENG_INVALID. */
#define ENG_EVICT_MIN_AGE 2u
static u32 eng_oldest_slot(const char* what, const u32* last_use, size_t stride, u32 n, u32 keep)
{
    u32 best = ENG_INVALID, best_use = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 use = *(const u32*)((const u8*)last_use + i * stride);
        if (i == keep || g.frames - use < ENG_EVICT_MIN_AGE) continue;
        if (best == ENG_INVALID || use < best_use) { best = i; best_use = use; }
    }
    static u32 evict_logs = 0, full_logs = 0;
    if (best != ENG_INVALID ? PS3_LOG_FIRST(evict_logs, 4) : PS3_LOG_FIRST(full_logs, 8))
        fprintf(stderr, "[rsx engine] %s table full: %s\n", what,
                best == ENG_INVALID ? "every entry was used this frame or the last, none evicted"
                                    : "evicting the least recently used");
    return best;
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690).
 *
 * A smaller clip on the same rows is the same memory, not a new surface: with
 * the same format and pitch, RSX draws into the top-left of what is already
 * there. God of War: Ascension renders its scene into a 1280x720 target, then
 * runs a 640x360 pass at the same address and pitch and copies the full
 * 1280x720 out for its SPU anti-aliasing; recreating the target at 640x360
 * lost the scene and left that quarter of the frame black.
 *
 * Only on the same sample grid, though: an antialiased target is stored at
 * sample resolution, so the same memory declared without antialiasing is a
 * different layout, and drawing it into the larger target would squeeze the
 * frame into its left part. */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt, u32 pitch, u32 aa_grid)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    u32 slot = ENG_MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            const eng_surface* s = &g.surfaces[i];
            if (s->fmt == want_fmt &&
                ((s->w == want_w && s->h == want_h) ||
                 (pitch && s->pitch == pitch && s->aa_grid == aa_grid &&
                  s->w >= want_w && s->h >= want_h))) {
                g.surfaces[i].last_use = g.frames;
                return i;
            }
            slot = i;
            break;
        }

    /* Never destroy a usable render target because one malformed command
     * decoded a guest pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 8))
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    slot < ENG_MAX_SURFACES ? "existing" : "absent");
        return slot < ENG_MAX_SURFACES ? slot : ENG_INVALID;
    }
    int evicted = 0;
    if (slot == ENG_MAX_SURFACES) {
        if (g.n_surfaces < ENG_MAX_SURFACES) {
            slot = g.n_surfaces;
        } else {
            slot = eng_oldest_slot("surface", &g.surfaces[0].last_use, sizeof(eng_surface),
                                   g.n_surfaces, g.last_present_surface);
            if (slot == ENG_INVALID) return ENG_INVALID;
            evicted = 1;
        }
    }

    eng_mt_sync();
    u32 seed_row = 0;
    const uint64_t _tr = ps3_trace_begin();
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (_tr) ps3_trace_end(_tr, PS3_TR_SURFACE_CREATE,
                           (want_w & 0xFFFFu) | (want_h << 16), (uint32_t)want_fmt);
    if (!handle)
        return (!evicted && slot < g.n_surfaces && g.surfaces[slot].handle) ? slot : ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    eng_wcb_retire_surface(slot);
    if (s->feedback) { g.be->texture_release(g.be->user, s->feedback); s->feedback = 0; }
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    s->location = location; s->offset = offset;
    s_tex_gen++;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt; s->aa_grid = aa_grid;
    s->handle = handle;
    s->color_serial = 0;
    s->last_write_serial = 0;
    s->guest_dirty = 0;
    s->guest_valid_serial = 0;
    s->guest_valid_x = s->guest_valid_y = s->guest_valid_w = s->guest_valid_h = 0;
    s->last_use = g.frames;
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 16))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : ""); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 aa_grid = sf.aa_sx | (sf.aa_sy << 8);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt, sf.color_pitch[sel], aa_grid);
    if (first == ENG_INVALID) return 0;
    g.surfaces[first].pitch = sf.color_pitch[sel];
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        /* At A's size, which may be larger than the clip (see above): the
         * framebuffer takes A's extent, and every attachment must cover it. */
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         g.surfaces[first].w, g.surfaces[first].h, fmt,
                                         sf.color_pitch[i + 1], aa_grid);
        if (slot == ENG_INVALID) break;
        g.surfaces[slot].pitch = sf.color_pitch[i + 1];
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

/* ---- coherence with guest memory around 2D transfers ------------------- */

/* Bytes per pixel of a surface a 2D transfer can touch: A8R8G8B8, and the
 * FP16 RGBA target an HDR pass renders into (Twisted Metal copies its bloom
 * out of one for the SPU post-FX). 0 for any other format. */
static u32 eng_transfer_bpp(rsx_be_format fmt)
{
    switch (fmt) {
    case RSX_BE_FMT_R8G8B8A8:      return 4u;
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    default:                       return 0u;
    }
}

/* The surface holding guest bytes [offset, offset + pitch * h), with the byte
 * position of `offset` inside its row (*xb) and its row (*y). Only a surface
 * laid out with the transfer's own pitch qualifies: another pitch puts the
 * rows elsewhere. The CLOSEST base at or below the address wins: surfaces'
 * guest ranges overlap, and an FP16 target right after an RGBA8 one must not
 * resolve into the RGBA8 one's rows (rsx_live_draw.c's resolve takes the same
 * rule, and its comment records copying out of a stale surface at 0x0 before
 * it did). */
static u32 eng_surface_at(u32 location, u32 offset, u32 pitch, u32* xb, u32* y)
{
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || offset < s->offset) continue;
        const u32 bpp = eng_transfer_bpp(s->fmt);
        if (!bpp || s->pitch != pitch) continue;
        const u32 d = offset - s->offset;
        if (d / pitch >= s->h || d % pitch >= s->w * bpp) continue;
        if (best != ENG_INVALID && g.surfaces[best].offset > s->offset) continue;
        best = i;
        *xb = d % pitch;
        *y = d / pitch;
    }
    return best;
}

static u8* s_coherence_rows;
static u32 s_coherence_cap;

/* PWCB cut 1: the page-aligned rectangle of surface pixels that covers the
 * bytes a transfer reads. The protection is page granular, so the copy is
 * expanded to the host pages the read touches; expanding per row and taking
 * the union keeps the rows a transfer never reads out of the copy. 4 KB is
 * the RSX/linux page the WCB module protects at (rsx_wcb.c: wcb_page_size;
 * a larger host page only means this copy is the conservative one). */
#define ENG_WCB_RECT_PAGE 4096ull
int rsx_wcb_transfer_rect(u32 guest_base, u32 pitch, u32 bpp, u32 xb, u32 byte_w,
                          u32 y, u32 rows, u32 surf_w, u32 surf_h,
                          rsx_wcb_copy_rect* out)
{
    if (!out || !pitch || !bpp || !byte_w || !rows || !surf_w || !surf_h)
        return 0;
    const u32 row_bytes = surf_w * bpp;
    if (xb >= row_bytes || y >= surf_h) return 0;
    if (rows > surf_h - y) rows = surf_h - y;
    u32 x0 = surf_w, x1 = 0, ry0 = surf_h, ry1 = 0;
    for (u32 r = 0; r < rows; r++) {
        const u64 row_base = (u64)guest_base + (u64)(y + r) * pitch;
        u64 a0 = row_base + xb;
        u64 a1 = a0 + byte_w;
        if (a1 > row_base + row_bytes) a1 = row_base + row_bytes;
        if (a1 <= a0) continue;
        const u64 p0 = a0 & ~(ENG_WCB_RECT_PAGE - 1ull);
        const u64 p1 = (a1 + ENG_WCB_RECT_PAGE - 1ull) & ~(ENG_WCB_RECT_PAGE - 1ull);
        u32 b0 = p0 > row_base ? (u32)(p0 - row_base) : 0u;
        u32 b1 = p1 < row_base + row_bytes ? (u32)(p1 - row_base) : row_bytes;
        if (b1 <= b0) continue;
        const u32 px0 = b0 / bpp;
        u32 px1 = (b1 + bpp - 1u) / bpp;
        if (px1 > surf_w) px1 = surf_w;
        if (px0 < x0) x0 = px0;
        if (px1 > x1) x1 = px1;
        if (y + r < ry0) ry0 = y + r;
        if (y + r + 1u > ry1) ry1 = y + r + 1u;
    }
    if (x1 <= x0 || ry1 <= ry0) return 0;
    out->x = x0; out->y = ry0; out->w = x1 - x0; out->h = ry1 - ry0;
    return 1;
}

/* The rectangle the guest-memory copy of `s` is known to match, at which
 * colour serial. A guest write clears it (the bytes are no longer the ones
 * the engine stored). */
static void eng_guest_valid_set(eng_surface* s, u32 x, u32 y, u32 w, u32 h)
{
    s->guest_valid_serial = s->color_serial;
    s->guest_valid_x = x; s->guest_valid_y = y;
    s->guest_valid_w = w; s->guest_valid_h = h;
}

static void eng_guest_valid_clear(eng_surface* s)
{
    s->guest_valid_serial = 0;
    s->guest_valid_x = s->guest_valid_y = s->guest_valid_w = s->guest_valid_h = 0;
}

static int eng_guest_valid_covers(const eng_surface* s, u32 x, u32 y, u32 w, u32 h)
{
    return s->guest_valid_serial && s->guest_valid_serial == s->color_serial &&
           x >= s->guest_valid_x && y >= s->guest_valid_y &&
           x + w <= s->guest_valid_x + s->guest_valid_w &&
           y + h <= s->guest_valid_y + s->guest_valid_h;
}

/* PS3RECOMP_LIGHT_PROBE=1: log every guest readback of a small lighting
 * render target -- the game measures exposure and sky light from 32x32 GPU
 * renders (ToneMapSample, QuickAmbientSky, HarmonicsSample) and reads their
 * mip chain on the CPU. Each line names the containing target (or the callsite
 * refinement), the rectangle the guest got, the decoded RGBA bytes it read,
 * and whether our guest copy was made at the surface's current colour serial.
 * `stored` is the serial guest memory held before this copy, `ticket` the
 * serial of an async copy fetched instead of the live surface, `covers`
 * whether the previous copy already covered the rectangle, and `fresh`
 * whether the bytes handed over are from the current serial. Default off;
 * only 32x32 and <=2x2 targets are logged so the shadow 8x8 copies stay out
 * of the run. */
static int s_light_probe = -1;
static int eng_light_probe_on(void)
{
    if (s_light_probe < 0) {
        const char* e = getenv("PS3RECOMP_LIGHT_PROBE");
        s_light_probe = e && *e && strcmp(e, "0") != 0;
    }
    return s_light_probe;
}

static void eng_light_probe_log(u32 slot, const eng_surface* s, u32 x, u32 y,
                                u32 w, u32 h, const char* how, const u8* rows,
                                u32 row_px, u64 stored, int covers, u64 ticket)
{
    if (!eng_light_probe_on()) return;
    if (!((s->w == 32 && s->h == 32) || s->w * s->h <= 4u)) return;
    const u32 bpp = eng_transfer_bpp(s->fmt);
    double mr = -1.0, mg = -1.0, mb = -1.0, luma = -1.0;
    u32 p0 = 0;
    if (rows && bpp == 4u && w && h && row_px >= w) {
        u64 r = 0, g = 0, b = 0;
        const u64 np = (u64)w * h;
        for (u32 ry = 0; ry < h; ry++) {
            const u8* p = rows + (size_t)ry * row_px * 4u;
            for (u32 rx = 0; rx < w; rx++, p += 4) {
                r += p[0]; g += p[1]; b += p[2];
            }
        }
        mr = (double)r / (double)np;
        mg = (double)g / (double)np;
        mb = (double)b / (double)np;
        luma = 0.2126 * mr + 0.7152 * mg + 0.0722 * mb;
        const u8* q = rows;
        p0 = ((u32)q[3] << 24) | ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
    }
    const int fresh = s->guest_valid_serial && s->guest_valid_serial == s->color_serial;
    fprintf(stderr, "[light probe] frame=%u slot=%u loc=%u addr=0x%08X fmt=0x%02X"
                    " %ux%u pitch=%u how=%s rect=%u,%u %ux%u mean=%.2f,%.2f,%.2f"
                    " luma=%.2f p0=%08X serial=%llu stored=%llu ticket=%llu"
                    " covers=%d fresh=%d dirty=%d\n",
            g.frames, slot, s->location, s->offset, s->fmt, s->w, s->h, s->pitch,
            how, x, y, w, h, mr, mg, mb, luma, p0,
            (unsigned long long)s->color_serial, (unsigned long long)stored,
            (unsigned long long)ticket, covers, fresh, s->guest_dirty);
}

/* PS3RECOMP_RSX_WCB_RECT_LOG=<n>: the first n transfers, with the rectangle
 * the guest asked for and the page-aligned one the copy covers. */
static int s_rect_log_left = -1;
static void eng_wcb_rect_log(u32 slot, const eng_surface* s, u32 xb, u32 bytes,
                             u32 y, u32 h, int pending, const rsx_wcb_copy_rect* rr)
{
    if (s_rect_log_left < 0) {
        const char* e = getenv("PS3RECOMP_RSX_WCB_RECT_LOG");
        s_rect_log_left = e ? atoi(e) : 0;
    }
    if (s_rect_log_left <= 0) return;
    s_rect_log_left--;
    fprintf(stderr, "[rsx wcb rect] slot=%u loc=%u addr=0x%08X surf %ux%u pitch=%u"
                    " ask xb=%u bytes=%u y=%u h=%u pending=%d copy=%s",
            slot, s->location, s->offset, s->w, s->h, s->pitch, xb, bytes, y, h,
            pending, rr ? "" : "whole\n");
    if (rr)
        fprintf(stderr, "%u,%u %ux%u\n", rr->x, rr->y, rr->w, rr->h);
}

/* RSX_ENGINE_COHERENCE_LOG=<n>: log the first n transfers the engine syncs,
 * with how many non-zero pixels moved -- the question when a composite comes
 * out black is whether the GPU surface had anything in it. */
static int eng_coherence_log(void)
{
    static int left = -1;
    if (left < 0) { const char* e = getenv("RSX_ENGINE_COHERENCE_LOG"); left = e ? atoi(e) : 0; }
    return left > 0 ? left-- : 0;
}

/* Pixels of `bpp` bytes whose colour (the first three quarters: R,G,B) is
 * not all zero. */
static u32 eng_nonzero_pixels(const u8* rows, u32 n, u32 bpp)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++, rows += bpp) {
        u8 any = 0;
        for (u32 b = 0; b < bpp * 3u / 4u; b++) any |= rows[b];
        k += any != 0;
    }
    return k;
}

static u8* eng_coherence_buffer(u32 bytes)
{
    if (s_coherence_cap < bytes) {
        u8* n = (u8*)realloc(s_coherence_rows, bytes);
        if (!n) return NULL;
        s_coherence_rows = n;
        s_coherence_cap = bytes;
    }
    return s_coherence_rows;
}

/* One row of 32-bit pixels between the backend's R,G,B,A and the guest's
 * A8R8G8B8 bytes (or straight through with TEX_RGBA). Copied a byte at a
 * time, with the TEX_RGBA test inside the loop, these two conversions were
 * about a quarter of the FIFO thread's time while Twisted Metal booted; at
 * the -O2 the runtime builds with, GCC vectorises neither form. Each pixel is
 * one 32-bit rotate instead: bytes s0..s3 load as s0 | s1<<8 | s2<<16 | s3<<24
 * on a little-endian host (ps3emu/endian.h), so rotating left by 8 gives
 * s3,s0,s1,s2 (to the guest) and right by 8 gives s1,s2,s3,s0 (from it). */
static void eng_row_convert(u8* dst, const u8* src, u32 w, int rgba, int to_guest)
{
    if (rgba) { memcpy(dst, src, (size_t)w * 4u); return; }
    for (u32 c = 0; c < w; c++) {
        u32 v;
        memcpy(&v, src + (size_t)c * 4u, 4);
        v = to_guest ? (v << 8) | (v >> 24) : (v >> 8) | (v << 24);
        memcpy(dst + (size_t)c * 4u, &v, 4);
    }
}

/* Rows read back from a colour surface (`n` surface pixels of `bpp` bytes
 * each per row, R,G,B,A order, contiguous), converted in place to the bytes
 * RSX leaves in memory and stored at `pitch`, `bytes` per row starting `skip`
 * bytes into each. `forced`: the destination may still be protected for
 * on-demand readback, so write through rsx_wcb_store, as one write when the
 * rows are already laid out exactly as guest memory is. */
static void eng_color_rows_store(u32 bpp, u32 location, u32 offset, u32 pitch, u8* rows,
                                 u32 n, u32 h, u32 skip, u32 bytes, int forced)
{
    const int rgba = rsx_texture_argb_is_rgba();
    const size_t stride = (size_t)n * bpp;
    for (u32 r = 0; r < h; r++) {
        u8* src = rows + (size_t)r * stride;
        if (bpp == 8u) {
            /* R,G,B,A halves, each stored big-endian: the layout
             * rsx_texture_decode reads W16_Z16_Y16_X16_FLOAT from. */
            for (u32 c = 0; c < n * 4u; c++) {
                const u8 t = src[2 * c]; src[2 * c] = src[2 * c + 1]; src[2 * c + 1] = t;
            }
        } else if (!rgba) {
            eng_row_convert(src, src, n, 0, 1);   /* in place: one pixel per load/store */
        }
    }
    if (forced && !skip && bytes == stride && bytes == pitch && h) {
        u8* dst = (u8*)eng_guest_ptr_raw(location, offset, pitch * h);
        if (dst) { rsx_wcb_store(dst, rows, (size_t)pitch * h); return; }
    }
    for (u32 r = 0; r < h; r++) {
        const u8* src = rows + (size_t)r * stride + skip;
        if (forced) {
            u8* dst = (u8*)eng_guest_ptr_raw(location, offset + r * pitch, bytes);
            if (!dst) return;
            rsx_wcb_store(dst, src, bytes);
        } else {
            u8* dst = (u8*)eng_guest_ptr(NULL, location, offset + r * pitch, bytes);
            if (!dst) return;
            rsx_texdirty_note_write_host(dst, bytes);
            memcpy(dst, src, bytes);
        }
    }
}

void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    eng_mt_sync();
    if (!g.ready || !g.be->readback || !pitch || !w || !h) return;
    u32 xb = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &xb, &y);
    if (slot == ENG_INVALID) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u: no surface\n",
                    location, offset, w, h);
        return;
    }
    eng_surface* s = &g.surfaces[slot];
    const u32 bpp = eng_transfer_bpp(s->fmt);
    /* The transfer counts 32-bit pixels, so w * 4 bytes per row; an FP16
     * surface pixel is two of them. Read back every surface pixel those bytes
     * touch, then store exactly the bytes. */
    u32 bytes = w * 4u;
    if (xb + bytes > s->w * bpp) bytes = s->w * bpp - xb;
    if (y + h > s->h) h = s->h - y;
    const u32 x = xb / bpp, skip = xb % bpp;
    const u32 n = (skip + bytes + bpp - 1u) / bpp;
    const int skip_on = eng_wcb_skip_on();
    /* Armed for on-demand readback: copy, in stream order, and reopen the
     * pages. With the skip cut, a copy whose serial is still the one guest
     * memory holds (and which covers this rectangle) is not made again. */
    if (s->wcb_pending) {
        if (skip_on && eng_guest_valid_covers(s, x, y, n, h)) {
            rsx_wcb_release(slot);
            s->wcb_pending = 0;
            WCB_COUNT(skipped, 1);
            s_wcb.partial_slot = (int)slot;   /* the rest stays stale: re-arm */
            eng_light_probe_log(slot, s, x, y, n, h, "armedskip", NULL, 0,
                                s->guest_valid_serial, 1, 0);
            return;
        }
        if (eng_wcb_rect_on()) {
            rsx_wcb_copy_rect rr;
            const int have = rsx_wcb_transfer_rect(s->offset, s->pitch, bpp, xb, bytes,
                                                   y, h, s->w, s->h, &rr);
            eng_wcb_rect_log(slot, s, xb, bytes, y, h, 1, have ? &rr : NULL);
            if (have) {
                s_wcb.rect_slot = (int)slot;
                s_wcb.rect = rr;
                const int done = rsx_wcb_resolve_cookie(slot, RSX_WCB_REASON_EXPLICIT);
                s_wcb.rect_slot = -1;
                if (done) return;
            }
        }
        if (rsx_wcb_resolve_cookie(slot, RSX_WCB_REASON_EXPLICIT)) return;
    }
    /* Not armed: the target has not been drawn since the last copy, and that
     * copy still covers this rectangle. */
    if (skip_on && eng_guest_valid_covers(s, x, y, n, h)) {
        WCB_COUNT(skipped, 1);
        eng_light_probe_log(slot, s, x, y, n, h, "readskip", NULL, 0,
                            s->guest_valid_serial, 1, 0);
        return;
    }
    const u64 probe_stored = s->guest_valid_serial;
    const int probe_covers = eng_guest_valid_covers(s, x, y, n, h);
    u8* rows = eng_coherence_buffer(n * bpp * h);
    if (!rows) return;
    const unsigned long long t0 = eng_wcb_now_ns();
    int streamed = 0;
    if (eng_wcb_stream_on() && g.be->readback_async && g.be->readback_async_fetch) {
        const u32 t = g.be->readback_async(g.be->user, s->handle, x, y, n, h);
        if (t) {
            WCB_COUNT(async_issued, 1);
            if (g.be->readback_async_fetch(g.be->user, t, rows, n * bpp) == 0) {
                streamed = 1;
                WCB_COUNT(async_fetched, 1);
            } else {
                WCB_COUNT(async_discarded, 1);
            }
        }
    }
    if (!streamed) {
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, x, y, n, h, rows, n * bpp);
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u <- surface 0x%08X (%u bpp) at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, bpp, x, y,
                eng_nonzero_pixels(rows, n * h, bpp));
    eng_color_rows_store(bpp, location, offset, pitch, rows, n, h, skip, bytes, 0);
    eng_guest_valid_set(s, x, y, n, h);
    eng_light_probe_log(slot, s, x, y, n, h, streamed ? "stream" : "direct", rows, n,
                        probe_stored, probe_covers, 0);
    /* The copy is now what guest memory holds. A rectangle that did not cover
     * the target leaves the rest stale, so the pages must be armed again
     * before the next read (the transfer's own read is still to come). This
     * only matters to the readback cuts; with both off nothing changes here,
     * exactly as before them. */
    if (eng_wcb_rect_on() || eng_wcb_skip_on()) {
        if (x == 0 && y == 0 && n >= s->w && h >= s->h) {
            s->guest_dirty = 0;
            if (s_wcb.partial_slot == (int)slot) s_wcb.partial_slot = -1;
        } else {
            s->guest_dirty = 1;
            s_wcb.partial_slot = (int)slot;
        }
    }
    WCB_COUNT(copied, 1);
    WCB_COUNT(bytes, (unsigned long long)pitch * h);
    {
        const unsigned long long done = eng_wcb_now_ns();
        WCB_COUNT(copy_ns, done - t0);
        eng_wcb_attr_note(0, slot, (u64)pitch * h, done - t0);
    }
}

/* PWCB cut 1: the transfer that read a rectangle has finished reading guest
 * memory. A rectangle-only resolve left the rest of the target stale and the
 * pages open; take them away again so a later read of another part faults and
 * is copied instead of reading old bytes. Called by cellGcmSys around
 * nv3089_blit, on the walker (owner) thread. */
void rsx_draw_engine_guest_read_done(void)
{
    const int slot = s_wcb.partial_slot;
    s_wcb.partial_slot = -1;
    s_wcb.rect_slot = -1;
    if (slot < 0 || (u32)slot >= g.n_surfaces) return;
    eng_surface* s = &g.surfaces[(u32)slot];
    if (!s->handle || s->wcb_pending || !s->guest_dirty) return;
    const u32 bpp = eng_transfer_bpp(s->fmt);
    if (!bpp || s->pitch < s->w * bpp || !s->w || !s->h) return;
    const u32 span = s->pitch * (s->h - 1u) + s->w * bpp;
    void* host = (void*)eng_guest_ptr_raw(s->location, s->offset, span);
    if (host && rsx_wcb_protect((u32)slot, host, span) == 0) {
        s->wcb_pending = 1;
        WCB_COUNT(armed, 1);
    }
}

void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    eng_mt_sync();
    if (!g.ready || !g.be->color_target_write || !pitch || !w || !h) return;
    u32 x = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &x, &y);
    /* color_target_write takes R,G,B,A bytes: an A8R8G8B8 surface only. */
    if (slot == ENG_INVALID || g.surfaces[slot].fmt != RSX_BE_FMT_R8G8B8A8) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u: no A8R8G8B8 surface\n",
                    location, offset, w, h);
        return;
    }
    x /= 4u;
    eng_surface* s = &g.surfaces[slot];
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    /* Guest bytes replaced the target's: a copy made from the surface no
     * longer describes guest memory (PWCB cut 3). */
    if (eng_wcb_skip_on()) eng_guest_valid_clear(s);
    u8* rows = eng_coherence_buffer(w * 4u * h);
    if (!rows) return;
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 r = 0; r < h; r++) {
        const u8* src = eng_guest_ptr(NULL, location, offset + r * pitch, w * 4u);
        if (!src) return;
        eng_row_convert(rows + (size_t)r * w * 4u, src, w, rgba, 0);
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u -> surface 0x%08X at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, x, y,
                eng_nonzero_pixels(rows, w * h, 4u));
    g.be->color_target_write(g.be->user, s->handle, x, y, w, h, rows, w * 4u);
    /* A 2D transfer is a write to this target too; the present trace needs to
     * know the frame is newer than the one last presented. */
    s->last_write_serial = ++g.write_serial;
    if (s_ptrace > 0) s->color_serial = s->last_write_serial;
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static void eng_zdepth_extend(eng_zdepth* z, u32 rt_w, u32 rt_h)
{
    if (!rt_w) rt_w = g.width;
    if (!rt_h) rt_h = g.height;
    if (rt_w > z->ext_w) z->ext_w = rt_w < z->w ? rt_w : z->w;
    if (rt_h > z->ext_h) z->ext_h = rt_h < z->h ? rt_h : z->h;
}

static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) {
                z->last_use = g.frames;
                eng_zdepth_extend(z, rt_w, rt_h);
                return i;
            }
            slot = i;
            if (want_w < z->w) want_w = z->w;
            if (want_h < z->h) want_h = z->h;
            break;
        }
    }
    int evicted = 0;
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths < ENG_MAX_ZDEPTHS) {
            slot = g.n_zdepths;
        } else {
            slot = eng_oldest_slot("zeta", &g.zdepths[0].last_use, sizeof(eng_zdepth),
                                   g.n_zdepths, ENG_INVALID);
            if (slot == ENG_INVALID) return ENG_INVALID;
            evicted = 1;
        }
    }

    eng_mt_sync();
    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (!evicted && slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    eng_wcb_retire_zdepth(slot);
    /* The old size's snapshot goes with it: it can never be valid again. */
    if (z->snapshot && g.be->texture_release) g.be->texture_release(g.be->user, z->snapshot);
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    if (z->conv) g.be->texture_release(g.be->user, z->conv);
    if (z->snapshot) g.be->texture_release(g.be->user, z->snapshot);
    z->conv = 0;
    z->location = location; z->offset = offset;
    s_tex_gen++;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->depth_serial = 0;
    z->guest_dirty = 0;
    z->last_use = g.frames;
    z->ext_w = z->ext_h = 0;
    eng_zdepth_extend(z, rt_w, rt_h);

    if (slot == g.n_zdepths) g.n_zdepths++;
    return slot;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
static u32 eng_zdepth_snapshot(u32 slot, u32 tex_w, u32 tex_h)
{
    eng_mt_sync();
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    /* The texture covers the top-left tex_w x tex_h of the zeta. The host
     * depth target is at least canvas-sized (eng_zdepth_get), so a 128x160
     * shadow map lives in the corner of a 1280x720 image; resolving all of it
     * stretched the texture's normalized coordinates over the whole canvas
     * and the lookups landed outside the map. The snapshot is the region the
     * texture names, copied 1:1 from the top-left (as the Vulkan backend's
     * copy already does). */
    const u32 w = (tex_w && tex_w < z->w) ? tex_w : z->w;
    const u32 h = (tex_h && tex_h < z->h) ? tex_h : z->h;
    if (z->snapshot_valid && z->snapshot && z->snap_w == w && z->snap_h == h)
        return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    /* The stale copy goes. A depth clear invalidates the snapshot and the
     * next read takes a new one, so a shadow map cleared and read every frame
     * used to leak one backend texture a frame per depth target, until the
     * backend's object table filled and new textures came back as 0. The
     * Metal backend keeps a released object until no recorded draw can still
     * name it; the Vulkan one hands back the same handle, refilled. */
    if (z->snapshot && z->snapshot != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->snapshot);
    z->snapshot = tex;
    z->snap_w = w;
    z->snap_h = h;
    z->snapshot_valid = 1;
    return tex;
}

/* PS3RECOMP_ENG_DEPTH_VIA_GUEST=1 (read at engine init): a depth target read
 * as A8R8G8B8 or DEPTH16 goes through guest memory even when the backend can
 * convert it itself (eng_zdepth_as_color). */
static int s_depth_via_guest;

/* A Z24S8 depth target read as a linear A8R8G8B8 texture of its own pitch:
 * a deferred renderer rebuilding depth from the bytes. Storing it into guest
 * memory for that (eng_zdepth_to_guest) waits for the GPU, converts on the CPU
 * and decodes and uploads the texture again, every time the depth changed:
 * at several reads a frame that is most of the FIFO thread. The backend
 * converts it on the GPU instead, in command order, into the texels the
 * decode would give, and the unit samples a view of that with its own
 * crossbar. Guest memory is left as it was (guest_dirty stays set for any
 * other reader). That holds for the zeta still bound too: a title may read
 * its depth with depth writes off and the zeta left in place, and the
 * conversion, in command order, takes the depth as the draws so far left it,
 * which is what the store reads back.
 * A Z16 depth target read as a linear DEPTH16 texture of its own pitch, a
 * shadow map, takes the same way: without it every read of a freshly
 * rendered shadow map is a wait for the GPU plus a decode and an upload of
 * up to 2 MB. Returns the view, or 0 for the store through guest memory. */
static u32 eng_zdepth_as_color(u32 slot, const rsx_dsp_texture* t)
{
    eng_mt_sync();
    eng_zdepth* z = &g.zdepths[slot];
    const u32 base_fmt = t->format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
    const int z16 = z->zfmt == 1u;      /* SURFACE_FORMAT Z16 */
    const int bytes = base_fmt == RSX_TEX_FMT_A8R8G8B8 && !z16 && !rsx_texture_argb_is_rgba();
    const int d16 = base_fmt == RSX_TEX_FMT_DEPTH16 && z16;
    if (s_depth_via_guest || !g.be->depth_as_color || !g.be->surface_view ||
        !z->handle || !(bytes || d16) || !(t->format & RSX_TEX_FMT_LINEAR) ||
        t->cubemap || t->dimension == 3 || t->mipmaps > 1)
        return 0;
    const u32 used_w = z->used_w < z->w ? z->used_w : z->w;
    const u32 used_h = z->used_h < z->h ? z->used_h : z->h;
    if (!t->width || !t->height || t->width > used_w || t->height > used_h ||
        t->pitch != z->pitch)
        return 0;
    if (!z->conv || z->conv_serial != z->depth_serial || z->conv_fmt != base_fmt ||
        z->conv_w != t->width || z->conv_h != t->height) {
        const u32 tex = g.be->depth_as_color(g.be->user, z->handle, t->width, t->height,
                                             base_fmt);
        if (!tex) return 0;
        if (z->conv) g.be->texture_release(g.be->user, z->conv);
        z->conv = tex;
        z->conv_w = t->width; z->conv_h = t->height; z->conv_fmt = base_fmt;
        z->conv_serial = z->depth_serial;
    }
    return g.be->surface_view(g.be->user, z->conv, t->remap & 0xFFFFu, t->format);
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* A 3D texture: each level is `depth` slices (halving with the level), one
 * face_bytes each -- consecutive for a linear image, one Morton block for a
 * swizzled one. */
static u32 eng_texture_span_3d(u32 fmt, u32 w, u32 h, u32 depth, u32 levels, u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    u32 total = 0, d = depth ? depth : 1u;
    for (u32 m = 0; m < n; m++) { total += lv[m].tl.face_bytes * d; if (d > 1) d >>= 1; }
    return total;
}

/* Slice z of a swizzled 3D level, de-interleaved into row-major rows of
 * w * bpp bytes, so the 2D decode can convert it as a linear image. */
static void eng_deswizzle_3d_slice(u8* dst, const u8* src, u32 w, u32 h, u32 d, u32 z, u32 bpp)
{
    u32 lw = 0, lh = 0, ld = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    while ((1u << ld) < d) ld++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            memcpy(dst + ((size_t)y * w + x) * bpp,
                   src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    *readable = 1;
    return hash;
}

/* The cheap per-frame change check: 64 bytes out of every 2 KB, plus the
 * last 64. A rewrite of the whole texture (a video frame, a UI redraw, a
 * streamed replacement) changes it at once; a small write between the
 * sampled blocks is left to the full hash, which every texture still gets
 * every ENG_TEXHASH_FULL_EVERY frames. */
#define ENG_TEXHASH_STRIDE      2048u
#define ENG_TEXHASH_FULL_EVERY  8u          /* a power of two */
#define ENG_TEXHASH_SMALL       16384u      /* at or below: full every frame */
static u64 eng_texture_sparse_hash(u32 location, u32 offset, u32 span,
                                   int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i + 64 <= span; i += ENG_TEXHASH_STRIDE)
        for (u32 k = 0; k < 64; k += 8) {
            u64 word;
            memcpy(&word, src + i + k, sizeof(word));
            hash = (hash ^ word) * 1099511628211ull;
        }
    for (u32 i = span > 64 ? span - 64 : 0; i < span; i++)
        hash = (hash ^ src[i]) * 1099511628211ull;
    *readable = 1;
    return hash;
}

/* Whether a cached texture gets the full hash at this use: always when it
 * is small or under PS3RECOMP_ENG_TEXHASH_FULL=1 (the old behaviour, read
 * at engine init), otherwise once ENG_TEXHASH_FULL_EVERY frames have passed
 * since its last one. New entries start staggered by slot, so the full
 * hashes of a scene's textures spread over the frames instead of landing on
 * the same one. */
static int s_texhash_full_always;
static int eng_texture_full_rehash(u32 span, u32 last_full_frame)
{
    return s_texhash_full_always || span <= ENG_TEXHASH_SMALL ||
           (u32)(g.frames - last_full_frame) >= ENG_TEXHASH_FULL_EVERY;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* Why the last eng_texture_slot answer was 0. Set on every failing return so
 * the census in sink_bind_textures can attribute the dummy fallback. */
static unsigned s_tex_miss_reason;

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. `into`, when nonzero, is an
 * existing 2D texture of exactly this key to rewrite instead of creating one
 * (rsx_draw_backend.texture_upload_ordered). */
static u32 eng_texture_upload_work(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                   u32 levels, u32 pitch, int cube, u32 remap, u32 depth,
                                   u32 into)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) { s_tex_miss_reason = RSX_TEX_MISS_FORMAT; return 0; }
    /* A volume: every slice of every level decoded and uploaded on its own.
     * inFAMOUS 2's occlusion volumes read an 8x8x8 cone LUT; uploaded as its
     * first slice and sampled in 2D it darkened whole boxes round every prop. */
    if (!into && depth > 1 && !cube && g.be->texture_create_3d && g.be->texture_upload_3d &&
        !lv[0].tl.compressed && lv[0].tl.bytes_per_texel) {
        eng_mt_sync();
        const u32 span = eng_texture_span_3d(fmt, w, h, depth, levels, pitch);
        const u8* src = eng_guest_ptr(NULL, location, offset, span);
        if (!src) { s_tex_miss_reason = RSX_TEX_MISS_ADDRESS; return 0; }
        const u32 handle = g.be->texture_create_3d(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   w, h, depth, nlv, remap, fmt);
        if (!handle) { s_tex_miss_reason = RSX_TEX_MISS_OTHER; return 0; }
        const u8* level_src = src;
        u32 d = depth;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            const u32 lin_bytes = tl->swizzled ? tl->row_bytes * tl->rows : 0u;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows + lin_bytes)) {
                g.be->texture_release(g.be->user, handle);
                s_tex_miss_reason = RSX_TEX_MISS_OTHER;
                return 0;
            }
            for (u32 z = 0; z < d; z++) {
                if (tl->swizzled) {
                    u8* lin = g.tex_staging + tl->dst_row_bytes * tl->rows;
                    eng_deswizzle_3d_slice(lin, level_src, lv[m].w, lv[m].h, d, z, tl->bytes_per_texel);
                    rsx_tex_layout ltl = *tl; ltl.swizzled = 0;
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes, lin, lv[m].w, lv[m].h,
                                       &ltl, rsx_texture_argb_is_rgba());
                } else {
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                                       level_src + (size_t)z * tl->face_bytes, lv[m].w, lv[m].h,
                                       tl, rsx_texture_argb_is_rgba());
                }
                g.be->texture_upload_3d(g.be->user, handle, z, m, lv[m].w, lv[m].h,
                                        g.tex_staging, tl->dst_row_bytes);
            }
            level_src += (size_t)tl->face_bytes * d;
            if (d > 1) d >>= 1;
        }
        return handle;
    }
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_ptr(NULL, location, offset, span);
    if (!src) { s_tex_miss_reason = RSX_TEX_MISS_ADDRESS; return 0; }

    eng_mt_sync();
    const u32 handle = into ? into
        : g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt), w, h, nlv, faces, remap, fmt);
    if (!handle) { s_tex_miss_reason = RSX_TEX_MISS_OTHER; return 0; }

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                if (!into) g.be->texture_release(g.be->user, handle);
                s_tex_miss_reason = RSX_TEX_MISS_OTHER;
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

/* ---- cube render targets (PS3RECOMP_RSX_CUBE_RT) -------------------------
 *
 * The dummy census found the dark players: an 8x8, one-mip cube texture whose
 * guest address is also a colour target. A TextureCube unit cannot sample the
 * 2-D target, so the backend reads the dummy. The game renders the six faces
 * as ordinary colour targets -- either six targets at the texture's face
 * stride, or one stacked target -- so assemble them on the GPU: one six-layer
 * cube image, one copy per face, refreshed when a face's colour serial moves.
 */
#define ENG_CUBE_FACES 6

typedef struct {
    rsx_be_cube_face face[ENG_CUBE_FACES];
    u64 serials[ENG_CUBE_FACES];
    rsx_be_format fmt;
    u32 w, h;              /* face size                                    */
    u32 found, complete;   /* bitmasks of faces with a target / usable one */
    u32 stride;            /* the face stride that matched, or 0           */
} eng_cube_desc;

static int s_cube_rt_env = -1;
static int s_cube_log = -1;

/* Live: PS3RECOMP_CTL can flip this mid-run (the A/B arms do); the
 * environment value seeds the live key. */
static int eng_cube_rt_on(void)
{
#if defined(__GNUC__) && !defined(_WIN32)
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_CUBE_RT);
#endif
    if (s_cube_rt_env < 0) {
        const char* e = getenv("PS3RECOMP_RSX_CUBE_RT");
        s_cube_rt_env = e && *e && strcmp(e, "0") != 0 ? 1 : 0;
    }
    return s_cube_rt_env;
}

static int eng_cube_log_on(void)
{
    if (s_cube_log < 0) { const char* e = getenv("PS3RECOMP_CUBE_RT_LOG");
                          s_cube_log = e && *e && strcmp(e, "0") != 0 ? 1 : 0; }
    return s_cube_log;
}

static u32 eng_surface_at_exact(u32 location, u32 offset)
{
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (s->handle && s->location == location && s->offset == offset) return i;
    }
    return ENG_INVALID;
}

/* The six faces of `t` when at least its base address is a colour target.
 * Returns 1 and fills `d`; `complete` says whether all six can be copied.
 * The face layout is tried in the orders the hardware can leave it in:
 * six targets one face-stride apart (the usual cube-RT setup), then one
 * stacked target with the faces in rows (or columns). */
static int eng_cube_collect(const rsx_dsp_texture* t, eng_cube_desc* d)
{
    eng_mt_sync();
    memset(d, 0, sizeof *d);
    const u32 base = eng_surface_at_exact(t->location, t->offset);
    if (base == ENG_INVALID) return 0;

    const u32 w = t->width, h = t->height;
    d->w = w; d->h = h;
    d->fmt = g.surfaces[base].fmt;
    d->serials[0] = g.surfaces[base].color_serial;

    u32 stride[3];
    int ns = 0;
    const u32 s_texture = rsx_texture_cube_face_stride(t->format, w, h,
                                                       t->mipmaps, t->pitch);
    const u32 s_pitch = t->pitch * h;
    const u32 s_base = g.surfaces[base].pitch * h;
    if (s_texture) stride[ns++] = s_texture;
    if (s_pitch && (ns == 0 || s_pitch != stride[0])) stride[ns++] = s_pitch;
    if (s_base && (ns == 0 || (s_base != stride[0] && (ns < 2 || s_base != stride[1]))))
        stride[ns++] = s_base;

    /* Six separate targets. A face must have been written (serial) and be at
     * least face-sized; extra pixels are ignored. */
    const u32 all = (1u << ENG_CUBE_FACES) - 1u;
    for (int c = 0; c < ns; c++) {
        u32 found = 0, complete = 0;
        for (u32 f = 0; f < ENG_CUBE_FACES; f++) {
            const u64 addr = (u64)t->offset + (u64)f * stride[c];
            if (addr > 0xFFFFFFFFu) break;
            const u32 slot = eng_surface_at_exact(t->location, (u32)addr);
            if (slot == ENG_INVALID) continue;
            const eng_surface* s = &g.surfaces[slot];
            found |= 1u << f;
            if (s->fmt != d->fmt || s->w < w || s->h < h || !s->color_serial) continue;
            complete |= 1u << f;
            d->face[f].handle = s->handle;
            d->face[f].x = 0; d->face[f].y = 0;
            d->face[f].w = w; d->face[f].h = h;
            d->serials[f] = s->color_serial;
        }
        if (found == all && complete == found) {
            d->found = found; d->complete = complete; d->stride = stride[c];
            return 1;
        }
        if (found > d->found) { d->found = found; d->complete = complete; d->stride = 0; }
    }

    /* One stacked target: rows (or, failing that, columns), each face a
     * window of the same colour serial. */
    {
        const eng_surface* s = &g.surfaces[base];
        if (s->fmt == d->fmt && s->color_serial && s->w >= w &&
            s->h >= (u64)h * ENG_CUBE_FACES) {
            for (u32 f = 0; f < ENG_CUBE_FACES; f++) {
                d->face[f].handle = s->handle;
                d->face[f].x = 0; d->face[f].y = f * h;
                d->face[f].w = w; d->face[f].h = h;
                d->serials[f] = s->color_serial;
            }
            d->found = d->complete = all;
            d->stride = 0;
            return 1;
        }
        if (s->fmt == d->fmt && s->color_serial && s->h >= h &&
            s->w >= (u64)w * ENG_CUBE_FACES) {
            for (u32 f = 0; f < ENG_CUBE_FACES; f++) {
                d->face[f].handle = s->handle;
                d->face[f].x = f * w; d->face[f].y = 0;
                d->face[f].w = w; d->face[f].h = h;
                d->serials[f] = s->color_serial;
            }
            d->found = d->complete = all;
            d->stride = 0;
            return 1;
        }
    }
    return 1;   /* base target exists, but the six faces are not all there */
}

static void eng_cube_log_desc(const rsx_dsp_texture* t, const eng_cube_desc* d,
                              const char* what)
{
    static u32 last_frame = 0xFFFFFFFFu;
    static u32 last_off = 0xFFFFFFFFu;
    if (g.frames == last_frame && t->offset == last_off) return;
    last_frame = g.frames; last_off = t->offset;
    fprintf(stderr, "[cube-rt] frame=%u %s addr=0x%08X %ux%u fmt=0x%02X stride=0x%X "
                    "found=0x%02X complete=0x%02X\n",
            g.frames, what, t->offset, d->w, d->h, t->format, d->stride,
            d->found, d->complete);
    for (u32 f = 0; f < ENG_CUBE_FACES; f++) {
        const eng_surface* s = NULL;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].handle == d->face[f].handle) {
                s = &g.surfaces[i]; break;
            }
        fprintf(stderr, "[cube-rt]   face %u %s handle=%u %ux%u at %u,%u serial=%llu "
                        "writer_fp=0x%016llx pipe=%u\n",
                f, (d->complete >> f) & 1u ? "ready" : "missing",
                d->face[f].handle, d->face[f].w, d->face[f].h,
                d->face[f].x, d->face[f].y,
                (unsigned long long)d->serials[f],
                s ? (unsigned long long)s->last_writer_fp : 0ull,
                s ? s->last_writer_pipe : 0u);
    }
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
/* The g.textures index eng_texture_slot's last nonzero answer came from, or
 * ENG_INVALID. */
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 depth,
                              u32 into)
{
    const unsigned long long wp2 = wp2_t0();
    const uint64_t _tr = ps3_trace_begin();
    const u32 handle = eng_texture_upload_work(location, offset, fmt, w, h, levels,
                                               pitch, cube, remap, depth, into);
    if (_tr) {
        uint64_t bytes = (uint64_t)w * h * 4u * (cube ? 6u : 1u) * (depth ? depth : 1u);
        ps3_trace_end(_tr, PS3_TR_TEX_UPLOAD, (uint32_t)(bytes >> 10),
                      levels ? levels : 1u);
    }
    if (wp2) { wp2_add(WPROF2_TEX_UPLOAD, wp2); s_wp2_tex_uploads++; }
    return handle;
}

/* PS3RECOMP_TEX_DIRTY: live mode, the paranoid hash cadence in frames, and
 * the counters that check reports. The mode is a live key so an A/B can flip
 * it in one launch; the environment seeds it at startup. */
static int s_texdirty_env = -1;
static int s_texdirty_live;
static int s_texdirty_paranoid;
static u64 s_texdirty_stale, s_texdirty_checked;
static u32 s_texdirty_log_frame;

static int eng_texdirty_env_on(void)
{
    if (s_texdirty_env < 0) {
        const char* e = getenv("PS3RECOMP_TEX_DIRTY");
        s_texdirty_env = e && *e && strcmp(e, "0") != 0 ? 1 : 0;
    }
    return s_texdirty_env;
}

static int eng_texdirty_want(void)
{
#if defined(__GNUC__) && !defined(_WIN32)
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_TEX_DIRTY);
#endif
    return eng_texdirty_env_on();
}

static u32 eng_texture_entry_span(const eng_texture* e)
{
    return e->depth > 1
        ? eng_texture_span_3d(e->format, e->width, e->height, e->depth, e->levels, e->pitch)
        : eng_texture_span(e->format, e->width, e->height, e->levels, e->pitch, e->cubemap);
}

/* The guest EA the entry's bytes are decoded from, or 0 when the location is
 * not one the 32-bit guest map resolves. */
static u32 eng_texture_ea(u32 location, u32 offset)
{
    return cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
}

/* Apply a live/environment mode change to every cached entry. Enabling
 * protects the spans and makes them re-upload once (their bytes are current,
 * but this establishes the protection); disabling uncovers and forces the
 * old hash path to establish its reference hash again. */
static void eng_texdirty_sync(void)
{
    int want = eng_texdirty_want();
    if (s_guest_reader) want = 0;       /* replay reads through cap_read */
    if (want == s_texdirty_live) return;
    if (want) {
        if (rsx_texdirty_set_enabled(1) != 0) {
            fprintf(stderr, "[texdirty] unavailable on this host; keeping hashes\n");
            s_texdirty_env = 0;
            s_texdirty_live = 0;
            return;
        }
        const char* pe = getenv("PS3RECOMP_TEX_DIRTY_PARANOID");
        s_texdirty_paranoid = pe ? atoi(pe) : 0;
        rsx_texdirty_publish_hook();
        u32 covered = 0;
        for (u32 i = 0; i < g.n_textures; i++) {
            eng_texture* e = &g.textures[i];
            if (!e->handle) continue;
            const u32 span = eng_texture_entry_span(e);
            const u32 ea = eng_texture_ea(e->location, e->offset);
            if (!span || !ea) continue;
            e->ea = ea;
            e->dirty_covered = 1;
            e->dirty_seq = rsx_texdirty_seq();
            e->prot_serial = rsx_texdirty_serial();
            e->rewritten = 1;
            e->hash_valid = 0;
            rsx_texdirty_cover(ea, span);
            covered++;
        }
        s_texdirty_live = 1;
        s_texdirty_log_frame = g.frames;
        fprintf(stderr, "[texdirty] on: %u cached textures covered, paranoid=%d\n",
                covered, s_texdirty_paranoid);
    } else {
        for (u32 i = 0; i < g.n_textures; i++) {
            eng_texture* e = &g.textures[i];
            if (!e->dirty_covered) continue;
            rsx_texdirty_uncover(e->ea, eng_texture_entry_span(e));
            e->dirty_covered = 0;
            e->rewritten = 1;
            e->hash_valid = 0;
            e->content_hash = ~(u64)0;
            e->sparse_hash = ~(u64)0;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;
        }
        rsx_texdirty_set_enabled(0);
        s_texdirty_live = 0;
        fprintf(stderr, "[texdirty] off: paranoid checked=%llu stale=%llu\n",
                (unsigned long long)s_texdirty_checked, (unsigned long long)s_texdirty_stale);
        s_texdirty_checked = s_texdirty_stale = 0;
    }
}

static u32 s_tslot_last = ENG_INVALID;
static u32 eng_texture_slot_work(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                 u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    s_tslot_last = ENG_INVALID;
    s_tex_miss_reason = RSX_TEX_MISS_OTHER;
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) {
        s_tex_miss_reason = RSX_TEX_MISS_SIZE;
        return 0;
    }
    if (!depth) depth = 1;
    const u32 span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                               : eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) {
        s_tex_miss_reason = RSX_TEX_MISS_FORMAT;
        return 0;
    }

    eng_texdirty_sync();

    /* A scene binds the same few hundred textures every frame, and a linear
     * scan of the whole cache per bind averaged several hundred entries.
     * Remember where each key was last found; a remembered slot is used only
     * if its entry still matches, otherwise the scan runs as before. Keys are
     * unique in the cache, so the result is the same either way. */
    static u16 s_slot_memo[4096];
    const u32 memo = (offset >> 4 ^ offset >> 16 ^ fmt * 0x9E37u ^ w * 31u ^ h ^
                      location << 11) & 4095u;
#define ENG_TEXTURE_KEY_MATCHES(e) ((e)->location == location && (e)->offset == offset && \
        (e)->format == fmt && (e)->width == w && (e)->height == h && \
        (e)->pitch == pitch && (e)->remap == remap && \
        (e)->cubemap == (u32)(cube != 0) && (e)->depth == depth)
    eng_texture* hit = NULL;
    if (s_slot_memo[memo] < g.n_textures && ENG_TEXTURE_KEY_MATCHES(&g.textures[s_slot_memo[memo]])) {
        hit = &g.textures[s_slot_memo[memo]];
    } else {
        for (u32 i = 0; i < g.n_textures; i++)
            if (ENG_TEXTURE_KEY_MATCHES(&g.textures[i])) {
                hit = &g.textures[i];
                s_slot_memo[memo] = (u16)i;
                break;
            }
    }
#undef ENG_TEXTURE_KEY_MATCHES
    if (hit) {
        eng_texture* e = hit;
        if (e->handle && e->last_hash_frame != g.frames) {
            if (s_texdirty_live && e->dirty_covered) {
                /* Generation check: any page written since the sequence this
                 * upload sampled makes it stale. No bytes are read here. */
                int need = 0;
                const u32 espan = eng_texture_entry_span(e);
                if (e->rewritten) {
                    need = 1;
                    e->rewritten = 0;
                } else if (rsx_texdirty_max_seq(e->ea, espan) > e->dirty_seq) {
                    need = 1;
                }
                const int gen_need = need;
                /* Debug: prove the generations are not missing writes. The
                 * content hash is only taken here and at upload, so the
                 * steady-state cost is unchanged. */
                if (s_texdirty_paranoid &&
                    (u32)(g.frames - e->last_full_frame) >= (u32)s_texdirty_paranoid) {
                    int r = 0;
                    const u64 ch = eng_texture_content_hash(location, offset, espan, &r);
                    s_texdirty_checked++;
                    if (r && e->hash_valid && ch != e->content_hash) {
                        /* Only a change the generations missed is stale; a
                         * generation-driven upload is supposed to differ. */
                        if (!gen_need) {
                            s_texdirty_stale++;
                            fprintf(stderr, "[texdirty] STALE 0x%08X %ux%u hash %016llx != %016llx\n",
                                    offset, w, h, (unsigned long long)ch,
                                    (unsigned long long)e->content_hash);
                        }
                        need = 1;
                    }
                    if (r) { e->content_hash = ch; e->hash_valid = 1; e->last_full_frame = g.frames; }
                    if (g.frames - s_texdirty_log_frame >= 60u) {
                        s_texdirty_log_frame = g.frames;
                        fprintf(stderr, "[texdirty] paranoid frame=%u checked=%llu stale=%llu\n",
                                g.frames, (unsigned long long)s_texdirty_checked,
                                (unsigned long long)s_texdirty_stale);
                    }
                }
                if (need) {
                    rsx_texdirty_reprotect(e->ea, espan);
                    const u64 seq0 = rsx_texdirty_seq();
                    const u32 into = (g.be->texture_upload_ordered && depth <= 1 &&
                                      e->levels == levels) ? e->handle : 0u;
                    const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                         levels, pitch, cube, remap, depth, into);
                    if (fresh && !into) {
                        s_tex_gen++;
                        eng_mt_sync();
                        g.be->texture_release(g.be->user, e->handle);
                        e->handle = fresh;
                        if (e->levels != levels) {
                            rsx_texdirty_uncover(e->ea, espan);
                            e->ea = eng_texture_ea(location, offset);
                            e->levels = levels;
                            rsx_texdirty_cover(e->ea, eng_texture_entry_span(e));
                        }
                    }
                    if (fresh) {
                        e->dirty_seq = seq0;
                        if (s_texdirty_paranoid) {
                            int r = 0;
                            e->content_hash = eng_texture_content_hash(location, offset,
                                                                       eng_texture_entry_span(e), &r);
                            e->hash_valid = (u8)r;
                            e->last_full_frame = g.frames;
                        }
                    }
                } else if (rsx_texdirty_serial() != e->prot_serial) {
                    rsx_texdirty_reprotect(e->ea, espan);
                }
                e->prot_serial = rsx_texdirty_serial();
                e->last_hash_frame = g.frames;
            } else {
                /* Every cached texture used to be hashed in full every frame;
                 * in a big scene that is hundreds of MB a second for the few
                 * that change. Now the sparse check every frame, and the full
                 * hash when it differs, when this entry's turn has come, for a
                 * small texture, or after a write the engine saw. */
                int readable = 0;
                u64 hash = e->content_hash;
                const u64 sparse = eng_texture_sparse_hash(location, offset, span,
                                                           &readable);
                /* The engine just stored a surface or depth target over these
                 * bytes: they are new, so upload without hashing them all. The
                 * stored hash becomes unknown and the next ordinary check
                 * re-uploads once rather than miss a change. */
                if (e->rewritten && readable) {
                    hash = ~e->content_hash;
                    e->rewritten = 0;
                } else if (readable && (sparse != e->sparse_hash ||
                                 eng_texture_full_rehash(span, e->last_full_frame))) {
                    hash = eng_texture_content_hash(location, offset, span, &readable);
                    e->last_full_frame = g.frames;
                }
                if (readable) e->sparse_hash = sparse;
                e->last_hash_frame = g.frames;
                if (readable && hash != e->content_hash) {
                    /* In place when the backend orders the upload after the draws
                     * that sampled the old bytes; a volume keeps create-and-swap,
                     * and so does a different mip count (the key does not hold it,
                     * and the levels to upload would not all exist). */
                    const u32 into = (g.be->texture_upload_ordered && depth <= 1 &&
                                      e->levels == levels) ? e->handle : 0u;
                    const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                         levels, pitch, cube, remap, depth, into);
                    if (fresh && !into) {
                        s_tex_gen++;
                        eng_mt_sync();
                        g.be->texture_release(g.be->user, e->handle);
                        e->handle = fresh;
                        e->levels = levels;
                    }
                    if (fresh) e->content_hash = hash;
                }
            }
        }
        e->last_use_serial = ++g.texture_use_serial;
        s_tslot_last = (u32)(e - g.textures);
        return e->handle;
    }

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        /* PS3RECOMP_RSX_TEX_PIN (#136): never evict an entry bound this
         * frame. A texture created mid-frame can otherwise evict the very
         * one the draw that needed it just bound, forcing a re-upload
         * cascade (the NHL port's puck-drop stall). Empty slots are taken
         * first above; when every entry is pinned this frame, fall back to
         * the plain LRU rather than refusing the bind. */
        static u32 s_pin_frame = 0xFFFFFFFFu;
        static u64 s_pin_base;
        if (s_pin_frame != g.frames) { s_pin_frame = g.frames; s_pin_base = g.texture_use_serial; }
        const int pin = eng_wcb_cut_on(PS3_LIVE_TEX_PIN, "PS3RECOMP_RSX_TEX_PIN");
        index = 0;
        u32 oldest = 0xFFFFFFFFu; u64 oldest_use = 0;
        int have_unpinned = 0;
        for (u32 i = 0; i < g.n_textures; i++) {
            const u64 use = g.textures[i].last_use_serial;
            if (pin && use > s_pin_base) {          /* bound this frame */
                if (!have_unpinned && (oldest == 0xFFFFFFFFu || use < oldest_use)) {
                    oldest = i; oldest_use = use;
                }
                continue;
            }
            if (!have_unpinned || use < oldest_use) {
                index = i; oldest_use = use; have_unpinned = 1;
            }
        }
        if (!have_unpinned && oldest != 0xFFFFFFFFu) index = oldest;
        evicted = g.textures[index].handle;
        if (g.textures[index].dirty_covered) {
            rsx_texdirty_uncover(g.textures[index].ea,
                                 eng_texture_entry_span(&g.textures[index]));
            g.textures[index].dirty_covered = 0;
        }
    }

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0); e.depth = depth; e.levels = levels;
    e.last_hash_frame = g.frames;
    e.last_full_frame = g.frames - (index & (ENG_TEXHASH_FULL_EVERY - 1u));
    e.last_use_serial = ++g.texture_use_serial;
    e.ea = eng_texture_ea(location, offset);
    if (s_texdirty_live && e.ea) {
        /* Protect before decoding, so a write during the upload is caught. */
        e.dirty_covered = 1;
        rsx_texdirty_cover(e.ea, span);
        e.dirty_seq = rsx_texdirty_seq();
        e.prot_serial = rsx_texdirty_serial();
        e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                      cube, remap, depth, 0u);
        if (!e.handle) {
            rsx_texdirty_uncover(e.ea, span);
            e.dirty_covered = 0;
        }
        if (e.handle && s_texdirty_paranoid) {
            int readable = 0;
            e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
            e.hash_valid = (u8)readable;
            e.last_full_frame = g.frames;
        }
    } else {
        int readable = 0;
        e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
        e.sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable);
        e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                      cube, remap, depth, 0u);
    }
    if (e.handle && evicted) { eng_mt_sync(); g.be->texture_release(g.be->user, evicted); s_tex_gen++; }
    if (e.handle || !evicted) g.textures[index] = e;
    if (e.handle) s_tslot_last = index;
    return e.handle;
}

static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    const unsigned long long wp2 = wp2_t0();
    const u32 handle = eng_texture_slot_work(location, offset, fmt, w, h, levels,
                                             pitch, cube, remap, depth);
    if (wp2) { wp2_add(WPROF2_TEX_SLOT, wp2); s_wp2_tex_slots++; }
    return handle;
}

/* ---- surfaces read through guest memory --------------------------------- */

/* A texture may name a surface's bytes in a format the surface was not
 * rendered in: a deferred renderer reads its D24S8 depth buffer as A8R8G8B8
 * to rebuild positions, samples its Z16 shadow map as DEPTH16, and reads back
 * as X32_FLOAT a float it packed into an RGBA8 target. On RSX that is only
 * memory. Here the surface is stored into guest memory in its own layout --
 * the bytes RSX would have left there -- and the texture is then decoded from
 * guest memory like any other, so every format the decoder knows reads them
 * the way RSX would. A surface is stored at most once per write to it. */

/* Re-hash, on their next use rather than next frame, the cached textures that
 * start inside [offset, offset + bytes): their bytes changed within this one. */
static void eng_textures_touch(u32 location, u32 offset, u32 bytes)
{
    for (u32 i = 0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location == location && e->offset >= offset && e->offset - offset < bytes) {
            e->last_hash_frame = g.frames - 1u;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;   /* hash it all */
            e->rewritten = 1;
            s_tex_gen++;
        }
    }
}

/* Formats that read an RGBA8 surface's 32-bit pixels as something else: the
 * host image cannot be sampled as them, the guest bytes can. */
static int eng_format_reinterprets_rgba8(u32 base_fmt)
{
    switch (base_fmt) {
    case RSX_TEX_FMT_DEPTH24_D8:
    case 0x91u:     /* DEPTH24_D8_FLOAT */
    case 0x92u:     /* DEPTH16          */
    case 0x93u:     /* DEPTH16_FLOAT    */
    case 0x95u:     /* Y16_X16          */
    case 0x9Cu:     /* X32_FLOAT        */
    case 0x9Fu:     /* Y16_X16_FLOAT    */
        return 1;
    default:
        return 0;
    }
}

/* Store a colour surface into guest memory, A,R,G,B per pixel at its own
 * pitch. Returns 0 when it cannot, and the caller samples as before. */
static int eng_surface_to_guest(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    /* Armed for on-demand readback: the resolve copies it, in stream order. */
    if (s->wcb_pending && rsx_wcb_resolve_cookie(slot, RSX_WCB_REASON_EXPLICIT)) return 1;
    if (!s->guest_dirty) return 1;
    const u32 bpp = eng_transfer_bpp(s->fmt);
    if (!bpp || s->pitch < s->w * bpp || !g.be->readback)
        return 0;
    rsx_draw_engine_guest_read(s->location, s->offset, s->pitch, s->w, s->h);
    s->guest_dirty = 0;
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    return 1;
}

static float* s_depth_rows;
static u32 s_depth_cap;

/* One row of depth values, clamped to [0, 1] (NaN to 0), quantized and stored
 * big-endian: Z16 as 16 bits, Z24S8 as 24 bits of depth and a zero stencil
 * byte. The format test is hoisted out of the pixel loops and the clamps are
 * selects, so both loops vectorize; a large depth target can be read back
 * several times a frame. */
static void eng_depth_row_to_guest(u8* dst, const float* src, u32 w, u32 bpp)
{
    if (bpp == 2u) {
        for (u32 c = 0; c < w; c++) {
            float d = src[c];
            d = d > 0.0f ? d : 0.0f;
            d = d < 1.0f ? d : 1.0f;
            /* Exactly round(d * 0xFFFF), as depth_as_color takes it: in
             * float the product rounds first and a value within 0.002 of a
             * half could go the other way. */
            const u32 v = (u32)((double)d * 65535.0 + 0.5);
            dst[2u * c] = (u8)(v >> 8); dst[2u * c + 1u] = (u8)v;
        }
        return;
    }
    for (u32 c = 0; c < w; c++) {
        float d = src[c];
        d = d > 0.0f ? d : 0.0f;
        d = d < 1.0f ? d : 1.0f;
        /* Float32 rounding at this magnitude can turn depth 1 into
         * 0x1000000. Quantize in double before storing the 24-bit value. */
        const u32 v = (u32)((double)d * 16777215.0 + 0.5);
        dst[4u * c] = (u8)(v >> 16); dst[4u * c + 1u] = (u8)(v >> 8);
        dst[4u * c + 2u] = (u8)v; dst[4u * c + 3u] = 0;
    }
}

/* Store a depth target into guest memory as its zeta format lays it out:
 * Z16 as one big-endian 16-bit value, Z24S8 as a big-endian word with the
 * depth in the top 24 bits. The stencil byte is written as 0: the host depth
 * target keeps no stencil to copy. Returns 0 when it cannot. */
static u8* s_depth_bytes;
static size_t s_depth_bytes_cap;

/* The guest-memory span a depth target's copy covers: used clip, zeta pitch. */
static int eng_zdepth_span(const eng_zdepth* z, u32* w, u32* h, u32* bpp)
{
    *bpp = (z->zfmt == 1u) ? 2u : 4u;               /* SURFACE_FORMAT Z16 = 1 */
    *w = z->used_w < z->w ? z->used_w : z->w;
    *h = z->used_h < z->h ? z->used_h : z->h;
    return *w && *h && z->pitch >= *w * *bpp;
}

/* `forced`: an on-demand resolve, whose destination is still protected. */
static int eng_zdepth_store(u32 slot, int forced)
{
    eng_mt_sync();
    eng_zdepth* z = &g.zdepths[slot];
    if (!g.be->depth_readback || !z->handle) return 0;
    u32 w, h, bpp;
    if (!eng_zdepth_span(z, &w, &h, &bpp)) return 0;
    const unsigned long long t0 = eng_wcb_now_ns();
    if (s_depth_cap < w * h) {
        float* n = (float*)realloc(s_depth_rows, (size_t)w * h * sizeof(float));
        if (!n) return 0;
        s_depth_rows = n;
        s_depth_cap = w * h;
    }
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, s_depth_rows,
                             w * (u32)sizeof(float)) != 0)
        return 0;
    if (forced) {
        /* Converted whole, then written through the protection: one write
         * when the rows are packed at the zeta pitch, else one per row. */
        const size_t row = (size_t)w * bpp, need = row * h;
        if (s_depth_bytes_cap < need) {
            u8* n = (u8*)realloc(s_depth_bytes, need);
            if (!n) return 0;
            s_depth_bytes = n;
            s_depth_bytes_cap = need;
        }
        for (u32 r = 0; r < h; r++)
            eng_depth_row_to_guest(s_depth_bytes + r * row, s_depth_rows + (size_t)r * w, w, bpp);
        u8* all = row == z->pitch ? (u8*)eng_guest_ptr_raw(z->location, z->offset, (u32)need) : NULL;
        if (all) {
            rsx_wcb_store(all, s_depth_bytes, need);
        } else {
            for (u32 r = 0; r < h; r++) {
                u8* dst = (u8*)eng_guest_ptr_raw(z->location, z->offset + r * z->pitch, (u32)row);
                if (!dst) return 0;
                rsx_wcb_store(dst, s_depth_bytes + r * row, row);
            }
        }
    } else {
        for (u32 r = 0; r < h; r++) {
            u8* dst = (u8*)eng_guest_ptr(NULL, z->location, z->offset + r * z->pitch, w * bpp);
            if (!dst) return 0;
            rsx_texdirty_note_write_host(dst, w * bpp);
            eng_depth_row_to_guest(dst, s_depth_rows + (size_t)r * w, w, bpp);
        }
    }
    z->guest_dirty = 0;
    WCB_COUNT(copied, 1);
    WCB_COUNT(bytes, (unsigned long long)z->pitch * h);
    {
        const unsigned long long done = eng_wcb_now_ns();
        WCB_COUNT(copy_ns, done - t0);
        eng_wcb_attr_note(1, slot, (u64)z->pitch * h, done - t0);
    }
    eng_textures_touch(z->location, z->offset, z->pitch * h);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] depth 0x%08X (%s, pitch %u) %ux%u stored to guest memory\n",
                z->offset, bpp == 2u ? "Z16" : "Z24S8", z->pitch, w, h);
    return 1;
}

static int eng_zdepth_to_guest(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (z->wcb_pending && rsx_wcb_resolve_cookie(0x10000u | slot, RSX_WCB_REASON_EXPLICIT)) return 1;
    if (!z->guest_dirty) return 1;
    return eng_zdepth_store(slot, 0);
}

/* A depth write or clear through the current zeta, recorded with what the
 * guest-memory copy needs to lay it out. */
static void eng_zdepth_wrote(u32 slot, const rsx_dsp_surface* sf)
{
    eng_zdepth* z = &g.zdepths[slot];
    z->depth_serial = ++g.write_serial;
    if (!z->guest_dirty) WCB_COUNT(marked_dirty, 1);
    z->guest_dirty = 1;
    z->zfmt = sf->depth_format;
    z->pitch = sf->zeta_pitch;
    z->used_w = sf->clip_w ? sf->clip_w : g.width;
    z->used_h = sf->clip_h ? sf->clip_h : g.height;
}

static void eng_surfaces_wrote(const u32* targets, u32 n_targets)
{
    const u64 serial = ++g.write_serial;
    for (u32 i = 0; i < n_targets; i++) {
        g.surfaces[targets[i]].color_serial = serial;
        g.surfaces[targets[i]].last_write_serial = serial;
        if (!g.surfaces[targets[i]].guest_dirty) WCB_COUNT(marked_dirty, 1);
        g.surfaces[targets[i]].guest_dirty = 1;
    }
}

/* ---- write colour buffers: on-demand readback ---------------------------
 *
 * docs/wcb-async.md has the design. In short: at a sync point a dirty target
 * is not copied; its guest pages are protected (rsx_wcb_protect). The first
 * guest touch of those pages faults, and eng_wcb_resolve -- always on this,
 * the engine's, thread -- copies the target and reopens them. In async mode a
 * target the guest read recently also gets a GPU copy queued at the sync
 * point, which the read then only collects. */

#define WCB_COOKIE_DEPTH 0x10000u

static int eng_wcb_parse_mode(const char* e)
{
    if (!e || !*e) return RSX_WCB_OFF;
    if (!strcmp(e, "off"))      return RSX_WCB_OFF;
    if (!strcmp(e, "sync"))     return RSX_WCB_SYNC;
    if (!strcmp(e, "ondemand")) return RSX_WCB_ONDEMAND;
    if (!strcmp(e, "async"))    return RSX_WCB_ASYNC;
    const int v = atoi(e);
    return v < RSX_WCB_OFF ? RSX_WCB_OFF : (v > RSX_WCB_ASYNC ? RSX_WCB_SYNC : v);
}

int rsx_draw_engine_wcb_mode(void)
{
    if (s_wcb.mode < 0) {
        s_wcb.mode = eng_wcb_parse_mode(getenv("PS3RECOMP_RSX_WCB"));
        const char* h = getenv("PS3RECOMP_RSX_WCB_HOT_FRAMES");
        s_wcb.hot_frames = h && atoi(h) > 0 ? (u32)atoi(h) : 8u;
        const char* st = getenv("PS3RECOMP_RSX_WCB_STATS");
        const int every = st ? atoi(st) : 0;
        s_wcb.stats_every = every <= 0 ? 0u : (every > 1000 ? 1000u : (u32)every);
        const char* at = getenv("PS3RECOMP_RSX_WCB_ATTR");
        s_wcb.attr_on = at && *at && strcmp(at, "0") != 0;
        s_wcb.attr_every = s_wcb.stats_every ? s_wcb.stats_every : 60u;
        if (s_wcb.attr_on) {
            rsx_wcb_attr_set(1);
            fprintf(stderr, "[rsx wcb] readback attribution on, %u-frame windows\n",
                    s_wcb.attr_every);
        }
        if (s_wcb.mode)
            fprintf(stderr, "[rsx wcb] mode %s%s\n",
                    s_wcb.mode == RSX_WCB_SYNC ? "sync (copy every dirty target at every sync point)" :
                    s_wcb.mode == RSX_WCB_ONDEMAND ? "ondemand (copy on first guest access)" :
                    "async (ondemand; queue copies of recently read targets at sync points)",
                    s_wcb.stats_every ? ", stats on" : "");
    }
    return s_wcb.mode;
}

void rsx_draw_engine_wcb_set_mode(int mode)
{
    rsx_draw_engine_wcb_mode();     /* settle the other knobs from the environment */
    s_wcb.mode = mode < RSX_WCB_OFF ? RSX_WCB_OFF : (mode > RSX_WCB_ASYNC ? RSX_WCB_ASYNC : mode);
}

static void eng_wcb_resolve(void* user, u32 cookie, int reason);

void rsx_draw_engine_wcb_set_wake(void (*wake)(void))
{
    s_wcb.wake = wake;
    if (s_wcb.armed_module) rsx_wcb_init(eng_wcb_resolve, NULL, wake);
}

static int eng_wcb_hot(const eng_surface* s)
{
    return s->wcb_reads && g.frames - s->wcb_last_read <= s_wcb.hot_frames;
}

static void eng_wcb_drop_ticket(eng_surface* s)
{
    if (!s->wcb_ticket) return;
    if (g.be->readback_async_release) g.be->readback_async_release(g.be->user, s->wcb_ticket);
    s->wcb_ticket = 0;
    WCB_COUNT(async_discarded, 1);
}

/* Copy colour surface `slot` into guest memory through the protection. The
 * rectangle is the transfer's when PS3RECOMP_RSX_WCB_RECT named one, else the
 * whole target. `ticket`: take the bytes from the async copy queued at the
 * sync point instead of reading the surface as it is now. */
static int eng_wcb_copy_surface(u32 slot, int use_ticket)
{
    eng_surface* s = &g.surfaces[slot];
    const u32 bpp = eng_transfer_bpp(s->fmt);
    if (!bpp || !s->handle || s->pitch < s->w * bpp || !s->w || !s->h) return 0;
    u32 rx = 0, ry = 0, rw = s->w, rh = s->h;
    if (s_wcb.rect_slot == (int)slot) {
        rx = s_wcb.rect.x; ry = s_wcb.rect.y;
        rw = s_wcb.rect.w; rh = s_wcb.rect.h;
        s_wcb.rect_slot = -1;
        if (!rw || !rh || rx + rw > s->w || ry + rh > s->h) { rx = ry = 0; rw = s->w; rh = s->h; }
    }
    const int full = rx == 0 && ry == 0 && rw == s->w && rh == s->h;
    const u64 probe_stored = s->guest_valid_serial;
    const int probe_covers = eng_guest_valid_covers(s, rx, ry, rw, rh);
    const u64 probe_ticket = (use_ticket && full && s->wcb_ticket)
                             ? s->wcb_ticket_serial : 0;
    /* The serial has not moved since the copy that already covers this
     * rectangle, and no guest write has replaced its bytes. */
    if (!use_ticket && eng_wcb_skip_on() && eng_guest_valid_covers(s, rx, ry, rw, rh)) {
        s->guest_dirty = full ? 0 : 1;
        if (!full) s_wcb.partial_slot = (int)slot;
        WCB_COUNT(skipped, 1);
        eng_light_probe_log(slot, s, rx, ry, rw, rh, "skip", NULL, 0,
                            probe_stored, probe_covers, 0);
        return 1;
    }
    const unsigned long long t0 = eng_wcb_now_ns();
    u8* rows = eng_coherence_buffer(rw * bpp * rh);
    if (!rows) return 0;
    int from_ticket = 0;
    int streamed = 0;
    if (use_ticket && full && s->wcb_ticket && g.be->readback_async_fetch) {
        const u32 t = s->wcb_ticket;
        s->wcb_ticket = 0;
        if (g.be->readback_async_fetch(g.be->user, t, rows, rw * bpp) == 0) {
            from_ticket = 1;
        } else {
            WCB_COUNT(async_discarded, 1);  /* fetch frees the ticket either way */
        }
    }
    if (!from_ticket && !use_ticket && eng_wcb_stream_on() &&
        g.be->readback_async && g.be->readback_async_fetch) {
        /* PWCB stream: queue the copy at the end of the open draw batch, then
         * flush that batch and wait on it once. The copy is ordered after the
         * draws recorded before it by vk_cmd_after_everything, so the bytes
         * are the ones the old flush-then-copy path produced. */
        eng_wcb_drop_ticket(s);
        const u32 t = g.be->readback_async(g.be->user, s->handle, rx, ry, rw, rh);
        if (t) {
            WCB_COUNT(async_issued, 1);
            if (g.be->readback_async_fetch(g.be->user, t, rows, rw * bpp) == 0) {
                streamed = 1;
                WCB_COUNT(async_fetched, 1);
            } else {
                WCB_COUNT(async_discarded, 1);
            }
        }
    }
    if (!from_ticket && !streamed) {
        eng_wcb_drop_ticket(s);
        if (!g.be->readback) return 0;
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, rx, ry, rw, rh, rows, rw * bpp);
    }
    eng_color_rows_store(bpp, s->location, s->offset + ry * s->pitch + rx * bpp,
                         s->pitch, rows, rw, rh, 0, rw * bpp, 1);
    /* Drawn again since the queued copy: guest memory holds the sync point's
     * bytes, and the next sync point arms it again for the newer ones. A
     * rectangle-only copy leaves the rest stale and stays dirty either way. */
    s->guest_dirty = full ? (from_ticket && s->color_serial != s->wcb_ticket_serial) : 1;
    if (!full) s_wcb.partial_slot = (int)slot;
    else if (s_wcb.partial_slot == (int)slot) s_wcb.partial_slot = -1;
    eng_guest_valid_set(s, rx, ry, rw, rh);
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    eng_light_probe_log(slot, s, rx, ry, rw, rh,
                        full ? (from_ticket ? "ticket" :
                                streamed ? "stream" :
                                use_ticket ? "guestreq" : "copy") : "rect",
                        rows, rw, probe_stored, probe_covers, probe_ticket);
    WCB_COUNT(copied, 1);
    WCB_COUNT(bytes, (unsigned long long)s->pitch * rh);
    if (from_ticket) WCB_COUNT(async_fetched, 1);
    if (!full) WCB_COUNT(rect_copies, 1);
    {
        const unsigned long long done = eng_wcb_now_ns();
        WCB_COUNT(copy_ns, done - t0);
        eng_wcb_attr_note(0, slot, (u64)s->pitch * rh, done - t0);
    }
    return 1;
}

/* PWCB cut 2: one submission for all the readbacks a fault/transfer pass
 * needs. Each surface is queued as an asynchronous copy into the open draw
 * batch (PS3RECOMP_RSX_WCB_BATCH); the first fetch submits that batch, so N
 * 4 KB mip copies cost one wait instead of N. The regions stay protected
 * until their bytes have been stored, so no waiter sees stale guest memory. */
#define ENG_WCB_BATCH_MAX 16
typedef struct {
    u32 slot, ticket;
    u32 x, y, w, h;
} eng_wcb_bjob;
static eng_wcb_bjob s_bjobs[ENG_WCB_BATCH_MAX];
static u32 s_nbjobs;

/* Queue one surface's copy; 1 when it is handled (queued or skipped), 0 when
 * the caller must copy it the ordinary way. */
static int eng_wcb_job_queue(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    s->wcb_pending = 0;
    const u32 bpp = eng_transfer_bpp(s->fmt);
    if (!bpp || !s->handle || s->pitch < s->w * bpp || !s->w || !s->h) return 0;
    if (!s->guest_dirty) {
        eng_light_probe_log(slot, s, 0, 0, s->w, s->h, "nodirty", NULL, 0,
                            s->guest_valid_serial,
                            eng_guest_valid_covers(s, 0, 0, s->w, s->h), 0);
        return 1;
    }
    u32 rx = 0, ry = 0, rw = s->w, rh = s->h;
    if (s_wcb.rect_slot == (int)slot) {
        rx = s_wcb.rect.x; ry = s_wcb.rect.y;
        rw = s_wcb.rect.w; rh = s_wcb.rect.h;
        s_wcb.rect_slot = -1;
        if (!rw || !rh || rx + rw > s->w || ry + rh > s->h) { rx = ry = 0; rw = s->w; rh = s->h; }
    }
    const int full = rx == 0 && ry == 0 && rw == s->w && rh == s->h;
    if (eng_wcb_skip_on() && eng_guest_valid_covers(s, rx, ry, rw, rh)) {
        s->guest_dirty = full ? 0 : 1;
        if (!full) s_wcb.partial_slot = (int)slot;
        WCB_COUNT(skipped, 1);
        eng_light_probe_log(slot, s, rx, ry, rw, rh, "batchskip", NULL, 0,
                            s->guest_valid_serial, 1, 0);
        return 1;
    }
    if (!g.be->readback_async) return 0;
    eng_wcb_drop_ticket(s);
    const u32 t = g.be->readback_async(g.be->user, s->handle, rx, ry, rw, rh);
    if (!t) return 0;
    if (s_nbjobs >= ENG_WCB_BATCH_MAX) {
        if (g.be->readback_async_release) g.be->readback_async_release(g.be->user, t);
        return 0;
    }
    WCB_COUNT(async_issued, 1);
    s_bjobs[s_nbjobs].slot = slot; s_bjobs[s_nbjobs].ticket = t;
    s_bjobs[s_nbjobs].x = rx; s_bjobs[s_nbjobs].y = ry;
    s_bjobs[s_nbjobs].w = rw; s_bjobs[s_nbjobs].h = rh;
    s_nbjobs++;
    return 1;
}

static void eng_wcb_job_store(const eng_wcb_bjob* j)
{
    eng_surface* s = &g.surfaces[j->slot];
    const u32 bpp = eng_transfer_bpp(s->fmt);
    if (!bpp || !g.be->readback_async_fetch) return;
    const u64 probe_stored = s->guest_valid_serial;
    const int probe_covers = eng_guest_valid_covers(s, j->x, j->y, j->w, j->h);
    const unsigned long long t0 = eng_wcb_now_ns();
    u8* rows = eng_coherence_buffer(j->w * bpp * j->h);
    if (!rows) { g.be->readback_async_release(g.be->user, j->ticket); return; }
    if (g.be->readback_async_fetch(g.be->user, j->ticket, rows, j->w * bpp) != 0) return;
    WCB_COUNT(async_fetched, 1);
    eng_color_rows_store(bpp, s->location, s->offset + j->y * s->pitch + j->x * bpp,
                         s->pitch, rows, j->w, j->h, 0, j->w * bpp, 1);
    const int full = j->x == 0 && j->y == 0 && j->w == s->w && j->h == s->h;
    s->guest_dirty = full ? 0 : 1;
    if (!full) s_wcb.partial_slot = (int)j->slot;
    else if (s_wcb.partial_slot == (int)j->slot) s_wcb.partial_slot = -1;
    eng_guest_valid_set(s, j->x, j->y, j->w, j->h);
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    eng_light_probe_log(j->slot, s, j->x, j->y, j->w, j->h, "batch", rows,
                        j->w, probe_stored, probe_covers, 0);
    WCB_COUNT(copied, 1);
    WCB_COUNT(bytes, (unsigned long long)s->pitch * j->h);
    if (!full) WCB_COUNT(rect_copies, 1);
    {
        const unsigned long long done = eng_wcb_now_ns();
        WCB_COUNT(copy_ns, done - t0);
        eng_wcb_attr_note(0, j->slot, (u64)s->pitch * j->h, done - t0);
    }
}

static void eng_wcb_batch_resolve(u32 cookie, int is_read)
{
    u32 extra[RSX_WCB_MAX_REGIONS];
    const u32 nx = rsx_wcb_take_requests(extra, RSX_WCB_MAX_REGIONS);
    u32 queued[RSX_WCB_MAX_REGIONS];
    u32 nq = 0;
    s_nbjobs = 0;
    s_wcb.batching = 1;
    WCB_COUNT(batches, 1);
    /* The region that faulted. */
    if (cookie & WCB_COOKIE_DEPTH) {
        const u32 slot = cookie & 0xFFFFu;
        if (slot < g.n_zdepths && g.zdepths[slot].wcb_pending) {
            g.zdepths[slot].wcb_pending = 0;
            if (g.zdepths[slot].guest_dirty && eng_zdepth_store(slot, 1) && is_read)
                WCB_COUNT(demand_copies, 1);
        }
    } else if (cookie < g.n_surfaces && g.surfaces[cookie].wcb_pending) {
        const u32 slot = cookie;
        if (eng_wcb_job_queue(slot)) {
            if (is_read) {
                WCB_COUNT(demand_copies, 1);
                g.surfaces[slot].wcb_reads++;
                g.surfaces[slot].wcb_last_read = g.frames;
            }
        } else {
            eng_surface* s = &g.surfaces[slot];
            s->wcb_pending = 0;
            if (eng_wcb_copy_surface(slot, 0)) {
                if (is_read) { WCB_COUNT(demand_copies, 1); s->wcb_reads++; s->wcb_last_read = g.frames; }
            }
        }
    }
    /* Every other request the guest threads are blocked on. */
    for (u32 i = 0; i < nx; i++) {
        if (extra[i] & WCB_COOKIE_DEPTH) {
            const u32 slot = extra[i] & 0xFFFFu;
            if (slot < g.n_zdepths && g.zdepths[slot].wcb_pending) {
                g.zdepths[slot].wcb_pending = 0;
                if (g.zdepths[slot].guest_dirty && eng_zdepth_store(slot, 1) && is_read)
                    WCB_COUNT(demand_copies, 1);
            }
            rsx_wcb_release(extra[i]);
            continue;
        }
        const u32 slot = extra[i];
        if (slot < g.n_surfaces && g.surfaces[slot].wcb_pending) {
            if (eng_wcb_job_queue(slot)) {
                queued[nq++] = extra[i];
                if (is_read) {
                    WCB_COUNT(demand_copies, 1);
                    g.surfaces[slot].wcb_reads++;
                    g.surfaces[slot].wcb_last_read = g.frames;
                }
            } else {
                eng_surface* s = &g.surfaces[slot];
                s->wcb_pending = 0;
                if (eng_wcb_copy_surface(slot, 0) && is_read) {
                    WCB_COUNT(demand_copies, 1);
                    s->wcb_reads++;
                    s->wcb_last_read = g.frames;
                }
                rsx_wcb_release(extra[i]);
            }
        } else {
            rsx_wcb_release(extra[i]);
        }
    }
    /* The first fetch submits the whole batch; the rest only read back what
     * is already complete. */
    WCB_COUNT(batched_copies, s_nbjobs);
    for (u32 i = 0; i < s_nbjobs; i++) eng_wcb_job_store(&s_bjobs[i]);
    for (u32 i = 0; i < nq; i++) rsx_wcb_release(queued[i]);
    s_nbjobs = 0;
    s_wcb.batching = 0;
}

/* rsx_wcb_resolve_fn: the guest (or the engine itself) is about to touch an
 * armed target's pages. Engine thread only. The module reopens the pages when
 * this returns. */
static void eng_wcb_batch_resolve(u32 cookie, int is_read);
static void eng_wcb_resolve(void* user, u32 cookie, int reason)
{
    (void)user;
    eng_mt_sync();
    /* A read in command-stream order -- the walker's own transfers, a texture
     * upload, an eviction -- wants the target as it is now. A guest thread
     * that saw the sync point's label wants it as of that label: the queued
     * copy, when there is one. */
    const int guest_read = reason == RSX_WCB_REASON_REQUEST;
    const int is_read = reason != RSX_WCB_REASON_FLUSH;
    const int saved_trig = s_wcb_trigger;
    s_wcb_trigger = (reason == RSX_WCB_REASON_REQUEST || reason == RSX_WCB_REASON_FAULT)
                    ? ENG_WCB_TRIG_GUEST
                    : reason == RSX_WCB_REASON_EXPLICIT ? saved_trig : ENG_WCB_TRIG_OTHER;
    /* PWCB cut 2: every request this pass is waiting for goes to the GPU as
     * one batch (one submission, one wait) instead of one submit_and_wait
     * each. The stream cut uses the same grouping. */
    if ((reason == RSX_WCB_REASON_REQUEST || reason == RSX_WCB_REASON_FAULT) &&
        !s_wcb.batching && (eng_wcb_batch_on() || eng_wcb_stream_on())) {
        eng_wcb_batch_resolve(cookie, is_read);
        s_wcb_trigger = saved_trig;
        return;
    }
    if (cookie & WCB_COOKIE_DEPTH) {
        const u32 slot = cookie & 0xFFFFu;
        if (slot >= g.n_zdepths || !g.zdepths[slot].wcb_pending) { s_wcb_trigger = saved_trig; return; }
        g.zdepths[slot].wcb_pending = 0;
        if (g.zdepths[slot].guest_dirty && eng_zdepth_store(slot, 1) && is_read)
            WCB_COUNT(demand_copies, 1);
        s_wcb_trigger = saved_trig;
        return;
    }
    if (cookie >= g.n_surfaces || !g.surfaces[cookie].wcb_pending) { s_wcb_trigger = saved_trig; return; }
    eng_surface* s = &g.surfaces[cookie];
    s->wcb_pending = 0;
    if (!eng_wcb_copy_surface(cookie, guest_read)) { s_wcb_trigger = saved_trig; return; }
    if (is_read) {
        WCB_COUNT(demand_copies, 1);
        s->wcb_reads++;
        s->wcb_last_read = g.frames;
    }
    s_wcb_trigger = saved_trig;
}

static void eng_wcb_module_up(void)
{
    if (!s_wcb.armed_module) {
        rsx_wcb_init(eng_wcb_resolve, NULL, s_wcb.wake);
        rsx_wcb_counters_get(&s_wcb.mod_mark);
        s_wcb.armed_module = 1;
    }
    s_wcb_live = 1;
    rsx_wcb_set_owner_thread();
}

/* A slot is about to hold another target, or the engine is going down: guest
 * memory gets what the old one rendered, and its ticket is freed. */
static void eng_wcb_retire_surface(u32 slot)
{
    if (slot >= ENG_MAX_SURFACES) return;
    eng_surface* s = &g.surfaces[slot];
    if (s->wcb_pending) rsx_wcb_resolve_cookie(slot, RSX_WCB_REASON_FLUSH);
    if (s->wcb_pending) { rsx_wcb_release(slot); s->wcb_pending = 0; }
    eng_wcb_drop_ticket(s);
    s->wcb_reads = 0;
    s->wcb_last_read = 0;
    s->wcb_ticket_serial = 0;
}

static void eng_wcb_retire_zdepth(u32 slot)
{
    if (slot >= ENG_MAX_ZDEPTHS) return;
    eng_zdepth* z = &g.zdepths[slot];
    if (z->wcb_pending) rsx_wcb_resolve_cookie(WCB_COOKIE_DEPTH | slot, RSX_WCB_REASON_FLUSH);
    if (z->wcb_pending) { rsx_wcb_release(WCB_COOKIE_DEPTH | slot); z->wcb_pending = 0; }
}

static int eng_is_display_offset(u32 location, u32 offset);

/* ondemand / async at a sync point: arm every dirty target, queue copies of
 * the hot ones. Nothing here waits for the GPU. */
static void eng_wcb_sync_point(int mode)
{
    eng_mt_sync();
    eng_wcb_module_up();
    const int prefetch = eng_wcb_prefetch_on() && g.be->readback_async && g.be->submit_async;
    u32 queued = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        /* A wait that timed out had the module open the pages without a
         * copy: arm the target again. */
        if (s->wcb_pending && !rsx_wcb_is_pending(i)) s->wcb_pending = 0;
        if (!s->handle || !s->guest_dirty) continue;
        if (eng_is_display_offset(s->location, s->offset)) continue;
        if (!s->wcb_pending) {
            const u32 bpp = eng_transfer_bpp(s->fmt);
            if (!bpp || s->pitch < s->w * bpp || !s->w || !s->h || !g.be->readback) continue;
            const u32 span = s->pitch * (s->h - 1u) + s->w * bpp;
            void* host = (void*)eng_guest_ptr_raw(s->location, s->offset, span);
            if (host && rsx_wcb_protect(i, host, span) == 0) {
                s->wcb_pending = 1;
                WCB_COUNT(armed, 1);
            } else {
                /* The host would not protect the pages: copy now, as sync
                 * mode does, rather than let the guest read stale bytes. */
                if (eng_surface_to_guest(i)) WCB_COUNT(sync_copies, 1);
                continue;
            }
        }
        if ((mode == RSX_WCB_ASYNC || prefetch) && g.be->readback_async && eng_wcb_hot(s) &&
            (!s->wcb_ticket || s->wcb_ticket_serial != s->color_serial)) {
            eng_wcb_drop_ticket(s);
            const u32 t = g.be->readback_async(g.be->user, s->handle, 0, 0, s->w, s->h);
            if (t) {
                s->wcb_ticket = t;
                s->wcb_ticket_serial = s->color_serial;
                WCB_COUNT(async_issued, 1);
                queued++;
            }
        }
    }
    if (g.be->depth_readback) for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->wcb_pending && !rsx_wcb_is_pending(WCB_COOKIE_DEPTH | i)) z->wcb_pending = 0;
        if (!z->handle || !z->guest_dirty || z->wcb_pending) continue;
        u32 w, h, bpp;
        if (!eng_zdepth_span(z, &w, &h, &bpp)) continue;
        const u32 span = z->pitch * (h - 1u) + w * bpp;
        void* host = (void*)eng_guest_ptr_raw(z->location, z->offset, span);
        if (host && rsx_wcb_protect(WCB_COOKIE_DEPTH | i, host, span) == 0) {
            z->wcb_pending = 1;
            WCB_COUNT(armed, 1);
        } else if (eng_zdepth_to_guest(i)) {
            WCB_COUNT(sync_copies, 1);
        }
    }
    /* Start the queued copies now, so the GPU works through them while the
     * walker records the rest of the frame. With frame pipelining the submit
     * does not wait; without it the backend leaves the batch open. */
    if (prefetch && queued) g.be->submit_async(g.be->user);
}

void rsx_draw_engine_wcb_service(void)
{
    if (!s_wcb_live || !g.ready) return;
    rsx_wcb_set_owner_thread();
    rsx_wcb_service();
}

void rsx_draw_engine_wcb_stats_get(rsx_draw_engine_wcb_stats* out)
{
    if (!out) return;
    *out = s_wcb.st;
    if (!s_wcb.armed_module) return;
    rsx_wcb_counters c;
    rsx_wcb_counters_get(&c);
    out->faults = c.faults;
    out->waits = c.waits;
    out->wait_ns = c.wait_ns;
    out->wait_max_ns = c.wait_max_ns;
    out->timeouts = c.timeouts;
}

/* PS3RECOMP_RSX_WCB_STATS: one line per window, per-frame averages. */
static void eng_wcb_frame_tick(void)
{
    if (!s_wcb.stats_every || !s_wcb.mode) return;
    if (++s_wcb.win_frames < s_wcb.stats_every) return;
    const double f = (double)s_wcb.win_frames;
    const rsx_draw_engine_wcb_stats* w = &s_wcb.win;
    rsx_wcb_counters c;
    memset(&c, 0, sizeof c);
    if (s_wcb.armed_module) rsx_wcb_counters_get(&c);
    const rsx_wcb_counters* m = &s_wcb.mod_mark;
    const unsigned long long waits = c.waits - m->waits, wait_ns = c.wait_ns - m->wait_ns;
    fprintf(stderr, "[rsx wcb] %u frames, per frame: dirty %.1f armed %.1f copied %.1f (%.0f KB:"
                    " sync %.1f demand %.1f, rect %.1f) skipped %.1f batch %.2f/%u;"
                    " async issued %.1f fetched %.1f discarded %.1f;"
                    " faults %.1f waits %.1f (avg %.0f us, max %.0f us) copy %.2f ms; timeouts %llu"
                    " pending %u\n",
            s_wcb.win_frames, w->marked_dirty / f, w->armed / f, w->copied / f,
            w->bytes / f / 1024.0, w->sync_copies / f, w->demand_copies / f,
            w->rect_copies / f, w->skipped / f,
            w->batches / f, (unsigned)w->batched_copies,
            w->async_issued / f, w->async_fetched / f, w->async_discarded / f,
            (c.faults - m->faults) / f, waits / f, waits ? wait_ns / 1e3 / waits : 0.0,
            c.wait_max_ns / 1e3, w->copy_ns / f / 1e6, (unsigned long long)(c.timeouts - m->timeouts),
            rsx_wcb_pending_count());
    memset(&s_wcb.win, 0, sizeof s_wcb.win);
    s_wcb.mod_mark = c;
    s_wcb.win_frames = 0;
}

/* PS3RECOMP_RSX_WCB_ATTR: one line per copied target per window, with the
 * trigger split. Runs even when the stats line is off, so the attribution
 * window follows PS3RECOMP_RSX_WCB_STATS when that is set and 60 frames
 * otherwise. */
static void eng_wcb_attr_flush(void)
{
    if (!s_wcb.attr_on) return;
    if (++s_wcb.attr_frames < s_wcb.attr_every) return;
    const double f = (double)s_wcb.attr_frames;
    double tot_reads = 0.0, tot_copy = 0.0, tot_wait = 0.0;
    unsigned rows = 0;
    for (u32 idx = 0; idx < ENG_MAX_SURFACES + ENG_MAX_ZDEPTHS; idx++) {
        eng_wcb_attr_row* r = &s_wcb.attr[idx];
        if (!r->used) continue;
        const u32 slot = r->depth ? idx - ENG_MAX_SURFACES : idx;
        uint64_t wns = 0; uint32_t wn = 0;
        rsx_wcb_attr_take_wait(r->depth ? (WCB_COOKIE_DEPTH | slot) : slot, &wns, &wn);
        r->wait_ns += wns;
        double reads = 0.0, copy = 0.0, bytes = 0.0;
        for (int t = 0; t < ENG_WCB_TRIG_COUNT; t++) {
            reads += (double)r->reads[t];
            copy += (double)r->copy_ns[t] / 1e6;
            bytes += (double)r->bytes[t];
        }
        tot_reads += reads; tot_copy += copy; tot_wait += (double)r->wait_ns / 1e6;
        fprintf(stderr,
                "[rsx wcb attr] loc=%u addr=0x%08X fmt=0x%02X %ux%u pitch=%u %s:"
                " reads %.2f/f [texbind %.2f guest %.2f other %.2f]"
                " copy %.3f ms/f [texbind %.3f guest %.3f other %.3f]"
                " wait %.3f ms/f bytes %.0f KB/f\n",
                r->location, r->offset, r->fmt, r->w, r->h, r->pitch,
                r->depth ? "depth" : "color",
                reads / f,
                (double)r->reads[ENG_WCB_TRIG_TEXBIND] / f,
                (double)r->reads[ENG_WCB_TRIG_GUEST] / f,
                (double)r->reads[ENG_WCB_TRIG_OTHER] / f,
                copy / f,
                (double)r->copy_ns[ENG_WCB_TRIG_TEXBIND] / 1e6 / f,
                (double)r->copy_ns[ENG_WCB_TRIG_GUEST] / 1e6 / f,
                (double)r->copy_ns[ENG_WCB_TRIG_OTHER] / 1e6 / f,
                (double)r->wait_ns / 1e6 / f, bytes / f / 1024.0);
        rows++;
    }
    fprintf(stderr, "[rsx wcb attr] %u frames: copies %.2f/f, copy %.3f ms/f,"
                    " wait %.3f ms/f, %u surfaces\n",
            s_wcb.attr_frames, tot_reads / f, tot_copy / f, tot_wait / f, rows);
    memset(s_wcb.attr, 0, sizeof s_wcb.attr);
    s_wcb.attr_frames = 0;
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
/* SET_TEXTURE_BORDER_COLOR as rsx_be_sampler_desc.border. The colour is
 * ARGB; a depth texture compares against its low 24 bits as depth. inFAMOUS
 * 2's sun shadow maps (DEPTH24_D8, wrap BORDER, a depth compare) set
 * 0x00FFFFFF, depth 1.0, so a lookup outside the map compares against the far
 * plane; read as transparent black it compared against 0 instead and flipped
 * the shadow term for everything the map does not cover.
 * PS3RECOMP_ENG_BORDER=0: always transparent black, the old behaviour. */
static u8 eng_border_class(u32 format, u32 border)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_BORDER"); on = e ? atoi(e) : 1; }
    if (!on) return 0u;
    const u32 base = format & 0x9Fu;               /* without the LN/UN bits */
    if (base >= 0x90u && base <= 0x93u)            /* DEPTH24_D8(_FLOAT), DEPTH16(_FLOAT) */
        return (border & 0x00FFFFFFu) == 0x00FFFFFFu ? 2u : 0u;   /* 1.0 -> white, 0 -> black */
    if (border == 0x00000000u) return 0u;
    if (border == 0xFF000000u) return 1u;
    if (border == 0xFFFFFFFFu) return 2u;
    return 3u;                                     /* not expressible: edge clamp */
}

static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6);
    out->mag_linear  = (u8)(magf == 2);
    out->mip_present = (u8)(minf >= 3);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* Each fragment unit's decoded registers, decoded again only after a write
 * changed one of them (rsx_dispatch.tex_wgen): the pipeline key and the
 * binding read all 16 units on every draw. PS3RECOMP_ENG_TEXWGEN=0 (read at
 * engine init) decodes every time. Port of the inFAMOUS 2 port's eng_tex
 * (github.com/ThiagoFraga761/infamous2-pc, MIT). */
static struct { rsx_dsp_texture t; u32 wgen; int valid; } s_tdec[RSX_DSP_NUM_TEXTURES];
static int s_tex_wgen_on;
static const rsx_dsp_texture* eng_tex(u32 u)
{
    if (!s_tex_wgen_on || !s_tdec[u].valid || s_tdec[u].wgen != g.rsx.tex_wgen[u]) {
        rsx_dsp_get_texture(&g.rsx, u, &s_tdec[u].t);
        s_tdec[u].wgen = g.rsx.tex_wgen[u];
        s_tdec[u].valid = 1;
    }
    return &s_tdec[u].t;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

/* Units addressed in texels (RSX_TEX_FMT_UNNORM), with each one's size in
 * dim. No host sampling mode is unnormalised for these, so the fragment
 * program divides the coordinates instead, and the divisor makes the sizes
 * pipeline identity (rsx_live_draw.c keys them the same way). */
static u32 eng_unnorm_units(u32 dim[RSX_DSP_NUM_TEXTURES][3])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const rsx_dsp_texture t = *eng_tex(u);
        dim[u][0] = dim[u][1] = dim[u][2] = 0;
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height)
            continue;
        mask |= 1u << u;
        dim[u][0] = t.width;
        dim[u][1] = t.height;
        dim[u][2] = t.depth;
    }
    return mask;
}

/* Units bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3 with a
 * depth), declared Texture3D and sampled with three coordinates. Only when
 * the backend can create one: otherwise the first slice stays a 2D texture
 * and the program samples it in 2D, as before. */
static u32 eng_volume_mask(void)
{
    if (!g.be->texture_create_3d || !g.be->texture_upload_3d) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const rsx_dsp_texture t = *eng_tex(u);
        if (t.enabled && !t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height)
            mask |= 1u << u;
    }
    return mask;
}

static u32 eng_cube_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const rsx_dsp_texture t = *eng_tex(u);
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t.enabled && t.cubemap && t.width && t.width == t.height)
            mask |= 1u << u;
    }
    return mask;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads. */
static int eng_guest_programs(const u8** out_vp, u32* out_vp_instrs,
                              const u8** out_fp, u32* out_fp_size)
{
    const u8* vp_uc = NULL;
    u32 vp_instrs = 0;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start < RSX_DSP_VP_INSTR) {
        vp_uc = (const u8*)(g.rsx.vp + start * 4);
        vp_instrs = rsx_vp_program_size_instrs(
            vp_uc, (RSX_DSP_VP_INSTR - start) * 16u);
    }

    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    const u8* fp_uc = NULL;
    u32 fp_size = 0;
    if (rsx_dsp_reg(&g.rsx, 0x08E4)) {
        u32 fp_loc = 0;
        const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
        fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, 16);
        fp_size = fp_uc ? rsx_fp_program_size(fp_uc, ENG_FP_MAX_BYTES) : 0;
        if (fp_size) fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, fp_size);
        if (!fp_uc) fp_size = 0;
    }

    if (out_vp) *out_vp = vp_uc;
    if (out_vp_instrs) *out_vp_instrs = vp_instrs;
    if (out_fp) *out_fp = fp_uc;
    if (out_fp_size) *out_fp_size = fp_size;
    return vp_instrs && fp_size;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const u8* uc = NULL;
    u32 instrs = 0;
    if (eng_guest_programs(&uc, &instrs, NULL, NULL)) {
        rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
        if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
            analysis.exact && analysis.input_mask)
            mask = analysis.input_mask;
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    Out.t8 = (float4)0; Out.t9 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* The texture units a translated pixel program samples. Every sampling call
 * the decompiler emits names its unit's sampler, rsx_samp[N], whatever the
 * texture's own spelling (rsx_tex[N] or, with a cube unit, rsx_texN); the
 * declaration's rsx_samp[16] is out of range and so not counted. */
static u32 eng_sampled_units(const char* ps)
{
    u32 mask = 0;
    for (const char* q = ps; (q = strstr(q, "rsx_samp[")) != NULL; q += 9) {
        const unsigned long u = strtoul(q + 9, NULL, 10);
        if (u < RSX_BE_MAX_TEXTURES) mask |= 1u << u;
    }
    return mask;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. `out_tex_used` gets the units the
 * pixel program samples: all of them for the built-in pair, which keeps
 * every enabled unit bound as before. */
/* Up to 16 comma-separated hex hashes from an environment variable. */
static u32 eng_parse_hash_list(const char* name, u64 out[16])
{
    const char* e = getenv(name);
    u32 n = 0;
    while (e && *e && n < 16) {
        char* end;
        const u64 v = strtoull(e, &end, 16);
        if (end == e) break;
        if (v) out[n++] = v;
        if (*end != ',') break;
        e = end + 1;
    }
    return n;
}

/* The per-draw fragment-program debug knobs; nonzero drops the draw.
 *
 * RSX_ENGINE_SKIP_FP=<hex>[,<hex>...] drops every draw through those
 * fragment programs: which pass paints a wrong smear, halo or black band is
 * found by removing passes one at a time.
 *
 * RSX_ENGINE_REGDUMP_FP=<hex>[,<hex>...] prints the non-zero words of the
 * whole RSX register file at the first draw through each of those programs,
 * so the state of a broken pass can be diffed register by register against a
 * working one -- which state it sets that the engine does not honour. */
static int eng_debug_fp(u64 fp_hash, u32 pipeline)
{
    static int init;
    static u64 skip[16], dump[16];
    static u32 n_skip, n_dump, dumped;
    if (!init) {
        init = 1;
        n_skip = eng_parse_hash_list("RSX_ENGINE_SKIP_FP", skip);
        n_dump = eng_parse_hash_list("RSX_ENGINE_REGDUMP_FP", dump);
    }
    for (u32 i = 0; i < n_dump; i++) {
        if (dump[i] != fp_hash || (dumped & (1u << i))) continue;
        dumped |= 1u << i;
        fprintf(stderr, "[rsx regdump] fp %016llx pipe %u frame %u begin\n",
                (unsigned long long)fp_hash, pipeline, g.frames);
        for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
            if (g.rsx.regs[r])
                fprintf(stderr, "[rsx regdump] 0x%04X = 0x%08X\n", r * 4u, g.rsx.regs[r]);
        fprintf(stderr, "[rsx regdump] fp %016llx end\n", (unsigned long long)fp_hash);
    }
    for (u32 i = 0; i < n_skip; i++)
        if (skip[i] == fp_hash) return 1;
    return 0;
}

/* User clip planes. NV4097_SET_USER_CLIP_PLANE_CONTROL (0x1478) holds one
 * 4-bit op per plane (bits 0..23, planes 0..5): 0 disabled, 1 ENABLE_LT, 2
 * ENABLE_GE. The vertex program writes the six distances into o[5].yzw and
 * o[6].yzw (o[5].x is fog, o[6].x the point size). As RPCS3 does
 * (fill_user_clip_data + the VP reg_table): GE keeps d >= 0 (factor +1), LT
 * keeps d <= 0 (factor -1), a disabled plane never clips. A planar
 * reflection clips its mirrored scene at the mirror plane this way; ignored,
 * everything behind the plane was drawn into the reflection.
 * Returns the op per plane, two bits each (0 = no plane enabled), the form
 * rsx_vp_apply_user_clip takes. PS3RECOMP_ENG_USER_CLIP=0 ignores the
 * register (the old behaviour). */
static u32 eng_user_clip_key(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_USER_CLIP"); on = e ? atoi(e) : 1; }
    if (!on) return 0;
    const u32 ctl = rsx_dsp_reg(&g.rsx, 0x1478u);
    u32 key = 0;
    for (u32 p = 0; p < 6; p++) {
        const u32 op = (ctl >> (p * 4u)) & 0xFu;
        if (op == 1u || op == 2u) key |= op << (p * 2u);
    }
    return key;
}

/* A guest fragment program's name for the debug knobs: its structural hash
 * (inline constants excluded) seeded with the FNV offset basis, the value
 * RSX_FP_DEBUG matches in the decompiler. Unlike a backend pipeline handle it
 * does not depend on the order programs are first drawn in, so a program
 * found in one run is named the same in the next. 0 for the built-in one. */
static u64 eng_fp_debug_hash(const u8* fp_uc, u32 fp_size)
{
    return fp_uc && fp_size
        ? rsx_fp_structural_hash(fp_uc, fp_size, 1469598103934665603ull) : 0;
}

/* The vp_c[] rows of the pipeline eng_pipeline_get last returned. */
static struct { u32 lo, n; int full; } s_vpc = { 0, 0, 1 };
static int s_vpc_force_full;

/* The raw eight-word unit, not the decoded struct: the pipeline only sees an
 * enabled unit, and reading the register file directly avoids twenty
 * memset+getter calls per draw. A disabled unit contributes nothing, which
 * is exactly what the key does. */
static u64 eng_pipe_hash_texture_state(u64 h)
{
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const u32 base = 0x1A00u + u * 0x20u;
        if (!(rsx_dsp_reg(&g.rsx, base + 0x0Cu) >> 31)) continue;
        h = eng_fnv1a(&u, sizeof u, h);
        for (u32 k = 0; k < 8; k++) {
            const u32 w = rsx_dsp_reg(&g.rsx, base + k * 4u);
            h = eng_fnv1a(&w, sizeof w, h);
        }
        const u32 sz = rsx_dsp_reg(&g.rsx, 0x1840u + u * 4u);
        h = eng_fnv1a(&sz, sizeof sz, h);
    }
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        const u32 base = 0x0900u + u * 0x20u;
        if (!(rsx_dsp_reg(&g.rsx, base + 0x0Cu) >> 31)) continue;
        h = eng_fnv1a(&u, sizeof u, h);
        for (u32 k = 0; k < 8; k++) {
            const u32 w = rsx_dsp_reg(&g.rsx, base + k * 4u);
            h = eng_fnv1a(&w, sizeof w, h);
        }
    }
    return h;
}

/* Cut 3's identity for one pipeline answer: the transform and fragment
 * program bytes (the fragment program lives in guest memory, so it can change
 * under the same SHADER_PROGRAM register), the registers the decision reads,
 * the enabled texture units, and the already-decoded layout and render
 * state. */
static u64 eng_pipe_fingerprint(const u8* vp, u32 vp_bytes,
                                const u8* fp, u32 fp_size, int fixed,
                                const rsx_vertex_layout_plan* layout,
                                const rsx_be_render_state* rs,
                                rsx_be_format rt_fmt, u32 rt_count)
{
    u64 h = 1469598103934665603ull;
    const u32 fx = fixed ? 1u : 0u;
    h = eng_fnv1a(&fx, sizeof fx, h);
    if (!fixed) {
        h = eng_vp_hash(vp, vp_bytes, h);
        if (fp && fp_size) h = eng_fnv1a_words(fp, fp_size, h);
    }
    /* Only the registers eng_pipeline_get reads. The whole register file
     * cannot be the identity: the transform-constant upload window and the
     * immediate-vertex registers change on every group without touching the
     * pipeline, and including them turned every draw into a memo miss. */
    u32 v = rsx_dsp_shader_control(&g.rsx);      h = eng_fnv1a(&v, sizeof v, h);
    v = rsx_dsp_reg(&g.rsx, 0x1FECu);             h = eng_fnv1a(&v, sizeof v, h);
    /* Colour format (srgb) and the antialias field (WPOS grid) both live in
     * SURFACE_FORMAT; the full word is the conservative form. */
    v = rsx_dsp_reg(&g.rsx, 0x0208u);             h = eng_fnv1a(&v, sizeof v, h);
    v = rsx_dsp_reg(&g.rsx, 0x1FF8u);             h = eng_fnv1a(&v, sizeof v, h);
    v = rsx_dsp_reg(&g.rsx, 0x1478u);             h = eng_fnv1a(&v, sizeof v, h);
    h = eng_pipe_hash_texture_state(h);
    h = eng_fnv1a(layout, sizeof *layout, h);
    h = rsx_draw_engine_hash_render_state(rs, h);
    h = eng_fnv1a(&rt_fmt, sizeof rt_fmt, h);
    h = eng_fnv1a(&rt_count, sizeof rt_count, h);
    return h;
}

static eng_pipe_memo* eng_pipe_memo_find(u64 fingerprint)
{
    if (s_memo_index) {
        const u32 s = eng_memo_index_get(s_pmemo_index, fingerprint);
        if (s && s <= ENG_PIPE_MEMO) {
            eng_pipe_memo* m = &s_pmemo[s - 1u];
            if (m->used && m->epoch == s_fast_epoch &&
                m->fingerprint == fingerprint)
                return m;
        }
    }
    for (u32 i = 0; i < ENG_PIPE_MEMO; i++)
        if (s_pmemo[i].used && s_pmemo[i].epoch == s_fast_epoch &&
            s_pmemo[i].fingerprint == fingerprint)
            return &s_pmemo[i];
    return NULL;
}

static eng_pipe_memo* eng_pipe_memo_alloc(void)
{
    u32 i;
    for (i = 0; i < ENG_PIPE_MEMO; i++)
        if (!s_pmemo[i].used || s_pmemo[i].epoch != s_fast_epoch) break;
    if (i == ENG_PIPE_MEMO) i = s_pmemo_next++ % ENG_PIPE_MEMO;
    eng_pipe_memo* m = &s_pmemo[i];
    m->used = 1;
    m->epoch = s_fast_epoch;
    return m;
}

static void eng_pipe_memo_restore(eng_pipe_memo* m, int* out_fixed,
                                  u32* out_tex_used, u64* out_fp_hash)
{
    *out_fixed = m->fixed;
    *out_tex_used = m->tex_used;
    *out_fp_hash = m->fp_hash;
    s_vpc.lo = m->vpc_lo;
    s_vpc.n = m->vpc_n;
    s_vpc.full = m->vpc_full;
    g.fp_constants.count = m->const_count;
    if (m->const_count)
        memcpy(g.fp_constants.values, m->const_values,
               (size_t)m->const_count * 16u);
}

static void eng_pipe_memo_save(u64 fingerprint, u32 handle, int fixed,
                               u32 tex_used, u64 fp_hash)
{
    eng_pipe_memo* m = eng_pipe_memo_alloc();
    m->fingerprint = fingerprint;
    if (s_memo_index)
        eng_memo_index_put(s_pmemo_index, (u32)(m - s_pmemo), fingerprint);
    m->handle = handle;
    m->fixed = (u8)fixed;
    m->tex_used = (u16)tex_used;
    m->fp_hash = fp_hash;
    m->vpc_lo = (u16)s_vpc.lo;
    m->vpc_n = (u16)s_vpc.n;
    m->vpc_full = (u8)s_vpc.full;
    m->const_count = g.fp_constants.count;
    if (m->const_count > m->const_cap) {
        u32* p = (u32*)realloc(m->const_values,
                               (size_t)m->const_count * 16u);
        if (!p) { m->used = 0; return; }
        m->const_values = p;
        m->const_cap = m->const_count;
    }
    if (m->const_count)
        memcpy(m->const_values, g.fp_constants.values,
               (size_t)m->const_count * 16u);
}

static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed,
                            u32* out_tex_used, u64* out_fp_hash)
{
    *out_fixed = 1;
    *out_tex_used = 0;
    *out_fp_hash = 0;
    const unsigned long long wp2_lookup = wp2_t0();
    s_wp2_pipe_gets++;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size);

    /* Cut 3: an unchanged program pair and state replays the stored answer
     * (including the buffered fragment constants the caller needs) without
     * walking either program again. */
    u64 pipe_fp = 0;
    if (s_pipe_memo) {
        pipe_fp = eng_pipe_fingerprint(vp_uc, fixed ? 0 : vp_instrs * 16u,
                                       fp_uc, fixed ? 0 : fp_size, fixed,
                                       layout, rs, rt_fmt, rt_count);
        eng_pipe_memo* memo = eng_pipe_memo_find(pipe_fp);
        if (memo) {
            s_pipe_hits++;
            s_wp2_pipe_hits++;
            eng_pipe_memo_restore(memo, out_fixed, out_tex_used, out_fp_hash);
            wp2_add(WPROF2_PIPE_LOOKUP, wp2_lookup);
            return memo->handle;
        }
        s_pipe_misses++;
    }
    wp2_add(WPROF2_PIPE_LOOKUP, wp2_lookup);
    const unsigned long long wp2_analyze = wp2_t0();

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    const u32 srgb_output = !fixed && rsx_fp_srgb_output_enabled(
        rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl,
        rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu);
    const u32 cube_mask = eng_cube_mask();
    const u32 vol_mask = eng_volume_mask();
    const u32 vtex_mask = eng_vtex_mask();
    /* BRB/CLB outcomes are baked into the program: the branch bits are
     * identity only for a program that tests them. */
    const int vp_branch_bits_used = !fixed && rsx_vp_uses_branch_bits(vp_uc, vp_instrs * 16u);
    const u32 vp_branch_bits = vp_branch_bits_used ? rsx_dsp_reg(&g.rsx, 0x1FF8u) : 0;
    const u32 ucp = fixed ? 0 : eng_user_clip_key();

    rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = fixed ? 0 : rsx_fp_texture_units(fp_uc, fp_size);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        const rsx_dsp_texture t = *eng_tex(u);
        rsx_fp_shadow_desc_from_texture(&shadow[u], t.format, t.dimension,
            t.cubemap, t.mipmaps, t.filter, t.wrap, t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    u8 gamma_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        const rsx_dsp_texture t = *eng_tex(u);
        if (t.enabled) gamma_masks[u] = rsx_texture_gamma_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 gamma_units = fixed ? 0 : rsx_fp_texture_gamma_units(fp_uc, fp_size, gamma_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(gamma_units & (1u << u))) gamma_masks[u] = 0;
    /* Texture sign expansion: SET_TEXTURE_ADDRESS's UNSIGNED_REMAP_BIASED
     * asks that every fetch come back re-centred to -1..1. Same rule as
     * gamma: only units the program samples count. */
    u8 expand_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        const rsx_dsp_texture t = *eng_tex(u);
        if (t.enabled) expand_masks[u] = rsx_texture_expand_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 expand_units = fixed ? 0 : rsx_fp_texture_expand_units(fp_uc, fp_size, expand_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(expand_units & (1u << u))) expand_masks[u] = 0;

    u32 unnorm_dim[RSX_DSP_NUM_TEXTURES][3];
    /* Shadow helpers normalize against the bound resource themselves. Only
     * sampled ordinary UN units need baked dimensions or cache identity. */
    const u32 unnorm_mask = eng_unnorm_units(unnorm_dim) &
        (fixed ? 0xFFFFu : sampled_units) & ~shadow_mask;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++)
        if (!(unnorm_mask & (1u << u)))
            unnorm_dim[u][0] = unnorm_dim[u][1] = unnorm_dim[u][2] = 0;

    g.fp_constants.count = 0;
    if (!fixed && rsx_fp_collect_constants(fp_uc, fp_size, &g.fp_constants) < 0) {
        wp2_add(WPROF2_PIPE_ANALYZE, wp2_analyze);
        return 0;
    }

    /* An antialiased target is held at its sample grid; WPOS must still count
     * pixels (rsx_fp_apply_wpos_grid). */
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    const u32 wpos_grid[2] = { sf.aa_sx ? sf.aa_sx : 1u, sf.aa_sy ? sf.aa_sy : 1u };
    const int wpos_scaled = !fixed && (wpos_grid[0] > 1u || wpos_grid[1] > 1u);

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    wp2_add(WPROF2_PIPE_ANALYZE, wp2_analyze);
    const unsigned long long wp2_key = wp2_t0();
    u64 key = 1469598103934665603ull;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        key = eng_vp_hash(vp_uc, vp_instrs * 16u, key);
        if (vp_branch_bits_used) key = eng_fnv1a(&vp_branch_bits, sizeof vp_branch_bits, key);
        key = rsx_fp_structural_hash(fp_uc, fp_size, key);
        if (!key) { wp2_add(WPROF2_PIPE_KEY, wp2_key); return 0; }
        const u32 fp_ctrl_key = fp_ctrl & 0x4Eu; /* colour width and fragment depth export */
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        if (srgb_output) key = eng_fnv1a(&srgb_output, sizeof srgb_output, key);
        if (gamma_units) key = eng_fnv1a(gamma_masks, sizeof gamma_masks, key);
        if (expand_units) key = eng_fnv1a(expand_masks, sizeof expand_masks, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        if (vol_mask) key = eng_fnv1a(&vol_mask, sizeof vol_mask, key);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow, sizeof shadow, key);
        }
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        if (ucp) key = eng_fnv1a(&ucp, sizeof ucp, key);
        key = eng_fnv1a(&unnorm_mask, sizeof unnorm_mask, key);
        if (unnorm_mask) key = eng_fnv1a(unnorm_dim, sizeof unnorm_dim, key);
        if (wpos_scaled) key = eng_fnv1a(wpos_grid, sizeof wpos_grid, key);
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);
    wp2_add(WPROF2_PIPE_KEY, wp2_key);
    const unsigned long long wp2_dir = wp2_t0();

    u32 slot = eng_pipeline_slot(key);
    for (; g.pipeline_index[slot]; slot = (slot + 1u) & (ENG_PIPELINE_INDEX - 1u)) {
        const eng_pipeline* p = &g.pipelines[g.pipeline_index[slot] - 1u];
        if (p->key == key) {
            *out_fixed = p->fixed;
            *out_tex_used = p->tex_used;
            *out_fp_hash = p->fp_hash;
            s_vpc.lo = p->vpc_lo; s_vpc.n = p->vpc_n; s_vpc.full = p->vpc_full;
            if (s_pipe_memo)
                eng_pipe_memo_save(pipe_fp, p->handle, p->fixed, p->tex_used,
                                   p->fp_hash);
            wp2_add(WPROF2_PIPE_DIR, wp2_dir);
            return p->handle;
        }
    }
    wp2_add(WPROF2_PIPE_DIR, wp2_dir);
    if (g.n_pipelines >= ENG_MAX_PIPELINES) return 0;

    const unsigned long long wp2_decompile = wp2_t0();
    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        rsx_vp_set_branch_bits(vp_branch_bits);
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        if (vi > 0 && ucp && rsx_vp_apply_user_clip(s_vs_hlsl, sizeof s_vs_hlsl, ucp) < 0) {
            vi = -1;
            static u32 said;
            if (PS3_LOG_FIRST(said, 8))
                fprintf(stderr, "[rsx engine] user clip planes 0x%06X: vertex program text not in the"
                                " decompiler's form, draw skipped\n", rsx_dsp_reg(&g.rsx, 0x1478u));
        }
        fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp_uc, fp_size, fp_ctrl, cube_mask, vol_mask,
                shadow_mask ? shadow : NULL, gamma_masks, expand_units ? expand_masks : NULL,
                s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        if (fi > 0 && nconst != g.fp_constants.count) fi = -1;
        if (fi > 0 && wpos_scaled &&
            rsx_fp_apply_wpos_grid(s_ps_hlsl, sizeof s_ps_hlsl, wpos_grid[0], wpos_grid[1]) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(s_ps_hlsl, sizeof s_ps_hlsl, unnorm_mask,
                                                (const u32 (*)[3])unnorm_dim, cube_mask,
                                                vol_mask) < 0)
            fi = -1;
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(s_ps_hlsl, sizeof s_ps_hlsl) < 0)
            fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
    }
    if (vi > 0 && fi > 0) {
        wp2_add(WPROF2_PIPE_DECOMPILE, wp2_decompile);
        const unsigned long long wp2_create = wp2_t0();
        eng_mt_sync();
        if (g.be->set_pipeline_key) g.be->set_pipeline_key(g.be->user, key);
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
        wp2_add(WPROF2_PIPE_CREATE, wp2_create);
    } else {
        wp2_add(WPROF2_PIPE_DECOMPILE, wp2_decompile);
    }
    if (handle) {
        if (s_wp2 > 0) s_wp2_pipe_created++;
        /* Diagnostic: how fast new pipelines (and so glslang translations) are
         * being created. A steady stream means the key is varying per draw. */
        static unsigned long long s_new_pipes;
        if ((++s_new_pipes & 63ull) == 0)
            fprintf(stderr, "[rsx engine] %llu pipelines created by frame %u\n",
                    s_new_pipes, g.frames);
    }
    const u64 fp_hash = fixed ? 0 : eng_fp_debug_hash(fp_uc, fp_size);
    /* RSX_ENGINE_SHADER_DUMP=<existing dir>: each new pipeline's two programs
     * as <dir>/pipe<handle>.vs.hlsl and .ps.hlsl, the handle being the one
     * RSX_ENGINE_PASS_DUMP's index names, and a guest fragment program's own
     * ucode as .fp.bin with its address and debug hash (the value
     * RSX_FP_DEBUG, RSX_ENGINE_SKIP_FP and RSX_ENGINE_REGDUMP_FP take) in
     * .fp.txt. A failed pipeline is pipe0. */
    {
        const char* dir = getenv("RSX_ENGINE_SHADER_DUMP");
        if (dir && *dir) {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof path, "%s/pipe%u.vs.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_vs_hlsl, f); fclose(f); }
            snprintf(path, sizeof path, "%s/pipe%u.ps.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_ps_hlsl, f); fclose(f); }
            if (!fixed && fp_uc && fp_size) {
                u32 fp_loc = 0;
                const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
                snprintf(path, sizeof path, "%s/pipe%u.fp.bin", dir, handle);
                if ((f = fopen(path, "wb"))) { fwrite(fp_uc, 1, fp_size, f); fclose(f); }
                snprintf(path, sizeof path, "%s/pipe%u.fp.txt", dir, handle);
                if ((f = fopen(path, "w"))) {
                    fprintf(f, "location %u offset 0x%08X size %u ctrl 0x%08X\n"
                               "fp %016llx\n",
                            fp_loc, fp_off, fp_size, fp_ctrl,
                            (unsigned long long)fp_hash);
                    fclose(f);
                }
            }
        }
    }
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 32))
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants, fp %016llx -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, (unsigned long long)fp_hash,
                handle ? "ok" : "FAILED (draw dropped)"); }

    { eng_pipeline* p = &g.pipelines[g.n_pipelines];
      rsx_vp_const_usage cu;
      p->vpc_full = 1; p->vpc_lo = p->vpc_n = 0;
      if (!fixed && rsx_vp_analyze_constants(vp_uc, vp_instrs * 16u, &cu) >= 0 &&
          cu.valid && !cu.relative) {
          p->vpc_full = 0;
          if (cu.lo <= cu.hi) { p->vpc_lo = (u16)cu.lo; p->vpc_n = (u16)(cu.hi - cu.lo + 1u); }
      }
      s_vpc.lo = p->vpc_lo; s_vpc.n = p->vpc_n; s_vpc.full = p->vpc_full; }
    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].fp_hash = fp_hash;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].tex_used =
        (u16)(fixed ? 0xFFFFu : eng_sampled_units(s_ps_hlsl));
    *out_tex_used = g.pipelines[g.n_pipelines].tex_used;
    *out_fp_hash = fp_hash;
    g.pipeline_index[slot] = (u16)(g.n_pipelines + 1u);
    g.n_pipelines++;
    *out_fixed = fixed;
    if (s_pipe_memo)
        eng_pipe_memo_save(pipe_fp, handle, fixed, *out_tex_used, fp_hash);
    return handle;
}

/* ---- draw accumulation --------------------------------------------------- */

/* The incremental reference hash reproduces eng_refs_key() byte for byte:
 * rsx_vertex_ref is two host-endian u32s and eng_fnv1a reads their memory
 * image. On a little-endian host that image is the low byte first; elsewhere
 * keep the whole-list pass (the shortcut is a pure optimisation, never an
 * answer). */
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define ENG_HAVE_FAST_REFS_HASH 1
#endif

/* eng_refs_key() hashes the list through eng_fnv1a_words, i.e. one native
 * u64 per 8-byte reference. rsx_vertex_ref is two u32s; on a little-endian
 * host its word is vertex_id in the low half, base_index in the high. */
static inline u64 eng_refs_hash_ref(u64 h, u32 vertex_id, u32 base_index)
{
    return rsx_vertex_refs_hash_fold(h, vertex_id, base_index);
}

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_hash = 1469598103934665603ull;
    dc.refs_remapped = 0;
    dc.fetch_ok = 1;
    dc.fetch_deferred = 0;
    dc.inl = NULL;
    dc.inl_bytes = 0;
    dc.n_immediate = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    if (s_refs_hash_fast)
        dc.refs_hash = eng_refs_hash_ref(dc.refs_hash, vertex_id, base_index);
    dc.n_refs++;
    return 1;
}

/* Room for `more` references past dc.n_refs, grown as dc_push_ref grows. */
static int dc_reserve_refs(u32 more)
{
    const u64 want = (u64)dc.n_refs + more;
    if (want <= dc.cap_refs) return 1;
    if (want > 0xFFFFFFFFull) return 0;
    u64 next = dc.cap_refs ? dc.cap_refs : 4096u;
    while (next < want) next *= 2u;
    if (next > 0xFFFFFFFFull) next = want;
    rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs, (size_t)next * sizeof(*r));
    if (!r) return 0;
    dc.refs = r;
    dc.cap_refs = (u32)next;
    return 1;
}

/* A run of big-endian indices appended as references in one pass: the
 * capacity is reserved up front and the loop works on locals, which the
 * per-index dc_push_ref (reloading dc after every store) could not. A
 * restart sentinel still cuts the strip where it stands. */
static int dc_push_index_run(const u8* run, u32 count, int is_u32, u32 base_index,
                             int restart_en, u32 restart_val)
{
    if (!dc_reserve_refs(count)) return 0;
    rsx_vertex_ref* out = dc.refs;
    u32 n = dc.n_refs;
    const int hash = s_refs_hash_fast;
    u64 h = dc.refs_hash;
    if (is_u32) {
        for (u32 i = 0; i < count; i++) {
            const u8* p = run + (size_t)i * 4u;
            const u32 index = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
            if (restart_en && index == restart_val) {
                dc.n_refs = n;
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) return 0;
                continue;
            }
            out[n].vertex_id = index; out[n].base_index = base_index;
            if (hash) h = eng_refs_hash_ref(h, index, base_index);
            n++;
        }
    } else if (!restart_en) {
        for (u32 i = 0; i < count; i++) {
            const u32 index = (u32)((run[2u * i] << 8) | run[2u * i + 1u]);
            out[n + i].vertex_id = index; out[n + i].base_index = base_index;
            if (hash) h = eng_refs_hash_ref(h, index, base_index);
        }
        n += count;
    } else {
        for (u32 i = 0; i < count; i++) {
            const u32 index = (u32)((run[2u * i] << 8) | run[2u * i + 1u]);
            if (index == restart_val) {
                dc.n_refs = n;
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) return 0;
                continue;
            }
            out[n].vertex_id = index; out[n].base_index = base_index;
            if (hash) h = eng_refs_hash_ref(h, index, base_index);
            n++;
        }
    }
    dc.refs_hash = h;
    dc.n_refs = n;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* PS3RECOMP_ENG_VTX_DIRECT=0 (read at engine init): convert every draw's
 * vertices into dc.verts and hand them to the backend's draw, as before
 * draw_fill, even when the backend has it. */
static int s_vtx_direct = 1;
/* PS3RECOMP_ENG_VTX_REMAP=0: do not collapse repeated vertex references.
 * The GPU draws the duplicates; the CPU skips the per-index hash build. */
static int s_vtx_remap = 1;

/* draw_fill's callback: the first *(u32*)ctx unique vertices into dst. */
static int s_fill_ok;   /* the last fill's result; -1 before it is called */
static int dc_fill_verts(void* ctx, void* dst)
{
    const unsigned long long wp2 = wp2_t0();
    const u32 n = *(const u32*)ctx;
    /* Write-only, possibly write-combined memory: streamed, whole vertices. */
    s_fill_ok = rsx_vertex_fetch_many_stream(&dc.fetch_plan, dc.refs, n,
                                             (u8*)dst);
    if (wp2) {
        wp2_add(WPROF2_CONVERT, wp2);
        s_wp2_conv_verts += n;
        s_wp2_conv_bytes += (unsigned long long)n * dc.layout.stride;
    }
    return s_fill_ok;
}

/* Every unique vertex into dc.verts. */
static int dc_convert_verts(void)
{
    const unsigned long long wp2 = wp2_t0();
    if (!dc_reserve_verts((u64)dc.n_refs * dc.layout.stride))
        return 0;
    const int ok = rsx_vertex_fetch_many(&dc.fetch_plan, dc.refs, dc.n_refs,
                                         dc.layout.stride ? dc.verts : NULL);
    if (wp2) {
        wp2_add(WPROF2_CONVERT, wp2);
        s_wp2_conv_verts += dc.n_refs;
        s_wp2_conv_bytes += (unsigned long long)dc.n_refs * dc.layout.stride;
    }
    return ok;
}

/* The identity of one fetch/remap answer. It covers everything
 * rsx_vertex_fetch_plan_prepare() reads -- the layout, the transform
 * base/divider, all sixteen decoded attribute descriptors and defaults --
 * plus the reference set the remap is built from. The pack/unpack path is
 * deliberately excluded: it has no plan and its stream is already in the
 * FIFO, so it bypasses the cache. */
static u64 eng_fetch_key(const rsx_vertex_fetch_plan* p,
                         const rsx_vertex_layout_plan* layout,
                         u64 refs_hash, u32 n, int allow_remap)
{
    u64 h = 1469598103934665603ull;
    h = eng_fnv1a(layout, sizeof *layout, h);
    h = eng_fnv1a(&p->base_offset, sizeof p->base_offset, h);
    h = eng_fnv1a(&p->divider_mask, sizeof p->divider_mask, h);
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        h = eng_fnv1a(&p->attr[a].desc, sizeof p->attr[a].desc, h);
        h = eng_fnv1a(&p->attr[a].elem_size, sizeof p->attr[a].elem_size, h);
        h = eng_fnv1a(&p->attr[a].stride, sizeof p->attr[a].stride, h);
        h = eng_fnv1a(p->attr[a].default_value, sizeof p->attr[a].default_value, h);
    }
    const u32 flags = (allow_remap ? 1u : 0u) | (s_vtx_remap ? 2u : 0u) |
                      (p->inline_data ? 4u : 0u);
    h = eng_fnv1a(&flags, sizeof flags, h);
    h = eng_fnv1a(&refs_hash, sizeof refs_hash, h);
    h = eng_fnv1a(&n, sizeof n, h);
    return h;
}

/* Identity of a draw's source reference list, hashed once and shared by the
 * fetch and remap memos (the fetch key used to hash the same words again). */
static u64 eng_refs_key(const rsx_vertex_ref* refs, u32 n)
{
    /* The same identity is available incrementally
     * (rsx_vertex_refs_hash_fold) for the fast path; both live in the
     * vertex-compaction unit so a fixture can check they agree. */
    return rsx_vertex_refs_hash(refs, n);
}

static eng_remap_memo* eng_remap_memo_find(u64 key)
{
    if (s_memo_index) {
        const u32 s = eng_memo_index_get(s_rmemo_index, key);
        if (s && s <= ENG_REMAP_MEMO) {
            eng_remap_memo* m = &s_rmemo[s - 1u];
            if (m->used && m->epoch == s_fast_epoch && m->key == key)
                return m;
        }
    }
    for (u32 i = 0; i < ENG_REMAP_MEMO; i++)
        if (s_rmemo[i].used && s_rmemo[i].epoch == s_fast_epoch &&
            s_rmemo[i].key == key)
            return &s_rmemo[i];
    return NULL;
}

static eng_remap_memo* eng_remap_memo_alloc(void)
{
    u32 i;
    for (i = 0; i < ENG_REMAP_MEMO; i++)
        if (!s_rmemo[i].used || s_rmemo[i].epoch != s_fast_epoch) break;
    if (i == ENG_REMAP_MEMO) i = s_rmemo_next++ % ENG_REMAP_MEMO;
    eng_remap_memo* m = &s_rmemo[i];
    m->used = 1;
    m->epoch = s_fast_epoch;
    return m;
}

static void eng_remap_memo_save(u64 key, const rsx_vertex_ref* refs,
                                u32 n_unique, const u32* occ, u32 n_source,
                                int remapped)
{
    eng_remap_memo* m = eng_remap_memo_alloc();
    m->key = key;
    if (s_memo_index)
        eng_memo_index_put(s_rmemo_index, (u32)(m - s_rmemo), key);
    m->n_source = n_source;
    m->n_unique = n_unique;
    m->remapped = remapped;
    if (remapped) {
        if (n_unique > m->refs_cap) {
            rsx_vertex_ref* p = (rsx_vertex_ref*)realloc(
                m->refs, (size_t)n_unique * sizeof(*p));
            if (!p) { m->used = 0; return; }
            m->refs = p;
            m->refs_cap = n_unique;
        }
        if (n_source > m->occ_cap) {
            u32* p = (u32*)realloc(m->occ, (size_t)n_source * sizeof(*p));
            if (!p) { m->used = 0; return; }
            m->occ = p;
            m->occ_cap = n_source;
        }
        if (n_unique) memcpy(m->refs, refs, (size_t)n_unique * sizeof(*refs));
        if (n_source) memcpy(m->occ, occ, (size_t)n_source * sizeof(*occ));
    }
}

static eng_fetch_memo* eng_fetch_memo_find(u64 key)
{
    if (s_memo_index) {
        const u32 s = eng_memo_index_get(s_fmemo_index, key);
        if (s && s <= ENG_FETCH_MEMO) {
            eng_fetch_memo* m = &s_fmemo[s - 1u];
            if (m->used && m->epoch == s_fast_epoch && m->key == key)
                return m;
        }
    }
    for (u32 i = 0; i < ENG_FETCH_MEMO; i++)
        if (s_fmemo[i].used && s_fmemo[i].epoch == s_fast_epoch &&
            s_fmemo[i].key == key)
            return &s_fmemo[i];
    return NULL;
}

static eng_fetch_memo* eng_fetch_memo_alloc(void)
{
    u32 i;
    for (i = 0; i < ENG_FETCH_MEMO; i++)
        if (!s_fmemo[i].used || s_fmemo[i].epoch != s_fast_epoch) break;
    if (i == ENG_FETCH_MEMO) i = s_fmemo_next++ % ENG_FETCH_MEMO;
    eng_fetch_memo* m = &s_fmemo[i];
    m->used = 1;
    m->epoch = s_fast_epoch;
    return m;
}

static int eng_fetch_memo_save_refs(eng_fetch_memo* m, const rsx_vertex_ref* refs,
                                    u32 n)
{
    if (n > m->refs_cap) {
        rsx_vertex_ref* p = (rsx_vertex_ref*)realloc(m->refs,
                                                     (size_t)n * sizeof(*p));
        if (!p) return 0;
        m->refs = p;
        m->refs_cap = n;
    }
    if (n) memcpy(m->refs, refs, (size_t)n * sizeof(*refs));
    return 1;
}

static int eng_fetch_memo_save_occ(eng_fetch_memo* m, const u32* occ, u32 n)
{
    if (n > m->occ_cap) {
        u32* p = (u32*)realloc(m->occ, (size_t)n * sizeof(*p));
        if (!p) return 0;
        m->occ = p;
        m->occ_cap = n;
    }
    if (n) memcpy(m->occ, occ, (size_t)n * sizeof(*occ));
    return 1;
}

static int dc_ensure_refs(u32 n)
{
    if (n <= dc.cap_refs) return 1;
    u32 next = dc.cap_refs ? dc.cap_refs : 4096u;
    while (next < n) next *= 2u;
    rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs, (size_t)next * sizeof(*r));
    if (!r) return 0;
    dc.refs = r;
    dc.cap_refs = next;
    return 1;
}

/* Copy a cached occurrence map back into the accumulator's remap object. The
 * hash table itself is only used by rsx_vertex_remap_build, which a hit does
 * not call; the index writer reads occurrence_to_unique. */
static int dc_ensure_occ(u32 n)
{
    if (n <= dc.ref_remap.occurrence_capacity) return 1;
    u32* p = (u32*)realloc(dc.ref_remap.occurrence_to_unique,
                           (size_t)n * sizeof(*p));
    if (!p) return 0;
    dc.ref_remap.occurrence_to_unique = p;
    dc.ref_remap.occurrence_capacity = n;
    return 1;
}

/* Restore a memoized collapse into the current accumulator. The source refs
 * already built for this draw are dropped only when the answer collapsed
 * them; an uncollapsed hit leaves them in place (it is the same list). The
 * caller falls back to rsx_vertex_remap_build if this cannot allocate. */
static int dc_remap_memo_restore(const eng_remap_memo* m)
{
    if (!m->remapped) {
        dc.n_refs = m->n_unique;
        dc.refs_remapped = 0;
        return 1;
    }
    if (!dc_ensure_refs(m->n_unique) || !dc_ensure_occ(m->n_source))
        return 0;
    if (m->n_unique)
        memcpy(dc.refs, m->refs, (size_t)m->n_unique * sizeof(*dc.refs));
    if (m->n_source)
        memcpy(dc.ref_remap.occurrence_to_unique, m->occ,
               (size_t)m->n_source * sizeof(*m->occ));
    dc.ref_remap.occurrence_capacity = m->n_source;
    dc.n_refs = m->n_unique;
    dc.refs_remapped = 1;
    return 1;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap, int refs_only)
{
    const unsigned long long wp2_refs = wp2_t0();
    if (dc.n_immediate) {
        dc.n_source_refs = dc.n_immediate;
        if (!dc_reserve_verts((u64)dc.n_immediate * layout->stride)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 v = 0; v < dc.n_immediate; v++)
            for (u32 slot = 0; slot < layout->count; slot++)
                memcpy(dc.verts + (u64)v * layout->stride + slot * 16u,
                       dc.immediate[v][layout->attrs[slot]], 16u);
        dc.n_verts = dc.n_immediate;
        wp2_add(WPROF2_FETCH_REFS, wp2_refs);
        return;
    }
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            if (run) {
                if (!dc_push_index_run(run, count, ia.is_u32, base_index, restart_en, restart_val))
                    dc.fetch_ok = 0;
                continue;
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) { wp2_add(WPROF2_FETCH_REFS, wp2_refs); return; }
    wp2_add(WPROF2_FETCH_REFS, wp2_refs);

    const unsigned long long wp2_plan = wp2_t0();
    dc.n_source_refs = dc.n_refs;
    dc.layout = *layout;
    if (refs_only) {
        /* M3: stop after the reference build. The worker does the remap,
         * span resolve, topology/index rebuild and conversion from these
         * references. */
        rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, NULL, NULL);
        wp2_add(WPROF2_FETCH_PLAN, wp2_plan);
        return;
    }

    /* Cut 1. An inline stream has no plan to reuse and an immediate group
     * never reaches here, so both keep the old path. The plan is initialized
     * before the key because the key is over the plan's decoded inputs; a
     * hit then skips the remap hash and the per-attribute span resolution. */
    const int fast_cacheable = s_fetch_memo && !dc.inl;
    const int remap_cacheable = s_remap_cache && s_vtx_remap && allow_remap &&
                                dc.n_refs > 1;
    /* One pass over the source references feeds both memos; the fetch key
     * used to hash the same words a second time. */
    u64 refs_key = 0;
    if (fast_cacheable || remap_cacheable) {
#ifdef ENG_HAVE_FAST_REFS_HASH
        if (s_refs_hash_fast) {
            /* Folded while the list was built; only eng_refs_key()'s tail
             * (the count) is left. */
            refs_key = eng_fnv1a(&dc.n_source_refs, sizeof dc.n_source_refs,
                                 dc.refs_hash);
        } else
#endif
        refs_key = eng_refs_key(dc.refs, dc.n_source_refs);
    }
    rsx_vertex_fetch_plan next_plan;
    u64 fetch_key = 0;
    eng_fetch_memo* fetch_hit = NULL;
    if (fast_cacheable) {
        rsx_vertex_fetch_plan_init(&next_plan, &g.rsx, layout,
                                   eng_guest_ptr, NULL);
        fetch_key = eng_fetch_key(&next_plan, layout, refs_key,
                                  dc.n_source_refs, allow_remap);
        fetch_hit = eng_fetch_memo_find(fetch_key);
    }

    if (fetch_hit) {
        s_fetch_hits++;
        s_wp2_fetch_hit++;
        /* When the remap did not collapse anything, the source references
         * just built are already the unique list: copying megabytes of them
         * back over themselves is the cost this cut exists to remove. */
        const int need_refs = fetch_hit->remapped ||
                              fetch_hit->n_unique != dc.n_source_refs;
        if ((need_refs && !dc_ensure_refs(fetch_hit->n_unique)) ||
            (fetch_hit->remapped && fetch_hit->n_source &&
             !dc_ensure_occ(fetch_hit->n_source))) {
            dc.fetch_ok = 0;
            wp2_add(WPROF2_FETCH_PLAN, wp2_plan);
            return;
        }
        if (need_refs && fetch_hit->n_unique)
            memcpy(dc.refs, fetch_hit->refs,
                   (size_t)fetch_hit->n_unique * sizeof(*dc.refs));
        dc.n_refs = fetch_hit->n_unique;
        dc.refs_remapped = fetch_hit->remapped;
        if (fetch_hit->remapped && fetch_hit->n_source) {
            memcpy(dc.ref_remap.occurrence_to_unique, fetch_hit->occ,
                   (size_t)fetch_hit->n_source * sizeof(*fetch_hit->occ));
            dc.ref_remap.occurrence_capacity = fetch_hit->n_source;
        }
        dc.fetch_plan = fetch_hit->plan;
    } else {
        if (fast_cacheable) { s_fetch_misses++; s_wp2_fetch_miss++; }
        /* Only a topology this engine rebuilds through an index buffer may
         * share repeated references. A line strip or a point list is drawn in
         * the order it arrived, so collapsing duplicates would reorder it. */
        if (s_vtx_remap && allow_remap && dc.n_refs > 1) {
            int restored = 0;
            if (remap_cacheable) {
                eng_remap_memo* rm = eng_remap_memo_find(refs_key);
                if (rm && dc_remap_memo_restore(rm)) {
                    s_remap_hits++;
                    s_wp2_remap_hit++;
                    restored = 1;
                }
            }
            if (!restored) {
                const u32 n_source = dc.n_refs;
                u32 unique = dc.n_refs;
                if (remap_cacheable) {
                    s_remap_misses++;
                    s_wp2_remap_miss++;
                }
                if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs,
                                           &unique)) {
                    dc.refs_remapped = unique < dc.n_refs;
                    dc.n_refs = unique;
                    if (remap_cacheable)
                        eng_remap_memo_save(refs_key, dc.refs, unique,
                                            dc.ref_remap.occurrence_to_unique,
                                            n_source, dc.refs_remapped);
                }
            }
        }
        if (fast_cacheable) {
            dc.fetch_plan = next_plan;
        } else {
            rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout,
                                       eng_guest_ptr, NULL);
            rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx,
                                             dc.inl, dc.inl_bytes);
        }
        rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
        if (fast_cacheable) {
            eng_fetch_memo* m = eng_fetch_memo_alloc();
            m->key = fetch_key;
            if (s_memo_index)
                eng_memo_index_put(s_fmemo_index, (u32)(m - s_fmemo), fetch_key);
            m->n_source = dc.n_source_refs;
            m->n_unique = dc.n_refs;
            m->remapped = dc.refs_remapped;
            m->plan = dc.fetch_plan;
            if (!eng_fetch_memo_save_refs(m, dc.refs, dc.n_refs) ||
                (m->remapped && m->n_source &&
                 !eng_fetch_memo_save_occ(m, dc.ref_remap.occurrence_to_unique,
                                          m->n_source)))
                m->used = 0;
        }
    }
    /* A backend that takes the vertices in its own memory has them converted
     * at the draw, straight into the memory the GPU reads: one write of the
     * converted bytes instead of a write here and a copy there, which read
     * them back cold. Only when the conversion cannot fail, since a failure
     * drops the draw before anything is bound. */
    if (g.be->draw_fill && s_vtx_direct && layout->stride &&
        !rsx_vertex_fetch_plan_can_fail(&dc.fetch_plan)) {
        dc.fetch_deferred = 1;
        dc.n_verts = dc.n_refs;
        wp2_add(WPROF2_FETCH_PLAN, wp2_plan);
        return;
    }
    wp2_add(WPROF2_FETCH_PLAN, wp2_plan);
    if (!dc_convert_verts()) {
        dc.fetch_ok = 0;
        return;
    }
    dc.n_verts = dc.n_refs;
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)prim;
    dc_reset();
    dc.inline_dropped_at_begin = r->inline_dropped;
}

/* A malformed/unbounded group must not render a truncated primitive. */
#define ENG_MAX_IMMEDIATE_VERTICES 4096u
static void sink_immediate_vertex(void* user, const rsx_dispatch* r)
{
    (void)user;
    dc.n_packets++;
    if (!dc.fetch_ok) return;
    if (dc.n_immediate == ENG_MAX_IMMEDIATE_VERTICES) {
        dc.fetch_ok = 0;
        return;
    }
    if (dc.n_immediate == dc.cap_immediate) {
        const u32 next = dc.cap_immediate ? dc.cap_immediate * 2u : 64u;
        void* p = realloc(dc.immediate, (size_t)next * sizeof(*dc.immediate));
        if (!p) { dc.fetch_ok = 0; return; }
        dc.immediate = p;
        dc.cap_immediate = next;
    }
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        float* value = dc.immediate[dc.n_immediate][attr];
        rsx_dsp_vertex_default(r, attr, value);
        /* The legacy default getter treats all-zero words as unwritten.
         * Direct and folded constants may explicitly set W/alpha to zero. */
        const u32 base = (0x1C00u + attr * 16u) >> 2;
        if (r->seen[base] || r->seen[base + 1] ||
            r->seen[base + 2] || r->seen[base + 3])
            memcpy(value, &r->regs[base], 16u);
    }
    dc.n_immediate++;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    /* Record source presence even when its declaration or length is invalid,
     * so mixed immediate/inline groups cannot draw a partial prefix. */
    dc.inl = data;
    dc.inl_bytes = bytes;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_idx >= ENG_MAX_BATCHES) return;
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* A direct copy is valid only for the entire matching physical sample
 * layout. Reinterpreted, swizzled, mipmapped and subrectangle aliases need a
 * different conversion and must not silently sample stale guest bytes. */
static int eng_color_feedback_compatible(const eng_surface* s,
                                          const rsx_dsp_texture* t,
                                          const rsx_dsp_surface* sf)
{
    const u32 base = t->format & 0x9fu;
    u32 bpp = 0, color = 0;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:        if (base == 0x85u) { bpp = 4; color = 8; } break;
    case RSX_BE_FMT_R16G16B16A16F:   if (base == 0x9au) { bpp = 8; color = 11; } break;
    case RSX_BE_FMT_R32G32B32A32F:   if (base == 0x9bu) { bpp = 16; color = 12; } break;
    case RSX_BE_FMT_R32F:            if (base == 0x9cu) { bpp = 4; color = 13; } break;
    default: break;
    }
    if (!bpp || sf->color_format != color || sf->raster_type != 1 ||
        !(t->format & 0x20u) || t->dimension != 2 ||
        t->cubemap || t->mipmaps > 1 || t->depth > 1 ||
        t->width != s->w || t->height != s->h) return 0;
    const u64 row = (u64)s->w * bpp;
    const u64 sp = s->pitch ? s->pitch : row;
    const u64 tp = t->pitch ? t->pitch : row;
    return row && sp >= row && tp == sp;
}

static u32 eng_color_feedback_failure(u32 unit, const eng_surface* s,
                                       const rsx_dsp_texture* t)
{
    static u32 logs;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] rejected colour feedback unit %u: "
                "target %ux%u fmt%u pitch%u, texture %ux%u fmt%02X pitch%u "
                "dim%u levels%u cube%u color%u raster%u\n", unit, s->w, s->h, (u32)s->fmt,
                s->pitch, t->width, t->height, t->format, t->pitch,
                t->dimension, t->mipmaps, t->cubemap, sf.color_format, sf.raster_type);
    return ENG_INVALID;
}

/* A unit's last binding of a plain guest texture -- no colour or depth target
 * at its address -- kept for the rest of the frame: the next draw with the
 * same registers gets the same handle and sampler without the target scans,
 * the sampler decode and the cache probe. The cache checks a texture for
 * changes once a frame, at its first bind, so within the frame the probe
 * would answer the same; s_tex_gen covers a target created at the address
 * and a handle replaced or evicted. PS3RECOMP_ENG_TEXMEMO=0 (read at engine
 * init) turns it off. Port of the inFAMOUS 2 port's binding memo
 * (github.com/ThiagoFraga761/infamous2-pc, MIT). */
typedef struct {
    rsx_dsp_texture t;
    rsx_be_sampler_desc samp;
    u32 handle, frame, gen, entry;
    u32 wgen;               /* the unit's tex_wgen when t was decoded */
} eng_tmemo;
static eng_tmemo s_tmemo[RSX_BE_MAX_TEXTURES];
static int s_texmemo_on;

static int eng_target_at(u32 location, u32 offset)
{
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) return 1;
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].location == location && g.zdepths[i].offset == offset) return 1;
    return 0;
}

/* The shader/draw the current texture binding belongs to, for the dummy
 * census. Set just before sink_bind_textures() in the draw path. */
static u64 s_miss_fp_hash;
static u32 s_miss_pipeline;

/* PS3RECOMP_ENG_FSBIND_LOG=<max lines>: the units a near-full-screen draw
 * samples, with the size, format and wrap it was decoded with. The tiled
 * logo wipe is a full-screen draw sampling a texture at a size/scale the
 * shader's coordinates do not expect; this names it without a GPU capture.
 * Off by default; `max` caps the lines so a run stays readable. */
static int s_fsbind_left = -1;
static int s_fs_draw;

static int eng_fsbind_on(void)
{
    if (s_fsbind_left < 0) {
        const char* e = getenv("PS3RECOMP_ENG_FSBIND_LOG");
        const char* g = getenv("PS3RECOMP_ENG_FSBIND_GATE");
        s_fsbind_left = e ? atoi(e) : (g ? 1000000000 : 0);
    }
    return s_fsbind_left > 0;
}

static void eng_fsbind_log(u32 u, const rsx_dsp_texture* t, const eng_surface* target)
{
    if (s_fsbind_left <= 0) return;
    rsx_be_sampler_desc sd;
    eng_decode_sampler(t->filter, t->wrap, t->control0, &sd);
    /* The tiled wipe needs the sampler to repeat (or mirror) a coordinate
     * past its texture. Logging every full-screen draw floods the run with
     * the clamp-sampled HUD quads, so those are skipped unless
     * PS3RECOMP_ENG_FSBIND_ALL=1. */
    static int all = -1;
    if (all < 0) all = getenv("PS3RECOMP_ENG_FSBIND_ALL") ? 1 : 0;
    const int wraps = sd.wrap_s == 1u || sd.wrap_s == 2u ||
                      sd.wrap_t == 1u || sd.wrap_t == 2u;
    if (!all && !wraps) return;
    /* Optional file gate: log only while it exists, so a run can cover the
     * pregame transition without a cap that boots/menus would exhaust. */
    static const char* gate = (const char*)-1;
    static int gate_open; static unsigned poll;
    if (gate == (const char*)-1) gate = getenv("PS3RECOMP_ENG_FSBIND_GATE");
    if (gate) {
        if ((poll++ & 63u) == 0) {
            FILE* g = fopen(gate, "rb");
            gate_open = g != NULL;
            if (g) fclose(g);
        }
        if (!gate_open) return;
    }
    /* One line per distinct binding: the same full-screen quad repeats every
     * frame and would otherwise bury the run. */
    static u64 seen[4096];
    u64 key = (u64)s_miss_fp_hash ^ ((u64)u << 3) ^ ((u64)t->offset << 17) ^
              ((u64)t->format << 41) ^ ((u64)t->width << 7) ^ ((u64)t->height << 23) ^
              ((u64)t->wrap << 31) ^ ((u64)t->filter << 11) ^
              ((u64)t->mipmaps << 47) ^ ((u64)t->cubemap << 53) ^
              ((u64)(target ? target->offset : 0) << 29);
    const u32 idx = (u32)(key ^ (key >> 32)) & 4095u;
    if (seen[idx] == key) return;
    seen[idx] = key;
    s_fsbind_left--;
    fprintf(stderr, "[eng fsbind] frame=%u fp=0x%016llX unit=%u addr=%u:0x%08X"
                    " fmt=0x%02X %ux%u pitch=%u mips=%u cube=%u dim=%u depth=%u"
                    " wrap=0x%08X ws=%u wt=%u filter=0x%08X lod=%.2f..%.2f"
                    " remap=0x%04X rt=0x%08X %ux%u\n",
            g.frames, (unsigned long long)s_miss_fp_hash, u, t->location,
            t->offset, t->format, t->width, t->height, t->pitch, t->mipmaps,
            t->cubemap, t->dimension, t->depth, t->wrap, sd.wrap_s, sd.wrap_t,
            t->filter, sd.min_lod, sd.max_lod, t->remap & 0xFFFFu,
            target ? target->offset : 0u, target ? target->w : 0u,
            target ? target->h : 0u);
}

static void eng_tex_miss_note_reason(u32 u, const rsx_dsp_texture* t, unsigned reason)
{
    rsx_tex_miss_note(reason, t->location, t->offset, t->format,
                      t->width, t->height, t->pitch, t->mipmaps, t->cubemap,
                      s_miss_fp_hash, s_miss_pipeline);
    (void)u;
}

static void eng_tex_miss_note_unit(u32 u, const rsx_dsp_texture* t)
{
    eng_tex_miss_note_reason(u, t, s_tex_miss_reason);
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot, u32 tex_used,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    u32 feedback[RSX_BE_MAX_COLOR_TARGETS] = {0};
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const rsx_dsp_texture t = *eng_tex(u);
        /* A unit left enabled by an earlier draw but not sampled by this
         * program is skipped: resolving it would cost a cache check, and an
         * aliased surface a store to guest memory, for nothing. */
        if (!t.enabled || !((tex_used >> u) & 1u)) continue;
        mask |= 1u << u;
        if (s_wcap_active) {
            const u32 span = (t.dimension == 3 && !t.cubemap)
                ? eng_texture_span_3d(t.format, t.width, t.height, t.depth,
                                      t.mipmaps, t.pitch)
                : eng_texture_span(t.format, t.width, t.height, t.mipmaps,
                                   t.pitch, (int)t.cubemap);
            if (span) rsx_wcap_note_texture(t.location, t.offset, span);
        }
        if (s_fs_draw && eng_fsbind_on())
            eng_fsbind_log(u, &t, n_targets && target_slots[0] < g.n_surfaces
                                      ? &g.surfaces[target_slots[0]] : NULL);
        eng_tmemo* mm = &s_tmemo[u];
        if (s_texmemo_on && mm->handle && mm->gen == s_tex_gen && mm->frame == g.frames &&
            ((s_tex_wgen_on && mm->wgen == g.rsx.tex_wgen[u]) || !memcmp(&mm->t, &t, sizeof t))) {
            mm->wgen = g.rsx.tex_wgen[u];
            samplers[u] = mm->samp;
            /* Still the most recently used, as the probe would have made it. */
            g.textures[mm->entry].last_use_serial = ++g.texture_use_serial;
            textures[u] = mm->handle;
            continue;
        }
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);
        samplers[u].border = eng_border_class(t.format, t.border_color);

        int sampled = -1, own = 0, surface_at = -1;
        /* DXT data is never a render target's output: no surface colour
         * format is block-compressed, so whatever a compressed texture names
         * came from memory. Matching it against a target at the same offset
         * found the current colour target and dropped the draw as an
         * incompatible feedback read (a 16x16 DXT1 texture over a 1280x720
         * target in Resistance: Fall of Man). Neither colour nor depth
         * surfaces are looked up for it. */
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        const int compressed = base_fmt == RSX_TEX_FMT_DXT1 ||
                               base_fmt == RSX_TEX_FMT_DXT23 ||
                               base_fmt == RSX_TEX_FMT_DXT45;
        /* A cube unit whose base address is a colour target: assemble the six
         * faces on the GPU and bind the cube. If the base target exists but
         * the six faces do not, bind the last complete image for the address;
         * only a probe that was never complete keeps the dummy (and the
         * census says so). Never let the 2-D surface path hand a 2-D target
         * to a cube unit (that is the census's `surface` miss). */
        if (t.cubemap && !compressed && g.be->cube_target_view && eng_cube_rt_on()) {
            eng_cube_desc cd;
            if (eng_cube_collect(&t, &cd)) {
                const u32 all = (1u << ENG_CUBE_FACES) - 1u;
                if (eng_cube_log_on())
                    eng_cube_log_desc(&t, &cd, cd.complete == all ? "cube" : "incomplete");
                if (cd.complete == all) {
                    const u32 cube = g.be->cube_target_view(g.be->user, t.location,
                                                            t.offset, cd.face,
                                                            cd.serials, cd.fmt,
                                                            cd.w, cd.h,
                                                            t.remap & 0xFFFFu,
                                                            t.format);
                    if (cube) { textures[u] = cube; continue; }
                }
                /* A face is being rewritten (its target was recreated) or has
                 * not been rendered yet this scene. Keep the last complete
                 * cube image for this address rather than flashing the dummy,
                 * which the pregame cinematics see as bright/dark
                 * alternation. Only a probe that was never complete stays on
                 * the dummy. */
                if (g.be->cube_target_last) {
                    const u32 last = g.be->cube_target_last(g.be->user, t.location,
                                                            t.offset, cd.fmt,
                                                            cd.w, cd.h,
                                                            t.remap & 0xFFFFu,
                                                            t.format);
                    if (last) { textures[u] = last; continue; }
                }
                eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_CUBE);
                textures[u] = 0;
                continue;
            }
        }
        for (u32 i = 0; i < g.n_surfaces && !compressed; i++) {
            if (!g.surfaces[i].handle || g.surfaces[i].location != t.location ||
                g.surfaces[i].offset != t.offset)
                continue;
            surface_at = (int)i;
            g.surfaces[i].last_use = g.frames;
            /* A current attachment requires an ordered immutable copy; it
             * cannot be sampled directly while the host pass writes it. */
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
            if (own) break;
            sampled = (int)i;
            break;
        }
        /* The depth at this address, when it was written after any colour
         * there: a depth-only pass names its zeta as a colour target too. */
        int zsrc = -1;
        for (u32 i = 0; i < g.n_zdepths && !compressed; i++) {
            const eng_zdepth* z = &g.zdepths[i];
            if (z->location != t.location || z->offset != t.offset || !z->depth_serial)
                continue;
            if (surface_at < 0 || z->depth_serial > g.surfaces[surface_at].color_serial) {
                zsrc = (int)i;
                g.zdepths[i].last_use = g.frames;
            }
            break;
        }
        if (zsrc >= 0) {
            /* DEPTH24_D8 keeps the backend's own snapshot when it has one;
             * anything else goes through guest memory. */
            if (base_fmt == RSX_TEX_FMT_DEPTH24_D8 && current_zslot != (u32)zsrc) {
                const u32 snap = eng_zdepth_snapshot((u32)zsrc, t.width, t.height);
                if (snap) {
                    if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
                    textures[u] = snap; continue;
                }
            }
            const u32 conv = eng_zdepth_as_color((u32)zsrc, &t);
            if (conv) {
                if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
                textures[u] = conv; continue;
            }
            int copied;
            {
                const int saved = s_wcb_trigger;
                s_wcb_trigger = ENG_WCB_TRIG_TEXBIND;
                copied = eng_zdepth_to_guest((u32)zsrc);
                s_wcb_trigger = saved;
            }
            if (copied) {
                static int logs;
                if (s_wcb.attr_on && logs++ < 32) {
                    const eng_zdepth* z = &g.zdepths[zsrc];
                    fprintf(stderr, "[rsx wcb attr] texbind-depth loc=%u addr=0x%08X"
                            " zfmt=%u pitch=%u used=%ux%u <- tex fmt=0x%02X base=0x%02X"
                            " %ux%u pitch=%u mips=%u dim=%u cube=%u linear=%u remap=0x%04X"
                            " current=%u\n",
                            z->location, z->offset, z->zfmt, z->pitch, z->used_w, z->used_h,
                            t.format, base_fmt, t.width, t.height, t.pitch, t.mipmaps,
                            t.dimension, t.cubemap, (t.format & RSX_TEX_FMT_LINEAR) ? 1u : 0u,
                            t.remap & 0xFFFFu, current_zslot == (u32)zsrc);
                }
                textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                               t.width, t.height, t.mipmaps, t.pitch,
                                               (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
                if (!textures[u]) eng_tex_miss_note_unit(u, &t);
                continue;
            }
        }
        if (own && zsrc < 0 && g.be->color_snapshot) {
            eng_surface* s = &g.surfaces[surface_at];
            rsx_dsp_surface sf;
            rsx_dsp_get_surface(&g.rsx, &sf);
            if (!eng_color_feedback_compatible(s, &t, &sf))
                return eng_color_feedback_failure(u, s, &t);
            u32 k = 0;
            while (k < n_targets && target_slots[k] != (u32)surface_at) k++;
            if (k == n_targets) return eng_color_feedback_failure(u, s, &t);
            if (!feedback[k]) {
                eng_mt_sync();
                const u32 copy = g.be->color_snapshot(g.be->user, s->handle);
                if (!copy) return eng_color_feedback_failure(u, s, &t);
                if (s->feedback) g.be->texture_release(g.be->user, s->feedback);
                s->feedback = feedback[k] = copy;
            }
            if (g.be->surface_view) {
                eng_mt_sync();
                textures[u] = g.be->surface_view(g.be->user, feedback[k],
                                               t.remap & 0xffffu, t.format);
                if (!textures[u]) return eng_color_feedback_failure(u, s, &t);
            } else if ((t.remap & 0xffffu) == 0xaae4u) {
                textures[u] = feedback[k];
            } else {
                return eng_color_feedback_failure(u, s, &t);
            }
            if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
            continue;
        }
        int reinterpreted = 0;
        if (sampled >= 0 && zsrc < 0 &&
            g.surfaces[sampled].fmt == RSX_BE_FMT_R8G8B8A8 &&
            eng_format_reinterprets_rgba8(base_fmt)) {
            const int saved = s_wcb_trigger;
            s_wcb_trigger = ENG_WCB_TRIG_TEXBIND;
            reinterpreted = eng_surface_to_guest((u32)sampled);
            s_wcb_trigger = saved;
        }
        if (reinterpreted) {
            static int logs;
            if (s_wcb.attr_on && logs++ < 32) {
                const eng_surface* s = &g.surfaces[sampled];
                fprintf(stderr, "[rsx wcb attr] texbind-reinterpret loc=%u addr=0x%08X"
                        " fmt=%d pitch=%u %ux%u <- tex fmt=0x%02X base=0x%02X %ux%u"
                        " pitch=%u mips=%u dim=%u cube=%u remap=0x%04X\n",
                        s->location, s->offset, (int)s->fmt, s->pitch, s->w, s->h,
                        t.format, base_fmt, t.width, t.height, t.pitch, t.mipmaps,
                        t.dimension, t.cubemap, t.remap & 0xFFFFu);
            }
            textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                           t.width, t.height, t.mipmaps, t.pitch,
                                           (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
            if (!textures[u]) eng_tex_miss_note_unit(u, &t);
            else if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
            continue;
        }
        if (sampled >= 0) {
            const u32 view = g.be->surface_view
                ? (eng_mt_sync(), g.be->surface_view(g.be->user, g.surfaces[sampled].handle,
                                     t.remap & 0xFFFFu, t.format))
                : 0;
            textures[u] = view ? view : g.surfaces[sampled].handle;
            if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
            continue;
        }
        if (base_fmt == RSX_TEX_FMT_DEPTH24_D8) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset && current_zslot != i) {
                    g.zdepths[i].last_use = g.frames;
                    const u32 snap = eng_zdepth_snapshot(i, t.width, t.height);
                    if (snap) {
                        if (t.cubemap) eng_tex_miss_note_reason(u, &t, RSX_TEX_MISS_SURFACE);
                        textures[u] = snap;
                    }
                    break;
                }
            if (textures[u]) continue;
        }
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
        if (!textures[u]) eng_tex_miss_note_unit(u, &t);
        if (s_texmemo_on && textures[u] && s_tslot_last != ENG_INVALID &&
            !eng_target_at(t.location, t.offset)) {
            mm->t = t; mm->samp = samplers[u];
            mm->handle = textures[u]; mm->frame = g.frames; mm->gen = s_tex_gen;
            mm->entry = s_tslot_last; mm->wgen = g.rsx.tex_wgen[u];
        }
    }
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (s_wcap_active) {
            const u32 span = eng_texture_span(vt.format, vt.width, vt.height,
                                              vt.mipmaps, vt.pitch, 0);
            if (span) rsx_wcap_note_texture(vt.location, vt.offset, span);
        }
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        samplers[u].border = eng_border_class(vt.format, vt.border_color);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u, 1u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* ---- per-pass capture (diagnostic) --------------------------------------- */

/* RSX_ENGINE_PASS_DUMP=<existing dir>: capture the draws pass by pass, from
 * the first draw RSX_ENGINE_PASS_DUMP_AFTER seconds after the engine's first
 * one or, without that, from the frame after the first presented frame of at
 * least RSX_ENGINE_PASS_DUMP_MINDRAWS guest draws (default 1).
 *
 * A pass is a run of draws and clears into one target set and zeta. When the
 * set changes, each colour target of the pass that ended is read back to
 * <dir>/passNNN_tK.ppm, its zeta to <dir>/passNNN_z.pgm (stretched from its
 * smallest to its largest value), and <dir>/passes.txt gets one line: the
 * targets, the zeta and its depth range, the draw and clear counts, each
 * clear's scissor, and the first draw's state and textures. Each texture
 * names the colour surface (S) and the depth target (Z) at its address with
 * the serial of their last write -- the newer one is what was sampled -- or G
 * for guest memory. Each presented surface goes to <dir>/presentNNN.ppm,
 * named after the pass count, and the capture runs on across presents until
 * RSX_ENGINE_PASS_DUMP_MAX passes (default 64). RSX_ENGINE_PASS_DUMP_DRAWS=1
 * also writes one line per draw to <dir>/draws.txt: its pipeline, colour
 * mask, blend, depth state and the textures it samples.
 *
 * The question it answers is which pass of a multi-pass frame first comes out
 * wrong. */
static struct {
    int state;              /* 0 unread, 1 waiting, 2 capturing, 3 done/off */
    const char* dir;
    u32 min_draws;
    u32 after_s;
    u32 max_passes;
    time_t start;
    FILE* index;
    FILE* draws_log;        /* RSX_ENGINE_PASS_DUMP_DRAWS: one line a draw */
    u32 pass;
    int open;
    u32 n_targets, targets[RSX_BE_MAX_COLOR_TARGETS], zslot;
    u32 draws, clears;
    char first[2048];
} s_pd;

static int pd_open_index(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/passes.txt", s_pd.dir);
    s_pd.index = fopen(path, "w");
    if (!s_pd.index) { s_pd.state = 3; return 0; }
    if (getenv("RSX_ENGINE_PASS_DUMP_DRAWS")) {
        snprintf(path, sizeof path, "%s/draws.txt", s_pd.dir);
        s_pd.draws_log = fopen(path, "w");
    }
    s_pd.state = 2;
    s_pd.open = 0;
    return 1;
}

static int pd_capturing(void)
{
    if (s_pd.state == 0) {
        s_pd.dir = getenv("RSX_ENGINE_PASS_DUMP");
        const char* m = getenv("RSX_ENGINE_PASS_DUMP_MINDRAWS");
        s_pd.min_draws = m ? (u32)strtoul(m, NULL, 0) : 1u;
        const char* a = getenv("RSX_ENGINE_PASS_DUMP_AFTER");
        s_pd.after_s = a ? (u32)strtoul(a, NULL, 0) : 0u;
        const char* x = getenv("RSX_ENGINE_PASS_DUMP_MAX");
        s_pd.max_passes = x ? (u32)strtoul(x, NULL, 0) : 64u;
        s_pd.start = time(NULL);
        s_pd.state = (s_pd.dir && *s_pd.dir) ? 1 : 3;
    }
    if (s_pd.state == 1 && s_pd.after_s &&
        difftime(time(NULL), s_pd.start) >= (double)s_pd.after_s && pd_open_index())
        fprintf(s_pd.index, "from a draw of frame %u, %u s in\n", g.frames, s_pd.after_s);
    return s_pd.state == 2;
}

static float pd_half(u16 h)
{
    const u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = (float)m / 16777216.0f;              /* m * 2^-24 */
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else {
        v = (1.0f + (float)m / 1024.0f);
        for (u32 i = e; i > 15; i--) v *= 2.0f;
        for (u32 i = e; i < 15; i++) v *= 0.5f;
    }
    return (h & 0x8000u) ? -v : v;
}

static u8 pd_unit(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* One colour surface to a binary PPM, alpha dropped, floats clamped to [0,1]. */
static void pd_write_surface(u32 slot, const char* path)
{
    eng_mt_sync();
    const eng_surface* s = &g.surfaces[slot];
    u32 bpp;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:      bpp = 4;  break;
    case RSX_BE_FMT_R16G16B16A16F: bpp = 8;  break;
    case RSX_BE_FMT_R32G32B32A32F: bpp = 16; break;
    case RSX_BE_FMT_R32F:          bpp = 4;  break;
    default: return;
    }
    if (!s->handle || !s->w || !s->h || !g.be->readback) return;
    u8* raw = (u8*)malloc((size_t)s->w * s->h * bpp);
    u8* rgb = (u8*)malloc((size_t)s->w * s->h * 3u);
    FILE* f = (raw && rgb) ? fopen(path, "wb") : NULL;
    if (f) {
        memset(raw, 0, (size_t)s->w * s->h * bpp);
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, raw, s->w * bpp);
        for (size_t i = 0; i < (size_t)s->w * s->h; i++) {
            const u8* p = raw + i * bpp;
            u8* o = rgb + i * 3u;
            if (s->fmt == RSX_BE_FMT_R8G8B8A8) {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else if (s->fmt == RSX_BE_FMT_R16G16B16A16F) {
                for (u32 c = 0; c < 3; c++) {
                    u16 h; memcpy(&h, p + c * 2u, 2);
                    o[c] = pd_unit(pd_half(h));
                }
            } else if (s->fmt == RSX_BE_FMT_R32G32B32A32F) {
                for (u32 c = 0; c < 3; c++) {
                    float v; memcpy(&v, p + c * 4u, 4);
                    o[c] = pd_unit(v);
                }
            } else {
                float v; memcpy(&v, p, 4);
                o[0] = o[1] = o[2] = pd_unit(v);
            }
        }
        fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
        fwrite(rgb, 1, (size_t)s->w * s->h * 3u, f);
        fclose(f);
    }
    free(raw);
    free(rgb);
}

/* A depth target to a greyscale PPM, stretched from its smallest to its
 * largest value so a shallow range still shows; the range goes to the index. */
static void pd_write_depth(u32 zslot, const char* path, float* lo_out, float* hi_out)
{
    eng_mt_sync();
    const eng_zdepth* z = &g.zdepths[zslot];
    *lo_out = *hi_out = -1.0f;
    if (!g.be->depth_readback || !z->handle) return;
    const u32 w = z->used_w && z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h && z->used_h < z->h ? z->used_h : z->h;
    float* d = (float*)malloc((size_t)w * h * sizeof(float));
    u8* px = (u8*)malloc((size_t)w * h);
    FILE* f = (d && px) ? fopen(path, "wb") : NULL;
    if (f) {
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, d, w * 4u) == 0) {
            float lo = 1.0f, hi = 0.0f;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (d[i] < lo) lo = d[i];
                if (d[i] > hi) hi = d[i];
            }
            const float span = hi > lo ? hi - lo : 1.0f;
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (u8)((d[i] - lo) / span * 255.0f);
            fprintf(f, "P5\n%u %u\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h, f);
            *lo_out = lo; *hi_out = hi;
        }
        fclose(f);
    }
    free(d);
    free(px);
}

static void pd_close_pass(void)
{
    if (!s_pd.open) return;
    char path[1024];
    fprintf(s_pd.index, "pass %03u: draws %u clears %u |", s_pd.pass, s_pd.draws,
            s_pd.clears);
    for (u32 k = 0; k < s_pd.n_targets; k++) {
        const eng_surface* s = &g.surfaces[s_pd.targets[k]];
        fprintf(s_pd.index, " %c 0x%08X fmt %d %ux%u p%u", 'A' + (int)k, s->offset,
                (int)s->fmt, s->w, s->h, s->pitch);
        snprintf(path, sizeof path, "%s/pass%03u_t%u.ppm", s_pd.dir, s_pd.pass, k);
        pd_write_surface(s_pd.targets[k], path);
    }
    if (s_pd.zslot != ENG_INVALID) {
        float lo, hi;
        snprintf(path, sizeof path, "%s/pass%03u_z.pgm", s_pd.dir, s_pd.pass);
        pd_write_depth(s_pd.zslot, path, &lo, &hi);
        const eng_zdepth* z = &g.zdepths[s_pd.zslot];
        fprintf(s_pd.index, " | zeta 0x%08X p%u depth %.9g..%.9g (%ux%u, written %ux%u)",
                z->offset, z->pitch, lo, hi, z->w, z->h, z->used_w, z->used_h);
    }
    fprintf(s_pd.index, " |%s\n", s_pd.first);
    fflush(s_pd.index);
    s_pd.pass++;
    s_pd.open = 0;
    if (s_pd.pass >= s_pd.max_passes) {
        fprintf(s_pd.index, "stopped after %u passes\n", s_pd.pass);
        fclose(s_pd.index);
        s_pd.index = NULL;
        if (s_pd.draws_log) { fclose(s_pd.draws_log); s_pd.draws_log = NULL; }
        s_pd.state = 3;
        fprintf(stderr, "[rsx engine] pass dump: %u passes written to %s\n",
                s_pd.pass, s_pd.dir);
    }
}

/* PS3RECOMP_ENG_POLY_OFFSET=0: draw without the guest's polygon offset (the
 * behaviour before it was honoured). */
static int eng_poly_offset_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET");
        on = !(e && *e && atoi(e) == 0);
    }
    return on;
}

/* A draw (or, with rs NULL, a clear) into this target set: close the pass
 * when the set changed, and describe the first draw of a new one. */
static void pd_note(const u32* targets, u32 n_targets, u32 zslot,
                    const rsx_be_render_state* rs, u32 pipeline, u32 tex_mask)
{
    if (!pd_capturing()) return;
    int same = s_pd.open && s_pd.n_targets == n_targets && s_pd.zslot == zslot;
    for (u32 k = 0; same && k < n_targets; k++) same = s_pd.targets[k] == targets[k];
    if (!same) {
        pd_close_pass();
        s_pd.open = 1;
        s_pd.n_targets = n_targets;
        memcpy(s_pd.targets, targets, n_targets * sizeof(u32));
        s_pd.zslot = zslot;
        s_pd.draws = s_pd.clears = 0;
        s_pd.first[0] = 0;
    }
    if (!rs) {
        s_pd.clears++;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const size_t n = strlen(s_pd.first);
        snprintf(s_pd.first + n, sizeof s_pd.first - n,
                 " [clear after %u draws, scissor %u,%u %ux%u]",
                 s_pd.draws, h & 0xFFFFu, v & 0xFFFFu, h >> 16, v >> 16);
        return;
    }
    if (s_pd.draws_log) {
        fprintf(s_pd.draws_log, "pass %03u draw %u: pipe %u cmask %08X blend %u %X/%X"
                " dt %u dw %u at %u |", s_pd.pass, s_pd.draws, pipeline, rs->color_mask,
                rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->depth_test, rs->depth_write,
                rs->alpha_test_enable);
        if (rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL) & 1u) {
            const u32 f = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FACTOR);
            const u32 b = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
            float ff, fb;
            memcpy(&ff, &f, sizeof ff);
            memcpy(&fb, &b, sizeof fb);
            fprintf(s_pd.draws_log, " po %g/%g", ff, fb);
        }
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!(tex_mask & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            fprintf(s_pd.draws_log, " t%u %u:0x%08X f%02X %ux%u", u, t.location, t.offset,
                    t.format & 0xFFu, t.width, t.height);
        }
        fprintf(s_pd.draws_log, "\n");
    }
    if (s_pd.draws++) return;

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    size_t at = strlen(s_pd.first);
    at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
        " rt_sel %u cfmt %u zfmt %u clip %ux%u | pipe %u dt %u dw %u df %u cmask %08X"
        " blend %u %X/%X stencil %u vpz %g+%g |",
        sf.color_target, sf.color_format, sf.depth_format, sf.clip_w, sf.clip_h,
        pipeline, rs->depth_test, rs->depth_write, rs->depth_func, rs->color_mask,
        rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->stencil_enable,
        vp.scale[2], vp.translate[2]);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && at < sizeof s_pd.first; u++) {
        if (!(tex_mask & (1u << u))) continue;
        const rsx_dsp_texture t = *eng_tex(u);
        /* S<n> a colour surface, Z<n> a depth target, each with the serial
         * of its last write: the newer one is what the binding sampled. */
        char src[48] = "G";
        size_t sl = 0;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == t.location &&
                g.surfaces[i].offset == t.offset) {
                sl = (size_t)snprintf(src, sizeof src, "S%u@%llu", i,
                                      (unsigned long long)g.surfaces[i].color_serial);
                break;
            }
        for (u32 i = 0; i < g.n_zdepths; i++)
            if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                snprintf(src + sl, sizeof src - sl, "%sZ%u@%llu", sl ? "," : "", i,
                         (unsigned long long)g.zdepths[i].depth_serial);
                break;
            }
        at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
            " t%u %u:0x%08X f%02X %ux%u p%u%s r%04X zf%u %s", u, t.location, t.offset,
            t.format & 0xFFu, t.width, t.height, t.pitch, t.cubemap ? " cube" : "",
            t.remap & 0xFFFFu, t.wrap >> 28, src);
    }
}

/* Called on every present: arm on the first frame heavy enough when no delay
 * was asked for, and while capturing, close the pass in progress and write
 * the presented surface. */
static void pd_present(u32 presented)
{
    if (s_pd.state == 0) pd_capturing();
    if (s_pd.state == 1 && !s_pd.after_s && g.guest_draws >= s_pd.min_draws) {
        if (pd_open_index())
            fprintf(s_pd.index, "frame %u (after a frame of %u guest draws)\n",
                    g.frames + 1, g.guest_draws);
        return;
    }
    if (s_pd.state == 2) {
        char path[1024];
        pd_close_pass();
        if (s_pd.state != 2) return;               /* that pass was the last */
        snprintf(path, sizeof path, "%s/present%03u.ppm", s_pd.dir, s_pd.pass);
        pd_write_surface(presented, path);
        fprintf(s_pd.index, "present%03u: surface %u 0x%08X, %u guest draws\n",
                s_pd.pass, presented, g.surfaces[presented].offset, g.guest_draws);
        fflush(s_pd.index);
    }
}

/* The guest scissor intersected with a w x h target, as out[x, y, w, h] in
 * the target's sample grid. The nv40 reset is a full 4096x4096 window and a
 * never-written register reads 0 here, which the w == 0 test treats as no
 * scissor, so only a real game scissor narrows anything
 * (rsx_live_draw.c:6180-6197). */
static void eng_scissor_rect(const rsx_dsp_surface* sf, u32 w, u32 h, u32 out[4])
{
    u32 sx = 0, sy = 0, sw = w, sh = h;
    const u32 hr = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
    const u32 vr = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
    /* The scissor is in pixels: scale it to the sample grid. */
    const u32 gx = (hr & 0xFFFFu) * sf->aa_sx, gw = (hr >> 16) * sf->aa_sx;
    const u32 gy = (vr & 0xFFFFu) * sf->aa_sy, gh = (vr >> 16) * sf->aa_sy;
    if (gw > 0 && gh > 0) {
        u32 right = sx + sw, bottom = sy + sh;
        if (gx > sx) sx = gx;
        if (gy > sy) sy = gy;
        if (gx + gw < right)  right = gx + gw;
        if (gy + gh < bottom) bottom = gy + gh;
        sw = right > sx ? right - sx : 0;
        sh = bottom > sy ? bottom - sy : 0;
    }
    out[0] = sx; out[1] = sy; out[2] = sw; out[3] = sh;
}

/* What a CLEAR_SURFACE may write in a w x h target: the surface clip from
 * the origin, then the scissor, as for a draw. Returns 0 when nothing is
 * left, 1 for the whole target, 2 for part of it (out[] the rectangle).
 *
 * Resistance: Fall of Man's co-op split screen renders both views into one
 * 2x1 antialiased 1280x720 scene target: the scissor on the left half (0,0
 * 640x720), a colour and depth clear and player 1's view, then the scissor
 * on the right half (640,0 640x720), a clear and player 2's view. Cleared
 * whole, the second clear wiped player 1's view, and only the HUD, drawn
 * later into the display buffer, was left on that side. */
static int eng_clear_rect(const rsx_dsp_surface* sf, u32 w, u32 h, u32 out[4])
{
    const u32 cw = sf->clip_w && sf->clip_w < w ? sf->clip_w : w;
    const u32 ch = sf->clip_h && sf->clip_h < h ? sf->clip_h : h;
    eng_scissor_rect(sf, cw, ch, out);
    if (!out[2] || !out[3]) return 0;
    /* Only a scissor narrower than the clip makes a clear partial. A clip
     * smaller than the target alone (a 1280x720 composite pass clearing the
     * 2560x720 zeta of a 2x1 antialiased scene, every frame) stays a whole
     * clear as before: as a partial one it cost every frame a pass of its
     * own, and Resistance's campaign fell from ~30 fps to ~1. */
    const int whole = out[0] == 0 && out[1] == 0 && out[2] >= cw && out[3] >= ch;
    if (whole) { out[0] = out[1] = 0; out[2] = w; out[3] = h; return 1; }
    /* PS3RECOMP_ENG_RECT_CLEAR=0: every clear whole, as before. */
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_RECT_CLEAR"); on = !(e && *e && atoi(e) == 0); }
    if (!on) { out[0] = out[1] = 0; out[2] = w; out[3] = h; return 1; }
    return 2;
}

static void eng_log_rect_clear(const char* what, const u32 rc[4], u32 w, u32 h)
{
    static u32 logs = 0;
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] %s clear limited to %u,%u %ux%u of a %ux%u target\n",
                what, rc[0], rc[1], rc[2], rc[3], w, h);
}

static u64 s_eng_calls = 0;
static u64 s_eng_eligible_m0 = 0;
static u64 s_eng_eligible_m1 = 0;
static u64 s_eng_eligible_m2 = 0;
static u64 s_eng_eligible_m3 = 0;
static u64 s_eng_eligible_remap = 0;
static u64 s_eng_not_deferred = 0;
static u64 s_eng_not_contiguous = 0;
static u64 s_eng_other_layout = 0;

static u32 s_gpu_remap_buf[65536];

static void eng_check_gpu_vertex_offload_ex(rsx_vtx_gpu_desc* desc, u32 uploaded,
                                            int deferred,
                                            const rsx_vertex_layout_plan* l,
                                            const rsx_vertex_fetch_plan* p,
                                            const rsx_vertex_ref* refs,
                                            u32* remap_buf)
{
    memset(desc, 0, sizeof(*desc));
    desc->vertex_count = uploaded;
    __atomic_fetch_add(&s_eng_calls, 1, __ATOMIC_RELAXED);
    if (!uploaded || !deferred || !refs || uploaded > 65536) {
        __atomic_fetch_add(&s_eng_not_deferred, 1, __ATOMIC_RELAXED);
        return;
    }

    if (l->count == 0 || l->count > 8) {
        __atomic_fetch_add(&s_eng_other_layout, 1, __ATOMIC_RELAXED);
        return;
    }

    /* Check that all active attributes have valid stable_base and consistent stride */
    const rsx_vertex_fetch_attr* a0 = &p->attr[l->attrs[0]];
    if (!a0->stable_base || !a0->stride) {
        __atomic_fetch_add(&s_eng_other_layout, 1, __ATOMIC_RELAXED);
        return;
    }

    const u32 src_stride = a0->stride;
    const u8* min_base = a0->stable_base;
    for (u32 i = 1; i < l->count; i++) {
        const rsx_vertex_fetch_attr* a = &p->attr[l->attrs[i]];
        if (!a->stable_base || a->stride != src_stride) {
            __atomic_fetch_add(&s_eng_other_layout, 1, __ATOMIC_RELAXED);
            return;
        }
        if (a->stable_base < min_base) {
            min_base = a->stable_base;
        }
    }

    /* All attributes must reside within [min_base, min_base + src_stride) */
    for (u32 i = 0; i < l->count; i++) {
        const rsx_vertex_fetch_attr* a = &p->attr[l->attrs[i]];
        ptrdiff_t diff = a->stable_base - min_base;
        if (diff < 0 || diff >= (ptrdiff_t)src_stride) {
            __atomic_fetch_add(&s_eng_other_layout, 1, __ATOMIC_RELAXED);
            return;
        }
    }

    u32 mode = 3; /* Generic Descriptor Unpack default */

    /* Mode 0: Fast MLB 3D Mesh (Interleaved Pos3 + UV2 + Color4, Stride 24) */
    if (l->count == 3 && l->attrs[0] == 0 && l->attrs[1] == 3 && l->attrs[2] == 8 &&
        src_stride == 24 &&
        p->attr[0].desc.type == RSX_VTX_TYPE_FLOAT  && p->attr[0].desc.size == 3 &&
        p->attr[3].desc.type == RSX_VTX_TYPE_UNORM8 && p->attr[3].desc.size == 4 &&
        p->attr[8].desc.type == RSX_VTX_TYPE_FLOAT  && p->attr[8].desc.size == 2 &&
        p->attr[0].stable_base == min_base &&
        p->attr[8].stable_base == min_base + 12 &&
        p->attr[3].stable_base == min_base + 20) {
        mode = 0;
    }
    /* Mode 2: Fast MLB 2D UI with Color (Interleaved Pos2 + UV2 + Color4, Stride 20) */
    else if (l->count == 3 && l->attrs[0] == 0 && l->attrs[1] == 3 && l->attrs[2] == 8 &&
             src_stride == 20 &&
             p->attr[0].desc.type == RSX_VTX_TYPE_FLOAT  && p->attr[0].desc.size == 2 &&
             p->attr[8].desc.type == RSX_VTX_TYPE_FLOAT  && p->attr[8].desc.size == 2 &&
             p->attr[3].desc.type == RSX_VTX_TYPE_UNORM8 && p->attr[3].desc.size == 4 &&
             p->attr[0].stable_base == min_base &&
             p->attr[8].stable_base == min_base + 8 &&
             p->attr[3].stable_base == min_base + 16) {
        mode = 2;
    }
    /* Mode 1: Fast MLB 2D UI (Interleaved Pos2 + UV2, Stride 16) */
    else if (l->count == 2 && l->attrs[0] == 0 && l->attrs[1] == 1 &&
             src_stride == 16 &&
             p->attr[0].desc.type == RSX_VTX_TYPE_FLOAT && p->attr[0].desc.size == 2 &&
             p->attr[1].desc.type == RSX_VTX_TYPE_FLOAT && p->attr[1].desc.size == 2 &&
             p->attr[0].stable_base == min_base &&
             p->attr[1].stable_base == min_base + 8) {
        mode = 1;
    }

    /* Populate attribute descriptors for Mode 3 */
    if (mode == 3) {
        desc->num_attrs = l->count;
        for (u32 i = 0; i < l->count; i++) {
            const u32 attr_id = l->attrs[i];
            const rsx_vertex_fetch_attr* a = &p->attr[attr_id];
            u32 slot = (u32)l->slot_by_attr[attr_id];
            u32 off = (u32)(a->stable_base - min_base);
            desc->attrs[i].src_off_dst_slot = (slot << 16u) | (off & 0xFFFFu);
            desc->attrs[i].type_count = ((u32)a->desc.size << 16u) | ((u32)a->desc.type & 0xFFFFu);
        }
    }

    /* Destination stride in floats: each slot is 16 bytes = 4 floats */
    const u32 dst_stride = (mode == 0) ? 12 : (mode == 1) ? 8 : (mode == 2) ? 12 : (l->stride / 4u);

    /* Check contiguous refs */
    int contiguous = 1;
    if (uploaded > 1) {
        const u32 f = refs[0].vertex_id;
        const u32 b = refs[0].base_index;
        for (u32 i = 1; i < uploaded; i++) {
            if (refs[i].vertex_id != f + i || refs[i].base_index != b) {
                contiguous = 0;
                break;
            }
        }
    }

    if (contiguous) {
        desc->gpu_eligible = 1;
        desc->layout_mode = mode;
        desc->flags = 0;
        desc->src_stride = src_stride;
        desc->dst_stride = dst_stride;
        desc->raw_data = min_base;
        desc->raw_bytes = uploaded * src_stride;
        if (mode == 0) __atomic_fetch_add(&s_eng_eligible_m0, 1, __ATOMIC_RELAXED);
        else if (mode == 1) __atomic_fetch_add(&s_eng_eligible_m1, 1, __ATOMIC_RELAXED);
        else if (mode == 2) __atomic_fetch_add(&s_eng_eligible_m2, 1, __ATOMIC_RELAXED);
        else __atomic_fetch_add(&s_eng_eligible_m3, 1, __ATOMIC_RELAXED);
        return;
    }

    /* Non-contiguous / indexed vertex remap offload */
    if (a0->stable_last < a0->stable_first) {
        __atomic_fetch_add(&s_eng_not_contiguous, 1, __ATOMIC_RELAXED);
        return;
    }

    const u32 span = a0->stable_last - a0->stable_first + 1;
    const u32 raw_bytes = span * src_stride;
    /* Safety bounds: reject if span exceeds 4 MB or is excessively sparse */
    if (raw_bytes > 4u * 1024u * 1024u || (span > 4096u && span > uploaded * 8u)) {
        __atomic_fetch_add(&s_eng_not_contiguous, 1, __ATOMIC_RELAXED);
        return;
    }

    const u32 first = a0->stable_first;
    const u32 max_elem = span - 1u;
    for (u32 i = 0; i < uploaded; i++) {
        const u32 elem = ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu) - first;
        if (elem > max_elem) {
            __atomic_fetch_add(&s_eng_not_contiguous, 1, __ATOMIC_RELAXED);
            return;
        }
        remap_buf[i] = elem;
    }

    desc->gpu_eligible = 1;
    desc->layout_mode = mode;
    desc->flags = 1; /* remap enabled */
    desc->src_stride = src_stride;
    desc->dst_stride = dst_stride;
    desc->raw_data = min_base;
    desc->raw_bytes = raw_bytes;
    desc->remap_indices = s_gpu_remap_buf;
    __atomic_fetch_add(&s_eng_eligible_remap, 1, __ATOMIC_RELAXED);
    if (mode == 0) __atomic_fetch_add(&s_eng_eligible_m0, 1, __ATOMIC_RELAXED);
    else if (mode == 1) __atomic_fetch_add(&s_eng_eligible_m1, 1, __ATOMIC_RELAXED);
    else if (mode == 2) __atomic_fetch_add(&s_eng_eligible_m2, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&s_eng_eligible_m3, 1, __ATOMIC_RELAXED);
}

static void eng_check_gpu_vertex_offload(rsx_vtx_gpu_desc* desc, u32 uploaded)
{
    eng_check_gpu_vertex_offload_ex(desc, uploaded, dc.fetch_deferred, &dc.layout,
                                    &dc.fetch_plan, dc.refs, s_gpu_remap_buf);
}

/* ---- multi-threaded draw packets (PS3RECOMP_WALKER_MT) ------------------
 *
 * The walker stays the state thread: it decodes the FIFO, resolves surfaces,
 * textures and pipelines, and builds an immutable packet per draw. In the
 * serial packet path (WORKERS=0) the packet is committed on the walker at
 * once. With one record worker (WORKERS=1) the packet and every byte it
 * borrowed are copied into an SPSC ring and the worker replays the backend
 * calls in draw order. Synchronous backend calls (creates, snapshots,
 * readbacks, present, WCB) drain the ring first, so the backend sees the same
 * sequence on one thread at a time.
 *
 * Packet/copied-payload/drain shape derived from the MIT inFAMOUS 2 port
 * (rsx_be_thread.c, bq_direct_begin): Portions derived from inFAMOUS 2 recomp
 * (c) 2026 the infamous2-recomp contributors, MIT.
 */

#define ENG_MT_PKT_MAGIC 0x4d545044u   /* "MTDP" */

typedef struct {
    rsx_dsp_vertex_attr desc;
    u32 elem_size;
    u32 stride;
    float def[4];
    u32 has_span;
    u32 span_off;
    u32 stable_first;
    u32 stable_last;
} eng_pkt_attr;

typedef struct {
    u32 magic;
    u32 total_bytes;
    u64 seq;

    u32 topology;
    u32 indexed;
    u32 n_draw;
    u32 uploaded;
    u32 stride;
    u32 deferred;
    u32 snapshot_ok;

    u32 n_targets;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];      /* backend handles */
    u32 track_targets[RSX_BE_MAX_COLOR_TARGETS]; /* engine slots */
    u32 depth_handle;
    u32 zslot;
    u32 do_clear_depth;
    u32 clear_depth_handle;
    u32 pipeline;

    u32 vpc_range;
    u32 vpc_lo;
    u32 vpc_n;
    u32 vpc_total;
    u32 fp_bytes;
    u32 tex_mask;
    u32 vtex_mask;
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    float W, H;
    u32 sc[4];
    u32 stencil;
    u32 has_bounds, has_bias, bias_bits;
    float bounds[2], bias[2];

    /* Payload, as 16-byte-aligned offsets from the packet start (0 = absent).
     * The same offsets are valid after the packet is copied into the ring. */
    u32 off_rows, rows_bytes;
    u32 off_tail, tail_bytes;
    u32 off_vp_full, vp_full_bytes;
    u32 off_fp;
    u32 off_verts;
    u32 off_idx;
    u32 off_refs, n_refs;
    u32 off_raw, raw_bytes;
    u32 off_remap, n_remap;
    u32 off_inline, inline_bytes;

    /* M3 worker packet: the state thread ships the source references, the
     * post-init fetch plan (guest pointer filled in by the worker) and the
     * draw's primitive/rebuild flags. The worker does remap, span resolve,
     * topology/index rebuild and conversion in its own scratch. */
    u32 prim;
    u32 rebuild;
    u32 allow_remap;
    u32 n_src;
    u32 off_src;
    u32 n_cuts;
    u32 off_cuts;
    rsx_vertex_fetch_plan plan_init;

    /* Vertex plan snapshot (pointers replaced by span offsets). */
    rsx_vertex_layout_plan plan_layout;
    u32 plan_base_offset, plan_divider_mask;
    u32 plan_inline_stride;
    u32 plan_inline_off[RSX_DSP_NUM_VERTEX_ATTR];
    eng_pkt_attr plan_attr[RSX_DSP_NUM_VERTEX_ATTR];
    rsx_vtx_gpu_desc gpu_desc;

    /* Engine bookkeeping, applied on the state thread in worker mode. */
    u32 track_color_mask;
    u32 track_depth_write;
    rsx_dsp_surface track_sf;
} eng_draw_pkt;

#define ENG_MT_MAX_WORKERS 4
#define ENG_MT_ARENA_BYTES (32u * 1024u * 1024u)
#define ENG_MT_CHUNK_PKTS  16u

/* Results of one packet's preparation, committed later in turn order. The
 * pointed-to bytes live in the worker's chunk arena. */
typedef struct {
    const u8* verts;
    const u32* idx;
    rsx_vtx_gpu_desc gpu;
    int ok, deferred, indexed;
    u32 n_draw, uploaded, topology;
} eng_prep;

/* Per-worker preparation state. The state thread hands each worker a chunk of
 * immutable packets; the worker does everything that needs no guest ordering
 * (remap, span resolve, topology/index rebuild, conversion) and then waits for
 * each packet's global turn to record the backend calls. The turn counter is
 * the ordered commit: packet `seq` may only touch the backend when
 * next_commit == seq, so the backend sees them in guest order on one thread
 * at a time while the expensive preparation runs on N threads. */
typedef struct {
    int id;
    rsx_vertex_remap remap;
    rsx_vertex_ref* refs; u32 refs_cap, n_unique, n_source;
    u32* remap_buf; u32 remap_cap;
    rsx_vertex_fetch_plan plan;
    u8* arena; u64 arena_cap, arena_used;
    eng_prep prep[ENG_MT_CHUNK_PKTS];
} eng_worker;

static struct {
    int inited;
    int on;
    int workers;
    rsx_record_queue* q[ENG_MT_MAX_WORKERS];
    eng_worker w[ENG_MT_MAX_WORKERS];
    eng_worker inline_w;
    u8* scratch;
    u32 cap, used;
    /* The chunk being filled by the state thread: [u32 count][u32 pad] and
     * then whole packet records back to back. */
    u8* chunk;
    u32 chunk_cap, chunk_used, chunk_pkts;
    u64 chunk_id, seq, pushed_seq;
    _Atomic u64 next_commit;
    u64 enqueued, inline_fallbacks, syncs, snapshots, snapshot_fail, chunks;
} s_mt;

static void eng_mt_worker_exec(void* ctx, const void* rec, u32 size);
static void eng_mt_sync(void);
static int  eng_mt_prepare_one(const eng_draw_pkt* p, eng_worker* w, eng_prep* out);
static void eng_mt_commit_packet(const eng_draw_pkt* p, const eng_prep* r);
static void eng_mt_run_packet_inline(const eng_draw_pkt* p);

static int eng_mt_env_int(const char* name, int def)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return (int)strtol(e, NULL, 0);
}

static int eng_mt_on(void)
{
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_WALKER_MT) != 0;
    return eng_mt_env_int("PS3RECOMP_WALKER_MT", 0) != 0;
}

static int eng_mt_workers(void)
{
    int n = ps3_live_ctl_get ? ps3_live_ctl_get(PS3_LIVE_WALKER_MT_WORKERS)
                             : eng_mt_env_int("PS3RECOMP_WALKER_MT_WORKERS", 0);
    if (n < 0) n = 0;
    if (n > ENG_MT_MAX_WORKERS) n = ENG_MT_MAX_WORKERS;
    return n;
}

static int eng_mt_snapshot_on(void)
{
    const char* e = getenv("PS3RECOMP_WALKER_MT_SNAPSHOT");
    if (!e || !*e) return 1;               /* conservative default */
    return strcmp(e, "0") != 0;
}

static int eng_w_reserve(void** p, u32* cap, u32 n, size_t elem)
{
    if (n <= *cap) return 1;
    u32 next = *cap ? *cap : 4096u;
    while (next < n) next *= 2u;
    void* q = realloc(*p, (size_t)next * elem);
    if (!q) return 0;
    *p = q; *cap = next;
    return 1;
}

static int eng_w_reserve64(void** p, u64* cap, u64 n)
{
    if (n <= *cap) return 1;
    u64 next = *cap ? *cap : 1u << 20;
    while (next < n) next *= 2u;
    void* q = realloc(*p, (size_t)next);
    if (!q) return 0;
    *p = q; *cap = next;
    return 1;
}

static u8* eng_w_bump(eng_worker* w, u64 bytes)
{
    bytes = (bytes + 15u) & ~15ull;
    if (w->arena_used + bytes > w->arena_cap) return NULL;
    u8* p = w->arena + w->arena_used;
    w->arena_used += bytes;
    return p;
}

static void eng_worker_free(eng_worker* w)
{
    free(w->refs); free(w->remap_buf);
    free(w->remap.occurrence_to_unique); free(w->remap.slots);
    free(w->arena);
    memset(w, 0, sizeof *w);
}

static void eng_mt_worker_ready(eng_worker* w, int id)
{
    memset(w, 0, sizeof *w);
    w->id = id;
    /* Reserved once; never reallocated while a chunk's outputs point into it. */
    if (!eng_w_reserve64((void**)&w->arena, &w->arena_cap, ENG_MT_ARENA_BYTES))
        fprintf(stderr, "[walker-mt] worker %d: arena allocation failed\n", id);
}

static void eng_mt_reset(void)
{
    for (int i = 0; i < ENG_MT_MAX_WORKERS; i++) {
        if (s_mt.q[i]) rsx_record_queue_destroy(s_mt.q[i]);
        eng_worker_free(&s_mt.w[i]);
    }
    eng_worker_free(&s_mt.inline_w);
    free(s_mt.chunk);
    free(s_mt.scratch);
    memset(&s_mt, 0, sizeof s_mt);
}

/* Make sure the scratch packet arena, chunk buffer and record workers exist. */
static int eng_mt_ensure(void)
{
    const int on = eng_mt_on();
    if (!on) {
        if (s_mt.q[0] || s_mt.on) eng_mt_sync();
        s_mt.on = 0;
        return 0;
    }
    s_mt.on = 1;
    if (s_mt.inited) return 1;
    s_mt.inited = 1;
    s_mt.workers = eng_mt_workers();
    /* The packet arena never reallocates while a packet is being built: every
     * offset in the packet is relative to this one block. */
    s_mt.cap = 64u * 1024u * 1024u;
    s_mt.scratch = (u8*)malloc(s_mt.cap);
    if (!s_mt.scratch) {
        s_mt.cap = 0;
        s_mt.on = 0;
        return 0;
    }
    s_mt.chunk_cap = (u32)eng_mt_env_int("PS3RECOMP_WALKER_MT_CHUNK_KB", 4096) * 1024u;
    if (s_mt.chunk_cap < 64u * 1024u) s_mt.chunk_cap = 64u * 1024u;
    s_mt.chunk = (u8*)malloc(s_mt.chunk_cap);
    if (!s_mt.chunk) {
        free(s_mt.scratch); s_mt.scratch = NULL; s_mt.cap = 0; s_mt.on = 0;
        return 0;
    }
    s_mt.chunk_used = 8;
    s_mt.next_commit = 1;
    eng_mt_worker_ready(&s_mt.inline_w, -1);
    if (s_mt.workers > 0) {
        const int mb = eng_mt_env_int("PS3RECOMP_WALKER_MT_QUEUE_MB", 16);
        const u32 qcap = (u32)(mb > 0 ? mb : 16) * 1024u * 1024u;
        int up = 0;
        for (int i = 0; i < s_mt.workers; i++) {
            eng_mt_worker_ready(&s_mt.w[i], i);
            s_mt.q[i] = rsx_record_queue_new(qcap);
            if (!s_mt.q[i] || rsx_record_queue_start(s_mt.q[i], eng_mt_worker_exec,
                                                     &s_mt.w[i]) != 0) {
                if (s_mt.q[i]) { rsx_record_queue_destroy(s_mt.q[i]); s_mt.q[i] = NULL; }
                break;
            }
            up++;
        }
        s_mt.workers = up;
        if (!up)
            fprintf(stderr, "[walker-mt] record workers did not start; serial packet path\n");
        else
            fprintf(stderr, "[walker-mt] %d record workers up (queue %d MB, chunk %u KB)\n",
                    up, mb > 0 ? mb : 16, s_mt.chunk_cap / 1024u);
    }
    return 1;
}

static u32 eng_mt_alloc(u32 bytes)
{
    bytes = (bytes + 15u) & ~15u;
    const u32 need = s_mt.used + bytes;
    if (need < s_mt.used || need > s_mt.cap) return 0;
    const u32 off = s_mt.used;
    s_mt.used = need;
    return off;
}

static void eng_mt_wait_turn(u64 seq)
{
    int spins = 0;
    while (atomic_load_explicit(&s_mt.next_commit, memory_order_acquire) != seq) {
        if (++spins < 20000) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
            continue;
        }
        spins = 0;
        sched_yield();
    }
}

static void eng_mt_take_turn(u64 seq)
{
    atomic_store_explicit(&s_mt.next_commit, seq + 1, memory_order_release);
}

static void eng_mt_wait_upto(u64 upto)
{
    int spins = 0;
    while (atomic_load_explicit(&s_mt.next_commit, memory_order_acquire) <= upto) {
        if (++spins < 20000) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
            continue;
        }
        spins = 0;
        sched_yield();
    }
}

static int eng_mt_chunk_add(const void* rec, u32 bytes)
{
    if (s_mt.chunk_pkts >= ENG_MT_CHUNK_PKTS) return 0;
    if (s_mt.chunk_used + bytes > s_mt.chunk_cap) return 0;
    memcpy(s_mt.chunk + s_mt.chunk_used, rec, bytes);
    s_mt.chunk_used += bytes;
    s_mt.chunk_pkts++;
    return 1;
}

static void eng_mt_chunk_close(void)
{
    if (!s_mt.chunk || s_mt.chunk_pkts == 0) return;
    memcpy(s_mt.chunk, &s_mt.chunk_pkts, sizeof(u32));
    const u32 bytes = s_mt.chunk_used;
    if (s_mt.workers > 0) {
        const int w = (int)(s_mt.chunk_id % (u64)s_mt.workers);
        if (!rsx_record_queue_push(s_mt.q[w], s_mt.chunk, bytes)) {
            /* The worker's ring is full: wait for everything already pushed
             * to commit (the ring drains as it runs), never for this chunk. */
            eng_mt_wait_upto(s_mt.pushed_seq);
            if (!rsx_record_queue_push(s_mt.q[w], s_mt.chunk, bytes)) {
                /* Still no room (a chunk bigger than the queue): run it here,
                 * packet by packet, after the drain left the turn here. */
                const u8* q = s_mt.chunk + 8;
                for (u32 i = 0; i < s_mt.chunk_pkts; i++) {
                    const eng_draw_pkt* p = (const eng_draw_pkt*)q;
                    eng_mt_run_packet_inline(p);
                    q += p->total_bytes;
                }
                s_mt.pushed_seq = s_mt.seq;
                s_mt.chunk_pkts = 0;
                s_mt.chunk_used = 8;
                s_mt.chunk_id++;
                return;
            }
        }
        s_mt.pushed_seq = s_mt.seq;
        s_mt.enqueued++;
    } else {
        /* Serial packet path: run inline in packet order. */
        const u8* q = s_mt.chunk + 8;
        for (u32 i = 0; i < s_mt.chunk_pkts; i++) {
            const eng_draw_pkt* p = (const eng_draw_pkt*)q;
            eng_mt_run_packet_inline(p);
            q += p->total_bytes;
        }
    }
    s_mt.chunk_pkts = 0;
    s_mt.chunk_used = 8;
    s_mt.chunk_id++;
    s_mt.chunks++;
}

/* Drain every published chunk. Called before any backend call the state
 * thread makes itself, and at every guest-visible sync point. Closes the open
 * chunk first so a sync point is also a chunk boundary. */
static void eng_mt_sync(void)
{
    if (!s_mt.on) return;
    eng_mt_chunk_close();
    if (!s_mt.workers) return;
    eng_mt_wait_upto(s_mt.seq);
    s_mt.syncs++;
}

void rsx_draw_engine_mt_sync(void)
{
    eng_mt_sync();
}

/* Snapshot every stable source span into the worker arena and repoint the
 * plan at the copy. This is the conservative path (default): the c690 mesh
 * rings can be rewritten while a packet waits in the queue. */
static int eng_mt_snapshot_spans(eng_worker* w)
{
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* s = &w->plan.attr[a];
        if (!s->stable_base) continue;
        const u64 bytes = (u64)(s->stable_last - s->stable_first) * s->stride +
                          s->elem_size;
        if (!bytes) continue;
        if (bytes > 32u * 1024u * 1024u) return 0;
        u8* dst = eng_w_bump(w, bytes);
        if (!dst) return 0;
        memcpy(dst, s->stable_base, (size_t)bytes);
        s->stable_base = dst;
    }
    return 1;
}

/* Worker-side guest reads: raw host pointer (live) or the replay harness's
 * cap_read. Never the WCB access hook -- WCB is serviced on the owner thread. */
static const u8* eng_guest_ptr_worker(void* user, u32 location, u32 offset,
                                      u32 min_bytes)
{
    (void)user;
    return eng_guest_ptr_raw(location, offset, min_bytes);
}

/* Reproduce dc_fetch's post-reference work on the worker: remap, span
 * resolve, topology/index rebuild, GPU descriptor and conversion. All output
 * lives in the worker arena (no realloc between packets), so a whole chunk
 * can be prepared before the first packet takes its commit turn. Returns 0
 * to drop the packet (nothing has touched the backend yet). */
static int eng_mt_prepare_one(const eng_draw_pkt* p, eng_worker* w, eng_prep* out)
{
    memset(out, 0, sizeof *out);
    const rsx_vertex_layout_plan* L = &p->plan_init.layout;
    const u32 n = p->n_src;
    w->n_source = n;
    w->n_unique = n;
    if (!n) return 0;
    if (!eng_w_reserve((void**)&w->refs, &w->refs_cap, n, sizeof(rsx_vertex_ref)))
        return 0;
    memcpy(w->refs, (const u8*)p + p->off_src, (size_t)n * sizeof(rsx_vertex_ref));

    u32 unique = n;
    int remapped = 0;
    if (s_vtx_remap && p->allow_remap && n > 1) {
        if (rsx_vertex_remap_build(&w->remap, w->refs, n, &unique))
            remapped = unique < n;
        else
            return 0;
    }
    w->n_unique = unique;
    w->plan = p->plan_init;
    w->plan.guest_ptr = eng_guest_ptr_worker;
    w->plan.guest_user = NULL;
    rsx_vertex_fetch_plan_prepare(&w->plan, w->refs, unique);

    const int deferred = g.be->draw_fill && s_vtx_direct && L->stride &&
                         !rsx_vertex_fetch_plan_can_fail(&w->plan);
    out->deferred = deferred;

    const u32* cuts = p->n_cuts ? (const u32*)((const u8*)p + p->off_cuts) : NULL;
    int indexed = 0;
    u32 n_draw = n;
    u32 topology = p->topology;
    if (p->rebuild) {
        if (!eng_topology_rebuild(p->prim, remapped, &indexed)) return 0;
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(p->prim, n, cuts, p->n_cuts)
            : n - n % 3u;
    } else {
        const rsx_topology topo = rsx_primitive_topology(p->prim);
        if (topo == RSX_TOPOLOGY_UNSUPPORTED) return 0;
        topology = (u32)topo;
    }
    if (!n_draw) return 0;
    out->indexed = indexed;
    out->n_draw = n_draw;
    out->uploaded = indexed ? unique : n_draw;
    out->topology = topology;

    if (indexed) {
        u32* idx = (u32*)eng_w_bump(w, (u64)n_draw * sizeof(u32));
        if (!idx) return 0;
        rsx_draw_engine_write_topology_indices(
            p->prim, n, cuts, p->n_cuts,
            remapped ? w->remap.occurrence_to_unique : NULL, idx);
        out->idx = idx;
    }

    if (eng_mt_snapshot_on()) {
        if (!eng_mt_snapshot_spans(w)) { __atomic_fetch_add(&s_mt.snapshot_fail, 1, __ATOMIC_RELAXED); return 0; }
        __atomic_fetch_add(&s_mt.snapshots, 1, __ATOMIC_RELAXED);
    }

    if (!eng_w_reserve((void**)&w->remap_buf, &w->remap_cap, out->uploaded, sizeof(u32)))
        return 0;
    eng_check_gpu_vertex_offload_ex(&out->gpu, out->uploaded, deferred, L,
                                    &w->plan, w->refs, w->remap_buf);

    const u64 vbytes = (u64)unique * L->stride;
    if (!vbytes) return 0;
    u8* verts = eng_w_bump(w, vbytes);
    if (!verts) return 0;
    const int ok = deferred
        ? rsx_vertex_fetch_many_stream(&w->plan, w->refs, unique, verts)
        : rsx_vertex_fetch_many(&w->plan, w->refs, unique, verts);
    if (!ok) return 0;
    out->verts = verts;
    out->ok = 1;
    return 1;
}

typedef struct {
    rsx_vtx_gpu_desc gpu;          /* must stay first: the backend reads it */
    const u8* verts;
    u32 bytes;
} eng_mt_fill_ctx;

static int eng_mt_copy_fill(void* ctx, void* dst)
{
    eng_mt_fill_ctx* c = (eng_mt_fill_ctx*)ctx;
    memcpy(dst, c->verts, c->bytes);
    s_fill_ok = 1;
    return 1;
}

static void eng_mt_run_packet_inline(const eng_draw_pkt* p)
{
    eng_prep pr;
    const int ok = eng_mt_prepare_one(p, &s_mt.inline_w, &pr);
    eng_mt_wait_turn(p->seq);
    if (ok && pr.ok) eng_mt_commit_packet(p, &pr);
    eng_mt_take_turn(p->seq);
    s_mt.inline_fallbacks++;
}

static void eng_pkt_binds(const eng_draw_pkt* p)
{
    if (p->do_clear_depth)
        g.be->clear_depth_stencil(g.be->user, p->clear_depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
}

/* Execute a packet from the walker, with the draw payload still in dc /
 * g.indices (the serial packet path, workers==0). */
static void eng_pkt_commit_live(eng_draw_pkt* p)
{
    eng_pkt_binds(p);
    g.be->bind_targets(g.be->user, p->targets, p->n_targets, p->depth_handle);
    g.be->bind_pipeline(g.be->user, p->pipeline);
    if (p->vpc_range) {
        g.be->bind_vs_constants_range(g.be->user, g.rsx.constants, p->vpc_lo,
                                      p->vpc_n, s_mt.scratch + p->off_tail,
                                      p->tail_bytes, p->vpc_total);
    } else {
        g.be->bind_vs_constants(g.be->user, g.vp_cb, p->vp_full_bytes);
    }
    g.be->bind_ps_constants(g.be->user, s_mt.scratch + p->off_fp, p->fp_bytes);
    g.be->bind_textures(g.be->user, p->textures, p->samplers, p->tex_mask);
    g.be->bind_vertex_textures(g.be->user, p->vtextures, p->vsamplers,
                               p->vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, p->W, p->H);
    g.be->set_scissor(g.be->user, p->sc[0], p->sc[1], p->sc[2], p->sc[3]);
    g.be->set_stencil_ref(g.be->user, p->stencil);
    if (g.be->set_depth_bounds && p->has_bounds)
        g.be->set_depth_bounds(g.be->user, p->bounds[0], p->bounds[1]);
    if (g.be->set_depth_bias && p->has_bias)
        g.be->set_depth_bias(g.be->user, p->bias[0], p->bias[1], p->bias_bits);

    if (p->deferred) {
        s_fill_ok = -1;
        rsx_vtx_gpu_desc gpu_desc;
        { const unsigned long long w2 = wp2_t0();
          eng_check_gpu_vertex_offload(&gpu_desc, p->uploaded);
          wp2_add(WPROF2_VTXOFF, w2); }
        const int drew = g.be->draw_fill(g.be->user, (rsx_topology)p->topology,
                            dc_fill_verts, &gpu_desc, p->uploaded, p->stride,
                            p->indexed ? g.indices : NULL,
                            p->indexed ? p->n_draw : 0);
        if (!drew) {
            if (!dc_convert_verts()) return;
            g.be->draw(g.be->user, (rsx_topology)p->topology, dc.verts,
                       p->uploaded, p->stride, p->indexed ? g.indices : NULL,
                       p->indexed ? p->n_draw : 0);
        }
    } else {
        g.be->draw(g.be->user, (rsx_topology)p->topology, dc.verts, p->uploaded,
                   p->stride, p->indexed ? g.indices : NULL,
                   p->indexed ? p->n_draw : 0);
    }
}

static void eng_pkt_track(const eng_draw_pkt* p)
{
    if (p->track_color_mask) {
        eng_surfaces_wrote(p->track_targets, p->n_targets);
        for (u32 i = 0; i < p->n_targets; i++) {
            g.surfaces[p->track_targets[i]].last_writer_fp = s_miss_fp_hash;
            g.surfaces[p->track_targets[i]].last_writer_pipe = s_miss_pipeline;
        }
    }
    if (p->track_depth_write && p->zslot != ENG_INVALID) {
        g.zdepths[p->zslot].had_write = 1;
        eng_zdepth_wrote(p->zslot, &p->track_sf);
    }
}

static void eng_mt_commit_packet(const eng_draw_pkt* p, const eng_prep* r)
{
    eng_pkt_binds(p);
    g.be->bind_targets(g.be->user, p->targets, p->n_targets, p->depth_handle);
    g.be->bind_pipeline(g.be->user, p->pipeline);
    const u8* base = (const u8*)p;
    if (p->vpc_range)
        g.be->bind_vs_constants_range(g.be->user, base + p->off_rows, p->vpc_lo,
                                      p->vpc_n, base + p->off_tail, p->tail_bytes,
                                      p->vpc_total);
    else
        g.be->bind_vs_constants(g.be->user, base + p->off_vp_full,
                                p->vp_full_bytes);
    g.be->bind_ps_constants(g.be->user, base + p->off_fp, p->fp_bytes);
    g.be->bind_textures(g.be->user, p->textures, p->samplers, p->tex_mask);
    g.be->bind_vertex_textures(g.be->user, p->vtextures, p->vsamplers,
                               p->vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, p->W, p->H);
    g.be->set_scissor(g.be->user, p->sc[0], p->sc[1], p->sc[2], p->sc[3]);
    g.be->set_stencil_ref(g.be->user, p->stencil);
    if (g.be->set_depth_bounds && p->has_bounds)
        g.be->set_depth_bounds(g.be->user, p->bounds[0], p->bounds[1]);
    if (g.be->set_depth_bias && p->has_bias)
        g.be->set_depth_bias(g.be->user, p->bias[0], p->bias[1], p->bias_bits);

    const u32* idx = r->indexed ? r->idx : NULL;
    if (r->deferred) {
        eng_mt_fill_ctx ctx;
        ctx.gpu = r->gpu;
        ctx.verts = r->verts;
        ctx.bytes = (u32)((u64)r->uploaded * p->stride);
        g.be->draw_fill(g.be->user, (rsx_topology)r->topology, eng_mt_copy_fill,
                        &ctx, r->uploaded, p->stride, idx,
                        r->indexed ? r->n_draw : 0);
    } else {
        g.be->draw(g.be->user, (rsx_topology)r->topology, r->verts, r->uploaded,
                   p->stride, idx, r->indexed ? r->n_draw : 0);
    }
}

static void eng_mt_worker_exec(void* ctx, const void* rec, u32 size)
{
    eng_worker* w = (eng_worker*)ctx;
    if (size < 8) return;
    u32 n = 0;
    memcpy(&n, rec, sizeof n);
    const u8* q = (const u8*)rec + 8;
    const u8* end = (const u8*)rec + size;
    if (n > ENG_MT_CHUNK_PKTS) n = ENG_MT_CHUNK_PKTS;

    /* Pass 1: prepare the whole chunk. The arena is reserved once, and its
     * outputs point into it, so nothing may realloc during this pass. */
    const u8* qq = q;
    u32 prepared = 0;
    w->arena_used = 0;
    for (u32 i = 0; i < n && qq + sizeof(eng_draw_pkt) <= end; i++) {
        const eng_draw_pkt* p = (const eng_draw_pkt*)qq;
        if (p->magic != ENG_MT_PKT_MAGIC || !p->total_bytes) break;
        eng_mt_prepare_one(p, w, &w->prep[i]);
        prepared++;
        qq += p->total_bytes;
    }

    /* Pass 2: commit in global packet order. */
    for (u32 i = 0; i < prepared; i++) {
        const eng_draw_pkt* p = (const eng_draw_pkt*)q;
        eng_mt_wait_turn(p->seq);
        if (w->prep[i].ok) eng_mt_commit_packet(p, &w->prep[i]);
        eng_mt_take_turn(p->seq);
        q += p->total_bytes;
    }
}

static void sink_end_work(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    const int wp = s_wprof > 0 && s_wprof_pass;
    unsigned long long wp_t0 = wp ? rsx_wprof_now() : 0;
    if (s_wprof > 0) s_wp_draws++;   /* exact even when drains are sampled */
    if (s_wp2 > 0) s_wp2_draw_groups++;
    if (s_no_draw) return;
    if (!g.ready || !dc.n_packets || !dc.fetch_ok) return;
    if (dc.n_immediate && (dc.n_arr || dc.n_idx || dc.inl_bytes ||
                          r->inline_dropped != dc.inline_dropped_at_begin)) return;

    const u32 prim = g.rsx.current_primitive;
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) return;
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) return;
    }

    const int mt = eng_mt_ensure();
    /* Worker draws: no immediate/inline stream (those create their vertices
     * from the FIFO on the state thread) and at least one record worker. */
    const int workerable = mt && s_mt.workers > 0 &&
                           !dc.n_immediate && !dc.inl_bytes;
    eng_draw_pkt* p = NULL;
    if (mt) {
        s_mt.used = (u32)((sizeof(eng_draw_pkt) + 15u) & ~15u);
        if (s_mt.used > s_mt.cap) return;
        p = (eng_draw_pkt*)s_mt.scratch;
        memset(p, 0, sizeof *p);
        p->magic = ENG_MT_PKT_MAGIC;
    }

    rsx_vertex_layout_plan layout;
    { const unsigned long long w2 = wp2_t0();
      eng_vertex_layout(&layout);
      wp2_add(WPROF2_LAYOUT, w2); }
    if (wp) wp_t0 = rsx_wprof_now();
    dc_fetch(&layout, rebuild, workerable);
    if (wp) wprof_phase(RSX_WPROF_FETCH, rsx_wprof_now() - wp_t0);
    if (!dc.fetch_ok) return;
    if (!workerable && !dc.n_verts) return;

    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    const unsigned long long w2_index = wp2_t0();
    if (!workerable && rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) {
            wp2_add(WPROF2_INDEX, w2_index);
            return;
        }
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                   dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    wp2_add(WPROF2_INDEX, w2_index);
    if (!n_draw) return;

    const unsigned long long w2_surface = wp2_t0();
    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    const u32 target = targets[0];

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);
    wp2_add(WPROF2_SURFACE, w2_surface);

    int pipeline_is_fixed = 1;
    u32 tex_used = 0;
    u64 fp_hash = 0;
    if (wp) wp_t0 = rsx_wprof_now();
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, &pipeline_is_fixed, &tex_used,
                                          &fp_hash);
    if (wp) wprof_phase(RSX_WPROF_PIPE, rsx_wprof_now() - wp_t0);
    if (!pipeline) return;
    { const unsigned long long w2 = wp2_t0();
      const int dbg = fp_hash && eng_debug_fp(fp_hash, pipeline);
      wp2_add(WPROF2_PIPE_DEBUG, w2);
      if (dbg) return; }

    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    if (wp) wp_t0 = rsx_wprof_now();
    const unsigned long long w2_texbind = wp2_t0();
    s_miss_fp_hash = fp_hash;
    s_miss_pipeline = pipeline;
    s_fs_draw = eng_fsbind_on() && target < g.n_surfaces &&
                (u64)(sf.clip_w ? sf.clip_w : g.width) *
                    (sf.clip_h ? sf.clip_h : g.height) * 10ull >=
                (u64)g.surfaces[target].w * g.surfaces[target].h * 9ull;
    const u32 tex_mask = sink_bind_textures(targets, n_targets, zslot, tex_used,
                                            textures, samplers);
    s_fs_draw = 0;
    wp2_add(WPROF2_TEX_BIND, w2_texbind);
    if (wp) wprof_phase(RSX_WPROF_TEX, rsx_wprof_now() - wp_t0);
    if (tex_mask == ENG_INVALID) return;
    if (wp) wp_t0 = rsx_wprof_now();
    const unsigned long long w2_vtex = wp2_t0();
    const u32 vtex_mask = sink_bind_vertex_textures(eng_vtex_mask(), vtextures,
                                                    vsamplers);
    wp2_add(WPROF2_VTEX, w2_vtex);
    if (wp) wprof_phase(RSX_WPROF_TEX, rsx_wprof_now() - wp_t0);

    if (indexed) {
        const unsigned long long w2 = wp2_t0();
        if (!dc_reserve_indices(n_draw)) { wp2_add(WPROF2_INDEX, w2); return; }
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
        wp2_add(WPROF2_INDEX, w2);
    }

    const unsigned long long w2_const = wp2_t0();
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    const int vpc_range = g.be->bind_vs_constants_range && !s_vpc.full && !s_vpc_force_full;
    if (!vpc_range) {
        memcpy(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u);
        memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);
    }

    const u32 nslots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 fp_bytes = (nslots + 1u) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants.count)
        memcpy(g.fp_cb, g.fp_constants.values, g.fp_constants.count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      const u32 win = rsx_dsp_reg(&g.rsx, 0x1D88);
      alpha[1] = ((win >> 12) & 0xFu) ? (float)(win & 0xFFFu) : 0.0f;
      alpha[2] = ((win >> 16) & 0xFu) ? -0.5f : 0.0f;
      alpha[3] = 0.0f; }
    wp2_add(WPROF2_CONST, w2_const);

    { const unsigned long long w2 = wp2_t0();
      pd_note(targets, n_targets, zslot, &rs, pipeline, tex_mask);
      wp2_add(WPROF2_SURFTRACK, w2); }

    if (wp) wp_t0 = rsx_wprof_now();
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    const int do_clear_depth =
        zslot != ENG_INVALID && !g.zdepths[zslot].cleared;
    if (do_clear_depth) {
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    u32 sc[4];
    eng_scissor_rect(&sf, g.surfaces[target].w, g.surfaces[target].h, sc);
    u32 stencil_ref = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu;
    int has_bounds = g.be->set_depth_bounds != NULL;
    float bounds[2] = { 0, 0 };
    if (has_bounds) {
        const u32 lo = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN);
        const u32 hi = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
        memcpy(&bounds[0], &lo, sizeof bounds[0]);
        memcpy(&bounds[1], &hi, sizeof bounds[1]);
    }
    int has_bias = g.be->set_depth_bias != NULL;
    float bias[2] = { 0, 0 };
    u32 bias_bits = sf.depth_format == 1u ? 16u : 24u;
    if (has_bias) {
        const u32 en = topology == RSX_TOPOLOGY_POINTS ? M_POLY_OFFSET_POINT
                     : (topology == RSX_TOPOLOGY_LINES || topology == RSX_TOPOLOGY_LINE_STRIP)
                       ? M_POLY_OFFSET_LINE : M_POLY_OFFSET_FILL;
        if (eng_poly_offset_on() && (rsx_dsp_reg(&g.rsx, en) & 1u)) {
            const u32 f = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FACTOR);
            const u32 b = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
            memcpy(&bias[0], &f, sizeof bias[0]);
            memcpy(&bias[1], &b, sizeof bias[1]);
            if (bias[0] != bias[0]) bias[0] = 0.0f;
            if (bias[1] != bias[1]) bias[1] = 0.0f;
        }
    }

    if (mt) {
        /* Fill the packet. Everything the worker reads is either by value in
         * the packet or copied into its arena. */
        p = (eng_draw_pkt*)s_mt.scratch;
        p->prim = prim;
        p->rebuild = (u32)rebuild;
        p->allow_remap = (u32)rebuild;
        p->topology = (u32)topology;
        p->stride = layout.stride;
        if (workerable) {
            /* The worker resolves the remap/plan/index/convert itself. */
            p->n_src = dc.n_source_refs;
            p->plan_init = dc.fetch_plan;
            p->plan_init.guest_ptr = NULL;
            p->plan_init.guest_user = NULL;
            p->off_src = eng_mt_alloc(dc.n_source_refs * (u32)sizeof(rsx_vertex_ref));
            if (!p->off_src) return;   /* arena exhausted: drop, nothing touched */
            memcpy(s_mt.scratch + p->off_src, dc.refs,
                   (size_t)dc.n_source_refs * sizeof(rsx_vertex_ref));
            if (dc.n_cuts) {
                p->n_cuts = dc.n_cuts;
                p->off_cuts = eng_mt_alloc(dc.n_cuts * (u32)sizeof(u32));
                if (!p->off_cuts) return;
                memcpy(s_mt.scratch + p->off_cuts, dc.cuts,
                       (size_t)dc.n_cuts * sizeof(u32));
            }
        } else {
            p->indexed = (u32)indexed;
            p->n_draw = indexed ? n_draw : 0;
            p->uploaded = indexed ? dc.n_verts : n_draw;
            p->deferred = (u32)dc.fetch_deferred;
        }
        p->n_targets = n_targets;
        memcpy(p->track_targets, targets, sizeof p->track_targets);
        for (u32 i = 0; i < n_targets; i++) p->targets[i] = g.surfaces[targets[i]].handle;
        p->depth_handle = depth_handle;
        p->zslot = zslot;
        p->do_clear_depth = (u32)do_clear_depth;
        p->clear_depth_handle = depth_handle;
        p->pipeline = pipeline;
        p->vpc_range = (u32)vpc_range;
        p->vpc_lo = s_vpc.lo;
        p->vpc_n = s_vpc.n;
        p->vpc_total = ENG_VP_CB_BYTES;
        p->fp_bytes = fp_bytes;
        p->tex_mask = tex_mask;
        p->vtex_mask = vtex_mask;
        memcpy(p->textures, textures, sizeof p->textures);
        memcpy(p->samplers, samplers, sizeof p->samplers);
        memcpy(p->vtextures, vtextures, sizeof p->vtextures);
        memcpy(p->vsamplers, vsamplers, sizeof p->vsamplers);
        p->W = W;
        p->H = H;
        memcpy(p->sc, sc, sizeof p->sc);
        p->stencil = stencil_ref;
        p->has_bounds = (u32)has_bounds;
        p->has_bias = (u32)has_bias;
        p->bias_bits = bias_bits;
        memcpy(p->bounds, bounds, sizeof p->bounds);
        memcpy(p->bias, bias, sizeof p->bias);
        p->track_color_mask = rs.color_mask ? 1u : 0u;
        p->track_depth_write = (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) ? 1u : 0u;
        p->track_sf = sf;

        if (vpc_range) {
            /* The backend reads rows[first..first+count) of the block it is
             * handed, so the packet copy keeps the same leading rows. */
            p->rows_bytes = (s_vpc.lo + s_vpc.n) * 16u;
            p->off_rows = eng_mt_alloc(p->rows_bytes);
            if (!p->off_rows) return;
            p = (eng_draw_pkt*)s_mt.scratch;
            memcpy(s_mt.scratch + p->off_rows + s_vpc.lo * 16u,
                   (const u8*)g.rsx.constants + s_vpc.lo * 16u,
                   s_vpc.n * 16u);
            p->tail_bytes = (u32)sizeof xf;
            p->off_tail = eng_mt_alloc(p->tail_bytes);
            if (!p->off_tail) return;
            p = (eng_draw_pkt*)s_mt.scratch;
            memcpy(s_mt.scratch + p->off_tail, xf, sizeof xf);
        } else {
            p->vp_full_bytes = ENG_VP_CB_BYTES;
            p->off_vp_full = eng_mt_alloc(p->vp_full_bytes);
            if (!p->off_vp_full) return;
            p = (eng_draw_pkt*)s_mt.scratch;
            memcpy(s_mt.scratch + p->off_vp_full, g.vp_cb, p->vp_full_bytes);
        }
        p->off_fp = eng_mt_alloc(p->fp_bytes);
        if (!p->off_fp) return;
        p = (eng_draw_pkt*)s_mt.scratch;
        memcpy(s_mt.scratch + p->off_fp, g.fp_cb, p->fp_bytes);
        p->total_bytes = s_mt.used;

        if (workerable) {
            /* The finalize point for a worker packet: no early return may
             * follow, or the ordered turn would never advance. */
            p->seq = ++s_mt.seq;
            if (!eng_mt_chunk_add(p, p->total_bytes)) {
                eng_mt_chunk_close();
                if (!eng_mt_chunk_add(p, p->total_bytes)) {
                    eng_mt_run_packet_inline(p);
                    goto mt_done;
                }
            }
            eng_pkt_track(p);
            if (!pipeline_is_fixed) g.guest_draws++;
            wp2_add(WPROF2_BIND_BE, w2_const);
            if (wp) wprof_phase(RSX_WPROF_DRAW, rsx_wprof_now() - wp_t0);
            return;
        }

        /* State-thread packet path (workers==0, or a draw with an immediate /
         * inline stream): drain every earlier packet, then commit this one in
         * order and take its turn so later worker packets can proceed. */
        if (s_mt.workers > 0) eng_mt_sync();
        p->seq = ++s_mt.seq;
        eng_pkt_commit_live(p);
        eng_mt_take_turn(p->seq);
        p = (eng_draw_pkt*)s_mt.scratch;
mt_done:
        if (p) {
            eng_pkt_track(p);
            if (!pipeline_is_fixed) g.guest_draws++;
        }
        wp2_add(WPROF2_BIND_BE, w2_const);
        if (wp) wprof_phase(RSX_WPROF_DRAW, rsx_wprof_now() - wp_t0);
        return;
    }

    /* The old path, unchanged when the key is off. */
    const unsigned long long w2_bind = wp2_t0();
    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    if (vpc_range)
        g.be->bind_vs_constants_range(g.be->user, g.rsx.constants, s_vpc.lo, s_vpc.n,
                                      xf, sizeof xf, ENG_VP_CB_BYTES);
    else
        g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);
    g.be->set_scissor(g.be->user, sc[0], sc[1], sc[2], sc[3]);
    g.be->set_stencil_ref(g.be->user, stencil_ref);
    if (g.be->set_depth_bounds) {
        const u32 lo = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN);
        const u32 hi = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
        float fmin, fmax;
        memcpy(&fmin, &lo, sizeof fmin);
        memcpy(&fmax, &hi, sizeof fmax);
        g.be->set_depth_bounds(g.be->user, fmin, fmax);
    }
    if (g.be->set_depth_bias) {
        float slope = 0.0f, units = 0.0f;
        const u32 en = topology == RSX_TOPOLOGY_POINTS ? M_POLY_OFFSET_POINT
                     : (topology == RSX_TOPOLOGY_LINES || topology == RSX_TOPOLOGY_LINE_STRIP)
                       ? M_POLY_OFFSET_LINE : M_POLY_OFFSET_FILL;
        if (eng_poly_offset_on() && (rsx_dsp_reg(&g.rsx, en) & 1u)) {
            const u32 f = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FACTOR);
            const u32 b = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
            memcpy(&slope, &f, sizeof slope);
            memcpy(&units, &b, sizeof units);
            if (slope != slope) slope = 0.0f;
            if (units != units) units = 0.0f;
        }
        g.be->set_depth_bias(g.be->user, slope, units,
                             sf.depth_format == 1u ? 16u : 24u);
    }
    wp2_add(WPROF2_BIND_BE, w2_bind);
    if (wp) wprof_phase(RSX_WPROF_BIND, rsx_wprof_now() - wp_t0);

    if (wp) wp_t0 = rsx_wprof_now();
    u32 uploaded = indexed ? dc.n_verts : n_draw;
    const unsigned long long w2_draw = wp2_t0();
    if (dc.fetch_deferred) {
        s_fill_ok = -1;
        rsx_vtx_gpu_desc gpu_desc;
        { const unsigned long long w2 = wp2_t0();
          eng_check_gpu_vertex_offload(&gpu_desc, uploaded);
          wp2_add(WPROF2_VTXOFF, w2); }
        const int drew = g.be->draw_fill(g.be->user, topology, dc_fill_verts, &gpu_desc,
                            uploaded, layout.stride, indexed ? g.indices : NULL,
                            indexed ? n_draw : 0);
        wp2_add(WPROF2_DRAW_BE, w2_draw);
        if (drew) {
            if (s_wp2 > 0 && s_fill_ok > 0) { s_wp2_draws++; s_wp2_verts += uploaded; }
            if (!s_fill_ok) return;
            static u32 logs = 0;
            if (s_fill_ok > 0 && PS3_LOG_FIRST(logs, 1))
                fprintf(stderr, "[rsx engine] vertices converted straight into the "
                                "backend's vertex memory (PS3RECOMP_ENG_VTX_DIRECT=0 copies)\n");
        } else {
            unsigned long long conv_t0 = wp ? rsx_wprof_now() : 0;
            if (!dc_convert_verts()) { wp2_add(WPROF2_DRAW_BE, w2_draw); return; }
            if (wp) wprof_phase(RSX_WPROF_FETCH, rsx_wprof_now() - conv_t0);
            g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
                       indexed ? g.indices : NULL, indexed ? n_draw : 0);
            wp2_add(WPROF2_DRAW_BE, w2_draw);
            if (s_wp2 > 0) { s_wp2_draws++; s_wp2_verts += uploaded; }
        }
    } else {
        g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
                   indexed ? g.indices : NULL, indexed ? n_draw : 0);
        wp2_add(WPROF2_DRAW_BE, w2_draw);
        if (s_wp2 > 0) { s_wp2_draws++; s_wp2_verts += uploaded; }
    }
    if (wp) wprof_phase(RSX_WPROF_DRAW, rsx_wprof_now() - wp_t0);
    if (!pipeline_is_fixed) g.guest_draws++;

    const unsigned long long w2_surftrack = wp2_t0();
    if (rs.color_mask) {
        eng_surfaces_wrote(targets, n_targets);
        for (u32 i = 0; i < n_targets; i++) {
            g.surfaces[targets[i]].last_writer_fp = s_miss_fp_hash;
            g.surfaces[targets[i]].last_writer_pipe = s_miss_pipeline;
        }
    }
    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        g.zdepths[zslot].had_write = 1;
        eng_zdepth_wrote(zslot, &sf);
    }
    wp2_add(WPROF2_SURFTRACK, w2_surftrack);
}

/* Draw source: a group whose active vertex arrays were resolved into one of
 * the five 2 MB mesh rings is a draw the SPU job filled (the 0x90-byte slots
 * in the command ring point here); everything else -- the PPU-built ring,
 * inline/immediate geometry -- is the main source. */
static int dc_source_is_mesh(void)
{
    if (dc.n_immediate || dc.inl || dc.fetch_plan.inline_data) return 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        const u8* b = dc.fetch_plan.attr[a].stable_base;
        if (!b) continue;
        const uintptr_t ea = (uintptr_t)b - (uintptr_t)vm_base;
        if (ea >= ENG_MESH_EA_LO && ea < ENG_MESH_EA_HI) return 1;
    }
    return 0;
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    if (s_wprof <= 0) { sink_end_work(user, r); return; }
    const int timed = s_wprof_pass;
    const unsigned long long t0 = timed ? rsx_wprof_now() : 0;
    sink_end_work(user, r);
    if (s_no_draw) return;   /* no plan was prepared; the previous one is stale */
    const int mesh = dc_source_is_mesh();
    if (timed) {
        const unsigned long long ns = rsx_wprof_now() - t0;
        if (mesh) s_wp_mesh_ns += ns;
        else s_wp_main_ns += ns;
    }
    if (mesh) s_wp_mesh_draws += dc.n_packets;
    else s_wp_main_draws += dc.n_packets;
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    eng_mt_sync();
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    if (pd_capturing()) {
        rsx_dsp_surface zs;
        rsx_dsp_get_surface(&g.rsx, &zs);
        pd_note(targets, n_targets,
                eng_zdepth_get(zs.zeta_location, zs.zeta_offset, zs.clip_w, zs.clip_h),
                NULL, 0, 0);
    }

    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. Each only where the
         * clip and the scissor let the clear through (eng_clear_rect). */
        int wrote = 0;
        for (u32 i = 0; i < n_targets; i++) {
            const eng_surface* s = &g.surfaces[targets[i]];
            u32 rc[4];
            const int part = eng_clear_rect(&sf, s->w, s->h, rc);
            if (!part) continue;
            if (part == 2 && g.be->clear_color_rect) {
                eng_log_rect_clear("colour", rc, s->w, s->h);
                g.be->clear_color_rect(g.be->user, s->handle, rgba,
                                       rc[0], rc[1], rc[2], rc[3]);
            } else
                g.be->clear_color(g.be->user, s->handle, rgba);
            wrote = 1;
        }
        if (wrote) eng_surfaces_wrote(targets, n_targets);
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w, sf.clip_h);
        if (zslot == ENG_INVALID) return;
        /* A depth target is at least the canvas (eng_zdepth_get), so a
         * shadow map's clip is smaller than it all the same; a clear that
         * covers everything the zeta was ever bound with is a whole clear,
         * which the backend folds into the next pass. */
        const eng_zdepth* z = &g.zdepths[zslot];
        u32 rc[4];
        const int part = eng_clear_rect(&sf, z->w, z->h, rc);
        if (!part) return;
        const int partial = part == 2 && g.be->clear_depth_stencil_rect &&
                            !(rc[0] == 0 && rc[1] == 0 &&
                              rc[2] >= z->ext_w && rc[3] >= z->ext_h);
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0, and the
         * FIFO walker writes it again at every flip, as the RSX does. A 0 is
         * the near plane: a title that sets 0 and never writes the register
         * again gets the far plane back from the next flip, not from here. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const int z16 = sf.depth_format == 1u;
        const float depth = z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                : (float)(zs >> 8) / 16777215.0f;
        const u32 handle = g.zdepths[zslot].handle;
        if (partial) {
            eng_log_rect_clear("depth", rc, z->w, z->h);
            /* The rest of a target nothing initialised yet starts as the
             * first bind would leave it (sink_end). */
            if (!g.zdepths[zslot].cleared)
                g.be->clear_depth_stencil(g.be->user, handle,
                                          RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                          1.0f, 0);
            g.be->clear_depth_stencil_rect(g.be->user, handle, flags, depth,
                                           z16 ? 0 : (u8)(zs & 0xFFu),
                                           rc[0], rc[1], rc[2], rc[3]);
        } else {
            g.be->clear_depth_stencil(g.be->user, handle, flags,
                                      depth, z16 ? 0 : (u8)(zs & 0xFFu));
        }
        g.zdepths[zslot].cleared = 1;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once.
             * Past a partial clear the rest still holds what was drawn. */
            if (!partial) g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0;
            eng_zdepth_wrote(zslot, &sf);
        }
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == d->location &&
                g.surfaces[i].offset == d->offset)
                return i;
    }
    return eng_current_surface();
}

static int (*s_is_display_fn)(u32 offset) = NULL;

void rsx_draw_engine_set_display_query(int (*fn)(u32 offset))
{
    s_is_display_fn = fn;
}

static int eng_is_display_offset(u32 location, u32 offset)
{
    if (s_is_display_fn && s_is_display_fn(offset)) return 1;
    for (int i = 0; i < 8; i++) {
        if (g.display_buffers[i].valid &&
            g.display_buffers[i].location == location &&
            g.display_buffers[i].offset == offset)
            return 1;
    }
    return 0;
}

void rsx_draw_engine_flush_dirty_to_guest(void)
{
    eng_mt_sync();
    if (!g.ready || !g.be) return;
    const int mode = rsx_draw_engine_wcb_mode();
    if (!mode) return;
    if (mode >= RSX_WCB_ONDEMAND) { eng_wcb_sync_point(mode); return; }

    /* sync: every dirty target, now, each behind a full GPU wait. */
    if (g.be->readback) {
        for (u32 i = 0; i < g.n_surfaces; i++) {
            eng_surface* s = &g.surfaces[i];
            if (s->guest_dirty) {
                if (eng_is_display_offset(s->location, s->offset))
                    continue;
                if (eng_surface_to_guest(i) && !s->guest_dirty) WCB_COUNT(sync_copies, 1);
            }
        }
    }
    if (g.be->depth_readback) {
        for (u32 i = 0; i < g.n_zdepths; i++) {
            if (g.zdepths[i].guest_dirty) {
                if (eng_zdepth_to_guest(i) && !g.zdepths[i].guest_dirty) WCB_COUNT(sync_copies, 1);
            }
        }
    }
}

/* PS3RECOMP_PRESENT_TRACE=1 (default off): one line per presented guest
 * frame with the source surface's last colour write serial. A present whose
 * source was last written before the previous present's source is showing an
 * image older than one already shown -- the backward-frame symptom. Cheap:
 * one compare and, when on, one fprintf per present. */
static u64 s_ptrace_prev_wseq, s_ptrace_back, s_ptrace_presents;

static void eng_present_trace(u32 buffer_id, u32 target)
{
    if (s_ptrace < 0) {
        const char* e = getenv("PS3RECOMP_PRESENT_TRACE");
        s_ptrace = e && *e && strcmp(e, "0") != 0;
    }
    if (!s_ptrace) return;
    const u64 ws = (target != ENG_INVALID && target < g.n_surfaces)
                 ? g.surfaces[target].last_write_serial : 0ull;
    const u32 off = (target != ENG_INVALID && target < g.n_surfaces)
                  ? g.surfaces[target].offset : 0u;
    const int back = (s_ptrace_prev_wseq && ws && ws < s_ptrace_prev_wseq);
    if (back) s_ptrace_back++;
    fprintf(stderr, "[ptrace] n=%llu flip=%u buf=%u off=0x%08X slot=%d wseq=%llu prev=%llu back=%d frames=%u draws=%u\n",
            (unsigned long long)s_ptrace_presents,
            cellGcm_flip_frame_count ? cellGcm_flip_frame_count() : 0u,
            buffer_id, off, (int)target, (unsigned long long)ws,
            (unsigned long long)s_ptrace_prev_wseq, back, g.frames, g.guest_draws);
    if (ws) s_ptrace_prev_wseq = ws;
    if (++s_ptrace_presents % 300ull == 0ull)
        fprintf(stderr, "[ptrace] summary: %llu presents, %llu stale-source (backward)\n",
                (unsigned long long)s_ptrace_presents, (unsigned long long)s_ptrace_back);
}

static void eng_present(u32 buffer_id)
{
    eng_mt_sync();
    if (!g.ready) return;
    rsx_draw_engine_flush_dirty_to_guest();
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;
    eng_present_trace(buffer_id, target);
    g.surfaces[target].last_use = g.frames;
    if (getenv("PS3RECOMP_METAL_FRAME_DUMP") && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    pd_present(target);
    g.last_present_surface = target;
    g.be->present(g.be->user, g.surfaces[target].handle);
    g.frames++;
    rsx_tex_miss_frame(stderr, g.frames);
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
    eng_wcb_frame_tick();
    eng_wcb_attr_flush();
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_replay_advance_frame(void)
{
    if (!g.ready) return;
    g.frames++;
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    eng_mt_reset();
    /* Re-read PS3RECOMP_TEX_DIRTY at each engine up (the fixtures change the
     * environment between engine sessions in one process). */
    s_texdirty_env = -1;
    s_texdirty_paranoid = 0;
    s_texdirty_live = 0;
    (void)rsx_wprof_enabled();
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    { const char* e = getenv("PS3RECOMP_ENG_TEXHASH_FULL");
      s_texhash_full_always = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_ENG_DEPTH_VIA_GUEST");
      s_depth_via_guest = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_ENG_TEXWGEN");
      s_tex_wgen_on = !(e && strcmp(e, "0") == 0); }
    memset(s_tdec, 0, sizeof s_tdec);    /* the dispatcher's counts restart */
    { const char* e = getenv("PS3RECOMP_ENG_VPC_FULL");
      s_vpc_force_full = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_ENG_TEXMEMO");
      s_texmemo_on = !(e && strcmp(e, "0") == 0); }
    { const char* e = getenv("PS3RECOMP_ENG_VTX_DIRECT");
      s_vtx_direct = !(e && strcmp(e, "0") == 0); }
    { const char* e = getenv("PS3RECOMP_ENG_VTX_REMAP");
      s_vtx_remap = !(e && strcmp(e, "0") == 0); }
    { const char* e = getenv("PS3RECOMP_WALKER_FAST");
      s_walker_fast = e && *e && strcmp(e, "0") != 0; }
    /* The umbrella sets both halves, but an explicit per-item value wins so
     * the two can be measured on their own (or with WALKER_FAST=0). */
    s_fetch_memo = s_walker_fast;
    s_pipe_memo = s_walker_fast;
    { const char* e = getenv("PS3RECOMP_FETCH_MEMO");
      if (e && *e) s_fetch_memo = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_PIPE_MEMO");
      if (e && *e) s_pipe_memo = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_REMAP_CACHE");
      s_remap_cache = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_MEMO_INDEX");
      s_memo_index = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_REFS_HASH_FAST");
      s_refs_hash_fast = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("EXP_NO_DRAW");
      s_no_draw = e && *e && strcmp(e, "0") != 0; }
    eng_fast_reset_caches();
    memset(s_tmemo, 0, sizeof s_tmemo);
    s_tex_gen++;
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.immediate_vertex = sink_immediate_vertex;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    g.ready = 1;
    memset(&s_wcb.st, 0, sizeof s_wcb.st);
    memset(&s_wcb.win, 0, sizeof s_wcb.win);
    s_wcb.win_frames = 0;
    if (rsx_draw_engine_wcb_mode() >= RSX_WCB_ONDEMAND) eng_wcb_module_up();
    /* Display buffers the title registered before the backend came up. */
    cellGcm_replay_display_buffers_to_engine();
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    eng_mt_sync();
    if (g.ready && s_wcb_live) {
        /* Armed targets go to guest memory and their pages reopen: the guest
         * may outlive the engine (a backend switch, a window close). */
        for (u32 i = 0; i < g.n_surfaces; i++) eng_wcb_retire_surface(i);
        for (u32 i = 0; i < g.n_zdepths; i++) eng_wcb_retire_zdepth(i);
        rsx_wcb_shutdown();
        s_wcb_live = 0;
    }
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].feedback) g.be->texture_release(g.be->user, g.surfaces[i].feedback);
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].conv) g.be->texture_release(g.be->user, g.zdepths[i].conv);
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    }
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    eng_mt_reset();
    rsx_texdirty_shutdown();
    s_texdirty_live = 0;
    rsx_tex_miss_flush(stderr);
    rsx_draw_engine_set_guest_memory(NULL, NULL);
    eng_fast_free_caches();
    free(dc.refs); free(dc.cuts); free(dc.verts); free(dc.immediate);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    /* A guest thread waiting on an armed target's bytes. */
    if (s_wcb_live && rsx_wcb_requests_pending()) rsx_draw_engine_wcb_service();
    rsx_dispatch_method(&g.rsx, m, arg);
}

void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count)
{
    if (!g.ready) return;
    if (s_wcb_live && rsx_wcb_requests_pending()) rsx_draw_engine_wcb_service();
    rsx_dispatch_method_run(&g.rsx, method, step, src, count);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    eng_mt_sync();
    rsx_draw_engine_flush_dirty_to_guest();
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    static u32 s_eng_pres_cnt = 0;
    if (++s_eng_pres_cnt % 120 == 0) {
        fprintf(stderr, "[vtx offload summary] calls=%llu m0=%llu m1=%llu m2=%llu m3=%llu remap=%llu not_def=%llu not_contig=%llu other=%llu\n",
                (unsigned long long)s_eng_calls,
                (unsigned long long)s_eng_eligible_m0,
                (unsigned long long)s_eng_eligible_m1,
                (unsigned long long)s_eng_eligible_m2,
                (unsigned long long)s_eng_eligible_m3,
                (unsigned long long)s_eng_eligible_remap,
                (unsigned long long)s_eng_not_deferred,
                (unsigned long long)s_eng_not_contiguous,
                (unsigned long long)s_eng_other_layout);
    }
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_note_flip(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

int rsx_draw_engine_walker_fast_enabled(void)
{
    return s_walker_fast;
}

void rsx_draw_engine_walker_fast_stats(rsx_walker_fast_stats* out)
{
    if (!out) return;
    out->fetch_hits = s_fetch_hits;
    out->fetch_misses = s_fetch_misses;
    out->pipeline_hits = s_pipe_hits;
    out->pipeline_misses = s_pipe_misses;
    out->remap_hits = s_remap_hits;
    out->remap_misses = s_remap_misses;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
