#ifndef PS3RECOMP_VK_DUMP_QUEUE_H
#define PS3RECOMP_VK_DUMP_QUEUE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the writer for `path` (idempotent; 0 on success). */
int  vk_dump_queue_init(const char* path);

/* Hand a packed RGB buffer of w*h*3 bytes to the writer. On success (0) the
 * queue owns `rgb` and the caller must not touch it; on failure (-1) the
 * caller keeps ownership and should write it itself. Never waits for the
 * filesystem: one pending frame is replaced by a newer one. */
int  vk_dump_queue_submit(unsigned char* rgb, size_t len, unsigned w, unsigned h);

/* Flush the pending frame, join the writer and free its state. */
void vk_dump_queue_shutdown(void);

/* Number of frames actually written (for diagnostics/tests). */
unsigned long long vk_dump_queue_writes(void);

#ifdef __cplusplus
}
#endif

#endif
