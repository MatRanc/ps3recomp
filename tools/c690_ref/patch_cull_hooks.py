#!/usr/bin/env python3
"""Add the cull wrappers to the generated c690 lifted module.

The generated module (`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/
spu_recomp.c`) is game-derived data and is not committed. This script makes
the two hand-written hooks reproducible after a relift:

  * c690_cull_fix        native sample-grid fix around the lifted cull
  * c690_corpus_cull_*   cull-stage spool for offline replay

It renames the generated 0x132E0 (0x25/0x26) and 0x12C08 (0x2C) bodies to
`*_body` and inserts the wrapper in front of them. Run it once after
`tools/lift_spu.sh` touched the module; it is idempotent.

Usage: patch_cull_hooks.py [path/to/spu_recomp.c]
"""
import sys
import os

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"
TARGETS = [
    (0x132E0, 1),   # 0x25/0x26 triangle cull
    (0x12C08, 2),   # 0x2C extra-block cull
]

WRAPPER = """void {name}_body(spu_context* ctx);
void {name}(spu_context* ctx) {{
        extern int g_c690_corpus_on;
        extern void c690_corpus_cull_enter(const spu_context*, uint32_t);
        extern void c690_corpus_cull_exit(const spu_context*, uint32_t);
        extern int c690_cull_fix_maybe(void);
        extern int c690_cull_fix_enter(spu_context*, int);
        extern void c690_cull_fix_leave(spu_context*);
        if (__builtin_expect(!g_c690_corpus_on && !c690_cull_fix_maybe(), 1)) {{
            {name}_body(ctx);
            return;
        }}
        const int patched = c690_cull_fix_enter(ctx, {family});
        if (g_c690_corpus_on) c690_corpus_cull_enter(ctx, 0x{lsa:05X}u);
        {name}_body(ctx);
        if (g_c690_corpus_on) c690_corpus_cull_exit(ctx, 0x{lsa:05X}u);
        if (patched) c690_cull_fix_leave(ctx);
}}
"""


def patch(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    changed = False
    for lsa, family in TARGETS:
        name = PREFIX + f"{lsa:08X}"
        if f"void {name}(spu_context* ctx) {{\n        extern int g_c690_corpus_on;" in text:
            continue
        old = f"void {name}(spu_context* ctx) {{\n"
        if text.count(old) != 1:
            raise SystemExit(f"{path}: expected one definition of {name}, "
                             f"found {text.count(old)}")
        body = f"void {name}_body(spu_context* ctx) {{\n"
        wrapper = WRAPPER.format(name=name, family=family, lsa=lsa)
        text = text.replace(old, wrapper + body)
        changed = True
    if changed:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    return changed


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
        path = os.path.join(root, "recomp-work", "spu_lifted",
                            "jobmod_c690c2f7b09a_eF5B8", "spu_recomp.c")
    if patch(path):
        print(f"patched {path}")
    else:
        print(f"already patched {path}")


if __name__ == "__main__":
    main()
