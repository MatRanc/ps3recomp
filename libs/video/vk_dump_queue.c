/*
 * ps3recomp - one-slot asynchronous writer for the windowed frame dump.
 *
 * PS3RECOMP_VK_DUMP makes every Nth present copy the readback into a PPM.
 * The write itself (a ~2.7 MB fwrite plus rename) ran on the RSX walker
 * thread; under writeback pressure on the session's filesystem it blocked the
 * walker for 0.5-0.7 s and stopped the display tick (docs/present-stalls.md).
 * This queue moves only the filesystem write off that thread. The caller keeps
 * building the packed RGB buffer (a CPU loop, no I/O); submit() takes
 * ownership of it and returns immediately.
 *
 * There is one pending slot: a newer frame replaces one the writer has not
 * started yet. The dump is a diagnostic, so dropping an older frame is the
 * right trade (it never grows memory or lags the picture).
 */
#include "vk_dump_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/posix_win32.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static pthread_t       s_thread;
static int             s_started;
static int             s_quit;
static char*           s_path;
static unsigned char*  s_pending;
static size_t          s_pending_len;
static unsigned        s_pending_w, s_pending_h;
static unsigned long long s_writes;

static void dump_write(unsigned char* rgb, size_t len, unsigned w, unsigned h)
{
    char tmp[1024];
    if (!s_path) { free(rgb); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE* f = fopen(tmp, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        fwrite(rgb, 1, len, f);
        fclose(f);
#ifdef _WIN32
        remove(s_path);
#endif
        if (rename(tmp, s_path) != 0) remove(tmp);
    }
    free(rgb);
}

static void* dump_thread(void* arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (!s_pending && !s_quit)
            pthread_cond_wait(&s_cv, &s_lock);
        if (!s_pending) {
            const int done = s_quit;
            pthread_mutex_unlock(&s_lock);
            if (done) break;
            continue;
        }
        unsigned char* rgb = s_pending;
        const size_t len = s_pending_len;
        const unsigned w = s_pending_w, h = s_pending_h;
        s_pending = NULL;
        s_pending_len = 0;
        pthread_mutex_unlock(&s_lock);

        dump_write(rgb, len, w, h);

        pthread_mutex_lock(&s_lock);
        s_writes++;
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

int vk_dump_queue_init(const char* path)
{
    if (!path || !*path) return -1;
    pthread_mutex_lock(&s_lock);
    if (s_started) {
        const int ok = s_path && strcmp(s_path, path) == 0;
        pthread_mutex_unlock(&s_lock);
        return ok ? 0 : -1;
    }
    s_path = strdup(path);
    if (!s_path) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    s_quit = 0;
    s_writes = 0;
    if (pthread_create(&s_thread, NULL, dump_thread, NULL) != 0) {
        free(s_path);
        s_path = NULL;
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    s_started = 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int vk_dump_queue_submit(unsigned char* rgb, size_t len, unsigned w, unsigned h)
{
    if (!rgb) return -1;
    pthread_mutex_lock(&s_lock);
    if (!s_started || s_quit) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    free(s_pending);
    s_pending = rgb;
    s_pending_len = len;
    s_pending_w = w;
    s_pending_h = h;
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void vk_dump_queue_shutdown(void)
{
    pthread_mutex_lock(&s_lock);
    if (!s_started) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_quit = 1;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_thread, NULL);

    pthread_mutex_lock(&s_lock);
    free(s_pending);
    s_pending = NULL;
    s_pending_len = 0;
    free(s_path);
    s_path = NULL;
    s_started = 0;
    s_quit = 0;
    pthread_mutex_unlock(&s_lock);
}

unsigned long long vk_dump_queue_writes(void)
{
    pthread_mutex_lock(&s_lock);
    const unsigned long long n = s_writes;
    pthread_mutex_unlock(&s_lock);
    return n;
}
