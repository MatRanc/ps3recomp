#!/usr/bin/env python3
"""
Generate a synthetic RSX FIFO dump (.bin) for testing and benchmarking replay_rsx.
Emits valid RSX method packets with headers, viewport/scissor setup, surface format,
register runs, vertex draws, and flip markers.
"""

import struct
import sys
from pathlib import Path

def generate_fifo_stream(out_path: str, num_draws: int = 500):
    words = []

    def emit(w: int):
        words.append(w & 0xFFFFFFFF)

    def method(m: int, args: list, non_inc: bool = False, subchannel: int = 0):
        count = len(args)
        hdr = ((2 if non_inc else 0) << 29) | (count << 18) | (subchannel << 13) | m
        emit(hdr)
        for a in args:
            emit(a)

    # 1. Surface setup (M_SURFACE_FORMAT = 0x0148)
    method(0x0148, [0x148]) # ARGB8
    # M_COLOR_A_PITCH = 0x0180
    method(0x0180, [1024])

    # 2. Viewport & Scissor setup
    # M_VIEWPORT_HORIZONTAL = 0x0300
    method(0x0300, [(1280 << 16) | 0]) # x=0, w=1280
    # M_VIEWPORT_VERTICAL = 0x0304
    method(0x0304, [(720 << 16) | 0])  # y=0, h=720
    # M_SCISSOR_HORIZONTAL = 0x0308
    method(0x0308, [(1280 << 16) | 0])
    # M_SCISSOR_VERTICAL = 0x030C
    method(0x030C, [(720 << 16) | 0])

    # 3. Clear (M_CLEAR_COLOR_VALUE = 0x1D90, M_CLEAR_BUFFERS = 0x1D94)
    method(0x1D90, [0xFF112233]) # ARGB clear color
    method(0x1D94, [0xF0])       # Clear mask: color A + depth

    # 4. Generate draw calls with varying state runs
    for i in range(num_draws):
        # Vertex constants / transform updates (M_TRANSFORM_CONSTANTS = 0x1E00)
        c_base = 0x1E00 + ((i % 16) * 16)
        method(c_base, [0x3F800000, 0x00000000, 0x00000000, 0x3F800000])

        # Register run simulating shader uniform updates
        reg_base = 0x0380 + ((i % 8) * 4)
        method(reg_base, [i * 7, i * 13, i * 17, i * 23])

        # Draw call (M_VERTEX_BEGIN_END = 0x1808)
        # 4 = TRIANGLES
        method(0x1808, [4])

        # Inline vertex data or draw arrays
        if (i % 2) == 0:
            # Inline vertices (M_INLINE_ARRAY = 0x1818)
            method(0x1818, [0x3F000000, 0x00000000, 0x00000000, 0x3F800000], non_inc=True)
            method(0x1818, [0x00000000, 0x3F000000, 0x00000000, 0x3F800000], non_inc=True)
            method(0x1818, [0x3F000000, 0x3F000000, 0x00000000, 0x3F800000], non_inc=True)
        else:
            # Draw arrays (M_VB_VERTEX_BATCH = 0x1814)
            method(0x1814, [(2 << 24) | 0]) # count=3, first=0

        # End primitive (M_VERTEX_BEGIN_END = 0)
        method(0x1808, [0])

        # Periodically emit fence reference (0x0050 = NV406E_SET_REFERENCE)
        if (i % 50) == 0:
            method(0x0050, [i + 1])

    # 5. Flip marker (GCM_FLIP_MARKER = 0xFEAD0100)
    emit(0xFEAD0100 | 1)

    # Write output binary
    data = bytearray()
    for w in words:
        data.extend(struct.pack('>I', w)) # Big-endian 32-bit words

    out_file = Path(out_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_bytes(data)
    print(f"Generated {len(words)} method words ({len(data)} bytes, {num_draws} draws) -> {out_path}")

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'tests/fixtures/sample_fifo.bin'
    num_draws = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    generate_fifo_stream(out_path, num_draws)
