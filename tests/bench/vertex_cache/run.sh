#!/usr/bin/env bash
# Vertex fetch+convert against the validation a content-checked vertex cache
# would do instead (bench_vertex_cache.c).
#
#   tests/bench/vertex_cache/run.sh [--native] [--frame | --direct]
#     --native  build with -march=native (F16C halves) instead of the
#               runtime's -msse4.1; VTX_BENCH_ISA=<flags> picks any other
#               (-march=znver1: a Zen+ host's instruction set on another one)
#     --frame   only the whole-frame tables
#     --direct  only the copy-vs-direct frame table (VTX_BENCH_PROFILE=<row>
#               runs one of its rows alone, 200 times, for a profiler)
#
# Built with the runtime's flags (CMakeLists.txt: -O2 -ffp-contract=off
# -msse4.1), against the real rsx_vertex_compact.c. Output binary goes to
# $VTX_BENCH_OUT (default: tests/bench/vertex_cache/out).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${VTX_BENCH_OUT:-$HERE/out}"
CC="${CC:-clang}"
ISA="${VTX_BENCH_ISA:--msse4.1}"
if [ "${1:-}" = "--native" ]; then ISA=-march=native; shift; fi

mkdir -p "$OUT"
V="$REPO/libs/video"
"$CC" -std=gnu17 -O2 -ffp-contract=off $ISA -Wall -Wextra -Wno-unused-parameter \
    -I "$REPO/include" -I "$V" -I "$V/tests" -o "$OUT/bench_vertex_cache" \
    "$HERE/bench_vertex_cache.c" "$V/rsx_vertex_compact.c" "$V/rsx_dispatch.c" -lm
"$OUT/bench_vertex_cache" "$@"
