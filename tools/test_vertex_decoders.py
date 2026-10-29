#!/usr/bin/env python3
"""The RSX vertex decoders, every SIMD build of them, against their scalar references.

libs/video/tests/test_vertex_fetch_exhaustive.c checks every half, unorm8, snorm16, uint8 and sint16 value through
rsx_vertex_fetch_many bit for bit; libs/video/tests/test_vertex_fetch_direct.c holds rsx_vertex_fetch_many and
_stream against the converter they replaced on random layouts, formats and reference sets;
libs/video/tests/test_vertex_remap.c checks rsx_vertex_remap_build. Nothing built them, and rsx_vertex_compact.c
picks its SIMD loops by the instruction set it is compiled for, which no build passed, so those loops were never
compiled in CI at all. On x86 the tests run scalar, -mssse3, -msse4.1 (the runtime's flags) and, where the host
has AVX2 and F16C, -march=x86-64-v3 (a Zen+ or Haswell host built -march=native); elsewhere with the compiler's
default (NEON on arm64)."""
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
if x86 and os.path.exists('/proc/cpuinfo'):
    flags=set(open('/proc/cpuinfo').read().split())
    if {'avx2','f16c','fma','bmi2','movbe'}<=flags:
        variants.append(['-march=x86-64-v3'])
video=root/'libs/video'
with tempfile.TemporaryDirectory(prefix='vertex-decoders-') as tmp:
    for test in ('test_vertex_fetch_exhaustive','test_vertex_fetch_direct','test_vertex_remap'):
        for flags in variants:
            binary=Path(tmp)/(test+''.join(flags).replace('-','_').replace('.',''))
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-Wall','-Wextra',
                '-Wno-unused-parameter',*flags,'-I'+str(root/'include'),'-I'+str(video),
                '-I'+str(video/'tests'),
                str(video/'tests'/(test+'.c')),str(video/'rsx_vertex_compact.c'),str(video/'rsx_dispatch.c'),
                '-lm','-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],timeout=120,capture_output=True,text=True)
            if result.returncode:
                print(result.stdout[-2000:],result.stderr[-2000:])
                raise SystemExit(f'FAIL: {test} {" ".join(flags) or "(default flags)"}')
            print(f'{test} {" ".join(flags) or "(default flags)"}: {result.stdout.strip().splitlines()[-1]}')
print('PASS: vertex decoders and remap, every SIMD build')
