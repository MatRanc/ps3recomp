#!/usr/bin/env python3
"""cellFs short/failed write predicate (include/ps3emu/fs_write_check.h)."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='fs-write-check-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'),
                    str(root / 'tests/test_fs_write_check.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
