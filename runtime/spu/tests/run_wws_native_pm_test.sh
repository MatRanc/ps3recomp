#!/bin/bash
# Build the WWS PM claim differential test against the generated (game-derived)
# policy module. The module must have been patched with
# tools/c690_ref/patch_wws_pm_native.py first.
#
# Usage: run_wws_native_pm_test.sh [path/to/wwsjob_pm/spu_recomp.c]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
MOD=${1:-$ROOT/../recomp-work/spu_lifted/wwsjob_pm/spu_recomp.c}
CC=${CC:-gcc}

if ! grep -q wws_pm_native_claim "$MOD"; then
    echo "error: $MOD is not patched; run tools/c690_ref/patch_wws_pm_native.py" >&2
    exit 1
fi

OUT=$(mktemp -d "${TMPDIR:-/tmp}/wws_native_pm.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

"$CC" -std=gnu17 -O2 -march=native -mavx2 -mfma -mssse3 -ffp-contract=off \
    -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS -DWWS_NATIVE_PM_TEST \
    -I "$ROOT/runtime/spu" -I "$ROOT/include" -I "$ROOT/runtime/platform" \
    -I "$(dirname "$MOD")" \
    "$MOD" "$HERE/test_wws_native_pm_main.c" \
    -lm -o "$OUT/test_wws_native_pm"
"$OUT/test_wws_native_pm"
