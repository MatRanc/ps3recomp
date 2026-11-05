/*
 * ps3recomp - the draw engine's texture binding memo (sink_bind_textures)
 *
 * A unit whose registers repeat within a frame and that names a plain guest
 * texture reuses the handle and sampler it got earlier in the frame. These
 * checks show the memo is taken, and that it is dropped when a colour target
 * is created at the address, when the cached texture is rewritten and its
 * handle replaced, when the registers change, and at the next frame.
 * Synthetic addresses only; no GPU.
 *
 *   gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *       -I libs/video/tests -o /tmp/test_tex_memo libs/video/tests/test_tex_memo.c \
 *       libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c \
 *       libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c \
 *       libs/video/rsx_fp_decompiler.c -lm
 */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

static void up_plain(void)
{
    engine_up();
    memset(vm_base + TEX_OFFSET, 0x11, 4 * 4 * 4);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    printf("-- the memo is taken\n");
    up_plain();
    const u32 first = stub.bound_tex[0];
    CHECK(stub.n_texture_create == 1 && s_tmemo[0].handle == first,
          "a plain guest texture is remembered (%u)", s_tmemo[0].handle);
    /* Force the probe to re-hash without telling the memo: a probe would now
     * see the new bytes and upload, the memo answers from earlier this frame. */
    memset(vm_base + TEX_OFFSET, 0x33, 4 * 4 * 4);
    g.textures[s_tmemo[0].entry].last_hash_frame = g.frames - 1u;
    const int uploads = stub.n_texture_upload;
    draw_triangle();
    CHECK(stub.n_texture_upload == uploads && stub.bound_tex[0] == first,
          "a repeat bind in the frame is answered by the memo");
    s_texmemo_on = 0;
    draw_triangle();
    CHECK(stub.n_texture_upload == uploads + 1,
          "with the memo off the same bind probes the cache");
    s_texmemo_on = 1;
    engine_down();

    printf("-- a target created at the address\n");
    up_plain();
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 surface = stub.last_created_color;
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    CHECK(stub.bound_tex[0] == surface,
          "the next bind samples the new target, not the remembered upload (%u, want %u)",
          stub.bound_tex[0], surface);
    engine_down();

    printf("-- target bloom\n");
    up_plain();
    s_tbloom_on = 1;
    s_tbloom_on = 1;                     /* engine_up resets the cached mode */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 bsurface = stub.last_created_color;
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    CHECK(stub.bound_tex[0] == bsurface,
          "a target created at the address is still found with the bloom on (%u, want %u)",
          stub.bound_tex[0], bsurface);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000u);
    draw_triangle();
    CHECK(stub.n_texture_create == 2 && stub.bound_tex[0] != bsurface,
          "a plain address still binds its guest texture with the bloom on");
    s_tbloom_on = 0;
    engine_down();

    printf("-- a rewritten texture\n");
    up_plain();
    const u32 old = stub.bound_tex[0];
    memset(vm_base + TEX_OFFSET, 0x44, 4 * 4 * 4);
    eng_textures_touch(RSX_LOCATION_MAIN, TEX_OFFSET, 64);
    draw_triangle();
    CHECK(stub.bound_tex[0] != old && !stub.alive[old] && stub.bound_tex[0] == s_tmemo[0].handle,
          "the replaced handle is bound and remembered, the old one is gone");
    engine_down();

    printf("-- local invalidation\n");
    s_texmemo_local = 1;
    up_plain();
    s_texmemo_local = 1;                 /* engine_up resets the cached mode */
    const u32 la = stub.bound_tex[0];
    const u32 lentry = s_tmemo[0].entry;
    const int lup = stub.n_texture_upload;
    s_tex_gen++;                          /* an upload/eviction elsewhere */
    draw_triangle();
    CHECK(stub.n_texture_upload == lup && stub.bound_tex[0] == la,
          "an unrelated upload generation bump keeps the local memo answer");
    g.textures[lentry].rewritten = 1;
    g.textures[lentry].last_hash_frame = g.frames - 1u;
    draw_triangle();
    CHECK(stub.n_texture_upload == lup + 1,
          "a rewritten entry is revalidated even when the memo key matches");
    s_texmemo_local = 0;
    engine_down();

    printf("-- local memo and targets elsewhere\n");
    s_texmemo_local = 1;
    up_plain();
    s_texmemo_local = 1;                 /* engine_up resets the cached mode */
    const u32 tla = stub.bound_tex[0];
    const int tlup = stub.n_texture_upload;
    s_target_gen++;                       /* a target created at another address */
    draw_triangle();
    CHECK(stub.n_texture_upload == tlup && stub.bound_tex[0] == tla,
          "a target created elsewhere keeps the local memo answer");
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 tsurf = stub.last_created_color;
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    CHECK(stub.bound_tex[0] == tsurf && stub.bound_tex[0] != tla,
          "a target created at the address drops the remembered plain texture (%u, want %u)",
          stub.bound_tex[0], tsurf);
    s_texmemo_local = 0;
    engine_down();

    printf("-- registers and frames\n");
    up_plain();
    const u32 a = stub.bound_tex[0];
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000u);
    draw_triangle();
    const u32 b = stub.bound_tex[0];
    CHECK(b != a && stub.n_texture_create == 2, "another offset misses the memo");
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.bound_tex[0] == a && stub.n_texture_create == 2,
          "changing back finds the first texture again");
    m(M_TEX_UNIT + 0x0C, 0);
    draw_triangle();
    CHECK(stub.bound_mask == 0, "a disabled unit binds nothing");
    m(M_TEX_UNIT + 0x0C, 0x80000000u);
    rsx_draw_engine_present();
    memset(vm_base + TEX_OFFSET, 0x55, 4 * 4 * 4);
    draw_triangle();
    CHECK(stub.bound_tex[0] != a && !stub.alive[a],
          "the next frame re-checks the bytes instead of using the memo");
    engine_down();

    printf("-- texture cache cap\n");
    up_plain();
    s_texcache_on = 1;                   /* engine_up resets the cached mode */
    const int cbase = stub.n_texture_create;
    const int crel = stub.n_texture_release;
    for (u32 i = 0; i < RSX_DRAW_ENGINE_TEXTURE_CACHE + 80u; i++) {
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x2000u + i * 0x100u);
        draw_triangle();
    }
    CHECK(stub.n_texture_release == crel,
          "with the cap raised past 1024 distinct textures nothing is evicted (%d releases)",
          stub.n_texture_release - crel);
    CHECK(stub.n_texture_create == cbase + (int)RSX_DRAW_ENGINE_TEXTURE_CACHE + 80,
          "every distinct texture got its own cache entry");
    s_texcache_on = 0;
    engine_down();

    free(vm_base);
    printf("tex memo failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
