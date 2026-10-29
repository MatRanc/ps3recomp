#!/usr/bin/env python3
"""The FIFO walker passes over NOP runs at once, in order, stopping at put and
at an IO page end, and leaves the trace ring and park as a word-at-a-time walk
would (libs/video/tests/test_gcm_nop_runs.c, which compiles cellGcmSys.c in).
Built with the draw engine off and on; the engine build also runs with
TCONST_FIFO=1, which keeps NOPs on the per-word path."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM NOP-run fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-nop-runs-') as tmp:
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    for engine, envs in (('0', [{}]), ('1', [{}, {'TCONST_FIFO': '1'}])):
        binary = Path(tmp) / ('test' + engine)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-ffunction-sections', '-fdata-sections', '-DENGINE_ON=' + engine,
                        '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                        str(root / 'libs/video/tests/test_gcm_nop_runs.c'), *linker,
                        '-o', str(binary)], check=True)
        for extra in envs:
            env = {k: v for k, v in os.environ.items() if k != 'TCONST_FIFO'}
            env.update(extra)
            result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True, env=env)
            print(result.stdout.strip())
            if result.returncode:
                print(result.stderr[-2000:])
                raise SystemExit('test_gcm_nop_runs failed')
