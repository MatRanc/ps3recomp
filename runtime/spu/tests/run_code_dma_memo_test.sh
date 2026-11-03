#!/usr/bin/env bash
# Offline unit test for SPU_CODE_DMA_MEMO: a GET into a VERIFIED streamed-code
# span is skipped only when the source bytes are identical to local store, and
# only at the registered source/offset mapping. Builds the real SPU runtime
# units, not a stub of the function under test.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
S="$REPO/runtime/spu"
CC="${CC:-clang}"
OUT="${SPU_TEST_OUT:-$HERE/out_code_dma_memo}"
mkdir -p "$OUT"
"$CC" -std=gnu17 -O2 -w -I "$S" -I "$REPO/include" -I "$REPO/runtime/platform" \
    -o "$OUT/test_code_dma_memo" \
    "$HERE/test_code_dma_memo_main.c" \
    "$S/spu_channels.c" "$S/spu_coherency.c" "$S/spu_vm_pagemap.c" "$S/spu_drain.c" \
    "$S/spu_interp.c" "$S/spu_workload.c" "$S/spurs_policy.c" "$S/spurs_job.c" \
    "$S/spu_tsp_weak.c" "$S/spurs_policy_blob_weak.c" "$S/c690_stats.c" \
    "$S/c690_corpus.c" "$S/spu_lockstep.c" \
    "$REPO/runtime/platform/win32_compat.c" "$REPO/runtime/host/host_stubs.c" \
    -lpthread -lm
"$OUT/test_code_dma_memo"
