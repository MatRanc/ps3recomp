/*
 * ps3recomp - apply the "Better depth of field" fragment-program patch
 *
 * The title stores its close-up/replay blur as two RSX fragment-program
 * headers in the EBOOT. The patch rebuilds the program (a depth-aware
 * 17-tap spiral) in the unused tail of the last code page and repoints both
 * headers at it. This module writes those bytes into guest memory right
 * after the ELF is loaded, before the first RSX command is walked, so the
 * draw engine reads the patched program and nothing has cached the old one.
 *
 * PS3RECOMP_DOF_PATCH (default 0) switches it on. The table is generated
 * from the user's own EBOOT by tools/gen_dof_patch_table.py; fingerprints of
 * the original headers/programs are checked first, and a mismatch refuses the
 * patch instead of writing into an unexpected image.
 */
#ifndef PS3RECOMP_RSX_DOF_PATCH_H
#define PS3RECOMP_RSX_DOF_PATCH_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when PS3RECOMP_DOF_PATCH asks for the patch. */
int rsx_dof_patch_enabled(void);

/* Apply the embedded patch to guest memory.
 *   vm      : guest address space (guest virtual address == byte offset)
 *   vm_size : bytes valid at vm, or 0 for the usual fully-backed 32-bit space
 * Returns 1 when applied, 0 when disabled, and a negative value when the
 * request was refused:
 *   -1  the original headers/programs do not match this EBOOT
 *   -2  the target code range is not unused (nonzero bytes)
 *   -3  an address is outside the guest VM
 *   -4  a header does not point at the patched code after the write
 */
int rsx_dof_patch_apply(u8* vm, u32 vm_size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_DOF_PATCH_H */
