#!/usr/bin/env python3
"""--ib-known-targets: a computed `bi` tries the il/ila-loaded entries before the resolver."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter, ib_known_entries  # noqa: E402


def ri18(op7, i18, rt):
    return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


PROGRAM = [
    ri18(0x21, 0x10, 5),        # 0x00 ila $5, 0x10    a function entry
    ri18(0x21, 0x14, 6),        # 0x04 ila $6, 0x14    a constant, not an entry
    rr(0x1A8, 0, 5, 0),         # 0x08 bi  $5
    rr(0x1A8, 0, 0, 0),         # 0x0C bi  $0          (return: untouched)
    rr(0x1A8, 0, 0, 0),         # 0x10 bi  $0
]
BOUNDS = [(0x00, 0x10), (0x10, 0x14)]
data = b"".join(w.to_bytes(4, "big") for w in PROGRAM)
insns = disassemble_spu(data, 0)
starts = {s for s, _ in BOUNDS}
entries = ib_known_entries(insns, starts)
assert entries == [0x10], entries

for on in (False, True):
    lifter = SPULifter()
    lifter.func_starts = starts
    if on:
        lifter.ib_entries = entries
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, e)
    body = "\n".join(lifter.functions[0].body_lines)
    fast = "if (SPU_IB_OK(ctx, 0x10)) { SPU_DIRECT(ctx, 0x10, spu_func_00000010); }"
    assert (fast in body) == on, body
    assert body.count("SPU_IB_OK") == (1 if on else 0), body
    assert "g_spu_trampoline_fn = spu_indirect_branch; return;" in body, body
print("ib-known-targets: ok")
