/* Offline unit test for the TEX_DIRTY source-range tracker used by
 * SPU_CODE_DMA_MEMO: registration protects the pages, a tracked write or a
 * direct PPU store moves the range generation, and an untracked/disabled range
 * never reports unchanged. Builds the real libs/video/rsx_wcb.c. */
#include "rsx_wcb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* vm_base;

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } \
                         else printf("ok: %s\n", m); } while (0)

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t map = page * 8;
    vm_base = mmap(NULL, map, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { perror("mmap"); return 2; }
    memset(vm_base, 0, map);

    CHECK(rsx_texdirty_set_enabled(1) == 0, "texdirty enable");

    uint64_t g0 = 0;
    CHECK(rsx_texdirty_track_range(0x2000, 0x2000, &g0) == 0, "track a source range");
    CHECK(rsx_texdirty_range_unchanged(0x2000, 0x2000, g0) == 1,
          "a freshly tracked range is unchanged");
    CHECK(rsx_texdirty_range_unchanged(0x4000, 0x2000, g0) == 0,
          "a different range is not tracked");
    uint64_t cur = 0;
    CHECK(rsx_texdirty_range_gen(0x2000, 0x2000, &cur) == 1 && cur == g0,
          "range_gen reads the published generation");

    rsx_texdirty_note_write(0x2010, 4);
    CHECK(!rsx_texdirty_range_unchanged(0x2000, 0x2000, g0),
          "an overlapping tracked write invalidates");
    uint64_t g1 = 0;
    CHECK(rsx_texdirty_track_range(0x2000, 0x2000, &g1) == 0 && g1 != g0,
          "re-tracking publishes the newer generation");
    CHECK(rsx_texdirty_range_unchanged(0x2000, 0x2000, g1),
          "the re-tracked range is unchanged at the new generation");

    rsx_texdirty_note_write(0x4000, 4);
    CHECK(rsx_texdirty_range_unchanged(0x2000, 0x2000, g1),
          "a write outside the range leaves it unchanged");

    /* Covered pages are read-only, so a direct PPU store faults and is charged
     * by the same handler. */
    *(volatile uint32_t*)(vm_base + 0x2020) = 0xDEADBEEFu;
    uint64_t g2 = 0;
    CHECK(rsx_texdirty_range_gen(0x2000, 0x2000, &g2) == 1 && g2 != g1,
          "a direct PPU store moves the generation");
    CHECK(!rsx_texdirty_range_unchanged(0x2000, 0x2000, g1),
          "the direct store invalidates the old generation");

    uint64_t g3 = 0;
    CHECK(rsx_texdirty_track_range(0x2000, 0x2000, &g3) == 0 &&
          rsx_texdirty_range_unchanged(0x2000, 0x2000, g3),
          "re-protecting restores a usable generation");

    uint64_t h0 = 0;
    CHECK(rsx_texdirty_track_range(0x6000, 0x1000, &h0) == 0, "track a second range");
    rsx_texdirty_note_write(0x2010, 4);
    CHECK(rsx_texdirty_range_unchanged(0x6000, 0x1000, h0),
          "a write to one range does not invalidate another");

    rsx_texdirty_set_enabled(0);
    CHECK(!rsx_texdirty_range_unchanged(0x6000, 0x1000, h0),
          "disable drops tracked ranges");
    CHECK(rsx_texdirty_track_range(0x6000, 0x1000, &h0) == -1,
          "tracking is unavailable while disabled");

    rsx_texdirty_shutdown();
    munmap(vm_base, map);
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASS", fails);
    return fails ? 1 : 0;
}
