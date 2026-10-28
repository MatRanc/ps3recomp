#!/usr/bin/env python3
"""Channel read value, MFC parameter latches and GET/PUT ordering, and the
indirect-branch fast path, with SPU_IB_FAST unset and =0."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: fixture requires the POSIX compiler/linker and pthreads")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / "runtime/spu"
common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
          "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
          "spurs_policy_blob_weak.c", "spu_interp.c"]
base = {k: v for k, v in os.environ.items()
        if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}
with tempfile.TemporaryDirectory(prefix="spu-runtime-cuts-") as tmp:
    binary = Path(tmp) / "fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-UNDEBUG",
                    *shlex.split(os.environ.get("CFLAGS", "")), "-pthread",
                    "-I" + str(root / "include"), "-I" + str(spu),
                    "-I" + str(root / "runtime/platform"),
                    str(spu / "tests/test_spu_runtime_cuts.c"),
                    *[str(spu / name) for name in common],
                    str(root / "runtime/platform/win32_compat.c"), "-lm",
                    "-o", str(binary)], check=True)
    for mode in (None, "0"):
        env = dict(base)
        if mode is not None:
            env["SPU_IB_FAST"] = mode
        p = subprocess.run([str(binary)], env=env, cwd=tmp, capture_output=True,
                           text=True, timeout=60)
        print("SPU_IB_FAST=%s:" % mode, p.stdout.strip())
        if p.returncode:
            print(p.stderr[-2500:])
            raise SystemExit(p.returncode)
