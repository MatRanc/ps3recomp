# What the walker waits on at a present (Vulkan, `VK_FRAME_PIPELINE=1`)

This covers `libs/video/rsx_vulkan_backend.c` on the register-file draw engine
path (`PS3RECOMP_RSX_ENGINE=dispatch`) with `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`.
Line numbers are as of the commit that adds this file.

The symptom: the FIFO walker spends about 11% of its time in
`vk_eng_flush_async` at the present, blocked on a fence, while the GPU is only
about 33% busy. A frame is about 750 draws, a batch is submitted every 512
(`VK_DRAW_BATCH_MAX`) and at the present.

Short version: with two batch sets the pipeline is **one submission** deep, not
one frame deep. A frame is two uneven batches (512 + ~238), so at the present
the walker waits for the frame's own 512-draw batch, which it submitted only
~238 draws of recording earlier. That wait is latency, not throughput: the GPU
starts the batch only when it is submitted, and has to finish 512 draws in the
time the walker records 238. `VK_BATCH_SETS=3` makes the ring deep enough that
the present waits on the previous frame instead.

## 1. One frame, step by step

Submissions in queue order, two batch sets, frame `f`:

| # | Submission | Set | Submitted at | What the walker then waits on |
|---|------------|-----|--------------|-------------------------------|
| 1 | `A_f`: draws 1-512 | 0 | 512th draw, `vk_eng_draw` → `vk_eng_flush_async` (4047) | set 1's fence = `B_{f-1}`, the previous frame's last batch |
| 2 | `B_f`: draws 513-750 | 1 | present, `vk_eng_present` → `vk_eng_flush_async` (2553) | set 0's fence = **`A_f`** |
| 3 | `P_f`: the present's blit + copy to `readback` | present's own | `vk_present_record_end` (2146) | `P_{f-1}`'s fence, at the *next* present |

The wait in row 2 is the one the profile shows. `A_f` holds 68% of the frame's
draws; if the GPU's frame time is a third of the wall-clock frame, `A_f` alone
takes ~22% of a frame on the GPU, and it cannot start before row 1. The walker
reaches row 2 after recording 238 draws, so it waits for the rest of `A_f`
plus the submit-to-start latency. The two batches are uneven, and the present
always waits on the bigger one, freshly submitted.

## 2. Every wait at a present, in order

`rsx_vulkan_backend_present` (4341) → `present_begin` (4353) →
`rsx_draw_engine_present` → `eng_present` (rsx_draw_engine.c:3460) →
`vk_eng_present` (2549):

1. **Batch-set fence, `vk_ebs_swap` (3706).** `vk_eng_flush_async` (3729)
   submits the open batch `B_f` without waiting and switches to the next set,
   waiting on that set's fence only because the walker is about to rewrite its
   rings and reset its command buffer and descriptor pool. With two sets that
   set is `A_f` (section 1). **This is the 11%.** `VK_EB_STATS=1` reports it
   under `present` (and, after this change, the 512th-draw swap under `max`;
   before, both were counted under `present`).
2. **Previous present's copy, `vk_present_complete_locked` (2104)**, called
   from `vk_present_record_begin` (2121) because the present's command buffer,
   fence and `readback` are single objects. It waits on `P_{f-1}`. With two
   sets this is already done: `A_f` starts with a full barrier
   (`present_order`, `vk_begin` 629) so it ran after `P_{f-1}`, and the walker
   has just waited for `A_f`. `VK_EB_STATS`: `present completion`. Then, on
   the CPU, `vk_invalidate` (2108), the centre pixel (2111) and the optional
   PPM dump (2113).
3. **The window, `vk_present_window` (2114), only with
   `PS3RECOMP_VK_WINDOW=1`:**
   - `vkAcquireNextImageKHR` (1978). Under FIFO it blocks once every
     swapchain image is queued for display, until a vblank frees one: the
     walker is the presenting thread, so a walker faster than the display is
     paced by it.
   - The blit's fence, waited straight after its submit (2036). The blit
     begins with a full barrier on the offscreen target, so it runs after
     everything already in the queue -- `B_f` included -- and this wait is a
     **full GPU drain every frame**. It defeats `VK_FRAME_PIPELINE` entirely
     when windowed.
   - `vkQueuePresentKHR` (2043); some WSI implementations block here under
     FIFO as well.
   `VK_EB_STATS`: `window` (new).
4. **The present's own copy `P_f`, only when the present comes from a flip
   method in the command stream** (0xE920/0xE924/0xE944 →
   `sink_flip`, rsx_draw_engine.c:3483). That path does not go through
   `present_begin`, so `present_split` is 0 and `vk_present_record_end`
   completes the present at once (2172): it waits for `P_f`, which runs after
   all of frame `f` -- another full drain. Only the host's
   `rsx_vulkan_backend_present` defers it (4350). Whether a title presents
   this way depends on how it flips; a large `present completion` in
   `VK_EB_STATS` on a headless run says it does.

Not waited on at a present: staging and ring reuse other than the set's own
rings (below), and any readback of labels or semaphores (section 3).

### Waits that defeat the ring whatever its depth

Every flush other than `vk_eng_flush_async` is `vk_eng_flush` (3799) →
`vk_submit_and_wait` (642), which waits for its own submission **and every
set in flight** (`vk_ebs_wait_other`, 3717). That is what keeps the rest of the
backend on its "everything earlier has completed" invariant, and it is
triggered by: a full ring or descriptor pool (`ring`, 3979 for a draw, 3784 for a texture upload), an
unbatched draw (`unbatched`), any object destroy (`destroy`: `vk_destroy_image`
493, `vk_destroy_buffer` 557, `vk_eng_drop_framebuffers` 2988,
`vk_eng_pipeline_release` 3198), the engine's own submit point (`engine`,
`vk_eng_submit_and_wait` 2432), and any `vk_begin` while a batch is open
(`begin`: texture create, target create, readback). Texture and target
creation also submit and wait on their own (`own submits`). A frame with a
texture-cache miss therefore drains the GPU at the miss regardless of
`VK_BATCH_SETS`; those columns say how often.

## 3. Every CPU read of a GPU result, and what orders it

| What the CPU reads | Where | Ordered by |
|--------------------|-------|------------|
| Presented frame: centre pixel, PPM dump | `vk_present_complete_locked` (2100-2113) from `s_vk.readback` | `present_fence` of the same present, waited at 2104 before the read. `P` starts with a full barrier (2127) and is submitted after the frame's last batch, so the copy sees the whole frame. Unchanged by any option here; `VK_FLIP_DEFER` only moves *when* 2104 runs. |
| A colour surface written back to guest memory (guest reference: CPU read of a render target, 2D transfer source) | rsx_draw_engine.c:891-892, 2823-2824 → `vk_eng_readback` (2591) | `submit_and_wait` first (`vk_eng_submit_and_wait` 2432: sync flush + `vk_ebs_wait_other`, all sets), then the readback's own `vk_begin` + `vk_submit_and_wait` (all sets again). |
| A depth buffer written back to guest memory | rsx_draw_engine.c:1564-1565, 2866-2867 → `vk_eng_depth_readback` (2882) | Same as above. |
| `rsx_draw_engine_readback_center` (tests, host checks) | rsx_draw_engine.c:3660 → `vk_eng_readback` | Same as above. |
| Labels / back-end semaphore release (NV4097 0x1D70/0x1D74) | rsx_commands.c:526-556 | **Not a GPU result.** The walker writes the value with `vm_write32` when it processes the method, before the draws ahead of it have even been submitted under `VK_DRAW_BATCH=1`. |
| NV406E semaphore release / acquire | cellGcmSys.c:2526 | Same: written and compared on the CPU at walk time. |
| `NV406E_SET_REFERENCE` (ref register) | rsx_commands.c:964, cellGcmSys.c:2081 | Same. |
| `NV4097_GET_REPORT` (zpass, timestamps) | cellGcmSys.c:2087 | Same: a fixed "visible" value and the host clock; no GPU query exists. |
| `NV4097_NOTIFY` | rsx_commands.c:503 | Same. |
| Flip status / flip handler | cellGcmSys.c:906, 3320 | Set by the vblank / flip tick on the host, not by GPU completion. |

So nothing the guest polls is produced by the GPU: those values are exactly as
ordered as they were with two sets (or one). The only GPU results the CPU reads
are the present's readback and the guest-reference readbacks, and both are
behind fences that cover every set.

GPU-only resources shared between batches -- the depth snapshot bounce buffer
(`s_snap_st`, 2845), the colour snapshot pool, textures, surfaces -- are
ordered on the GPU: every batched operation starts with a full memory barrier
(`vk_cmd_after_everything`), whose first scope reaches back across earlier
submissions on the queue, and freeing any of them waits for all sets. The
host-written per-batch memory (vertex, index, uniform and upload-staging
rings, the descriptor pool) belongs to one set and is rewritten only after
that set's fence (3706). The unbatched staging buffers (`s_up_stage`,
`s_rb_stage`) and the shared UBO / descriptor sets are used only by
synchronous submissions.

## 4. What changed (all opt-in, defaults unchanged)

### `VK_BATCH_SETS=2|3|4` (with `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`)

The two batch sets become a ring of N (`vk_eb_on` 3650, `vk_ebs_swap` 3696).
A swap still waits only on the set it is about to reuse; every synchronous
flush and destroy waits for all of them (`vk_ebs_wait_other` 3717), and
`vk_eb_release` frees all but the current one. Default 2: the same code path
and behaviour as before.

With 2 batches a frame:

| Sets | 512th draw waits on | Present waits on | Present's `P_{f-1}` wait |
|------|---------------------|------------------|--------------------------|
| 2 | `B_{f-1}` | **`A_f` (this frame)** | already done |
| 3 | `A_{f-1}` | `B_{f-1}` | the copy only (queued right after `B_{f-1}`) |
| 4 | `B_{f-2}` | `A_{f-1}` | `P_{f-1}`, so `B_{f-1}` as well: in effect the same as 3 |

It is a ring of batches rather than of frames because a frame's batch count
varies; the frame bound comes from the present's single `readback`, command
buffer and fence (section 2, wait 2): the present always waits for the previous
present, so the GPU can never be more than about one frame behind the walker.
That caps the useful depth at 3 for a two-batch frame; 4 only helps frames of
three or more batches. Going beyond one frame in flight would need the
present's objects multiplied too, and would show the window and the dump a
frame later still -- not done.

Cost: each set is 176 MiB of host-visible, coherent memory (64 vertex + 16
index + 32 uniform + 64 staging, 3603-3606) plus a descriptor pool. If a set
cannot be allocated the ring stays at the sets it got (logged).

### `VK_FLIP_DEFER=1`

A present made by a flip method in the command stream leaves its second half
(wait on `P_f`, readback, dump, window) to the next present or to
`present_finish`, as the host's present already does under
`VK_FRAME_PIPELINE=1` (`vk_flip_defer` 2137, used at 2172). Engine path only;
the fixed-function path draws into the target itself and still completes at
once. Removes wait 4 of section 2.

### `VK_WINDOW_ASYNC=1` (windowed only)

The window blit gets its own command buffer and is waited on before the next
blit instead of right after its submit (`vk_window_async` 1959, 1973, 2036).
That one wait guards everything the old one did: the blit's command buffer and
`sem_acquire` are reused only after it. The blit still runs after the frame on
the GPU (its barrier is unchanged); only the walker stops waiting for it.
Removes the full drain of section 2, wait 3.

### `VK_PRESENT_MODE=fifo|mailbox|immediate|relaxed` (windowed only)

`vk_pick_present_mode` (1830), used at swapchain creation (1916); a mode the
surface lacks falls back to FIFO (logged). Does the mode matter here? Only
windowed: headless runs have no swapchain. Windowed under FIFO,
`vkAcquireNextImageKHR` blocks the walker -- and with it the FIFO drain that
writes the labels the guest waits on -- whenever it presents faster than the
display; MAILBOX and IMMEDIATE never wait for a vblank (MAILBOX drops the
queued frame, IMMEDIATE tears), RELAXED tears only when late. The frame clock
already paces the guest at 60 Hz, so FIFO is invisible when the walker keeps
up and costs a vblank wait when it bunches presents. The shutdown hold loop
sleeps 16 ms a frame outside FIFO so it does not spin.

### `VK_EB_STATS=1`

The swap wait is now counted under the reason of the flush that triggered it
(`present`, `max`, or `other` for the rare swap from neither), the clear and
upload that fill a batch count as `max`, and a new `window` column times the
acquire, the blit wait and `vkQueuePresentKHR`. Comparing with numbers taken
before this change: old `present` = new `present` + `max`.

## 5. What was verified here, and what was not

No GPU was available. On Mesa lavapipe (CPU Vulkan), under the Khronos
validation layer, with both the fixed-function and the engine pass of
`.github/workflows/linux-vulkan.yml`:

- defaults, and `VK_FRAME_PIPELINE=1` with `VK_BATCH_SETS=2,3,4`, each also
  with `VK_DRAW_BATCH_MAX=1` and `=2` so the ring wraps every few operations:
  every scene passes, no validation message; synchronization validation
  (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`) on
  `--draw --rtt --depthtex --feedback --tex-io` with 4 sets and 1-draw
  batches reports no hazard;
- windowed under Xvfb: FIFO, MAILBOX, IMMEDIATE, RELAXED, `VK_WINDOW_ASYNC=1`
  and all options together, including the shutdown hold loop: no validation
  message, correct pixels;
- `VK_FLIP_DEFER=1` with a 0xE944 flip injected into the host's command
  stream (a local change, not committed): correct presented pixel, no
  validation message.

lavapipe says nothing about timing. It cannot show whether the wait moves, and
host-memory hazards on the rings (the CPU writing a set the GPU still reads)
are not something the validation layer tracks; the argument for those is the
fence discipline in section 3, which the existing two-set code already relies
on.

## 6. What to measure on the GPU

Run each with `VK_EB_STATS=1` on the same scene and compare the 2 s lines
(`gpu wait ms/2s: ...`), walker CPU time in `vk_eng_flush_async`, frame time,
and GPU busy:

| Run | Environment on top of `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 VK_EB_STATS=1` | Expect |
|-----|-----------------------------------------|--------|
| base | -- | `present` ≈ today's 11% |
| A | `VK_BATCH_SETS=3` | `present` near 0; `max` unchanged or lower |
| B | `VK_BATCH_SETS=4` | like A for a two-batch frame: part of the wait moves from `present` to `present completion`; better only for frames of three or more batches |
| C | `VK_DRAW_BATCH_MAX=1024` (2 sets) | one batch a frame: the present waits on the previous frame's batch; check `ring` does not start firing (rings are sized per batch) |
| D | A + `VK_FLIP_DEFER=1` | `present completion` drops if the title flips through the command stream; no change otherwise |
| E (window) | A + `VK_WINDOW_ASYNC=1` | `window` drops from a full frame's GPU time to the acquire |
| F (window) | E + `VK_PRESENT_MODE=mailbox` (then `immediate`) | `window` near 0; watch for tearing with `immediate` |

Also check:

- Rendering is identical frame for frame (`PS3RECOMP_VK_DUMP` with
  `PS3RECOMP_VK_DUMP_EVERY`) between base and A/B: a difference means a
  host-written resource is reused too early, which no test here can catch.
- `ring`, `unbatched`, `destroy`, `begin`, `own submits`: if these are large
  the walker drains the GPU there regardless of the ring depth, and they are
  the next thing to fix, not the present.
- Host memory: +176 MiB per extra set; on a small-BAR or integrated system
  watch for the "no memory for batch set" log line.
- Input-to-display latency: A lets the GPU trail the walker by up to about a
  frame (the same bound `VK_FRAME_PIPELINE=1` intends); the window shows the
  frame one present later, as it already does under `VK_FRAME_PIPELINE=1`.
