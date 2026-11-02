# Window freeze and input lag under SDL2 + Vulkan on GNOME Wayland

This is a static review of the code. Nothing was built against the game or run.
Line numbers refer to `mlb16-work` at `2521008` unless marked *(patch)*, which
means branch `fix/window-pump`.

Symptoms (MLB The Show 16, Linux, GNOME Wayland, RTX 3060 Ti,
`PS3RECOMP_VK_WINDOW=1`):

1. The window runs for about 1 s and then freezes while the game keeps going:
   rumble continues and the flip counter advances. Moving the mouse unfreezes it.
   Headless runs never show this.
2. Controller presses are sometimes late or lost.

Hypothesis tested: both come from SDL event-pump starvation.

**Short verdict.** The hypothesis is only partly right.

- The pump can starve. A real path in the frame-clock loop can keep
  `SDL_PollEvent` from running for seconds (§1.3).
- Pump starvation alone cannot explain *"moving the mouse unfreezes it"*. Mouse
  motion does not make our pump run sooner. That signature points to the
  presentation side (the compositor, Xwayland, or the NVIDIA WSI), so the frozen
  frames would be stuck there. Our code turns that into a **silent** freeze: it
  drops the frame with no log while every counter keeps moving (§2).
- The controller is polled on the guest thread, not through the window pump, so
  input lag does not share this cause directly (§3).

The patch adds diagnostics and two opt-in knobs so these cases can be told apart
on the real machine (§4).

---

## 1. Who calls `SDL_PollEvent`, how often, and what blocks it

### 1.1 The thread

- `main()` starts `frame_clock` with `CreateThread`, which is pthreads off
  Windows (`templates/project/main.cpp:454`). The guest runs on the main thread
  through `run_guest` (`main.cpp:464`, `main.cpp:409`).
- `frame_clock` calls `rsx_backend_init` first (`main.cpp:207`). That runs
  `vk_window_open` (`libs/video/rsx_vulkan_backend.c:4603`), which calls
  `SDL_InitSubSystem(SDL_INIT_VIDEO)` and `SDL_CreateWindow`
  (`rsx_vulkan_backend.c:1832-1840`).
- So the **window, the SDL video subsystem and the event pump all live on the
  `frame_clock` thread**, not the process main thread. That is consistent: the
  pump runs on the thread that created the window. It is not the "wrong thread"
  in the usual sense.
- Caveat: SDL3, and therefore `sdl2-compat`, documents event and video calls as
  main-thread-only. On Linux X11 and Wayland this works in practice when it is
  always the same thread, but SDL does not promise it.
- The pump itself is `rsx_vulkan_backend_pump_messages`
  (`rsx_vulkan_backend.c:4847-4856`). It drains the whole SDL queue with
  `SDL_PollEvent` and keeps only `SDL_QUIT` and Esc (`rsx_vulkan_backend.c:4852-4855`).
- Its only callers are `frame_clock` (`main.cpp:256`) and the shutdown hold loop
  (`rsx_vulkan_backend.c:4801`).

### 1.2 How often

The pump runs **once per outer pass** of `frame_clock` (`main.cpp:219-275`). One
pass does the following:

1. `cellGcm_fifo_kick_wait(idle_ms)` (`main.cpp:220`). This waits up to 4 ms on
   the kick event (`libs/video/cellGcmSys.c:1866-1872`).
2. The catch-up loop (`main.cpp:224-242`). Each 16 ms tick it runs
   `cellGcmTickVBlank`, `cellGcmTickFlip`, a present if a flip is pending
   (`main.cpp:233`), and a FIFO drain (`main.cpp:239`). It runs up to 240 ticks.
3. One more present and drain (`main.cpp:249-254`), then **the pump**
   (`main.cpp:256`), then another possible present (`main.cpp:266-272`).

When the thread keeps up, the pump runs roughly every 4 ms plus one drain. That
is plenty.

### 1.3 What starves it: the catch-up loop

- `now` is read **once**, before the loop (`main.cpp:221`). The loop condition
  `now - next_tick >= 0` (`main.cpp:224`) never sees time pass inside the loop.
- On overflow, `next_tick = now` (`main.cpp:243`) resyncs to that **stale**
  `now`, so the next pass starts behind by the whole time the loop just took.

Let `c` be the average cost of one tick (drain plus the occasional present):

- If `c < 16 ms`, the lag converges and the pump runs every pass.
- If `c > 16 ms`, meaning the frame-clock thread is saturated (walker- or
  GPU-bound, i.e. the title runs below 60 Hz), each pass is longer than the
  last. It reaches the 240-tick cap and stays there. The pump then runs **once
  every 240 ticks, which is at least 240 × c ≥ 3.8 s**. During that time nothing
  calls `SDL_PollEvent`. Rumble (guest thread) and flips (the ticks) keep going.

Other things that can hold this thread, each delaying the pump by its own length:

| Blocker | Where | Bound |
|---|---|---|
| FIFO drain (`gcm_rsx_process_fifo_unlocked`), incl. walker parks on SPU-filled buffers and the FIFO lock | `cellGcmSys.c:2051-2066` | unbounded in principle |
| GPU fence wait when a present completes | `rsx_vulkan_backend.c:2293` (`vk_present_complete_locked`, `UINT64_MAX`) | the GPU frame |
| Swapchain-recreate barrier waiting for the present thread | `rsx_vulkan_backend.c:2071-2085`, called from `2133`, `2150` | as long as `vkQueuePresentKHR` takes |
| Synchronous present when `PS3RECOMP_VK_PRESENT_THREAD=0` | `rsx_vulkan_backend.c:2226-2233` | FIFO: until the compositor completes the present |
| Idle wait | `cellGcmSys.c:1871` | 4 ms |

With the present thread on (the default, `rsx_vulkan_backend.c:4754-4756`), the
frame-clock thread never calls `vkQueuePresentKHR` itself. A compositor that
stops completing presents therefore blocks the present thread
(`rsx_vulkan_backend.c:2032`), not the pump.

---

## 2. From "no events" to "no new image": what the code proves and what it doesn't

### Proven from the code: a frame can vanish silently

`vk_present_window` (`rsx_vulkan_backend.c:2124-2237`) runs from
`vk_present_complete_locked` *after* `s_vk.presented++`
(`rsx_vulkan_backend.c:2301-2303`). It returns **without a log or a counter** in
two cases:

- **No free present slot** (`rsx_vulkan_backend.c:2138-2139`). There are
  `n - 1` slots, so two for three images (`rsx_vulkan_backend.c:1979`). A slot
  stays busy until the present thread returns from `vkQueuePresentKHR`
  (`rsx_vulkan_backend.c:2032-2040`). If that call blocks (FIFO waiting for a
  frame callback, vblank or Xwayland present completion), the slots are used up
  after two frames and every later frame is dropped here.
- **`vkAcquireNextImageKHR(..., timeout 0, ...)` returns `VK_NOT_READY` or
  `VK_TIMEOUT`** (`rsx_vulkan_backend.c:2142-2147`). This happens when the
  presentation engine has not returned an image (no `wl_buffer.release`, no
  Present idle).

In both cases the slot is released and the frame is gone. There is no retry: the
next try is the next guest flip. So *"the game runs, the flip counter advances,
the window is frozen"* is exactly what the code does whenever the WSI or
compositor stops giving images back. The headless path never reaches
`vk_present_window` (`rsx_vulkan_backend.c:2126`), which explains why the
benchmark never shows it.

### Inferred, not verifiable here: why the WSI or compositor stops

What happens next depends on code outside this repo: the NVIDIA Vulkan WSI,
mutter, Xwayland, and SDL2 vs `sdl2-compat`. These are hypotheses, ranked by how
well they fit "mouse motion unfreezes it":

1. **The compositor only completes presents when something else repaints.**
   Moving the cursor makes mutter (or Xwayland) run a frame. That frame delivers
   the pending frame callbacks, buffer releases or Present completions, the
   blocked `vkQueuePresentKHR` returns, slots free up, and frames flow again.
   This matches the mouse signature, and it does not depend on our pump at all.
   - NVIDIA on GNOME Wayland/Xwayland has a history of this kind of stall around
     implicit and explicit sync, depending on driver, mutter and Xwayland
     versions.
   - If I remember right, Xwayland's present code uses a ~1 s fallback timer in
     flip mode (`TIMER_LEN_FLIP`). That could make the window crawl at about
     1 fps rather than freeze outright, and may fit the "~1 s" detail.
   - **Unverified.**
2. **The WSI only reads its Wayland queue when some other thread reads the
   display fd.** With a 0 timeout, a WSI may only dispatch events that are
   already queued, not read the socket. On Wayland the only other reader is
   `SDL_PumpEvents`, so starving the pump (§1.3) would stall image releases.
   Mouse motion still would not explain the recovery, unless the WSI is blocked
   in its own `poll` on that fd elsewhere. **Unverified, driver-specific.**
3. **Plain pump starvation (§1.3).** This is real in our code, but by itself it
   predicts a freeze that lasts until the next pump (≥ 3.8 s). It does not
   predict recovery on mouse motion. Mutter also pings clients, and SDL answers
   the ping in its pump; a gap of about 5 s or more should bring up GNOME's
   *"not responding"* dialog. If that dialog has ever appeared, the pump is
   starving.

### Which video driver is SDL using?

Check the log line `window: SDL video driver '...'` (`rsx_vulkan_backend.c:1859-1860`):

- Classic SDL2 prefers `x11`, which runs through **Xwayland** on GNOME.
- `sdl2-compat` (SDL2 API on SDL3, now the default on several distros; `cellPad.c:506`
  already allows for it) prefers native `wayland`.

The two have different stall mechanisms, so this one line decides which of the
above applies. The present mode is logged on the swapchain line
(`rsx_vulkan_backend.c:1981-1983`). It is MAILBOX when offered and FIFO
otherwise (`rsx_vulkan_backend.c:1913-1931`).

### `__GL_SYNC_TO_VBLANK=0`

Exported by `tools/run.sh:53`. Nothing in the repo reads it, and it is an NVIDIA
**OpenGL** variable. As far as I know the Vulkan driver does not consult it, so
it has no effect on this backend. Vulkan vsync is the swapchain present mode
above, overridable with `PS3RECOMP_VK_PRESENT_MODE=fifo|mailbox`
(`rsx_vulkan_backend.c:1927-1931`). It is harmless, but it does not do what the
script implies.

---

## 3. Is the input lag the same cause?

**Not directly.**

- The controller is read by `pad_poll_sdl2`, which calls
  `SDL_GameControllerUpdate()` itself (`libs/input/cellPad.c:565`). It runs on
  whichever guest thread calls the cellPad API, at most every 4 ms
  (`cellPad.c:682`, `cellPad.c:695-718`).
- That refreshes SDL's joystick state from the device without
  `SDL_PollEvent`. A starved window pump does not make pad state stale.
- Focus is not a factor either: `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`
  is set (`cellPad.c:645`).

Mechanisms that do fit "needs several presses":

- **Perception.** While the window is frozen, a press can be registered and acted
  on without any visible change, so the player presses again. If the menu
  *overshoots* when the picture unfreezes, this is the cause, and it is the same
  root cause as §2.
- **Level sampling at a low guest rate.** cellPad returns the button state at the
  moment the guest reads it. There is no latching of press edges between reads
  (`cellPad.c:695-718`, the packet logic at `cellPad.c:1097-1127`).
  - If MLB 16's input logic runs slowly, a tap shorter than the gap between two
    guest reads is never seen. The game may run slowly because it is
    walker-bound, or because the guest stalls on labels the saturated frame clock
    drains late (§1.3).
  - This is the most likely way a press can be truly lost. It ties to the same
    saturation as §1.3, but not to the SDL pump.
- **Hotplug events eaten by the window pump.** The window pump removes every SDL
  event (`rsx_vulkan_backend.c:4852`), including `SDL_CONTROLLERDEVICEADDED/REMOVED`
  that `pad_sdl_hotplug` looks for with `SDL_PeepEvents` (`cellPad.c:544`).
  Hotplug then falls back to the 1 s rescan (`cellPad.c:480`, `cellPad.c:558`).
  This only affects connect and disconnect latency, not presses.
- Two threads call into SDL's joystick layer: the window pump updates joysticks
  inside `SDL_PumpEvents`, and the guest calls `SDL_GameControllerUpdate`. SDL
  locks around this, so it is safe, but it is worth knowing.

---

## 4. Fix, A/B switches, and risks

Branch `fix/window-pump` (off `mlb16-work`). **Every behaviour change is opt-in.
With no new variables set, behaviour is unchanged**; the new code only reads
two environment variables once and caches them.

| Variable | What it does | Where *(patch)* |
|---|---|---|
| `PS3RECOMP_VK_PRESENT_STATS=1` | One line per second from the pump: pumps, **max pump gap**, blits submitted, presents completed by `vkQueuePresentKHR`, frames **dropped for no slot** and **dropped for not-ready**, longest `vkQueuePresentKHR`. Diagnostic only. | `rsx_vulkan_backend.c` "Window diagnostics" block before the presentation thread; counters in `vk_present_window`, `vk_present_thread_main`, the synchronous fallback, `rsx_vulkan_backend_pump_messages` |
| `PS3RECOMP_PUMP_MS=<n>` | Also pump inside the catch-up loop, at most every *n* ms, on the same (window) thread. Closes the §1.3 starvation. | `templates/project/main.cpp`, `frame_clock` |
| `PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS=<n>` | Acquire waits up to *n* ms (capped at 1000) instead of 0, so the WSI's own wait loop can dispatch its events before the frame is dropped. | `vk_present_window` / `vk_acquire_timeout_ns` |

`templates/project/main.cpp` is a template. If the MLB 16 project carries its
own copy of `main.cpp`, port the `PS3RECOMP_PUMP_MS` hunk there too. I could not
see that project.

### Suggested runs on the real machine

Run each with `PS3RECOMP_VK_PRESENT_STATS=1`. Leave the mouse still until it
freezes, then read the lines printed during the freeze.

1. Baseline. Note the `SDL video driver` and `mode` lines.
2. Read the stats lines during the freeze:
   - `max pump gap` in seconds: §1.3 starvation is present. Try
     `PS3RECOMP_PUMP_MS=8`.
   - `no-slot` climbs and `longest vkQueuePresentKHR` is hundreds of ms or more:
     the present thread is stuck in the WSI waiting on the compositor (§2,
     hypothesis 1). Try `PS3RECOMP_VK_PRESENT_MODE=mailbox` and then `fifo`, and
     the other SDL video driver (`SDL_VIDEODRIVER=x11` or `=wayland`).
   - `not-ready` climbs with `shown` at 0: the WSI holds every image. Try
     `PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS=4`, and `PS3RECOMP_PUMP_MS=8` (hypothesis 2).
   - `blits` and `shown` track the flip rate while the window is frozen: frames
     reach the compositor and it does not show them. This is purely on the
     compositor or driver side; only a different video driver, present mode, or
     a driver/mutter update will fix it.
3. Input: watch whether the menu overshoots when the picture unfreezes (it is the
   display, §3) or not (the press was really lost).

### Risks

- **`PS3RECOMP_PUMP_MS`.**
  - An empty `SDL_PollEvent` costs microseconds; at 8 ms that is negligible.
  - It still runs on the window thread, so it adds no new threading.
  - A window close is now acted on mid-burst. It quits the same way as before.
  - It does **not** help with one single long drain or walker park, because it
    only pumps between ticks. If `max pump gap` stays large with it on, the next
    step is a pump hook in the walker's park loop.
  - **Not done here:** the stale-`now` resync (`main.cpp:221`, `main.cpp:243`)
    keeps a saturated frame clock in 240-tick bursts. Re-reading the clock would
    change guest vblank/flip pacing, so it needs its own A/B test.
- **`PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS`.**
  - This blocks the frame-clock thread (the walker) for up to *n* ms per present,
    while `present_mu` is held (`vk_present_complete_locked`). That delays the
    FIFO drain and the label writes the guest waits on, so it can lower the guest
    frame rate.
  - Keep it small (2-8 ms).
  - If a driver ignores the timeout and blocks, the cap at 1000 ms is the worst
    case.
- **`SDL_VIDEODRIVER=x11` (or `=wayland`)** as a workaround.
  - It needs no code and is trivially reverted.
  - x11 goes through Xwayland, whose sync on NVIDIA depends on the driver
    (explicit sync arrived with the 555 series), and it loses native fractional
    scaling.
  - Whether it helps depends on which driver is in use today. Check the log line.
- **A separate pump thread** was rejected. SDL video and event calls must stay on
  the thread that owns the window. A pump from another thread would race the
  frame clock's pump and break SDL3's main-thread rule.

---

## What I could not verify without running it

- Which SDL video driver (x11/Xwayland or wayland) and present mode the user's
  run uses, and whether SDL is classic SDL2 or `sdl2-compat`.
- Whether frames are actually dropped at `rsx_vulkan_backend.c:2139` or `2144`
  during the freeze, or reach the compositor and are not shown. The new stats are
  there to answer this.
- How the NVIDIA WSI behaves on Wayland/Xwayland: whether a timeout-0 acquire
  reads the socket, and when `vkQueuePresentKHR` blocks. Also mutter's and
  Xwayland's repaint behaviour, including the ~1 s Xwayland timer, which is from
  memory.
- Whether MLB 16's frame clock is actually saturated (`c > 16 ms`), i.e. whether
  §1.3 happens in practice. `max pump gap` will show it.
- MLB 16's guest input-polling rate (§3).
- That the patch builds in the real project. Both changed files pass
  `-fsyntax-only` here (gcc/g++ against the system SDL2 and Vulkan headers,
  `-Wall -Wextra` clean for the backend). Nothing was linked or run, and the MLB
  16 project's own `main.cpp`, if it has one, was not visible.

No code was taken from RPCS3.
