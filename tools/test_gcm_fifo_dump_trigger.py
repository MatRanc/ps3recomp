#!/usr/bin/env python3
"""A FIFO dump armed for a later scene starts on the first flip after the
live PS3RECOMP_DUMP_FIFO_TRIGGER write
(libs/video/tests/test_gcm_fifo_dump_trigger.c, which compiles cellGcmSys.c
in and links the real ps3_live_ctl.c)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM FIFO dump fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-fifo-dump-trigger-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_fifo_dump_trigger.c'),
                    str(root / 'libs/video/ps3_live_ctl.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_gcm_fifo_dump_trigger failed')
