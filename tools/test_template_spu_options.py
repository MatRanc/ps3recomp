#!/usr/bin/env python3
"""The template's PS3RECOMP_SPU_FAST_FLOAT / PS3RECOMP_SPU_FAST_LS reach the fast paths they name.

Both paths are chosen by the instruction set an SPU TU is compiled for (FMA, SSSE3 on x86), and the template passed
only the defines, so on a default build both options selected nothing while FAST_LS still took the local-store
diagnostics away. This builds a stand-in lifted SPU source with the template's own SPU source rule, which fails to
compile unless each option that is on selects its path; and checks that the headers refuse a define they cannot
honour."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
if not shutil.which('cmake'):
    if os.environ.get('CI'): raise SystemExit('CMake is required for this CI regression')
    print('SKIP: CMake is unavailable'); raise SystemExit(0)
X86=platform.machine().lower() in ('x86_64','amd64','i386','i686')
text=(ROOT/'templates/project/CMakeLists.txt').read_text()
start=text.index('# PS3RECOMP_SPU_FAST_FLOAT:')
end=text.index('# app.rc carries',start)
rule=text[start:end]

FIXTURE=r'''
#include "spu_helpers.h"
#if defined(WANT_FAST_FLOAT) && !(defined(SPU_FAST_FLOAT) && defined(__FMA__))
#error "PS3RECOMP_SPU_FAST_FLOAT=ON did not select the FMA float path"
#endif
#if defined(WANT_FAST_LS) && !(defined(SPU_LIFTED_FAST_LS) && (defined(__SSSE3__) || defined(__ARM_NEON)))
#error "PS3RECOMP_SPU_FAST_LS=ON did not select the vector local-store path"
#endif
#if !defined(WANT_FAST_FLOAT) && defined(SPU_FAST_FLOAT)
#error "SPU_FAST_FLOAT defined with the option off"
#endif
#if !defined(WANT_FAST_LS) && defined(SPU_LIFTED_FAST_LS)
#error "SPU_LIFTED_FAST_LS defined with the option off"
#endif
int main(void) { return 0; }
'''

def configure_and_build(root,name,fast_float,fast_ls):
    project=root/name; project.mkdir()
    (project/'spu_fixture.c').write_text(FIXTURE)
    (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\n'
        'project(SpuOptionFixture LANGUAGES C CXX)\n'
        'set(PS3RECOMP_DIR "'+ROOT.as_posix()+'")\n'
        'set(SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spu_fixture.c")\n'+rule+
        'add_executable(spu_fixture ${SPU_SOURCES})\n'
        'target_include_directories(spu_fixture PRIVATE "${PS3RECOMP_DIR}/include")\n'
        'if(PS3RECOMP_SPU_FAST_FLOAT)\n  target_compile_definitions(spu_fixture PRIVATE WANT_FAST_FLOAT)\nendif()\n'
        'if(PS3RECOMP_SPU_FAST_LS)\n  target_compile_definitions(spu_fixture PRIVATE WANT_FAST_LS)\nendif()\n')
    build=root/(name+'-build')
    generator=['-G','Ninja'] if shutil.which('ninja') else []
    cfg=subprocess.run(['cmake','-S',str(project),'-B',str(build),*generator,
        '-DPS3RECOMP_SPU_FAST_FLOAT='+('ON' if fast_float else 'OFF'),
        '-DPS3RECOMP_SPU_FAST_LS='+('ON' if fast_ls else 'OFF')],capture_output=True,text=True)
    if cfg.returncode: return 'configure',cfg.stdout+cfg.stderr
    b=subprocess.run(['cmake','--build',str(build)],capture_output=True,text=True)
    if b.returncode: return 'build',b.stdout+b.stderr
    return 'ok',''

def expect(what,got,want,log=''):
    if got!=want: raise SystemExit(f'FAIL: {what}: {got}, want {want}\n{log}')

with tempfile.TemporaryDirectory(prefix='template-spu-options-') as tmp:
    root=Path(tmp)
    state,log=configure_and_build(root,'off',False,False); expect('both options off',state,'ok',log)
    state,log=configure_and_build(root,'fast-ls',False,True); expect('PS3RECOMP_SPU_FAST_LS=ON',state,'ok',log)
    state,log=configure_and_build(root,'fast-float',True,False)
    if X86:
        expect('PS3RECOMP_SPU_FAST_FLOAT=ON',state,'ok',log)
    else:   # an x86 path: refused at configure rather than silently selecting nothing
        expect('PS3RECOMP_SPU_FAST_FLOAT=ON off x86',state,'configure',log)

    if X86:
        # The headers refuse a define compiled without the instruction set its path needs.
        cc=os.environ.get('CC','cc')
        src=root/'guard.c'; src.write_text('#include "spu_helpers.h"\nint main(void) { return 0; }\n')
        for define,flag,message in (('SPU_LIFTED_FAST_LS','-mno-ssse3','needs SSSE3'),
                                    ('SPU_FAST_FLOAT','-mno-fma','needs FMA')):
            r=subprocess.run([cc,'-std=gnu17','-fsyntax-only','-D'+define,flag,'-I'+str(ROOT/'include'),
                '-I'+str(ROOT/'runtime/spu'),str(src)],capture_output=True,text=True)
            if r.returncode==0 or message not in r.stderr:
                raise SystemExit(f'FAIL: -D{define} {flag} was not refused by the headers\n{r.stderr}')
print('PASS: the template\'s SPU fast-path options select their paths, and the headers refuse one that cannot')
