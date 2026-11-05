#!/usr/bin/env bash
# Offline gate for the native c690 0x21 emit command.
#
#   tools/c690_ref/run_native_21_test.sh <capture dir> [iters]
#
# <capture dir> is the PS3RECOMP_C690_21_CAPTURE spool (pNNN_in.bin).  For every
# input the harness runs the lifted 0x13080 and the native stage and compares
# the full LS, all 128 GPRs, the return PC, status and stop code.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORPUS=${1:?usage: $0 <capture dir> [iters]}
ITERS=${2:-1}
MOD=${SPU_LIFTED:-$ROOT/../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8}
OUT=${SPU_TEST_OUT:-$ROOT/../recomp-work/build-native21}
CC=${CC:-clang}
mkdir -p "$OUT"

python3 "$ROOT/tools/c690_ref/gen_reg_table.py" "$MOD/spu_recomp.h" "$OUT/reg_c690.c"

"$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off \
    -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I "$ROOT/include" -I "$ROOT/runtime/spu" -I "$ROOT/libs/video" \
    -o "$OUT/test_native_21_corpus" \
    "$ROOT/tools/c690_ref/test_native_21_corpus.c" "$OUT/reg_c690.c" \
    "$MOD/spu_recomp.c" "$ROOT/runtime/spu/c690_native_21.c" \
    "$ROOT/runtime/spu/job_c690_native.c" -lm

"$OUT/test_native_21_corpus" "$CORPUS" "$ITERS"
