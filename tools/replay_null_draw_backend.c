#include "replay_null_draw_backend.h"

#include <stdlib.h>
#include <string.h>

static u32 s_next_handle = 1;
static unsigned long long s_draws, s_vertices;
static unsigned char* s_fill;
static size_t s_fill_cap;

static int nb_init(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; return 0; }
static void nb_shutdown(void* u) { (void)u; free(s_fill); s_fill = NULL; s_fill_cap = 0; }
static void nb_submit(void* u, u32 reason) { (void)u; (void)reason; }

static u32 nb_tex_create(void* u, rsx_be_format f, u32 w, u32 h, u32 m,
                         u32 faces, u32 remap, u32 fmt)
{ (void)u; (void)f; (void)w; (void)h; (void)m; (void)faces; (void)remap; (void)fmt;
  return s_next_handle++; }
static void nb_tex_upload(void* u, u32 t, u32 f, u32 m, u32 w, u32 h,
                          const void* p, u32 rb, u32 rows)
{ (void)u; (void)t; (void)f; (void)m; (void)w; (void)h; (void)p; (void)rb; (void)rows; }
static void nb_tex_release(void* u, u32 t) { (void)u; (void)t; }

static u32 nb_ct_create(void* u, rsx_be_format f, u32 w, u32 h,
                        const void* seed, u32 seed_rb)
{ (void)u; (void)f; (void)w; (void)h; (void)seed; (void)seed_rb;
  return s_next_handle++; }
static void nb_ct_release(void* u, u32 s) { (void)u; (void)s; }
static void nb_ct_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                        const void* rows, u32 rb)
{ (void)u; (void)s; (void)x; (void)y; (void)w; (void)h; (void)rows; (void)rb; }
static u32 nb_surface_view(void* u, u32 s, u32 remap, u32 fmt)
{ (void)u; (void)remap; (void)fmt; return s ? s : s_next_handle++; }

static u32 nb_dt_create(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; return s_next_handle++; }
static void nb_dt_release(void* u, u32 d) { (void)u; (void)d; }
static u32 nb_depth_snap(void* u, u32 d, u32 w, u32 h)
{ (void)u; (void)d; (void)w; (void)h; return s_next_handle++; }
static u32 nb_depth_as_color(void* u, u32 d, u32 w, u32 h, u32 fmt)
{ (void)u; (void)d; (void)w; (void)h; (void)fmt; return 0; }

static u32 nb_pipe_create(void* u, const char* vs, const char* ps,
                          const rsx_be_render_state* rs,
                          const rsx_vertex_layout_plan* l,
                          u32 stride, rsx_be_format fmt, u32 rt_count)
{ (void)u; (void)vs; (void)ps; (void)rs; (void)l; (void)stride; (void)fmt; (void)rt_count;
  return s_next_handle++; }
static void nb_pipe_release(void* u, u32 p) { (void)u; (void)p; }

static void nb_bind_targets(void* u, const u32* s, u32 n, u32 d)
{ (void)u; (void)s; (void)n; (void)d; }
static void nb_bind_pipeline(void* u, u32 p) { (void)u; (void)p; }
static void nb_bind_vs(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void nb_bind_ps(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void nb_bind_tex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; (void)t; (void)s; (void)m; }
static void nb_bind_vtex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; (void)t; (void)s; (void)m; }
static void nb_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void nb_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void nb_stencil(void* u, u32 r) { (void)u; (void)r; }
static void nb_depth_bounds(void* u, float lo, float hi) { (void)u; (void)lo; (void)hi; }
static void nb_depth_bias(void* u, float sl, float un, u32 bits)
{ (void)u; (void)sl; (void)un; (void)bits; }

static void nb_draw(void* u, rsx_topology topo, const void* v, u32 n,
                    u32 stride, const u32* idx, u32 ni)
{ (void)u; (void)topo; (void)v; (void)stride; (void)idx; (void)ni;
  s_draws++; s_vertices += n; }

static int nb_draw_fill(void* u, rsx_topology topo,
                        int (*fill)(void* ctx, void* dst), void* ctx,
                        u32 n, u32 stride, const u32* idx, u32 ni)
{ (void)u; (void)topo; (void)idx; (void)ni;
  const size_t bytes = (size_t)n * stride;
  if (bytes > s_fill_cap) {
      unsigned char* p = (unsigned char*)realloc(s_fill, bytes);
      if (!p) return 0;
      s_fill = p; s_fill_cap = bytes;
  }
  if (!fill(ctx, s_fill)) return 1;
  s_draws++; s_vertices += n;
  return 1;
}

static void nb_clear_color(void* u, u32 s, const float rgba[4])
{ (void)u; (void)s; (void)rgba; }
static void nb_clear_depth(void* u, u32 d, u32 flags, float dv, u8 st)
{ (void)u; (void)d; (void)flags; (void)dv; (void)st; }
static void nb_clear_color_rect(void* u, u32 s, const float rgba[4],
                                u32 x, u32 y, u32 w, u32 h)
{ (void)u; (void)s; (void)rgba; (void)x; (void)y; (void)w; (void)h; }
static void nb_clear_depth_rect(void* u, u32 d, u32 flags, float dv, u8 st,
                                u32 x, u32 y, u32 w, u32 h)
{ (void)u; (void)d; (void)flags; (void)dv; (void)st; (void)x; (void)y; (void)w; (void)h; }

static void nb_present(void* u, u32 s) { (void)u; (void)s; }
static void nb_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                        void* out, u32 pitch)
{ (void)u; (void)s; (void)x; (void)y; (void)w; (void)h; (void)pitch;
  memset(out, 0, (size_t)w * 4u * h); }
static int nb_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h,
                             float* out, u32 pitch)
{ (void)u; (void)d; (void)x; (void)y; (void)w; (void)h; (void)out; (void)pitch;
  return -1; }
static u32 nb_color_snapshot(void* u, u32 s) { (void)u; (void)s; return s_next_handle++; }
static u32 nb_tex_create_3d(void* u, rsx_be_format f, u32 w, u32 h, u32 d,
                            u32 m, u32 remap, u32 fmt)
{ (void)u; (void)f; (void)w; (void)h; (void)d; (void)m; (void)remap; (void)fmt;
  return s_next_handle++; }
static void nb_tex_upload_3d(void* u, u32 t, u32 z, u32 m, u32 w, u32 h,
                             const void* p, u32 rb)
{ (void)u; (void)t; (void)z; (void)m; (void)w; (void)h; (void)p; (void)rb; }

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
void replay_null_reset(void) { s_draws = 0; s_vertices = 0; }
