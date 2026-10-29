# FIFO walker replay benchmark

Nanoseconds per method word through `cellGcm_rsx_process_fifo`, and a check
that two builds of the walker do exactly the same thing.

```sh
tests/bench/fifo/run_fifo_bench.sh              # working tree against HEAD
tests/bench/fifo/run_fifo_bench.sh mlb16-work   # ... against any revision
CC=clang FIFO_BENCH_FRAMES=300 tests/bench/fifo/run_fifo_bench.sh
```

`bench_fifo.c` writes one synthetic frame into IO memory: about 300k method
words in 49k headers, and 128k NOP words. 90% of the method words are
register writes on subchannels 0 and 1:

- runs of 1-16, incrementing and non-incrementing: texture units, vertex
  formats, packed vertex constants and the rest of the render state;
- transform constant uploads of 4-256 words the way
  cellGcmSetVertexProgramConstants writes them, one header per 32 words
  starting at SET_TRANSFORM_CONSTANT_LOAD (sometimes the load in a header of
  its own), some running past slot 511;
- vertex program uploads of 32-508 words, the load slot and then 32 words
  per header.

The other 10% are methods with side effects: draws, clears, immediate and
inline vertices, NV406E semaphores, back-end labels, SET_REFERENCE,
GET_REPORT, notifies and a user command. The frame also has NOP runs (1-3
words of padding, and 32-1023 words left over in a reservation), JUMPs,
CALL/RET and a flip. The frame runs through the real walker (`cellGcmSys.c`,
included) into the real dispatcher (`rsx_dispatch.c`). The graphics backends
are stubbed, and each callback they would get is recorded instead.

The script builds the bench against both trees and replays each
configuration twice. The `--verify` run hashes the whole dispatcher state at
every handler call. The timed run reports the best of N frames. The script
fails if the handler-call record or the final state differs, or the `walk`
line: the walker's trace ring (what a `[FIFOSTEP]` dump shows), the number
of words it has read, the last ring offset and the park, hashed at the end
and, with `--verify`, at every handler call.

| config   | meaning |
|----------|---------|
| `engine` | register-file draw engine on, no vtable backend: the Metal and Vulkan default, and the path the walker sampler profiled |
| `legacy` | the engine on and `rsx_state` fed every method too (`PS3RECOMP_FIFO_LEGACY=1`) |
| `vtable` | the engine off: every method feeds `rsx_state` |

## Results

### fifo-dispatch-2: bulk uploads, NOP skip with the engine on

Intel Xeon @ 2.10GHz (4 vCPU, shared), gcc 13 `-O2`, the frame above. The timed run's best frame
moves by 15% or more between runs of the same binary on this shared host,
so these are medians of 15 interleaved runs of 30 frames each, pinned to
one core:

| config   | mlb16-work | bulk uploads | NOP skip | both |
|----------|------------|--------------|----------|------|
| `engine` | 12.7 ns/word | 11.0 ns/word | 9.8 ns/word | 8.1 ns/word |
| `legacy` | 14.3 ns/word | 14.4 ns/word | 11.8 ns/word | 11.7 ns/word |
| `vtable` | 6.2 ns/word  | 6.2 ns/word  | 6.3 ns/word  | 6.3 ns/word (unchanged path) |

With clang 18, `engine` goes from 14.2 to 9.4 ns/word. On the previous
frame (no NOPs, 16-word uploads) `engine` goes from 10.6 to 10.1. The
handler record and the final state are identical to mlb16-work in every
configuration, and so is `walk` for `engine` and `legacy`. `vtable`'s
`walk` differs on purpose: its NOP skip now leaves the trace ring and the
park as a word-at-a-time walk does, which the engine configurations always
did, and its hash is now the same as theirs.

- `rsx_dispatch_method_run` copies an incrementing run through the program
  (0x0B80-0x0BFC) or constant (0x1F00-0x1F7C) window in one go: the words
  land in consecutive words of `vp[]` / `constants[]`, so it is the
  register stores and write counts, one clipped copy, and the program load
  pointer advanced once. `rsx_dispatch_method` does nothing else for these
  words: no sink, no generation counter, no cache to invalidate; the engine
  reads `vp[]` and `constants[]` at the draw. Non-incrementing runs, a load pointer high
  enough to wrap, and `RSX_CONST_LOG` take the word-at-a-time path.
- The walker's NOP-run skip is on with the engines mirroring the stream. An
  engine never saw a NOP: a NOP is a method header with no data. The skip
  now fills the trace ring and forgets a park as the method path does.
  `TCONST_FIFO` and `GCM_RINGDBG` look at each NOP there, so with an engine
  on they keep the per-word walk.

What is left per word in `engine`: the walker's `gcm_3d_method_quiet` scan
(one call per method word, to find the stretch the dispatcher can take as a
run) and `gcm_io2ea`; in the dispatcher, the methods that still go one at a
time (packed vertex constants, texture registers, executes).

### fifo-dispatch

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
