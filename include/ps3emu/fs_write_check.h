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

static inline int ps3_fs_write_failed(size_t written, uint64_t requested, int ferror_flag)
{
    return ferror_flag != 0 || written != (size_t)requested;
}

#endif /* PS3RECOMP_FS_WRITE_CHECK_H */
