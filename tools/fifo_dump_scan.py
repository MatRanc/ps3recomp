#!/usr/bin/env python3
"""Summarise a PS3RECOMP_DUMP_FIFO stream (or a PS3RECOMP_WALKER_CAPTURE
.fifo) without a graphics backend.

The dump is a linear, branch-free list of RSX method packets plus
0xFEAD0000|buffer flip markers, big-endian. It is not a rendered image and
carries no vertex/texture bytes, so this tool answers "which draws did the
guest submit and with what state", not "what did they look like".

    tools/fifo_dump_scan.py /tmp/logo_teamselect.bin --draws --top 20

Per frame (between flip markers) it prints the primitive histogram and, with
--draws, every BEGIN_END primitive plus the surface/target, texture binds,
depth/blend/cull state that were current when the draw batch (0x1814 /
0x1824) arrived. 3D mesh draws use primitive 5 (triangles); the 2D UI layer
is primitive 8 (quads).
"""
import argparse
import struct
import sys
from collections import Counter

FLIP_MASK = 0xFFFF0000
FLIP_TAG  = 0xFEAD0000

M_BEGIN_END   = 0x1808
M_VERT_BATCH  = 0x1814
M_INLINE      = 0x1818
M_IDX_OFFSET  = 0x181C
M_IDXBUF_FMT  = 0x1820
M_IDX_BATCH   = 0x1824
M_RT_HORIZ    = 0x0200
M_RT_VERT     = 0x0204
M_RT_FORMAT   = 0x0208
M_RT_PITCH    = 0x020C
M_COLOR_AOFF  = 0x0210
M_ZETA_OFF    = 0x0214
M_COLOR_TGT   = 0x0220
M_BLEND_EN    = 0x0310
M_BLEND_SF    = 0x0314
M_BLEND_DF    = 0x0318
M_BLEND_EQ    = 0x0320
M_DEPTH_TEST  = 0x0A74
M_DEPTH_FUNC  = 0x0A6C
M_DEPTH_WRITE = 0x0A70
M_CULL_EN     = 0x183C
M_CULL_FACE   = 0x1830
M_TEX_OFF     = 0x1A00
M_TEX_FMT     = 0x1A04
M_TEX_CTL0    = 0x1A0C
M_TEX_RECT    = 0x1A18

PRIM = {0: "end", 1: "points", 2: "lines", 3: "line_strip", 4: "line_loop",
        5: "triangles", 6: "tri_strip", 7: "tri_fan", 8: "quads",
        9: "quad_strip", 0x10: "polygon"}


def load_words(path):
    data = open(path, "rb").read()
    if len(data) < 4:
        raise SystemExit("empty or truncated dump")
    if data[:4] == b"RSXF":
        # 32-byte native struct header, then big-endian method words.
        if len(data) < 32:
            raise SystemExit("truncated RSXF header")
        data = data[32:]
    if len(data) % 4:
        raise SystemExit("payload is not a multiple of 4 bytes")
    return [struct.unpack_from(">I", data, i)[0] for i in range(0, len(data), 4)]


class State:
    def __init__(self):
        self.prim = 0
        self.surface = {}
        self.textures = {}
        self.depth = {}
        self.blend = {}
        self.cull = {}
        self.rt_offset = None
        self.zeta_offset = None

    def summary(self):
        t = []
        for u in sorted(self.textures):
            off, fmt, ctl = self.textures[u]
            enabled = "on" if (ctl or 0) & 0x80000000 else "off"
            t.append(f"t{u}:0x{off:08X} f{fmt:08X} {enabled}")
        return ("surf_col=0x%08X surf_zeta=0x%08X " % (
                    self.surface.get(M_COLOR_AOFF, 0) & 0x0FFFFFFF,
                    self.surface.get(M_ZETA_OFF, 0) & 0x0FFFFFFF)
                + f"fmt={self.surface.get(M_RT_FORMAT, 0):08X} "
                + f"cull={self.cull.get(M_CULL_EN, 0):X}/{self.cull.get(M_CULL_FACE, 0):X} "
                + f"depth={self.depth.get(M_DEPTH_TEST, 0):X} "
                + f"dwrite={self.depth.get(M_DEPTH_WRITE, 0):X} "
                + f"blend={self.blend.get(M_BLEND_EN, 0):X} "
                + (" ".join(t) if t else "no-tex"))


def scan(words, show_draws, top, only_prim):
    st = State()
    frame = 0
    frame_word_start = 0
    prims = Counter()
    methods = Counter()
    draws = 0
    triangles = 0
    draw_total = 0
    triangle_total = 0
    frames_reported = 0
    words_in_frame = 0

    def emit_draw(batch_method, count_field):
        # A draw batch is any dispatched 0x1814/0x1824 word, including the
        # second and later data words of a non-incrementing packet. The old
        # count only looked at the packet's base method, so a type-2 packet
        # carrying two batches was counted once (12 short on the team-select
        # capture, while the walker dispatched all of them).
        nonlocal draws, triangles, draw_total, triangle_total
        draws += 1
        draw_total += 1
        if st.prim in (5, 6, 7):
            triangles += 1
            triangle_total += 1
        if show_draws and (only_prim is None or st.prim == only_prim):
            kind = "index" if batch_method == M_IDX_BATCH else "array"
            print(f"  draw f{frame} #{draws} {kind} prim={PRIM.get(st.prim, hex(st.prim))}"
                  f" count_field={count_field} {st.summary()}")

    def flush_frame():
        # A trailing flip marker leaves an empty section; do not report it as
        # a frame. A dump that stops mid-frame still reports its partial one.
        nonlocal frames_reported
        if words_in_frame == 0:
            return
        frames_reported += 1
        ph = ", ".join(f"{PRIM.get(k, hex(k))}={v}" for k, v in prims.most_common(8)) or "no draws"
        print(f"[frame {frame}] words={words_in_frame} draws={draws} begin_end={sum(prims.values())}  {ph}")

    i, n = 0, len(words)
    while i < n:
        w = words[i]
        if (w & FLIP_MASK) == FLIP_TAG:
            flush_frame()
            frame += 1
            prims.clear()
            draws = 0
            triangles = 0
            words_in_frame = 0
            st = State()
            i += 1
            continue
        words_in_frame += 1
        typ = w >> 29
        if typ not in (0, 2):
            # Branch/NOP/unknown control word. The dump does not contain
            # JUMP/CALL/RET, so this is only seen on hand-made fixtures.
            i += 1
            continue
        count = (w >> 18) & 0x7FF
        method = w & 0x1FFC
        subch = (w >> 13) & 7
        data = words[i + 1:i + 1 + count]
        methods[(subch, method)] += 1

        for k in range(count):
            m = method + (k * 4 if typ == 0 else 0)
            v = data[k]
            if m == M_BEGIN_END:
                st.prim = v
                if v:
                    prims[v] += 1
            elif m == M_COLOR_AOFF:
                st.surface[m] = v
            elif m == M_ZETA_OFF:
                st.surface[m] = v
            elif m in (M_RT_HORIZ, M_RT_VERT, M_RT_FORMAT, M_RT_PITCH, M_COLOR_TGT):
                st.surface[m] = v
            elif m == M_DEPTH_TEST:
                st.depth[m] = v
            elif m == M_DEPTH_FUNC:
                st.depth[m] = v
            elif m == M_DEPTH_WRITE:
                st.depth[m] = v
            elif m == M_BLEND_EN or m == M_BLEND_SF or m == M_BLEND_DF or m == M_BLEND_EQ:
                st.blend[m] = v
            elif m == M_CULL_EN or m == M_CULL_FACE:
                st.cull[m] = v
            elif M_TEX_OFF <= m < M_TEX_OFF + 16 * 0x20:
                u = (m - M_TEX_OFF) // 0x20
                off, fmt, ctl = st.textures.get(u, (0, 0, 0))
                reg = (m - M_TEX_OFF) % 0x20
                if reg == 0x00:
                    off = v
                elif reg == 0x04:
                    fmt = v
                elif reg == 0x0C:
                    ctl = v
                st.textures[u] = (off, fmt, ctl)
            elif m == M_IDX_OFFSET:
                st.surface["idx_off"] = v
            elif m == M_IDXBUF_FMT:
                st.surface["idx_fmt"] = v

            if m in (M_VERT_BATCH, M_IDX_BATCH):
                emit_draw(m, count)

        # A batch header with no data words is still one draw; when it has
        # data words the per-word loop above already dispatched them.
        if count == 0 and method in (M_VERT_BATCH, M_IDX_BATCH):
            emit_draw(method, 0)
        if method == M_BEGIN_END and show_draws and count and only_prim is None:
            if data and data[0]:
                print(f"  begin f{frame} prim={PRIM.get(data[0], hex(data[0]))}")
        i += 1 + count

    flush_frame()
    print(f"[total] frames={frames_reported} words={n} draw_batches={draw_total}"
          f" triangle_batches={triangle_total}")
    print("[methods] top %d:" % top)
    for (subch, m), c in methods.most_common(top):
        print(f"   subch{subch} 0x{m:04X}  {c}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--draws", action="store_true", help="print each draw batch with the state current at it")
    ap.add_argument("--prim", type=lambda s: int(s, 0), default=None,
                    help="with --draws, only print this primitive (5 = triangles)")
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args()
    scan(load_words(a.dump), a.draws, a.top, a.prim)


if __name__ == "__main__":
    sys.exit(main())
