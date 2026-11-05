#!/usr/bin/env bash
#
# Offline ownership harness for the PS3RECOMP_C690_GPU_2A binding.
#
# Drives the real runtime/spu/c690_gpu_2a.c export/PUT path from five lane
# threads that all carry spu_id 0 (what spu_run_spurs_job creates) plus one
# walker thread that drains a mirror of vk_c690_sink/vk_c690_prepare. The
# captured meter tail spools (C69STG01) are the input when present; otherwise
# the harness synthesises 32-byte format-A jobs.
#
# Runs both sanitizer variants:
#   address+undefined  heap-use-after-free / double free / bounds
#   thread             shared lane state and the live key
#
# Usage: tools/run_c690_gpu_2a_race.sh [spool-dir]
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

SPOOL="${1:-${ROOT_DIR}/../recomp-work/captures/gpu2a/fncap2b2w3}"
OUT="${C690_GPU_2A_RACE_OUT:-/tmp/opencode/gpu2a_race}"
mkdir -p "${OUT}"

SRC="tests/test_c690_gpu_2a_race.c runtime/spu/c690_gpu_2a.c libs/video/ps3_live_ctl.c"
INC="-Iruntime/spu -Ilibs/video -Iinclude"
LINK="-lpthread -lm"

echo "[race] spool: ${SPOOL}"
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer ${INC} \
    -o "${OUT}/race_asan" ${SRC} ${LINK}
C690_GPU_2A_SPOOL="${SPOOL}" \
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
    "${OUT}/race_asan"

clang -O1 -g -fsanitize=thread -fno-omit-frame-pointer ${INC} \
    -o "${OUT}/race_tsan" ${SRC} ${LINK}
C690_GPU_2A_SPOOL="${SPOOL}" TSAN_OPTIONS=halt_on_error=1 \
    "${OUT}/race_tsan"

echo "[race] ASan/UBSan and TSan clean"
