/*
 * ps3recomp - a hook for host code about to read guest memory indirectly
 *
 * Most guest-memory reads are ordinary loads and need nothing: if the page is
 * one the RSX draw engine has armed for on-demand render-target readback
 * (libs/video/rsx_wcb.h), the load faults and the fault handler makes the
 * bytes coherent before retrying it. Two kinds of access do not fault:
 *
 *   - the host kernel reading a guest buffer (write(2) from cellFsWrite):
 *     a protected page makes the system call fail with EFAULT instead;
 *   - code that asks the OS for a page's state before touching it (the SPU
 *     DMA guard, mfc_ea_range_committed), which sees PAGE_NOACCESS and gives
 *     up on the transfer.
 *
 * Those call ps3_guest_access(host, len) first. The hook is NULL unless the
 * draw engine's on-demand readback is on, so the cost is one load.
 */
#ifndef PS3EMU_GUEST_ACCESS_H
#define PS3EMU_GUEST_ACCESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in runtime/spu/spu_vm_pagemap.c; set by libs/video/rsx_wcb.c. */
extern void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len);

static inline void ps3_guest_access(const void* host, size_t len)
{
    void (*fn)(const void*, size_t) = g_ps3_guest_access_hook;
    if (fn) fn(host, len);
}

/* About to WRITE guest memory from host code (SPU/MFC PUT, a 2D transfer, a
 * file read, an HLE memcpy). Resolves any render-target readback on the
 * range, then tells texture page tracking it is dirty and opens its
 * read-only pages. One load each when neither is armed. */
extern void (*volatile g_ps3_guest_write_hook)(const void* host, size_t len);

static inline void ps3_guest_write(const void* host, size_t len)
{
    /* Nothing to do (and byte-identical behaviour to before this hook) when
     * texture page tracking is off: the write hook is only published while
     * it is on. */
    void (*w)(const void*, size_t) = g_ps3_guest_write_hook;
    if (!w) return;
    void (*a)(const void*, size_t) = g_ps3_guest_access_hook;
    if (a) a(host, len);
    w(host, len);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_GUEST_ACCESS_H */
