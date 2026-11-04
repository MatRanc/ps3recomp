/*
 * Offline replay of a PS3RECOMP_WALKER_CAPTURE frame through the real
 * register-file draw engine (libs/video/rsx_draw_engine.c) with a null
 * backend. The walker, dispatcher, vertex fetch/remap, texture decode/hash,
 * pipeline key and bind traffic are all the shipping code path; only Vulkan
 * command recording is absent.
 */
#include "replay_rsx_engine_core.h"
#include "replay_null_draw_backend.h"
#include "rsx_draw_engine.h"

#include "../libs/video/cellGcmSys.c"
#include "../../runtime/memory/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- VM / runtime stubs for standalone execution ------------------------ */
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
int g_spu_ls_watch_n = -1;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (method == 0x50u) g_rsx_last_reference = data;
    return 0;
}
int rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }

void rsx_live_draw_set_display_buffer(u32 id, u32 loc, u32 off, u32 w, u32 h, u32 p)
{ (void)id; (void)loc; (void)off; (void)w; (void)h; (void)p; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d)
{ (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f,
                                u32 g, u32 h, u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
  (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

#ifndef _WIN32
HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, const char* name)
{ (void)sa; (void)manual; (void)initial; (void)name; return (HANDLE)(uintptr_t)1; }
BOOL SetEvent(HANDLE h) { (void)h; return 1; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
#endif

static uint32_t s_replay_heap = 0x00800000u;
static u32 replay_host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    s_replay_heap = (s_replay_heap + align - 1u) & ~(align - 1u);
    u32 r = s_replay_heap;
    s_replay_heap += size;
    return r;
}
static void replay_host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

/* ---- captured guest memory --------------------------------------------- */

typedef struct { u32 location, page, len, data_off; } mem_ent;

struct rsx_engine_replay {
    u32 io_addr, io_size, stream_start, stream_end;
    u8* fifo;
    size_t fifo_len;
    rsx_wcap_state state;
    u8* mem;
    size_t mem_len;
    mem_ent* ents;
    size_t nents;
    unsigned long long hits, misses;
    u32 words;
};

static const u8* cap_read(void* user, u32 location, u32 offset, u32 min_bytes)
{
    struct rsx_engine_replay* r = (struct rsx_engine_replay*)user;
    if (!min_bytes) return NULL;
    const u32 page = offset & ~4095u;
    const u32 in_page = offset & 4095u;
    /* First entry at or after (location, page). */
    size_t lo = 0, hi = r->nents;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const mem_ent* e = &r->ents[mid];
        if (e->location < location || (e->location == location && e->page < page))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= r->nents) { r->misses++; return NULL; }
    const mem_ent* e = &r->ents[lo];
    if (e->location != location || e->page != page) { r->misses++; return NULL; }
    const size_t need = (size_t)in_page + min_bytes;
    const size_t pages = (need + 4095u) / 4096u;
    if (lo + pages > r->nents) { r->misses++; return NULL; }
    for (size_t k = 1; k < pages; k++) {
        const mem_ent* n = &r->ents[lo + k];
        if (n->location != location || n->page != page + (u32)k * 4096u ||
            n->data_off != e->data_off + (u32)k * 4096u) {
            r->misses++;
            return NULL;
        }
    }
    r->hits++;
    return r->mem + e->data_off + in_page;
}

/* ---- file loading ------------------------------------------------------ */

static void set_err(char* err, size_t n, const char* msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

static u8* read_file(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    u8* p = (u8*)malloc((size_t)n ? (size_t)n : 1);
    if (!p) { fclose(f); return NULL; }
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return p;
}

rsx_engine_replay* rsx_engine_replay_create(const char* prefix, char* err, size_t errlen)
{
    struct rsx_engine_replay* r = (struct rsx_engine_replay*)calloc(1, sizeof *r);
    if (!r) { set_err(err, errlen, "out of memory"); return NULL; }

    char path[600];
    snprintf(path, sizeof path, "%s.fifo", prefix);
    size_t fifo_file_len = 0;
    u8* fifo_file = read_file(path, &fifo_file_len);
    if (!fifo_file || fifo_file_len < 32) { set_err(err, errlen, "cannot read .fifo"); goto fail; }
    {
        u32 hdr[8];
        memcpy(hdr, fifo_file, sizeof hdr);
        if (hdr[0] != 0x46585352u) { set_err(err, errlen, ".fifo magic"); free(fifo_file); goto fail; }
        r->io_addr = hdr[2] ? hdr[2] : 0x00100000u;
        r->io_size = hdr[3] ? hdr[3] : 0x00400000u;
        r->fifo_len = fifo_file_len - 32u;
        r->fifo = (u8*)malloc(r->fifo_len ? r->fifo_len : 1);
        if (!r->fifo) { set_err(err, errlen, "out of memory"); free(fifo_file); goto fail; }
        memcpy(r->fifo, fifo_file + 32u, r->fifo_len);
        r->words = (u32)(r->fifo_len / 4u);
        free(fifo_file);
    }

    snprintf(path, sizeof path, "%s.state", prefix);
    {
        size_t n = 0;
        u8* p = read_file(path, &n);
        if (!p || n != sizeof(rsx_wcap_state)) {
            free(p); set_err(err, errlen, "cannot read .state"); goto fail;
        }
        memcpy(&r->state, p, sizeof r->state);
        free(p);
        if (r->state.magic != RSX_WCAP_MAGIC) { set_err(err, errlen, ".state magic"); goto fail; }
    }

    snprintf(path, sizeof path, "%s.mem", prefix);
    {
        size_t n = 0;
        u8* p = read_file(path, &n);
        if (!p || n < 24) { free(p); set_err(err, errlen, "cannot read .mem"); goto fail; }
        u32 hdr[6];
        memcpy(hdr, p, sizeof hdr);
        if (hdr[0] != 0x50414357u || hdr[3] != 4096u) {
            free(p); set_err(err, errlen, ".mem magic"); goto fail;
        }
        r->nents = hdr[2];
        r->mem_len = hdr[4];
        const size_t ent_bytes = r->nents * sizeof(mem_ent);
        if (24 + ent_bytes + r->mem_len > n) {
            free(p); set_err(err, errlen, ".mem truncated"); goto fail;
        }
        r->ents = (mem_ent*)malloc(ent_bytes ? ent_bytes : 1);
        r->mem = (u8*)malloc(r->mem_len ? r->mem_len : 1);
        if (!r->ents || !r->mem) { free(p); set_err(err, errlen, "out of memory"); goto fail; }
        memcpy(r->ents, p + 24, ent_bytes);
        memcpy(r->mem, p + 24 + ent_bytes, r->mem_len);
        free(p);
    }

    r->stream_start = 0x00080000u;
    r->stream_end = r->stream_start + (u32)r->fifo_len;

    /* The real VM is 4 GB and the title may put an IO mapping or a local
     * surface anywhere in it. A 4 GB-aligned window with every page mprotected
     * read/write is virtual-only until touched, so this costs nothing until a
     * 2D transfer writes there. */
    if (vm_init() != CELL_OK) { set_err(err, errlen, "guest VM"); goto fail; }
    ppu_vm_size = 0;
    for (u64 a = 0; a < 0x100000000ull; a += 0x40000000ull) {
        if (vm_protect((u32)a, 0x40000000u, 1, 1, 0) != CELL_OK) {
            set_err(err, errlen, "guest VM protect"); goto fail;
        }
    }

    u32 ctx_out = replay_host_alloc(4, 4);
    if (!cellGcmSetupContext(ctx_out, 0x00080000u, r->io_size, r->io_addr,
                             replay_host_alloc, replay_host_w32)) {
        set_err(err, errlen, "cellGcmSetupContext"); goto fail;
    }

    rsx_draw_engine_set_guest_memory(cap_read, r);
    rsx_draw_engine_set_backend(replay_null_backend());
    rsx_draw_engine_set_default(1);
    if (rsx_draw_engine_init(r->state.width ? r->state.width : 1280,
                             r->state.height ? r->state.height : 720) != 0) {
        set_err(err, errlen, "rsx_draw_engine_init"); goto fail;
    }
    rsx_draw_engine_state_restore(&r->state);

    {
        const u32 ea = gcm_io2ea(r->stream_start);
        if (!ea) { set_err(err, errlen, "stream IO unmapped"); goto fail; }
        memcpy(vm_base + ea, r->fifo, r->fifo_len);
    }
    fprintf(stderr, "[replay-engine] %s: %u words, %llu pages, %.1f MB guest image\n",
            prefix, r->words, (unsigned long long)r->nents,
            (double)r->mem_len / (1024.0 * 1024.0));
    return r;

fail:
    rsx_engine_replay_destroy(r);
    return NULL;
}

int rsx_engine_replay_run_frame(rsx_engine_replay* r, rsx_engine_replay_stats* out)
{
    if (!r) return -1;
    rsx_draw_engine_mt_sync();
    rsx_draw_engine_state_restore(&r->state);
    replay_null_reset();
    r->hits = r->misses = 0;

    s_fifo_getoff = r->stream_start;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, r->stream_end);
    s_flip_request_count = 0;
    s_fifo_flips_reached = 0;
    s_fifo_flips_queued = 0;
    s_ref_qhead = 0;
    s_ref_qtail = 0;
    s_fifo_back_jumps = 0;

    for (int pass = 0; s_fifo_getoff != r->stream_end && pass < 256; pass++)
        cellGcm_rsx_process_fifo();

    while (s_ref_qhead != s_ref_qtail) s_ref_qhead++;

    rsx_draw_engine_mt_sync();
    rsx_draw_engine_replay_advance_frame();
    if (out) {
        out->draws = replay_null_draws();
        out->vertices = replay_null_draw_vertices();
        out->read_hits = r->hits;
        out->read_misses = r->misses;
        out->words = r->words;
        out->frame_hash = replay_null_hash();
    }
    return (s_fifo_getoff == r->stream_end) ? 0 : -2;
}

void rsx_engine_replay_destroy(rsx_engine_replay* r)
{
    if (!r) return;
    if (rsx_draw_engine_enabled()) rsx_draw_engine_shutdown();
    free(r->fifo);
    free(r->mem);
    free(r->ents);
    free(r);
    vm_shutdown();
    ppu_vm_size = 0;
}
