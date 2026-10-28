/*
 * Exhaustive bit-exact verification for RSX vertex SIMD decoders:
 * - Every uint16 half float value (0..65535) against scalar rsx_compact_be_f16
 *   (including denormals, infinities, NaNs, and normal floats)
 * - Every uint8 unorm8 value (0..255) against scalar u / 255.0f
 * - Every int16 snorm16 value (-32768..32767) against scalar s / 32767.0f
 * - Every uint8 uint8 value (0..255) against scalar (float)u
 * - Every int16 sint16 value (-32768..32767) against scalar (float)s
 *
 * Tested across component counts n = 1, 2, 3, 4 through rsx_vertex_fetch_many.
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include "rsx_dispatch.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK_EXACT(cond, ...) do { \
    if (!(cond)) { \
        fails++; \
        if (fails <= 20) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } \
} while (0)

static void test_exhaustive_half16(void)
{
    enum { N = 65536 };
    static u8 input[N * 8];
    static rsx_vertex_ref refs[N];
    static float out_many[N * 4];
    static float out_ref[4];

    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
        u16 h0 = (u16)i;
        u16 h1 = (u16)(i ^ 0x5555);
        u16 h2 = (u16)(i ^ 0xAAAA);
        u16 h3 = (u16)(~i);
        input[i * 8 + 0] = (u8)(h0 >> 8); input[i * 8 + 1] = (u8)(h0 & 0xFF);
        input[i * 8 + 2] = (u8)(h1 >> 8); input[i * 8 + 3] = (u8)(h1 & 0xFF);
        input[i * 8 + 4] = (u8)(h2 >> 8); input[i * 8 + 5] = (u8)(h2 & 0xFF);
        input[i * 8 + 6] = (u8)(h3 >> 8); input[i * 8 + 7] = (u8)(h3 & 0xFF);
    }

    for (u32 n = 1; n <= 4; n++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 0);
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_HALF;
        f->desc.size = n;
        f->desc.stride = 8;
        f->desc.frequency = 0;
        f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_HALF, n);
        f->stride = 8;
        f->stable_base = input;
        f->stable_first = 0;
        f->stable_last = N - 1;
        f->default_value[0] = f->default_value[1] = f->default_value[2] = 0.0f;
        f->default_value[3] = 1.0f;

        memset(out_many, 0, sizeof out_many);
        int ok = rsx_vertex_fetch_many(&plan, refs, N, (u8*)out_many);
        CHECK_EXACT(ok, "fetch_many failed for half16 n=%u", n);

        for (u32 i = 0; i < N; i++) {
            rsx_vertex_decode_element(RSX_VTX_TYPE_HALF, n, input + i * 8, out_ref);
            float* actual = out_many + i * 4;
            if (memcmp(actual, out_ref, 16) != 0) {
                u32 u_act[4], u_exp[4];
                memcpy(u_act, actual, 16);
                memcpy(u_exp, out_ref, 16);
                CHECK_EXACT(0, "half16 mismatch at i=0x%04x n=%u: got [0x%08x, 0x%08x, 0x%08x, 0x%08x] exp [0x%08x, 0x%08x, 0x%08x, 0x%08x]",
                            i, n, u_act[0], u_act[1], u_act[2], u_act[3], u_exp[0], u_exp[1], u_exp[2], u_exp[3]);
            }
        }
    }
}

static void test_exhaustive_unorm8(void)
{
    enum { N = 256 };
    static u8 input[N * 4];
    static rsx_vertex_ref refs[N];
    static float out_many[N * 4];
    static float out_ref[4];

    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
        input[i * 4 + 0] = (u8)i;
        input[i * 4 + 1] = (u8)(255 - i);
        input[i * 4 + 2] = (u8)(i ^ 0x55);
        input[i * 4 + 3] = (u8)(i ^ 0xAA);
    }

    for (u32 n = 1; n <= 4; n++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 0);
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_UNORM8;
        f->desc.size = n;
        f->desc.stride = 4;
        f->desc.frequency = 0;
        f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_UNORM8, n);
        f->stride = 4;
        f->stable_base = input;
        f->stable_first = 0;
        f->stable_last = N - 1;
        f->default_value[0] = f->default_value[1] = f->default_value[2] = 0.0f;
        f->default_value[3] = 1.0f;

        memset(out_many, 0, sizeof out_many);
        int ok = rsx_vertex_fetch_many(&plan, refs, N, (u8*)out_many);
        CHECK_EXACT(ok, "fetch_many failed for unorm8 n=%u", n);

        for (u32 i = 0; i < N; i++) {
            rsx_vertex_decode_element(RSX_VTX_TYPE_UNORM8, n, input + i * 4, out_ref);
            float* actual = out_many + i * 4;
            if (memcmp(actual, out_ref, 16) != 0) {
                u32 u_act[4], u_exp[4];
                memcpy(u_act, actual, 16);
                memcpy(u_exp, out_ref, 16);
                CHECK_EXACT(0, "unorm8 mismatch at i=%u n=%u: got 0x%08x exp 0x%08x", i, n, u_act[0], u_exp[0]);
            }
        }
    }
}

static void test_exhaustive_uint8(void)
{
    enum { N = 256 };
    static u8 input[N * 4];
    static rsx_vertex_ref refs[N];
    static float out_many[N * 4];
    static float out_ref[4];

    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
        input[i * 4 + 0] = (u8)i;
        input[i * 4 + 1] = (u8)(255 - i);
        input[i * 4 + 2] = (u8)(i ^ 0x33);
        input[i * 4 + 3] = (u8)(i ^ 0xCC);
    }

    for (u32 n = 1; n <= 4; n++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 0);
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_UINT8;
        f->desc.size = n;
        f->desc.stride = 4;
        f->desc.frequency = 0;
        f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_UINT8, n);
        f->stride = 4;
        f->stable_base = input;
        f->stable_first = 0;
        f->stable_last = N - 1;
        f->default_value[0] = f->default_value[1] = f->default_value[2] = 0.0f;
        f->default_value[3] = 1.0f;

        memset(out_many, 0, sizeof out_many);
        int ok = rsx_vertex_fetch_many(&plan, refs, N, (u8*)out_many);
        CHECK_EXACT(ok, "fetch_many failed for uint8 n=%u", n);

        for (u32 i = 0; i < N; i++) {
            rsx_vertex_decode_element(RSX_VTX_TYPE_UINT8, n, input + i * 4, out_ref);
            float* actual = out_many + i * 4;
            if (memcmp(actual, out_ref, 16) != 0) {
                u32 u_act[4], u_exp[4];
                memcpy(u_act, actual, 16);
                memcpy(u_exp, out_ref, 16);
                CHECK_EXACT(0, "uint8 mismatch at i=%u n=%u: got 0x%08x exp 0x%08x", i, n, u_act[0], u_exp[0]);
            }
        }
    }
}

static void test_exhaustive_snorm16(void)
{
    enum { N = 65536 };
    static u8 input[N * 8];
    static rsx_vertex_ref refs[N];
    static float out_many[N * 4];
    static float out_ref[4];

    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
        s16 s0 = (s16)i;
        s16 s1 = (s16)-s0;
        s16 s2 = (s16)(s0 ^ 0x5555);
        s16 s3 = (s16)(s0 ^ 0xAAAA);
        input[i * 8 + 0] = (u8)(s0 >> 8); input[i * 8 + 1] = (u8)(s0 & 0xFF);
        input[i * 8 + 2] = (u8)(s1 >> 8); input[i * 8 + 3] = (u8)(s1 & 0xFF);
        input[i * 8 + 4] = (u8)(s2 >> 8); input[i * 8 + 5] = (u8)(s2 & 0xFF);
        input[i * 8 + 6] = (u8)(s3 >> 8); input[i * 8 + 7] = (u8)(s3 & 0xFF);
    }

    for (u32 n = 1; n <= 4; n++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 0);
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_SNORM16;
        f->desc.size = n;
        f->desc.stride = 8;
        f->desc.frequency = 0;
        f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_SNORM16, n);
        f->stride = 8;
        f->stable_base = input;
        f->stable_first = 0;
        f->stable_last = N - 1;
        f->default_value[0] = f->default_value[1] = f->default_value[2] = 0.0f;
        f->default_value[3] = 1.0f;

        memset(out_many, 0, sizeof out_many);
        int ok = rsx_vertex_fetch_many(&plan, refs, N, (u8*)out_many);
        CHECK_EXACT(ok, "fetch_many failed for snorm16 n=%u", n);

        for (u32 i = 0; i < N; i++) {
            rsx_vertex_decode_element(RSX_VTX_TYPE_SNORM16, n, input + i * 8, out_ref);
            float* actual = out_many + i * 4;
            if (memcmp(actual, out_ref, 16) != 0) {
                u32 u_act[4], u_exp[4];
                memcpy(u_act, actual, 16);
                memcpy(u_exp, out_ref, 16);
                CHECK_EXACT(0, "snorm16 mismatch at i=%d n=%u: got [0x%08x] exp [0x%08x]", (s16)i, n, u_act[0], u_exp[0]);
            }
        }
    }
}

static void test_exhaustive_sint16(void)
{
    enum { N = 65536 };
    static u8 input[N * 8];
    static rsx_vertex_ref refs[N];
    static float out_many[N * 4];
    static float out_ref[4];

    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
        s16 s0 = (s16)i;
        s16 s1 = (s16)-s0;
        s16 s2 = (s16)(s0 ^ 0x3333);
        s16 s3 = (s16)(s0 ^ 0xCCCC);
        input[i * 8 + 0] = (u8)(s0 >> 8); input[i * 8 + 1] = (u8)(s0 & 0xFF);
        input[i * 8 + 2] = (u8)(s1 >> 8); input[i * 8 + 3] = (u8)(s1 & 0xFF);
        input[i * 8 + 4] = (u8)(s2 >> 8); input[i * 8 + 5] = (u8)(s2 & 0xFF);
        input[i * 8 + 6] = (u8)(s3 >> 8); input[i * 8 + 7] = (u8)(s3 & 0xFF);
    }

    for (u32 n = 1; n <= 4; n++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 0);
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_SINT16;
        f->desc.size = n;
        f->desc.stride = 8;
        f->desc.frequency = 0;
        f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_SINT16, n);
        f->stride = 8;
        f->stable_base = input;
        f->stable_first = 0;
        f->stable_last = N - 1;
        f->default_value[0] = f->default_value[1] = f->default_value[2] = 0.0f;
        f->default_value[3] = 1.0f;

        memset(out_many, 0, sizeof out_many);
        int ok = rsx_vertex_fetch_many(&plan, refs, N, (u8*)out_many);
        CHECK_EXACT(ok, "fetch_many failed for sint16 n=%u", n);

        for (u32 i = 0; i < N; i++) {
            rsx_vertex_decode_element(RSX_VTX_TYPE_SINT16, n, input + i * 8, out_ref);
            float* actual = out_many + i * 4;
            if (memcmp(actual, out_ref, 16) != 0) {
                u32 u_act[4], u_exp[4];
                memcpy(u_act, actual, 16);
                memcpy(u_exp, out_ref, 16);
                CHECK_EXACT(0, "sint16 mismatch at i=%d n=%u: got [0x%08x] exp [0x%08x]", (s16)i, n, u_act[0], u_exp[0]);
            }
        }
    }
}

int main(void)
{
    printf("Running exhaustive SIMD vertex decode checks...\n");
    test_exhaustive_half16();
    printf("  half16 (65,536 values x 4 sizes = 262,144 conversions): %s\n", fails ? "FAILED" : "OK");

    test_exhaustive_unorm8();
    printf("  unorm8 (256 values x 4 sizes = 1,024 conversions):       %s\n", fails ? "FAILED" : "OK");

    test_exhaustive_uint8();
    printf("  uint8  (256 values x 4 sizes = 1,024 conversions):       %s\n", fails ? "FAILED" : "OK");

    test_exhaustive_snorm16();
    printf("  snorm16 (65,536 values x 4 sizes = 262,144 conversions): %s\n", fails ? "FAILED" : "OK");

    test_exhaustive_sint16();
    printf("  sint16  (65,536 values x 4 sizes = 262,144 conversions): %s\n", fails ? "FAILED" : "OK");

    if (fails == 0) {
        printf("All exhaustive checks PASSED (total 788,480 conversions bit-exact)!\n");
        return 0;
    } else {
        printf("Exhaustive checks FAILED with %d mismatch(es)\n", fails);
        return 1;
    }
}
