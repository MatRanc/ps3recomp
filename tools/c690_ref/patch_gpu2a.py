#!/usr/bin/env python3
"""Wrap the generated c690 0x2A body (LS 0x10F78) for PS3RECOMP_C690_GPU_2A.

The GPU binding needs function-entry/exit hooks around the lifted 0x10F78 body
(where the output cursor and the source table are live).  The wrapper is
inert when the live key is off: two calls into c690_gpu_2a.c that test a
cached global.  It composes with the capture wrapper from
patch_cull_hooks.py --capture (the calls are added around whatever 0x10F78
definition is present, including a `_body`-renaming wrapper).

Usage: patch_gpu2a.py [path/to/spu_recomp.c]
Idempotent.
"""
import sys

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"
LSA = 0x10F78
DECL = ("void c690_gpu_2a_enter(spu_context*);\n"
        "void c690_gpu_2a_exit(spu_context*);\n")


def brace_end(text, open_pos):
    depth = 0
    for i in range(open_pos, len(text)):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                end = text.find('\n', i)
                return len(text) if end < 0 else end + 1
    raise SystemExit("unbalanced braces around the 0x2A body")


def patch(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    name = PREFIX + f"{LSA:08X}"
    sig = f"void {name}(spu_context* ctx) {{\n"
    if text.count(sig) != 1:
        raise SystemExit(f"{path}: expected one definition of {name}, "
                         f"found {text.count(sig)}")
    start = text.index(sig)
    body_at = text.index('{', start)
    end = brace_end(text, body_at)
    body = text[body_at:end]
    if "c690_gpu_2a_enter" in body and "c690_gpu_2a_exit" in body:
        return False
    if DECL.split('\n')[0] not in text:
        text = text[:start] + DECL + text[start:]
        start += len(DECL)
        body_at = text.index('{', start)
        end = brace_end(text, body_at)
    insert_at = body_at + 1
    text = (text[:insert_at] + "\n        c690_gpu_2a_enter(ctx);" +
            text[insert_at:])
    # Re-find the end after the insertion and add the exit before the brace.
    body_at = text.index('{', start)
    end = brace_end(text, body_at)
    close = text.rfind('}', body_at, end)
    text = text[:close] + "        c690_gpu_2a_exit(ctx);\n" + text[close:]
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        import os
        path = os.environ.get(
            "SPU_RECOMP",
            "recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c")
    changed = patch(path)
    print(f"{path}: {'wrapped' if changed else 'already wrapped'}")


if __name__ == "__main__":
    main()
