#!/bin/sh
# The FIFO walker before and after: same frame, same record, ns per word.
#
#   tests/bench/fifo/run_fifo_bench.sh [BASE]     BASE: a git revision (default HEAD)
#
# Builds bench_fifo.c twice -- against BASE's libs/video (git archive) and
# against the working tree -- then, for each configuration (bench_fifo.c),
# runs a --verify replay (register state hashed at every handler call) and a
# timed replay of each, and fails if the handler-call record or the final
# state differs. FIFO_BENCH_FRAMES sets the timed frames (100). Output in
# tests/bench/fifo/out/ (or $FIFO_BENCH_OUT).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
base=${1:-HEAD}
out=${FIFO_BENCH_OUT:-$here/out}
cc=${CC:-cc}
mkdir -p "$out"
rm -rf "$out/base"
mkdir -p "$out/base"
git -C "$root" archive "$base" libs/video include runtime | tar -x -C "$out/base"

flags="-std=gnu17 -D_GNU_SOURCE -O2 -fno-strict-aliasing -ffp-contract=off -w"
case $(uname -m) in x86_64|amd64) flags="$flags -msse4.1" ;; esac
case $(uname -s) in Darwin) gc="-Wl,-dead_strip" ;; *) gc="-ffunction-sections -fdata-sections -Wl,--gc-sections" ;; esac

build() {   # tree, output
    # A tree from before the dispatcher's run API has no engine run to stub.
    old=; grep -q rsx_dispatch_method_run "$1/libs/video/rsx_dispatch.h" || old=-DBENCH_BASE
    $cc $flags -I "$1/include" -I "$1/libs/video" $old "-DGCM_SYS_C=\"$1/libs/video/cellGcmSys.c\"" \
        "$here/bench_fifo.c" "$1/libs/video/rsx_dispatch.c" $gc -pthread -o "$2"
}
build "$out/base" "$out/bench_base"
build "$root" "$out/bench_new"

fail=0
for cfg in engine legacy vtable; do
    for mode in "--verify --frames 2" "--frames ${FIFO_BENCH_FRAMES:-100}"; do
        "$out/bench_base" --$cfg $mode 2>/dev/null | grep -v '^\[' > "$out/base.txt"
        "$out/bench_new"  --$cfg $mode 2>/dev/null | grep -v '^\[' > "$out/new.txt"
        grep -v '^time' "$out/base.txt" > "$out/base.rec"
        grep -v '^time' "$out/new.txt"  > "$out/new.rec"
        head -2 "$out/new.txt"
        if cmp -s "$out/base.rec" "$out/new.rec"; then
            echo "  record identical: $(grep '^calls' "$out/new.txt") / $(grep '^state' "$out/new.txt")"
        else
            echo "  RECORD DIFFERS"; diff "$out/base.rec" "$out/new.rec" || true; fail=1
        fi
        case $mode in --verify*) continue ;; esac   # hashing at every call: not a timing
        echo "  before: $(grep '^time' "$out/base.txt")"
        echo "  after:  $(grep '^time' "$out/new.txt")"
    done
done
exit $fail
