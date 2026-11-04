#!/usr/bin/env python3
"""tools/fifo_dump_scan.py draw counting.

A batch method is dispatched once per data word of a NON-INCREMENTING
(type-2) packet, but only once at its first data word of an incrementing
(type-0) packet. The scanner used to count only the packet's base method, so
the team-select capture reported 645 batches while replay_rsx dispatched 657
(12 type-2 0x1814 packets with two data words each). These fixtures pin that
rule down without a game or a capture file.
"""
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "fifo_dump_scan", root / "tools" / "fifo_dump_scan.py")
fds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fds)

FLIP = 0xFEAD0000


def hdr(typ, method, count, subch=0):
    return (typ << 29) | (count << 18) | (subch << 13) | method


def run(words):
    out = io.StringIO()
    with redirect_stdout(out):
        fds.scan(words, False, 5, None)
    text = out.getvalue()
    total = next(l for l in text.splitlines() if l.startswith("[total]"))
    fields = dict(part.split("=") for part in total.split()[1:] if "=" in part)
    return text, fields


def test_type2_packet_dispatches_each_word():
    words = [
        hdr(0, 0x1808, 1), 8,                 # BEGIN quads
        hdr(2, 0x1814, 2), 0, 0,              # two array batches
        hdr(0, 0x1808, 1), 0,                 # END
        FLIP,
    ]
    text, t = run(words)
    assert t["draw_batches"] == "2", (text, t)
    assert t["triangle_batches"] == "0", (text, t)
    assert t["frames"] == "1", (text, t)


def test_type0_packet_embedded_batch():
    words = [
        hdr(0, 0x1808, 1), 5,                 # BEGIN triangles
        hdr(0, 0x1810, 2), 0, 0,              # 0x1810, then 0x1814
        hdr(0, 0x1808, 1), 0,
        FLIP,
    ]
    text, t = run(words)
    assert t["draw_batches"] == "1", (text, t)
    assert t["triangle_batches"] == "1", (text, t)


def test_type2_triangle_batches():
    words = [
        hdr(0, 0x1808, 1), 5,
        hdr(2, 0x1814, 3), 0, 0, 0,
        hdr(0, 0x1808, 1), 0,
        FLIP,
    ]
    text, t = run(words)
    assert t["draw_batches"] == "3", (text, t)
    assert t["triangle_batches"] == "3", (text, t)


def test_trailing_marker_is_not_a_frame():
    words = [hdr(0, 0x1808, 1), 8, hdr(2, 0x1814, 1), 0, FLIP, FLIP]
    text, t = run(words)
    assert t["frames"] == "1", (text, t)
    assert t["draw_batches"] == "1", (text, t)


def test_partial_trailing_frame_is_reported():
    words = [hdr(0, 0x1808, 1), 8, hdr(2, 0x1814, 1), 0, FLIP,
             hdr(0, 0x1808, 1), 8, hdr(2, 0x1814, 1), 0]
    text, t = run(words)
    assert t["frames"] == "2", (text, t)
    assert t["draw_batches"] == "2", (text, t)


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)}/{len(tests)}")
