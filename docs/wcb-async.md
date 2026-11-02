# On-demand render-target readback (WCB ondemand / async)

MLB The Show 16 renders some textures on the GPU and then reads them from the
PPU or an SPU: the generated player faces (cached to `HeadFaceCacheFile`), the
crowd sprite sheets, probably the jersey/number composites. The draw engine
renders on the host GPU, so unless the rendered bytes are copied back into
guest memory those reads see black.

`PS3RECOMP_RSX_WCB=1` (the original "write colour buffers") copies **every**
dirty offscreen target at **every** sync point (`SET_REFERENCE`,
`SEMAPHORE_RELEASE`, flip), each behind a full GPU stall. On the real machine
that took MLB 16 from ~24 fps to ~11 fps, which is why it is off by default.
Most of those copies are wasted: of the ~440 draws a frame, most targets are
shadow maps, G-buffer planes and post-processing chains that only the GPU ever
samples again.

This document describes two new modes that copy only what the guest actually
reads, and copy it only when it reads it.

## Modes

| `PS3RECOMP_RSX_WCB` | What happens at a sync point | When a guest thread reads a target |
|---|---|---|
| `0` / `off` (default) | nothing | it reads whatever guest memory holds (black) |
| `1` / `sync` | every dirty offscreen target is read back now, each behind a GPU stall | bytes are already there |
| `2` / `ondemand` | each dirty offscreen target's guest pages are **protected**; nothing is copied | the access faults; the engine thread copies that target, the pages reopen, the access is retried |
| `3` / `async` | as `ondemand`, plus: a target the guest read within the last `PS3RECOMP_RSX_WCB_HOT_FRAMES` frames (default 8) gets a GPU copy **queued** into a staging buffer (its own fence, no wait) | the fault only collects the queued copy, waiting for that batch alone if it is not done yet |

Display buffers are never read back in any mode (they are presented).

Default stays `off` until the real-machine A/B below has been run.

### When each mode is correct

* **sync** — always gives the bytes as of the sync point. Slow.
* **ondemand** — the guest never sees stale or partial bytes, but it gets the
  target **as it is when the guest first touches it**, which can include draws
  the RSX executed after the sync point the guest waited for. That is the same
  thing real hardware would show if the RSX had already moved on, and it only
  matters for a title that reads a target while also letting the RSX render
  into it again without a semaphore in between (a race on hardware too). For
  the MLB cases (faces rendered, then read and cached; crowd sheets rendered,
  then read by SPU jobs) the target is not redrawn before the read.
* **async** — for a target with a queued copy, the guest gets exactly the
  bytes as of the sync point (same as `sync`), even when the target is drawn
  again before the read. The first read of a target (no history yet) behaves
  as `ondemand`. Reads in command-stream order by the engine itself (an NV3089
  transfer out of the target, a texture upload over it) always take the
  current bytes, as they must.

No mode returns a previous frame's bytes in place of the current ones; there is
no "one frame late" mode. Faces and crowd sheets are rendered once and read
once, so a late mode would mostly read black on the first frame anyway.

## Design

### Why page protection

Guest memory is one flat host mapping (`vm_base + ea`, `runtime/memory/vm.h`):
recompiled PPU code, SPU DMA (`memcpy` in `spu_dma.h`) and the HLE all touch it
with ordinary loads and stores. Protecting the host pages of a target is the one
signal that catches *every* reader, including ones the runtime has no hook in
(a PPU loop walking a crowd sheet, an SPU job DMAing a face). The alternatives
were weaker: SPU DMA GETs and texture uploads are only some of the readers, and
the PPU has no read hook at all.

### The pieces

* `libs/video/rsx_wcb.{h,c}` — a target-agnostic region tracker in **host**
  addresses: a cookie and a page span per region, a spinlock-guarded table,
  `mprotect`/`VirtualProtect`, the fault handler (POSIX `sigaction` on
  `SIGSEGV`/`SIGBUS`, chained to the previous handler; Windows: first-in-line
  VEH), request/wait between threads, and counters. Compiled inside
  `rsx_draw_engine.c` (`#include`), so every engine build has it.
* `libs/video/rsx_draw_engine.c` — the policy: which targets to arm, the copy
  itself (`eng_wcb_resolve`), the hot-target heuristic, async tickets,
  eviction/shutdown, counters.
* Backend: three optional entries in `rsx_draw_backend` —
  `readback_async`, `readback_async_fetch`, `readback_async_release`. The
  Vulkan backend implements them; Metal/D3D12 leave them NULL and `async`
  then behaves as `ondemand`.

### Sync point (ondemand/async)

For each dirty, non-display colour target and each dirty depth target that is
not already armed: protect the pages covering
`[ea, ea + pitch * (h - 1) + w * bpp)` (`PROT_NONE`). If the host refuses,
the target is copied now, as `sync` would. In `async`, a hot target also gets
`readback_async` (a new one if it was drawn since the last ticket; the old
ticket is released). Nothing here waits for the GPU.

A target that stays armed and is drawn again is not re-armed or copied: it
simply stays armed. That is the saving — a shadow map is armed once and never
copied.

### The read

1. A guest thread (PPU, SPU, HLE) touches an armed page and faults.
2. The handler finds the regions covering that page, marks them requested,
   wakes the engine thread (the FIFO kick event) and waits (yield, then 20 µs /
   200 µs sleeps).
3. The engine thread services requests between methods
   (`rsx_draw_engine_method`), at the start of each FIFO pass and in its idle
   wait (`cellGcm_fifo_kick_wait`). For each region: take the queued copy if
   the request came from a guest thread and one exists, else read the target
   back (one `submit_and_wait` for that target only); convert to the guest
   layout; write it.
4. The pages reopen; the waiting thread retries its access and sees the bytes.

A fault on the engine thread itself (walker code, a guest callback running on
it) resolves inline. Engine reads of guest memory (`eng_guest_ptr`) resolve
overlapping regions first, in stream order, without faulting.

### Never stale, never partial

* The pages stay inaccessible to every thread until the copy is complete. On
  Linux the copy is written **through** the protection with `pwrite` on
  `/proc/self/mem` (probed once at start-up), and the pages are reopened only
  after the last byte lands. A concurrent reader either faults and waits or
  sees the whole copy.
* A page shared by two armed targets is reopened only when neither is armed;
  touching it resolves both.
* Where `/proc/self/mem` cannot be used (Windows, macOS, a kernel booted with
  `proc_mem.force_override=never`), the pages are reopened **before** the copy
  and a reader on another thread can see it half written. Targets sharing a page
  are then copied as a group. The start-up line says which applies:
  `writes through the protection` or `reopen-then-write`.

### Host paths that do not fault

* `cellFsWrite` (libs/filesystem): `write(2)` on a protected buffer fails with
  `EFAULT` instead of faulting, so it calls `ps3_guest_access()` first
  (`include/ps3emu/guest_access.h`). `runtime/ppu/ppu_fs.cpp` already
  write-touches the buffer (`fs_prefault`), which faults and resolves.
* The SPU DMA guard (`mfc_ea_range_committed`, first touch of a 64 KB block):
  the POSIX `VirtualQuery` shim reports a `PROT_NONE` page as uncommitted and
  the guard would "commit" it read-write over stale bytes, so it calls the hook
  before asking.

### Failure handling

* A request the engine thread does not service within
  `PS3RECOMP_RSX_WCB_TIMEOUT_MS` (default 5000) reopens the pages with the
  stale bytes, logs once, and counts a timeout; the next sync point arms the
  target again. A hang is worse than one wrong texel, and the counter shows it.
* Evicting or recreating a target, or shutting the engine down, copies whatever
  is still armed and reopens the pages.

## Environment variables

| Variable | Meaning |
|---|---|
| `PS3RECOMP_RSX_WCB` | `0`/`off`, `1`/`sync`, `2`/`ondemand`, `3`/`async` |
| `PS3RECOMP_RSX_WCB_STATS=N` | print counters every N frames (per-frame averages over the window) |
| `PS3RECOMP_RSX_WCB_HOT_FRAMES` | async: a read this many frames ago still makes a target hot (default 8) |
| `PS3RECOMP_RSX_WCB_TIMEOUT_MS` | how long a guest thread waits for a copy before reading stale bytes (default 5000) |
| `PS3RECOMP_RSX_WCB_FORCED=0` | disable the `/proc/self/mem` write-through (test the fallback) |

The stats line:

```
[rsx wcb] 60 frames, per frame: dirty D armed A copied C (K KB: sync S demand M) async issued I fetched F discarded X; faults Fa waits W (avg U us, max V us) copy T ms; timeouts N pending P
```

* `dirty` — targets that became dirty after their last copy
* `armed` — targets newly protected at a sync point
* `copied`, `KB` — copies to guest memory and their size; `sync` = at a sync
  point (sync mode, or protection refused), `demand` = because the guest read
* `async issued/fetched/discarded` — queued copies; fetched ones served a read;
  discarded ones were superseded or dropped unread
* `faults`, `waits` — guest faults on armed pages; guest-thread blocks and their
  average/maximum duration (max is since start)
* `copy` — engine-thread time spent copying per frame
* `pending` — regions armed right now (GPU-only targets stay armed: expected)

## Real-machine test plan

Build with the Vulkan backend as usual. Use the same save/scene for every run
(e.g. a stadium at-bat view with crowd and a close-up of the batter). Let each
run settle for ~60 s before reading fps.

1. **Baseline** — `PS3RECOMP_RSX_WCB=0`. Expect ~36 fps, black faces/crowd.
2. **Old** — `PS3RECOMP_RSX_WCB=1 PS3RECOMP_RSX_WCB_STATS=60`. Expect ~11 fps;
   correct visuals; stats `sync` ≈ `copied` ≈ tens per frame.
3. **ondemand** — `PS3RECOMP_RSX_WCB=2 PS3RECOMP_RSX_WCB_STATS=60`.
4. **async** — `PS3RECOMP_RSX_WCB=3 PS3RECOMP_RSX_WCB_STATS=60`.
5. If 4 shows few fetches, add `VK_DRAW_BATCH=1` (async copies are only
   asynchronous when draw batching is on; without it each is one synchronous
   submission and async ≈ ondemand).

What to look for:

* Start-up line: `[rsx wcb] on-demand ... (writes through the protection ...)`.
  If it says `reopen-then-write`, the kernel refused `/proc/self/mem`; report it.
* In 3/4, steady state: `copied` per frame should be small (the targets the game
  reads — a handful), not tens; `pending` stays non-zero (GPU-only targets).
  `timeouts` must be 0.
* `waits` avg: the price of each read. In 4 it should fall below 3 once targets
  are hot, with `fetched` ≈ the per-frame reads.
* fps: 3 and 4 should be close to the baseline 36, far above 11. If `copied` is
  high, a CPU loop is touching many target pages (false sharing or a CPU-written
  target); note which and send the stats line.
* Visuals, compared against run 2: **crowd** sprites present and not black or
  smeared; **player faces** correct on the batter close-up and after a reload
  (faces come from `HeadFaceCacheFile` — delete the cache once to force
  regeneration); **jerseys/numbers** correct on several players.
* Stability: no new crashes; a crash with a SIGSEGV inside guest code that is
  *not* reported by the guest-pointer trap would point at the fault handler
  chain.

## Risks / open questions

* **Semantics in ondemand**: the guest gets the target as of its first touch,
  not as of the sync point (see above). async fixes this for hot targets only.
* **Non-Linux hosts**: reopen-then-write can expose a half-written copy to a
  concurrent reader. Only matters if the game reads from two threads at once
  while the copy runs.
* **False sharing**: armed pages are whole host pages (4 KB, 16 KB on Apple
  Silicon). CPU data sharing a page with a GPU-only target makes every touch of
  it copy that target once per sync point. Visible as high `demand` copies.
* **CPU-written targets**: a CPU write to an armed target first copies it (the
  write must land on the rendered bytes). Correct, but a wasted readback when
  the CPU overwrites it whole.
* **Kernel reads**: system calls other than `cellFsWrite` that read a guest
  buffer straight from an armed page fail with `EFAULT` instead of faulting
  (network send, other file paths). Add `ps3_guest_access()` where that shows up.
* **Other fault handlers**: a handler installed *after* this one that claims
  `PROT_NONE` faults as its own (a demand-commit VEH on Windows ports) could
  reopen pages without a copy. Linux ports have none today.
* **Depth targets** are copied on demand but never asynchronously.
* **Signal-context work**: the waiting thread spins/sleeps inside the SIGSEGV
  handler and the engine thread may copy inline from one. This relies on the
  fault being synchronous (a guest load/store), which is the only kind armed
  pages produce.
* **Engine-thread stalls**: a guest thread waits as long as the engine thread
  is busy elsewhere (a long shader compile, a present). The timeout bounds it.
