/* Short/failed write detection shared by cellFsWrite and cellFsUtilWriteFile
 * (include/ps3emu/fs_write_check.h). */
#include "ps3emu/fs_write_check.h"
#include <assert.h>
#include <stdio.h>

/* The live control value, which must win over the environment. */
static int g_live_fs_write_check;
int ps3_live_ctl_get(int key)
{
    (void)key;
    return g_live_fs_write_check;
}

int main(void)
{
    assert(ps3_fs_write_failed(4096, 4096, 0) == 0);
    assert(ps3_fs_write_failed(0, 4096, 0) == 1);          /* ENOSPC: no cursor movement */
    assert(ps3_fs_write_failed(100, 4096, 0) == 1);        /* short write */
    assert(ps3_fs_write_failed(4096, 4096, 1) == 1);       /* ferror, full length */
    assert(ps3_fs_write_failed(0, 0, 0) == 0);             /* zero-byte write is fine */

    /* cellFsUtility used its own one-off getenv and ignored a live flip. */
    setenv("PS3RECOMP_FS_WRITE_CHECK", "1", 1);
    g_live_fs_write_check = 0;
    assert(fs_write_check_on() == 0);
    g_live_fs_write_check = 1;
    assert(fs_write_check_on() == 1);
    puts("PASS: short/failed cellFs writes are detected; the live switch wins");
    return 0;
}
