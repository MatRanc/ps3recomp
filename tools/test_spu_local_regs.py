#!/usr/bin/env python3
"""--local-regs / --const-bi: lifted code must leave the same registers, local
store and channel state as the plain lift.

Two programs. IRQ loops ten times through a local-store read-modify-write, a
brsl call, MFC and event-mask channel writes, an event-mask channel read and an
`ila / bi` constant branch. On the fifth pass it unmasks a pending event, and
the runtime takes the SPU interrupt: the handler clobbers r3/r4 (the loop
counter and accumulator), bumps a counter in LS, masks the event and irets, and
the runtime's register restore (spu_drain.c spu_irq_regs_save) must hand the
loop its registers back.

CALLS holds values in registers across every kind of call --local-regs
reloads after selectively: a leaf (reloads what it writes), a loop of
chunks that tail-call each other (a cyclic summary), a return through a non-r0
link, a non-leaf callee (writes r0: reload all), a callee that moves its link
so the drain resumes at another lifted entry (reload all), one that leaves
through a computed `bi` (reload all), an indirect bisl call, a bisled not
taken, and rdch / wrch / rchcnt. Leaves read registers written on only one
side of a branch (the write-back must still come).

Each program and each combination of --merge-chunks, --const-bi and
--ib-goto is lifted with and without --local-regs through the spu_lifter.py
CLI, linked against the real runtime and run with SPU_DIRECT on and off; the
outputs of a pair must match exactly. The --local-regs build counts the
register write-backs and reloads it executes (entry loads aside).

Usage:
    python3 tools/test_spu_local_regs.py
"""
import json
import os
import re
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


def irq_setup():
    vec = 0x30000000 | ((HANDLER >> 2) << 7)           # bra HANDLER, planted at LS 0
    return (f'ctx.ls[0] = 0x{vec >> 24:02X}; ctx.ls[1] = 0x{(vec >> 16) & 0xFF:02X}; '
            f'ctx.ls[2] = 0x{(vec >> 8) & 0xFF:02X}; ctx.ls[3] = 0x{vec & 0xFF:02X};\n'
            '    ctx.int_enable = 1;\n'
            '    ctx.event_status = 0x20u;           /* pending, masked until the fifth pass */')


def irq_ok(lines):
    # The loop ran to the end with its own registers after the handler
    # clobbered them, and the handler ran exactly once.
    return (lines[4] == 'r3 0000000A 0000000A 0000000A 0000000A'
            and lines[-1].endswith(f'mark={1:08X}')
            and 'stop=2000' in lines[0] and 'mask=0' in lines[0])


def irq_src(flags, src, labels):
    assert ('spu_v4u sr' in src) == ('--local-regs' in flags), flags
    if '--const-bi' in flags:
        assert 'SPU_IB_OK(ctx, 0x14C)' in src, flags          # chunk: direct branch
        if '--merge-chunks' in flags:
            assert 'SPU_GOTO_OK(ctx, 0x14C)' in src, flags    # group: goto
    if '--ib-goto' in flags:
        assert 'SPU_GOTO_OK(ctx, 0x14C)) goto loc_0000014C;' in src, flags   # computed bi: goto


# ---- CALLS: a small assembler with labels ----------------------------------
RD_IN_MBOX = 29


def assemble(base, items):
    """items: label strings and fn(addr, labels) -> word. Returns (words, labels)."""
    labels, at = {}, base
    for it in items:
        if isinstance(it, str):
            labels[it] = at
        else:
            at += 4
    words, at = [], base
    for it in items:
        if not isinstance(it, str):
            words.append(it(at, labels))
            at += 4
    return words, labels


def IL(rt, v): return lambda at, L: ri16(0x081, v, rt)
def ILA(rt, lbl): return lambda at, L: ri18(0x21, L[lbl], rt)
def AI(rt, ra, v): return lambda at, L: ri10(0x1C, v, ra, rt)
def ORI(rt, ra, v): return lambda at, L: ri10(0x04, v, ra, rt)
def A(rt, ra, rb): return lambda at, L: rr(0x0C0, rb, ra, rt)
def CEQI(rt, ra, v): return lambda at, L: ri10(0x7C, v, ra, rt)
def BR(lbl): return lambda at, L: ri16(0x064, rel(at, L[lbl]), 0)
def BRZ(rt, lbl): return lambda at, L: ri16(0x040, rel(at, L[lbl]), rt)
def BRNZ(rt, lbl): return lambda at, L: ri16(0x042, rel(at, L[lbl]), rt)
def BRSL(rt, lbl): return lambda at, L: ri16(0x066, rel(at, L[lbl]), rt)
def BI(ra): return lambda at, L: rr(0x1A8, 0, ra, 0)
def BISL(rt, ra): return lambda at, L: rr(0x1A9, 0, ra, rt)
def BISLED(rt, ra): return lambda at, L: rr(0x1AB, 0, ra, rt)
def RDCH(rt, ch): return lambda at, L: rr(0x00D, 0, ch, rt)
def WRCH(ch, rt): return lambda at, L: rr(0x10D, 0, ch, rt)
def RCHCNT(rt, ch): return lambda at, L: rr(0x00F, 0, ch, rt)
def STOP(code): return lambda at, L: code


def AI_DELTA(rt, ra, to, frm):
    """ai rt, ra, L[to] - L[frm]: moves a link to another address."""
    def f(at, L):
        d = L[to] - L[frm]
        assert -512 <= d < 512, d
        return ri10(0x1C, d, ra, rt)
    return f


CALLS, CALL_LABELS = assemble(0x100, [
    'MAIN',
    IL(20, 100), IL(21, 200), IL(22, 0), IL(23, 0), IL(24, 0), IL(25, 0), IL(27, 0),
    IL(30, 4),
    ILA(9, 'LEAF'),
    'LOOP',
    AI(20, 20, 1),
    CEQI(11, 30, 2),
    BRZ(11, 'SKIPW'),
    AI(21, 21, 50),          # r21 dirty on one side only; LEAF reads it from ctx
    'SKIPW',
    BRSL(0, 'LEAF'),         # leaf: reads r20/r21, writes r22
    A(25, 20, 22),
    BRSL(0, 'CA'),           # chunks CL <-> CM tail-call each other: r23 r24 r26
    A(25, 25, 23),
    A(25, 25, 24),
    BRSL(4, 'LREG'),         # returns through $4: r27
    A(25, 25, 27),
    BRSL(0, 'NONLEAF'),      # calls LEAF itself, so writes r0: reload all
    A(25, 25, 22),
    WRCH(WR_EVENT_MASK, 30),   # interrupts are off: only the mask changes
    AI(28, 30, 9),             # stale values the channel reads must replace
    RDCH(28, RD_EVENT_MASK),
    AI(29, 30, 7),
    RCHCNT(29, RD_IN_MBOX),
    A(25, 25, 28),
    A(25, 25, 29),
    BISLED(13, 9),           # no event pending: not taken, sets the link r13
    AI(30, 30, -1),
    BRNZ(30, 'LOOP'),
    BRSL(0, 'RESUME'),       # returns to G instead: the drain resumes there
    'R_RESUME',
    A(31, 20, 25),
    BRSL(0, 'INDF'),         # leaves through a computed bi to K, which returns
    A(31, 31, 21),
    BISL(0, 9),              # indirect call (LEAF)
    A(31, 31, 22),
    A(31, 31, 13),
    STOP(0x2000),
    'G',                     # reached with r0 = R_RESUME + this offset
    AI(20, 20, 500),
    BI(7),                   # computed: back to R_RESUME (RESUME saved it in r7)
    'K',
    A(21, 21, 5),            # reads the r5 INDF set just before leaving
    BI(0),
    'LEAF',
    A(22, 22, 20),
    A(22, 22, 21),
    BI(0),
    'CA',
    IL(26, 3),
    'CL',
    AI(23, 23, 1),
    AI(26, 26, -1),
    BRZ(26, 'CE'),
    'CM',
    AI(24, 24, 2),
    BR('CL'),
    'CE',
    BI(0),
    'LREG',
    AI(27, 27, 3),
    BI(4),
    'NONLEAF',
    ORI(8, 0, 0),
    BRSL(0, 'LEAF'),
    ORI(0, 8, 0),
    BI(0),
    'RESUME',
    ORI(7, 0, 0),
    AI_DELTA(0, 0, 'G', 'R_RESUME'),
    BI(0),
    'INDF',
    ILA(5, 'K'),
    BI(5),
])
CALL_STARTS = [CALL_LABELS[n] for n in ('MAIN', 'LOOP', 'SKIPW', 'G', 'K', 'LEAF', 'CA', 'CL', 'CM',
                                        'CE', 'LREG', 'NONLEAF', 'RESUME', 'INDF')]


def calls_ok(lines):
    regs = {ln.split()[0]: int(ln.split()[1], 16) for ln in lines[1:-1]}
    # G ran (r20), K ran (r21, after reading INDF's r5), the chain ran its
    # loop each time (r23/r24), LREG returned four times (r27).
    return ('stop=2000' in lines[0] and 'mask=1' in lines[0] and regs['r28'] == 1
            and regs['r29'] == 0 and regs['r20'] == 100 + 4 + 500
            and regs['r21'] == 200 + 50 + CALL_LABELS['K']
            and regs['r23'] == 4 * 3 and regs['r24'] == 4 * 4 and regs['r27'] == 4 * 3)


def calls_src(flags, src, labels):
    assert ('spu_v4u sr' in src) == ('--local-regs' in flags), flags
    if '--local-regs' not in flags:
        return
    # After the leaf call only what it writes comes back: r22, not r20.
    leaf = f"spu_func_{labels['LEAF']:08X}(ctx); spu_drain_call"
    lines = src.splitlines()
    i = next(i for i, ln in enumerate(lines) if leaf in ln and 'host_depth++' in ln)
    assert 'sr22 = spu_u2v(ctx->gpr[22])' in lines[i + 1], lines[i + 1]
    assert 'sr20 =' not in lines[i + 1] and 'sr25 =' not in lines[i + 1], lines[i + 1]
    # After the call that moves its link, everything comes back.
    resume = f"spu_func_{labels['RESUME']:08X}(ctx); spu_drain_call"
    i = next(i for i, ln in enumerate(lines) if resume in ln)
    assert 'sr20 = spu_u2v(ctx->gpr[20])' in lines[i + 1], lines[i + 1]


PROGRAMS = [
    # name, words, base, starts, entry, driver setup, output check, source check
    ('irq', PROGRAM, BASE, STARTS, BASE, irq_setup(), irq_ok, irq_src),
    ('calls', CALLS, 0x100, CALL_STARTS, 0x100, '', calls_ok, calls_src),
]

DRIVER = r"""
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
unsigned long long lr_st, lr_ld;
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
    SETUP
    int halted = spu_run_with_halt(ENTRY, &ctx);
    printf("halted=%d status=%X stop=%X int_enable=%u mask=%X\n", halted, ctx.status,
           ctx.stop_code, ctx.int_enable, ctx.event_mask);
    for (int i = 0; i < 128; i++)
        printf("r%d %08X %08X %08X %08X\n", i, ctx.gpr[i]._u32[0], ctx.gpr[i]._u32[1],
               ctx.gpr[i]._u32[2], ctx.gpr[i]._u32[3]);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) h = (h ^ ctx.ls[i]) * 1099511628211ull;
    printf("ls %016llX mark=%02X%02X%02X%02X\n", (unsigned long long)h,
           ctx.ls[MARK], ctx.ls[MARK + 1], ctx.ls[MARK + 2], ctx.ls[MARK + 3]);
    fprintf(stderr, "lr-count %llu %llu\n", lr_st, lr_ld);
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
"""
RUNTIME = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
FLAG_SETS = ([], ['--merge-chunks'], ['--const-bi'], ['--merge-chunks', '--const-bi'],
             ['--merge-chunks', '--ib-known-targets', '--ib-goto'])
_STORE_RE = re.compile(r"(ctx->gpr\[(\d+)\] = spu_v2u\(sr\2\));")
_RELOAD_RE = re.compile(r"(?<!spu_v4u )(sr(\d+) = spu_u2v\(ctx->gpr\[\2\]\));")


def count_syncs(src):
    """Static write-backs and reloads (entry loads aside), and the source with
    each one counting itself into lr_st / lr_ld."""
    n_st, n_ld = len(_STORE_RE.findall(src)), len(_RELOAD_RE.findall(src))
    src = _RELOAD_RE.sub(r"\1, lr_ld++;", _STORE_RE.sub(r"\1, lr_st++;", src))
    return n_st, n_ld, 'extern unsigned long long lr_st, lr_ld;\n' + src


def run_program(tmp, cc, cflags, objs, prog, failed, totals):
    name, words, base, starts, entry, setup, ok_fn, src_fn = prog
    end = base + 4 * len(words)
    pdir = tmp / name
    pdir.mkdir()
    (pdir / 'prog.bin').write_bytes(b''.join(struct.pack('>I', w) for w in words))
    bounds = list(zip(starts, starts[1:] + [end]))
    (pdir / 'funcs.json').write_text(json.dumps([{'start': s, 'end': e} for s, e in bounds]))
    labels = CALL_LABELS if name == 'calls' else {}
    driver = (DRIVER.replace('SETUP', setup).replace('ENTRY', f'spu_func_{entry:08X}')
              .replace('MARK', hex(MARK)))
    for extra in FLAG_SETS:
        results, counts = {}, ''
        for local in (False, True):
            flags = extra + (['--local-regs'] if local else [])
            out = pdir / ('_'.join(f.strip('-') for f in flags) or 'plain')
            subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'), str(pdir / 'prog.bin'),
                            '--base', hex(base), '--functions', str(pdir / 'funcs.json'),
                            '-o', str(out), *flags], check=True, capture_output=True)
            src = (out / 'spu_recomp.c').read_text()
            src_fn(flags, src, labels)
            n_st, n_ld, src = count_syncs(src)
            (out / 'spu_recomp.c').write_text(src.replace('../../runtime/spu/', str(SPU) + '/'))
            p = out / 'spu_recomp.h'
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
                                     timeout=20, env=env)
                assert ran.returncode == 0, ran.stderr[-2000:]
                results[(local, direct)] = ran.stdout
                if local:
                    d_st, d_ld = (int(x) for x in re.search(r'lr-count (\d+) (\d+)',
                                                            ran.stderr).groups())
                    counts += f' D{direct}:{d_st}/{d_ld}'
                    totals[(name, ' '.join(extra) or 'plain', direct)] = (
                        n_st if direct == '1' else 0, n_ld if direct == '1' else 0, d_st, d_ld)
            if local:
                counts = f'  stores/reloads static {n_st}/{n_ld} run' + counts
        for direct in ('1', '0'):
            plain, local = results[(False, direct)], results[(True, direct)]
            lines = plain.splitlines()
            tag = f"{name}: {' '.join(extra) or 'plain'}"
            ok = ok_fn(lines)
            same = plain == local
            print(f'{tag:50s} SPU_DIRECT={direct}: plain {"ok" if ok else "WRONG"}, '
                  f'--local-regs {"identical" if same else "DIFFERS"}'
                  + (counts if direct == '0' else ''))
            if not ok:
                print('    ' + '\n    '.join(lines[:1] + lines[1:-1][3:32] + lines[-1:]))
            if not ok or not same:
                failed.append(f'{tag} direct={direct}')
                if not same:
                    for a, b in zip(lines, local.splitlines()):
                        if a != b:
                            print(f'    plain: {a}\n    local: {b}')


def main():
    names = [i.mnemonic for i in disassemble_spu(b''.join(struct.pack('>I', w) for w in PROGRAM), BASE)]
    assert names[9] == 'brsl' and names[10] == 'wrch' and names[15] == 'rdch' \
        and names[19] == 'ceqi' and names[-1] == 'iret', names
    names = [i.mnemonic for i in disassemble_spu(b''.join(struct.pack('>I', w) for w in CALLS), 0x100)]
    assert 'unknown' not in ' '.join(names) and {'bisled', 'bisl', 'rchcnt', 'rdch', 'wrch'} <= set(names), names
    if os.name == 'nt':
        print('SKIP: fixture requires the POSIX compiler/linker')
        return
    cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
    cflags = ['-std=gnu17', '-O2', '-I' + str(ROOT / 'include'), '-I' + str(SPU),
              '-I' + str(ROOT / 'runtime/platform')]
    failed, totals = [], {}
    with tempfile.TemporaryDirectory(prefix='spu-local-regs-') as tmp:
        tmp = Path(tmp)
        objs = []
        for src in RUNTIME + ['../platform/win32_compat.c']:
            o = tmp / (Path(src).stem + '.o')
            subprocess.run([cc, *cflags, '-c', str(SPU / src), '-o', str(o)], check=True)
            objs.append(str(o))
        for prog in PROGRAMS:
            run_program(tmp, cc, cflags, objs, prog, failed, totals)
    t = [sum(v[i] for v in totals.values()) for i in range(4)]
    print(f'--local-regs, all programs and flag sets: {t[0]} write-backs / {t[1]} reloads '
          f'in the source, {t[2]} / {t[3]} executed (both SPU_DIRECT runs)')
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
    print('local-regs: ok')


if __name__ == '__main__':
    main()
