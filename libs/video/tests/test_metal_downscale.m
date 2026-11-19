/* PS3_DOWNSCALE (supersampling): a picture larger than the window's drawable
 * is reduced into it with a Lanczos filter before the present, so every
 * source pixel counts: a one-pixel checkerboard three times the drawable's
 * size comes out an even grey, a white picture stays white up to its edge
 * (the filter clamps there: no dark 1-pixel frame), a picture that fits is
 * left alone, and PS3_DOWNSCALE=0 samples as before (the checkerboard then
 * comes out black and white, whichever pixel each sample lands on). Needs a
 * Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_downscale.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_downscale && /tmp/test_metal_downscale && /tmp/test_metal_downscale off
 */
#include "../rsx_metal_backend.m"
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

enum { D = 32, S = 96 };               /* the drawable, and a picture 3x its size */
typedef unsigned char px4[4];          /* BGRA8 */

static id<MTLTexture> tex(int n, MTLTextureUsage usage)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:n height:n mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    return t;
}

/* One present of an n x n picture into the D x D drawable. */
static void present(const px4* in, int n, px4* out)
{
    id<MTLTexture> src = tex(n, MTLTextureUsageShaderRead);
    id<MTLTexture> dst = tex(D, MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0 withBytes:in bytesPerRow:n * 4];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_blit_to_display(cb, src, dst);
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    [dst getBytes:out bytesPerRow:D * 4 fromRegion:MTLRegionMake2D(0, 0, D, D) mipmapLevel:0];
}

int main(int argc, char** argv)
{
    const int off = argc > 1 && !strcmp(argv[1], "off");
    @autoreleasepool {
        assert(eng_downscale_from(NULL) == 1 && eng_downscale_from("") == 1 && eng_downscale_from("lanczos") == 1);
        assert(eng_downscale_from("1") == 1 && eng_downscale_from("0") == 0 && eng_downscale_from("off") == 0);
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("test_metal_downscale: no Metal device, skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        assert(eng_init(NULL, D, D) == 0);
        unsetenv("PS3_AA"); unsetenv("PS3_SHARPEN"); unsetenv("PS3_VIBRANCE"); unsetenv("PS3_CONTRAST");
        unsetenv("PS3_VIGNETTE"); unsetenv("PS3_GRAIN"); unsetenv("PS3_LUT"); unsetenv("PS3_LUT_PRESET"); unsetenv("PS3_UPSCALE");
        if (off) setenv("PS3_DOWNSCALE", "0", 1); else unsetenv("PS3_DOWNSCALE");
        static px4 big[S * S], small[D * D], out[D * D];

        /* A one-pixel checkerboard, three times the drawable. */
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                const unsigned char v = ((x + y) & 1) ? 255 : 0;
                big[y * S + x][0] = big[y * S + x][1] = big[y * S + x][2] = v; big[y * S + x][3] = 255;
            }
        present(big, S, out);
        int lo = 255, hi = 0;
        for (int i = 0; i < D * D; i++) { if (out[i][1] < lo) lo = out[i][1]; if (out[i][1] > hi) hi = out[i][1]; }
        if (off) {
            assert(!s_eng_down_out && s_eng_down_on == 0);
            assert(lo < 60 || hi > 195);                 /* samples, not an average */
            puts("test_metal_downscale off: ok");
            return 0;
        }
        assert(s_eng_down_out && [s_eng_down_out width] == D && [s_eng_down_out height] == D);
        assert(lo >= 110 && hi <= 145);                  /* an even grey, edge to edge */

        /* White stays white, the outer row and column too. */
        memset(big, 255, sizeof big);
        present(big, S, out);
        for (int i = 0; i < D * D; i++) assert(out[i][0] >= 253 && out[i][1] >= 253 && out[i][2] >= 253);

        /* A picture the drawable's size is not touched. */
        for (int i = 0; i < D * D; i++) { small[i][0] = (unsigned char)(i * 7); small[i][1] = (unsigned char)(i * 3); small[i][2] = (unsigned char)i; small[i][3] = 255; }
        id<MTLTexture> before = s_eng_down_out;
        present(small, D, out);
        assert(s_eng_down_out == before && !memcmp(small, out, sizeof small));
        puts("test_metal_downscale: ok");
    }
    return 0;
}
