#!/usr/bin/env python3
"""Build and run the PS3RECOMP_SPU_FTZ twin differential (SPU codegen v3).

For one generated module, compile tools/spu_ftz_diff_main.c twice -- against
the base source and against its `_ftz` twin -- and replay captured entry states
through both, with the twin arm running MXCSR FTZ|DAZ. The final 256 KiB local
store and all 128 GPRs must be bit-identical.

Examples:
  # c690 0xFDA0 stage corpus (combined LS+GPR in/out files)
  tools/spu_ftz_diff.py --module jobmod_c690c2f7b09a_eF5B8 --entry 0xFDA0 \
      --inputs 'recomp-work/corpus_edge1_*/stage/job_*_in.bin' \
      --want-suffix _out.bin

  # captured 0x21 kernel states (ls.bin + gpr.bin per lsa)
  tools/spu_ftz_diff.py --module jobmod_c690c2f7b09a_eF5B8 \
      --capdirs recomp-work/captures/fncap21
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # ps3recomp-sync
CFLAGS = [
    "-O2", "-march=native", "-std=gnu17", "-fno-strict-aliasing", "-fwrapv",
    "-mfma", "-mssse3", "-mavx2", "-ffp-contract=off",
    "-DSPU_FAST_FLOAT", "-DSPU_LIFTED_FAST_LS", "-DSPU_USE_DISPATCH_MT",
    "-DSPU_WRCH_INLINE",
]


def includes():
    return [f"-I{ROOT}/runtime/spu", f"-I{ROOT}/include", f"-I{ROOT}/libs/video",
            f"-I{ROOT}/runtime/ppu", f"-I{ROOT}/runtime/memory",
            f"-I{ROOT}/runtime/syscalls", f"-I{ROOT}/runtime/platform"]


def build(outdir, module_dir, reg_fn, tag):
    exe = os.path.join(outdir, "diff_" + tag)
    cmd = ["clang"] + CFLAGS + includes() + [
        f"-DSPU_DIFF_REG_FN={reg_fn}", "-o", exe,
        os.path.join(HERE, "spu_ftz_diff_main.c"),
        os.path.join(module_dir, "spu_recomp.c"), "-lm",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write("build failed (%s):\n%s\n" % (tag, r.stderr[-4000:]))
        sys.exit(2)
    return exe


def run(exe, inp, entry, ftz, outfile, want=None):
    env = dict(os.environ)
    if ftz:
        env["SPU_DIFF_FTZ"] = "1"
    env["SPU_DIFF_WRITE"] = outfile
    cmd = [exe, inp, hex(entry)]
    if want:
        cmd.append(want)
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.stderr.write("run failed: %s\n%s%s\n" % (" ".join(cmd), r.stdout, r.stderr))
        return None
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spu-dir", default=os.path.join(os.path.dirname(ROOT),
                    "recomp-work", "spu_lifted"))
    ap.add_argument("--module", required=True)
    ap.add_argument("--entry", default=None, help="hex entry for --inputs")
    ap.add_argument("--inputs", action="append", default=[],
                    help="glob of combined LS+GPR input files")
    ap.add_argument("--want-suffix", default=None,
                    help="replace this suffix in the input path for the expected output")
    ap.add_argument("--capdirs", action="append", default=[],
                    help="directory whose 0x<lsa>/ subdirs hold ls.bin+gpr.bin")
    args = ap.parse_args()

    base = os.path.join(args.spu_dir, args.module)
    twin = os.path.join(args.spu_dir, args.module + "_ftz")
    if not os.path.isfile(os.path.join(twin, "spu_recomp.c")):
        sys.stderr.write("no twin at %s; run tools/spu_ftz_twin.py first\n" % twin)
        return 2

    tmp = tempfile.mkdtemp(prefix="spu_ftz_diff.", dir=os.environ.get("TMPDIR", "/tmp"))
    exe_b = build(tmp, base, args.module + "_spu_recomp_register", "base")
    exe_t = build(tmp, twin, args.module + "_ftz_spu_recomp_register", "ftz")

    cases = []   # (label, input, entry, want)
    if args.inputs:
        if not args.entry:
            sys.stderr.write("--inputs needs --entry\n")
            return 2
        for pat in args.inputs:
            for f in sorted(glob.glob(pat)):
                want = None
                if args.want_suffix:
                    w = re.sub(r"_in\.bin$", args.want_suffix, f)
                    if os.path.isfile(w):
                        want = w
                cases.append((os.path.basename(f), f, int(args.entry, 0), want))
    for cd in args.capdirs:
        for d in sorted(glob.glob(os.path.join(cd, "0x*"))):
            if not os.path.isdir(d):
                continue
            m = re.match(r"(0x[0-9A-Fa-f]+)", os.path.basename(d))
            if not m:
                continue
            cases.append((os.path.basename(d), d, int(m.group(1), 0), None))
    if not cases:
        sys.stderr.write("no cases\n")
        return 2

    bad = 0
    exact_want = 0
    for label, inp, entry, want in cases:
        ob = os.path.join(tmp, "b.bin")
        ot = os.path.join(tmp, "t.bin")
        rb = run(exe_b, inp, entry, False, ob, want)
        rt = run(exe_t, inp, entry, True, ot, want)
        if rb is None or rt is None:
            bad += 1
            continue
        same = open(ob, "rb").read() == open(ot, "rb").read()
        want_ok = ""
        if want:
            exact_want += 1
            with open(want, "rb") as f:
                want_ok = " want=%s" % ("ok" if f.read() == open(ot, "rb").read() else "DIFF")
        print("%-12s %s  base[%s] twin[%s]%s" %
              (label, "SAME" if same else "DIFF", rb, rt, want_ok))
        if not same or "DIFF" in want_ok:
            bad += 1
    print("cases=%d diff=%d want_checked=%d" % (len(cases), bad, exact_want))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
