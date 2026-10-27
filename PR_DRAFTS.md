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

# PR 4: sys_memory: memory containers as the kernel defines them

Branch: `sys-memory-containers` (one commit on `4e8f3ca`, `runtime/syscalls/sys_memory.{c,h}`)

Three gaps, all hit by MLB The Show 16 (BCUS01089) when it loads a match:
- `sys_memory_container_get_size` took the container id first and wrote `{total, used}`. The kernel's signature is
  `(sys_memory_info_t* info, u32 cid)` and the struct is `{total, available}`.
- `SYS_MEMORY_CONTAINER_ID_INVALID` (0xFFFFFFFF) is the process's default container. Titles carve containers out of
  it: get its size, destroy it, recreate it smaller, create their own. ps3recomp answered ESRCH, so the carve gave
  back container 0.
- `sys_memory_allocate_from_container` (syscall 350) wasn't registered.

MLB 16 builds its "SharedMemContAnim"/"SharedMemContMovie" buffer pools that way. The pool never got memory, and the
first match load called a virtual method on a NULL buffer from it. This answers the default container from the same
numbers as `sys_memory_get_user_memory_size`, writes the available size, and adds syscall 350 through the same
allocator as `sys_memory_allocate` (charged to the container, credited back by `sys_memory_free`).

Tested: MLB 16 now allocates its pools (25 MB and 7 MB from carved containers) and gets past that point of the match
load.

# PR 5: cellVideoOut: report the 3D resolutions unavailable

Branch: `cellvideoout-no-3d` (one commit on `4e8f3ca`, `libs/video/cellVideoOut.c`)

`cellVideoOutGetResolutionAvailability` answered 1 for every resolution id, including the stereoscopic 3D and
dual-view ones (0x80 and up; 0x81 is 720p frame packing). That tells a title a 3D TV is attached. MLB The Show 16
then asks at boot whether to turn 3D on, and on yes renders every frame twice, stacked over-under. This reports ids
from 0x80 up as unavailable.

# PR 6: cellFs: implement cellFsFtruncate on the cellFsOpen fd table

Branch: `cellfs-ftruncate` (one commit on `4e8f3ca`, `runtime/ppu/ppu_fs.cpp`)

`cellFsFtruncate` (0x0E2939E5) resolved to `libs/filesystem/cellFs.c`, which looks the fd up in that file's own
table. `cellFsOpen` in `runtime/ppu/ppu_fs.cpp` never fills that table, so every call returned EBADF and the file
kept its old size. This adds `cellFsFtruncate` next to the other fd calls in `ppu_fs.cpp`, through the same
`fd_file()` lookup, and registers it.

MLB The Show 16 (BCUS01089) sizes its head/face cache (314,584,064 bytes) and arena logo cache (9 MB) this way
before a match loads. Tested: both files now have the requested size and the call returns CELL_OK; the match load
gets past the starting lineups.

# PR 7: cellSpurs: carry argTaskset through the taskset attribute

Branch: `spurs-taskset-args` (one commit on `4e8f3ca`, `libs/spurs/cellSpurs.c`)

`_cellSpursTasksetAttributeInitialize` zeroed the attribute without storing `argTaskset`, and
`cellSpursCreateTasksetWithAttribute` created the taskset with args 0. The taskset argument reaches every task in
r4. MLB The Show 16's Bink task DMAs its setup block from it, so it got its event flag address as 0, and
`cellSpursEventFlagWait` failed straight away with 0x80410911 (null pointer). The task retries in a loop, which kept
one host core at 99% for the whole run.

This stores `argTaskset` in the (HLE-private) attribute and passes it to `spurs_create_taskset`, which already
writes it to the guest taskset. Priority and max contention are still not carried.

Tested: the Bink task now starts with r4 = {args 0x01476E80, spurs 0x01243E00} and sleeps in its wait.

# PR 8: cellSpurs: read the workload attribute's maxContention from the guest stack

Branch: `spurs-workload-maxcontention` (one commit on `4e8f3ca`, `libs/spurs/cellSpurs.c`, `runtime/ppu/ppu_sysprx.cpp`)

`_cellSpursWorkloadAttributeInitialize` takes nine arguments. The ninth, `maxContention`, is passed on the caller's
stack, beyond the generic 8-GPR adapter, so the HLE stored 1 and every workload added with an attribute ran on one
SPU at a time. This registers a context handler (same pattern as `sceNpScore` and `cellSaveData`) that runs the
existing initializer and stores the stack argument (`r1 + 112`).

MLB The Show 16 (BCUS01089) asks for 5 on its mesh job lists and 4 on its DXT list. Tested: `AddWorkloadWA` now logs
those values.

Not included (separate discussion): the scheduler consumes a workload's ready count per dispatch, where the SDK
describes it as a level, and it probes idle workloads with ready count 0. Both contributed to a match-load deadlock
in MLB 16; they are opt-in switches on our working branch until tested on other titles.
