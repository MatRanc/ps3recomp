#!/usr/bin/env python3
"""Add the cull wrappers to the generated c690 lifted module.

The generated module (`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/
spu_recomp.c`) is game-derived data and is not committed. This script makes
the hand-written hooks reproducible after a relift:

  * c690_cull_fix        native sample-grid fix around the lifted cull
  * c690_corpus_cull_*   cull-stage spool for offline replay
  * c690_cull_bypass     the MLB16_CULL_BYPASS dispatch helper, routed through
                         the runtime's live key (PS3_LIVE_CULL_BYPASS) instead
                         of a one-time getenv

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
        extern int c690_cull_fix_begin(spu_context*, int);
        extern void c690_cull_fix_mid(spu_context*);
        extern void c690_cull_fix_end(spu_context*);
        extern void c690_cull_fix_leave(spu_context*);
        if (__builtin_expect(!g_c690_corpus_on && !c690_cull_fix_maybe(), 1)) {{
            {name}_body(ctx);
            return;
        }}
        /* Snapshot the game's real input before any transform patch. */
        if (g_c690_corpus_on) c690_corpus_cull_enter(ctx, 0x{lsa:05X}u);
        const int mode = c690_cull_fix_begin(ctx, {family});
        if (mode == 1) {{
            {name}_body(ctx);          /* sample-grid (2x) run */
            c690_cull_fix_mid(ctx);    /* save list, restore real input */
            {name}_body(ctx);          /* game run, unpatched */
            /* Exit snapshot is the game's real output, before the union. */
            if (g_c690_corpus_on) c690_corpus_cull_exit(ctx, 0x{lsa:05X}u);
            c690_cull_fix_end(ctx);    /* append sample-grid-only triangles */
        }} else {{
            {name}_body(ctx);
            if (mode == 2) c690_cull_fix_leave(ctx);
            if (g_c690_corpus_on) c690_corpus_cull_exit(ctx, 0x{lsa:05X}u);
        }}
}}
"""

# The bypass helper and the three dispatch sites. The switch lives in the
# generated tree, so a fresh lift has neither; this makes it reproducible.
# The runtime function owns the live key and the environment fallback.
BYPASS_DEF = """/* MLB16_CULL_BYPASS is a live key (PS3_LIVE_CULL_BYPASS) with an
 * environment fallback. 0 = normal, 1 = both cull families, 2 = 0x2C only,
 * 3 = 0x25/0x26 only. Inert at 0. */
static int c690_cull_bypass(void) {
        extern int c690_cull_bypass_mode(void);
        return c690_cull_bypass_mode();
}

"""

# handler LS address -> the bypass value that skips its family
BYPASS_SITES = [
    (0x13518, 3),   # opcode 0x26 -> 0x132E0
    (0x13520, 3),   # opcode 0x25 -> 0x132E0
    (0x12DC0, 2),   # opcode 0x2C -> 0x12C08
]


def _brace_end(text, open_pos):
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
    raise SystemExit("unbalanced braces in the cull bypass helper")


def patch_bypass(text, path):
    """Install the live-key bypass helper and the three dispatch guards."""
    changed = False
    marker = "static int c690_cull_bypass(void)"
    if "c690_cull_bypass_mode" not in text:
        pos = text.find(marker)
        if pos >= 0:
            # Old one-shot getenv helper, with its comment: replace it whole.
            start = text.rfind("/*", 0, pos)
            if start < 0 or pos - start > 600 or \
                    text.find("*/", start, pos) < 0:
                start = pos
            end = _brace_end(text, text.index('{', pos))
            text = text[:start] + BYPASS_DEF + text[end:]
        else:
            # Fresh lift: put the helper before the first lifted function.
            pos = text.find("\nvoid jobmod_")
            if pos < 0:
                raise SystemExit(f"{path}: no lifted function to anchor the "
                                 "cull bypass helper")
            text = text[:pos + 1] + BYPASS_DEF + text[pos + 1:]
        changed = True

    for lsa, family in BYPASS_SITES:
        name = PREFIX + f"{lsa:08X}"
        sig = f"void {name}(spu_context* ctx) {{\n"
        if text.count(sig) != 1:
            raise SystemExit(f"{path}: expected one definition of {name}, "
                             f"found {text.count(sig)}")
        line = (f"        {{ int m = c690_cull_bypass(); "
                f"if (m == 1 || m == {family}) SPU_RET(ctx); }}\n")
        at = text.index(sig) + len(sig)
        if text.startswith(line, at):
            continue
        text = text[:at] + line + text[at:]
        changed = True
    return text, changed


def patch(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    text, changed = patch_bypass(text, path)
    for lsa, family in TARGETS:
        name = PREFIX + f"{lsa:08X}"
        old = f"void {name}(spu_context* ctx) {{\n"
        body = f"void {name}_body(spu_context* ctx) {{\n"
        if old + "        extern int g_c690_corpus_on;" in text:
            if "c690_cull_fix_begin" in text:
                continue        # already the union wrapper
            # Migrate the old single-grid wrapper: drop it and give the body
            # its original definition back, then insert the union wrapper.
            start = text.index(old)
            bstart = text.find(body, start)
            if bstart < 0:
                raise SystemExit(f"{path}: old {name} wrapper without a body")
            text = text[:start] + text[bstart:]
            if text.count(body) != 1:
                raise SystemExit(f"{path}: expected one body for {name}")
            text = text.replace(body, old, 1)
        if text.count(old) != 1:
            raise SystemExit(f"{path}: expected one definition of {name}, "
                             f"found {text.count(old)}")
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
