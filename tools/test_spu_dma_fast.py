#!/usr/bin/env python3
"""The DMA fast path and the inline channel ops against the full paths.

runtime/spu/tests/test_spu_dma_fast_ops.c drives GET/PUT, the rejected
transfers, lists with a stall-and-notify element, tag status and the atomic
lock-line commands through the channels, the way lifted code does, and prints
everything the SPU can read back and the guest/local-store state each step
leaves. The same steps run on:

  inline, no env          mfc_submit_fast/mfc_do_transfer_fast, spu_wrch_fast
  inline, SPU_DMA_SLOW=1  the full mfc_submit/mfc_do_transfer
  inline, SPU_CHHIST      the out-of-line spu_wrch/spu_rdch (g_spu_wrch_slow)
  out-of-line, no env     what a build without SPU_WRCH_INLINE does
  out-of-line, SLOW       both full paths

and stdout and stderr (the reject/skip/list log lines) must be identical.

  --against <checkout>    also build the fixture against another tree's
                          runtime/spu (e.g. the commit before the fast paths)
                          and require the same output from it.
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: DMA fast-path fixture currently requires a POSIX C compiler")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
tests = root / "runtime/spu/tests"
common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
          "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
          "spurs_policy_blob_weak.c", "spu_interp.c"]
clean_env = {k: v for k, v in os.environ.items()
             if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_", "MLB_"))}


def build(tree, out, inline, old=False):
    spu = tree / "runtime/spu"
    cc = os.environ.get("CC", "cc")
    flags = ["-std=gnu17", "-O2", "-UNDEBUG", *shlex.split(os.environ.get("CFLAGS", "")),
             "-pthread", "-I" + str(spu), "-I" + str(tree / "include"),
             "-I" + str(tree / "runtime/platform")]
    # SPU_WRCH_INLINE for the "lifted" half alone, as the project template
    # sets it for the lifted sources only.
    ops = out.with_suffix(".ops.o")
    subprocess.run([cc, *flags, *(["-DSPU_WRCH_INLINE"] if inline else []), "-c",
                    str(tests / "test_spu_dma_fast_ops.c"), "-o", str(ops)], check=True)
    subprocess.run([cc, *flags, *(["-DFIXTURE_OLD_RUNTIME"] if old else []),
                    str(ops), str(tests / "test_spu_dma_fast_rt.c"),
                    *[str(spu / name) for name in common],
                    str(tree / "runtime/platform/win32_compat.c"), "-lm", "-o", str(out)],
                   check=True)


def run(binary, cwd, extra=None):
    p = subprocess.run([str(binary)], env={**clean_env, **(extra or {})}, cwd=cwd,
                       capture_output=True, text=True, timeout=60)
    if p.returncode:
        print(p.stdout + p.stderr)
        raise SystemExit(f"{binary.name} {extra or ''}: exit {p.returncode}")
    path = [l for l in p.stdout.splitlines() if l.startswith("path:")]
    out = [l for l in p.stdout.splitlines() if not l.startswith("path:")]
    return path, out, p.stderr.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--against", type=Path, help="another checkout whose runtime/spu to compare with")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="spu-dma-fast-") as tmp:
        tmp = Path(tmp)
        inline, plain = tmp / "inline", tmp / "plain"
        build(root, inline, True)
        build(root, plain, False)
        modes = [
            ("inline", inline, {}, "path: dma_fast=1 wrch_slow=0"),
            ("inline SPU_DMA_SLOW", inline, {"SPU_DMA_SLOW": "1"}, "path: dma_fast=0 wrch_slow=0"),
            # A count no run reaches, so the histogram never prints.
            ("inline SPU_CHHIST", inline, {"SPU_CHHIST": "1000000000"}, "path: dma_fast=1 wrch_slow=1"),
            ("out-of-line", plain, {}, "path: dma_fast=1 wrch_slow=0"),
            ("out-of-line SPU_DMA_SLOW", plain, {"SPU_DMA_SLOW": "1"}, "path: dma_fast=0 wrch_slow=0"),
        ]
        if args.against:
            old = tmp / "old"
            build(args.against.resolve(), old, False, old=True)
            modes.append(("old runtime", old, {}, "path: old runtime"))
        ref = None
        for name, binary, env, want_path in modes:
            path, out, err = run(binary, tmp, env)
            assert path == [want_path], f"{name}: {path} (wanted {want_path})"
            if ref is None:
                ref = (name, out, err)
                assert len(out) > 60, out
                assert any("[mfc] REJECTED malformed transfer" in l for l in err), err
                assert any("[spu-dma] SKIP GET" in l for l in err), err
                assert any("[mfc-list] STALL" in l for l in err), err
                continue
            for what, a, b in (("stdout", ref[1], out), ("stderr", ref[2], err)):
                if a != b:
                    for i, (x, y) in enumerate(zip(a, b)):
                        if x != y:
                            raise SystemExit(f"{name} vs {ref[0]}: {what} line {i + 1} differs\n"
                                             f"  {ref[0]}: {x}\n  {name}: {y}")
                    raise SystemExit(f"{name} vs {ref[0]}: {what} has {len(b)} lines, not {len(a)}")
            print(f"PASS: {name}: {len(out)} state lines and {len(err)} log lines identical to {ref[0]}")


if __name__ == "__main__":
    main()
