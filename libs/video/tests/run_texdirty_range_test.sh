#!/usr/bin/env bash
# Offline unit test for the TEX_DIRTY source-range tracker (SPU_CODE_DMA_MEMO's
# O(1) path). Builds the real libs/video/rsx_wcb.c, no game code.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-cc}"
OUT="${TEXDIRTY_TEST_OUT:-$HERE/out_texdirty_range}"
mkdir -p "$OUT"
"$CC" -std=gnu17 -O2 -w -I "$REPO/libs/video" -I "$REPO/include" \
    -o "$OUT/test_texdirty_range" \
    "$HERE/test_texdirty_range.c" "$REPO/libs/video/rsx_wcb.c" \
    -lpthread -lm
"$OUT/test_texdirty_range"
