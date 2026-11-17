#!/usr/bin/env python3
"""Hook the c690 normal/tangent functions for runtime/spu/c690_native_soa.c.

Wraps 0xFA20 (clear), 0xFA58 (normals) and 0xFCB8 (normalize), and adds the
same call to the existing 0xFDA0 (tangents) wrapper that patch_cull_hooks.py
installs. The hook is a weak reference, so a runtime without the kernels still
links and runs the lifted bodies.

Run on the base module, after patch_cull_hooks.py and before
tools/spu_ftz_twin.py (the twins are derived from the patched base and get
SPU_TWIN_VARIANT from their preamble).

Usage: patch_native_soa.py <.../jobmod_c690c2f7b09a_eF5B8/spu_recomp.c>   (idempotent)
"""
import re
import sys

TAG = "/* C690_SOA */"
DECL = TAG + """
#ifndef SPU_TWIN_VARIANT
#define SPU_TWIN_VARIANT 0
#endif
extern int c690_soa_try(spu_context*, uint32_t, int, void (*)(spu_context*)) __attribute__((weak));
"""


def main() -> int:
    path = sys.argv[1]
    s = open(path).read()
    if TAG in s:
        print(f"{path}: already patched")
        return 0
    m = re.search(r"^void (\w+_)spu_func_0000FDA0_body\(spu_context\* ctx\);\n", s, re.M)
    if not m:
        raise SystemExit("0xFDA0 wrapper not found: run patch_cull_hooks.py first")
    p = m.group(1) + "spu_func_0000"
    call = "        if (c690_soa_try && c690_soa_try(ctx, 0x%su, SPU_TWIN_VARIANT, %s%s_body)) return;\n"

    # 0xFDA0: first statement of the existing wrapper.
    head = f"void {p}FDA0(spu_context* ctx) {{\n"
    assert s.count(head) == 1, "0xFDA0 wrapper head"
    s = s.replace(head, head + call % ("FDA0", p, "FDA0"))

    for lsa in ("FA20", "FA58", "FCB8"):
        head = f"void {p}{lsa}(spu_context* ctx) {{\n"
        assert s.count(head) == 1, f"0x{lsa} definition"
        s = s.replace(head,
                      f"void {p}{lsa}_body(spu_context* ctx);\n" + head + call % (lsa, p, lsa)
                      + f"        __attribute__((musttail)) return {p}{lsa}_body(ctx);\n}}\n"
                      + f"void {p}{lsa}_body(spu_context* ctx) {{\n")

    i = min(s.index(f"void {p}{lsa}_body(spu_context* ctx);\n") for lsa in ("FA20", "FA58", "FCB8", "FDA0"))

    # Declarations go ahead of the first function that uses them.
    s = s[:i] + DECL + s[i:]
    open(path, "w").write(s)
    print(f"{path}: patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
