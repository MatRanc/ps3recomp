# PR drafts (not opened yet)

Text for pull requests to sp00nznet/ps3recomp, kept here until they're posted. Each PR's branch is in this fork.

# PR 1: Template: use the Vulkan backend on Linux when it's built

Branch: `template-vulkan-backend` (one commit on `4e8f3ca`, `templates/project/main.cpp`, +8 lines)

`templates/project/main.cpp` chooses D3D12 on Windows, Metal on macOS and the null backend everywhere else, so a
port configured with `-DPS3RECOMP_RSX_VULKAN=ON` still runs headless. `runtime/host/host_posix.c` already picks
Vulkan under `PS3RECOMP_RSX_VULKAN`; this adds the same case to the template, using
`rsx_vulkan_backend_init/present/pump_messages` from `rsx_vulkan_backend.h`.

Tested on Fedora 44 (RTX 3060 Ti, Wayland) with an MLB The Show 16 port: before, the null backend; after, the
Vulkan window opens and the title's first screens draw with `PS3RECOMP_VK_GUEST_PROGRAMS=1`.

# PR 2: cellGcmSys: opt-in dedicated thread for GCM handler delivery

Branch: `gcm-handler-thread` (one commit on `4e8f3ca`, `libs/video/cellGcmSys.c`)

`ppu_gcm_pump` runs the guest's vblank/flip/user handlers at HLE boundaries on whatever guest thread made the call,
and `ppu_guest_call` gives the callback that thread's id. When that thread already holds a lock the handler takes,
the handler re-enters it instead of waiting.

MLB The Show 16 (BCUS01089) hangs on this at random during its boot screens. Its lock wrappers record the owner's
thread id beside the lwmutex and unlock only when the caller matches. Its flip handler, delivered inside its
DisplayFlip thread while DisplayFlip held that lock, re-entered it and cleared the owner record on unlock; DisplayFlip's
own unlock was then skipped, the lwmutex stayed owned (recursion 1) and two threads blocked on it for good. The
handler stuck inside the pump also stopped all further vblank/flip delivery.

On the PS3 these handlers run on GCM's interrupt thread. With `GCM_HANDLER_THREAD=1`, `ppu_gcm_pump` delivers only on
a dedicated host thread (no guest context, so `ppu_guest_call` gives it the host-thread sentinel id); calls from
anywhere else just start that thread and return. Default off, so existing ports are unchanged.

Tested: MLB 16 hung in most runs without it; 3 of 3 runs got past the boot screens with it. Linux only so far; the
Windows path compiles the same calls (`CreateThread`, `Sleep`) but wasn't run.

Open question for the maintainers: the dedicated thread runs handlers concurrently with guest threads, as on hardware.
The file's comments suggest serializing with guest execution was deliberate for some titles, so this stays opt-in.

# PR 3: cellFs: implement cellFsAioWrite

Branch: `cellfs-aio-write` (one commit on `4e8f3ca`, `runtime/ppu/ppu_fs.cpp`)

`cellFsAioWrite` (NID `0x4CEF342E`) was unregistered, so it took the unresolved-NID path: CELL_OK, nothing written,
and the completion callback never ran. MLB The Show 16 (BCUS01089) copies files into its `/dev_hdd1` cache in 64 KB
chunks this way and polls a status word its callback sets, so its boot parked on the first screen for good.

This adds it next to `cellFsAioRead`, with the same contract and the same synchronous completion: an absolute write
at `offset` under the stream lock, the fd's position restored afterwards, then the callback
`(aio, error, id, size)`. It reports CELL_FS_EIO on a short write. `PS3_FSLOG` logs it like the read.

Tested: MLB 16 now makes about 500 of these writes during boot and goes on to its first menu (the experience-level
screen) instead of stopping on the autosave notice.
