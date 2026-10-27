# Savage Wind PS5: what was measured, and what it forces

Append-only; each entry names the run it comes from (`ps5/evidence/<step>/`). Console: PS5 at 10.0.1.39, title
PPSA90212, probe `ps5/probe/m0_probe.c`.

## 2026-10-27 — M0 probe (runs 3–11)

**S3, large code: works.** A 256 MiB `.text` block (`m0_bigtext.S`) in a 269 MB `eboot.bin` loads, and code at both
ends runs (run3). The generated game code can be one static link. Separately, PS5_vkQuake records that a title cannot
`dlopen`/`sceKernelLoadStartModule` its own modules (`PS5_vkQuake/tools/build.sh` comment, measured in PS5_Vulkan), so
the game code is linked into the eboot (plan D5).

**S1, fixed guest window: works through the kernel's own calls, not `mmap`.**
- Kernel pages are 16 KiB (`getpagesize`, run3).
- `mmap` of the 12.5 GiB window at `0x7000000000`, read-write or `PROT_NONE`, fixed: `ENOMEM` (run3). Anonymous
  mappings draw on the flexible budget: 448 MiB configured, 186 MiB free at probe time; no `PROT_NONE` region of even
  1 GiB is available (run4/5). Small fixed mappings anywhere in `0x100000000`–`0x7900000000` succeed (run4).
- `sceKernelReserveVirtualRange(0x7000000000, 12.5 GiB, MAP_FIXED)`: rc 0, at the exact address (run5).
- Backing inside it with direct memory (`sceKernelAllocateDirectMemory` + `sceKernelMapDirectMemory`, fixed):
  module image 256 MiB, HLE heap 64 MiB, guest heap 3 GiB, all at their `runtime/rt.h` addresses, written whole in
  6.5 ms (run5). Direct memory: 12,288 MiB total, 12,286 MiB free before the probe (shared with RADV later).
- `sceKernelMprotect` at 16 KiB and at 4 KiB inside the window: rc 0 (run5). Whether a 4 KiB request protects only
  4 KiB or rounds to the 16 KiB page is not yet measured.
- Zeroing 256 MiB (in-process reboot reset): 7.1 ms.

Forces: `runtime/platform/ps5.c` implements `plat_reserve` with `sceKernelReserveVirtualRange` and `plat_commit` with
direct memory mapped fixed; never anonymous `mmap` for guest memory. `PLAT_PAGE` stays 16 KiB.

**S2, guest stacks: works with runtime-owned, demand-mapped stacks.**
- `pthread_create` with a 256 MiB `stacksize`: `EAGAIN` (35); 128 MiB is accepted (run4). Anonymous `mmap` of a
  256 MiB stack: `ENOMEM`.
- 48 stacks of 256 MiB as reserved addresses only, top 1 MiB backed, passed with `pthread_attr_setstack`: all 48 threads
  ran on their given stacks. A SIGSEGV handler on an alternate signal stack maps 1 MiB of direct memory at the faulting
  address and returns; walking 8 MiB down each stack took exactly 384 such maps and every thread joined with the right
  result (run11). The first attempts (runs 6–10) reported 0 faults because the compiler treated stores outside the
  local object as undefined; the probe now writes through an opaque address.
- Fault recovery by `mprotect` from a SIGSEGV handler: works (every run).

Forces: guest threads get stacks from a reserved range, committed on fault in 1 MiB steps — the same lazy-commit model
`runtime/platform/windows.c` uses. The fault handler is shared with the guest window's watchpoints.

**S6, CPU:** dependent integer loop 0.866 ns/iteration with and without `__thread` (emulated TLS costs nothing
measurable here), dependent float loop 1.45 ns, `memcpy` 13.4–13.8 GB/s (runs 3–11). The Linux build host gives
1.10 ns / 2.9 ns / 3.6 GB/s, but it is a server, not the desktop Savage Wind's 60 fps was measured on; the meaningful
comparison is generated game code on a desktop Zen 3/4, still to do.

**Open:** every run's klog shows `SceSysCore.elf ... exited on signal 12` as the title closes through
`sceSystemServiceLoadExec("exit")`. PS5_vkQuake recorded SIGSYS on the title's own exit path; this one is a system
process. Not yet known whether it happens for other titles on this console.

## 2026-10-27 — S4 Vulkan on RADV (ps5/evidence/s4-vulkan, runs 1–2)

Probe `ps5/probe/s4_vulkan.c`, RADV archive pinned in `ps5/deps.lock` (PS5_Mesa f10c4bac9b + Forever patches 0001–0003).

**Go for the renderer.** "PlayStation 5 GPU (RADV NAVI21)", device API 1.4.354. Every feature `runtime/nvn/vulkan/device.c`
requires is present (1.3 dynamicRendering, synchronization2, demote/terminate, maintenance4; 1.2 timelineSemaphore,
shaderInt8, mirror-clamp, 8-bit access, scalarBlockLayout; 1.1 16-bit storage) and so is every copied core feature
(BC, anisotropy, int64, depth clamp, wide lines, clip distance, storage-image extended formats for PR #194's TAA).
Optional extensions present: custom border color, attribute divisor, external memory host, maintenance5, feedback loop
layout, dynamic rendering local read, graphics pipeline library, present id/wait, device fault. Absent:
`VK_KHR_unified_image_layouts` (device.c already treats it as optional). Formats: D32S8 yes, D24S8 no (device.c's depth
mapping must use D32S8), BC7 yes, ASTC no (BOTW's ASTC textures need the existing CPU/GPU decode path). Heaps: 4 GiB +
8 GiB device-local. Device creation with the full set succeeds.

**Display:** one VideoOut display, 3840x2160; display-plane surface, B8G8R8A8 UNORM/SRGB, FIFO/mailbox/immediate;
a 3-image 4K FIFO swapchain presents at 59.94 Hz steadily (16.683 ms, run1).

**120 Hz: not granted on this run.** The WSI read our param.json (attribute3 0x40040) and asked VideoOut:
`wsi/videoout: 119.88 Hz not supported (0); presenting at 59.94 Hz` (run2 stderr). `sceVideoOutIsOutputSupported`
returned 0, i.e. the console/TV output at the time offered no 120 Hz mode; this is the console's answer, not the
title's metadata (the WSI only asks because 0x40 is set). To check: console Screen and Video → 120 Hz output
Automatic, VRR on, TV on a 120 Hz-capable HDMI input, then re-run.
