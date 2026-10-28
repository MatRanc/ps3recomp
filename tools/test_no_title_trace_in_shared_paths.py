#!/usr/bin/env python3
"""No MLB The Show 16 trace probes in the shared runtime.

MLB_SEMA_TRACE was a temporary mlb16 investigation: getenv-gated probes, with MLB 16's own FIFO and ring addresses
hard-coded, in the SPU DMA PUT path and in the FIFO walker's method dispatch, JUMP and CALL handling, which every
title runs. The variable also counted as a DMA diagnostic, so setting it took every transfer off the fast path.
Such a probe belongs in a title's port, not in runtime/ or libs/."""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
probe=re.compile(r'getenv\s*\(\s*"MLB_|"MLB_SEMA_TRACE"')
found=[]
for top in ('runtime','libs','include'):
    for path in sorted((ROOT/top).rglob('*')):
        if path.suffix not in ('.c','.h','.cpp','.hpp','.m','.mm') or '/tests/' in path.as_posix():
            continue
        for n,line in enumerate(path.read_text(errors='replace').splitlines(),1):
            if probe.search(line):
                found.append(f'{path.relative_to(ROOT)}:{n}: {line.strip()}')
if found:
    raise SystemExit('FAIL: MLB-only trace probes in the shared runtime:\n'+'\n'.join(found))
print('PASS: no MLB-only trace probes in runtime/, libs/ or include/')
