/*
 * ps3recomp - apply the "Better depth of field" fragment-program patch.
 * See rsx_dof_patch.h.
 */
#include "rsx_dof_patch.h"
#include "rsx_dof_patch_table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t fnv1a64(const uint8_t* p, uint32_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int rsx_dof_patch_enabled(void)
{
    const char* e = getenv("PS3RECOMP_DOF_PATCH");
    if (!e || !*e) return 0;
    if (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "no") ||
        !strcmp(e, "false")) return 0;
    return 1;
}

int rsx_dof_patch_apply(u8* vm, u32 vm_size)
{
    if (!rsx_dof_patch_enabled()) return 0;
    if (!vm) {
        fprintf(stderr, "[dof] PS3RECOMP_DOF_PATCH=1 but there is no guest VM\n");
        return -3;
    }

    for (uint32_t i = 0; i < RSX_DOF_VERIFY_REGION_COUNT; i++) {
        const rsx_dof_verify_region* r = &g_rsx_dof_verify_regions[i];
        if ((uint64_t)r->addr + r->len > 0x100000000ull ||
            (vm_size && (uint64_t)r->addr + r->len > vm_size)) {
            fprintf(stderr, "[dof] refused: verify region 0x%08X+0x%X is outside the guest VM\n",
                    r->addr, r->len);
            return -3;
        }
        uint64_t h = fnv1a64(vm + r->addr, r->len);
        if (h != r->fnv1a64) {
            fprintf(stderr,
                    "[dof] refused: EBOOT fragment program/header at 0x%08X does not match "
                    "the generated patch (got %016llx, want %016llx)\n",
                    r->addr, (unsigned long long)h, (unsigned long long)r->fnv1a64);
            return -1;
        }
    }

    if ((uint64_t)RSX_DOF_PATCH_CODE_ADDR + RSX_DOF_PATCH_CODE_SIZE > 0x100000000ull ||
        (vm_size && (uint64_t)RSX_DOF_PATCH_CODE_ADDR + RSX_DOF_PATCH_CODE_SIZE > vm_size)) {
        fprintf(stderr, "[dof] refused: code target 0x%08X+0x%X is outside the guest VM\n",
                RSX_DOF_PATCH_CODE_ADDR, RSX_DOF_PATCH_CODE_SIZE);
        return -3;
    }
    for (uint32_t i = 0; i < RSX_DOF_PATCH_CODE_SIZE; i++) {
        if (vm[RSX_DOF_PATCH_CODE_ADDR + i] != 0) {
            fprintf(stderr,
                    "[dof] refused: code target 0x%08X is not unused (byte 0x%X is nonzero)\n",
                    RSX_DOF_PATCH_CODE_ADDR, i);
            return -2;
        }
    }

    for (uint32_t i = 0; i < RSX_DOF_PATCH_NUM_WRITES; i++) {
        const rsx_dof_patch_write* w = &g_rsx_dof_patch_writes[i];
        if ((uint64_t)w->addr + 8 > 0x100000000ull ||
            (vm_size && (uint64_t)w->addr + 8 > vm_size)) {
            fprintf(stderr, "[dof] refused: write target 0x%08X is outside the guest VM\n", w->addr);
            return -3;
        }
        memcpy(vm + w->addr, w->bytes, 8);
    }

    /* Both headers must now name the patched program: size at +0, signed
     * ucode offset at +0x10, relative to the header's own guest address. */
    for (uint32_t i = 0; i < RSX_DOF_VERIFY_REGION_COUNT; i++) {
        u32 hdr = g_rsx_dof_verify_regions[i].addr;
        u32 size = be32(vm + hdr);
        int32_t off = (int32_t)be32(vm + hdr + 0x10);
        u32 code = (u32)((int64_t)hdr + off);
        if (size != RSX_DOF_PATCH_CODE_SIZE || code != RSX_DOF_PATCH_CODE_ADDR) {
            fprintf(stderr,
                    "[dof] refused: header 0x%08X names size 0x%X code 0x%08X after the write\n",
                    hdr, size, code);
            return -4;
        }
    }

    printf("[dof] applied Better depth of field at 0x%08X (0x%X bytes) to both program headers\n",
           RSX_DOF_PATCH_CODE_ADDR, RSX_DOF_PATCH_CODE_SIZE);
    return 1;
}
