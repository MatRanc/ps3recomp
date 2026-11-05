/*
 * ps3recomp - the walker capture's guest-memory imaging
 *
 * A captured frame must contain every page a cold replay can ask for. The
 * crowded replay failed because a live mesh-ring vertex read was noted at a
 * narrower span than the replay resolved, and the .mem kept only the live
 * pages (engine=0 was not the capture's bug, but the dropped draws were).
 * This fixture drives the real capture begin/flip/finish path and checks the
 * two rules that fix it:
 *   - a main-memory read images its whole 1 MB IO-mapping page, so the tail
 *     of a multi-MB span is present even if the live range was narrower;
 *   - a local-memory read images only its 4 KB page (local is linear, so
 *     there is nothing to widen).
 * Synthetic addresses only; the bytes come from a private mmap of the guest
 * window and the test cleans its own files up.
 *
 *   gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *       -I libs/video/tests -o /tmp/test_walker_capture libs/video/tests/test_walker_capture.c \
 *       libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c \
 *       libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c \
 *       libs/video/rsx_fp_decompiler.c -lm
 */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

#include <unistd.h>
#include <sys/mman.h>
#include <stdarg.h>

static unsigned failures;
static void check(int ok, const char* fmt, ...)
{
    printf("[%s] ", ok ? "PASS" : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    failures += !ok;
}

typedef struct { u32 location, page, len, data_off; } mem_ent;

/* Find a page entry in the capture's .mem. Returns the file offset of its
 * 4 KB payload, or 0 when absent. */
static long mem_page_data(const char* path, u32 location, u32 page,
                          u32* pages_out, int* missing_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    u32 hdr[6];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0x50414357u) { fclose(f); return -1; }
    *pages_out = hdr[2];
    u32 n = hdr[2];
    mem_ent* ents = malloc(n ? (size_t)n * sizeof *ents : 1);
    if (!ents) { fclose(f); return -1; }
    if (n && fread(ents, sizeof *ents, n, f) != n) { free(ents); fclose(f); return -1; }
    long at = -1;
    for (u32 i = 0; i < n; i++) {
        if (ents[i].location == location && ents[i].page == page) {
            at = (long)(24 + (size_t)n * sizeof *ents + ents[i].data_off);
            break;
        }
    }
    free(ents);
    fclose(f);
    (void)missing_out;
    return at;
}

int main(void)
{
    vm_base = mmap(NULL, GUEST_BYTES, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { puts("FAIL: guest mmap"); return 2; }
    ppu_vm_size = GUEST_BYTES;
    engine_up();

    char prefix[256];
    snprintf(prefix, sizeof prefix, "/tmp/test_walker_capture_%ld", (long)getpid());
    char trig[300], path[320];
    snprintf(trig, sizeof trig, "%s.trigger", prefix);
    snprintf(path, sizeof path, "%s.mem", prefix);
    setenv("PS3RECOMP_WALKER_CAPTURE", prefix, 1);
    s_wcap = -1;

    /* Seed recognizable bytes in a main 1 MB page and a local page. The
     * read's IO page (0x00900000) is above the fixture's 8 MB IO map, so it
     * tests the per-read 1 MB expansion rather than the mapped-main pass. */
    const u32 main_off = 0x00900123u;      /* 1 MB page 0x00900000 */
    const u32 local_off = 0x00040123u;     /* 4 KB page 0x00040000 */
    memset(vm_base + 0x00900000u, 0xA5, 0x100000u);
    memset(vm_base + GUEST_LOCAL_EA + 0x00040000u, 0x5A, 0x1000u);

    /* Trigger -> flip boundary -> armed capture (the real order). */
    FILE* t = fopen(trig, "wb");
    if (t) { fputc('x', t); fclose(t); }
    check(rsx_wcap_begin_if_triggered() == 0, "trigger waits for a flip boundary");
    rsx_wcap_flip(0);
    check(rsx_wcap_begin_if_triggered() == 1, "the next drain starts the capture");

    /* The live frame reads 12 bytes. Main memory is imaged at IO-page
     * granularity; local memory at 4 KB. */
    rsx_wcap_note_read(RSX_LOCATION_MAIN, main_off, 12,
                       cellGcmResolveLocated(0, main_off));
    rsx_wcap_note_read(RSX_LOCATION_LOCAL, local_off, 12,
                       cellGcmResolveLocated(1, local_off));
    rsx_wcap_flip(1);
    rsx_wcap_drain_end();

    u32 pages = 0; int missing = 0;
    long main_first = mem_page_data(path, RSX_LOCATION_MAIN, 0x00900000u, &pages, &missing);
    long main_last  = mem_page_data(path, RSX_LOCATION_MAIN, 0x009FF000u, &pages, &missing);
    long main_out   = mem_page_data(path, RSX_LOCATION_MAIN, 0x00A00000u, &pages, &missing);
    check(main_first >= 0 && main_last >= 0,
          "a 12-byte main read images its whole 1 MB IO page (%u pages)", pages);
    check(main_out < 0, "the next 1 MB page is not imaged");
    check(main_first > 0, "the main page payload is readable");

    long mapped   = mem_page_data(path, RSX_LOCATION_MAIN, 0x00500000u, &pages, &missing);
    long unmapped = mem_page_data(path, RSX_LOCATION_MAIN, 0x00800000u, &pages, &missing);
    check(mapped >= 0 && unmapped < 0,
          "capture begin images every mapped main IO page, read or not");

    if (main_first > 0) {
        FILE* f = fopen(path, "rb");
        if (f) {
            unsigned char got = 0;
            fseek(f, main_first + (main_off & 0xFFFu), SEEK_SET);
            if (fread(&got, 1, 1, f) == 1 && got == 0xA5)
                check(1, "the imaged main page carries the guest bytes");
            else
                check(0, "the imaged main page carries the guest bytes (got 0x%02X)", got);
            fclose(f);
        }
    }

    long local_hit = mem_page_data(path, RSX_LOCATION_LOCAL, 0x00040000u, &pages, &missing);
    long local_out = mem_page_data(path, RSX_LOCATION_LOCAL, 0x00041000u, &pages, &missing);
    check(local_hit >= 0 && local_out < 0,
          "a local read images only its 4 KB page");

    snprintf(trig, sizeof trig, "%s.missing", prefix);
    FILE* m = fopen(trig, "rb");
    check(m == NULL, "no page was dropped as unbacked");
    if (m) fclose(m);

    /* Clean up the synthetic capture. */
    remove(path);
    snprintf(path, sizeof path, "%s.fifo", prefix);   remove(path);
    snprintf(path, sizeof path, "%s.state", prefix);  remove(path);
    snprintf(path, sizeof path, "%s.trigger", prefix); remove(path);

    engine_down();
    munmap(vm_base, GUEST_BYTES);
    vm_base = NULL;
    printf("[walker_capture] %s (%u failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
