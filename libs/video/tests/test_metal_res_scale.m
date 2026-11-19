/* PS3_RES_SCALE in the Metal backend: the knob's spellings; targets made at
 * the scaled size (a seeded colour target filled nearest-neighbour); views and
 * copies that inherit it; viewports, scissors and rectangle clears that
 * follow; readbacks, colour and depth, that hand the guest its own pixels back
 * (also at 1.5); a depth target read as A8R8G8B8 or DEPTH16 converted at the
 * guest's size from the scaled depth; and the frame dump's box filter. Needs a
 * Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_res_scale.m libs/video/rsx_texture_layout.c \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_res_scale && /tmp/test_metal_res_scale
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <math.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }\n"
/* every 2x2 block one value: the guest pixel it stands for */
"struct D { float d [[depth(any)]]; }; fragment D fs(V v [[stage_in]]) {"
" return {0.125f + floor(v.p.x * 0.5f) * 0.125f + floor(v.p.y * 0.5f) * 0.0625f}; }\n";
static float expected(u32 x, u32 y) { return .125f + x * .125f + y * .0625f; }

/* A guest-sized texture's bytes, bpp each. */
static void texture_bytes(id<MTLTexture> t, u32 w, u32 h, u32 bpp, u8* out)
{
    id<MTLBuffer> buf = [s_dev newBufferWithLength:(NSUInteger)w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [c blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
destinationBytesPerRow:(NSUInteger)w * bpp destinationBytesPerImage:(NSUInteger)w * h * bpp];
    [b endEncoding]; [c commit]; [c waitUntilCompleted];
    assert(c.status == MTLCommandBufferStatusCompleted);
    memcpy(out, [buf contents], (size_t)w * h * bpp);
}

int main(void)
{
    @autoreleasepool {
        /* the knob */
        assert(res_scale_from(NULL) == 1.0f && res_scale_from("") == 1.0f);
        assert(res_scale_from("2") == 2.0f && res_scale_from("1.5") == 1.5f && res_scale_from("2x") == 2.0f);
        assert(res_scale_from("1080p") == 1.5f && res_scale_from("1440P") == 2.0f && res_scale_from("4k") == 3.0f);
        assert(res_scale_from("720p") == 1.0f && res_scale_from("1.6") == 1.5f);   /* quarter steps */
        assert(res_scale_from("2.5") == 2.5f && res_scale_from("3") == 3.0f && res_scale_from("4") == 4.0f);
        assert(res_scale_from("5") == 1.0f && res_scale_from("0.5") == 1.0f && res_scale_from("big") == 1.0f);

        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("test_metal_res_scale: no Metal device, skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];

        /* at 1 (the default), nothing is scaled and the old paths are taken */
        assert(s_res_scale == 1.0f && res_px(1280) == 1280 && res_px(7) == 7);
        assert(eng_init(NULL, 4, 3) == 0);
        u8 seed[4 * 3 * 4];
        for (u32 i = 0; i < 12; i++) { seed[i * 4] = (u8)(i * 20); seed[i * 4 + 1] = (u8)(255 - i); seed[i * 4 + 2] = 7; seed[i * 4 + 3] = 255; }
        { const u32 plain = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
          assert(plain && !eng_obj_is_scaled(plain) && [eng_obj(plain) width] == 4 && [eng_obj(plain) height] == 3);
          const u32 pz = eng_depth_target_create(NULL, 4, 3);
          assert(pz && !eng_obj_is_scaled(pz) && [eng_obj(pz) width] == 4);
          assert(!eng_obj_is_scaled(eng_surface_view(NULL, plain, 0xAAE4, 0x85)));
          const u32 pc = eng_color_snapshot(NULL, plain);
          assert(pc && !eng_obj_is_scaled(pc));
          const u32 ps = eng_depth_snapshot(NULL, pz, 4, 3);
          assert(ps && !eng_obj_is_scaled(ps) && [eng_obj(ps) width] == 4);
          eng_set_viewport(NULL, 1.0f, 2.0f, 4.0f, 3.0f);
          assert(s_eng_pending.vp[0] == 1.0f && s_eng_pending.vp[1] == 2.0f &&
                 s_eng_pending.vp[2] == 4.0f && s_eng_pending.vp[3] == 3.0f);
          eng_set_scissor(NULL, 1, 1, 3, 2);
          assert(s_eng_pending.sc[0] == 1 && s_eng_pending.sc[1] == 1 && s_eng_pending.sc[2] == 3 && s_eng_pending.sc[3] == 2);
          const float red[4] = { 1, 0, 0, 1 };
          eng_clear_color_rect(NULL, plain, red, 1, 1, 2, 1);
          const EngRecord* r = &s_eng_rec[s_eng_rec_count - 1];
          assert(r->sc[0] == 1 && r->sc[1] == 1 && r->sc[2] == 2 && r->sc[3] == 1);
          eng_encode_and_commit(nil);
          assert(!s_eng_encoding_failed);
          u8 back1[4 * 3 * 4];
          eng_readback(NULL, plain, 0, 0, 4, 3, back1, 16);
          for (u32 y = 0; y < 3; y++)
              for (u32 x = 0; x < 4; x++) {
                  const u8* q = back1 + (y * 4 + x) * 4;
                  if (y == 1 && (x == 1 || x == 2)) assert(q[0] == 255 && q[1] == 0 && q[2] == 0 && q[3] == 255);
                  else assert(!memcmp(q, seed + (y * 4 + x) * 4, 4));
              } }

        s_res_scale = 2.0f;
        assert(res_px(4) == 8 && res_px(3) == 6 && res_px(0) == 0);

        /* a seeded colour target: 8x6, each guest pixel a 2x2 block */
        const u32 color = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
        assert(color && eng_obj_is_scaled(color));
        assert([eng_obj(color) width] == 8 && [eng_obj(color) height] == 6);
        u8 big[8 * 6 * 4];
        [eng_obj(color) getBytes:big bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 6) mipmapLevel:0];
        assert(!memcmp(big + (3 * 8 + 5) * 4, seed + (1 * 4 + 2) * 4, 4));   /* (5,3) -> guest (2,1) */
        /* read back at the guest's size: the seed, exactly */
        u8 back[4 * 3 * 4];
        memset(back, 0xCD, sizeof back);
        eng_readback(NULL, color, 0, 0, 4, 3, back, 16);
        assert(!memcmp(back, seed, sizeof seed));
        u8 roi[2 * 2 * 4 + 4];
        memset(roi, 0xCD, sizeof roi);
        eng_readback(NULL, color, 1, 1, 2, 2, roi, 12);                     /* padded rows */
        assert(!memcmp(roi, seed + (1 * 4 + 1) * 4, 8) && !memcmp(roi + 12, seed + (2 * 4 + 1) * 4, 8));
        assert(roi[8] == 0xCD);

        /* a view and a copy of it are scaled too; a guest texture is not */
        const u32 view = eng_surface_view(NULL, color, 0xAAE4, 0x85);
        assert(view && eng_obj_is_scaled(view));
        const u32 copy = eng_color_snapshot(NULL, color);
        assert(copy && eng_obj_is_scaled(copy) && [eng_obj(copy) width] == 8);
        const u32 tex = eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, 1, 1, 0xAAE4, 0x85);
        assert(tex && !eng_obj_is_scaled(tex) && [eng_obj(tex) width] == 4);

        /* viewport and scissor follow the targets */
        eng_set_viewport(NULL, 0.0f, 0.0f, 4.0f, 3.0f);
        assert(s_eng_pending.vp[2] == 8.0f && s_eng_pending.vp[3] == 6.0f);
        eng_set_scissor(NULL, 1, 1, 3, 2);
        assert(s_eng_pending.sc[0] == 2 && s_eng_pending.sc[1] == 2 && s_eng_pending.sc[2] == 6 && s_eng_pending.sc[3] == 4);
        eng_set_scissor(NULL, 0, 0, 0, 0);          /* "no scissor" stays that */
        assert(s_eng_pending.sc[2] == 0 && s_eng_pending.sc[3] == 0);

        /* a rectangle clear covers the guest's rectangle, in the target's
         * pixels, and nothing else (the copy queued above runs first) */
        { const float green[4] = { 0, 1, 0, 1 };
          eng_clear_color_rect(NULL, color, green, 1, 1, 2, 1);
          const EngRecord* r = &s_eng_rec[s_eng_rec_count - 1];
          assert(r->kind == ENG_REC_CLEAR_COLOR && r->sc[0] == 2 && r->sc[1] == 2 && r->sc[2] == 4 && r->sc[3] == 2);
          eng_encode_and_commit(nil);
          assert(!s_eng_encoding_failed);
          eng_readback(NULL, color, 0, 0, 4, 3, back, 16);
          for (u32 y = 0; y < 3; y++)
              for (u32 x = 0; x < 4; x++) {
                  const u8* q = back + (y * 4 + x) * 4;
                  if (y == 1 && (x == 1 || x == 2)) assert(q[0] == 0 && q[1] == 255 && q[2] == 0 && q[3] == 255);
                  else assert(!memcmp(q, seed + (y * 4 + x) * 4, 4));
              }
          [eng_obj(color) getBytes:big bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 6) mipmapLevel:0];
          assert(big[(2 * 8 + 2) * 4 + 1] == 255 && big[(3 * 8 + 5) * 4 + 1] == 255);     /* the 4x2 block */
          assert(!memcmp(big + (1 * 8 + 2) * 4, seed + (0 * 4 + 1) * 4, 4));              /* above it */
          assert(!memcmp(big + (2 * 8 + 6) * 4, seed + (1 * 4 + 3) * 4, 4));              /* right of it */
          /* the copy was taken before the clear */
          u8 cp[8 * 6 * 4];
          texture_bytes(eng_obj(copy), 8, 6, 4, cp);
          assert(!memcmp(cp + (2 * 8 + 2) * 4, seed + (1 * 4 + 1) * 4, 4)); }

        /* depth: 8x6, its snapshot 8x6, its readback the guest's 4x3 */
        const u32 depth = eng_depth_target_create(NULL, 4, 3);
        assert(depth && eng_obj_is_scaled(depth) && [eng_obj(depth) width] == 8);
        NSError* err = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(fixture) options:nil error:&err];
        assert(lib);
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.depthAttachmentPixelFormat = MTL_DEPTH_FORMAT; pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
        id<MTLRenderPipelineState> pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        assert(pso);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = eng_obj(depth); rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = eng_obj(depth); rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionAlways; dsd.depthWriteEnabled = YES;
        [e setRenderPipelineState:pso]; [e setDepthStencilState:[s_dev newDepthStencilStateWithDescriptor:dsd]];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        assert(cb.status == MTLCommandBufferStatusCompleted);
        const u32 snap = eng_depth_snapshot(NULL, depth, 4, 3);
        assert(snap && eng_obj_is_scaled(snap) && [eng_obj(snap) width] == 8 && [eng_obj(snap) height] == 6);
        /* the top-left 2x2 of the guest's: 4x4 of the target's */
        const u32 part = eng_depth_snapshot(NULL, depth, 2, 2);
        assert(part && eng_obj_is_scaled(part) && [eng_obj(part) width] == 4 && [eng_obj(part) height] == 4);
        float dz[4 * 3 + 2];
        for (int i = 0; i < 14; i++) dz[i] = -7.0f;
        assert(eng_depth_readback(NULL, depth, 0, 0, 4, 3, dz, 16) == 0);
        for (u32 y = 0; y < 3; y++)
            for (u32 x = 0; x < 4; x++) assert(fabsf(dz[y * 4 + x] - expected(x, y)) < 1e-6f);
        assert(dz[12] == -7.0f);
        float droi[3] = { -9, -9, -9 };
        assert(eng_depth_readback(NULL, depth, 2, 1, 2, 1, droi, 12) == 0);
        assert(fabsf(droi[0] - expected(2, 1)) < 1e-6f && fabsf(droi[1] - expected(3, 1)) < 1e-6f && droi[2] == -9);
        /* the partial snapshot holds the target's own top-left pixels */
        { float pz[4 * 4];
          texture_bytes(eng_obj(part), 4, 4, 4, (u8*)pz);
          for (u32 y = 0; y < 4; y++)
              for (u32 x = 0; x < 4; x++) assert(fabsf(pz[y * 4 + x] - expected(x / 2, y / 2)) < 1e-6f); }

        /* read as A8R8G8B8 (a shadow map the game samples as colour): the
         * guest's 4x3, each texel the packed depth of its guest pixel */
        const u32 argb = eng_depth_as_color(NULL, depth, 4, 3, RSX_TEX_FMT_A8R8G8B8);
        assert(argb && !eng_obj_is_scaled(argb) && [eng_obj(argb) width] == 4 && [eng_obj(argb) height] == 3);
        /* ...and as DEPTH16, a 3x2 corner of it */
        const u32 d16 = eng_depth_as_color(NULL, depth, 3, 2, RSX_TEX_FMT_DEPTH16);
        assert(d16 && !eng_obj_is_scaled(d16) && [eng_obj(d16) width] == 3 && [eng_obj(d16) height] == 2);
        assert(eng_depth_as_color(NULL, depth, 5, 3, RSX_TEX_FMT_A8R8G8B8) == 0);   /* larger than the target */
        eng_encode_and_commit(nil);
        assert(!s_eng_encoding_failed);
        { u8 px[4 * 3 * 4];
          texture_bytes(eng_obj(argb), 4, 3, 4, px);
          for (u32 y = 0; y < 3; y++)
              for (u32 x = 0; x < 4; x++) {
                  const u32 v = (u32)floor((double)expected(x, y) * 16777215.0 + 0.5);
                  const u8* q = px + (y * 4 + x) * 4;      /* R = [15:8], G = [7:0], A = [23:16] */
                  assert(q[0] == ((v >> 8) & 0xFF) && q[1] == (v & 0xFF) && q[2] == 0 && q[3] == (v >> 16));
              }
          u16 p16[3 * 2];
          texture_bytes(eng_obj(d16), 3, 2, 2, (u8*)p16);
          for (u32 y = 0; y < 2; y++)
              for (u32 x = 0; x < 3; x++)
                  assert(p16[y * 3 + x] == (u16)floor((double)expected(x, y) * 65535.0 + 0.5)); }

        /* a rectangle clear of the depth: the guest's (1,1) 2x1, nothing else */
        eng_clear_depth_stencil_rect(NULL, depth, RSX_BE_CLEAR_DEPTH, 0.03125f, 0, 1, 1, 2, 1);
        { const EngRecord* r = &s_eng_rec[s_eng_rec_count - 1];
          assert(r->kind == ENG_REC_CLEAR_DS && r->sc[0] == 2 && r->sc[1] == 2 && r->sc[2] == 4 && r->sc[3] == 2); }
        assert(eng_depth_readback(NULL, depth, 0, 0, 4, 3, dz, 16) == 0);
        for (u32 y = 0; y < 3; y++)
            for (u32 x = 0; x < 4; x++)
                assert(fabsf(dz[y * 4 + x] - ((y == 1 && (x == 1 || x == 2)) ? 0.03125f : expected(x, y))) < 1e-6f);

        /* at 1.5 a seed goes up and comes back exactly */
        s_res_scale = 1.5f;
        assert(res_px(4) == 6 && res_px(3) == 5);
        const u32 c15 = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
        assert(c15 && eng_obj_is_scaled(c15) && [eng_obj(c15) width] == 6 && [eng_obj(c15) height] == 5);
        memset(back, 0xCD, sizeof back);
        eng_readback(NULL, c15, 0, 0, 4, 3, back, 16);
        assert(!memcmp(back, seed, sizeof seed));
        eng_set_scissor(NULL, 0, 0, 4, 3);
        assert(s_eng_pending.sc[2] == 6 && s_eng_pending.sc[3] == 5);   /* to the target's edge */
        s_res_scale = 2.0f;

        /* the frame dump's box filter: each 2x2 block averaged */
        { u8 img[4 * 2 * 4];
          for (int i = 0; i < 8; i++) { img[i * 4] = (u8)(i * 10); img[i * 4 + 1] = 100; img[i * 4 + 2] = (u8)(i < 4 ? 0 : 255); img[i * 4 + 3] = 255; }
          eng_res_box_down(img, 4, 2, 2, 1);
          assert(img[0] == 25 && img[1] == 100 && img[2] == 128 && img[4] == 45); }

        /* back at 1, nothing is scaled */
        s_res_scale = 1.0f;
        const u32 plain = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
        assert(plain && !eng_obj_is_scaled(plain) && [eng_obj(plain) width] == 4);
        eng_set_viewport(NULL, 0.0f, 0.0f, 4.0f, 3.0f);
        assert(s_eng_pending.vp[2] == 4.0f);
        puts("test_metal_res_scale: ok");
    }
    return 0;
}
