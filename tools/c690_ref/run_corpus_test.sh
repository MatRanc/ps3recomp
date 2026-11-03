#!/bin/bash
# Build the c690 stage reference and compare it with every captured 0xFDA0
# stage pair in a PS3RECOMP_C690_CORPUS directory.
#
#   tools/c690_ref/run_corpus_test.sh recomp-work/corpus_edge1_...
#
# The reference and the lifted module must use the same SPU float/LS options
# for the comparison to be bit-exact.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORPUS=${1:?usage: $0 <corpus dir> [stage dir]}
STAGE=${2:-$CORPUS/stage}
CC=${CC:-clang}

python3 "$ROOT/tools/c690_corpus_report.py" "$CORPUS" --extract-stage "$STAGE" >/dev/null

"$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off \
    -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I "$ROOT/runtime/spu" -I "$ROOT/include" \
    -o "$STAGE/test_ref_c690_stage" \
    "$ROOT/tools/c690_ref/test_ref_c690_stage.c"

pass=0
fail=0
for meta in "$STAGE"/job_*_meta.txt; do
    [ -e "$meta" ] || continue
    base=${meta%_meta.txt}
    if "$STAGE/test_ref_c690_stage" "${base}_in.bin" "${base}_out.bin"; then
        pass=$((pass + 1))
    else
        echo "FAIL ${base##*/}"
        fail=$((fail + 1))
    fi
done
echo "stage pairs: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
