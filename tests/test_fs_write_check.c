/* Short/failed write detection shared by cellFsWrite and cellFsUtilWriteFile
 * (include/ps3emu/fs_write_check.h). */
#include "ps3emu/fs_write_check.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(ps3_fs_write_failed(4096, 4096, 0) == 0);
    assert(ps3_fs_write_failed(0, 4096, 0) == 1);          /* ENOSPC: no cursor movement */
    assert(ps3_fs_write_failed(100, 4096, 0) == 1);        /* short write */
    assert(ps3_fs_write_failed(4096, 4096, 1) == 1);       /* ferror, full length */
    assert(ps3_fs_write_failed(0, 0, 0) == 0);             /* zero-byte write is fine */
    puts("PASS: short/failed cellFs writes are detected");
    return 0;
}
