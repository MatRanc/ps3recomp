#!/usr/bin/env python3
"""The libs/video/tests fixtures that build and pass on a POSIX host with a C
compiler and no GPU. Each is compiled on its own against the sources it needs
and run; any failure fails the script.

Fixtures in libs/video/tests that are not listed here, and why:

  test_gcm_flip_status.c      builds, fails: it expects cellGcmTickFlip to complete a
                              flip after a bare cellGcmResetFlipStatus, but the tick now
                              completes only a flip that executed in the command stream
                              (or a direct one), so the first assert never holds.
  test_rsx_fifo.c             builds with host stubs, fails: 0 of 6 methods dispatched;
                              its build line (-std=c11, rsx_commands.c alone) predates
                              rsx_commands.c needing guest memory and the GCM IO map.
  test_live_draw_*.c (5)      Windows only: they drive the D3D12 rsx_live_draw.c through
                              live_draw_test_harness.h and need a GPU and the full runtime.
  test_shader_msl.c,          need glslang / spirv-cross; CMake builds them behind
  test_shader_spirv.c         PS3RECOMP_MSL_TRANSLATION / PS3RECOMP_SPIRV_TRANSLATION.
  test_*.m (11)               Metal, macOS only (some are wired as CMake targets).
  validate_*.c                take a directory of captured shader programs as argument.
  bench_vertex_fetch.c        a benchmark, not a test.
  rsx_harness_main.c          a visual D3D12 harness.
"""
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the video fixtures link with a POSIX cc')
    raise SystemExit(0)

V = 'libs/video/'
# the draw engine, command processor and decompilers
ENGINE = [V + s for s in (
    'rsx_commands.c', 'rsx_dispatch.c', 'rsx_draw_engine.c', 'rsx_texture_layout.c',
    'rsx_vertex_compact.c', 'rsx_vertex_fetch.c', 'rsx_vp_decompiler.c',
    'rsx_fp_decompiler.c', 'rsx_null_backend.c', 'rsx_live_draw_config.c')]
# fixtures that #include rsx_draw_engine.c themselves
DISPATCH = [V + s for s in (
    'rsx_dispatch.c', 'rsx_vertex_compact.c', 'rsx_texture_layout.c',
    'rsx_vp_decompiler.c', 'rsx_fp_decompiler.c')]
# cellGcmSys.c is #included; its pump thread uses the CreateThread shim
GCM = ['runtime/platform/win32_compat.c']

FIXTURES = {
    'test_depth24_rounding': ENGINE,
    'test_engine_program_identity': ENGINE,
    'test_fp_decompiler': ENGINE,
    'test_fp_export_cache': ENGINE,
    'test_rsx_command_texture_offset': ENGINE,
    'test_rsx_framebuffer_srgb': ENGINE,
    'test_rsx_primitives': ENGINE,
    'test_rsx_texture_gamma': ENGINE,
    'test_rsx_texture_offset': ENGINE,
    'test_rsx_transform_constants': ENGINE,
    'test_rsx_vp_run': ENGINE,
    'test_shadow_engine': ENGINE,
    'test_shadow_metadata': ENGINE,
    'test_texture_layout': ENGINE,
    'test_user_command': ENGINE,
    'test_vertex_fetch_exhaustive': ENGINE,
    'test_vertex_fetch_plan': ENGINE,
    'test_vertex_remap': ENGINE,
    'test_vp_const_usage': ENGINE,
    'test_color_feedback_engine': DISPATCH,
    'test_tex_memo': DISPATCH,
    'test_vpc_range': DISPATCH,
    'test_gcm_user_command': GCM,
    'test_gcm_fifo_user_command': GCM,
    'test_gcm_flip_wrap': [],
    'test_nv3089_engine_read': [],
}


def run(name, extra, tmp):
    exe = Path(tmp) / name
    link = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-lm']
    cmd = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
           '-ffunction-sections', '-fdata-sections',
           '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
           '-I' + str(root / 'libs/video/tests'), '-I' + str(root / 'runtime/platform'),
           str(root / 'libs/video/tests' / (name + '.c')), *[str(root / e) for e in extra],
           *link, '-pthread', '-o', str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode:
        return name, 'build failed', (b.stdout + b.stderr)[-1500:]
    try:
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return name, 'timed out', ''
    if r.returncode:
        return name, 'exit %d' % r.returncode, (r.stdout + r.stderr)[-1500:]
    return name, None, ''


with tempfile.TemporaryDirectory(prefix='video-fixtures-') as tmp:
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        results = list(pool.map(lambda kv: run(kv[0], kv[1], tmp), FIXTURES.items()))
bad = [r for r in results if r[1]]
for name, why, out in results:
    print(('FAIL ' if why else 'PASS ') + name + (' (%s)' % why if why else ''))
    if why:
        print(out)
if bad:
    raise SystemExit('%d of %d video fixtures failed' % (len(bad), len(results)))
print('%d video fixtures passed' % len(results))
