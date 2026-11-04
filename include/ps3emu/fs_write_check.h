/*
 * ps3recomp - short/failed write detection shared by the cellFs write paths.
 *
 * A cache/install worker loops while its nwrite cursor advances. Returning
 * CELL_OK with a zero cursor on a read-only/full volume leaves it with no
 * progress signal, so it loops forever. Both runtime/ppu/ppu_fs.cpp and
 * libs/filesystem/cellFs.c use this one predicate.
 */
#ifndef PS3RECOMP_FS_WRITE_CHECK_H
#define PS3RECOMP_FS_WRITE_CHECK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../libs/video/ps3_live_ctl.h"   /* PS3RECOMP_FS_WRITE_CHECK */

/* ferror() is sticky: once a write fails, every later write on that stream
 * reports the old error even after it succeeds. That made one full-disk write
 * turn every later cellFsWrite on the same fd into EIO. Clear it first, so a
 * write that works is reported as one. */
static inline void ps3_fs_write_begin(FILE* f)
{
    if (f) clearerr(f);
}

/* The error-reporting switch, shared by every cellFs write path. A live
 * control value wins over the environment, so a benchmark can flip it. */
static inline int fs_write_check_on(void)
{
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_FS_WRITE_CHECK) != 0;
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3RECOMP_FS_WRITE_CHECK"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

static inline int ps3_fs_write_failed(size_t written, uint64_t requested, int ferror_flag)
{
    return ferror_flag != 0 || written != (size_t)requested;
}

#endif /* PS3RECOMP_FS_WRITE_CHECK_H */
