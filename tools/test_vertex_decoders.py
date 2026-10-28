#!/usr/bin/env python3
"""The RSX vertex decoders, every SIMD build of them, against their scalar references.

libs/video/tests/test_vertex_fetch_exhaustive.c checks every half, unorm8, snorm16, uint8 and sint16 value through
rsx_vertex_fetch_many bit for bit; libs/video/tests/test_vertex_remap.c checks rsx_vertex_remap_build. Nothing
built either, and rsx_vertex_compact.c picks its SSSE3 and SSE4.1 decoders by the instruction set it is compiled
for, which no build passed, so those decoders were never compiled in CI at all. On x86 both tests run scalar,
-mssse3 and -msse4.1; elsewhere with the compiler's default (NEON on arm64)."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
x86=platform.machine().lower() in ('x86_64','amd64','i386','i686')
variants=[[],['-mssse3'],['-msse4.1']] if x86 else [[]]
video=root/'libs/video'
with tempfile.TemporaryDirectory(prefix='vertex-decoders-') as tmp:
    for test in ('test_vertex_fetch_exhaustive','test_vertex_remap'):
        for flags in variants:
            binary=Path(tmp)/(test+''.join(flags).replace('-','_').replace('.',''))
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-Wall','-Wextra',
                '-Wno-unused-parameter',*flags,'-I'+str(root/'include'),'-I'+str(video),
                str(video/'tests'/(test+'.c')),str(video/'rsx_vertex_compact.c'),str(video/'rsx_dispatch.c'),
                '-lm','-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],timeout=120,capture_output=True,text=True)
            if result.returncode:
                print(result.stdout[-2000:],result.stderr[-2000:])
                raise SystemExit(f'FAIL: {test} {" ".join(flags) or "(default flags)"}')
            print(f'{test} {" ".join(flags) or "(default flags)"}: {result.stdout.strip().splitlines()[-1]}')
print('PASS: vertex decoders and remap, every SIMD build')
