#include "replay_null_draw_backend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- output gate --------------------------------------------------------
 *
 * A 64-bit digest of everything this backend is asked to do in one frame:
 * resource creation (by the semantic arguments, not the fake handle), every
 * binding, every constant block, every draw's parameters and vertex/index
 * bytes, and every piece of dynamic state. Handles are mapped to a digest of
 * the request that created them, so the gate is independent of allocation
 * order and can compare a frame replayed with different cache settings.
 *
 * replay_null_reset() starts a new frame. The digest of the same captured
 * frame is bit-identical on every repeat and with PS3RECOMP_WALKER_FAST on
 * or off; replay_rsx_engine reports it.
 */
#define RG_BASIS 1469598103934665603ull

static u64 s_hash = RG_BASIS;
static u64* s_digest;
static size_t s_digest_cap;
static u32 s_next_handle = 1;

static inline void rg_bytes(const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    u64 h = s_hash;
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        u64 w;
        memcpy(&w, b + i, sizeof w);
        h ^= w;
        h *= 1099511628211ull;
    }
    for (; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    s_hash = h;
}
static inline void rg_u64(u64 v) { rg_bytes(&v, sizeof v); }
static inline void rg_u32(u32 v) { rg_bytes(&v, sizeof v); }
static inline void rg_tag(const char* tag)
{
    rg_bytes(tag, strlen(tag) + 1u);
}

static int rg_ensure(u32 h)
{
    if (h < s_digest_cap) return 1;
    size_t cap = s_digest_cap ? s_digest_cap : 256u;
    while (cap <= h) cap *= 2u;
    u64* p = (u64*)realloc(s_digest, cap * sizeof(*p));
    if (!p) return 0;
    memset(p + s_digest_cap, 0, (cap - s_digest_cap) * sizeof(*p));
    s_digest = p;
    s_digest_cap = cap;
    return 1;
}

static u64 rg_digest_of(u32 handle)
{
    return handle < s_digest_cap ? s_digest[handle] : 0ull;
}

/* Give the next fake handle the digest of the request that created it. */
static u32 rg_new_handle(u64 digest)
{
    const u32 h = s_next_handle++;
    if (rg_ensure(h)) s_digest[h] = digest;
    return h;
}

/* Finish a creation: `save` is the frame digest from before the request was
 * hashed into a standalone digest `d`. Put the request in the frame stream
 * and give the new handle that identity. */
static u32 rg_commit(u64 save, const char* tag, u64 d)
{
    s_hash = save;
    rg_tag(tag);
    rg_u64(d);
    return rg_new_handle(d);
}

/* A handle for an operation that derived a resource from another (a surface
 * view, a depth snapshot): its identity is the request, not the counter. */
static u32 rg_new_derived(u32 source, const char* tag, u32 a, u32 b, u32 c)
{
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag(tag);
    rg_u64(rg_digest_of(source));
    rg_u32(a); rg_u32(b); rg_u32(c);
    const u64 d = s_hash;
    return rg_commit(save, tag, d);
}

/* ---- state -------------------------------------------------------------- */

static unsigned long long s_draws, s_vertices;
static unsigned char* s_fill;
static size_t s_fill_cap;

static int nb_init(void* u, u32 w, u32 h)
{
    (void)u;
    rg_tag("init"); rg_u32(w); rg_u32(h);
    return 0;
}
static void nb_shutdown(void* u)
{
    (void)u;
    free(s_fill); s_fill = NULL; s_fill_cap = 0;
    free(s_digest); s_digest = NULL; s_digest_cap = 0;
}
static void nb_submit(void* u, u32 reason)
{
    (void)u;
    rg_tag("submit"); rg_u32(reason);
}

static u32 nb_tex_create(void* u, rsx_be_format f, u32 w, u32 h, u32 m,
                         u32 faces, u32 remap, u32 fmt)
{
    (void)u;
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag("texture_create");
    rg_u32((u32)f); rg_u32(w); rg_u32(h); rg_u32(m); rg_u32(faces);
    rg_u32(remap); rg_u32(fmt);
    const u64 d = s_hash;
    s_hash = save;
    return rg_commit(save, "texture_create", d);
}
static void nb_tex_upload(void* u, u32 t, u32 f, u32 m, u32 w, u32 h,
                          const void* p, u32 rb, u32 rows)
{
    (void)u;
    rg_tag("texture_upload"); rg_u64(rg_digest_of(t));
    rg_u32(f); rg_u32(m); rg_u32(w); rg_u32(h); rg_u32(rb); rg_u32(rows);
    if (p && rb && rows) rg_bytes(p, (size_t)rb * rows);
}
static void nb_tex_release(void* u, u32 t)
{
    (void)u;
    rg_tag("texture_release"); rg_u64(rg_digest_of(t));
}

static u32 nb_ct_create(void* u, rsx_be_format f, u32 w, u32 h,
                        const void* seed, u32 seed_rb)
{
    (void)u;
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag("color_target_create");
    rg_u32((u32)f); rg_u32(w); rg_u32(h);
    if (seed && seed_rb && h) rg_bytes(seed, (size_t)seed_rb * h);
    const u64 d = s_hash;
    s_hash = save;
    return rg_commit(save, "color_target_create", d);
}
static void nb_ct_release(void* u, u32 s)
{
    (void)u;
    rg_tag("color_target_release"); rg_u64(rg_digest_of(s));
}
static void nb_ct_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                        const void* rows, u32 rb)
{
    (void)u;
    rg_tag("color_target_write"); rg_u64(rg_digest_of(s));
    rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h); rg_u32(rb);
    if (rows && rb && h) rg_bytes(rows, (size_t)rb * h);
}
static u32 nb_surface_view(void* u, u32 s, u32 remap, u32 fmt)
{
    (void)u; (void)remap; (void)fmt;
    return rg_new_derived(s, "surface_view", remap, fmt, 0);
}

static u32 nb_dt_create(void* u, u32 w, u32 h)
{
    (void)u;
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag("depth_target_create"); rg_u32(w); rg_u32(h);
    const u64 d = s_hash;
    s_hash = save;
    return rg_commit(save, "depth_target_create", d);
}
static void nb_dt_release(void* u, u32 d)
{
    (void)u;
    rg_tag("depth_target_release"); rg_u64(rg_digest_of(d));
}
static u32 nb_depth_snap(void* u, u32 d, u32 w, u32 h)
{
    (void)u;
    return rg_new_derived(d, "depth_snapshot", w, h, 0);
}
static u32 nb_depth_as_color(void* u, u32 d, u32 w, u32 h, u32 fmt)
{
    (void)u;
    /* This backend cannot convert depth: record the request, return none. */
    rg_tag("depth_as_color"); rg_u64(rg_digest_of(d));
    rg_u32(w); rg_u32(h); rg_u32(fmt);
    return 0;
}

static u32 nb_pipe_create(void* u, const char* vs, const char* ps,
                          const rsx_be_render_state* rs,
                          const rsx_vertex_layout_plan* l,
                          u32 stride, rsx_be_format fmt, u32 rt_count)
{
    (void)u;
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag("pipeline_create");
    if (vs) rg_bytes(vs, strlen(vs) + 1u); else rg_u32(0);
    if (ps) rg_bytes(ps, strlen(ps) + 1u); else rg_u32(0);
    if (rs) rg_bytes(rs, sizeof *rs);
    if (l) rg_bytes(l, sizeof *l);
    rg_u32(stride); rg_u32((u32)fmt); rg_u32(rt_count);
    const u64 d = s_hash;
    s_hash = save;
    return rg_commit(save, "pipeline_create", d);
}
static void nb_pipe_release(void* u, u32 p)
{
    (void)u;
    rg_tag("pipeline_release"); rg_u64(rg_digest_of(p));
}

static void nb_bind_targets(void* u, const u32* s, u32 n, u32 d)
{
    (void)u;
    rg_tag("bind_targets"); rg_u32(n); rg_u64(rg_digest_of(d));
    for (u32 i = 0; i < n; i++) rg_u64(rg_digest_of(s[i]));
}
static void nb_bind_pipeline(void* u, u32 p)
{
    (void)u;
    rg_tag("bind_pipeline"); rg_u64(rg_digest_of(p));
}
static void nb_bind_vs(void* u, const void* d, u32 n)
{
    (void)u;
    rg_tag("bind_vs_constants"); rg_u32(n);
    if (d && n) rg_bytes(d, n);
}
static void nb_bind_ps(void* u, const void* d, u32 n)
{
    (void)u;
    rg_tag("bind_ps_constants"); rg_u32(n);
    if (d && n) rg_bytes(d, n);
}
/* `mask` is a bit per unit, not a count: the arrays always carry every
 * unit's entry, zeroed for the ones the draw does not bind. */
static void nb_bind_tex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    rg_tag("bind_textures"); rg_u32(mask);
    for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) {
        rg_u64(rg_digest_of(t[i]));
        if (s) rg_bytes(&s[i], sizeof s[i]);
    }
}
static void nb_bind_vtex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    rg_tag("bind_vertex_textures"); rg_u32(mask);
    for (u32 i = 0; i < RSX_BE_MAX_VERTEX_TEXTURES; i++) {
        rg_u64(rg_digest_of(t[i]));
        if (s) rg_bytes(&s[i], sizeof s[i]);
    }
}
static void nb_viewport(void* u, float x, float y, float w, float h)
{
    (void)u;
    rg_tag("viewport"); rg_bytes(&x, 4); rg_bytes(&y, 4);
    rg_bytes(&w, 4); rg_bytes(&h, 4);
}
static void nb_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    rg_tag("scissor"); rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h);
}
static void nb_stencil(void* u, u32 r)
{
    (void)u;
    rg_tag("stencil_ref"); rg_u32(r);
}
static void nb_depth_bounds(void* u, float lo, float hi)
{
    (void)u;
    rg_tag("depth_bounds"); rg_bytes(&lo, 4); rg_bytes(&hi, 4);
}
static void nb_depth_bias(void* u, float sl, float un, u32 bits)
{
    (void)u;
    rg_tag("depth_bias"); rg_bytes(&sl, 4); rg_bytes(&un, 4); rg_u32(bits);
}

static void nb_draw(void* u, rsx_topology topo, const void* v, u32 n,
                    u32 stride, const u32* idx, u32 ni)
{
    (void)u;
    rg_tag("draw"); rg_u32((u32)topo); rg_u32(n); rg_u32(stride); rg_u32(ni);
    if (idx && ni) rg_bytes(idx, (size_t)ni * sizeof(*idx));
    if (v && n && stride) rg_bytes(v, (size_t)n * stride);
    s_draws++; s_vertices += n;
}

static int nb_draw_fill(void* u, rsx_topology topo,
                        int (*fill)(void* ctx, void* dst), void* ctx,
                        u32 n, u32 stride, const u32* idx, u32 ni)
{
    (void)u;
    const size_t bytes = (size_t)n * stride;
    if (bytes > s_fill_cap) {
        unsigned char* p = (unsigned char*)realloc(s_fill, bytes);
        if (!p) return 0;
        s_fill = p; s_fill_cap = bytes;
    }
    if (!fill(ctx, s_fill)) return 1;
    rg_tag("draw_fill"); rg_u32((u32)topo); rg_u32(n); rg_u32(stride); rg_u32(ni);
    if (idx && ni) rg_bytes(idx, (size_t)ni * sizeof(*idx));
    if (s_fill && bytes) rg_bytes(s_fill, bytes);
    s_draws++; s_vertices += n;
    return 1;
}

static void nb_clear_color(void* u, u32 s, const float rgba[4])
{
    (void)u;
    rg_tag("clear_color"); rg_u64(rg_digest_of(s)); rg_bytes(rgba, 16);
}
static void nb_clear_depth(void* u, u32 d, u32 flags, float dv, u8 st)
{
    (void)u;
    rg_tag("clear_depth_stencil"); rg_u64(rg_digest_of(d));
    rg_u32(flags); rg_bytes(&dv, 4); rg_bytes(&st, 1);
}
static void nb_clear_color_rect(void* u, u32 s, const float rgba[4],
                                u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    rg_tag("clear_color_rect"); rg_u64(rg_digest_of(s)); rg_bytes(rgba, 16);
    rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h);
}
static void nb_clear_depth_rect(void* u, u32 d, u32 flags, float dv, u8 st,
                                u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    rg_tag("clear_depth_stencil_rect"); rg_u64(rg_digest_of(d));
    rg_u32(flags); rg_bytes(&dv, 4); rg_bytes(&st, 1);
    rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h);
}

static void nb_present(void* u, u32 s)
{
    (void)u;
    rg_tag("present"); rg_u64(rg_digest_of(s));
}
static void nb_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                        void* out, u32 pitch)
{
    (void)u;
    rg_tag("readback"); rg_u64(rg_digest_of(s));
    rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h); rg_u32(pitch);
    memset(out, 0, (size_t)w * 4u * h);
}
static int nb_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h,
                             float* out, u32 pitch)
{
    (void)u; (void)out;
    rg_tag("depth_readback"); rg_u64(rg_digest_of(d));
    rg_u32(x); rg_u32(y); rg_u32(w); rg_u32(h); rg_u32(pitch);
    return -1;
}
static u32 nb_color_snapshot(void* u, u32 s)
{
    (void)u;
    return rg_new_derived(s, "color_snapshot", 0, 0, 0);
}
static u32 nb_tex_create_3d(void* u, rsx_be_format f, u32 w, u32 h, u32 d,
                            u32 m, u32 remap, u32 fmt)
{
    (void)u;
    const u64 save = s_hash;
    s_hash = RG_BASIS;
    rg_tag("texture_create_3d");
    rg_u32((u32)f); rg_u32(w); rg_u32(h); rg_u32(d); rg_u32(m);
    rg_u32(remap); rg_u32(fmt);
    const u64 dg = s_hash;
    s_hash = save;
    return rg_commit(save, "texture_create_3d", dg);
}
static void nb_tex_upload_3d(void* u, u32 t, u32 z, u32 m, u32 w, u32 h,
                             const void* p, u32 rb)
{
    (void)u;
    rg_tag("texture_upload_3d"); rg_u64(rg_digest_of(t));
    rg_u32(z); rg_u32(m); rg_u32(w); rg_u32(h); rg_u32(rb);
    if (p && rb && h) rg_bytes(p, (size_t)rb * h);
}

static const rsx_draw_backend s_backend = {
    .init = nb_init,
    .shutdown = nb_shutdown,
    .submit_and_wait = nb_submit,
    .texture_create = nb_tex_create,
    .texture_upload = nb_tex_upload,
    .texture_release = nb_tex_release,
    .color_target_create = nb_ct_create,
    .color_target_release = nb_ct_release,
    .color_target_write = nb_ct_write,
    .surface_view = nb_surface_view,
    .depth_target_create = nb_dt_create,
    .depth_target_release = nb_dt_release,
    .depth_snapshot = nb_depth_snap,
    .pipeline_create = nb_pipe_create,
    .pipeline_release = nb_pipe_release,
    .bind_targets = nb_bind_targets,
    .bind_pipeline = nb_bind_pipeline,
    .bind_vs_constants = nb_bind_vs,
    .bind_ps_constants = nb_bind_ps,
    .bind_textures = nb_bind_tex,
    .bind_vertex_textures = nb_bind_vtex,
    .set_viewport = nb_viewport,
    .set_scissor = nb_scissor,
    .set_stencil_ref = nb_stencil,
    .set_depth_bounds = nb_depth_bounds,
    .set_depth_bias = nb_depth_bias,
    .draw = nb_draw,
    .clear_color = nb_clear_color,
    .clear_depth_stencil = nb_clear_depth,
    .present = nb_present,
    .readback = nb_readback,
    .depth_readback = nb_depth_readback,
    .color_snapshot = nb_color_snapshot,
    .texture_create_3d = nb_tex_create_3d,
    .texture_upload_3d = nb_tex_upload_3d,
    .depth_as_color = nb_depth_as_color,
    .clear_color_rect = nb_clear_color_rect,
    .clear_depth_stencil_rect = nb_clear_depth_rect,
    .draw_fill = nb_draw_fill,
    .texture_upload_ordered = 0,
};

const rsx_draw_backend* replay_null_backend(void) { return &s_backend; }
unsigned long long replay_null_draws(void) { return s_draws; }
unsigned long long replay_null_draw_vertices(void) { return s_vertices; }
unsigned long long replay_null_hash(void) { return s_hash; }
void replay_null_reset(void)
{
    s_draws = 0; s_vertices = 0;
    s_hash = RG_BASIS;
}
