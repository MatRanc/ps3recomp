#!/usr/bin/env python3
"""Summarise a PS3RECOMP_C690_TAIL_CAPTURE spool.

Usage:
  tools/c690_tail_report.py DIR [--json] [--extract OUT]

The tail spool (runtime/spu/c690_corpus.c) holds one IN/OUT pair per sampled
tail command: full 256 KiB local store + all 128 GPRs + 24 meta words + a
16-byte-line write bitmap. `tail_hist.txt`, written periodically by the same
module, has the exact per-format-key call and time totals for the sampled
window (including commands whose pairs were not kept).

A 0x21 key is (sub-selector target LSA << 8) | (r6 & 0x1f) at that target:
  * low three bits = selector 1 or 7;
  * bit 4 = the A730/A3E0 (clear) versus BB80/BB98 (set) format path.
The 0x25/0x26/0x2A keys are the opcode.
"""
import argparse
import collections
import json
import os
import struct
import sys

MAGIC = b'C69STG01'
T_IN, T_OUT = 9, 10
META_WORDS = 24
BITMAP_BYTES = 2048
LS_SIZE = 256 * 1024
GPR_SIZE = 128 * 16
HDR = struct.Struct('<IIQII')

OP_NAME = {0x21: '0x21', 0x25: '0x25', 0x26: '0x26', 0x2A: '0x2A'}


def decode_key(key):
    lsa = key >> 8
    if lsa in (0x13178, 0x13140, 0x131A0, 0x131F8):
        sel = key & 7
        if key & 0x10:
            path = 'BB80' if sel == 1 else 'BB98'
        else:
            path = 'A730' if sel == 1 else 'A3E0'
        return f'0x21 sel{sel} {path} @0x{lsa:05X}'
    op = key & 0xFF
    return OP_NAME.get(op, f'0x{op:02X}')


def changed_lines(bitmap):
    return [i for i in range(LS_SIZE // 16)
            if bitmap[i >> 3] & (1 << (i & 7))]


def parse_spool(path):
    with open(path, 'rb') as f:
        blob = f.read()
    if blob[:8] != MAGIC:
        raise ValueError(f'{path}: bad magic')
    off = 12
    pairs = {}
    while off + HDR.size <= len(blob):
        rtype, size, ea, lsa, aux = HDR.unpack_from(blob, off)
        off += HDR.size
        payload = blob[off:off + size]
        off += size
        if len(payload) != size:
            break
        meta = struct.unpack_from('<%dI' % META_WORDS, payload, 0)
        base = META_WORDS * 4
        bitmap = payload[base:base + BITMAP_BYTES]
        ls = payload[base + BITMAP_BYTES:base + BITMAP_BYTES + LS_SIZE]
        gpr = payload[base + BITMAP_BYTES + LS_SIZE:]
        ent = {
            'type': rtype, 'lsa': lsa, 'spu': aux, 'frame': ea & 0xFFFFFFFF,
            'op': meta[4], 'key': meta[5], 'key_lsa': meta[6], 'seq': meta[7],
            'gpr3': meta[8], 'gpr6': meta[11],
            'entry_ns': (meta[21] << 32) | meta[20],
            'exit_ns': (meta[23] << 32) | meta[22],
            'cmd': meta[12:20], 'ls': ls, 'gpr': gpr, 'bitmap': bitmap,
        }
        pairs.setdefault(meta[7], {})['in' if rtype == T_IN else 'out'] = ent
    return {k: v for k, v in pairs.items() if 'in' in v and 'out' in v}


def read_hist(path):
    hist = {}
    if not os.path.exists(path):
        return hist
    with open(path) as f:
        lines = f.read().splitlines()
    for line in lines[1:]:
        parts = line.split()
        if len(parts) == 4:
            hist[int(parts[0], 16)] = {'calls': int(parts[1]),
                                       'ns': int(parts[2]),
                                       'pairs': int(parts[3])}
    return hist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--extract')
    args = ap.parse_args()

    d = args.dir
    spools = sorted(f for f in os.listdir(d) if f.startswith('tail_') and
                    f.endswith('.bin'))
    pairs = {}
    errors = []
    for name in spools:
        try:
            pairs.update(parse_spool(os.path.join(d, name)))
        except Exception as exc:  # noqa: BLE001
            errors.append((name, str(exc)))

    hist = read_hist(os.path.join(d, 'tail_hist.txt'))
    by_key = collections.defaultdict(list)
    for seq, p in sorted(pairs.items()):
        by_key[p['in']['key']].append(p)

    total_calls = sum(v['calls'] for v in hist.values())
    total_ns = sum(v['ns'] for v in hist.values())

    rows = []
    for key, plist in sorted(by_key.items(), key=lambda kv: -len(kv[1])):
        h = hist.get(key, {})
        calls = h.get('calls', len(plist))
        ns = h.get('ns', sum(p['out']['exit_ns'] - p['in']['entry_ns'] for p in plist))
        avg = ns / calls / 1000.0 if calls else 0.0
        lines = sorted({ln for p in plist for ln in changed_lines(p['in']['bitmap'])})
        rows.append({
            'key': f'0x{key:08X}', 'class': decode_key(key),
            'pairs': len(plist), 'calls': calls, 'ns': ns, 'avg_us': avg,
            'job_pct': (100.0 * calls / total_calls) if total_calls else 0.0,
            'time_pct': (100.0 * ns / total_ns) if total_ns else 0.0,
            'write_lines': len(lines),
            'write_range': [f'0x{lines[0] * 16:05X}', f'0x{(lines[-1] + 1) * 16:05X}'] if lines else None,
        })

    if args.extract:
        os.makedirs(args.extract, exist_ok=True)
        for key, plist in by_key.items():
            for p in plist:
                tag = f"{p['in']['op']:02X}_{key:08X}_{p['in']['seq']:06d}_f{p['in']['frame']}"
                for side in ('in', 'out'):
                    base = os.path.join(args.extract, f'{tag}_{side}')
                    with open(base + '_ls.bin', 'wb') as f:
                        f.write(p[side]['ls'])
                    with open(base + '_gpr.bin', 'wb') as f:
                        f.write(p[side]['gpr'])
                    with open(base + '_meta.json', 'w') as f:
                        json.dump({k: v for k, v in p[side].items()
                                   if k not in ('ls', 'gpr', 'bitmap')}, f)

    out = {'dir': d, 'spools': spools, 'errors': errors,
           'total_calls': total_calls, 'total_ns': total_ns,
           'pairs': len(pairs), 'keys': rows}
    if args.json:
        print(json.dumps(out, indent=2))
    else:
        print(f"tail spool: {d}  spools={len(spools)} pairs={len(pairs)} "
              f"keys={len(rows)} calls={total_calls} time={total_ns/1e6:.1f}ms")
        if errors:
            for name, exc in errors:
                print(f'  ERROR {name}: {exc}')
        print(f"{'key':>12} {'class':<34} {'pairs':>5} {'calls':>9} "
              f"{'jobs%':>6} {'time_ms':>10} {'time%':>6} {'avg_us':>7} "
              f"{'wr_lines':>8} write_range")
        for r in rows:
            print(f"{r['key']:>12} {r['class']:<34} {r['pairs']:5d} "
                  f"{r['calls']:9d} {r['job_pct']:6.2f} {r['ns']/1e6:10.2f} "
                  f"{r['time_pct']:6.2f} {r['avg_us']:7.2f} "
                  f"{r['write_lines']:8d} {r['write_range'] or ''}")
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
