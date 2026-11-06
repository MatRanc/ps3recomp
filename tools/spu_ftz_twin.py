#!/usr/bin/env python3
"""Generate the MXCSR FTZ|DAZ twin of every lifted SPU module (SPU codegen v3).

The shipped game build carries two variants of each lifted module:

  * the base module (`<SPU_DIR>/<name>/spu_recomp.c`), registered as variant 0
    and exactly the code the title has always run;
  * a twin in `<SPU_DIR>/<name>_ftz/`, identical control flow and native hooks
    but with the six SPU float helpers (`fa/fs/fm/fma/fms/fnms`) replaced by the
    hardware `spu_hw_*` forms that rely on the host thread running with MXCSR
    FTZ|DAZ.

`PS3RECOMP_SPU_FTZ` (live key, default 0) is sampled once per lifted run; the
runtime sets MXCSR for the run and dispatch prefers the variant-1 functions.
When the key is off the base module runs unchanged, so a build tree without the
variant registry (the play tree) is inert: the twin's registration calls a
weak `spu_register_function_var`, sees it absent, and registers nothing.

The twin is derived from the already-generated, already-patched base source, so
it inherits the cull/native/Pm patches byte for byte; only the float helper
selection differs. Everything this script writes is regenerated from the base
on every run, so it is idempotent and can be re-run after any relift.

Usage:
  tools/spu_ftz_twin.py --spu-dir recomp-work/spu_lifted [--remove]
"""
import argparse
import os
import re
import sys

FTZ_PREAMBLE = r"""
/* PS3RECOMP_SPU_FTZ twin: SPU float on a host thread running MXCSR FTZ|DAZ.
 * SPU has no denormals and every zero result is +0, so the hardware can flush
 * denormal operands/results and only the sign of zero is canonicalised. */
#if defined(__FMA__) && (defined(__SSE__) || defined(__AVX__))
#include <immintrin.h>
static inline __m128 spu_hw_ftz(__m128 v) {
    return _mm_andnot_ps(_mm_cmpeq_ps(v, _mm_setzero_ps()), v);
}
static inline __m128 spu_hw_ld(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
static inline u128 spu_hw_st(__m128 v) {
    u128 r; _mm_storeu_ps((float*)r._u32, spu_hw_ftz(v)); return r;
}
static inline u128 spu_hw_fa(u128 a, u128 b) { return spu_hw_st(_mm_add_ps(spu_hw_ld(a), spu_hw_ld(b))); }
static inline u128 spu_hw_fs(u128 a, u128 b) { return spu_hw_st(_mm_sub_ps(spu_hw_ld(a), spu_hw_ld(b))); }
static inline u128 spu_hw_fm(u128 a, u128 b) { return spu_hw_st(_mm_mul_ps(spu_hw_ld(a), spu_hw_ld(b))); }
static inline u128 spu_hw_fma(u128 a, u128 b, u128 c)  { return spu_hw_st(_mm_fmadd_ps(spu_hw_ld(a), spu_hw_ld(b), spu_hw_ld(c))); }
static inline u128 spu_hw_fms(u128 a, u128 b, u128 c)  { return spu_hw_st(_mm_fmsub_ps(spu_hw_ld(a), spu_hw_ld(b), spu_hw_ld(c))); }
static inline u128 spu_hw_fnms(u128 a, u128 b, u128 c) { return spu_hw_st(_mm_fnmadd_ps(spu_hw_ld(a), spu_hw_ld(b), spu_hw_ld(c))); }
#else
/* No FMA: fall back to the exact helpers; the twin is then identical. */
#define spu_hw_fa spu_fa
#define spu_hw_fs spu_fs
#define spu_hw_fm spu_fm
#define spu_hw_fma spu_fma
#define spu_hw_fms spu_fms
#define spu_hw_fnms spu_fnms
#endif
extern void spu_register_function_var(uint32_t, void (*)(spu_context*), int)
    __attribute__((weak));
"""

HELPER_RE = re.compile(r"\bspu_(fnms|fma|fms|fa|fs|fm)\s*\(")
REG_LINE = ("    for (const spu_func_entry* e = spu_function_table; e->func; ++e)\n"
            "        spu_register_function(e->addr, e->func);")


def lead_end(text: str) -> int:
    """Byte offset just past the leading comment/blank/#include block."""
    pos = 0
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if s.startswith("#include") or s.startswith("/*") or s.startswith("*") \
                or s.startswith("//") or s == "" or s.startswith("#pragma"):
            pos += len(line)
            # A block comment that has already ended on this line stops the run.
            if s.startswith("*/"):
                break
            continue
        break
    return pos


def twin_module(name: str, src: str, hdr: str):
    prefix = name + "_"
    tprefix = name + "_ftz_"
    c = src.replace(prefix, tprefix)
    h = hdr.replace(prefix, tprefix)
    c = HELPER_RE.sub(lambda m: "spu_hw_" + m.group(1) + "(", c)
    at = lead_end(c)
    c = c[:at] + FTZ_PREAMBLE + c[at:]
    if REG_LINE not in c:
        raise SystemExit(f"{name}: no register loop to convert")
    c = c.replace(REG_LINE,
                  "    if (!spu_register_function_var) return;   /* play tree: no variants */\n"
                  "    for (const spu_func_entry* e = spu_function_table; e->func; ++e)\n"
                  "        spu_register_function_var(e->addr, e->func, 1);")
    return c, h


def write_if_changed(path: str, text: str) -> bool:
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == text:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def add_extern(text: str, decl: str, anchor: str) -> str:
    """Add `decl` after a line containing `anchor` (once)."""
    if decl in text:
        return text
    out = []
    for line in text.splitlines(keepends=True):
        out.append(line)
        if anchor in line and "extern" in line:
            indent = line[:len(line) - len(line.lstrip())]
            out.append(f"{indent}{decl}\n")
    return "".join(out)


def patch_registration(path: str, twin_names) -> bool:
    """Call each twin module's register function next to its base one."""
    if not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    orig = text
    for name in twin_names:
        text = add_extern(text, f"extern void {name}_ftz_spu_recomp_register(void);",
                          f"{name}_spu_recomp_register(void);")
        base = name + "_spu_recomp_register();"
        twin = name + "_ftz_spu_recomp_register();"
        if twin in text:
            continue
        # Insert the twin call after the line holding the base registration,
        # keeping the same image id (set by the spu_begin_image before it).
        out = []
        for line in text.splitlines(keepends=True):
            out.append(line)
            if base in line and "extern" not in line and not line.lstrip().startswith("*"):
                indent = line[:len(line) - len(line.lstrip())]
                if "spu_begin_image" in line:
                    # spu_begin_image(N); base_register(); -> keep image for twin
                    m = re.search(r"spu_begin_image\((\d+)\)", line)
                    out.append(f"{indent}spu_begin_image({m.group(1)}); {twin}\n")
                else:
                    out.append(f"{indent}{twin}\n")
        text = "".join(out)
    if text != orig:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return True
    return False


def patch_workloads(path: str, twin_names) -> bool:
    """Add the twin entry registration (weak) to spu_workloads.c."""
    if not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    orig = text
    if "spu_workload_register_img_ftz" not in text:
        marker = '#include "spu_workload.h"'
        decl = (marker + "\n"
                "/* PS3RECOMP_SPU_FTZ twin entry (weak: the play runtime has no\n"
                " * variant registry and ignores it). */\n"
                "extern void spu_workload_register_img_ftz(uint64_t, void (*)(spu_context*))\n"
                "    __attribute__((weak));\n")
        text = text.replace(marker, decl, 1)
    for name in twin_names:
        text = add_extern(text, f"extern void {name}_ftz_spu_recomp_register(void);",
                          f"{name}_spu_recomp_register(void);")
        # The twin entry function has the same suffix as the base one.
        for line in text.splitlines():
            m = re.search(r"extern void (" + re.escape(name) + r"_spu_func_[0-9A-Fa-f]+)\(spu_context\*\);", line)
            if m:
                twin_fn = m.group(1).replace(name + "_spu_func_", name + "_ftz_spu_func_", 1)
                text = add_extern(text, f"extern void {twin_fn}(spu_context*);", m.group(1) + "(spu_context*);")
                break
    already_fp = set(re.findall(r"spu_workload_register_img_ftz\(\s*([^,]+),", text))
    for name in twin_names:
        base = name + "_spu_func_"
        twin = name + "_ftz_spu_func_"
        out = []
        pat = re.compile(r"spu_workload_register_img\(\s*([^,]+),\s*(" +
                         re.escape(base) + r"[0-9A-Fa-f]+)")
        regbase = name + "_spu_recomp_register();"
        regtwin = name + "_ftz_spu_recomp_register();"
        have_reg = regtwin in text
        for line in text.splitlines(keepends=True):
            out.append(line)
            if (not have_reg) and regbase in line and "extern" not in line:
                mreg = re.search(r"spu_begin_image\((\d+)\)", line)
                indent = line[:len(line) - len(line.lstrip())]
                if mreg:
                    out.append(f"{indent}spu_begin_image({mreg.group(1)}); {regtwin}\n")
                else:
                    out.append(f"{indent}{regtwin}\n")
                have_reg = True
            m = pat.search(line)
            if m and m.group(1).strip() not in already_fp:
                indent = line[:len(line) - len(line.lstrip())]
                entry = m.group(2).replace(base, twin, 1)
                fp = m.group(1).strip()
                already_fp.add(fp)
                out.append(f"{indent}if (spu_workload_register_img_ftz) "
                           f"spu_workload_register_img_ftz({fp}, {entry});\n")
        text = "".join(out)
    if text != orig:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spu-dir", default="recomp-work/spu_lifted")
    ap.add_argument("--remove", action="store_true",
                    help="Delete the generated twin directories instead.")
    args = ap.parse_args()

    root = args.spu_dir
    names = sorted(d for d in os.listdir(root)
                   if os.path.isfile(os.path.join(root, d, "spu_recomp.c")))
    if args.remove:
        for name in names:
            if not name.endswith("_ftz"):
                continue
            d = os.path.join(root, name)
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))
            os.rmdir(d)
        print("removed twin directories")
        return 0

    base_names = [n for n in names if not n.endswith("_ftz")]
    made = 0
    for name in base_names:
        d = os.path.join(root, name)
        with open(os.path.join(d, "spu_recomp.c"), "r", encoding="utf-8") as f:
            src = f.read()
        with open(os.path.join(d, "spu_recomp.h"), "r", encoding="utf-8") as f:
            hdr = f.read()
        c, h = twin_module(name, src, hdr)
        tdir = os.path.join(root, name + "_ftz")
        if write_if_changed(os.path.join(tdir, "spu_recomp.c"), c):
            made += 1
        write_if_changed(os.path.join(tdir, "spu_recomp.h"), h)

    twins = [n for n in base_names if os.path.isdir(os.path.join(root, n + "_ftz"))]
    for reg in ("jobmods_register.c", "jobmods_register_dxt.c"):
        if patch_registration(os.path.join(root, reg), twins):
            print(f"patched {reg}")
    if patch_workloads(os.path.join(root, "spu_workloads.c"), twins):
        print("patched spu_workloads.c")
    print(f"twins: {len(twins)} modules, {made} rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
