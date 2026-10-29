#!/usr/bin/env bash
# Vertex fetch+convert against the validation a content-checked vertex cache
# would do instead (bench_vertex_cache.c).
#
#   tests/bench/vertex_cache/run.sh [--native] [--frame]
#     --native  build with -march=native (F16C halves) instead of the
#               runtime's -msse4.1
#     --frame   only the whole-frame table
#
# Built with the runtime's flags (CMakeLists.txt: -O2 -ffp-contract=off
# -msse4.1), against the real rsx_vertex_compact.c. Output binary goes to
# $VTX_BENCH_OUT (default: tests/bench/vertex_cache/out).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${VTX_BENCH_OUT:-$HERE/out}"
CC="${CC:-clang}"
ISA=-msse4.1
if [ "${1:-}" = "--native" ]; then ISA=-march=native; shift; fi

mkdir -p "$OUT"
V="$REPO/libs/video"
"$CC" -std=gnu17 -O2 -ffp-contract=off "$ISA" -Wall -Wextra -Wno-unused-parameter \
    -I "$REPO/include" -I "$V" -o "$OUT/bench_vertex_cache" \
    "$HERE/bench_vertex_cache.c" "$V/rsx_vertex_compact.c" "$V/rsx_dispatch.c" -lm
"$OUT/bench_vertex_cache" "$@"
