# FIFO walker replay benchmark

Nanoseconds per method word through `cellGcm_rsx_process_fifo`, and a check
that two builds of the walker do exactly the same thing.

```sh
tests/bench/fifo/run_fifo_bench.sh              # working tree against HEAD
tests/bench/fifo/run_fifo_bench.sh mlb16-work   # ... against any revision
CC=clang FIFO_BENCH_FRAMES=300 tests/bench/fifo/run_fifo_bench.sh
```

`bench_fifo.c` writes one synthetic frame into IO memory: about 300k method
words in 62k headers. 90% of the words are register writes in runs of 1-16,
incrementing and non-incrementing, on subchannels 0 and 1. That includes
texture units, vertex formats, packed vertex constants, transform constants
and microcode. The other 10% are methods with side effects: draws, clears,
immediate and inline vertices, NV406E semaphores, back-end labels,
SET_REFERENCE, GET_REPORT, notifies and a user command. The frame also has
JUMPs, CALL/RET and a flip. The frame runs through the real walker
(`cellGcmSys.c`, included) into the real dispatcher (`rsx_dispatch.c`).
The graphics backends are stubbed, and each callback they would get is
recorded instead.

The script builds the bench against both trees and replays each
configuration twice. The `--verify` run hashes the whole dispatcher state at
every handler call. The timed run reports the best of N frames. The script
fails if the handler-call record or the final state differs.

| config   | meaning |
|----------|---------|
| `engine` | register-file draw engine on, no vtable backend: the Metal and Vulkan default, and the path the walker sampler profiled |
| `legacy` | the engine on and `rsx_state` fed every method too (`PS3RECOMP_FIFO_LEGACY=1`) |
| `vtable` | the engine off: every method feeds `rsx_state` |

## Results

Intel Xeon @ 2.80GHz (4 vCPU, shared), gcc 13 `-O2`. The first column is the
walker before `fifo-dispatch`; the last two are the two commits on it:

| config   | before   | run path | + dispatcher run |
|----------|----------|----------|------------------|
| `engine` | 19.7 ns/word | 14.0 ns/word | 11.2 ns/word |
| `legacy` | 20.0 ns/word | 14.4 ns/word | 14.2 ns/word |
| `vtable` | 7.4 ns/word  | 7.7 ns/word  | 7.6 ns/word (code unchanged: noise) |

With clang 18, `engine` goes from 20.5 to 12.9 ns/word. The handler record
and the final state are identical in every configuration.

The vtable path's real cost is `rsx_process_method` (stubbed here, as are
the backends' draws), so its column only measures the walk itself.
