/*
 * ps3recomp - platform-neutral RSX draw engine over the register file
 *
 * The vtable backends in rsx_commands.c are driven by rsx_state, a curated
 * dirty-flag struct that models no primitive restart, no vertex textures and
 * no two-sided stencil, and whose setters fire only at SET_BEGIN_END. The
 * register-file dispatcher in rsx_dispatch.c has all of it already, in
 * portable C, behind the rsx_dispatch_sink interface. This engine implements
 * that sink and drives a backend through the record-oriented interface below,
 * so a title's frame reaches the host API the way the hardware issued it.
 *
 * The engine logic is carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has actually shipped on. That file is
 * vendored and is NOT touched: what moves here is the orchestration, and the
 * comments name the reference line numbers so the two can be compared. The
 * mathematics is already shared and already portable -- rsx_vertex_compact.c,
 * rsx_restart_cuts.h, rsx_texture_layout.c, the two decompilers.
 *
 * The split between engine and backend is semantic, at the granularity the
 * engine thinks in, not a 1:1 wrapper over one host API's calls: resources by
 * what they are for, pipelines from the two HLSL strings plus render state
 * plus the vertex layout, and per-draw binding of what a draw names. Resource
 * state tracking, descriptor lifetime and command encoding belong entirely to
 * the backend, which is why nothing here mentions a barrier.
 *
 * Handles are small integers owned by the backend; 0 always means "none".
 */
#ifndef PS3RECOMP_RSX_DRAW_ENGINE_H
#define PS3RECOMP_RSX_DRAW_ENGINE_H

#include "rsx_dispatch.h"
#include "rsx_primitives.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the engine hands a backend ------------------------------------ */

/* Surface and texture formats the engine asks for. The texture side is
 * rsx_texfmt's classification, which rsx_texture_decode already produces host
 * rows for, so a backend maps one enum rather than two. */
typedef enum {
    RSX_BE_FMT_NONE = 0,
    RSX_BE_FMT_R8,
    RSX_BE_FMT_R8G8,
    RSX_BE_FMT_R8G8B8A8,      /* decoded colour: always R,G,B,A byte order */
    RSX_BE_FMT_BC1,
    RSX_BE_FMT_BC2,
    RSX_BE_FMT_BC3,
    RSX_BE_FMT_R16,
    RSX_BE_FMT_R16G16,
    RSX_BE_FMT_R16G16F,
    RSX_BE_FMT_R16G16B16A16F, /* SET_SURFACE_FORMAT 0xB, the HDR target    */
    RSX_BE_FMT_R32F,
    RSX_BE_FMT_R32G32B32A32F
} rsx_be_format;

/* One texture unit's sampler, decoded out of SET_TEXTURE_FILTER,
 * SET_TEXTURE_ADDRESS and SET_TEXTURE_CONTROL0 the way rsx_live_draw.c's
 * decode_sampler (1464-1487) reads them. The wrap fields stay as the guest's
 * own 1..8 codes because that is what both backends already translate. */
typedef struct rsx_be_sampler_desc {
    u8  min_linear, mag_linear, mip_linear, mip_present;
    u8  wrap_s, wrap_t, wrap_r;
    /* What the BORDER wrap returns outside 0..1, reduced to the colours
     * every backend can express: SET_TEXTURE_BORDER_COLOR read the way the
     * unit's format reads it (a depth format takes its depth from the low 24
     * bits). 0 transparent black (also the old behaviour), 1 opaque black,
     * 2 opaque white, 3 any other colour, which the backend clamps to the
     * edge texel instead -- nearer than black for the colours seen (a video
     * title's chroma planes border at 0x80, neutral). */
    u8  border;
    float min_lod, max_lod;
} rsx_be_sampler_desc;

/* The NV4097 render state a pipeline is built from: rsx_live_draw.c's
 * render_state_t (1312-1327) with the D3D12 enums left undecoded, since the
 * guest values are the portable form and each backend has its own table.
 * The stencil REFERENCE is deliberately absent -- it is set on the encoder,
 * not baked into the pipeline, and so is not part of the pipeline key. */
typedef struct rsx_be_render_state {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    /* Targets 1..3 of an MRT set: COLOR_MASK_MRT (0x0370), one nibble per
     * target at [4i..4i+3] = A,R,G,B, and BLEND_ENABLE_MRT (0x036C), bit i.
     * Target 0 keeps color_mask/blend_enable. Both reset to 0 (RPCS3). */
    u32 color_mask_mrt, blend_mrt;
    u32 rt_fp16;                    /* the target is an FP16 HDR surface */
    u32 stencil_enable, stencil_two_sided;
    u32 depth_bounds_test;          /* SET_DEPTH_BOUNDS_TEST_ENABLE */
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
} rsx_be_render_state;

/* submit_and_wait reasons, for a backend that wants to log or count them. */
#define RSX_BE_FLUSH_VERTEX_RING     0
#define RSX_BE_FLUSH_GUEST_REFERENCE 1
#define RSX_BE_FLUSH_SHUTDOWN        2

/* clear_depth_stencil flags */
#define RSX_BE_CLEAR_DEPTH   0x1u
#define RSX_BE_CLEAR_STENCIL 0x2u

#define RSX_BE_MAX_TEXTURES        RSX_DSP_NUM_TEXTURES
#define RSX_BE_MAX_VERTEX_TEXTURES RSX_DSP_NUM_VERTEX_TEXTURES
/* Colour targets a draw can name: A alone, or an MRT set of A, B, C and D. */
#define RSX_BE_MAX_COLOR_TARGETS   4

/* How many distinct guest textures the engine keeps before it evicts the
 * least recently used one. Public so the eviction test can be exact about
 * where the boundary is rather than assuming a number. */
#define RSX_DRAW_ENGINE_TEXTURE_CACHE 1024

typedef struct {
    uint32_t src_off_dst_slot; /* low 16 bits = src byte offset within vertex, high 16 bits = dst slot index */
    uint32_t type_count;       /* low 16 bits = format type (1..7), high 16 bits = count of components (1..4) */
} rsx_vtx_attr_desc;

/* Macro Target 2: descriptor for offloading vertex format unpacking to GPU compute */
typedef struct rsx_vtx_gpu_desc {
    u32               vertex_count;       /* must remain first member for *(const u32*)ctx */
    int               gpu_eligible;       /* 1 if eligible for GPU compute unpack, 0 otherwise */
    u32               layout_mode;        /* 0 = Fast MLB 3D, 1 = Fast MLB 2D UI, 2 = Fast MLB 2D UI with Color, 3 = Generic Descriptor */
    u32               flags;              /* bit 0: remap enabled */
    u32               src_stride;         /* source vertex stride in bytes (e.g. 24, 16, 20, 22, 36) */
    u32               dst_stride;         /* destination vertex stride in floats */
    const void*       raw_data;           /* guest pointer to raw vertex attribute bytes */
    u32               raw_bytes;          /* total bytes of raw vertex data */
    const u32*        remap_indices;      /* remapped index buffer if flags & 1 */
    u32               num_attrs;          /* number of active attributes */
    rsx_vtx_attr_desc attrs[8];           /* attribute descriptors for Mode 3 */
} rsx_vtx_gpu_desc;

/* ---- the backend interface ---------------------------------------------- */

/* One face of a cube map assembled from colour targets. `x, y, w, h` is the
 * rectangle of `handle` that holds the face (a face is either its own target,
 * or a window into a stacked one). */
typedef struct rsx_be_cube_face {
    u32 handle;
    u32 x, y, w, h;
} rsx_be_cube_face;

#define RSX_BE_HAS_COLOR_SNAPSHOT 1
typedef struct rsx_draw_backend {
    void* user;

    /* Lifecycle and submission. submit_and_wait is fully synchronous: the
     * engine recycles its staging the moment it returns, which is the same
     * contract as rsx_live_draw.c's ld_flush (1659-1736). */
    int  (*init)(void* user, u32 width, u32 height);
    void (*shutdown)(void* user);
    void (*submit_and_wait)(void* user, u32 reason);

    /* Textures. `remap` is the raw TEXTURE_CONTROL1 crossbar and `rsx_fmt`
     * the SET_TEXTURE_FORMAT byte it is decoded against, because the crossbar
     * is a property of the view rather than of the pixels. Levels arrive one
     * upload each, already decoded to host rows by rsx_texture_decode. */
    u32  (*texture_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                           u32 mips, u32 faces, u32 remap, u32 rsx_fmt);
    void (*texture_upload)(void* user, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows);
    void (*texture_release)(void* user, u32 texture);

    /* Colour targets. `seed` is the guest's own bytes for the surface,
     * decoded to R,G,B,A rows, or NULL when they could not be resolved: a
     * title CPU-initialises its render targets and then samples them before
     * anything draws into them, which is why the D3D12 backend's off_rt_get
     * seeds a new target the same way. */
    u32  (*color_target_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                                const void* seed, u32 seed_row_bytes);
    void (*color_target_release)(void* user, u32 surface);
    /* Optional. Write R,G,B,A rows into a rectangle of a colour target: pixels
     * the guest put there outside any draw, with a 2D transfer. NULL when the
     * backend cannot, and the engine then leaves the target as it is. */
    void (*color_target_write)(void* user, u32 surface, u32 x, u32 y, u32 w,
                               u32 h, const void* rows, u32 row_bytes);
    /* A texture handle for a colour target a texture unit names, wearing that
     * unit's TEXTURE_CONTROL1 crossbar. An uploaded texture bakes the crossbar
     * in when it is decoded, so only a surface bind needs this; 0 means the
     * backend has no view and the engine binds the target itself. */
    u32  (*surface_view)(void* user, u32 surface, u32 remap, u32 rsx_format);
    /* Optional. Build (or refresh) a six-layer cube image from six colour
     * targets with a GPU copy, one layer per face, and return a texture
     * handle with a cube view. `serials` are the faces' colour serials, so a
     * backend can skip the copy while nothing changed; the engine owns
     * nothing here, the backend keeps the handle in its own cache. 0 when the
     * backend cannot (the engine leaves the unit on the dummy). */
    u32  (*cube_target_view)(void* user, u32 location, u32 offset,
                             const rsx_be_cube_face faces[6],
                             const u64 serials[6],
                             rsx_be_format fmt, u32 w, u32 h,
                             u32 remap, u32 rsx_format);
    /* Optional. The most recent complete cube image this backend built for a
     * guest address. A probe can be incomplete for a frame or two while a
     * face is rewritten; binding the last complete image keeps the lighting
     * stable instead of flashing the dummy. 0 when none was ever built there
     * (the engine then keeps the dummy), and the backend keeps ownership --
     * the handle is valid only while its cache entry lives. */
    u32  (*cube_target_last)(void* user, u32 location, u32 offset,
                             rsx_be_format fmt, u32 w, u32 h,
                             u32 remap, u32 rsx_format);

    /* One depth target per guest zeta address. */
    u32  (*depth_target_create)(void* user, u32 w, u32 h);
    void (*depth_target_release)(void* user, u32 depth);
    /* Resolve a depth target into a sampleable texture and return it; 0 when
     * the backend cannot. The engine only asks after a depth-writing draw. */
    u32  (*depth_snapshot)(void* user, u32 depth, u32 w, u32 h);

    /* Pipelines, from the decompilers' HLSL plus the state that selects a
     * variant. This is the one call that hides D3DCompile against glslang
     * plus spirv-cross plus -newLibraryWithSource:, and the input layout with
     * it. 0 means the pair could not be built; the engine caches that.
     *
     * `rt_count` is how many colour attachments the pass will have, and every
     * one of them takes target A's blend and colour mask: a zero-initialised
     * secondary attachment writes nothing at all, which would leave a
     * deferred pass with only its first G-buffer plane. It is part of the
     * engine's pipeline key, because a host API matches a pipeline's
     * attachments against the pass it is encoded in. */
    u32  (*pipeline_create)(void* user, const char* vs_hlsl, const char* ps_hlsl,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 vertex_stride, rsx_be_format rt_fmt,
                            u32 rt_count);
    /* Optional: the engine's 64-bit identity for the pipeline_create that
     * follows. It hashes the guest microcode plus every input that changes
     * the HLSL, so a backend can keep an on-disk shader cache keyed by the
     * guest program instead of by the HLSL text. NULL when unused. */
    void (*set_pipeline_key)(void* user, u64 key);
    void (*pipeline_release)(void* user, u32 pipeline);

    /* Per-draw binding. The backend owns every ring the D3D12 engine used to
     * own itself, so the engine hands over CPU-side arrays and never mentions
     * a descriptor. */
    /* The colour targets this draw writes, and the depth attachment under
     * them. `count` is 1 for an ordinary draw and up to
     * RSX_BE_MAX_COLOR_TARGETS for an MRT set, whose members are all target
     * A's size and format -- the rule both render paths bind by, and the one
     * a host API enforces on a pass's attachments. */
    void (*bind_targets)(void* user, const u32* surfaces, u32 count, u32 depth);
    void (*bind_pipeline)(void* user, u32 pipeline);
    void (*bind_vs_constants)(void* user, const void* data, u32 bytes);
    void (*bind_ps_constants)(void* user, const void* data, u32 bytes);
    void (*bind_textures)(void* user, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask);
    void (*bind_vertex_textures)(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask);
    void (*set_viewport)(void* user, float x, float y, float w, float h);
    void (*set_scissor)(void* user, u32 x, u32 y, u32 w, u32 h);
    void (*set_stencil_ref)(void* user, u32 ref);
    /* Optional: the depth bounds for the next draw, called before every
     * draw like the stencil reference. The test itself is pipeline state
     * (rsx_be_render_state.depth_bounds_test); a backend that leaves this
     * NULL draws as if the test were off. */
    void (*set_depth_bounds)(void* user, float min, float max);
    /* Optional. The NV4097 polygon offset for the next draw, called before
     * every draw like the stencil reference (SET_POLY_OFFSET_{POINT,LINE,
     * FILL}_ENABLE 0x0A60/0x0A64/0x0A68, SCALE_FACTOR 0x0A78, BIAS 0x0A7C).
     * glPolygonOffset's terms: `slope` multiplies the primitive's maximum
     * depth slope and `units` the smallest resolvable difference of a depth
     * buffer of `depth_bits` bits (16 for Z16, 24 for Z24S8), so a backend
     * whose depth format is another converts the units. Both are 0 when the
     * offset is off. Left NULL, every draw is drawn without it. */
    void (*set_depth_bias)(void* user, float slope, float units, u32 depth_bits);
    /* One draw. `indices` is NULL for a non-indexed one. Everything the
     * hardware has and the host does not -- quads, quad strips, fans,
     * polygons -- has already been expanded into a triangle list by the time
     * it gets here, so the topology is only ever one a host API has. */
    void (*draw)(void* user, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count);

    /* Clears, which are ordered against draws rather than folded into a pass:
     * a title draws the world, clears depth, and draws the weapon over it. */
    void (*clear_color)(void* user, u32 surface, const float rgba[4]);
    void (*clear_depth_stencil)(void* user, u32 depth, u32 flags,
                                float depth_value, u8 stencil);

    /* Present the named surface, and read a rectangle of one back. The rows
     * come back in the format the target was created with, so R,G,B,A for an
     * ordinary colour surface. */
    void (*present)(void* user, u32 surface);
    void (*readback)(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                     void* out, u32 out_pitch);

    /* Optional: read a rectangle of a depth target back as floats in [0, 1],
     * `out_pitch` bytes per row. Returns 0 on success. The engine uses it to
     * store a depth buffer into guest memory when a texture reads those bytes
     * in a format of its own (a D24S8 buffer as A8R8G8B8, a Z16 shadow map as
     * DEPTH16); left NULL, such a texture reads guest memory as it is. */
    int  (*depth_readback)(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                           float* out, u32 out_pitch);
    /* Optional same-target colour feedback. Return a fresh, immutable texture
     * of the target's full physical extent and storage format. Its GPU copy
     * must execute after all preceding writes and before the next draw. The
     * engine owns the returned handle and releases it with texture_release.
     * 0 rejects the consuming draw; NULL retains the legacy guest-upload route. */
    u32 (*color_snapshot)(void* user, u32 surface);

    /* Optional. 3D textures (SET_TEXTURE_FORMAT dimension 3, CONTROL3 depth):
     * a volume of `depth` slices per level, each slice one upload of decoded
     * rows. A backend that leaves these NULL gets the first slice as a 2D
     * texture through texture_create/texture_upload, as before. */
    u32  (*texture_create_3d)(void* user, rsx_be_format fmt, u32 w, u32 h,
                              u32 depth, u32 mips, u32 remap, u32 rsx_fmt);
    void (*texture_upload_3d)(void* user, u32 texture, u32 z, u32 mip,
                              u32 w, u32 h, const void* src, u32 row_bytes);
    /* Nonzero when texture_upload is ordered after every draw already handed
     * to the backend, so rewriting a texture those draws sample is safe. The
     * engine then refreshes a changed 2D texture in place instead of creating
     * a new one and releasing the old (an allocation, a free and a wait for
     * the GPU each time). Zero keeps the create-and-swap. */
    int texture_upload_ordered;
    /* Optional. A depth target's top-left w x h read as a texture of base
     * format `rsx_fmt` reads the bytes the zeta holds: a fresh texture (a
     * colour target for surface_view) whose texel is what depth_readback,
     * stored as eng_zdepth_to_guest stores it and decoded, gives.
     *  - A8R8G8B8 (0x85) of a Z24S8 zeta: R8G8B8A8, R = depth bits 15..8,
     *    G = bits 7..0, B = 0 (the stencil byte), A = bits 23..16, with
     *    round(depth * 0xFFFFFF) taken exactly;
     *  - DEPTH16 (0x92) of a Z16 zeta: R16 unorm of round(depth * 0xFFFF).
     * Any other format: 0. Made in command order, like
     * depth_snapshot, so no wait, and from the depth the draws so far left,
     * which may be the target the current pass still attaches; the engine
     * owns the handle. 0 when the backend cannot; NULL keeps the store
     * through guest memory. */
    u32  (*depth_as_color)(void* user, u32 depth, u32 w, u32 h, u32 rsx_fmt);

    /* Optional, both or neither: clear_color and clear_depth_stencil limited
     * to the rectangle (x, y, w, h) of the target, in the engine's pixels
     * (the sample grid of an antialiased surface), w and h never 0. RSX
     * clears only what the scissor and the surface clip let through: a
     * split-screen title renders the left view, then clears and renders the
     * right one in the same target with the scissor on that half. The engine
     * calls these only for a clear whose scissor leaves part of the clip
     * alone; left NULL, such a clear covers the whole target, as before. */
    void (*clear_color_rect)(void* user, u32 surface, const float rgba[4],
                             u32 x, u32 y, u32 w, u32 h);
    void (*clear_depth_stencil_rect)(void* user, u32 depth, u32 flags,
                                     float depth_value, u8 stencil,
                                     u32 x, u32 y, u32 w, u32 h);

    /* Optional, in place of bind_vs_constants: the transform constant block
     * of `total_bytes` (the same layout bind_vs_constants gets) of which the
     * vertex program reads only rows [first, first + count) of `rows` (the
     * whole 512-row file) and the `tail_bytes` of `tail` after the 512 rows
     * (the viewport epilogue). Rows outside the range are never read and may
     * hold anything. count may be 0. */
    void (*bind_vs_constants_range)(void* user, const void* rows, u32 first, u32 count,
                                    const void* tail, u32 tail_bytes, u32 total_bytes);

    /* Optional, in place of draw: the same draw, with the vertices written
     * straight into the backend's own vertex memory instead of copied there
     * from a CPU-side array. Once the backend has reserved vertex_count *
     * stride bytes for the draw (16-byte aligned, write-only: it may be
     * uncached, write-combined memory), it calls fill(ctx, dst) exactly once
     * to have the engine convert the vertices into them, then records the
     * draw -- or, if fill returns 0, records nothing. Between the reservation
     * and fill the backend calls nothing that could reuse those bytes.
     * Returns 1 when the draw was handled (drawn, dropped by the backend's own
     * rules, or failed in fill), 0 when the backend could not reserve the
     * memory and has recorded nothing: the engine then converts into a CPU
     * array and calls draw. Left NULL, every draw goes through draw. */
    int  (*draw_fill)(void* user, rsx_topology topology,
                      int (*fill)(void* ctx, void* dst), void* ctx,
                      u32 vertex_count, u32 stride, const u32* indices,
                      u32 index_count);

    /* Optional asynchronous colour readback, for on-demand render-target
     * readback (PS3RECOMP_RSX_WCB=async, docs/wcb-async.md). readback_async
     * queues a copy of the rectangle -- ordered after every draw recorded so
     * far and before every later one -- into backend-owned memory, and returns
     * at once with a nonzero ticket (0: cannot, and the engine reads back
     * synchronously when the guest needs the bytes). readback_async_fetch
     * waits for that copy alone if it has not finished, writes the rows in
     * the format readback would, and frees the ticket; 0 on success.
     * readback_async_release frees a ticket nobody fetched. All three are
     * called on the engine's thread. */
    u32  (*readback_async)(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h);
    int  (*readback_async_fetch)(void* user, u32 ticket, void* out, u32 out_pitch);
    void (*readback_async_release)(void* user, u32 ticket);
    /* Optional. Start the recorded work on the GPU without waiting and
     * continue recording into a fresh set (the present path's submit). Used
     * by PS3RECOMP_RSX_WCB_PREFETCH to let a copy queued at a sync point run
     * while the walker records the rest of the frame. NULL when the backend
     * cannot submit asynchronously; the engine then leaves the batch open. */
    void (*submit_async)(void* user);

} rsx_draw_backend;

/* ---- the engine --------------------------------------------------------- */

/* Register the backend the engine drives. Passing NULL unregisters it. */
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend);

/* Is the engine the path this run takes? True when PS3RECOMP_RSX_ENGINE names
 * "dispatch" (or the backend asked for it by default) and a backend has been
 * registered. Read once and cached, so it is cheap in the FIFO walker. */
int  rsx_draw_engine_enabled(void);

/* Called by a backend that wants the engine to be the default path with no
 * environment set. PS3RECOMP_RSX_ENGINE=vtable still turns it off. */
void rsx_draw_engine_set_default(int on);

/* A runner with its own RSX IO map supplies its address resolver before init.
 * NULL restores the toolkit's cellGcm mapping. */
void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user);
void rsx_draw_engine_set_display_query(int (*fn)(u32 offset));

int  rsx_draw_engine_init(u32 width, u32 height);
void rsx_draw_engine_shutdown(void);

/* One FIFO method, with its subchannel bits still on: the engine masks them
 * out itself, as rsx_live_draw_method (6902-6976) does, because a title whose
 * SPU-built command lists bind NV4097 to another subchannel would otherwise
 * write its state into the wrong register bank. */
void rsx_draw_engine_method(u32 method, u32 arg);

/* rsx_draw_engine_method for a run of `count` NV4097 methods method,
 * method + step, ... (step 4 or 0) on a 3D subchannel, with their words
 * still big-endian in guest memory at `src`. */
void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count);

/* cellGcmSetDisplayBuffer, so the flip can resolve a buffer id to a surface
 * the engine has actually rendered rather than to whatever is bound. */
void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height);

/* The guest's SET_REFERENCE sync point: submit and wait, so a title spinning
 * on its fence sees the GPU catch up. */
void rsx_draw_engine_flush(void);

/* Wait until every packet published to the draw-record worker has executed.
 * No-op when PS3RECOMP_WALKER_MT is off or WORKERS=0. The replay harness and
 * any caller that tears down backend state between frames calls this. */
void rsx_draw_engine_mt_sync(void);

/* Present whatever the frame has recorded, for a host that drives the flip
 * itself rather than through the FIFO's 0xE944. */
void rsx_draw_engine_present(void);
/* A runner retiring a queued flip names the buffer explicitly. */
void rsx_draw_engine_present_buffer(u32 buffer_id);
/* A flip the engine never sees as 0xE944 (a 0xFEAD driver word or the SetFlip
 * HLE, both handled by cellGcmSys): remember the buffer for the host's next
 * rsx_draw_engine_present() without presenting now. */
void rsx_draw_engine_note_flip(u32 buffer_id);

/* Guest memory and the surfaces the engine renders on the host GPU, kept
 * coherent around a 2D transfer (NV3089) that the FIFO walker performs on
 * guest memory. A title copies its rendered scene out to main memory for
 * post-processing, back into VRAM, and into its display buffer with such
 * transfers; on the engine those pixels exist only on the GPU.
 *
 * Before the transfer reads guest bytes, guest_read writes the pixels of any
 * engine surface in that rectangle back to guest memory. After it writes,
 * guest_wrote pushes the guest bytes into any engine surface the rectangle
 * falls in. `offset` is the rectangle's first byte, `pitch` the guest row
 * pitch, `w` and `h` in 32-bit pixels. Only 8-bit-per-channel surfaces laid
 * out with the same pitch take part; anything else is left alone. */
void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h);
void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h);

/* PWCB cut 1 (PS3RECOMP_RSX_WCB_RECT): the page-aligned surface-pixel
 * rectangle covering the byte span a 2-D transfer reads. `guest_base` is the
 * surface's guest address, `pitch` its row pitch, `bpp` bytes per pixel, `xb`
 * the first byte read inside a row, `byte_w` bytes per row, `y` the first row
 * and `rows` how many. The rectangle is clamped to the surface. Returns 1 when
 * non-empty. Exported for the unit test. */
typedef struct rsx_wcb_copy_rect {
    u32 x, y, w, h;
} rsx_wcb_copy_rect;
int rsx_wcb_transfer_rect(u32 guest_base, u32 pitch, u32 bpp, u32 xb, u32 byte_w,
                          u32 y, u32 rows, u32 surf_w, u32 surf_h,
                          rsx_wcb_copy_rect* out);

/* PWCB cut 1: the transfer's guest read of a rectangle is over (nv3089_blit
 * has copied the source to its destination). Re-protect the rest of a surface
 * that a rectangle-only resolve left stale, so a later read of another part
 * faults and is copied rather than reading old bytes. */
void rsx_draw_engine_guest_read_done(void);

/* Flush all dirty offscreen render surfaces (and depth buffers) back to guest
 * memory before CPU/SPU synchronization points (SET_REFERENCE, SEMAPHORE_RELEASE,
 * flip). This ensures titles reading back offscreen-rendered textures from guest
 * memory (e.g. MLB The Show 16 dynamic player faces and crowd textures) observe
 * valid rendered texels instead of uninitialized RAM. */
void rsx_draw_engine_flush_dirty_to_guest(void);

/* ---- walker profiler and frame capture (PS3RECOMP_WALKER_PROF/CAPTURE) -- */

/* The phases the profiler splits a draw group into. WALKER and DISPATCH are
 * filled by cellGcmSys.c; the rest by sink_end in this file. `OTHER` is
 * derived at report time as dispatch minus the five measured phases, so an
 * early-returning group (a dropped draw) is accounted for rather than lost. */
enum {
    RSX_WPROF_FETCH = 0,
    RSX_WPROF_TEX,
    RSX_WPROF_PIPE,
    RSX_WPROF_BIND,
    RSX_WPROF_DRAW,
    RSX_WPROF_PHASES
};

/* Cached PS3RECOMP_WALKER_PROF. Cheap (one static compare) when off. */
int  rsx_wprof_enabled(void);
/* Start of a drain pass: decides (PS3RECOMP_WALKER_PROF_SAMPLE, default 8)
 * whether this pass is timed. cellGcmSys uses the return value to gate every
 * per-method timestamp, so a skipped pass costs one counter. */
int  rsx_wprof_drain_begin(void);
unsigned long long rsx_wprof_now(void);
/* cellGcmSys.c: one drain pass, and the part of it spent inside the engine. */
void rsx_wprof_note_drain(unsigned long long ns);
void rsx_wprof_note_dispatch(unsigned long long ns);
/* A FIFO flip word: the pass that contains it completes a frame. */
void rsx_wprof_note_flip(void);
/* vkCreate*Pipelines time, reported as `pipecrt`/`pipems` in the next report. */
void rsx_wprof_note_pipeline_create(unsigned long long ns);
/* Log and reset the frame counters if a flip was seen in this pass.
 * `profiled` is the value rsx_wprof_drain_begin() returned for the same
 * pass, so a skipped pass never reports (the two drain threads used to share
 * one global). */
void rsx_wprof_drain_end(int profiled);

/* Replay only: a replayed capture has no display buffer, so eng_present()
 * never reaches its frame tick. Advance g.frames by one after each replayed
 * frame so the per-frame texture revalidation / hash cadence and the
 * last_use ageing follow the live path instead of staying on frame 0. */
void rsx_draw_engine_replay_advance_frame(void);

/* Live switches from PS3RECOMP_CTL (cellGcmSys polls the file and calls these
 * on the walker thread). Changing WALKER_FAST flushes the fetch/pipeline
 * memos so the new arm starts cold. */
void rsx_draw_engine_set_walker_fast(int on);
void rsx_draw_engine_set_fetch_memo(int on);
void rsx_draw_engine_set_pipe_memo(int on);
void rsx_draw_engine_set_remap_cache(int on);
void rsx_draw_engine_set_memo_index(int on);
void rsx_draw_engine_set_refs_hash_fast(int on);
void rsx_draw_engine_set_no_draw(int on);

/* Captured dispatcher state: enough to start a replay at the frame boundary
 * the capture began on. The sink and the coverage counters are not part of
 * it; regs/vp/constants and the BEGIN_END accumulator are. */
#define RSX_WCAP_MAGIC 0x50414357u /* "WCAP" */
typedef struct rsx_wcap_state {
    u32 magic;
    u32 version;
    u32 width, height;
    u32 io_addr, io_size;
    u32 in_begin_end;
    u32 current_primitive;
    u32 inline_len;
    u32 inline_dropped;
    u32 regs[RSX_DSP_NUM_REGS];
    u32 vp[RSX_DSP_VP_WORDS];
    u32 constants[RSX_DSP_NUM_CONSTANTS][4];
    u8  inline_data[RSX_DSP_INLINE_MAX_BYTES];
} rsx_wcap_state;

/* PS3RECOMP_WALKER_CAPTURE=<prefix>: capture one pitch frame, triggered by
 * the existence of <prefix>.trigger, into <prefix>.fifo (the frame's method
 * stream), <prefix>.state (rsx_wcap_state) and <prefix>.mem (the guest pages
 * the draws read). Called by cellGcmSys.c at a drain boundary. */
int  rsx_wcap_begin_if_triggered(void);
void rsx_wcap_word(u32 w);
void rsx_wcap_note_texture(u32 location, u32 offset, u32 len);
void rsx_wcap_flip(u32 w);
void rsx_wcap_drain_end(void);

/* Snapshot / restore the dispatcher state used by a capture. */
int rsx_draw_engine_state_snapshot(rsx_wcap_state* out);
int rsx_draw_engine_state_restore(const rsx_wcap_state* in);

/* PS3RECOMP_WALKER_FAST=1 (read at engine init, default off): cut 1's
 * fetch/remap plan cache and cut 3's pipeline input memo. A memo survives a
 * frame boundary; it is invalidated by a change in its key (layout, the
 * reference set, program bytes or pipeline-relevant state) and, for the fetch
 * plan, by a new guest-memory resolver. The fetch memo holds no guest payload:
 * vertex bytes are always re-read. The counters are for the unit tests and
 * the offline A/B; they are cumulative until the next init. */
int  rsx_draw_engine_walker_fast_enabled(void);
typedef struct rsx_walker_fast_stats {
    unsigned long long fetch_hits, fetch_misses;
    unsigned long long pipeline_hits, pipeline_misses;
    unsigned long long remap_hits, remap_misses;
} rsx_walker_fast_stats;
void rsx_draw_engine_walker_fast_stats(rsx_walker_fast_stats* out);
/* ---- write colour buffers (WCB): render targets the guest reads ---------
 *
 * PS3RECOMP_RSX_WCB selects how rendered targets reach guest memory. See
 * docs/wcb-async.md for the design and when each mode is safe.
 *
 *   0, off       never (default)
 *   1, sync      every dirty target copied at every sync point, each a full
 *                GPU stall (the original behaviour)
 *   2, ondemand  at a sync point a dirty target's guest pages are protected;
 *                it is copied only when the CPU or an SPU first touches them
 *   3, async     ondemand, plus: a target the guest has read recently gets
 *                its copy queued on the GPU at the sync point, so the read
 *                only waits for that copy (and gets the bytes as of the sync
 *                point, as sync mode would)
 *
 * PS3RECOMP_RSX_WCB_STATS=N prints the counters every N frames (1 = every
 * frame; any other nonzero value up to 1000 is the window). */
#define RSX_WCB_OFF      0
#define RSX_WCB_SYNC     1
#define RSX_WCB_ONDEMAND 2
#define RSX_WCB_ASYNC    3

int  rsx_draw_engine_wcb_mode(void);
/* Override the environment (tests, or a port that knows its title). */
void rsx_draw_engine_wcb_set_mode(int mode);
/* Owner thread: copy the targets other threads are waiting for. The FIFO
 * walker calls it between methods; the idle wait calls it too. Cheap when
 * nothing is waiting. */
void rsx_draw_engine_wcb_service(void);
/* How a faulting thread wakes the owner thread out of its idle wait. */
void rsx_draw_engine_wcb_set_wake(void (*wake)(void));

typedef struct rsx_draw_engine_wcb_stats {
    unsigned long long marked_dirty;    /* targets that became dirty after a copy  */
    unsigned long long armed;           /* targets whose pages were protected       */
    unsigned long long copied;          /* targets copied to guest memory, any path */
    unsigned long long bytes;           /* bytes those copies wrote                 */
    unsigned long long sync_copies;     /*   ... at a sync point (sync mode, or a
                                         *   target whose pages could not be armed) */
    unsigned long long demand_copies;   /*   ... because the guest touched them     */
    unsigned long long async_issued;    /* copies queued on the GPU                 */
    unsigned long long async_fetched;   /*   ... whose bytes the guest got          */
    unsigned long long async_discarded; /*   ... superseded or dropped unread       */
    unsigned long long skipped;         /* copies avoided: guest copy still valid   */
    unsigned long long rect_copies;     /*   ... of the copies, rectangle-only      */
    unsigned long long batches;         /* batched request resolves (one submission) */
    unsigned long long batched_copies;  /*   ... copies those batches carried       */
    unsigned long long faults;          /* guest faults on armed pages              */
    unsigned long long waits;           /* guest-thread waits for a copy            */
    unsigned long long wait_ns;         /*   ... total                              */
    unsigned long long wait_max_ns;     /*   ... longest                            */
    unsigned long long timeouts;        /* waits given up on (stale bytes read)     */
    unsigned long long copy_ns;         /* engine-thread time spent copying         */
} rsx_draw_engine_wcb_stats;

/* Cumulative since the engine came up. */
void rsx_draw_engine_wcb_stats_get(rsx_draw_engine_wcb_stats* out);

/* --- test hooks ---------------------------------------------------------- */

/* Draws that ran the guest's OWN programs in the last presented frame. A draw
 * through the built-in pair is not one of them. */
u32  rsx_draw_engine_guest_draws(void);

/* The centre pixel of the surface the last flip presented, as A8R8G8B8 --
 * the convention the RSX clear colour arrives in, so a host can compare the
 * two. Zero when nothing has been presented or the surface is not an
 * 8-bit-per-channel one. */
u32  rsx_draw_engine_readback_center(void);

/* ---- pieces the engine exposes for its own tests ------------------------ */
/*
 * These are the neutral parts the unit tests in libs/video/tests exercise
 * without a GPU: the surface set's keying, the texture cache's revalidation
 * and eviction, the pipeline key's stability, and restart-cut expansion.
 */

/* Decode the NV4097 render state out of the register file. */
void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* out);

/* Fold the structural render state into a pipeline key. The alpha REFERENCE
 * and everything else that lives in a constant buffer are deliberately left
 * out, so a title changing constants per draw does not multiply pipelines --
 * rsx_live_draw.c's ld_hash_structural_render_state (3148-3187). */
u64  rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash);

/* How many indices the topology expansion produces for `source_refs`
 * references cut at `cuts`, and the indices themselves. `indices` must have
 * room for the count. Triangle strips alternate winding per segment; a quad
 * strip pairs its vertices from each segment's own start, and a polygon is a
 * fan around each segment's own first vertex. */
u32  rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                          const u32* cuts, u32 cut_count);
void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DRAW_ENGINE_H */
