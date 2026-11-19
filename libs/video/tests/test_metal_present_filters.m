/* The draw engine's present pass with the picture filters PS3_VIGNETTE,
 * PS3_GRAIN, PS3_LUT / PS3_LUT_PRESET and PS3_LUT_STRENGTH: that with all of
 * them off the present is the plain one and an exact copy, that the filter
 * shader at neutral values is one too and agrees with the plain present on
 * PS3_VIBRANCE and PS3_CONTRAST, and what each filter does to a small
 * synthetic display buffer; and PS3_UPSCALE=metalfx up to a larger drawable.
 * (PS3_AA, PS3_SHARPEN, PS3_VIBRANCE and PS3_CONTRAST on their own:
 * test_metal_present_post.m.) Needs a Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_present_filters.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_present_filters && /tmp/test_metal_present_filters
 */
#include "../rsx_metal_backend.m"
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

enum { W = 32, H = 32 };
typedef unsigned char px4[4];          /* BGRA8 */

static id<MTLTexture> tex(MTLTextureUsage usage)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    return t;
}

/* One present of `in`; the result in `out`. */
static void present(const px4* in, px4* out)
{
    id<MTLTexture> src = tex(MTLTextureUsageShaderRead);
    id<MTLTexture> dst = tex(MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in bytesPerRow:W * 4];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_blit_to_display(cb, src, dst);
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    [dst getBytes:out bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("test_metal_present_filters: no Metal device, skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        assert(eng_init(NULL, W, H) == 0);
        static px4 in[W * H], out[W * H], ref[W * H];

        /* Read from an empty environment: all off, the plain present. */
        unsetenv("PS3_AA"); unsetenv("PS3_SHARPEN"); unsetenv("PS3_VIBRANCE"); unsetenv("PS3_CONTRAST");
        unsetenv("PS3_VIGNETTE"); unsetenv("PS3_GRAIN"); unsetenv("PS3_LUT"); unsetenv("PS3_LUT_PRESET");
        unsetenv("PS3_LUT_STRENGTH"); unsetenv("PS3_UPSCALE");
        eng_post_options();
        assert(s_eng_post_read && !s_eng_post_on && !s_eng_lut && !s_eng_post_lib);
        assert(!s_eng_fxaa && !s_eng_smaa && s_eng_sharpen == 0.0f && s_eng_vibrance == 0.0f && s_eng_contrast == 1.0f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                px4* q = &in[y * W + x];       /* BGRA: a dull, a vivid and a grey third */
                if (x < 11)      { (*q)[0] = 100; (*q)[1] = 120; (*q)[2] = 140; }
                else if (x < 22) { (*q)[0] = 20;  (*q)[1] = 40;  (*q)[2] = 230; }
                else             { (*q)[0] = (*q)[1] = (*q)[2] = (unsigned char)(y * 8); }
                (*q)[3] = 0xC0;
            }
        present(in, out);
        assert(s_eng_blit_pso_post == 0 && !s_eng_post_lib && !memcmp(in, out, sizeof in));

        /* The filter shader at neutral values (it is chosen only when a
         * filter is on, but must then not touch what is not asked for). */
        s_eng_post_on = 1;
        present(in, out);
        assert(s_eng_blit_pso_post == 1 && s_eng_post_lib && !s_eng_post_failed);
        for (int i = 0; i < W * H; i++)
            for (int k = 0; k < 4; k++) assert(abs((int)out[i][k] - (int)in[i][k]) <= 1);

        /* Vibrance, contrast and sharpening are the plain present's: the
         * same picture from either shader, to rounding. */
        s_eng_vibrance = 0.6f; s_eng_contrast = 1.3f; s_eng_sharpen = 0.5f;
        s_eng_post_on = 0;
        present(in, ref);
        assert(s_eng_blit_pso_post == 0 && memcmp(in, ref, sizeof in));
        s_eng_post_on = 1;
        present(in, out);
        assert(s_eng_blit_pso_post == 1);
        for (int i = 0; i < W * H; i++)
            for (int k = 0; k < 4; k++) assert(abs((int)out[i][k] - (int)ref[i][k]) <= 1);
        s_eng_vibrance = 0.0f; s_eng_contrast = 1.0f; s_eng_sharpen = 0.0f;

        /* Vignette: corners darker, the centre as it was. */
        s_eng_vignette = 1.0f;
        present(in, out);
        printf("vignette 1: corner %u (was %u), centre %u (was %u)\n",
               out[31 * W + 31][0], in[31 * W + 31][0], out[16 * W + 16][0], in[16 * W + 16][0]);
        assert(out[31 * W + 31][0] < in[31 * W + 31][0]);
        assert(abs((int)out[16 * W + 16][0] - (int)in[16 * W + 16][0]) <= 1);
        s_eng_vignette = 0.0f;

        /* Grain: some pixels move, none far, and the next frame differs. */
        s_eng_grain = 1.0f;
        static px4 out2[W * H];
        present(in, out);
        present(in, out2);
        { int moved = 0, maxd = 0;
          for (int i = 0; i < W * H; i++) {
              const int d = abs((int)out[i][1] - (int)in[i][1]);
              moved += d > 0; if (d > maxd) maxd = d;
          }
          printf("grain 1: %d of %d pixels moved, at most %d\n", moved, W * H, maxd);
          assert(moved > W * H / 4 && maxd <= 16 && memcmp(out, out2, sizeof out)); }
        s_eng_grain = 0.0f;

        /* The values come from the environment, clamped. */
        setenv("PS3_VIGNETTE", "0.4", 1); setenv("PS3_GRAIN", "7", 1);
        s_eng_post_read = 0;
        eng_post_options();
        assert(s_eng_post_on && fabsf(s_eng_vignette - 0.4f) < 1e-6f && s_eng_grain == 1.0f && !s_eng_lut);
        unsetenv("PS3_VIGNETTE"); unsetenv("PS3_GRAIN");

        /* A LUT preset, through the same sampling as a PS3_LUT file. Warm:
         * blue down on the grey column; strength 0 is no change. */
        setenv("PS3_LUT_PRESET", "warm", 1);
        s_eng_post_read = 0;
        eng_post_options();
        assert(s_eng_post_on && s_eng_lut && s_eng_lut_size == 32.0f && s_eng_vignette == 0.0f && s_eng_grain == 0.0f);
        present(in, out);
        printf("LUT warm: grey 128 -> B %u G %u R %u\n", out[16 * W + 26][0], out[16 * W + 26][1], out[16 * W + 26][2]);
        assert(out[16 * W + 26][0] + 6 < in[16 * W + 26][0] && out[16 * W + 26][2] >= in[16 * W + 26][2]);
        s_eng_lut_strength = 0.0f;
        present(in, out);
        for (int i = 0; i < W * H; i++)
            for (int k = 0; k < 4; k++) assert(abs((int)out[i][k] - (int)in[i][k]) <= 1);
        unsetenv("PS3_LUT_PRESET");

        /* An identity LUT file in ReShade's layout (16 tiles of 16x16) changes
         * nothing beyond rounding; a wrongly shaped one is refused. */
        { char dir[] = "/tmp/lut_XXXXXX";
          const char* tmp = getenv("TMPDIR");
          char tmpl[600];
          snprintf(tmpl, sizeof tmpl, "%s/lut_XXXXXX", tmp && *tmp ? tmp : "/tmp");
          char* d = mkdtemp(tmpl);
          if (!d) d = mkdtemp(dir);
          assert(d);
          enum { N = 16 };
          NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
              pixelsWide:N * N pixelsHigh:N bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
              colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
              bytesPerRow:N * N * 4 bitsPerPixel:32];
          for (int y = 0; y < N; y++)
              for (int x = 0; x < N * N; x++) {
                  unsigned char* q = rep.bitmapData + (size_t)y * N * N * 4 + (size_t)x * 4;
                  q[0] = (unsigned char)((x % N) * 255 / (N - 1));
                  q[1] = (unsigned char)(y * 255 / (N - 1));
                  q[2] = (unsigned char)((x / N) * 255 / (N - 1));
                  q[3] = 255;
              }
          char path[700];
          snprintf(path, sizeof path, "%s/id.png", d);
          [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
          setenv("PS3_LUT", path, 1);
          setenv("PS3_LUT_STRENGTH", "1", 1);
          s_eng_post_read = 0;
          eng_post_options();
          assert(s_eng_post_on && s_eng_lut && s_eng_lut_size == 16.0f);
          present(in, out);
          int worst = 0;
          for (int i = 0; i < W * H; i++)
              for (int k = 0; k < 3; k++) { const int dd = abs((int)out[i][k] - (int)in[i][k]); if (dd > worst) worst = dd; }
          printf("identity LUT file: worst difference %d\n", worst);
          assert(worst <= 3);
          snprintf(path, sizeof path, "%s/bad.png", d);
          NSBitmapImageRep* bad = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
              pixelsWide:20 pixelsHigh:16 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
              colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
              bytesPerRow:20 * 4 bitsPerPixel:32];
          [[bad representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
          setenv("PS3_LUT", path, 1);
          s_eng_post_read = 0;
          eng_post_options();
          assert(!s_eng_lut && !s_eng_post_on);
          unsetenv("PS3_LUT"); unsetenv("PS3_LUT_STRENGTH");
          char cmd[800]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", d);
          if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", d); }

        /* Back to all off: the plain present again, an exact copy. */
        s_eng_post_read = 0;
        eng_post_options();
        present(in, out);
        assert(!s_eng_post_on && s_eng_blit_pso_post == 0 && !memcmp(in, out, sizeof in));

        /* ---- PS3_UPSCALE=metalfx: up to a larger drawable ---- */
        { s_upscale = 1; __atomic_store_n(&s_upscale_on, 1, __ATOMIC_RELEASE);
          for (int i = 0; i < W * H; i++) { in[i][0] = in[i][1] = in[i][2] = 90; in[i][3] = 0xFF; }
          id<MTLTexture> src = tex(MTLTextureUsageShaderRead);
          [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in bytesPerRow:W * 4];
          /* a drawable twice the size and wider than the picture: the picture
           * is fitted, 2W x 2H in the middle, black bars left and right */
          enum { DW = 3 * W, DH = 2 * H };
          MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:DW height:DH mipmapped:NO];
          dd.usage = MTLTextureUsageRenderTarget;
          dd.storageMode = MTLStorageModeShared;
          id<MTLTexture> dst = [s_dev newTextureWithDescriptor:dd];
          id<MTLCommandBuffer> cb = [s_queue commandBuffer];
          eng_blit_to_display(cb, src, dst);
          [cb commit];
          [cb waitUntilCompleted];
          assert(cb.status == MTLCommandBufferStatusCompleted);
          static px4 big[DW * DH];
          [dst getBytes:big bytesPerRow:DW * 4 fromRegion:MTLRegionMake2D(0, 0, DW, DH) mipmapLevel:0];
          int off = 0, bars = 0;
          for (int y = 0; y < DH; y++)
              for (int x = 0; x < DW; x++) {
                  const unsigned char* q = big[y * DW + x];
                  if (x >= W / 2 && x < W / 2 + 2 * W) off |= abs((int)q[0] - 90) > 2;
                  else bars |= q[0] | q[1] | q[2];
              }
          printf("MetalFX: %s, flat 90 stays flat: %s, bars black: %s\n",
                 __atomic_load_n(&s_upscale_on, __ATOMIC_ACQUIRE) ? (s_eng_fx_out ? "scaled" : "not used") : "gave up",
                 off ? "no" : "yes", bars ? "no" : "yes");
          assert(!off && !bars);
          /* On a Mac with MetalFX it is used, at the picture's size in the drawable. */
          if (__atomic_load_n(&s_upscale_on, __ATOMIC_ACQUIRE))
              assert(s_eng_fx_out && [s_eng_fx_out width] == 2 * W && [s_eng_fx_out height] == 2 * H);
          /* A drawable that is not larger: MetalFX is left out. */
          s_eng_fx_out = nil; s_eng_fx_scaler = nil;
          present(in, out);
          assert(!s_eng_fx_out && !memcmp(in, out, sizeof in));
          s_upscale = 0; __atomic_store_n(&s_upscale_on, 0, __ATOMIC_RELEASE); }

        puts("test_metal_present_filters: ok");
    }
    return 0;
}
