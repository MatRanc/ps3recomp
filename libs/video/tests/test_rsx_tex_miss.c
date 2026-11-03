/*
 * rsx_tex_miss: the dummy-texture census aggregates misses by reason and by
 * texture key, prints a per-frame summary with the reason counts, clears the
 * frame counters, and keeps the cumulative top-N.
 */
#include "rsx_tex_miss.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char* slurp(FILE* f, char* buf, size_t cap)
{
    size_t n;
    fflush(f);
    rewind(f);
    n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    return buf;
}

int main(void)
{
    rsx_tex_miss_set_enabled(1);
    rsx_tex_miss_reset();
    assert(rsx_tex_miss_enabled());
    assert(rsx_tex_miss_total() == 0);

    /* The same 256x256 swizzled DXT45 at one address, bound by two programs. */
    rsx_tex_miss_note(RSX_TEX_MISS_ADDRESS, 0, 0x00123400u, 0x88u, 256, 256, 0, 1, 0,
                      0xabcull, 3);
    rsx_tex_miss_note(RSX_TEX_MISS_ADDRESS, 0, 0x00123400u, 0x88u, 256, 256, 0, 1, 0,
                      0xdefull, 4);
    /* One different format, and one size rejection. */
    rsx_tex_miss_note(RSX_TEX_MISS_FORMAT, 0, 0x00200000u, 0xADu, 64, 64, 256, 1, 0,
                      0x111ull, 9);
    rsx_tex_miss_note(RSX_TEX_MISS_SIZE, 1, 0x00000100u, 0x85u, 0, 512, 0, 1, 0,
                      0x222ull, 11);
    /* A cube whose six faces are not all current is its own reason. */
    rsx_tex_miss_note(RSX_TEX_MISS_CUBE, 0, 0x059f3000u, 0x85u, 8, 8, 64, 1, 1,
                      0x333ull, 12);

    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_ADDRESS) == 2);
    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_FORMAT) == 1);
    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_SIZE) == 1);
    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_CUBE) == 1);
    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_SURFACE) == 0);
    assert(rsx_tex_miss_total() == 5);

    FILE* f = tmpfile();
    assert(f);
    assert(rsx_tex_miss_frame(f, 7) == 5);

    char out[8192];
    slurp(f, out, sizeof out);
    assert(strstr(out, "frame=7 misses=5 format=1 surface=0 address=2 size=1 cube=1 other=0 total=5"));
    /* The two identical address misses aggregate; the last program is kept. */
    assert(strstr(out, "top 1: address loc=0 addr=0x00123400 fmt=0x88 256x256 "
                       "pitch=0 mips=1 cube=0 n=2 fp=0x0000000000000def pipe=4"));
    assert(strstr(out, "top 2: format"));
    assert(strstr(out, "top 3: size"));

    /* The frame counters reset; the cumulative ones do not. */
    assert(rsx_tex_miss_frame(f, 8) == 0);
    assert(rsx_tex_miss_total() == 5);
    assert(rsx_tex_miss_run_count(RSX_TEX_MISS_ADDRESS) == 2);

    /* The shutdown flush prints the run totals and the same top list. */
    rsx_tex_miss_flush(f);
    slurp(f, out, sizeof out);
    assert(strstr(out, "end: misses=5 format=1 surface=0 address=2 size=1 cube=1 other=0"));
    assert(strstr(out, "addr=0x00123400 fmt=0x88 256x256"));

    /* A caller tag in bits 8-15 is printed as kind (the Vulkan backend puts
     * its object kind there) and the format byte is still in bits 0-7. */
    rsx_tex_miss_reset();
    rsx_tex_miss_note(RSX_TEX_MISS_OTHER, 0, 0x00000343u, (4u << 8) | 0x85u,
                      8, 8, 0, 1, 0, 0, 0);
    rsx_tex_miss_frame(f, 9);
    slurp(f, out, sizeof out);
    assert(strstr(out, "fmt=0x85 8x8 pitch=0 mips=1 cube=0 kind=4"));

    fclose(f);
    puts("rsx_tex_miss: ok");
    return 0;
}
