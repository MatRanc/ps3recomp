/*
 * ps3recomp - VS constants by range (bind_vs_constants_range)
 *
 * A backend that takes a range gets only the vp_c[] rows the guest vertex
 * program can read, plus the viewport tail; a program with a relative
 * constant read, the built-in program and PS3RECOMP_ENG_VPC_FULL get the
 * whole block. Synthetic programs only; no GPU.
 *
 *   gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *       -I libs/video/tests -o /tmp/test_vpc_range libs/video/tests/test_vpc_range.c \
 *       libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c \
 *       libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c \
 *       libs/video/rsx_fp_decompiler.c -lm
 */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

static struct { int ranged, full; u32 first, count, total; float row5[4], tail[8]; } vs;
static void range_vs(void* u, const void* rows, u32 first, u32 count,
                     const void* tail, u32 tail_bytes, u32 total)
{
    (void)u;
    vs.ranged++; vs.first = first; vs.count = count; vs.total = total;
    memcpy(vs.row5, (const u8*)rows + 5 * 16, 16);
    if (tail_bytes == sizeof vs.tail) memcpy(vs.tail, tail, sizeof vs.tail);
}
static void full_vs(void* u, const void* d, u32 n) { vs.full++; stub_bind_vs(u, d, n); }

/* MOV o[out] = c[c] at p: the test MOV with src0 retyped as a constant. */
static void mov_const(u8* p, u32 c, u32 out, int end, int relative)
{
    rsx_test_vp_mov_out(p, 0, RSX_TEST_VP_SWZ_IDENT, out, end);
    u32 d1, d2, d3;
    memcpy(&d1, p + 4, 4); memcpy(&d2, p + 8, 4); memcpy(&d3, p + 12, 4);
    d1 |= (c & 0x3FFu) << 12;
    d2 = (d2 & ~(3u << 23)) | (3u << 23);
    if (relative) d3 |= 2u;
    rsx_test_put_le32(p + 4, d1); rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}
static void load_vp(int relative)
{
    u8 vp[32];
    mov_const(vp, 5, 0, 0, 0);
    mov_const(vp + 16, 9, 7, 1, relative);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 8; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    for (u32 i = 0; i < 12; i++) { u8* p = vm_base + VTX_OFFSET + i * 4; p[0] = 0x3F; p[1] = 0x80; }

    rsx_draw_backend be = g_stub_backend;
    be.bind_vs_constants = full_vs;
    be.bind_vs_constants_range = range_vs;
    engine_up_with(&be);
    rsx_test_fp_mov_r0_col0(vm_base + TEX_OFFSET, RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2u);
    m(M_VP_UPLOAD_CONST_ID, 5);
    const float c5[4] = { 1.5f, 2.5f, 3.5f, 4.5f };
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, &c5[i], 4); m(M_VP_UPLOAD_CONST + i * 4, w); }

    load_vp(0);
    memset(&vs, 0, sizeof vs);
    draw_triangle();
    CHECK(stub.n_draw == 1 && vs.ranged == 1 && vs.full == 0,
          "a fixed-index program binds by range (%d ranged, %d full)", vs.ranged, vs.full);
    CHECK(vs.first == 5 && vs.count == 5 && vs.total == ENG_VP_CB_BYTES,
          "the range is c[5]..c[9] of the whole block (%u+%u of %u)", vs.first, vs.count, vs.total);
    CHECK(!memcmp(vs.row5, c5, 16) && vs.tail[0] != 0.0f,
          "the rows are the constants and the viewport tail comes with them");

    s_vpc_force_full = 1;
    memset(&vs, 0, sizeof vs);
    draw_triangle();
    CHECK(vs.full == 1 && vs.ranged == 0, "PS3RECOMP_ENG_VPC_FULL binds the whole block");
    s_vpc_force_full = 0;

    load_vp(1);
    memset(&vs, 0, sizeof vs);
    draw_triangle();
    CHECK(vs.full == 1 && vs.ranged == 0, "a relative constant read binds the whole block");

    m(0x8E4, 0);    /* no fragment program: the built-in pair */
    memset(&vs, 0, sizeof vs);
    draw_triangle();
    CHECK(vs.full == 1 && vs.ranged == 0, "the built-in program binds the whole block");
    engine_down();

    free(vm_base);
    printf("vpc range failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
