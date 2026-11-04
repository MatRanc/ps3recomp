#!/usr/bin/env python3
"""Route the 0x25/0x26 cull's 0xD080 call through the native screen stage.

The generated module is game-derived and not committed.  After a lift, run
patch_cull_hooks.py first (it installs the union/bypass wrappers), then this
script.  It replaces the two `spu_link(0x13400) ... 0xD080 ... spu_drain_call`
brackets (opcode 0x25 and 0x26 paths) with a call to the runtime helper
`c690_native_screen_try()`, which runs the flattened native subtree, the 2x1
union, or the lifted call in shadow mode, and returns 0 when everything is
off.  Idempotent.

Usage: patch_native_screen.py [path/to/spu_recomp.c]
"""
import os
import sys

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"
OLD = ("        ctx->gpr[0] = spu_link(0x13400); { int32_t _si = "
       "(int32_t)ctx->image_id; ctx->host_depth++; "
       + PREFIX + "0000D080(ctx); spu_drain_call(ctx, 0x13400); "
       "ctx->host_depth--; spu_img_restore(ctx, _si); }\n")
NEW = ("        ctx->gpr[0] = spu_link(0x13400);\n"
       "        if (!c690_native_screen_try(ctx, 0x13400u, "
       + PREFIX + "0000D080)) {\n"
       "            int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; "
       + PREFIX + "0000D080(ctx); spu_drain_call(ctx, 0x13400); "
       "ctx->host_depth--; spu_img_restore(ctx, _si);\n"
       "        }\n")

DECL = ("/* Native screen-cull hook (runtime/spu/c690_native_screen.h). */\n"
        "extern int c690_native_screen_try(spu_context*, uint32_t, "
        "void (*)(spu_context*));\n\n")


def patch(path):
    text = open(path, "r", encoding="utf-8").read()
    if "c690_native_screen_try" in text:
        return False
    n = text.count(OLD)
    if n != 2:
        raise SystemExit(f"{path}: expected 2 0xD080 call brackets, found {n}")
    text = text.replace(OLD, NEW)
    pos = text.find("\nvoid jobmod_")
    if pos < 0:
        raise SystemExit(f"{path}: no lifted function to anchor the helper")
    text = text[:pos + 1] + DECL + text[pos + 1:]
    open(path, "w", encoding="utf-8").write(text)
    return True


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
        path = os.path.join(root, "recomp-work", "spu_lifted",
                            "jobmod_c690c2f7b09a_eF5B8", "spu_recomp.c")
    print(("patched " if patch(path) else "already patched ") + path)


if __name__ == "__main__":
    main()
