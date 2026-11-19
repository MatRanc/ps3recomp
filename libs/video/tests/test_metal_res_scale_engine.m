/* PS3_RES_SCALE through the real draw engine and the Metal backend, on the
 * GPU, at 1, 1.5, 2 and 3. No title data.
 *
 *   - A fragment program that writes WPOS into a float target: what the guest
 *     reads back at pixel (x, y) is that pixel's own window position (the
 *     position of the target's pixel under its centre, divided back), under
 *     each SET_SHADER_WINDOW convention: origin TOP or BOTTOM, pixel centres
 *     HALF or INTEGER. At 1 that is x + 0.5 exactly, as it always was.
 *   - A scissored draw and a scissored clear touch the guest's rectangle and
 *     nothing else, whatever the scale.
 *
 * Needs glslang and spirv-cross, like test_metal_depth_bounds (same sources
 * and libraries; not registered with CMake):
 *   libs/video/tests/test_metal_res_scale_engine.m libs/video/rsx_tiling.c
 *   libs/video/rsx_shader_msl.cpp libs/video/rsx_dispatch.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 *   -fobjc-arc -I libs/video -I include, PS3RECOMP_HAVE_MSL_TRANSLATION=1,
 *   -framework Metal -framework QuartzCore -framework AppKit -Wl,-dead_strip
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include <math.h>

u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
float cellAvconfExt_display_gamma(void) { return 1.0f; }
/* The backend's own (non-engine) draw path, which this test never takes. */
u32 cellGcmResolveOffset(u32 offset) { return offset; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }

static unsigned checks, failures;
#define CHECK(x, ...) do { checks++; if (x) { printf("[PASS] " __VA_ARGS__); printf("\n"); } \
                           else { failures++; printf("[FAIL] " __VA_ARGS__); printf("\n"); } } while (0)

static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

#define VB 0x10000u
enum { N = 64 };

/* Two triangles over the whole target, one colour. */
static void full_rect(float r, float gr, float b)
{
    const float c[4] = { r, gr, b, 1 };
    const float p[6][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,-1}, {1,1}, {-1,1} };
    for (u32 i = 0; i < 6; i++) {
        const float v[12] = { p[i][0], p[i][1], 0.5f, 1, c[0], c[1], c[2], c[3], 0, 0, 0, 1 };
        for (u32 k = 0; k < 12; k++) be_float(vm_base + VB + (i * 12 + k) * 4, v[k]);
    }
    method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 5u << 24); method(0x1808, 0);
}

/* The state every case shares: a 64x64 colour target of `color_format` and a
 * Z24S8 zeta, a pass-through vertex program, depth test off. */
static int setup(u32 color_format, u32 bytes_per_pixel)
{
    if (rsx_draw_engine_init(N, N) != 0) { CHECK(0, "engine init"); return -1; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0198, 0xFEED0000);                 /* CONTEXT_DMA_ZETA: local */
    method(0x0210, 0x40000);                    /* COLOR_A_OFFSET */
    method(0x020C, N * bytes_per_pixel);        /* COLOR_A_PITCH */
    method(0x0214, 0xC0000);                    /* ZETA_OFFSET */
    method(0x022C, N * 4);                      /* ZETA_PITCH */
    method(0x0200, (u32)N << 16);
    method(0x0204, (u32)N << 16);
    method(0x0208, color_format | (2u << 5) | (1u << 8));   /* colour, Z24S8, linear */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80000000u | VB);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80000000u | (VB + 16));
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80000000u | (VB + 32));
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    method(0x1D60, 64);                         /* SHADER_CONTROL: 32-bit exports */
    method(0x0A6C, 0x207);                      /* DEPTH_FUNC ALWAYS */
    method(0x0A74, 0);                          /* DEPTH_TEST_ENABLE off */
    method(0x0A70, 0);                          /* DEPTH_MASK off */
    return 0;
}

/* The window position a guest pixel reads back at scale k: the position of
 * the target's pixel under its centre, in the guest's pixels. */
static float wpos_of(u32 v, float k) { return (floorf(((float)v + 0.5f) * k) + 0.5f) / k; }

/* MOV r0, WPOS into a W32Z32Y32X32 float target; `window` is
 * SET_SHADER_WINDOW (height | origin << 12 | pixel centres << 16). */
static void wpos_case(float k, u32 window, const char* what)
{
    if (setup(0x0C, 16) != 0) return;
    rsx_test_fp_instr(vm_base + 0x60000,
        RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(0) | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | RSX_TEST_FP_EXEC_ALWAYS);
    method(0x08E4, 0x60002);
    method(0x1D88, window);
    method(0x1D94, 0xF3);                       /* clear */
    full_rect(1, 1, 1);
    s_engine_backend.submit_and_wait(NULL, 0);
    static float px[N * N * 4];
    memset(px, 0, sizeof px);
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, N, N, px, N * 16);
    const int bottom = (window >> 12) & 0xF, integer = (window >> 16) & 0xF;
    const float bias = integer ? -0.5f : 0.0f;
    unsigned bad = 0; float worst = 0.0f; u32 wx = 0, wy = 0;
    for (u32 y = 0; y < N; y++)
        for (u32 x = 0; x < N; x++) {
            const float* q = px + (y * N + x) * 4;
            const float ex = wpos_of(x, k) + bias;
            const float ey = (bottom ? (float)(window & 0xFFFu) - wpos_of(y, k) : wpos_of(y, k)) + bias;
            const float d = fmaxf(fabsf(q[0] - ex), fabsf(q[1] - ey));
            if (d > worst) { worst = d; wx = x; wy = y; }
            bad += d > 1e-3f;
        }
    const float* q = px + (10 * N + 3) * 4;
    CHECK(!bad, "scale %.2f, %s: WPOS is the guest's (pixel (3,10) reads %.4f, %.4f; worst error %.5f at (%u,%u))",
          (double)k, what, (double)q[0], (double)q[1], (double)worst, wx, wy);
    if (k == 1.0f)
        CHECK(!bottom && !integer ? (q[0] == 3.5f && q[1] == 10.5f) : 1, "scale 1, %s: exactly the pixel centre", what);
    rsx_draw_engine_shutdown();
}

/* Red everywhere, then green under a scissor (as a draw, or as a clear):
 * green inside the guest's rectangle and red outside it, to the pixel. */
static void scissor_case(float k, int as_clear)
{
    if (setup(8, 4) != 0) return;               /* A8R8G8B8 */
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    method(0x08E4, 0x60002);
    method(0x08C0, 0u | ((u32)N << 16));        /* SCISSOR_HORIZONTAL: all */
    method(0x08C4, 0u | ((u32)N << 16));
    method(0x1D90, 0xFFFF0000u);                /* COLOR_CLEAR_VALUE: red */
    method(0x1D94, 0xF3);
    full_rect(1, 0, 0);
    enum { SX = 9, SY = 5, SW = 21, SH = 14 };
    method(0x08C0, (u32)SX | ((u32)SW << 16));
    method(0x08C4, (u32)SY | ((u32)SH << 16));
    if (as_clear) { method(0x1D90, 0xFF00FF00u); method(0x1D94, 0xF0); }   /* green, colour only */
    else full_rect(0, 1, 0);
    s_engine_backend.submit_and_wait(NULL, 0);
    static u8 px[N * N * 4];
    memset(px, 0, sizeof px);
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, N, N, px, N * 4);
    unsigned bad = 0, inside_n = 0;
    for (u32 y = 0; y < N; y++)
        for (u32 x = 0; x < N; x++) {
            const u8* q = px + (y * N + x) * 4;
            const int inside = x >= SX && x < SX + SW && y >= SY && y < SY + SH;
            const int green = q[1] > 200 && q[0] < 50, red = q[0] > 200 && q[1] < 50;
            inside_n += green;
            bad += inside ? !green : !red;
        }
    CHECK(!bad && inside_n == SW * SH, "scale %.2f, scissored %s: %u green pixels (want %u), %u wrong",
          (double)k, as_clear ? "clear" : "draw", inside_n, (unsigned)(SW * SH), bad);
    rsx_draw_engine_shutdown();
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        s_headless = 1; s_guest_shaders = 1;
        vm_base = calloc(1, 1u << 21); ppu_vm_size = 1u << 21;
        rsx_draw_engine_set_backend(&s_engine_backend);

        static const float scales[] = { 1.0f, 1.5f, 2.0f, 3.0f };
        for (u32 i = 0; i < sizeof scales / sizeof scales[0]; i++) {
            const float k = scales[i];
            /* what rsx_metal_backend_init does with PS3_RES_SCALE */
            s_res_scale = k;
            rsx_fp_set_wpos_scale(k);
            wpos_case(k, 0, "origin top");
            wpos_case(k, (u32)N | (1u << 12), "origin bottom");
            wpos_case(k, (u32)N | (1u << 12) | (1u << 16), "origin bottom, integer centres");
            scissor_case(k, 0);
            scissor_case(k, 1);
        }
        s_res_scale = 1.0f;
        rsx_fp_set_wpos_scale(1.0f);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
