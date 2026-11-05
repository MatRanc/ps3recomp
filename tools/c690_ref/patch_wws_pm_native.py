#!/usr/bin/env python3
"""Route the WWS job manager's ticket claim through the native runtime path.

The generated policy module (`recomp-work/spu_lifted/wwsjob_pm/spu_recomp.c`,
LS base 0xA00, image 6) is game-derived and not committed. This script makes
the native head section reproducible after a relift:

  * includes `runtime/spu/wws_native_pm.h` (PS3RECOMP_WWS_NATIVE_PM, default 0)
  * renames the lifted claim 0x23A8 to ..._body and puts a one-line wrapper in
    front of it that calls wws_pm_native_claim() when the key is on.

Everything else in the PM (job fetch, descriptor DMA, job-code re-DMA, the
exit tail) still runs through the lifted code, so the guest-visible job order,
cursors and counters are unchanged when the key is off or on.

Idempotent. Usage: patch_wws_pm_native.py [path/to/spu_recomp.c]
"""
import os
import sys

PREFIX = "wwsjob_pm_spu_func_"
NAME = PREFIX + "000023A8"
MARK = "wws_pm_native_claim"

WRAPPER = (
    "void {name}_body(spu_context* ctx);\n"
    "void {name}(spu_context* ctx) {{\n"
    "        if (wws_native_pm_enabled()) {{ {mark}(ctx); return; }}\n"
    "        {name}_body(ctx);\n"
    "}}\n\n"
).format(name=NAME, mark=MARK)

DECL = ('/* Native WWS PM head (runtime/spu/wws_native_pm.h). */\n'
        '#include "wws_native_pm.h"\n\n')


def patch(path):
    text = open(path, "r", encoding="utf-8").read()
    if MARK in text:
        return False
    sig = "void %s(spu_context* ctx) {" % NAME
    pos = text.find(sig)
    if pos < 0:
        raise SystemExit("%s: no %s definition" % (path, NAME))
    text = text[:pos] + text[pos:].replace(sig, "void %s_body(spu_context* ctx) {" % NAME, 1)
    text = text[:pos] + WRAPPER + text[pos:]
    anchor = text.find("#include \"spu_helpers.h\"")
    if anchor < 0:
        raise SystemExit("%s: no spu_helpers.h include to anchor the helper" % path)
    end = text.find("\n", anchor) + 1
    text = text[:end] + DECL + text[end:]
    open(path, "w", encoding="utf-8").write(text)
    return True


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
        path = os.path.join(root, "recomp-work", "spu_lifted", "wwsjob_pm",
                            "spu_recomp.c")
    print(("patched " if patch(path) else "already patched ") + path)


if __name__ == "__main__":
    main()
