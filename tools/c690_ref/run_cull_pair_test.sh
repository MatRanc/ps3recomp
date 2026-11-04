#!/usr/bin/env bash
# Offline gate for the 0x25/0x26 cull union fix.
#
#   tools/c690_ref/run_cull_pair_test.sh <corpus dir> [module dir]
#
# The corpus is a PS3RECOMP_C690_CULL_PAIRS spool (cull_*.bin). The module dir
# defaults to recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8 and is read-only.
# The generated module must have been patched with patch_cull_hooks.py (the
# test calls the *_body symbol directly, so either wrapper works).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORPUS=${1:?usage: $0 <corpus dir> [module dir]}
MOD=${2:-$ROOT/../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8}
OUT=${SPU_TEST_OUT:-$ROOT/../recomp-work/build-holes}
CC=${CC:-gcc}
mkdir -p "$OUT"

python3 "$ROOT/tools/c690_ref/gen_reg_table.py" "$MOD/spu_recomp.h" "$OUT/reg_c690.c"

"$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off \
    -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I "$ROOT/include" -I "$ROOT/runtime/spu" -I "$ROOT/libs/video" \
    -o "$OUT/test_cull_pairs" \
    "$ROOT/tools/c690_ref/test_cull_pairs.c" "$OUT/reg_c690.c" \
    "$MOD/spu_recomp.c" "$ROOT/runtime/spu/c690_cull_fix.c" -lm

PS3RECOMP_C690_CULL_FIX=1 "$OUT/test_cull_pairs" "$CORPUS"
