#!/usr/bin/env python3
"""Route the c690 0x21 emit command through the native whole-command body.

The generated module is game-derived and not committed.  This script renames
the lifted 0x13080 entry to ``*_body`` and inserts an entry wrapper that calls
``c690_native_21_try()`` (and the optional corpus capture hooks).  The hooks are
declared weak, so a runtime that does not provide them (the play/gpu trees and
older builds) still links and takes the lifted body.  Idempotent: a module that
already contains ``c690_native_21_try`` is left unchanged.

Usage: patch_native_21.py [path/to/spu_recomp.c]
"""
import os
import sys

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"
ADDR = "00013080"
FN = PREFIX + ADDR


def wrapper():
    return (
        "void %(fn)s_body(spu_context* ctx);\n"
        "void %(fn)s(spu_context* ctx) {\n"
        "        extern int c690_native_21_capture_begin(spu_context*, uint32_t)"
        " __attribute__((weak));\n"
        "        extern void c690_native_21_capture_end(spu_context*)"
        " __attribute__((weak));\n"
        "        extern int c690_native_21_try(spu_context*, uint32_t)"
        " __attribute__((weak));\n"
        "        if (__builtin_expect(c690_native_21_capture_begin &&\n"
        "                c690_native_21_capture_begin(ctx, 0x%(a)su), 0)) {\n"
        "            %(fn)s_body(ctx);\n"
        "            if (c690_native_21_capture_end) c690_native_21_capture_end(ctx);\n"
        "            return;\n"
        "        }\n"
        "        if (__builtin_expect(c690_native_21_try &&\n"
        "                c690_native_21_try(ctx, 0x%(a)su), 0)) return;\n"
        "        __attribute__((musttail)) return %(fn)s_body(ctx);\n"
        "}\n"
        "void %(fn)s_body(spu_context* ctx) {\n"
        % {"fn": FN, "a": ADDR}
    )


def patch(path):
    text = open(path, "r", encoding="utf-8").read()
    if "c690_native_21_try" in text:
        return False
    old = "void %s(spu_context* ctx) {\n" % FN
    n = text.count(old)
    if n != 1:
        # A previous partial patch (body only) can leave a forward declaration.
        if "void %s_body(spu_context* ctx) {" % FN in text:
            return False
        raise SystemExit("%s: expected one %s definition, found %d"
                         % (path, old.strip(), n))
    text = text.replace(old, wrapper(), 1)
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
