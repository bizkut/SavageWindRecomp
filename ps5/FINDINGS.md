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

## 2026-10-27 — S4 on console 10.0.1.41 (run3-41): 4K at 119.88 Hz

From here on the test console is **10.0.1.41**. Same eboot as run2. All S4 checks pass (0 failed): VideoOut offers
3840x2160 at 119880 and 59940 mHz; the WSI logs `119.88 Hz selected with title VRR metadata` (attribute3 0x40040
taken as authoritative); a 3-image 4K FIFO swapchain presented 720 frames at a settled **119.88 Hz (8.342 ms/frame),
worst interval 8.46 ms**. The .39 refusal (run2) was that console's output setup, not the title.
The console's AvControl confirms VRR: `HFR:o ... VRR:TypeA`, `119.88Hz request is converted to VRR 119.88Hz`,
`VRR(peg:120 range:48 - 120)` while the title runs, back to `peg:60 range:48 - 60` after it exits (klog lines
284–359). TV-side confirmation still to be read off the screen by the user.

## 2026-10-27 — S5 shaderc on the console (.41, ps5/evidence/s5-shaderc run4)

`ps5/tools/build-shaderc.sh` cross-builds the desktop pin (shaderc v2026.3 + its DEPS glslang/SPIRV-Tools) with the
SDK's prospero.cmake into one 27 MB static `libshaderc_combined.a`. Probe `ps5/probe/s5_shaderc.c`, with
`runtime/nvn/vulkan/spirv.c`'s options (Vulkan 1.3 target, optimization zero):
- vertex, fragment (scalar layout, 8/16-bit storage, demote) and compute shaders compile; a bad shader is rejected
  with glslang's message;
- all three SPIR-V files are **byte-identical** to the same shaderc pin built on the Linux host and pass `spirv-val`
  (vulkan1.3);
- first compile 167 ms (one-time glslang initialisation), then ~0.9 ms per fragment shader;
- the console-compiled compute shader goes through RADV (ACO pipeline 1.0 ms), dispatches and gives 256/256
  correct results.

Run1 failed on a GLSL error in the probe's own shader (host glslangValidator gave the same error); run3's PC-side
recheck exposed a use-after-free in the probe's verdict (read after `shaderc_result_release`), fixed before run4.

**M0 status: S1–S5 go.** S6 (CPU headroom with real generated code) remains, and needs the game build.

## 2026-10-27 — Storage under /data/savagewind, unelevated title (.41, ps5/evidence/io-probe run1)

Probe `ps5/probe/io_probe.c`. With ShadowMountPlus on .41 the title, without any elevation, creates and writes
`/data/savagewind` (the directory now exists there, 0777).
- **Writes are throttled as in ps5-wow:** 1 MiB sequential writes run at 20 MiB/s (slowest 65 ms) until ~2 GiB, then
  12 → 5 → 2 MiB/s with single writes taking up to 1.9 s; 3 GiB took ~9 min. 64 KiB create/write/close files, done
  after that: 20.7 ms each, slowest 0.7 s.
- **Reads (3 GiB file):** sequential 1 MiB 133 MiB/s buffered, 111 MiB/s O_DIRECT; random 64 KiB 41 MiB/s
  (1.5–1.6 ms per read), 4 threads 124–171 MiB/s; random 4 KiB 0.6–0.8 ms per read. Measured with the write budget
  already exhausted; whether reads share it is not separated yet.
- For comparison, PS5_MihawkOS measured a **Lapy-elevated** title writing `/data` at 143 MiB/s with no throttling over
  1.2 GiB, and again after LoadExec (`PS5_MihawkOS/docs/SELF_INSTALL.md` phase 0, row 1). Reads were not measured there.

What it means for Savage Wind: `romfs.bin` arrives over FTP (not title writes), so the write budget only touches
saves, settings and shader/pipeline caches. Reads are the open question: 1.5 ms per random 64 KiB read is slow for
streaming; next step is the same probe Lapy-elevated, and a read-only run with an unspent write budget.

## 2026-10-27 — Root payload and storage broker on .41

This supersedes the preceding entry's proposed Lapy follow-up.

**Direct root-payload baseline** (`ps5/evidence/io-probe/payload-direct.txt` and `payload-direct.klog`): the same
3 GiB workload as the unelevated title, but as an elfldr payload. Sequential 1 MiB writes stay at 35–36 MiB/s
through all 3,072 MiB (no 20 → 2 MiB/s title-budget collapse); sequential reads are 517 MiB/s buffered / 552 MiB/s
O_DIRECT; random 64 KiB 94/81 MiB/s, four readers 197/302 MiB/s; 4 KiB 6.0/10.3 MiB/s. Two hundred
create/write/close 64 KiB files take 11.0 ms each, worst 18 ms. Root bypasses the cumulative title budget but does
not make pwrite free.

**Savage Wind broker v2** (`ps5/storagebroker`, port 9142; console evidence
`ps5/evidence/io-probe/broker-run3-storagebroker-bench.txt` and `broker-run3.klog`): a normal PPSA90212 title sent
synchronous 1 MiB writes over loopback to the root payload. It held **27 MiB/s across all 3 GiB**, with no collapse
after 2 GiB; slowest request 66 ms. Two hundred durable atomic 64 KiB replacements (same-directory temp + file
fsync + rename + directory fsync) average 15.5 ms, worst 138 ms. Diagnostic broker reads are 548 MiB/s sequential,
79 MiB/s random 64 KiB, 5.0 MiB/s random 4 KiB, 88 MiB/s with four connections; these open/close per request and
are not the RomFS design. The runtime keeps direct persistent-fd reads.

Console path checks reject parent traversal, absolute paths, dot/empty components and broker-owned names. PS5 title
and payload syscalls refuse creating symlink/hard-link fixtures (EPERM/EOPNOTSUPP), so those guards were exercised
by `make -C ps5/storagebroker host-test`: final and intermediate symlinks and multiply linked files are rejected,
and create/pwrite/pread/stat/atomic-replace/unlink all pass (0 failures).

The broker uses a unique magic/token and **port 9142**, never WoW's 9141, confines every operation below a
pre-opened `/data/savagewind` fd via no-follow component walking, and gives FTP-compatible modes. ShadowMountPlus
remains a prerequisite for direct RomFS reads and token access; the broker is the write path, not a replacement
filesystem.

A Lapy resident-daemon experiment was abandoned in favor of this payload broker. On firmware 5.50 its daemon
preflight passed, but the standard client falsely reported success because ShadowMountPlus already exposed `/data`;
the daemon itself refused that request with `cred_refs=3`/EBUSY, the exact race documented by MihawkOS. That
test-only resident daemon remains in memory on .41 until the next console reboot; it has no boot hook and no retained
files.

## 2026-10-27 — NSP inputs, keys, nstool2 and ProsperoEden

User inputs under `/home/bizkut/Downloads/Wind`: base `01007EF00011E000` v0 (Program NCA 14,474,805,248 bytes) and
update `01007EF00011E800` v1114112 / 1.9.0 (Program NCA 988,376,064 bytes), each with a generic AES-CBC ticket.
No key bytes were printed or retained in evidence.

With the supplied firmware-19 `prod.keys` and embedded base ticket, both nstool pins fully decrypt the base Program
NCA and enumerate its ExeFS (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`) and RomFS. The update NCA header also
decrypts and identifies SDK 21.4.1/key generation 3, but its partitions report `AES-CTR Key was not determined`.
The update ticket's Rights ID matches the NCA; the supplied `title.keys` has no mapping for it. Firmware-19
`prod.keys` is new enough and carries the named generation-3 derivation slots, so the remaining prerequisite is a
correct update title-key mapping or a fresh update NSP/ticket from the user's console. Full temporary NCA/ticket
copies were deleted after the check.

`../nstool2` at `7e0a49b61a` and all pinned submodules cross-compile successfully for the PS5 through
`ps5/tools/build-nstool2.sh`. The one-shot importer orchestration/extraction cannot be qualified until the update
title key is corrected.

ProsperoEden is not a bulk-extractor reference: its local metadata bridge opens NSP/XCI through upstream Eden and
extracts only control RomFS title/icon data. It has no complete ExeFS/RomFS export or base/update output path.
Current Savage Wind reads the merged `romfs/` directory directly (`runtime/romfs_image.c`); the importer therefore
outputs `/data/savagewind/game/romfs/`, not legacy `romfs.bin`.

### Correction: both embedded tickets are required for patch merging

The preceding missing-update-key diagnosis is superseded. The failed `--basenca` command loaded only the update
ticket; nstool also opens the base NCA, which needs its own ticket. With both `--tik base.tik --tik update.tik`
and the supplied `prod.keys`, nstool2 successfully enumerates the merged update ExeFS and RomFS, including
`main.npdm` and `System/Version.txt`, without AES-CTR-key or base-RomFS errors. No replacement keys/NSPs are needed.
The absence of a matching `title.keys` entry is harmless when both embedded tickets are loaded.

## 2026-10-27 — App-start permissions and runtime storage, broker v4

`runtime/paths.c::rt_paths_init` now requests root-broker permission repair on every PS5 app startup, before
changing to `/data/savagewind`. Ownership is preserved, real directories become 0777 and regular files 0666.
Traversal is bounded and stays on the root filesystem device; symlinks, hard links and special files are not repaired.
Broker startup repairs root accessibility before publishing its token, including when the root began at 0700.

Host behavior/security tests pass for v4 repair, exclusive creation, append, truncate, flush, rename (including
directories), bounded directory listing and removal. Console `.41` smoke evidence:
`ps5/evidence/io-probe/storage-runtime3.klog`. A root payload created 0700 parent/nested dirs and a 0600 file;
app startup reported `scanned=11 changed=3 skipped=0 failed=0`, then verified 0777/0777/0666. An outside-root
sentinel remained 0600 (verified through FTP) and all fixtures were removed afterwards. The same runtime backend
passed random-offset save writes, truncate, fsync, rename/delete, settings replacement and an atomic cache replacement
larger than the 16 MiB protocol limit.

The runtime save/settings/Vulkan-cache callsites now select this backend only on PS5; RomFS/DLC reads remain direct.
This is storage-component qualification, not a boot of the full PS5 game runtime: PS5 platform/input/audio/renderer
integration is still pending, as is native log redirection.
