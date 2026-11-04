#!/usr/bin/env bash
# Offline gate for the native screen cull (0x25/0x26, LS 0xD080 subtree).
#
#   tools/c690_ref/run_native_screen_test.sh <corpus dir> [module dir]
#
# Runs the patched generated module in four modes over every captured cull
# pair (same spool as run_cull_pair_test.sh):
#   lift  - lifted body, no switches            (baseline)
#   exact - native mode 0                        (must equal the capture)
#   union - native mode 1                        (superset of lift)
#   fix   - lifted union driver (c690_cull_fix)  (must equal native union)
# and compares the full LS/GPR hashes and kept-index lists.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORPUS=${1:?usage: $0 <corpus dir> [module dir]}
MOD=${2:-$ROOT/../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8}
OUT=${SPU_TEST_OUT:-$ROOT/../recomp-work/build-screen}
CC=${CC:-gcc}
mkdir -p "$OUT"

python3 "$ROOT/tools/c690_ref/gen_reg_table.py" "$MOD/spu_recomp.h" "$OUT/reg_c690.c"

"$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off \
    -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I "$ROOT/include" -I "$ROOT/runtime/spu" -I "$ROOT/libs/video" \
    -o "$OUT/test_native_screen" \
    "$ROOT/tools/c690_ref/test_native_screen.c" "$OUT/reg_c690.c" \
    "$MOD/spu_recomp.c" "$ROOT/runtime/spu/c690_native_screen.c" \
    "$ROOT/runtime/spu/c690_cull_fix.c" -lm

for m in lift exact union fix; do
    case "$m" in
        exact) env PS3RECOMP_C690_NATIVE_SCREEN=1 "$OUT/test_native_screen" "$m" "$CORPUS" ;;
        union) env PS3RECOMP_C690_NATIVE_SCREEN=2 "$OUT/test_native_screen" "$m" "$CORPUS" ;;
        fix)   env PS3RECOMP_C690_CULL_FIX=1 "$OUT/test_native_screen" "$m" "$CORPUS" ;;
        *)     "$OUT/test_native_screen" "$m" "$CORPUS" ;;
    esac > "$OUT/$m.txt" 2> "$OUT/$m.err"
done

python3 - "$OUT" <<'PY'
import sys, collections
out = sys.argv[1]
def read(name):
    pairs = {}
    lists = {}
    for line in open(f"{out}/{name}.txt"):
        f = line.split()
        if not f: continue
        if f[0] == "PAIR":
            pairs[int(f[2])] = f
        elif f[0] == "LIST":
            lists[int(f[2])] = [int(x) for x in f[3:]]
    return pairs, lists

P = {}; L = {}
for m in ("lift", "exact", "union", "fix"):
    P[m], L[m] = read(m)

seqs = sorted(P["lift"])
err = 0
exact_ls_bad = exact_gpr_bad = exact_pc_bad = 0
union_removed = union_added = union_ls_out = 0
fix_diff = 0
base_capture_bad = 0
for s in seqs:
    a = P["lift"][s]; b = P["exact"][s]; c = P["union"][s]; d = P["fix"][s]
    # fields: 0 PAIR 1 mode 2 id 3 seq 4 ctx 5 hls 6 hgpr 7 pc 8 ptr 9 ln 10 bad
    if a[10] != "0": base_capture_bad += 1
    if a[5] != b[5]: exact_ls_bad += 1
    if a[6] != b[6]: exact_gpr_bad += 1
    if a[7] != b[7]: exact_pc_bad += 1
    bl, ul, fl = L["lift"][s], L["union"][s], L["fix"][s]
    # union must begin with the original list (no removal or reordering)
    if ul[:len(bl)] != bl: union_removed += 1
    union_added += max(0, len(ul) - len(bl)) // 3
    # the whole union output must equal the lifted union fix
    if ul != fl: fix_diff += 1
    # Full-LS/GPR equality with the lifted union driver: it is the proven
    # safe implementation, so the native union must be byte-identical.
    if c[5] != d[5] or c[6] != d[6]: fix_diff += 1

# outside-list check: exact mode must be byte-identical to lift, and the
# native union's non-list bytes must match the lifted union's non-list bytes.
# The dump only carries hashes, so re-read the per-pair extra field: PAIR
# carries (ptr, len, bad); the full-LS comparison is the hash equality above.
print(f"pairs                 : {len(seqs)}")
print(f"baseline capture bad  : {base_capture_bad} (must be 0)")
print(f"exact LS hash differs : {exact_ls_bad} (must be 0)")
print(f"exact GPR hash differs: {exact_gpr_bad} (must be 0)")
print(f"exact pc differs      : {exact_pc_bad} (must be 0)")
print(f"union removed/reorder : {union_removed} (must be 0)")
print(f"union added triangles : {union_added}")
print(f"union != lifted union : {fix_diff} (must be 0)")
err = base_capture_bad or exact_ls_bad or exact_gpr_bad or exact_pc_bad \
      or union_removed or fix_diff
print("RESULT:", "FAIL" if err else "PASS")
sys.exit(1 if err else 0)
PY

# mean ns/call from the per-mode stderr
echo "--- timings ---"
for m in lift exact union fix; do
    cat "$OUT/$m.err"
done
