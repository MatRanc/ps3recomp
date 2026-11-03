#include "spu_context.h"
#include "spu_interp.h"
#include "spu_overlay_residency.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint8_t* vm_base;
static int dummy_calls;
static void dummy_fn(spu_context* c) { (void)c; dummy_calls++; }
void spu_begin_image(int id);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size);

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok: %s\n", m); } while (0)

int main(void)
{
    static uint8_t mem[0x4000];
    vm_base = mem;
    static spu_context ctx;
    uint8_t code[0x100];
    for (int i = 0; i < 0x100; i++) code[i] = (uint8_t)(i * 7 + 3);
    memcpy(mem + 0x1000, code, sizeof code);

    spu_begin_image(1);
    spu_register_function(0x4000, dummy_fn);

    spu_overlay_code_segment seg = { 0x4000, 0x100, code };
    CHECK(spu_overlay_register_verified_region(0x1000, 0x4000, 0x100, 1, &seg, 1) == 1,
          "register verified region");

    spu_context_init(&ctx, 0);
    ctx.image_id = 1;
    memcpy(ctx.ls + 0x4000, mem + 0x1000, 0x100);
    spu_overlay_note_get(&ctx, 0x1000, ctx.ls + 0x4000, 0x100);
    CHECK(spu_lifted_lookup(&ctx, 0x4000) == dummy_fn, "lifted lookup verifies the load");

    CHECK(spu_overlay_code_get_noop(&ctx, 0x1000, 0x4000, 0x100, mem + 0x1000) == 1,
          "identical reload is a no-op");
    CHECK(spu_overlay_code_get_noop(&ctx, 0x1010, 0x4010, 0x40, mem + 0x1010) == 1,
          "identical partial reload is a no-op");
    CHECK(spu_overlay_code_get_noop(&ctx, 0x1004, 0x4000, 0x100, mem + 0x1000) == 0,
          "wrong source offset is not a no-op");
    CHECK(spu_overlay_code_get_noop(&ctx, 0x1000, 0x4100, 0x40, mem + 0x1100) == 0,
          "outside the span is not a no-op");

    mem[0x1000 + 9] ^= 0xFF;
    CHECK(spu_overlay_code_get_noop(&ctx, 0x1000, 0x4000, 0x100, mem + 0x1000) == 0,
          "changed source is not a no-op");
    mem[0x1000 + 9] ^= 0xFF;

    ctx.ls[0x4000 + 4] ^= 0xFF;
    CHECK(spu_overlay_code_get_noop(&ctx, 0x1000, 0x4000, 0x100, mem + 0x1000) == 0,
          "changed local store is not a no-op");
    ctx.ls[0x4000 + 4] ^= 0xFF;

    printf("%s (%d failures)\n", fails ? "FAILED" : "PASS", fails);
    return fails ? 1 : 0;
}

/* Link stubs for the runtime pieces this test does not exercise. */
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid, g_ydkj_real_spurs_ea;
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
void ydkj_wake_all_event_flags(void) { }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a;(void)b; }
