#!/usr/bin/env python3
"""The c690 tail-stage capture hook records IN/OUT stage pairs, the 0x21 format
key and the LS write bitmap (runtime/spu/c690_corpus.c).

Builds runtime/spu/tests/test_c690_tail_capture.c with the real c690_corpus.c,
runs the simulated command boundaries under a temporary capture directory and
parses the spool. Also checks that the hook is inert when the env var is unset.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
RUNTIME = root / 'runtime/spu'
MAGIC = b'C69STG01'
META_WORDS = 24
BITMAP_BYTES = 2048
LS_SIZE = 256 * 1024
GPR_SIZE = 128 * 16
T_IN, T_OUT = 9, 10


def parse_spool(path):
    blob = path.read_bytes()
    if blob[:8] != MAGIC:
        raise AssertionError(f'{path}: bad magic {blob[:8]!r}')
    off = 12
    pairs = {}
    while off + 24 <= len(blob):
        rtype, size, ea, lsa, aux = struct.unpack_from('<IIQII', blob, off)
        off += 24
        payload = blob[off:off + size]
        off += size
        assert len(payload) == size, f'{path}: short payload'
        meta = struct.unpack_from('<%dI' % META_WORDS, payload, 0)
        bitmap = payload[META_WORDS * 4:META_WORDS * 4 + BITMAP_BYTES]
        ls = payload[META_WORDS * 4 + BITMAP_BYTES:
                     META_WORDS * 4 + BITMAP_BYTES + LS_SIZE]
        gpr = payload[META_WORDS * 4 + BITMAP_BYTES + LS_SIZE:]
        ent = {
            'type': rtype, 'lsa': lsa, 'spu': aux, 'op': meta[4], 'key': meta[5],
            'key_lsa': meta[6], 'seq': meta[7], 'ls': ls, 'gpr': gpr,
            'bitmap': bitmap,
        }
        seq = meta[7]
        pairs.setdefault(seq, {})['in' if rtype == T_IN else 'out'] = ent
    return {k: v for k, v in pairs.items() if 'in' in v and 'out' in v}


def changed_lines(bitmap):
    return [i for i in range(LS_SIZE // 16)
            if bitmap[i >> 3] & (1 << (i & 7))]


def run(binary, mode, env):
    full = os.environ.copy()
    full.pop('PS3RECOMP_C690_TAIL_CAPTURE', None)
    full.pop('PS3RECOMP_C690_TAIL_PAIRS', None)
    full.pop('PS3RECOMP_C690_TAIL_STRIDE', None)
    full.update(env)
    out = subprocess.run([str(binary), mode], capture_output=True, text=True,
                         env=full, timeout=60)
    if out.returncode:
        raise AssertionError(f'{mode}: exit {out.returncode}\n{out.stdout}\n{out.stderr}')
    return out.stdout + out.stderr


def main():
    with tempfile.TemporaryDirectory(prefix='c690-tail-') as tmp:
        tmp = Path(tmp)
        binary = tmp / 'test_c690_tail_capture'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu17', '-O1',
            '-I' + str(RUNTIME), '-I' + str(root / 'include'),
            '-I' + str(root / 'runtime/platform'),
            str(RUNTIME / 'tests/test_c690_tail_capture.c'),
            str(RUNTIME / 'c690_corpus.c'), '-o', str(binary),
        ], check=True)

        # Off by default: no directory is created and no spool is written.
        cap = tmp / 'off'
        out = run(binary, 'basic', {'PS3RECOMP_C690_TAIL_CAPTURE': ''})
        assert 'tail_on=0' in out, out
        assert not cap.exists() and not list(tmp.glob('tail_*.bin'))

        # One pair per stage, every command sampled.
        cap = tmp / 'basic'
        out = run(binary, 'basic', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '1',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
        })
        assert 'tail_on=1' in out, out
        files = sorted(cap.glob('tail_*.bin'))
        assert len(files) == 1, files
        pairs = parse_spool(files[0])
        assert len(pairs) == 3, pairs
        p = [pairs[i] for i in sorted(pairs)]
        assert [x['in']['op'] for x in p] == [0x21, 0x25, 0x2A], p
        assert [x['in']['lsa'] for x in p] == [0x13140, 0x132E0, 0x10F78], p
        # 0x21: selector 1, target 0x13140 -> key = (sel | lsa<<8).
        assert p[0]['in']['key'] == 1 | (0x13140 << 8), hex(p[0]['in']['key'])
        assert p[0]['in']['key_lsa'] == 0x13140
        assert p[1]['in']['key'] == 0x25
        assert p[2]['in']['key'] == 0x2A
        # The 0x21 key comes from r6 at the sub-selector body entry.
        g6 = struct.unpack_from('<I', p[0]['in']['gpr'], 6 * 16)[0]
        assert g6 == 1, hex(g6)
        # Each stage changed exactly its own 16-byte line, recorded in both.
        for pair, line, byte in ((p[0], 0x1F750 // 16, 0x22),
                                 (p[1], 0x204C0 // 16, 0x33),
                                 (p[2], 0x2F870 // 16, 0x44)):
            assert changed_lines(pair['in']['bitmap']) == [line], pair
            assert pair['in']['ls'][line * 16] == 0x11
            assert pair['out']['ls'][line * 16] == byte
        hist = (cap / 'tail_hist.txt').read_text()
        assert '0x01314001 1 ' in hist and '0x0000002A 1 ' in hist, hist

        # Per-key cap: two different 0x21 selectors, one pair each.
        cap = tmp / 'keys'
        run(binary, 'keys', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '1',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
        })
        pairs = parse_spool(sorted(cap.glob('tail_*.bin'))[0])
        keys = sorted(x['in']['key'] for x in pairs.values())
        assert keys == [1 | (0x13140 << 8), 7 | (0x131A0 << 8)], [hex(k) for k in keys]

        # Cap is checked before the 256 KiB snapshot: six same-key commands
        # with a cap of two keep two pairs and take exactly two snapshots.
        cap = tmp / 'cap'
        out = run(binary, 'cap', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '2',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
        })
        pairs = parse_spool(sorted(cap.glob('tail_*.bin'))[0])
        assert len(pairs) == 2, pairs
        assert 'snapshots=2' in out, out
        hist = (cap / 'tail_hist.txt').read_text()
        assert ('0x%08X 6 ' % (1 | (0x13140 << 8))) in hist, hist

        # A trigger file that has not appeared leaves the capture idle.
        cap = tmp / 'no-trigger'
        run(binary, 'basic', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '8',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
            'PS3RECOMP_C690_TAIL_TRIGGER': str(tmp / 'never'),
        })
        assert not list(cap.glob('tail_*.bin')), list(cap.glob('tail_*.bin'))

        # An existing trigger file arms it before the first command.
        cap = tmp / 'triggered'
        trig = tmp / 'armed'
        trig.write_text('')
        run(binary, 'basic', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '8',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
            'PS3RECOMP_C690_TAIL_TRIGGER': str(trig),
        })
        assert len(parse_spool(sorted(cap.glob('tail_*.bin'))[0])) == 3
        # The tail hook only polls the trigger; it never consumes or deletes
        # it, so a second waiter (the walker capture) cannot race it away.
        assert trig.exists(), 'tail capture deleted its own trigger'

        # Two 0x2A commands back to back: the function wrapper closes each
        # one at its own return, so neither pair contains the other's write.
        cap = tmp / 'exit'
        run(binary, 'exit', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '8',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
        })
        pairs = parse_spool(sorted(cap.glob('tail_*.bin'))[0])
        assert len(pairs) == 2, pairs
        p = [pairs[i] for i in sorted(pairs)]
        assert [x['in']['op'] for x in p] == [0x2A, 0x2A], p
        assert [x['in']['lsa'] for x in p] == [0x10F78, 0x10F78], p
        assert [x['in']['key'] for x in p] == [0x2A, 0x2A], p
        assert changed_lines(p[0]['in']['bitmap']) == [0x2F87], p[0]
        assert changed_lines(p[1]['in']['bitmap']) == [0x2F88], p[1]
        assert p[0]['out']['ls'][0x2F87 * 16] == 0x44, p[0]
        assert p[1]['out']['ls'][0x2F88 * 16] == 0x55, p[1]
        hist = (cap / 'tail_hist.txt').read_text()
        assert '0x0000002A 2 ' in hist, hist

        # A730 heavy-body leaf (0xB830) through its own capture wrapper.
        cap = tmp / 'a730'
        run(binary, 'a730', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '1',
            'PS3RECOMP_C690_TAIL_STRIDE': '1',
        })
        pairs = parse_spool(sorted(cap.glob('tail_*.bin'))[0])
        assert len(pairs) == 1, pairs
        p = list(pairs.values())[0]
        assert p['in']['op'] == 0xA7 and p['in']['lsa'] == 0xB830, p['in']
        assert p['in']['key'] == 0xA7, hex(p['in']['key'])
        assert changed_lines(p['in']['bitmap']) == [0x2F83], p['in']
        assert p['out']['ls'][0x2F83 * 16] == 0x66, p['out']
        hist = (cap / 'tail_hist.txt').read_text()
        assert '0x000000A7 1 ' in hist, hist

        # Stride 2 samples the 1st and 3rd stage opens, not the 2nd.
        cap = tmp / 'stride'
        run(binary, 'basic', {
            'PS3RECOMP_C690_TAIL_CAPTURE': str(cap),
            'PS3RECOMP_C690_TAIL_PAIRS': '8',
            'PS3RECOMP_C690_TAIL_STRIDE': '2',
        })
        pairs = parse_spool(sorted(cap.glob('tail_*.bin'))[0])
        assert sorted(x['in']['op'] for x in pairs.values()) == [0x21, 0x2A], pairs

    print('PASS: c690 tail capture IN/OUT pairs, format key, bitmap, cap, '
          'stride, 0x2A function exit, A730 leaf, non-consuming trigger, off')
    return 0


if __name__ == '__main__':
    sys.exit(main())
