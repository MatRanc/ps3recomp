/* Committed-page bitmap: one bit per 64 KB page across the 4 GB guest space
 * (8 KB total). The MFC DMA guard (mfc_ea_range_committed in spu_dma.h) tests
 * these bits instead of calling VirtualQuery per transfer -- the syscall version
 * measured 94 CPU-s in a 50 s run.
 *
 * Storage lives here, in the runtime, because spu_dma.h is a header inlined into
 * whichever TU handles MFC (the runtime's spu_channels.c, or a per-game override
 * of it). It used to be defined in lbp/main.cpp, which made every other port fail
 * to link. Titles whose demand-commit fault handler seeds the bitmap just declare
 * `extern uint8_t g_vm_page_bitmap[65536/8]` and set bits; the guard treats it as
 * a self-healing cache (miss -> one VirtualQuery -> remember), so a port that
 * never seeds it is correct, just slower on first touch per page. */

unsigned char g_vm_page_bitmap[65536 / 8];

/* ps3emu/guest_access.h: NULL until the RSX draw engine arms on-demand
 * render-target readback (libs/video/rsx_wcb.c). Lives here, beside the page
 * bitmap, because the SPU DMA guard that consults it is inlined into whichever
 * TU handles MFC. */
#include <stddef.h>
void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len);
