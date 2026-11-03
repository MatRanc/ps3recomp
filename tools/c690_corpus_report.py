#!/usr/bin/env python3
"""Summarise a PS3RECOMP_C690_CORPUS capture.

Usage:
  tools/c690_corpus_report.py DIR [--extract-stage OUT] [--json]

The record format is documented in runtime/spu/c690_corpus.c. The report is
game-data analysis only; it never writes into the capture directory unless
--extract-stage asks for it.
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

MAGIC = b"C69CORP1"
HDR = struct.Struct("<8sIIIIIIQ")   # magic, version, job, frame, spu, image, desc, start
REC = struct.Struct("<IIQII")       # type, size, ea, lsa, aux
STAGE_META = struct.Struct("<IIII")  # ls_size, gpr_size, pc, image
STAGE_MAGIC = b"C69STG01"

T_DESC, T_GPR, T_GET, T_PUT, T_STAGE_IN, T_STAGE_OUT, T_OPCODES, T_END = range(1, 9)


def parse(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HDR.size:
        raise ValueError("short header")
    fields = HDR.unpack_from(blob, 0)
    if fields[0] != MAGIC:
        raise ValueError("bad magic")
    job = {
        "path": path,
        "version": fields[1],
        "index": fields[2],
        "frame": fields[3],
        "spu_id": fields[4],
        "image_id": fields[5],
        "desc_ea": fields[6],
        "start_ns": fields[7],
        "gets": [],
        "puts": [],
        "opcodes": None,
        "desc": None,
        "gpr": None,
        "stage_in": None,
        "stage_out": None,
        "end_ns": None,
        "truncated": False,
        "unknown": 0,
    }
    off = HDR.size
    n = len(blob)
    while off + REC.size <= n:
        rtype, size, ea, lsa, aux = REC.unpack_from(blob, off)
        off += REC.size
        if off + size > n:
            job["truncated"] = True
            break
        payload = blob[off:off + size]
        off += size
        if rtype == T_GET:
            job["gets"].append((ea, lsa, size, payload))
        elif rtype == T_PUT:
            job["puts"].append((ea, lsa, size, payload))
        elif rtype == T_DESC:
            job["desc"] = (ea, payload)
        elif rtype == T_GPR:
            job["gpr"] = (aux, payload)
        elif rtype in (T_STAGE_IN, T_STAGE_OUT):
            key = "stage_in" if rtype == T_STAGE_IN else "stage_out"
            if len(payload) >= STAGE_META.size:
                ls_size, gpr_size, pc, image = STAGE_META.unpack_from(payload, 0)
                body = payload[STAGE_META.size:]
                if len(body) >= ls_size + gpr_size:
                    job[key] = {
                        "stage_lsa": aux,
                        "ls_size": ls_size,
                        "gpr_size": gpr_size,
                        "pc": pc,
                        "image": image,
                        "ls": body[:ls_size],
                        "gpr": body[ls_size:ls_size + gpr_size],
                    }
        elif rtype == T_OPCODES:
            if len(payload) >= 256 * 4:
                job["opcodes"] = list(struct.unpack_from("<256I", payload, 0))
        elif rtype == T_END:
            job["end_ns"] = ea
        else:
            job["unknown"] += 1
    return job


def parse_stage_spool(path):
    """Read a per-lane stage spool written by c690_corpus.c (stage_<n>.bin).

    Returns {seq: {"in": body, "out": body, "spu", "frame", "lsa"}} where body
    is the (meta, ls, gpr) payload. Pairs are adjacent per lane; only complete
    pairs are returned.
    """
    blob = open(path, "rb").read()
    if len(blob) < 12 or blob[:8] != STAGE_MAGIC:
        raise ValueError("bad stage magic")
    off = 12
    pairs = {}
    n = len(blob)
    while off + 24 <= n:
        rtype, size, ea, lsa, aux = struct.unpack_from("<IIQII", blob, off)
        off += 24
        if off + size > n:
            break
        payload = blob[off:off + size]
        off += size
        if rtype not in (T_STAGE_IN, T_STAGE_OUT) or len(payload) < STAGE_META.size:
            continue
        ls_size, gpr_size, pc, image = STAGE_META.unpack_from(payload, 0)
        body = payload[STAGE_META.size:]
        if len(body) < ls_size + gpr_size:
            continue
        seq = ea >> 32
        ent = pairs.setdefault(seq, {"spu": aux, "frame": ea & 0xFFFFFFFF, "lsa": lsa})
        ent["in" if rtype == T_STAGE_IN else "out"] = {
            "ls_size": ls_size, "gpr_size": gpr_size, "pc": pc, "image": image,
            "ls": body[:ls_size], "gpr": body[ls_size:ls_size + gpr_size],
        }
    return {k: v for k, v in pairs.items() if "in" in v and "out" in v}


def opcode_signature(job):
    ops = job["opcodes"]
    if not ops:
        return "none"
    used = tuple((i, ops[i]) for i in range(256) if ops[i])
    return hashlib.sha1(repr(used).encode()).hexdigest()[:12]


def desc_signature(job):
    if not job["desc"]:
        return "none"
    return hashlib.sha1(job["desc"][1][:32]).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--extract-stage")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.dir) if f.endswith(".c69"))
    jobs = []
    errors = []
    for name in files:
        try:
            jobs.append(parse(os.path.join(args.dir, name)))
        except Exception as exc:  # noqa: BLE001 - report, keep going
            errors.append((name, str(exc)))

    if args.extract_stage:
        os.makedirs(args.extract_stage, exist_ok=True)

    variants = collections.Counter()
    op_totals = [0] * 256
    get_sizes = collections.Counter()
    put_sizes = collections.Counter()
    get_ea_min = get_ea_max = None
    put_ea_min = put_ea_max = None
    gets = puts = 0
    stage_pairs = 0
    spool_pairs = 0
    frames = collections.Counter()
    spus = collections.Counter()
    for job in jobs:
        variants[(desc_signature(job), opcode_signature(job))] += 1
        frames[job["frame"]] += 1
        spus[job["spu_id"]] += 1
        if job["opcodes"]:
            for i, c in enumerate(job["opcodes"]):
                op_totals[i] += c
        for ea, lsa, size, _ in job["gets"]:
            gets += 1
            get_sizes[size] += 1
            get_ea_min = ea if get_ea_min is None else min(get_ea_min, ea)
            get_ea_max = ea + size if get_ea_max is None else max(get_ea_max, ea + size)
        for ea, lsa, size, _ in job["puts"]:
            puts += 1
            put_sizes[size] += 1
            put_ea_min = ea if put_ea_min is None else min(put_ea_min, ea)
            put_ea_max = ea + size if put_ea_max is None else max(put_ea_max, ea + size)
        if job["stage_in"] and job["stage_out"]:
            stage_pairs += 1
            if args.extract_stage:
                base = os.path.join(args.extract_stage, f"job_{job['index']:06d}")
                with open(base + "_in.bin", "wb") as f:
                    f.write(job["stage_in"]["ls"])
                    f.write(job["stage_in"]["gpr"])
                with open(base + "_out.bin", "wb") as f:
                    f.write(job["stage_out"]["ls"])
                    f.write(job["stage_out"]["gpr"])
                with open(base + "_meta.txt", "w") as f:
                    f.write(f"stage_lsa=0x{job['stage_in']['stage_lsa']:05X} "
                            f"in_pc=0x{job['stage_in']['pc']:05X} "
                            f"out_pc=0x{job['stage_out']['pc']:05X} "
                            f"desc_ea=0x{job['desc_ea']:08X} frame={job['frame']} "
                            f"spu={job['spu_id']}\n")

    if args.extract_stage:
        for name in sorted(f for f in os.listdir(args.dir) if f.startswith("stage_") and f.endswith(".bin")):
            try:
                spairs = parse_stage_spool(os.path.join(args.dir, name))
            except Exception as exc:  # noqa: BLE001
                errors.append((name, str(exc)))
                continue
            spool_pairs += len(spairs)
            for seq, ent in sorted(spairs.items()):
                base = os.path.join(args.extract_stage, f"spool_{name[6:-4]}_{seq:06d}")
                with open(base + "_in.bin", "wb") as f:
                    f.write(ent["in"]["ls"]); f.write(ent["in"]["gpr"])
                with open(base + "_out.bin", "wb") as f:
                    f.write(ent["out"]["ls"]); f.write(ent["out"]["gpr"])
                with open(base + "_meta.txt", "w") as f:
                    f.write(f"stage_lsa=0x{ent['lsa']:05X} "
                            f"in_pc=0x{ent['in']['pc']:05X} "
                            f"out_pc=0x{ent['out']['pc']:05X} "
                            f"frame={ent['frame']} spu={ent['spu']} spool={name}\n")
    elif os.path.isdir(args.dir):
        for name in sorted(f for f in os.listdir(args.dir) if f.startswith("stage_") and f.endswith(".bin")):
            try:
                spool_pairs += len(parse_stage_spool(os.path.join(args.dir, name)))
            except Exception:  # noqa: BLE001
                pass

    summary = {
        "jobs": len(jobs),
        "errors": errors,
        "truncated": sum(1 for j in jobs if j["truncated"]),
        "frames": len(frames),
        "spu_counts": dict(spus),
        "gets": gets,
        "puts": puts,
        "get_size_hist": dict(sorted(get_sizes.items())),
        "put_size_hist": dict(sorted(put_sizes.items())),
        "get_ea_range": [get_ea_min, get_ea_max],
        "put_ea_range": [put_ea_min, put_ea_max],
        "stage_pairs": stage_pairs,
        "spool_stage_pairs": spool_pairs,
        "variants": [
            {"desc": d, "opcodes": o, "count": c}
            for (d, o), c in variants.most_common()
        ],
        "opcode_total": [(i, c) for i, c in enumerate(op_totals) if c],
    }

    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print(f"jobs={summary['jobs']} frames={summary['frames']} "
              f"truncated={summary['truncated']} stage_pairs={stage_pairs} "
              f"spool_stage_pairs={spool_pairs}")
        if errors:
            print(f"errors={len(errors)}")
            for name, exc in errors[:10]:
                print(f"  {name}: {exc}")
        print(f"spu_counts={summary['spu_counts']}")
        print(f"gets={gets} sizes={summary['get_size_hist']}")
        print(f"  ea=[0x{get_ea_min or 0:08X},0x{get_ea_max or 0:08X})")
        print(f"puts={puts} sizes={summary['put_size_hist']}")
        print(f"  ea=[0x{put_ea_min or 0:08X},0x{put_ea_max or 0:08X})")
        print(f"opcode totals: " + ", ".join(f"0x{i:02X}:{c}" for i, c in summary["opcode_total"]))
        print(f"layout variants: {len(summary['variants'])}")
        for v in summary["variants"][:10]:
            print(f"  desc={v['desc']} ops={v['opcodes']} jobs={v['count']}")
    if errors:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
