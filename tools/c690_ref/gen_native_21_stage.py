#!/usr/bin/env python3
"""Flatten the c690 0x21 emit command (LS 0x13080 and its callees) into one C
function.

The lifted 0x21 handler is a call/tail-call graph: the dispatcher 0x13080
selects a sub-selector, which calls (brsl) one of the format setup routines
(0xBB80/0xBB98/0xA3C0/0xA3E0/0xA3F8), and those tail-transfer into the pack /
transform / normalize kernels.  Every transfer marshals the whole live register
set through ctx->gpr.  Flattening the reachable blocks into one function keeps
sr0..sr127 live and removes the marshalling; the SPU arithmetic and LS helpers
are unchanged, so the result is bit-exact by construction.

The generated file is game data (the c690 module is our own EBOOT's image) but
is committed next to the runtime that uses it, like the other native stages.

Usage:
    tools/c690_ref/gen_native_21_stage.py \
        ../../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
        runtime/spu/c690_native_21_stage.inc
"""
import re
import sys

PREFIX = "jobmod_c690c2f7b09a_eF5B8_spu_func_"

# Extra entry points reached by the format dispatch tables the hot paths read
# (LS 0x14300..0x14500 in the captured command states).  The tables are built
# by earlier commands; this is the union observed across our meter captures
# and the static table words that fall inside the c690 image.
DISPATCH_TARGETS = """
0A730 0A798 0A808 0A810 0A818 0A8A0 0A940 0A944 0A948 0A9B0 0ABB8 0AD18
0AF10 0AF98 0B268 0B488 0B748 0B830 0B958 0BA10 0BB20 0BE00 0BED0 0C018
0C0F8 0C290 0C398 0C420 0C5B8 0C8E8 0C9D0 0CC48 0CD80 0D080
""".split()

ROOT = "00013080"


def blocks(text):
    """Map LS address -> (name, body, is_body) for every generated function."""
    out = {}
    pat = re.compile(r"^void (" + PREFIX + r"([0-9A-Fa-f]+)(_body)?)\(spu_context\* ctx\) \{",
                     re.M)
    for m in pat.finditer(text):
        addr = m.group(2)
        name = m.group(1)
        i = text.index("{", m.start())
        depth = 0
        j = i
        while j < len(text):
            c = text[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[i + 1:j]
        is_body = bool(m.group(3))
        # A *_body definition wins over the hook wrapper with the same address.
        if addr not in out or is_body:
            out[addr] = (name, body, is_body)
    return out


def refs(body):
    """Direct/brsl callees named in a body.

    The runtime's --ib-known-targets lists (`if (SPU_IB_OK(...)) SPU_DIRECT`)
    are a resolver hint list for a computed branch, not unconditional edges.
    Drop them: the computed branch is handled by the flattened switch, and the
    unknown selector/format targets must stay out of the graph so the stage
    falls back to the lifted code for them.
    """
    body = re.sub(r"if \(SPU_IB_OK\(ctx, 0x[0-9A-Fa-f]+\)\) \{ "
                  r"SPU_DIRECT\(ctx,[^}]*\); \}", "", body)
    r = set()
    for m in re.finditer(r"SPU_DIRECT\(ctx,\s*0x[0-9A-Fa-f]+,\s*" + PREFIX +
                         r"([0-9A-Fa-f]+)\)", body):
        r.add(m.group(1))
    for m in re.finditer(PREFIX + r"([0-9A-Fa-f]+)\(ctx\)", body):
        r.add(m.group(1))
    return r


def reachable(bodies):
    seen = set()
    todo = ([ROOT, "00013140", "000131A0"] +
            ["%08X" % int(a, 16) for a in DISPATCH_TARGETS])
    while todo:
        a = todo.pop()
        if a in seen or a not in bodies:
            continue
        seen.add(a)
        for r in refs(bodies[a][1]):
            if r not in seen:
                todo.append(r)
    return seen


CALL_RE = re.compile(
    r"ctx->gpr\[(\d+)\] = spu_link\(0x([0-9A-Fa-f]+)\); \{ int32_t _si = "
    r"\(int32_t\)ctx->image_id; ctx->host_depth\+\+; (" + PREFIX +
    r"([0-9A-Fa-f]+))\(ctx\); spu_drain_call\(ctx, 0x\2\); ctx->host_depth--; "
    r"spu_img_restore\(ctx, _si\); \}")


def transform(addr, body, ret_labels):
    # brsl calls: the link register is the return pc, but the flattened body
    # needs a unique continuation label per call site.  Assign one id per
    # call site and push that.
    def call_sub(m):
        link, ret, callee = m.group(1), m.group(2), m.group(4)
        key = "R_%s_%s" % (ret, addr)
        if key in ret_labels:
            n = 2
            while "%s_%d" % (key, n) in ret_labels:
                n += 1
            key = "%s_%d" % (key, n)
        nid = len(ret_labels) + 1
        ret_labels[key] = nid
        return ("sr%s = spu_u2v(spu_link(0x%s)); c21_push(%d); goto F_%s;\n"
                "%s: ;" % (link, ret, nid, callee, key))
    IBCALL_RE = re.compile(
        r"\{ uint32_t _tg = ctx->gpr\[(\d+)\]\._u32\[0\]; "
        r"ctx->gpr\[(\d+)\] = spu_link\(0x([0-9A-Fa-f]+)\); int32_t _si = "
        r"\(int32_t\)ctx->image_id; ctx->pc = _tg; ctx->host_depth\+\+; "
        r"spu_indirect_branch\(ctx\); spu_drain_call\(ctx, 0x\3\); "
        r"ctx->host_depth--; spu_img_restore\(ctx, _si\); \}")

    def ibcall_sub(m):
        treg, link, ret = m.group(1), m.group(2), m.group(3)
        key = "R_%s_%s_ib" % (ret, addr)
        if key in ret_labels:
            n = 2
            while "%s_%d" % (key, n) in ret_labels:
                n += 1
            key = "%s_%d" % (key, n)
        nid = len(ret_labels) + 1
        ret_labels[key] = nid
        return ("sr%s = spu_u2v(spu_link(0x%s)); c21_push(%d); "
                "ctx->pc = sr%s[0] & SPU_LS_MASK; goto L_IB_%s;\n%s: ;" %
                (link, ret, nid, treg, treg, key))
    body = IBCALL_RE.sub(ibcall_sub, body)
    body = CALL_RE.sub(call_sub, body)

    # Per-block register loads and ctx->gpr marshalling stores disappear;
    # sr0..sr127 stay live in the flattened function.
    # Drop only the per-block prologue loads (sr0..sr127 are initialised once
    # at the top).  Keep the ctx->gpr stores and the reloads after a brsl:
    # they are the lifted marshalling the caller observes across a call.
    body = re.sub(r"spu_v4u sr\d+ = spu_u2v\(ctx->gpr\[\d+\]\);\s*", "", body)

    # Interrupt polls: the native stage runs to completion (same contract as
    # the native 0xFDA0 / 0xD080 stages; the c690 job does not take an SPU
    # interrupt in this command).
    body = "\n".join(l for l in body.splitlines()
                     if "ctx->int_enable && (ctx->event_status & ctx->event_mask)" not in l)

    # Tail transfers become gotos.
    body = re.sub(r"SPU_DIRECT\(ctx,\s*0x[0-9A-Fa-f]+,\s*" + PREFIX +
                  r"([0-9A-Fa-f]+)\);", r"goto F_\1;", body)
    # Terminal returns.
    body = body.replace("SPU_RET(ctx);", "goto L_SPURET;")
    body = re.sub(r"SPU_RET_REG\(ctx, (\d+)\);", r"goto L_SPURET_REG_\1;", body)

    # Computed (register) branches.  The lifted code publishes pc and then
    # tests a list of known targets; the flattened switch on the same value
    # covers them all.
    body = re.sub(r"ctx->pc = ctx->gpr\[(\d+)\]\._u32\[0\];"
                  r"(?: if \(SPU_IB_OK\(ctx, 0x[0-9A-Fa-f]+\)\) \{ goto [^}]*\})*"
                  r"\s*g_spu_trampoline_fn = spu_indirect_branch; (?:return;|goto L_DONE;)",
                  r"ctx->pc = sr\1[0] & SPU_LS_MASK; goto L_IB_\1;", body)

    # Constant-target branches with the interrupt gate around them.
    body = re.sub(r"ctx->int_enable = [01];\s*", "", body)
    body = re.sub(r"spu_fncap_capture\(ctx, 0x[0-9A-Fa-f]+\);\s*", "", body)
    body = re.sub(r" && SPU_GOTO_OK\(ctx, 0x[0-9A-Fa-f]+\)", "", body)
    body = re.sub(r"ctx->pc = 0x([0-9A-Fa-f]+); if \(SPU_IB_OK\(ctx, 0x\1\)\) "
                  r"\{ goto F_([0-9A-Fa-f]+); \} g_spu_trampoline_fn = "
                  r"spu_indirect_branch; (?:return;|goto L_DONE;)",
                  r"goto F_\2;", body)
    # The interrupt/drain-gated fallback of a constant direct branch: the
    # native stage runs to completion, so the direct transfer always applies.
    body = re.sub(r"if \(!SPU_GOTO_OK\(ctx, 0x[0-9A-Fa-f]+\)\) \{ "
                  r"ctx->pc = 0x[0-9A-Fa-f]+; g_spu_trampoline_fn = "
                  r"spu_indirect_branch; (?:return;|goto L_DONE;) \}", "", body)

    body = re.sub(r"ctx->pc = 0x([0-9A-Fa-f]+)u; g_spu_trampoline_fn = "
                  r"spu_indirect_branch; (?:return;|goto L_DONE;)",
                  r"ctx->pc = \1u; goto L_LIFT_DISPATCH;", body)

    # Stop paths: reproduce the stop and finish.
    body = re.sub(r"ctx->stop_code = (0x[0-9A-Fa-f]+)u; ctx->status = "
                  r"SPU_STATUS_STOPPED_BY_STOP; spu_stop\(ctx\); return;",
                  r"ctx->stop_code = \1u; ctx->status = SPU_STATUS_STOPPED_BY_STOP; "
                  r"spu_stop(ctx); goto L_STOP;", body)

    # Anything left is a terminal return of the top-level command.
    body = re.sub(r"\breturn;", "goto L_DONE;", body)

    # Block-local labels must not collide across the flattened bodies.
    for loc in set(re.findall(r"loc_([0-9A-Fa-f]+)", body)):
        body = body.replace("loc_" + loc, "loc_" + addr + "_" + loc)
    return body


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_native_21_stage.py <spu_recomp.c> <out.inc>")
    src, dst = sys.argv[1], sys.argv[2]
    bodies = blocks(open(src).read())
    if ROOT not in bodies:
        raise SystemExit("no %s in %s" % (ROOT, src))
    have = sorted(reachable(bodies), key=lambda x: int(x, 16))

    ret_labels = {}
    out_first = []
    out_first.append("/* GENERATED by tools/c690_ref/gen_native_21_stage.py from the")
    out_first.append(" * c690 lifted module; DO NOT EDIT.  Flattened 0x21 emit")
    out_first.append(" * command %s and its reachable callees, no per-block GPR" % ROOT)
    out_first.append(" * marshalling.  Addresses are LS words. */")
    out_first.append("static uint16_t c21_ret_stack[16];")
    out_first.append("static int c21_ret_sp;")
    out_first.append("static void c21_push(uint32_t pc) {")
    out_first.append("    if (c21_ret_sp < 16) c21_ret_stack[c21_ret_sp++] = pc;")
    out_first.append("}")
    sync = " ".join("ctx->gpr[%d]=spu_v2u(sr%d);" % (i, i) for i in range(128))
    out_first.append("#define C21_SYNC() do { %s } while (0)" % sync)
    out_first.append("")
    out_first.append("static int c690_native_21_stage(spu_context* ctx, uint32_t entry) {")
    out_first.append("    spu_v4u " + ", ".join("sr%d" % i for i in range(128)) + ";")
    for i in range(128):
        out_first.append("    sr%d = spu_u2v(ctx->gpr[%d]);" % (i, i))
    out_first.append("    c21_ret_sp = 0;")
    out_first.append("    switch (entry) {")
    for a in ["00013080", "00013140", "000131A0"]:
        out_first.append("    case 0x%su: goto F_%s;" % (a.lstrip("0"), a))
    out_first.append("    default: return 0;")
    out_first.append("    }")

    body_lines = []
    for addr in have:
        body_lines.append("F_%s: ;" % addr)
        body_lines.append(transform(addr, bodies[addr][1], ret_labels))

    # Indirect-branch switches: one per register the code branches through.
    all_text = "\n".join(body_lines)
    ib_regs = set(re.findall(r"goto L_IB_(\d+);", all_text))
    cases = "\n".join("        case 0x%su: goto F_%s;" % (a.lstrip("0") or "0", a)
                      for a in have)
    for reg in sorted(ib_regs, key=int):
        body_lines.append("L_IB_%s: ;" % reg)
        body_lines.append("    switch (sr%s[0] & SPU_LS_MASK) {" % reg)
        body_lines.append(cases)
        body_lines.append("        default: goto L_LIFT_DISPATCH;")
        body_lines.append("    }")

    # SPU_RET: return to the pushed link, or finish at the top level.
    body_lines.append("L_SPURET: ;")
    body_lines.append("    if (c21_ret_sp) {")
    body_lines.append("        uint32_t r = c21_ret_stack[--c21_ret_sp];")
    body_lines.append("        switch (r) {")
    for key in sorted(ret_labels, key=lambda k: ret_labels[k]):
        body_lines.append("        case %du: goto %s;" % (ret_labels[key], key))
    body_lines.append("        default: goto L_LIFT_DISPATCH;")
    body_lines.append("        }")
    body_lines.append("    }")
    body_lines.append("    goto L_DONE;")
    for reg in sorted(set(re.findall(r"goto L_SPURET_REG_(\d+);", all_text)), key=int):
        body_lines.append("L_SPURET_REG_%s: ctx->pc = sr%s[0] & SPU_LS_MASK; goto L_LIFT_DISPATCH;" %
                          (reg, reg))

    body_lines.append("L_DONE: ;")
    body_lines.append("    ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;")
    body_lines.append("    if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch;")
    body_lines.append("    return 1;")
    body_lines.append("L_STOP: ;")
    body_lines.append("    return 1;")
    body_lines.append("L_LIFT_DISPATCH: ;")
    body_lines.append("    return 2;")
    body_lines.append("}")

    open(dst, "w").write("\n".join(out_first + body_lines) + "\n")
    print("wrote %s (%d blocks)" % (dst, len(have)))


if __name__ == "__main__":
    main()
