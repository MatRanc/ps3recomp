/*
 * Unit test for RSX Draw Engine Render-Target Readback (WCB) to Guest Memory.
 * Verifies that dirty offscreen render targets are flushed back to guest memory
 * upon synchronization, while display buffers and clean surfaces are preserved.
 */
#define main engine_retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; return 0; }

static unsigned checks, failures;
#define T(c, msg) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static u32 s_readback_calls = 0;
static u32 s_last_readback_surface = 0;
static const u8 k_test_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };

static void test_readback_hook(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                               void* out, u32 pitch)
{
    (void)u; (void)x; (void)y;
    s_readback_calls++;
    s_last_readback_surface = s;
    for (u32 r = 0; r < h; r++) {
        for (u32 c = 0; c < w; c++) {
            u8* p = (u8*)out + (size_t)r * pitch + c * 4u;
            memcpy(p, k_test_pixel, 4);
        }
    }
}

int main(void)
{
    vm_base = (u8*)calloc(1, GUEST_BYTES);
    ppu_vm_size = GUEST_BYTES;

    rsx_draw_backend wcb_backend = g_stub_backend;
    wcb_backend.readback = test_readback_hook;

    engine_up();
    rsx_draw_engine_set_backend(&wcb_backend);

    /* 1. Set up an offscreen render target at offset 0x40000 */
    const u32 offscreen_off = 0x40000u;
    m(M_SURFACE_FORMAT, 0x148); /* ARGB8 */
    m(M_COLOR_A_PITCH, 1024);
    m(M_COLOR_A_OFFSET, offscreen_off);
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);

    /* Draw a triangle to the offscreen target */
    draw_triangle();

    u32 slot = eng_current_surface();
    T(slot != ENG_INVALID, "offscreen surface slot allocated");
    T(g.surfaces[slot].guest_dirty == 1, "surface marked guest_dirty after draw");
    T(s_readback_calls == 0, "no readback issued before sync");

    /* Verify guest memory does NOT yet contain test pixels */
    u8* guest_ptr = vm_base + GUEST_LOCAL_EA + offscreen_off;
    T(memcmp(guest_ptr, k_test_pixel, 4) != 0, "guest memory empty before flush");

    /* 2. Flush dirty surfaces to guest */
    rsx_draw_engine_flush_dirty_to_guest();

    T(s_readback_calls == 1, "readback invoked exactly once during flush");
    T(s_last_readback_surface == g.surfaces[slot].handle, "readback targeted offscreen surface");
    T(g.surfaces[slot].guest_dirty == 0, "guest_dirty cleared after flush");

    /* Check that guest memory was populated with rendered pixels (converted to guest ARGB8) */
    /* With k_test_pixel = {11, 22, 33, 44} (R,G,B,A), guest ARGB8 stores A,R,G,B = 44,11,22,33 */
    T(guest_ptr[0] == 0x44 && guest_ptr[1] == 0x11 && guest_ptr[2] == 0x22 && guest_ptr[3] == 0x33,
      "guest memory correctly holds flushed texels");

    /* 3. Redundant flush should be a no-op */
    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 1, "redundant flush is zero-cost no-op");

    /* 4. Display buffer check: display targets are presented to swapchain, not read back */
    const u32 display_off = 0x80000u;
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, display_off, 1024, 256, 256);
    m(M_COLOR_A_OFFSET, display_off);
    draw_triangle();

    u32 disp_slot = eng_current_surface();
    T(disp_slot != ENG_INVALID, "display surface slot valid");
    T(g.surfaces[disp_slot].guest_dirty == 1, "display target marked dirty after draw");

    rsx_draw_engine_flush_dirty_to_guest();
    T(s_readback_calls == 1, "display scanout targets are not flushed by WCB");
    T(g.surfaces[disp_slot].guest_dirty == 1, "display target dirty state retained for presentation");

    engine_down();
    free(vm_base);

    printf("test_color_readback_guest: checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
