#!/usr/bin/env python3
"""Route the four c690 0x2A field passes through the native pack.

The generated module is game-derived and not committed.  This script renames
each of the four per-field passes (0x7840 position, 0x7FC0 normal, 0x7AC0
fixed s16, 0x7540 f16) to ``*_body`` and inserts an entry wrapper that calls
``c690_native_2a_try()``.  The reference is weak, so a runtime that does not
provide the hook (an older play/gpu build) still links and takes the lifted
body.  Idempotent.

Usage: patch_native_2a.py [path/to/spu_recomp.c]
"""
import os
import re
import sys

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"
PASSES = ["00007840", "00007FC0", "00007AC0", "00007540"]

DECL = (
    "/* Native 0x2A field-pass hook (runtime/spu/c690_native_2a.h).  Weak so\n"
    " * an older runtime without the hook still links (the lifted body runs). */\n"
    "extern int c690_native_2a_try(spu_context*, uint32_t)"
    " __attribute__((weak));\n\n"
)


def wrapper(addr):
    fn = PREFIX + addr
    return (
        "void %s_body(spu_context* ctx);\n"
        "void %s(spu_context* ctx) {\n"
        "        if (__builtin_expect(!c690_native_2a_try ||\n"
        "                !c690_native_2a_try(ctx, 0x%su), 1))\n"
        "            __attribute__((musttail)) return %s_body(ctx);\n"
        "}\n"
        "void %s_body(spu_context* ctx) {\n" % (fn, fn, addr, fn, fn)
    )


def patch(path):
    text = open(path, "r", encoding="utf-8").read()
    if "c690_native_2a_try" in text:
        return False
    for addr in PASSES:
        old = "void %s%s(spu_context* ctx) {\n" % (PREFIX, addr)
        if text.count(old) != 1:
            raise SystemExit("%s: expected one %s definition, found %d"
                             % (path, old.strip(), text.count(old)))
        text = text.replace(old, wrapper(addr), 1)
    pos = text.find("\nvoid jobmod_")
    if pos < 0:
        raise SystemExit("%s: no lifted function to anchor the declaration" % path)
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
