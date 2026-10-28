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

## 2026-10-28 — M5 audio and movies linked on the host (not yet run)

Eboot `e2b5c156…`. `runtime/ps5/audio.c` serves the SDL audio-stream calls of `runtime/services/audio.c` with one
SceAudioOut main port (48 kHz stereo S16, 256-frame periods, `sceAudioOutOpen(0xff, MAIN, 0, 256, 48000, 1)` as
PS5_vkQuake's `src/audio_ps5.cpp`) and one worker; `make test-ps5-audio` passes (also under ASan/UBSan). Movies link
`runtime/hle/movie.c` on pacbrew's installed FFmpeg (ps5-payload-ffmpeg 7.0.1-1; vp9/vp8/h264 decoders, matroska and
mov demuxers, vorbis/opus/aac present in the title map); with no host RomFS folder a movie is read from the RomFS image
the game opened. Two link findings:
- The installed package is not exactly the pinned recipe's build: its configure line has `--enable-version3
  --enable-openssl` but not the recipe's `--enable-gpl --enable-libx264` (an encoder only; LGPL-3 FFmpeg). The archives
  are pinned by digest in `ps5/deps.lock` (`pacbrew-ffmpeg`).
- The archives reach libc names no title module exports: `_setjmp`/`_longjmp` (OpenSSL ASYNC), `___mb_cur_max`
  (libiconv) — tail jumps in `runtime/ps5/libc_aliases.S` — and `isatty` (av_log's default callback), `getnameinfo`,
  `gai_strerror`, which only libScePosixForWebKit's stub defines (null imports in a title, `../forever/PS5_Vulkan/tools/
  radv-link.sh`); `ps5/tools/link-recipe.sh` binds them to the platform layer's `ps5_*`. NEEDED adds only
  libSceAudioOut.sprx. Still to see on the console: the AudioOut port, 10 s counter lines with zero underruns/errors,
  the intro movie, and the user's listening check.

## 2026-10-28 — M5 run 1: game audio on SceAudioOut (.41, ps5/evidence/m5/run1)

Eboot `e2b5c156…`: AudioOut port open at 48 kHz stereo S16 (256-frame outputs); the game's renderer stream (F32
stereo) plays through it. Over 90 s, every 10 s window: ~1875 periods, **0 underruns, 0 errors, 0 dropped frames**,
queue peak 2512 frames. No `[movie]` line in this run: with nobody at the controller the game stayed at the title
screen, so the intro movie path is not yet exercised. Listening check and the 10-minute run pending (user away).

## 2026-10-28 — M5 run 4: the title screen's silence is the game's (.41, ps5/evidence/m5/run4)

Runs 2-3 logged an exact -120 dBFS output for 90 s at the title screen. Eboot `b0d0d4e5…` added stage meters: the
SDK renderer renders all 2000 frames per 10 s (127 us each) and finds its device sink in every one, but **0 voice
data-source commands (max 0)** and sink peak 0.000 (enabled command ids 0x2188880: none of 1-6, the PCM/ADPCM
sources); the PS5 side passes it on unchanged (2000 puts, 480000 frames, all zero, 1 stream resumed, gain 1.00). The
RomFS read count also stops at 16 (boot) for the whole run, as in M4 runs 2-4: nothing from `Sound/Resource` is read.
Desktop showed the same (docs/SAVAGE-WIND.md, 2026-10-16: no `AcquireVoiceSlot` in 50 s at the title and menu, no
`Sound/Resource` read; the first sound is the `Continue` jingle `Sys_GameStart.bfstm`). So silence is correct there;
the listening check needs Continue or gameplay. The meters stay, one line each per 10 s: eboot `b8495e03…` folds the
PS5 side's stage levels into the `[audio] AudioOut` line and drops the mix-buffer scan (not yet run on the console).

## 2026-10-28 — Mouse-wheel zoom for scope and map (.41, ps5/evidence/m4/wheel-run1)

Shared input: the wheel pulses d-pad Up/Down (scope, AppCamera id 4) or right stick up/down (map, AppMap id 0x2a)
only while that screen's open-state byte (new optional site `screen_state`, 0xfe on 1.9.0) is 2; otherwise dropped.
Toggle `mouse_wheel_zoom`, default on. Eboot `87c68e8a…` runs ~60 fps with audio, no fault. Scope/map zoom on hardware
needs a USB mouse (deferred with the USB checks). 1.8.2 site unresolved (no 1.8.2 main here).

## 2026-10-28 — M5: real game audio during the user's session (.41, ps5/evidence/m5/user-cutscene.klog)

Captured while the user played (title → the opening, a real-time rendered cutscene; no `[movie]` line, so no FFmpeg
movie played). Audio: the renderer went from 0–2 voice sources at the title screen to **up to 40** in play
(render 127 → ~520 us per frame); output level peaks −10 to −38 dBFS, rms −31 to −64 dBFS; **0 underruns, 0
errors, 0 dropped frames** in every 10 s window. Video: 59.9 fps in most 2 s windows, dips to 37–53 fps with up to
312 ms frames as the world first loads (draws rise to ~470k per 2 s). Pending: the user's listening check, and a
pre-rendered movie for the FFmpeg path.

## 2026-10-28 — M6 measure 1: per-frame CPU/GPU at the title screen (.41, ps5/evidence/m6/measure1)

Eboot `cf832101…`, 200 s idle at the title screen with no player. Each 60 s window had 99.0–99.2 % of frames on
time; frame times p50 16.75, p95 16.75–17.00, p99 17.00–17.25 ms. Per frame: render thread CPU 1.1 ms, GPU 1.8 ms,
guest threads 43.5 ms summed. The 3 hitches (44–47 ms) all came in the first minute, exactly 119 frames apart, and were
logged as command translation on the render thread, with no compiles or uploads. These are title-screen figures;
gameplay CPU/GPU (S6, the M6 target) needs a player or the benchmark route.

## 2026-10-28 — M6 measure1: per-frame CPU/GPU on the title screen (.41, ps5/evidence/m6/measure1.klog)

Eboot `cf832101…` adds `runtime/frametime.c`: GPU timestamps around every submission (always on), the render
thread's CPU time, the guest threads' waits (kernel waits, core waits, sleeps, backend, RomFS reads) and a `[hitch]`
line with its cause for each frame over 1.5 periods (25 ms at 60 fps); `[perf]` lines per minute and per 10 minutes.
200 s idle at the title screen: 99.0–99.2 % of frames on time (≤ 17.5 ms), p95 16.75–17.00 ms, p99 ≤ 17.25 ms;
render thread 1.1 ms CPU per frame, GPU 1.8 ms per frame. Findings:
- The only three hitches (44–47 ms, frames 124/243/362) were the first three 2 s stats windows: "[vk] command counts"
  went out as ~31 separate klog writes, ~1.4 ms each on the render thread. Now one write. Every `LOG` on the render
  thread costs about that much: the logs stay one line per event.
- The PS5's `pthread_getcpuclockid` gives every thread the same clock, the calling thread's: every guest thread read
  the render thread's CPU time (their "43.5 ms per frame summed" is ~40 × 1.1 ms). Guest threads now record their
  own `CLOCK_THREAD_CPUTIME_ID` at each of their waits (`plat_thread_cpu_ns`); the render thread's figure was right.
- S6 (CPU headroom): on the title screen the render thread uses 1.1 of 16.68 ms and the GPU 1.8 ms. That is not
  the game's load; the guest threads' CPU and in-game numbers need measure2 (fixed clocks) and the benchmark.

Eboot `4b70bf98…` (measure2, host-built, not yet run) adds the fixes above, D6's first-start precompilation on the
PS5 (`runtime/platform/main_ps5.c`: the existing `precompile.c` over the package RomFS, before the game runs, with
the pre-warm's progress bar and a klog line every 5 s; caches through the broker as before) and the built-in
benchmark (`runtime/benchmark.c`, below in the plan's M6).

## 2026-10-28 — M6: first-start shader precompile on .41 (ps5/evidence/m6/precompile-first*.klog)

Eboot `4b70bf98…` (measure2), with no shader caches: **880.5 s in total**. Reading the shader archives took 29 s,
translating and compiling 13,997 shaders took 29–400 s (13,350 translated, 623 cached, 24 failed), and 13,973
pipeline libraries took 400–880 s. The pipeline-library rate starts at ~660/s and falls to ~10/s by the end; the
cause is under investigation. Title heap peaked at 1,148 MB. The game started afterwards. The on-screen indicator is a
bar with no text yet.

## 2026-10-28 — M6: first-start precompile, where the 880 s went (host measurements, build/pcbench)

Host harness: the real `shader_precompile_run` over `build/romfs-reference` on RADV's null NAVI21 device (Mesa 25.2.8,
`RADV_FORCE_FAMILY=navi21`), Xeon Silver 4116.
- **Translation step: the storage broker, not the CPU.** On the host the same 13,997 translations take 47 s on 7
  threads (CPU-bound, 662 %); on .41 they took 370 s on 16 threads. Each made two files (cache/xlate, cache/spirv:
  ~26,700), at the measured 11–15 ms per file creation on /data, one broker request at a time (`request_lock`):
  ~13.9 ms per file observed. Fix: `runtime/nvn/vulkan/cache_pack.c` — the precompilation's entries go into one
  `precompiled.pack` + `precompiled.idx` per cache folder, appended 8 MiB at a time by a writer thread; readers
  (`xlate_load`, `spirv.c cache_load`) look in the pack first, the same bytes and checks as the files.
- **Pipeline-library step:** the host builds all 13,973 libraries in 59 s on 7 threads and 49–51 s on 13 (CPU-bound;
  the memory budget costs ~4 % at 13 threads; peak RSS 0.44–0.82 GB, far below the 1.3 GB-per-shader estimate). The
  rate curve's shape (fast small shaders first, then ~100/s) is the take order; on .41 the first 7,400 libraries
  tracked the host's 7-thread run, then fell to ~10/s, so the console-only slowdown is in the largest shaders. Large
  allocations in that step: 84,000 of ≥256 KiB (39 GB churned, ~20 per large shader) and 309 million in all.
  RADV's own disk cache (`/app0/radv-shader-cache` database, default on PS5) costs the host 10 % (65.3 vs 59.1 s);
  the title now turns it off (`MESA_SHADER_CACHE_DISABLE`), since it keeps the driver's binaries itself. The
  remaining console-only cause is not identified yet: a new 5 s klog line splits the workers' time (work wall vs
  its thread CPU, reads, budget waits; process CPU; title heap segments and libc fallbacks; RADV buffers).
- **The 24 failed translations** (state at this entry; since fixed by the translator work, 13,997/13,997 on the host,
  console to confirm) were the translator's, identical on desktop (the host build failed the same 24):
  20 compute shaders of `agl_technique_pfx.sharcb` (16 × unknown instruction `eb1804a000470000` at 0x18, 2 ×
  `eb28048600f70600` at 0x450, 1 × `eb28048600f70400` at 0x3f0, 1 × `eb18048000470300` at 0x10) and 4 fragment shaders
  of `agl_common.sharcb` (`tlds info 6`); now logged once, grouped, after the step.
- Packs on the host: 92 MB (xlate, 13,973 entries) + 267 MB (SPIR-V, 13,899) in two files each; all 13,973 libraries
  built from them; a second run found every translation in the packs (13,973 cached in 3.1 s).
- Eboot `9d7ed132…` (this build) also draws text on the precompile screen (heading, percentage, step, "only on the
  first start and after an update") in Fira Sans through `glyphs_text_line`, copied into the window image.

## 2026-10-28 — First-start precompile, run 2: 276 s (.41, ps5/evidence/m6/precompile-first2.klog)

Eboot `9d7ed132…`: pack files instead of ~26,700 broker file creations, and RADV's own disk cache off. Total
**276.2 s** (run 1: 880.5 s): translation 189.5 s, pipeline libraries ~85 s; the TV shows "Compiling shaders" with
percentage and step. The same 24 shaders failed as on desktop (a translator gap). Every 5 s line reports **process CPU
1.0 cores** while 16 worker threads are busy: the title's threads appear to share one core. Under investigation
(thread affinity); this would also limit the game. The xlate pack was written empty (0 entries) although 13,909
translations were made.

## 2026-10-28 — First-start precompile, run 3 (.41, ps5/evidence/m6/precompile-first3.klog)

Eboot `059dd307…`: **done in 318.0 s** (translation 184.5 s, pipeline libraries 129.5 s). Both cache packs are
filled; the memory-budget wait is 0 with the budget sized from free direct memory (10,931 MB free). **Affinity is
ruled out**: every worker's mask is 0x1fff and the per-item `sceKernelGetCurrentCpu` histogram spreads over cores
0–12. Wall-clock scaling is 28/s with 1 worker vs 61/s with 16 (**2.1x**; 6.6x on the host), so the workers contend
on a shared lock. After start-up the AOC lookup re-opens the packages once per DLC index ("Add-on content not
found" six or more times), and three ~28 ms render-thread hitches remain in the first 6 s.

Changes after run 3 (host-built, not yet run on the console):
- **Heap arenas per thread** (`runtime/ps5/heap_arenas.c`, linked with `--wrap=ps5_heap_*`): the SDK's title heap is 8
  dlmalloc mspaces, each behind a lock, handed out round robin, so 16 workers share them two by two. The precompile
  workers now each get an mspace of their own from the same range (`plat_thread_heap_own`; a pool of 32, returned
  when the thread ends). The SDK's dlmalloc keeps FOOTERS, so a free or realloc from any thread goes to the owning
  arena. The 5 s line adds the heap's call count and time (sampled one call in 64 by the time stamp counter, since
  the build's emulated TLS allocates on first use and can't be used inside malloc) and the number of threads on
  arenas of their own. The scaling check now compares 1 worker, 8 and all workers on arenas of their own, and all
  workers on the shared arenas, in one run.
- **AOC:** `savagewind_package_addons` (ps5/importer) reads which add-on indexes the packages hold once per process,
  and `fs_aoc_present`/`load_image` answer from that mask instead of opening the packages for every index.
- **The three ~28 ms hitches at frames 124/244/364** were the first three 2 s windows' `[vk] command counts` dump
  (still ~1 KB in one klog write). The PS5 no longer dumps it.

## 2026-10-28 — First-start precompile, run 4: 120 s, 0 failed (.41, ps5/evidence/m6/precompile-first4*.klog)

Eboot `08f0e14f…` (translator fix + per-worker heap arenas + cached add-on lookup). **done in 119.8 s** (run 1: 880.5
s): reading 25.0 s, translation 77.6 s, pipeline libraries 37.9 s; **13,997/13,997 translated, 0 failed** (was 24).
The scaling check confirms the heap lock as the contention: 1 worker 27/s; 16 workers sharing the SDK's 8 arenas 52/s
(1.9x); 8 workers with own arenas 201/s (7.4x); 16 with own arenas 279/s (10.3x). Add-on lookup is now one 0.10 s scan
(no repeated package opens), and no hitch occurs in the first windows (59.9 fps, max 18 ms). The user saw the
"Compiling shaders" screen finish. Title heap peaks at 1,728 MB (freeing it after precompile is still open). The
game's own threads still use the shared arenas.

## 2026-10-28 — M6 benchmark: Great Plateau route, two runs (.41, ps5/evidence/m6/bench1-2.klog)

Built-in benchmark (eboot `08f0e14f…`, 4K output, 2560×1440 internal (FSR 1), no player), 60 s measured after 20 s settling:

| | run 1 | run 2 |
|---|---|---|
| frame ms mean / p95 / p99 / max | 16.707 / 16.768 / 26.719 / 39.8 | 16.683 / 16.771 / 18.237 / 33.4 |
| on time | 98.4 % | 98.8 % |
| hitches | 37 (30 command translation, 4 pipeline creation, 3 shader compile) | 30 (all command translation) |
| render thread CPU mean / p95 | 10.35 / 13.98 ms | 10.58 / 14.40 ms |
| GPU mean / p95 | 7.74 / 8.48 ms | 7.77 / 8.47 ms |

The runs agree within 2 % (mean −0.14 %, p95 +0.02 %): the benchmark meets its done-when. The frame target of 95 %
on time is met on this route. **S6 / headroom:** the GPU uses ~47 % of a 60 fps frame; the render thread (translating
NVN commands to Vulkan) uses 62 % on average and 86 % at p95, so it is the limit and the source of the remaining
hitches. Guest-thread CPU figures are summed per-thread self-reports and are indicative only.

**Deferred by the user (2026-10-28):** the M6 10-minute played route (4K and 1080p) and freeing the precompile heap; M7 started.

## 2026-10-28 — M7: MihawkGen frame generation linked on the host (not yet run)

Eboot `46779550…` links PS5_MihawkGen `c57232a9…` (`ps5/deps.lock`; `.deps/ps5/mihawkgen` from `git archive`, weights
SHA-256 checked) behind `runtime/nvn/vulkan/mihawk.cpp`, and ships `network.fgw` (1,757,240 bytes) with MihawkGen's
licence and notices. Desktop `make runtime-objs` still builds without it (Makefile filter; its backend is FSR 3).
Video > Frame generation (On / Off, `frame_generation` in savagewind.ini) is listed only at 119.88 Hz. Expected in
klog with it on: `[fg] MihawkGen neural 3840x2160, 3 slots, … passes, weights /app0/network.fgw (1757240 bytes)`,
`[fg] MihawkGen on the final 3840x2160 images …`, `[fg] first generated frame …`, then every 2 s
`[fg] … generated frames/s: … presented (MihawkGen: gpu_ms … max … over … frames; … game frames shown twice …)`.
The benchmark summary now also has `presents …: … presented fps (… in frame generation's slot); game frames … fps`,
`present intervals ms: …` (times handed to the display, a 6-bucket histogram) and `frame generation gpu ms per
generated frame …`. Host: `make test-framepace-fg` passes (9/9: 60 at 119.88 Hz keeps the game at 16.683 ms and
spaces the presents 8.342 ms; FSR 3's period unchanged). Still needed: the FG-on and FG-off benchmarks on .41, VRR on
the TV, and the user's eyes on artifacts (camera pan, Link running, HUD).

## 2026-10-28 — M7 first benchmark: MihawkGen on the PS5 (.41, ps5/evidence/m7/bench-fg0/1.klog)

Eboot `4677955…`. **FG off:** unchanged from M6 (59.9 fps, 98.7 % on time, GPU 7.7 ms). **FG on, broken pacing:**
game 22.6 fps, presented 45.2 fps instead of 120; 0 % on time with 1,326 "render queue / GPU wait" hitches; present
intervals mostly 18–25 ms. GPU is not the limit (10.5 ms per game frame + MihawkGen 2.8 ms per generated frame).
User report (playing): the hearts and health bar leave ghosts, because MihawkGen gets frames with the HUD on; the
HUD-less variant D7 deferred is now required.

## 2026-10-28 — M7: why FG on ran at 22.6 fps, and the HUD-less fix (eboot `bd57c53b…`, host-built, not yet run)

In bench-fg1.klog the device has **one queue** (`[vk] presents on the calling thread`), so each pair's presents ran
on the render thread: `present_wait_turn` waited for the GPU to finish the generated frame, then half a frame more
before the game's frame (27 ms in the present per frame; render thread 15.1 ms CPU). Changes: with one queue,
MihawkGen's first frame starts the present thread on that queue (`vk_queue_lock` around submits and presents), so the
render thread no longer waits for the GPU or the spacing; one more swapchain image (5) and six acquire semaphores for
two presents per game frame; a `[fgpipe]` line per 2 s with each wait (acquire, present-queue room, generator slot,
queue lock; the present thread's GPU wait, spacing, present call, queue lock; queue depth). The generator now takes the
HUD-less images of both frames (`exec.c` `vk_fg_mihawk_inputs`, the capture desktop's FSR 3 path uses, drawn to 4K as
the final image is) and `fg_hud.frag` puts the newest frame's HUD (where final and HUD-less differ, 3 × 3 dilated)
over the result. `fg-capture.txt` (`<count> [<every>]`) in the data root saves pairs of window images (generated, then
real; halved PNG) to `screenshots/`.

## 2026-10-28 — M7 benchmark 2: own present thread (.41, ps5/evidence/m7/bench2-fg0/1.klog, captures/)

Eboot `bd57c53b…` (presents on their own thread on the one shared queue, 5 swapchain images). **FG on: presented
115.8 fps, game 57.9 fps**, 78.8 % on time; GPU 11.43 ms per game frame + MihawkGen 2.78 ms per generated frame.
Present intervals: 75 % in 6.5–10 ms, 10 % under 6.5, 12 % in 10–15. FG off is unchanged (59.9 fps, 98.8 %, GPU 7.72
ms). The HUD-less capture is not yet used on the PS5 ("from the final images"). Six generated/real capture pairs
(1920×1080) were taken on the benchmark route; the hearts look clean in them, but that route's camera turns too slowly
to show the ghosting the user saw in play.

## 2026-10-28 — M7 benchmark 2, read closer, and MihawkGen at the game image's size (host-built, not yet run)

From bench2-fg1.klog:
- **The HUD-less path was used in play.** The `[fg]` lines in settling and the route say "0 of them from the final
  images"; the one "from the final images" line was the first generated frame, at start-up/loading, where there is no
  3D scene composite (68 per 2 s there).
- **57.9 game fps is late frames, not a cap:** frame time p50 16.687 ms (the 59.94 grid; framepace.c applies no VSync
  margin under MihawkGen, `make test-framepace-fg`), mean 17.27 from 21 % late frames. In the route the render thread's
  acquire wait was 0.02 ms per frame (the 6.16 ms whole-run mean comes from start-up and loading), but its CPU rose to
  13.6 ms per frame (10.7 with FG off), and the late frames follow it.
- **GPU 11.43 ms per game frame includes MihawkGen** (the frame's GPU time sums every submission of that frame): 7.72
  game + 2.78 generator + ~0.9 for the 4K HUD-less draw (with its FSR 1 passes), the composite and the copies.
- **Uneven intervals:** a generated frame was presented as soon as the GPU finished it, sometimes under half a period
  after the game frame before it (9.8 % under 6.5 ms); late game frames make the 10–15 ms ones.

Changes (eboot `be3a1081…`): MihawkGen was meant to run at the game image's size, on the assumption that it is
1920×1080 (wrong: the game's final image is made at the window's 4K, RS_OUT; bench3 below), on the two HUD-less images
(`exec.c fg_mihawk`; the copy alternates between two images), puts the HUD over the result there, and the generated
frame is presented as FSR 3's is, through the same crop: no window-sized offscreen images or blits (but the
generator stayed at 4K, the final image's size). A generated frame now waits until half a period after the last game
frame's present. `[fgpipe]` adds the render thread's CPU in the present and in frame generation's recording. The
benchmark gains 10 s of unmeasured fast motion after the summary (Link runs forward while the camera turns at full
speed right, then left), where 8 generated/real pairs are captured with FG on.

For the input owner (not frame generation): in the captures the button prompts show keyboard keys (Move WASD,
Controls Tab) although a DualSense is connected; the benchmark sends no pad input, so the prompts may follow the last
device that sent input.

## 2026-10-28 — M7 benchmark 3: HUD-less generation, fast-motion captures (.41, ps5/evidence/m7/bench3-*, captures3/)

Eboot `be3a1081…`. FG on: presented 114.1 fps, game 57.1 fps, 73.9 % on time; present intervals now 94.0 % in
6.5–10 ms (p95 9.97). The render thread waits 6.86 ms per frame for room in the present queue (max depth 2). The
intended 1080p MihawkGen did not take effect: it still ran at 3840×2160 (2.75 ms). FG off unchanged (98.9 %).
**HUD:** generation now uses the HUD-less images with the newest HUD composited over them. In 8 generated/real pairs
captured during a fast camera turn with Link running, the hearts are clean and sharp in every generated frame (the
pixels around them differ by the motion itself), so the ghosting the user reported is gone in these captures; the
user's own check in play is still pending.

Why 4K, and the queue wait (host analysis of bench3-fg1.klog): the game's final image is drawn at the window's size
(textures.c RS_OUT: 3840×2160 at 4K, for a sharp HUD); only the 3D scene is at the internal size (`[post] scene
2560x1440 fsr1 -> 3840x2160`), and the "1920x1080" in early `[post]` lines is the title screen before the scale
applies. The queue wait: each generated present waited until half a period after the previous game frame's present,
so a late present moved every later one later and never back; the queue filled and the render thread waited.
Changes (host-built, eboot in the plan's M7 status): MihawkGen runs at the 3D scene's internal size (2560×1440 here;
the HUD-less images scaled down to it, the generated frame scaled up with FSR 1 EASU + RCAS as the scene is, then the
4K HUD over it); the pairs keep a grid of the game's period (a late frame moves it later, early frames bring it back
1 ms a pair, a frame over half a period off starts it over); one more queued pair (3) and swapchain image (6).

## 2026-10-28 — Correction: the benchmarks ran at 2560×1440 internal, not 1080p

Every in-game `[post]` line of the M6 and M7 benchmarks reads `scene 2560x1440 fsr1 -> 3840x2160`; the
`1920x1080 fsr1` line appears once, at the title screen. Earlier entries and messages said "1080p internal". The
GPU 7.7 ms per frame figure is at 1440p internal.

## 2026-10-28 — M7 benchmark 4: MihawkGen at 1440p, present grid (.41, ps5/evidence/m7/bench4-*)

Eboot `b3cca2a6…`. MihawkGen now runs at the scene size, 2560×1440: **1.90 ms per generated frame** (was 2.75 at 4K).
FG on: presented 115.7, game 57.9 fps, 79.1 % on time, GPU 11.6 ms per game frame. The present-queue wait is gone (0),
but the render thread now waits 5.45 ms per frame to acquire a swapchain image; intervals are 78.8 % in 6.5–10 ms.
FG off unchanged (98.7 %).

Analysis (host): the presents came at exactly the refresh rate (game 59.94, two presents each, 119.88 Hz FIFO), so
FIFO could never show them faster than they arrived: every late frame or long present left one more present queued
at the display, never drained, until no window image was free and the render thread's acquire became the brake
(vkQueuePresentKHR also blocked up to 6.4 ms). Changes (host-built, not yet run): with MihawkGen the game is held 0.5 %
under half the refresh rate (59.64 fps, presents 8.38 ms apart, 119.28 Hz: VRR shows each as it comes, and the queue
drains; `make test-framepace-fg` updated); and the generated frame's window image is acquired without waiting: with
none free, that game frame is shown without its generated one (counted as "dropped" in the `[fg]` line) instead of
the render thread waiting.

## 2026-10-28 — M7 benchmark 5: display queue drained (.41, ps5/evidence/m7/bench5-fg1.klog)

Eboot `771afc26…` (game held 0.5 % under half the refresh rate with FG on; a generated frame is dropped instead of
stalling when no window image is free). The render thread's acquire wait fell from 5.45 to 0.61 ms per frame, with 0
generated frames dropped. Still: game 57.8 fps, presented 115.5, 79.6 % on time, MihawkGen 1.92 ms. With every
pipeline wait now near zero, what remains is the render thread's own CPU time with FG on (late frames at p95 19.5 ms),
the work queued as "Render-thread CPU" after M7.

Where the remaining frames go (host analysis, per 2 s window of the route in bench5-fg1 vs bench4-fg0): it is not the
render thread's CPU in frame generation. Over the route the render thread used 11.22 ms per frame with FG on and
10.68 off (+0.54); `[fgpipe]` puts the whole present at 0.4–1.0 ms and frame generation's recording at 0.11–0.15 ms.
The late frames sit in the camera-turning half: there the render thread's CPU is the same with and without FG
(13.0–14.1 ms), FG off holds 59.9 fps (93–99 % on time), and FG on drops to 53–58 fps (35–64 % on time); in the still
second half FG on is 99 % on time. What FG adds there is GPU time on the frame's path: 11.0–11.8 ms per game frame
against 7.3–7.9 off. That is MihawkGen's 1.9 ms plus about 1.8 ms of 4K passes on the one queue (the HUD-less copy,
its scale-down, FSR 1 EASU + RCAS of the generated frame, the HUD composite, the generated frame's present), all
ahead of the next frame's GPU work, which the game's own waits then see. Cheaper options, not taken without eyes on
the result: the composite sampling the generated frame bilinearly (no EASU/RCAS passes), or 1080p internal. The
benchmark summary now gives the present's and frame generation's CPU per frame, and `internal = <height>p` in
benchmark.txt picks the internal resolution for a run (eboot in the plan's M7 status).

## 2026-10-28 — M7: 1080p vs 1440p internal with FG on (.41, ps5/evidence/m7/res-*.klog, captures-res/)

Eboot `22a6cf72…`, same route, FG on: **1080p internal: 88.4 % on time, GPU 10.35 ms, game 58.5 / presented 117.0
fps. 1440p: 79.4 %, GPU 11.62 ms, 57.8 / 115.6.** The capture PNGs are halved to 1920×1080, so they cannot show the
difference at 4K (their detail measure is equal: 11.16 vs 11.22). Default chosen: 1080p internal with FG on, 1440p
with FG off. Per the agent's analysis (bench5 entry), the FG-on misses are GPU time in the camera-turning half
(~7.5 → ~11.5 ms per game frame: MihawkGen 1.9 ms + ~1.8 ms of 4K passes), not render-thread CPU.

## 2026-10-28 — Render-thread profile (.41, ps5/evidence/m7/rprof-fg0/1.klog)

Diag eboot `c5bc5951…` (`render_profile = 1`), 1080p internal, the benchmark route. Render thread CPU per frame while
turning: **11.76 ms with FG, 11.01 without.** Of it: draw translation 9.65 / 8.89 ms over ~3,500 draws (per draw:
state preparation, descriptor sets, textures and fragment-stage sets ~1–1.6 ms each), present 2.57 / 0.18 ms. Vulkan
driver time inside it: 6.29 ms with FG (13,321 calls; `vkQueueSubmit2` 2.07 ms for 7 calls, `vkAcquireNextImageKHR`
1.06, `vkCmdDrawIndexed` 1.13) vs 3.35 ms without (`vkQueueSubmit2` 0.34 ms). So FG adds ~1.7 ms of submit cost on the
render thread, and ~3.3 ms of per-draw driver calls are what PR #189's recorder thread would move off it.

## 2026-10-28 — M7: frame generation's 4K passes cut, per-pass timestamps (eboot `4df894cb…`, host-built, not yet run)

Per generated frame the FG path ran, after MihawkGen at the internal size: the HUD-less blit (4K), its scale-down,
EASU into a 4K image, RCAS into another, `fg_hud.frag` into a third (FG_MH_OUT), then the present's 4K copy of that
into the window. Now (`exec.c fg_mihawk`, `vk_present_draw`, `shaders.c fg_out.frag`): RCAS and the HUD composite are
one pass, and that pass is drawn straight into the window image at the present when the game image is 1:1 there (the
PS5 at 4K, no SMAA) — two 4K images written and read fewer (RCAS's and FG_MH_OUT), the present's copy gone. In FSR 1
mode the bytes are the old ones: the same RCAS code reads the same EASU image, and its result goes into an 8-bit UNORM
target as rcas.frag's went into its RGBA8 image (the window must be 8-bit UNORM, else FG_MH_OUT is drawn first and
presented; a non-8-bit game image keeps RCAS's own image). Not fused: the HUD-less blit and its scale-down, since the
scale-down reads the blit's sRGB-encoded bytes and a shader can't reproduce the hardware's encode exactly (the
generator's input would change). savagewind.ini: `fg_upscale = bilinear` (EASU skipped, the generated frame sampled
bilinearly in the last pass; softer, default `fsr1`), `fg_passes = separate` (the old passes, for A/B),
`fg_split_submits = 1` (A/B, off: the game's frame and FG's passes each a submission of its own, so the game's fences
are signalled before FG's passes and those don't wait for the window image's acquire; two vkQueueSubmit2 more a frame,
which cost ~0.3 ms each on the render thread while the GPU is busy, rprof-fg1.klog). The `[fg]` line adds each pass's
GPU time (timestamps before and after it, mean over the frames that had it): HUD-less copy, down, EASU, RCAS, HUD
(separate mode or the not-1:1 case), present (the generated frame's into the window: with the fusion it includes RCAS
and the HUD).

## 2026-10-28 — Recorder thread (PR #189 port) and fused FG passes: A/B on .41 (ps5/evidence/m7/ab-*.klog)

Eboot `4df894cb…` (1080p internal, benchmark route), each a single run unless noted:

| | on time | render thread ms | GPU ms | game / presented fps |
|---|---|---|---|---|
| FG on, recorder off (2 runs) | 88.3 / 88.2 % | 11.22 / 11.33 | 10.34 / 9.99 | 58.3 / 116.6 |
| **FG on, recorder on (2 runs)** | **98.6 / 99.0 %** | **9.14 / 9.12** | 9.97 / 10.02 | 58.8 / 117.7 |
| FG off, recorder off | 99.0 % | 10.51 | 6.72 | 59.9 / 59.9 |
| FG off, recorder on | 98.8 % | 8.61 | 6.80 | 59.9 / 59.9 |

The recorder thread takes ~2 ms off the render thread and lifts FG-on frames on time from 88 % to 99 %. Fused FG
passes (RCAS + HUD drawn straight into the window): GPU 9.99 ms vs 10.34 with `fg_passes = separate`. No fatal lines.
**Recorder now on by default on the PS5** (`VK_RECORDER_DEFAULT`; desktop unchanged); eboot `83284089…` confirms
"recorder thread on" with no ini key: 98.8 %, render 9.22 ms. Presented 117.7 fps vs the 119.28 cap: the game averages
58.8, not 59.64.

## 2026-10-28 — Game code at -O2 (.41, ps5/evidence/m7/ab-O2-*.klog)

Eboot `a2401ab0…` (`PS5_GAMECODE_OPT=-O2`, recorder on): FG on 99.1 % / 99.1 % on time, render 9.10 / 9.13 ms, p99
17.04 / 17.09 ms (−O1: 98.6–99.0 %, 9.1 ms, p99 17.6–20.3); FG off 99.1 %, render 8.51. Within noise except a
slightly lower p99; -O2 costs ~2.5× the PC compile time. Kept as a switch, -O1 stays the default.
The game still averages 58.8 fps with FG on (3,530 frames per 60 s vs 3,578 at the 59.64 cap). The klog shows one
~4 s stretch of 148 consecutive ~27 ms "command translation" frames (frames ~1340–1488, before the measured window)
and 30 such hitches inside it.

The three recorder-on runs each report 3,530 frames and 30 hitches in the measured minute. The configured game grid
is 16.767 ms (59.64 fps); the O2 run's p50 is 16.754 ms and p95 16.846 ms, not a uniformly slower grid. Its two-second
`[vk]` windows each show one ~61–63 ms render-CPU hitch, and the following frame of the earlier uncapped hitch logs
occurs immediately after the `[vk]`/`[fg]`/`[fgpipe]` reports. Those reports run synchronously on the render thread.
The game grid retains its target after a sub-period lateness, restarts after a full period, and the separate present
grid's 1 ms correction cannot set the game's period. The reports are a strong suspect for the recurring lost time,
not proof until an A/B run; the hitch time also includes ordinary rendering and grid catch-up.

The earlier burst starts during the loading-screen transition to the world at 1440p, before the benchmark switches to
1080p at 27.3 s, and continues into settling. Frame 1248 initializes MihawkGen and takes 358 ms; successive ~25–28 ms
frames have about 17–18 ms in the guest present thread's backend and no significant acquire, queue-room, compile or
upload wait. `ft_frame` writes each long `[hitch]` synchronously before its next CPU timestamp, so the logged
\"command translation\" includes the preceding hitch log itself. The burst stops being logged at the 300-line limit;
this is consistent with diagnostic feedback, not evidence of a specific game event.

PS5 runtime `LOG` now queues klog messages to a bounded logger thread (drops are reported); fatal and fault diagnostics
remain synchronous. `log_sync = 1` in savagewind.ini or `SAVAGEWIND_LOG_SYNC=1` restores synchronous writes for A/B.
The change is host-built pending console measurement; compare the benchmark's frame count, p50/p95/p99, hitch count,
two-second maxima and present cadence in both modes, and check for dropped log lines.

## 2026-10-28 — Asynchronous klog: M7 frame-generation target met (.41, ps5/evidence/m7/ab-log-*.klog)

Eboot `0b5a59cf…`: klog lines go to a 512-line queue drained by a logger thread; fatal/fault output stays synchronous;
`log_sync = 1` restores the old path. Synchronous klog writes had cost ~61 ms after every 2 s report and fed a hitch
loop (each hitch line made the next frame late).

| FG on, 1080p internal | on time | game / presented fps | frame ms mean / p99 / max | measured hitches |
|---|---|---|---|---|
| async, run 1 | **100.0 %** | **59.6 / 119.3** | 16.767 / 16.984 / 18.4 | **0** |
| async, run 2 | **100.0 %** | **59.6 / 119.3** | 16.767 / 16.979 | 0 |
| log_sync = 1 | 98.8 % | 58.8 / 117.7 | 16.994 / 18.128 | 30 |
| async, FG off | 100.0 % | 59.9 / 59.9 | 16.683 / 16.798 | 0 |

Presented cadence 119.3 Hz with present-interval p95 8.51 ms, i.e. **M7's settled 119–120 Hz presented cadence is
met on the benchmark** (render thread 8.7 ms, GPU 9.9 ms incl. MihawkGen 1.63 ms). The remaining hitches are all
outside the measured route: loading-screen package reads on guest threads (65) and loading waits. "could not lower
logger priority" is logged once (the logger runs at default priority). Every earlier benchmark's "command translation"
hitches were largely this logging.

## 2026-10-28 — PS5 starts with controller button prompts (.41, ps5/evidence/m4/prompts-pad-at-start.png)

Before any input the prompts showed keyboard keys (Move WASD, Controls Tab) because the last-used device started as the keyboard. On the PS5 it now starts as the controller (`INPUT_START_DEVICE`; desktop unchanged); a benchmark capture with no input shows controller icons (sticks, Options). Keys, clicks or a mouse turn switch to keyboard icons; a controller button or stick switches back.

## 2026-10-28 — M7 checks by the user on .41

HUD clean with FG on (no ghosting on hearts/health bar); VRR active on the TV. Scene ghosting is visible in fast
motion (expected from colour-only generation; M8 motion vectors are the planned improvement). Input latency and the
physical DualSense remain untested. With the benchmark at 100 % on time and 119.3 Hz presented, M7's done-when is met
except the latency A/B; FG becomes the 120 Hz first-launch default.

## 2026-10-28 — Loading read investigation (instrumented, console run pending)

In `ab-log-async-a.klog`, 65 loading-screen hitches were attributed to package reads. Thread 124 commonly spent roughly
26 ms on three reads totalling 1.5 MiB, while the presenting guest thread waited; the log shows a second reader opened,
not saturation of the four-reader pool. The per-reader stream does synchronous seek/read through the NCA decrypt and
update-patch layers. `package-runtime step=read-profile` now reports cumulative request sizes, adjacent offsets on the
same thread, and pool waits every 1024 requests. `SAVAGEWIND_LOG_FILES=1` logs changing RomFS file names but adds logging
overhead. Neither the new counters nor an after-change hitch count have been measured on console; these measurements
are needed before choosing and qualifying a read-ahead or concurrency change.

## 2026-10-28 — M8 PRs integrated (off by default), precompile null fix, first-launch defaults (.41)

PRs #194/#195 (motion vectors, TAA, FSR 3.1) are integrated behind `antialias = taa` / `upscaler = fsr3`, off by
default. The first build (`c658aad8…`) crashed in the precompile's pipeline warm-up: `stage_layouts+0x132` read
`b->si->mvec` (PR #194's push constant) with `si` unset in `vk_pso_warm_stage`. Fixed by setting it. Eboot
`394c05da…`: precompile 68.2 s (all 13,997 translations cached), then **first-launch defaults with no keys in the ini:
FG 1, FSR 1, Performance (1080p internal at 4K)**, written to the ini. Benchmark: 99.4 % on time, 59.6 / 119.1 fps,
GPU 9.95 ms. Loading still shows 70 package-read hitches; read profile: 4,096 reads, 2.76 GB, 599 sequential,
0 reader-pool waits, 730 reads of ≥512 KB.

## 2026-10-28 — HDR10 output (.41, ps5/evidence/m7/ab-hdr-*.klog)

Eboot `273f379b…` (Video > HDR, default Off). HDR on: swapchain format 64 with colour space `1000104008` (HDR10
ST2084, BT.2020), "SDR white at 200 nits", and RADV's VideoOut reports `dynamic_range=2` with HDR scanout samples
every 10 s. HDR off: format 44, colour space 0 (SDR). AvControl reports the TV as `HDR:o HFR:o VRR:TypeA` in both runs,
a usable capability hint (the driver itself offers HDR from title metadata only). FG benchmark: HDR on 99.8 % on time,
GPU 10.03 ms; off 99.6 %, 9.98 ms; both 59.6 / 119.3 fps, so HDR costs nothing measurable. Whether the TV shows HDR
and how it looks needs the user. One HDR-off run produced no result (the first run after deploying; rerun was fine).

## 2026-10-28 — PS5 dynamic resolution, first console runs (.41, ps5/evidence/m7/ab-drs-*.klog)

Eboot `7bafc763…`. **Controller works:** with 1440p internal and `gpu_load = 8` ms for the first 20 s, the size
stepped down to 720p (21.5 s at 1280×720, 2.1 s at 1600×900) and back up to 2560×1440 (36.4 s) once the load ended.
**Cost with DRS compiled in but off is above the agreed 2 %:** GPU 10.33–10.36 ms and render thread 10.02–10.04 ms,
against 9.95–9.98 ms GPU and 9.05–9.17 ms render in the builds just before (99.6–99.8 % on time either way). Edge
artifacts at 720p are not yet captured (the fast-motion captures run after the route, at full size).

## 2026-10-28 — PS5 dynamic resolution, build 2 (.41, ps5/evidence/m7/ab-drs2-*.klog, captures-drs2/)

Eboot `406db332…`. **Compiled in but off, the cost is now within noise:** 100.0 / 99.9 % on time, render thread
8.94–8.96 ms, GPU 9.96 ms (before DRS: 99.6–99.8 %, render 9.01–9.17, GPU 9.95–9.98). With DRS on at 1440p and
`gpu_load = 8` for 20 s: the size steps to 1600×900 (2.1 s) and 1280×720 (21.8 s), then back to 2560×1440 (36.1 s).
Under the load the run is 78.6 % on time (game 54.6 fps), and the steps cause 12 shader-compile and 13
pipeline-creation hitches (first use of each size's variants). The 720p capture shows a full, unstretched frame with a
sharp HUD; a green/blue arc and glow near the sun side look like the game's sun lens flare, not confirmed (the DRS-off
run captured a different frame). The run tool's control payload timed out twice during DRS-on runs (the title kept
running; the klog was captured directly).

## 2026-10-28 — HDR default follows the system output (.41, ps5/evidence/m7/hdrdet-sys*.klog, hdr-default-on.klog)

`sceVideoOutGetOutputStatus` on a handle opened before RADV (layout: RADV's `struct videoout_output_status`) reads
**dynamic_range 1 with PS5 Settings > Video Output > HDR Off, and 2 with "On when supported"** on .41's HDR TV
(return 0 both times). `sceVideoOutGetCurrentOutputMode_` fails there (0x80290018) and is not used; the SDK's device
capability query has no documented ABI and is not called. First-launch default: 2 → `hdr = on`, 1 → off, failure or
other values → off. Confirmed: with no `hdr` key the game logged "HDR at start: yes" and wrote `hdr = on`.

## 2026-10-28 — Package read cost and SHA-NI (host investigation)

On the Xeon Silver 4116 host, 512 random merged-package reads per size averaged 0.138 ms (4 KiB), 0.222 ms
(16 KiB), 0.580 ms (64 KiB), 3.585 ms (512 KiB), and 7.070 ms (1 MiB). Isolated nstool AES-CTR cost
1.579 ms/MiB and SHA-256 in 16 KiB blocks cost 5.202 ms/MiB. IVFC verifies its hash layers at open and verifies
whole data blocks for each read, including partial ends; the host results identify hashing as the dominant
large-read CPU cost, not proof of the exact console breakdown. Host `perf` was unavailable (paranoid=4).

IVFC verification now uses an x86 SHA-NI implementation when CPU features and a one-time software comparison
pass; otherwise it retains nstool's software verifier. On this host SHA-NI is unavailable: the fallback matched
41,062/41,062 manifest files with four readers. Under QEMU x86-64 `-cpu max`, accelerated digests matched the
software implementation for empty, short, padding-boundary and 4/16 KiB inputs, and the startup self-test
also compared a 1 MiB input. These are host/emulated results; title startup selection, sample hashes, and
benchmark hitch counts require console evidence. DRS first-start precompilation now tracks a separate completion
marker and includes DRS variants of observed shader keys; no claim that all pipeline-state hitches are removed.

## 2026-10-28 — FSR 3.1 runs on PS5 (.41, ps5/evidence/m8/ab-fsr3fix2-*.klog)

Eboot `ed5b9c9e…`. The context failure was FidelityFX's debug-label wide-string conversion throwing (`range_error`
from `wstring_convert`; the earlier `0x8000000A` = `FFX_ERROR_INVALID_ARGUMENT` came from the same path); fixed, with
FFX exceptions now logged at our boundary. Earlier "FSR 3.1" benchmark rows were not FSR 3.1 (context never created).

| FG on | internal | on time | render ms | GPU ms | game / presented fps |
|---|---|---|---|---|---|
| FSR 1 Performance (ref.) | 1920×1080 | 99.8 % | 8.8 | 10.1 | 59.6 / 119.2 |
| **FSR 3.1 Performance** | 1920×1080 | 97.5 % | 11.5 | 12.35 | 59.5 / 118.9 |
| **FSR 3.1 Quality** | 2560×1440 | 95.4 % | 11.7 | 14.41 | 59.5 / 119.0 |

FSR 3.1 dispatches every frame (2,100+ per run). It costs ~2.3 ms GPU and ~2.7 ms render-thread CPU over FSR 1 at the
same internal size (the motion-vector and TAA inputs). Image quality comparison still to do.

## 2026-10-28 — Button icons at native 4K (.41, ps5/evidence/m8/shots/)

A native 3840×2160 crop of the prompt bar (new `shot.txt` tool) shows the L-stick icon ~70 px tall with 2–3 px soft
edges: the 50×50 glyph-sheet cells magnified with linear filtering. The HUD is drawn at output resolution, so the
upscaler is not involved. A 4× glyph sheet and procedural PlayStation stick icons are being built.
