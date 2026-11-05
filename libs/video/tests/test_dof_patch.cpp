/*
 * ps3recomp - "Better depth of field" patch and its fragment program
 *
 * Checks, without a GPU or a game run:
 *   - the generated 0xA90-byte program decompiles with every opcode handled
 *     and the HLSL translates to SPIR-V (both fp16 and fp32 export control,
 *     and with texture gamma/sign-expand state on the units it samples);
 *   - optionally (argv[1] = EBOOT.elf), the patch applied to a real image:
 *     the two headers follow to 0x10A4400, the bytes there are the generated
 *     program, and a mismatching EBOOT, a used target or a header that does
 *     not follow are refused.
 *
 * Built as a CMake target when glslang is available.
 */
#include "../rsx_fp_decompiler.h"
#include "../rsx_dof_patch.h"
#include "../rsx_dof_patch_table.h"
#include "../rsx_shader_spirv.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void check(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t* p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static std::vector<uint8_t> program_from_table()
{
    std::vector<uint8_t> code(RSX_DOF_PATCH_CODE_SIZE, 0);
    for (uint32_t i = 0; i < RSX_DOF_PATCH_NUM_WRITES; i++) {
        const rsx_dof_patch_write* w = &g_rsx_dof_patch_writes[i];
        if (w->addr >= RSX_DOF_PATCH_CODE_ADDR &&
            w->addr + 8 <= RSX_DOF_PATCH_CODE_ADDR + RSX_DOF_PATCH_CODE_SIZE)
            memcpy(code.data() + (w->addr - RSX_DOF_PATCH_CODE_ADDR), w->bytes, 8);
    }
    return code;
}

static int compile_fragment(const char* hlsl, char* why, size_t why_size)
{
    static std::vector<uint32_t> words(1u << 20);
    uint32_t n = 0;
    if (rsx_hlsl_to_spirv(hlsl, RSX_SHADER_STAGE_FRAGMENT, words.data(),
                          (uint32_t)words.size(), &n, why, (uint32_t)why_size))
        return -1;
    return n > 5 && words[0] == 0x07230203u ? 0 : -1;
}

static void test_program(const char* label, int ctrl, const uint8_t* ucode, uint32_t size)
{
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    static char why[1024];

    int n = rsx_fp_decompile(ucode, size, (uint32_t)ctrl, hlsl, sizeof hlsl);
    char name[160];
    snprintf(name, sizeof name, "%s: decompiles", label);
    check(name, n > 0);
    if (n <= 0) return;
    /* 168 16-byte slots through PROGRAM_END (slot 167), 20 of them
     * inline-constant slots: 148 real instructions. */
    snprintf(name, sizeof name, "%s: 148 instructions (got %d)", label, n);
    check(name, n == 148);
    snprintf(name, sizeof name, "%s: no unhandled opcode", label);
    check(name, !strstr(hlsl, "unhandled FP opcode") && !strstr(hlsl, "TODO: unhandled"));
    why[0] = 0;
    snprintf(name, sizeof name, "%s: HLSL -> SPIR-V", label);
    check(name, compile_fragment(hlsl, why, sizeof why) == 0);
    if (why[0]) printf("       (%s)\n", why);

    uint8_t gamma[16] = {0}, expand[16] = {0};
    gamma[0] = gamma[1] = gamma[2] = 0xFu;          /* the three units it samples */
    expand[1] = 0x3u;                               /* re-centred RG on a tap unit */
    uint32_t nk = 0;
    n = rsx_fp_decompile_buffered_color_ex(ucode, size, (uint32_t)ctrl, 0, gamma,
                                           hlsl, sizeof hlsl, &nk);
    snprintf(name, sizeof name, "%s: gamma/expand state decompiles", label);
    check(name, n > 0);
    if (n > 0) {
        why[0] = 0;
        snprintf(name, sizeof name, "%s: gamma/expand HLSL -> SPIR-V", label);
        check(name, compile_fragment(hlsl, why, sizeof why) == 0);
        if (why[0]) printf("       (%s)\n", why);
    }
}

/* Load a real EBOOT into a flat guest image (vaddr == byte offset). */
static bool load_eboot(const char* path, std::vector<uint8_t>& vm)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file((size_t)sz);
    bool ok = fread(file.data(), 1, file.size(), f) == file.size();
    fclose(f);
    if (!ok || file.size() < 64 || memcmp(file.data(), "\x7f""ELF", 4) != 0 ||
        file[4] != 2 || file[5] != 2)
        return false;
    uint64_t phoff = be64(file.data() + 32);
    uint16_t phentsize = be16(file.data() + 54);
    uint16_t phnum = be16(file.data() + 56);
    vm.assign(0x2000000u, 0);   /* covers every segment and 0x10A4400 */
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = file.data() + phoff + (uint64_t)i * phentsize;
        if (be32(ph) != 1) continue;   /* PT_LOAD */
        uint64_t off = be64(ph + 8), vaddr = be64(ph + 16);
        uint64_t filesz = be64(ph + 32), memsz = be64(ph + 40);
        if (!memsz) continue;
        if (vaddr + memsz > vm.size() || off + filesz > file.size())
            return false;
        memcpy(vm.data() + vaddr, file.data() + off, (size_t)filesz);
    }
    return true;
}

int main(int argc, char** argv)
{
    std::vector<uint8_t> code = program_from_table();
    check("table code is nonzero",
          code[0] == 0x96 && code[1] == 0x00 && code.size() == 0xA90u);

    test_program("fp16 export", 0x00, code.data(), (uint32_t)code.size());
    test_program("fp32 export", 0x40, code.data(), (uint32_t)code.size());

    if (argc > 1) {
        std::vector<uint8_t> vm;
        check("EBOOT loads into a guest image", load_eboot(argv[1], vm));
        if (!vm.empty()) {
            setenv("PS3RECOMP_DOF_PATCH", "1", 1);

            vm[RSX_DOF_PATCH_CODE_ADDR] = 0xAA;   /* target is not unused */
            check("used code target is refused",
                  rsx_dof_patch_apply(vm.data(), (uint32_t)vm.size()) == -2);
            vm[RSX_DOF_PATCH_CODE_ADDR] = 0;

            check("patch applies to the real EBOOT",
                  rsx_dof_patch_apply(vm.data(), (uint32_t)vm.size()) == 1);
            bool all_ok = true;
            for (uint32_t i = 0; i < RSX_DOF_VERIFY_REGION_COUNT; i++) {
                uint32_t hdr = g_rsx_dof_verify_regions[i].addr;
                int32_t off = (int32_t)be32(vm.data() + hdr + 0x10);
                all_ok = all_ok && be32(vm.data() + hdr) == RSX_DOF_PATCH_CODE_SIZE &&
                         (uint32_t)((int64_t)hdr + off) == RSX_DOF_PATCH_CODE_ADDR;
            }
            check("both headers follow to 0x10A4400", all_ok);
            check("patched bytes in the image are the generated program",
                  !memcmp(vm.data() + RSX_DOF_PATCH_CODE_ADDR, code.data(), code.size()));
            test_program("live image", 0x00, vm.data() + RSX_DOF_PATCH_CODE_ADDR,
                         RSX_DOF_PATCH_CODE_SIZE);

            uint32_t hdr = g_rsx_dof_verify_regions[0].addr;
            uint8_t saved = vm[hdr];
            vm[hdr] ^= 0x01;
            check("mismatching EBOOT is refused",
                  rsx_dof_patch_apply(vm.data(), (uint32_t)vm.size()) == -1);
            vm[hdr] = saved;
            unsetenv("PS3RECOMP_DOF_PATCH");
        }
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
