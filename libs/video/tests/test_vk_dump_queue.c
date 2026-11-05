/*
 * The windowed frame dump's asynchronous writer: a submit must not touch the
 * filesystem, and the file must still end up complete and current.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vk_dump_queue.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned char* make_rgb(unsigned w, unsigned h, unsigned char v)
{
    unsigned char* p = (unsigned char*)malloc((size_t)w * h * 3);
    if (p) memset(p, v, (size_t)w * h * 3);
    return p;
}

static int read_ppm(const char* path, unsigned* w, unsigned* h, unsigned char** data)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char magic[3] = { 0 };
    unsigned mw = 0, mh = 0, maxv = 0;
    if (fscanf(f, "%2s %u %u %u", magic, &mw, &mh, &maxv) != 4 || strcmp(magic, "P6") || maxv != 255) {
        fclose(f);
        return -1;
    }
    fgetc(f);
    size_t n = (size_t)mw * mh * 3;
    unsigned char* p = (unsigned char*)malloc(n ? n : 1);
    if (!p || fread(p, 1, n, f) != n) {
        free(p);
        fclose(f);
        return -1;
    }
    fclose(f);
    *w = mw; *h = mh; *data = p;
    return 0;
}

int main(void)
{
    char dir[] = "/tmp/vk_dump_queue_XXXXXX";
    char path[512];
    unsigned w, h;
    unsigned char* px;

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/frame.ppm", dir);

    CHECK(vk_dump_queue_init(path) == 0);
    CHECK(vk_dump_queue_init(path) == 0);   /* idempotent */

    CHECK(vk_dump_queue_submit(make_rgb(8, 4, 0x11), 8u * 4u * 3u, 8, 4) == 0);
    CHECK(vk_dump_queue_submit(make_rgb(8, 4, 0x22), 8u * 4u * 3u, 8, 4) == 0);

    for (int i = 0; i < 200 && vk_dump_queue_writes() < 1; i++) usleep(10000);

    CHECK(vk_dump_queue_writes() >= 1);
    CHECK(read_ppm(path, &w, &h, &px) == 0);
    CHECK(w == 8 && h == 4);
    CHECK(px[0] == 0x11 || px[0] == 0x22);   /* one of the submitted frames */
    free(px);

    /* A rejected submit keeps ownership with the caller (never touched). */
    vk_dump_queue_shutdown();
    unsigned char* own = make_rgb(2, 2, 0x33);
    CHECK(vk_dump_queue_submit(own, 2u * 2u * 3u, 2, 2) == -1);
    free(own);

    /* Re-init works after shutdown and the last frame lands complete. */
    CHECK(vk_dump_queue_init(path) == 0);
    CHECK(vk_dump_queue_submit(make_rgb(2, 2, 0x44), 2u * 2u * 3u, 2, 2) == 0);
    vk_dump_queue_shutdown();
    CHECK(read_ppm(path, &w, &h, &px) == 0);
    CHECK(w == 2 && h == 2);
    CHECK(px[0] == 0x44);
    free(px);

    snprintf(path, sizeof path, "%s/frame.ppm", dir);
    remove(path);
    rmdir(dir);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("test_vk_dump_queue: PASS\n");
    return 0;
}
