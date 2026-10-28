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

## 2026-10-27 — NSP import payload and TV screen on .41 (ps5/evidence/import, runs 1–3)

Title `ps5/title/import_ui.c` (diagnostic build `-DIMPORT_UI_AUTOTEST`, which presses Cross itself and says so in
klog) with the embedded `savagewind-import.elf`; inputs on the console: base NSP + 1.9.0 update NSP + `prod.keys` in
`/data/savagewind/import/`.

- run1: the first send loop failed before elfldr had the ELF (`elfldr_read: No such process`). Rewritten after
  ps5-wow's `wine_launch` (MSG_DONTWAIT sends under poll, write side shut after the last byte).
- run2: ELF delivered (3,608,840 bytes); the payload ran as root (created `.import-lock`) but left silently through an
  unlogged early exit. Every exit path now logs to klog.
- run3: full path to RomFS extraction: checking_packages → checking_keys → awaiting_confirm → extracting_exefs →
  extracting_romfs; cancel by `cancel` file → `cancelled`, keys and packages untouched. **Measured extraction rate
  15 MiB/s** (6.0 of 14.1 GiB written after ~6.5 min; ~17 min for the whole RomFS), well under the 36 MiB/s raw
  root-payload write rate.

Decision (user, 2026-10-27): no RomFS extraction on the console. The runtime reads RomFS from the packages directly
(decrypt + update patch per read); ExeFS is extracted on the PC, where the game code is generated and linked.

## 2026-10-28 — 1.9.0 game code generated on the PC and compiled for the PS5

ExeFS extracted on the PC from base NSP + 1.9.0 update NSP (both tickets, `prod.keys`; package-library tool):
`main` f6f0b20b…, `main.npdm` 38a7484e…, `rtld` 49308064…, `sdk` 1d187f29…, `subsdk0` 767c35bc… (SHA-256 prefixes);
`main` build id matches `cd57b23fa4bbad65`. `tools/gen.py all build/exefs-1.9.0 generated-ps5 --hle
runtime/hle/hle_symbols.txt --simd c`: main 559 chunks, subsdk0 179, sdk 254; 83 s on one core, 2.2 GB peak RSS,
606 MB of C. `ps5/tools/build-gamecode.sh` compiles the 996 files with the Prospero clang 18 (desktop GENFLAGS,
`-march=x86-64-v3 -fPIC`) in 324 s on 24 threads into `libgamecode.a` (428 MB; **219.6 MB of code**, within the
256 MiB `.text` S3 loaded). Its only external needs are 40 runtime symbols (`rt_*`, `hle_longjmp`, `fnprof_*`) plus
`memset`/`setjmp`. Not linked or run yet: that needs the M2 runtime target; S6 (CPU headroom) uses it.

## 2026-10-28 — RomFS read directly from the packages (.41, ps5/evidence/import/package-probe-run1)

`ps5/importer/package_core.*`/`package_stream.cpp` (nstool2 + `nstool-streams.patch`) serve the merged 1.9.0 RomFS
from base NSP + update NSP: decrypt, update patch (BKTR) and IVFC data-block checks per read; at open, CNMT
selection, NCA identity/size and filesystem-header hashes (no whole-NCA hash per launch, no NCA signature check).
Host: all **41,062/41,062** RomFS files byte-identical to the stock-nstool extraction; ExeFS from the PC tool is the
same 5 files as that extraction.

Console probe `savagewind-package-probe.elf` (root elfldr payload, same core) on .41, reading the packages in
`/data/savagewind/import`: open **6.25 s**; 220/220 sampled files (20 largest + 200 seeded random) match the host
hashes; sequential **125 MiB/s**; random 64 KiB 0.69 ms / **90 MiB/s** (1 thread), 3.08 ms / 81 MiB/s (4 threads:
one mutex per package stream serialises reads). For comparison, plain files on the same console from a root payload:
517 MiB/s sequential, 94 MiB/s random 64 KiB; from the unelevated title: 133 MiB/s, 41 MiB/s. Not yet measured
inside the title process, and not yet with the game's read pattern (needs M2). The 6.25 s open and the
4-thread serialisation are the costs to reduce first.

## 2026-10-28 — Base XCI identified on the PC (host only; the XCI is not copied to the console)

Completed XCI (31,943,819,264 bytes, size stable) with the 1.9.0 update NSP through `savagewind-exefs`: CNMT
selection accepts it as base `01007EF00011E000` with matching update, and the 5 ExeFS files are SHA-256-identical to
the base NSP + update NSP result (so `main` carries the 1.9.0 build id). The XCI alone is rejected with
`wrong_version: Add the Breath of the Wild 1.9.0 update.` The XCI's RomFS was not read through the backend.

## 2026-10-28 — Package-backed ExeFS host check

The package library exposes read-only ExeFS streams from the merged Program NCA. The host package test opened `main`
and `main.npdm`, checked the NSO build ID and NPDM magic, compared an offset read against the original header, and
rejected a traversal name and a missing file. With the user's base and update NSPs, the same run matched
**41,062/41,062** RomFS files; package open was 1.117 s. This is a host check, not evidence that the title can open
the packages or reach game code on a console.

## 2026-10-28 — Package RomFS reads inside the unelevated title (.41, ps5/evidence/import/package-probe-title-run3)

Same probe as the payload run, built into a PPSA90212 title (`ps5/tools/build-package-probe-title.sh`). Run 1 and 2
failed: the title libc answers `stat("")` with EINVAL (not ENOENT), so `std::filesystem::exists` on the unset
`title.keys` path threw; fixed with non-throwing checks. Run 3: open **2.55 s**; 220/220 samples match; sequential
**68.7 MiB/s**; random 64 KiB 1.49 ms / **41.9 MiB/s** (1 thread), 5.88 ms / **42.5 MiB/s** (4 threads).

| Reads in the title | Sequential | Random 64 KiB, 1 thread | 4 threads |
|---|---:|---:|---:|
| Plain files (io-probe run1) | 133 MiB/s | 41 MiB/s | 124–171 MiB/s |
| Packages (this run) | 68.7 MiB/s | 41.9 MiB/s | 42.5 MiB/s |

Single-thread random reads equal plain files; the title's own I/O is the limit there. Sequential is half, and four
threads do not scale because one mutex serialises each package stream: per-thread streams are the next change.

## 2026-10-28 — M2 title linked on the host

`bash ps5/tools/build-game.sh` linked the user's 1.9.0 generated code into
`ps5/dist/game/PPSA90212/eboot.bin`; the link map is `build/ps5/game/title.map`. The current eboot is 252,573,183
bytes. This is a link result, not a console boot or proof that execution reaches NVN. The M2 build has no renderer,
input, audio or movie backend; unavailable generated-code hooks fail loudly if reached.

With the package-path fix and optional `title.keys` handling, the host package test again checked the ExeFS header and
NPDM plus **41,062/41,062** RomFS files. The desktop `runtime-objs` target compiled with the local host dependencies.

## 2026-10-28 — M2 run 1 and import audit

`ps5/evidence/m2/run1.klog` shows startup and guest reservation, then a SIGSEGV at the first `getcwd` call in
`rt_paths_init`. The SDK platform source documents that title libc's `getcwd` depends on `__getcwd`, unavailable
to titles; the title recipe now binds `getcwd` to the platform's directory-walking `ps5_getcwd` and uses static
libunwind for `_Unwind_Backtrace`. The rebuilt eboot is 252,575,519 bytes (SHA-256
`d1bcfa363f00a87d75bd2ca84be78c479cb6c706b1aebd4bd3f057a8efdf6627`); this rebuild has not been run here.

The title builder now checks imports against DT_NEEDED module stubs in converter provider order and the `libc.prx`
API manifest when that module is the provider. System `libSceLibcInternal.sprx` is distinct from `libc.prx` and was
present in run 1; its SDK stub is interface evidence, not a console test of every function's internal dependencies.
The rebuilt title has no unresolved provider names under this static check; three host tests cover the check.

## 2026-10-28 — M2 run 2 and working-directory independence

`ps5/evidence/m2/run2.klog` shows guest reservation and successful broker permission repair, then a data-root
initialisation failure. The old combined `stat`/`chdir` condition did not record which operation failed. The PS5
title now reports their separate results and errno values, and its normal data reads use absolute data-root paths;
broker writes remain root-relative. A host test exercised failed `chdir` with an accessible root and rejection of
an inaccessible root. The rebuilt eboot SHA-256 is
`75aea93dee27996766c0549f6b7cb06fe6fd44c788ddd7775bd680b017872e90`; this change needs a console run.

## 2026-10-28 — M2 run 3 and termination diagnostics

`ps5/evidence/m2/run3.klog` confirms `chdir` returns EPERM and the title proceeds with absolute data reads. It
then stops after package selection's `open-program-nca` marker without a recorded signal or diagnostic. The cause
is not established. The title's previous C++ allocation operator trapped on allocation failure without invoking
`new_handler` or throwing `bad_alloc`; this is fixed and host-tested, but is not claimed as the observed cause.

The next build adds direct-klog termination/exit instrumentation and heap, flexible/direct-memory and calling-thread
stack snapshots at individual NCA stages. The rebuilt eboot SHA-256 is
`3da8314636396c788a38279b7025cc035826dc1dc02fdd4fead572e71873d3f4`; it has not been console-tested here.

## 2026-10-28 — M2 run 4 and package-open bisect

`ps5/evidence/m2/run4.klog` reaches `nca-process`; the last snapshot reports a 16 MiB title heap, zero libc heap
fallbacks, 211,812,352 bytes of available flexible memory and 8,984 bytes used on the 2 MiB main stack. No installed
termination or allocation handler reported before the session stopped; this does not establish why it stopped.
The runtime's guest fault handler is installed only in the guest thread after package startup, so it was not installed
on this path.

Two host-built variants add per-stage NCA tracing: a package-probe title with the M2 allocator, diagnostics and guest
reservation (SHA-256 `c10b0182453a13d132a8173ea247dbb6c74e7e18190f10e411e6448dceb774e5`), and the M2 title with
package opens on a dedicated 8 MiB worker (SHA-256
`e17541d6c8b6a462c1e2fa3a9353d8a24d89f602f2373f7b3132d758b25aa320`). A host mock verified both open calls run
on the worker with at least 8 MiB stack; these two variants still require console measurements.

## 2026-10-28 — M2 run 5: package worker and game entry

`ps5/evidence/m2/run5.klog` shows that moving package opening from the 2 MiB main stack to an 8 MiB worker permits
the NCA processing and both RomFS/ExeFS opens to finish; the game enters linked code at `7002614ee0`. The worker is
retained because the earlier main-stack path overflowed. Run 5 then aborts in strict service lookup. Its fatal
service name was lost in the asynchronous stderr pipe, so this does not identify the missing service.

The next build makes runtime `LOG`/`FATAL` direct synchronous klog writes and includes the existing `audren:u` and
`audout:u` service implementations. M2 has no host audio device: the SDL boundary reports unavailable and the audio
HLE uses its existing no-device timing path. The existing HID service and vibration-state implementation are linked;
no host controller is connected. The rebuilt title SHA-256 is
`0f749992125d25e5890169ed96677ac3a5a9b935dd437153fddbf00b506a1643`; it needs a console run.

## 2026-10-28 — M2 run 7: VI HLE boundary

`ps5/evidence/m2/run7.klog` reaches `nvdrv` and `vi:m`/`vi:s`/`vi:u` guest services through the desktop-compatible
unknown-service path, then faults after the `vi:m` stub returns no display object. The desktop runtime's `nn::vi`
HLE was absent from the M2 link. The next build links that HLE: its guest display, layer and native-window handles
are software-side objects, not a PS5 VideoOut surface. M2 still stops at `nvnBootstrapLoader` before a renderer is
created, and logs the requested NVN name. The rebuilt title SHA-256 is
`05b3c8625f89f6fedc60450c7814fc0e46530a73e981fd0a07a60d1b62222777`; it requires console qualification.

## 2026-10-28 — M2: the PS5 title runs the generated 1.9.0 game code to its first NVN call (.41, ps5/evidence/m2)

`bash ps5/tools/build-game.sh`: runtime + `libgamecode.a` + package library statically linked into one 252.6 MB
`eboot.bin`. Runs 8 and 9 (same eboot `05b3c862…`): startup → guest reserved `0x7000000000-0x7320000000` → broker
permission repair → packages opened (RomFS 15,132,662,100 bytes in **2.83 s / 3.18 s**) → `entering linked game code
at 7002614ee0` → services lm, apm, appletOE, aoc:u, ectx:aw, time:u, fsp-srv, nvdrv, vi:* → **`first NVN call:
nvnBootstrapLoader requested=nvnDeviceInitialize`** → LoadExec exit. ExeFS and RomFS both come from the packages.

What runs 1–7 found, each fixed before the next:
- run1: `getcwd` in a title jumps to an unmapped address (SDK `directory.c` notes its internal import fails in
  titles); bound to the SDK's `ps5_getcwd`. The title build now checks every dynamic import against the SDK stubs of
  the modules it loads.
- run2/3: `chdir` in a title fails with EPERM; the PS5 build uses absolute paths under the data root (writes stay
  broker-relative).
- run3/4: the process vanished with no dump inside nstool's `NcaProcess::process`: the 2 MiB main-thread stack
  overflowed. Package opens run on a joined 8 MiB thread. (Also: the title C++ runtime's `operator new` trapped on
  failure without `new_handler`/`bad_alloc`; fixed.)
- run5/6: `LOG`/`FATAL` now go to klog synchronously; the PS5 main forced HLE strict mode, which desktop never
  enables, so `ectx:aw` aborted. Removed: unknown services take the upstream logging stub as on desktop.
- run7: `vi.c` (desktop nn::vi HLE) was not linked; added.

Also measured: `stat("")` in a title returns EINVAL (not ENOENT), which makes throwing `std::filesystem` calls throw.
Flexible memory free at startup: 212 MB of 448 MB (the eboot's own segments use the rest). Not yet in M2: rendering
(M3), input devices (M4), audio output and movies (M5; the audio service HLE runs with no host device).

## 2026-10-28 — Package RomFS: one reader per concurrent read (host check)

The package stream no longer serialises reads behind one mutex. Every layer of a RomFS reader keeps a position, so
concurrent reads now check out their own reader; up to 4 exist, made on first concurrent use. Extra readers share
the first one's keys, BKTR indirect/subsection tables and verified IVFC hash layer (`nstool-streams.patch` adds
copy-over-stream constructors to the pietendo BKTR and IVFC streams and `NcaProcess::getPartitionReaderFactory`);
each adds two package file descriptors and at most 229 KiB host RSS (measured with three extra readers, thread
overhead included). The C API is unchanged.

Host, base NSP + 1.9.0 update NSP: **41,062/41,062** RomFS files match, now hashed by 4 threads at once. Random
64 KiB reads with 4 threads: **82.6 → 406 MiB/s** (1 thread: 106.6 / 111.8 MiB/s). This is a host check; the console
rate is still the 42.5 MiB/s of the title run above until the rebuilt probe title runs on .41.

## 2026-10-28 — M3 title linked on the host (renderer on RADV, not yet run)

`bash ps5/tools/build-game.sh` now links the desktop Vulkan backend (`runtime/nvn/*`, `runtime/nvn/vulkan/*` without
FSR 3 frame generation and the ImGui glue), the real `nvnBootstrapLoader`, RADV (pinned archive, linked whole) and
shaderc into `ps5/dist/game/PPSA90212/eboot.bin`: 278,721,662 bytes, SHA-256
`51fc7cfee12d53cb8a2cd89c9b00e0cd62e478db53ccbeada3ed185d7c94c8da`, `.text` 233 MB. Found while linking: a title has
no Vulkan loader and RADV exports only `vk_icdGetInstanceProcAddr`, so the build generates one trampoline per command
of RADV's `vulkan_core.h` (`ps5/tools/gen-vk-globals.py`); RADV's archive carries Mesa's zlib 1.3.1, which collided
with the separately compiled zlib, so the runtime now uses RADV's. `runtime/ps5/window.c` makes the VideoOut display
plane surface from the modes the display reports (largest extent, 119.88 Hz preferred; host test
`ps5/probe/test_window_modes.c` covers a 4K, a 1080p and a 1440p mode list). The desktop `runtime-objs` target still
compiles. This is a link result: no console run yet.

## 2026-10-28 — Per-thread package readers in the title (.41, package-probe-title-run4)

The per-stream mutex is replaced by a pool of up to 4 readers per package. Inside the unelevated title: open 3.01 s,
220/220 samples match, sequential 69.2 MiB/s, random 64 KiB 41.9 MiB/s with 1 thread and **163.5 MiB/s with 4
threads** (it was 42.5). Title plain-file reads measured 124–171 MiB/s with 4 threads.

## 2026-10-28 — M3 run 1: VideoOut + RADV up in the game title (.41, ps5/evidence/m3/run1)

The title found 2 modes (3840×2160 at 119.88 and 59.94 Hz) and chose 119.88 Hz. AvControl reports
VRR(peg:120 range:48-120); the GPU is RADV NAVI21 with Vulkan 1.4.354; the swapchain is 3840×2160 with 4 images,
FIFO, at 119.88 Hz. After that the host faults inside `ipc_handle_request` handling a guest request with a 270-byte
buffer at 0x7090000000. No frame has been presented yet.

## 2026-10-28 — M3 run 1: VideoOut 4K at 119.88 Hz with VRR, then a time-zone service fault (.41, ps5/evidence/m3/run1.klog)

Eboot `51fc7cfe…`. The renderer came up: the display reported two modes (3840x2160 at 119.880 and 59.940 Hz), mode 0
was chosen, the WSI logged `119.88 Hz selected with title VRR metadata` and AvControl `VRR(peg:120 range:48 - 120)`;
RADV NAVI21 (API 1.4.354) made the device with maintenance5, graphics pipeline libraries and feedback loops, a
3840x2160 swapchain of 4 FIFO images, and the game's NVN device initialized. The title then stopped on SIGSEGV at
address 0 in `ITimeZoneService` command 101 (ToCalendarTimeWithMyRule): the title libc's `localtime_r` leaves
`tm_zone` NULL, which `runtime/services/time.c` read. Fixed. The same run showed the shader and pipeline caches
being looked up at cwd-relative paths, which a title cannot use (no `chdir`); their reads now resolve through the data
root. The rebuilt eboot (`b1bb1921…`) needs a console run.

## 2026-10-28 — M3 run 2: first frame presented, then RADV's shader address window was full (.41, ps5/evidence/m3/run2.klog)

Eboot `b1bb1921…`: `[vk] first game frame presented: game image 1920x1080 into the 3840x2160 swapchain (fifo)`, then
`vkCreateDescriptorPool` failed with -2 (VK_ERROR_OUT_OF_DEVICE_MEMORY, not host memory) about 0.5 s later. RADV's PS5
winsys puts every descriptor pool, push-constant and shader buffer in a 4 GiB window at 0x200000000, where the kernel
places mappings given no address; the runtime's 256 MiB guest-stack reservations (40 threads) were placed there too
(a stack at 0x210000000 in this log) and filled it. Guest stacks are now reserved at fixed addresses from
0x1000000000. Rebuilt eboot `b692af88…`; needs a console run.

## 2026-10-28 — M3 run 3: the game renders on VideoOut at ~58.6 fps (.41, ps5/evidence/m3/run3)

With guest stacks moved out of RADV's 32-bit BO window (run 2: `vkCreateDescriptorPool` -2), the game presents its
1920×1080 image into the 3840×2160 FIFO swapchain at 119.88 Hz with VRR: `frame 109 53.8 fps max 252ms`, then
`frame 227 58.6 fps max 75ms` (236 draws/2 s, 0 waits). After ~4 s the game's own sead assertion fires
(`[lm] … seadRawPrintNin.cpp PrintStringImpl … SDK has been aborted or failed assertion with no message`) →
`svcBreak(reason=0)` at guest 0x700267974c. Not yet seen on the TV by a person; title screen unconfirmed.

## 2026-10-28 — M3 run 3's abort: the SDK's controller applet path on an empty hid shared memory (host analysis)

The game's own backtrace in run3.klog, symbolized against the 1.9.0 SDK module (base 0x7002504000):
`nn::diag::detail::AbortImpl` ← `nn::hid::GetNpadJoyHoldType()` (0x7002a1a770) ← `nn::hid::ShowControllerSupport`
(0x7002a43410) ← game code ← `nnMain`. `GetNpadJoyHoldType` reads `NpadSharedMemoryAccessor::GetNpadCondition()`
from the hid shared memory and returns 0x58cca (module 202 hid, description 710) when its "initialized" byte is 0. No
input service fills that memory: desktop never reads it, because `hle/input.c` answers the Npad calls itself
(`GetNpadStyleSet`: player 1 FullKey; `GetNpadStates`: connected). The PS5 link left `input.c` out with its device
code, so the recompiled SDK reported no controller and the game opened the controller applet. The guest-facing part
is now `runtime/hle/npad.c` (with the vibration handles), and `hle/motion.c` no longer touches SDL (the SDL sensor
calls moved to `input.c`); both are linked into the title. Before M4 player 1 reads nothing pressed and the six-axis
sensor lies flat and still, as on desktop with no device in hand. Host check: `ps5/probe/test_npad_hle.c`.

Fatal reports now explain an SDK abort: svcBreak with a 4-byte argument logs the aborted result
(`[kernel] svcBreak: the SDK aborted on result …`), and every fatal report prints the guest call chain from its frame
records (`guest backtrace (frame pointers)`: `f_<entry>+offset` and the module's exported name when the function is
that symbol) and the last 16 IPC requests of any thread (`thread`, service, command, result).

## 2026-10-28 — M3 run 4: 100 s of rendering with no fault (.41, ps5/evidence/m3/run4)

Run 3's abort: with no controller, the game called `nn::hid::ShowControllerSupport`, then `GetNpadJoyHoldType` read
zeroed HID shared memory and aborted on result 0x58cca. Desktop's Npad HLE is now shared (`hle/npad.c`, SDL-free
`hle/motion.c`); before M4, player 1 is connected with nothing pressed. Run 4 (eboot `d395fc11…`) ran the full 100 s
window: mostly **59.9 fps, max 17 ms** per 2 s window. The hitches line up with first-time shader compiles (898 over
the run; 335 in the 7.7 s stall at frame 2720, plus 33 MB of uploads). That is the cold-cache case D6/M6 precompile
targets. Not yet confirmed on the TV by a person.

## 2026-10-28 — M4 steps 1–2 (.41, ps5/evidence/m4 run1–2)

DualSense through scePad: user service, pad open and "DualSense added" logged; buttons and sticks confirmed working by
the user over Chiaki-ng Remote Play with a keyboard mapped as the gamepad. Gyro/accelerometer and rumble (step 2,
eboot `138c0748…`) are wired, but this setup only delivers Chiaki-ng's idle motion (constant 0 1 0 G, 5–6 samples/s)
and no rumble target. **Deferred by the user:** physical-DualSense checks (gyro aim, flat-table axes, rumble,
power-off/PS-button reconnect).

## 2026-10-28 — M4 step 3: Savage Wind's Options pages on PS5 (.41, ps5/evidence/m4/run3)

Eboot `5bfb3044…` with the button-prompt font in the title folder. Start-up reads the Video page from
`savagewind.ini` (frame rate 60, upscaler off), resolution Auto → mode 0 3840×2160 at 119.88 Hz with VRR, and the
game runs ~60 fps (frame 1082 at 59.9 fps) with no fault. Pages, the Resolution/Frame rate rows and the Confirm-button
option (default Circle = Switch layout) are not yet checked by hand. Step 4 (USB keyboard/mouse polling) is not
started. Host: desktop `runtime-objs` builds; `test-vibration` and `test-ps5-input` pass; `test-bind-capture` cannot
link here (no libSDL3 on this PC, same before this change).

## 2026-10-28 — M4 step 4: USB keyboard and mouse opened (.41, ps5/evidence/m4/run4)

Eboot `b8ace418…`: keyboard and mouse sysmodules and `sceKeyboardInit`/`sceMouseInit` return 0; USB keyboard indexes
0–1 and mouse indexes 0–1 open for the signed-in user; the game runs with no fault. Key, click, motion and
unplug/replug evidence needs a device plugged into the console (not yet done). The PS5 Quit dialog now says it closes
the game and returns to the PS5 home screen.

**Deferred by the user (2026-10-28):** USB keyboard/mouse hands-on checks (keys, clicks, motion, unplug/replug).
