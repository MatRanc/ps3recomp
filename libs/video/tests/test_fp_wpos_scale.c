/* PS3_RES_SCALE in the fragment-program decompiler (rsx_fp_set_wpos_scale):
 * at 1 every program is the text it always was; at another scale WPOS is
 * divided back to the guest's window coordinates before the window origin
 * and the pixel-centre bias apply, and a comparison lookup with unnormalised
 * coordinates is taken to the scaled map's texels.
 *
 * Build, from this directory:
 *   cc -std=c11 -Wall -Wextra -I../../../include \
 *       test_fp_wpos_scale.c ../rsx_fp_decompiler.c -o test_fp_wpos_scale
 */
#include "../rsx_fp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, what) do { if (c) printf("[PASS] %s\n", what); \
                            else { printf("[FAIL] %s\n", what); g_fail++; } } while (0)

/* Store host word H so that rsx_fp_read_word(p) == H. */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define INSRC(x)    ((u32)(x) << 13)
#define TEXU(x)     ((u32)(x) << 17)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_INPUT     1u
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define CTRL_32BIT  0x40u

static char a[65536], b[65536], c[65536];

int main(void)
{
    u8 prog[16];
    u32 nk = 0;
    put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(0) | END);   /* MOV r0, WPOS */
    put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
    put_word(prog + 8, 0);
    put_word(prog + 12, 0);

    CHECK(rsx_fp_wpos_scale() == 1.0f, "the scale starts at 1");
    /* Buffered (the draw engine's form). */
    CHECK(rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, a, sizeof a, &nk) >= 0, "decompiles");
    CHECK(strstr(a, "#define _rsx_wpos(p) float4((p).x + fp_alpha.z, "
                    "(fp_alpha.y != 0.0 ? fp_alpha.y - (p).y : (p).y) + fp_alpha.z, (p).z, (p).w)\n") != NULL,
          "scale 1: the window-convention macro as it was");
    rsx_fp_set_wpos_scale(2.0f);
    CHECK(rsx_fp_wpos_scale() == 2.0f, "the scale is kept");
    rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, b, sizeof b, &nk);
    CHECK(strstr(b, "#define _rsx_wpos(p) float4((p).x * 0.5 + fp_alpha.z, "
                    "(fp_alpha.y != 0.0 ? fp_alpha.y - (p).y * 0.5 : (p).y * 0.5) + fp_alpha.z, (p).z, (p).w)\n") != NULL,
          "scale 2: x and y halved before the origin and the bias");
    CHECK(strstr(b, "_rsx_wpos(input.position)") != NULL, "scale 2: the read is still the wrapper");
    /* Nothing but the macro's line differs. */
    { const char* la = strstr(a, "#define _rsx_wpos"); const char* lb = strstr(b, "#define _rsx_wpos");
      CHECK(la && lb && (la - a) == (lb - b) && !strncmp(a, b, (size_t)(la - a)) &&
            !strcmp(strchr(la, '\n'), strchr(lb, '\n')), "scale 2: only the macro's line changed"); }
    rsx_fp_set_wpos_scale(1.5f);
    rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, b, sizeof b, &nk);
    CHECK(strstr(b, "(p).x * 0.666666667 + fp_alpha.z") != NULL, "scale 1.5: 1/1.5 to nine digits");
    /* Unbuffered. */
    rsx_fp_set_wpos_scale(2.0f);
    rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, b, sizeof b);
    CHECK(strstr(b, "#define _rsx_wpos(p) float4((p).x * 0.5, (p).y * 0.5, (p).z, (p).w)\n") != NULL,
          "scale 2, unbuffered: halved");
    /* Back at 1 (and at a value that is not a scale): byte-identical. */
    rsx_fp_set_wpos_scale(1.0f);
    rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, c, sizeof c, &nk);
    CHECK(!strcmp(a, c), "back at 1: byte-identical");
    rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, c, sizeof c);
    CHECK(strstr(c, "#define _rsx_wpos(p) (p)\n") != NULL, "back at 1, unbuffered: the identity");
    rsx_fp_set_wpos_scale(0.0f);
    CHECK(rsx_fp_wpos_scale() == 1.0f, "0 is 1");
    /* A program without WPOS never changes. */
    put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);   /* MOV r0, COL0 */
    rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, a, sizeof a, &nk);
    rsx_fp_set_wpos_scale(3.0f);
    rsx_fp_decompile_buffered_ex(prog, sizeof prog, CTRL_32BIT, 0, b, sizeof b, &nk);
    CHECK(!strcmp(a, b) && !strstr(b, "_rsx_wpos"), "no WPOS read: byte-identical at any scale");
    rsx_fp_set_wpos_scale(1.0f);

    /* A comparison lookup (TEX on unit 0, a DEPTH24_D8 map, compare LESS):
     * normalised coordinates are untouched; unnormalised ones are scaled. */
    {
        rsx_fp_shadow_desc d[16];
        memset(d, 0, sizeof d);
        put_word(prog + 0, OPC(0x17) | TEXU(0) | OUTMASK_ALL | INSRC(4) | END);   /* TEX r0, TC0, #0 */
        rsx_fp_shadow_desc_from_texture(&d[0], 0xB0, 2, 0, 1, 0x01010000, 3u << 28, 0x80000000);
        CHECK(d[0].enabled == 1 && !d[0].unnormalized, "a normalised comparison unit");
        CHECK(rsx_fp_decompile_buffered_shadow_ex(prog, sizeof prog, CTRL_32BIT, 0, d, a, sizeof a, &nk) >= 0,
              "the comparison program decompiles");
        rsx_fp_set_wpos_scale(2.0f);
        rsx_fp_decompile_buffered_shadow_ex(prog, sizeof prog, CTRL_32BIT, 0, d, b, sizeof b, &nk);
        CHECK(!strcmp(a, b), "normalised comparison: byte-identical at scale 2");
        rsx_fp_set_wpos_scale(1.0f);

        rsx_fp_shadow_desc_from_texture(&d[0], 0xB0 | 0x40, 2, 0, 1, 0x01010000, 3u << 28, 0x80000000);
        CHECK(d[0].enabled == 1 && d[0].unnormalized, "an unnormalised comparison unit");
        rsx_fp_decompile_buffered_shadow_ex(prog, sizeof prog, CTRL_32BIT, 0, d, a, sizeof a, &nk);
        CHECK(strstr(a, "    float2 uv = coordinate.xy / size;\n") != NULL, "scale 1: texels over the map's size");
        rsx_fp_set_wpos_scale(2.0f);
        rsx_fp_decompile_buffered_shadow_ex(prog, sizeof prog, CTRL_32BIT, 0, d, b, sizeof b, &nk);
        CHECK(strstr(b, "    float2 uv = coordinate.xy * 2 / size;\n") != NULL, "scale 2: guest texels to the map's");
        rsx_fp_set_wpos_scale(1.0f);
        rsx_fp_decompile_buffered_shadow_ex(prog, sizeof prog, CTRL_32BIT, 0, d, c, sizeof c, &nk);
        CHECK(!strcmp(a, c), "back at 1: byte-identical");
    }

    printf("test_fp_wpos_scale: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
