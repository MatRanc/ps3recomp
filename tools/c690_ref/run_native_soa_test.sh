#!/usr/bin/env bash
# Offline gate + timing for the native c690 normal/tangent kernels.
#
#   [CMD21=<0x21 capture dir>] tools/c690_ref/run_native_soa_test.sh <dir with 0xFDA0 stage *_in.bin> [module dir]
#
# Works on a private copy of the lifted c690 module (patched with
# patch_native_soa.py, twins regenerated from it), so the shared spu_lifted
# tree is not touched. Each float twin is built with the game's flags and its
# lifted bodies are compared with the kernels over the whole local store.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=${1:?usage: $0 <stage dir> [module dir]}
MOD=${2:-$ROOT/../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8}
OUT=${SPU_TEST_OUT:-$ROOT/../recomp-work/build-soa-test}
CC=${CC:-clang}
N=$(basename "$MOD")
rm -rf "$OUT/spu"; mkdir -p "$OUT/spu"
cp -r "$MOD" "$OUT/spu/$N"
python3 "$ROOT/tools/c690_ref/patch_native_soa.py" "$OUT/spu/$N/spu_recomp.c"
python3 "$ROOT/tools/spu_ftz_twin.py" --spu-dir "$OUT/spu" >/dev/null
python3 "$ROOT/tools/spu_ftz_twin.py" --spu-dir "$OUT/spu" --approx >/dev/null

rc=0
v=0
for suf in "" _ftz _approx; do
    M="$OUT/spu/$N$suf"
    # The dispatcher's table: this twin's functions.
    python3 "$ROOT/tools/c690_ref/gen_reg_table.py" "$OUT/spu/$N/spu_recomp.h" "$OUT/reg_base.c" >/dev/null
    sed "s/${N}_spu_func/${N}${suf}_spu_func/g" "$OUT/reg_base.c" > "$OUT/reg$suf.c"
    MODS="$OUT/spu/$N/spu_recomp.c"; [ -z "$suf" ] || MODS="$MODS $M/spu_recomp.c"
    "$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off -w \
        -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS -Dmain=screen_test_main \
        -I "$ROOT/include" -I "$ROOT/runtime/spu" -I "$ROOT/libs/video" \
        -c -o "$OUT/stubs$suf.o" "$ROOT/tools/c690_ref/test_native_screen.c"
    "$CC" -O2 -march=native -mavx2 -mfma -ffp-contract=off -w \
        -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS -DPFX="${N}${suf}_" -DVARIANT=$v \
        -I "$ROOT/include" -I "$ROOT/runtime/spu" -I "$ROOT/libs/video" \
        -o "$OUT/test_native_soa$suf" \
        "$ROOT/tools/c690_ref/test_native_soa.c" "$OUT/stubs$suf.o" "$OUT/reg$suf.c" \
        $MODS "$ROOT/runtime/spu/c690_native_soa.c" \
        "$ROOT/runtime/spu/c690_native_screen.c" "$ROOT/runtime/spu/c690_cull_fix.c" \
        "$ROOT/runtime/spu/c690_holes.c" -lm
    "$OUT/test_native_soa$suf" "$STAGE"/*_in.bin || rc=1
    # Whole 0x21 commands (PS3RECOMP_C690_21_CAPTURE corpus), when present.
    if [ -n "${CMD21:-}" ]; then "$OUT/test_native_soa$suf" --cmd 13080 "$CMD21"/p*_in.bin || rc=1; fi
    v=$((v + 1))
done
exit $rc
