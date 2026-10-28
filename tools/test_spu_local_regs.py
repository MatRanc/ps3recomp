#!/usr/bin/env python3
"""--local-regs / --const-bi: lifted code must leave the same registers, local
store and channel state as the plain lift.

The program loops ten times through a local-store read-modify-write, a brsl
call, MFC and event-mask channel writes, an event-mask channel read and an
`ila / bi` constant branch. On the fifth pass it unmasks a pending event, and
the runtime takes the SPU interrupt: the handler clobbers r3/r4 (the loop
counter and accumulator), bumps a counter in LS, masks the event and irets, and
the runtime's register restore (spu_drain.c spu_irq_regs_save) must hand the
loop its registers back. Each combination of --merge-chunks and --const-bi is
lifted with and without --local-regs through the spu_lifter.py CLI, linked
against the real runtime and run with SPU_DIRECT on and off; the outputs of a
pair must match exactly.

Usage:
    python3 tools/test_spu_local_regs.py
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
SPU = ROOT / 'runtime' / 'spu'
sys.path.insert(0, str(TOOLS))
from spu_disasm import disassemble_spu  # noqa: E402


def ri16(op9, i16, rt): return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt
def ri18(op7, i18, rt): return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt
def ri10(op8, i10, ra, rt): return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt
def rr(op11, rb, ra, rt): return (op11 << 21) | (rb << 14) | (ra << 7) | rt


BASE, LOOP, SKIP, FWD, HELPER, HANDLER = 0x100, 0x114, 0x13C, 0x14C, 0x160, 0x16C
MARK = 0x2100
MFC_LSA, WR_EVENT_MASK, RD_EVENT_MASK = 16, 1, 11


def rel(at, tgt): return (tgt - at) >> 2


PROGRAM = [
    ri16(0x081, 0, 3),                   # 0x100 il   $3, 0          counter
    ri16(0x081, 0, 4),                   # 0x104 il   $4, 0          accumulator
    ri18(0x21, 0x2000, 10),              # 0x108 ila  $10, 0x2000    LS buffer
    ri16(0x081, 0x20, 11),               # 0x10C il   $11, 0x20      event bit
    ri18(0x21, 0x3000, 12),              # 0x110 ila  $12, 0x3000
    ri10(0x1C, 1, 3, 3),                 # 0x114 ai   $3, $3, 1      <- LOOP
    ri10(0x34, 0, 10, 5),                # 0x118 lqd  $5, 0($10)
    rr(0x0C0, 3, 5, 5),                  # 0x11C a    $5, $5, $3
    ri10(0x24, 0, 10, 5),                # 0x120 stqd $5, 0($10)
    ri16(0x066, rel(0x124, HELPER), 0),  # 0x124 brsl $0, HELPER
    rr(0x10D, 0, MFC_LSA, 12),           # 0x128 wrch MFC_LSA, $12
    ri10(0x7C, 5, 3, 6),                 # 0x12C ceqi $6, $3, 5
    rr(0x0C1, 11, 6, 7),                 # 0x130 and  $7, $6, $11
    ri16(0x040, rel(0x134, SKIP), 7),    # 0x134 brz  $7, SKIP
    rr(0x10D, 0, WR_EVENT_MASK, 7),      # 0x138 wrch SPU_WrEventMask, $7
    rr(0x00D, 0, RD_EVENT_MASK, 8),      # 0x13C rdch $8, SPU_RdEventMask  <- SKIP
    rr(0x0C0, 8, 4, 4),                  # 0x140 a    $4, $4, $8
    ri18(0x21, FWD, 13),                 # 0x144 ila  $13, FWD
    rr(0x1A8, 0, 13, 0),                 # 0x148 bi   $13             constant target
    ri10(0x7C, 10, 3, 9),                # 0x14C ceqi $9, $3, 10      <- FWD
    ri16(0x040, rel(0x150, LOOP), 9),    # 0x150 brz  $9, LOOP
    ri16(0x061, MARK >> 2, 14),          # 0x154 lqa  $14, MARK
    rr(0x0C0, 14, 4, 15),                # 0x158 a    $15, $4, $14
    0x00002000,                          # 0x15C stop 0x2000
    rr(0x0C0, 3, 4, 4),                  # 0x160 a    $4, $4, $3     <- HELPER
    ri10(0x1C, 3, 4, 4),                 # 0x164 ai   $4, $4, 3
    rr(0x1A8, 0, 0, 0),                  # 0x168 bi   $0
    ri16(0x081, 0x777, 3),               # 0x16C il   $3, 0x777      <- HANDLER
    ri16(0x081, 0x555, 4),               # 0x170 il   $4, 0x555
    ri16(0x061, MARK >> 2, 20),          # 0x174 lqa  $20, MARK
    ri10(0x1C, 1, 20, 20),               # 0x178 ai   $20, $20, 1
    ri16(0x041, MARK >> 2, 20),          # 0x17C stqa $20, MARK
    ri16(0x081, 0, 21),                  # 0x180 il   $21, 0
    rr(0x10D, 0, WR_EVENT_MASK, 21),     # 0x184 wrch SPU_WrEventMask, $21
    rr(0x1AA, 0, 0, 0) | 0x40000,        # 0x188 irete
]
END = BASE + 4 * len(PROGRAM)
# One chunk per branch target, as find_spu_functions splits them; FWD is the
# constant `bi` target, which a plain lift reaches through the resolver.
STARTS = [BASE, LOOP, SKIP, FWD, HELPER, HANDLER]

DRIVER = r'''
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
int main(void)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.policy_mode = 0;
    ctx.status = SPU_STATUS_RUNNING;
    spu_begin_image(77);
    spu_recomp_register();
    const uint32_t vec = 0x30000000u | ((HANDLER >> 2) << 7);   /* bra HANDLER */
    ctx.ls[0] = (uint8_t)(vec >> 24); ctx.ls[1] = (uint8_t)(vec >> 16);
    ctx.ls[2] = (uint8_t)(vec >> 8); ctx.ls[3] = (uint8_t)vec;
    ctx.int_enable = 1;
    ctx.event_status = 0x20u;           /* pending, masked until the fifth pass */
    int halted = spu_run_with_halt(spu_func_00000100, &ctx);
    printf("halted=%d status=%X stop=%X int_enable=%u mask=%X\n", halted, ctx.status,
           ctx.stop_code, ctx.int_enable, ctx.event_mask);
    for (int i = 0; i < 128; i++)
        printf("r%d %08X %08X %08X %08X\n", i, ctx.gpr[i]._u32[0], ctx.gpr[i]._u32[1],
               ctx.gpr[i]._u32[2], ctx.gpr[i]._u32[3]);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) h = (h ^ ctx.ls[i]) * 1099511628211ull;
    printf("ls %016llX mark=%02X%02X%02X%02X\n", (unsigned long long)h,
           ctx.ls[MARK], ctx.ls[MARK + 1], ctx.ls[MARK + 2], ctx.ls[MARK + 3]);
    return 0;
}

/* Link-only boundaries outside this CPU-only fixture (as test_spu_direct.c). */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
'''
RUNTIME = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']


def main():
    names = [i.mnemonic for i in disassemble_spu(b''.join(struct.pack('>I', w) for w in PROGRAM), BASE)]
    assert names[9] == 'brsl' and names[10] == 'wrch' and names[15] == 'rdch' \
        and names[19] == 'ceqi' and names[-1] == 'iret', names
    if os.name == 'nt':
        print('SKIP: fixture requires the POSIX compiler/linker')
        return
    cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
    cflags = ['-std=gnu17', '-O2', '-I' + str(ROOT / 'include'), '-I' + str(SPU),
              '-I' + str(ROOT / 'runtime/platform')]
    with tempfile.TemporaryDirectory(prefix='spu-local-regs-') as tmp:
        tmp = Path(tmp)
        (tmp / 'prog.bin').write_bytes(b''.join(struct.pack('>I', w) for w in PROGRAM))
        bounds = list(zip(STARTS, STARTS[1:] + [END]))
        (tmp / 'funcs.json').write_text(json.dumps([{'start': s, 'end': e} for s, e in bounds]))
        objs = []
        for src in RUNTIME + ['../platform/win32_compat.c']:
            o = tmp / (Path(src).stem + '.o')
            subprocess.run([cc, *cflags, '-c', str(SPU / src), '-o', str(o)], check=True)
            objs.append(str(o))
        driver = DRIVER.replace('HANDLER', hex(HANDLER)).replace('MARK', hex(MARK))
        failed = []
        for extra in ([], ['--merge-chunks'], ['--const-bi'], ['--merge-chunks', '--const-bi']):
            results = {}
            for local in (False, True):
                flags = extra + (['--local-regs'] if local else [])
                out = tmp / ('_'.join(f.strip('-') for f in flags) or 'plain')
                subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'), str(tmp / 'prog.bin'),
                                '--base', hex(BASE), '--functions', str(tmp / 'funcs.json'),
                                '-o', str(out), *flags], check=True, capture_output=True)
                src = (out / 'spu_recomp.c').read_text()
                assert ('spu_v4u sr' in src) == local, flags
                if '--const-bi' in flags:
                    assert 'SPU_IB_OK(ctx, 0x14C)' in src, flags          # chunk: direct branch
                    if '--merge-chunks' in flags:
                        assert 'SPU_GOTO_OK(ctx, 0x14C)' in src, flags    # group: goto
                for name in ('spu_recomp.c', 'spu_recomp.h'):
                    p = out / name
                    p.write_text(p.read_text().replace('../../runtime/spu/', str(SPU) + '/'))
                (out / 'driver.c').write_text(driver)
                exe = out / 'run'
                subprocess.run([cc, *cflags, '-I' + str(out), str(out / 'spu_recomp.c'),
                                str(out / 'driver.c'), *objs, '-pthread', '-lm', '-o', str(exe)],
                               check=True)
                for direct in ('1', '0'):
                    env = {k: v for k, v in os.environ.items()
                           if not k.startswith(('SPU_', 'SPURS_', 'PS3_SPU_', 'YDKJ_', 'LBP_', 'GH3_'))}
                    env['PS3_SPU_DIRECT'] = direct
                    ran = subprocess.run([str(exe)], capture_output=True, text=True,
                                         timeout=60, env=env)
                    assert ran.returncode == 0, ran.stderr[-2000:]
                    results[(local, direct)] = ran.stdout
            for direct in ('1', '0'):
                plain, local = results[(False, direct)], results[(True, direct)]
                lines = plain.splitlines()
                tag = ' '.join(extra) or 'plain'
                # The loop ran to the end with its own registers after the
                # handler clobbered them, and the handler ran exactly once.
                ok = (lines[4] == 'r3 0000000A 0000000A 0000000A 0000000A'
                      and lines[-1].endswith('mark=00000001')
                      and 'stop=2000' in lines[0] and 'mask=0' in lines[0])
                same = plain == local
                print(f'{tag:28s} SPU_DIRECT={direct}: plain {"ok" if ok else "WRONG"}, '
                      f'--local-regs {"identical" if same else "DIFFERS"}  ({lines[5]})')
                if not ok:
                    print('    ' + '\n    '.join([lines[0], lines[4], lines[-1]]))
                if not ok or not same:
                    failed.append(f'{tag} direct={direct}')
                    if not same:
                        for a, b in zip(lines, local.splitlines()):
                            if a != b:
                                print(f'    plain: {a}\n    local: {b}')
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
    print('local-regs: ok')


if __name__ == '__main__':
    main()
