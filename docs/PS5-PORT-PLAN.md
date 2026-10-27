# Savage Wind on PS5: port plan

Status: planned, not started (2026-10-27). Nothing in this file has run on a console yet; every number below is
either a cited measurement from another project or an explicit target.

## Goal

Savage Wind as a native PS5 homebrew title, built from the user's own game files, with:

| Target | Definition of done |
|---|---|
| **4K output** | VideoOut scanout at 3840×2160; the 3D scene rendered at native 4K or upscaled to it (FSR 1 / FSR 3.1). |
| **120 fps output** | 119.88 Hz presented cadence in play: real frames + PS5_MihawkGen generated frames. |
| **VRR** | Retail VRR title metadata; console log shows `VRR(peg:120 range:48 - 120)`; the TV reports VRR. |
| **Frame generation** | `../PS5_MihawkGen` neural method, one generated frame between two real ones (60 → 120). |
| **Input** | DualSense (buttons, sticks, triggers, gyro, rumble), USB keyboard and USB mouse (mouse camera), hot-plug. |

Non-goals: store/retail packaging, shipping any game file or generated game code, a software-rendered fallback,
DLSS, the Metal backend, game version 1.8.2.

## Sources and baseline

| What | Where | State used by this plan |
|---|---|---|
| Savage Wind | this repo, branch **`origin/test`** | 19 commits ahead of `main` (local checkout is `main`): FSR 3 frame generation (`runtime/nvn/vulkan/framegen.cpp`), desktop VRR, DLC. Port from `test`, not `main`. |
| Motion vectors + TAA | PR #194 (`mvec-taa`) | Open. Per-object + camera motion vectors, jitter, TAA. Tested on 1.8.2, Linux, RTX 3080 Ti only. |
| FSR 3.1 upscaler | PR #195 (`fsr3-upscaler`) | Open, depends on #194, **conflicts with current `test`** in `Makefile`, `runtime/hle/framepace.c`, `third_party/FidelityFX/ffx_compat.h`. Same test scope as #194; 1.9.0 and Windows untested. |
| Toolchain, libc, crt | `../forever/PS5_PayloadSDK` (`.downloads/ps5-payload-sdk.zip`) | Prospero clang, `-femulated-tls`, custom rtld (`crt/rtld_so.c`). |
| Vulkan driver | `../forever/PS5_Mesa` RADV + `../forever/PS5_Vulkan/tools/radv-link.sh` | RADV static archive linked whole into the eboot; VideoOut WSI through `VK_KHR_display` (`PS5_Mesa/src/vulkan/wsi/wsi_common_videoout.c`). |
| VRR WSI patch | `../forever/ps5-proton/ps5-wow/ps5/radv/patches/0003-videoout-vrr-idle-floor.patch` | Console-qualified in Forever 01.000.021 (`../forever/WORKSPACE.json`): VRR 119.88 Hz, peg 120, range 48–120. |
| Static libraries | `../forever/pacbrew-repo` | Has `ffmpeg`, `zlib`, `libvpx`, `SDL3`, `SDL3-input`. **No shaderc/glslang package.** |
| Reference port | `../PS5_vkQuake` | Native Vulkan title on RADV: build/link/package/deploy recipe, `VK_KHR_display` surface (`platform/ps5/ps5_window.c`), AudioOut (`src/audio_ps5.cpp`), pad (`src/input_ps5.cpp`), evidence gates (`docs/PLAN.md`). Measured 119.88 fps in its demo loop on RADV. |
| Keyboard / mouse | `../forever/ps5-proton/ps5-wow/ps5/title/input.c` | Working `sceKeyboardOpen/Read/ReadState`, `sceMouseOpen/Read`, `scePadRead` record layouts. |
| Frame generation | `../PS5_MihawkGen/framegen/` | C++ Vulkan compute API (`framegen/include/framegen.h`), neural weights `framegen/models/network.fgw` (1.7 MB). README claims 2.2 ms at 4K on a base PS5; no captured run artifact in the checkout. |

Pins at the time of writing (consume these read-only; `../forever` is shared by other projects and is never edited
from this port): Savage Wind `origin/test` `6572c35483`, PR #194 `35efe256e4`, PR #195 `5039b30407`;
PS5_PayloadSDK `fdb2763789`, PS5_Vulkan `2cf7597741`, PS5_Mesa `f10c4bac9b`, pacbrew-repo `2e3eaf9c5f`,
ps5-wow `bdb6e38f26`, PS5_MihawkGen `c57232a9f1`, PS5_vkQuake `19b90239a3`. `ps5/deps.lock` records these and the
build refuses a mismatch.

## Decisions

**D1. Base branch.** Branch `ps5` from `origin/test`. Rebase PR #194/#195 onto it only after they merge upstream, or
cherry-pick them in M8 if they are still open. PS5 code must not break the desktop builds.

**D2. Layout, following vkQuake.** New files only, compiled *instead of* desktop files; no `#ifdef` forest in shared
code:

```
runtime/platform/ps5.c          plat_* (memory, clock, threads, faults, paths)
runtime/ps5/window.c            VK_KHR_display surface, output-mode/VRR policy, suspend points
runtime/ps5/input.c             scePad / sceKeyboard / sceMouse -> runtime/hle/input.c's state
runtime/ps5/audio.c             SceAudioOut ring + worker, replacing SDL audio streams
runtime/ps5/sdl_shim.{c,h}      only the SDL3 calls the kept code still makes (vkQuake rule: a symbol needs a caller)
runtime/nvn/vulkan/mihawk.cpp   PS5_MihawkGen backend behind vk_fg_generate
ps5/                            build-title.sh, link script, sce_sys/param.json, deploy/run wrappers, evidence/
```

`make PLATFORM=ps5` (or `ps5/build-title.sh`) selects them. Every PS5 compile goes through `prospero-*`; reuse
vkQuake's toolchain canary (an emulated-TLS symbol must exist in a target object, `PS5_vkQuake/docs/FINDINGS.md`).

**D3. Vulkan: RADV, not ps5vk.** Savage Wind needs Vulkan 1.3 `dynamicRendering` + `synchronization2`, storage images
and depth/stencil; ps5vk was a 1.0 device without stencil or storage images when vkQuake measured it in 2026-09
(`PS5_vkQuake/docs/FINDINGS.md`). RADV runs vkd3d-proton (which requires those features) in Forever, and runs vkQuake.
Apply the Forever patch series (`0001` HDR10/immediate, `0002` cache DB, `0003` VRR) to the RADV build.

**D4. Display and VRR.** Surface = `vkCreateDisplayPlaneSurfaceKHR` on the single 3840×2160 VideoOut mode (vkQuake's
`ps5_window.c` pattern). `param.json`: `attribute3 = (attribute3 & ~0x80000) | 0x40040`. vkQuake and MihawkGen's
FrameGen Lab both use `0x80040`, which **turns system VRR off** (fixed 120 Hz) — do not copy theirs. Present mode
FIFO; with VRR the panel follows the presents between 48 and 120 Hz. Replace `runtime/window_size.c`'s desktop mode
list with "4K, 119.88 Hz if granted, else 59.94 Hz".

**D5. Game code is built on the user's PC, never shipped.** The project's rule is that no build contains game code,
generated C included (`docs/SAVAGE-WIND.md` "Reglas legales"). The PS5 has no compiler, and `runtime/gamebuild.c`
spawns one. Flow:

Here, **game code** means the user's Switch ExeFS executables (`main`, `sdk`, `subsdk0`, etc.), not RomFS assets.
Savage Wind's existing generator disassembles their ARM64 instructions and writes equivalent C; the PS5 PC helper
cross-compiles that C for the PS5's x86-64 Zen 2 CPU and links it with the open-source runtime into `eboot.bin`.
This is a one-time operation per game build id. Generated C and PS5 objects are cached on the user's PC; a Savage
Wind update with the same supported game version only relinks the eboot instead of regenerating/recompiling it.

1. The user supplies ExeFS and a complete merged RomFS, manually or through the NSP payload (D11).
2. The PC helper generates C from ExeFS, cross-compiles it for the PS5 target, and caches the generated C/objects.
3. It links those objects with the shipped runtime into the user's signed `eboot.bin` and uploads it to
   `/data/homebrew/<TITLE_ID>/`.
4. The merged RomFS remains as **`/data/savagewind/game/romfs/`**; current `runtime/romfs_image.c` reads that folder
   in place with a persistent descriptor cache. `romfs.bin` is a legacy fallback and is not produced by this port.
5. At startup the title checks its linked game-code build id against `runtime/game_versions.h` and validates the
   RomFS version/file count.

**Data outside the title folder.** Everything that is the user's, or costly to rebuild, lives under `/data/savagewind/`
(decided 2026-10-27), so installing a new title build — which replaces `/data/homebrew/<TITLE_ID>/` — never erases it:

```
/data/savagewind/
  game/      romfs/ (merged base + update), aoc*/ (DLC)
  save/      saves
  cache/     SPIR-V and VkPipelineCache
  savagewind.ini
  logs/
```

The title folder holds only what a build ships: eboot, `sce_module/`, `sce_sys/`, MihawkGen's `network.fgw`. Because
the game code is linked into the eboot (D5 step 2), a title update means relinking it on the PC; the update package
therefore ships the runtime objects and the PC-side link step, and the title refuses to start with a clear message
when its eboot carries no game code. Files the title creates under `/data` are owned by the title's uid; it sets
directories 0777 and files 0666 so FTP can read and replace them (ps5-homebrew-title-ftp-transaction finding).

Loading: a title cannot load its own modules at runtime (PS5_vkQuake/PS5_Vulkan measurement), and a 256 MiB `.text`
loads and runs in the eboot (S3, `ps5/FINDINGS.md`), so the game code is linked into a per-user `eboot.bin` on the PC
(the PS5 package ships the runtime as objects plus the link step, never game code). Supported game version:
1.9.0 only.

**D6. Shaders.** Maxwell → GLSL translation stays at runtime; GLSL → SPIR-V needs shaderc/glslang on the console. Build
glslang + SPIRV-Tools + shaderc for Prospero from pinned upstream sources in this repo's `ps5/deps/` (not in
`../forever/pacbrew-repo`); vkQuake's `src/locale_shims.c` already supplies the `_l` locale functions glslang
needs. RADV's ACO compiles pipelines on-device (as in Forever). Persist Savage Wind's
SPIR-V cache and a `VkPipelineCache` under `/data/savagewind/cache/`; first-start precompile via the existing
`runtime/nvn/precompile.c`.

**D7. Frame generation: MihawkGen neural, 2×.** Plug into the seam `test` already has: `vk_fg_generate`
(`runtime/nvn/vulkan/exec.c`) is called before present and its frame is presented first
(`runtime/nvn/vulkan/device.c`, "presents two frames for each the game draws"). That seam currently drives `test`'s
AMD FSR 3 *frame interpolation* (`framegen.cpp`); add a frame-generation backend choice (`fsr3-fi` | `mihawk`),
default `mihawk` on PS5. This is independent of the upscaler setting (FSR 1, or PR #195's FSR 3.1 *upscaler*), which
only decides the 3D scene's internal resolution before MihawkGen sees the final 4K frame:

- API (`framegen/include/framegen.h`): `fg_context_create` with the 4K output size and `slots` = frames in flight,
  `fg_context_load_network`, then per real frame `fg_record_generate(ctx, cmd, slot, FG_METHOD_NEURAL, &pair, out)`;
- caller-owned synchronisation: the generator records no barriers around its inputs/output. Savage Wind transitions
  both colour inputs to `SHADER_READ_ONLY_OPTIMAL` and the output to `GENERAL` with writes visible to compute, then
  makes the output's compute writes visible to the blit; a slot is reused only after its fence signalled;
- inputs: previous and current final colour (HUD included); MihawkGen's still-region gate keeps static HUD exact;
- `t = 0.5`, output `R8G8B8A8_UNORM`, then blit to the swapchain image;
- reuse only MihawkGen's API, contract and GPU-timestamp method (`fg_context_set_profile`); not the WoW/vkd3d bridge.
- weights loaded from the title folder (`network.fgw`); missing/invalid weights disable FG with a logged reason;
- off in menus, the pause screen and on camera cuts (reuse `test`'s existing rules);
- later A/B: `FG_METHOD_MVWARP` (0.44 ms) / `FG_METHOD_MVSPLAT` (4.7 ms) once PR #194's vectors exist; hudless +
  HUD-overlay variant if moving HUD elements smear.

**D8. Input: native backends, not SDL.** `pacbrew-repo/SDL3-input` lacks keyboard/mouse, drops short pad transitions
(`scePadReadState`) and exposes no motion data (`SDL3-input/PORT_STATUS.md:30-39`). Write `runtime/ps5/input.c`
against `scePad`/`sceKeyboard`/`sceMouse` (record layouts from `ps5-wow/ps5/title/input.c`). These are backends
only: they fill the same host state `runtime/hle/input.c`, `bindings.c` and `motion.c` read today, so the existing
Switch-controller mapping, rebinding and settings stay the single mapping. Do not wire PS5 input to the game's HID
services directly; that would create a second mapping to maintain.

- Native DualSense is the chosen gamepad backend (user preference), independent of any SDL2 libraries used elsewhere.
  Use `../ps5-native-gamepad-input-research/include/ps5_pad.hpp` and `CONTROLLER-DATA.md` as the ABI/lifecycle reference.
  Open for the signed-in user, with exactly one owner of each pad handle. Drain `scePadRead(handle, records, 64)`
  oldest-first and publish the final valid sample as current; do not also call `scePadReadState` in the same cycle.
- Explicitly configure motion with `scePadSetMotionSensorState`, tilt-correction/deadband controls and orientation
  reset as illustrated in `examples/05-motion-touch.cpp`; check each result instead of assuming sensor availability.
  Process sensor time from each sample's microsecond timestamp, not rendering/frame-generation cadence.
- Choose one orientation source: the supplied pad quaternion, normalized and coordinate-converted, OR raw gyro/accel
  fused by the existing motion algorithm. Never feed the supplied orientation through raw-sensor fusion again.
  Preserve BOTW six-axis output conventions. Research documents gyro rad/s and acceleration G, but physical axis,
  sign and response validation is still required on PS5; do not guess the conversion.
- On disconnect, interception or connection-generation change, neutralize buttons/triggers/sticks (raw stick center
  128), use identity orientation (`w=1`), and clear sample timing, calibration and cached motion history. Stop rumble
  and restore feedback state on close. Verify damage, weapon-hit and explosion vibration separately; prompts use
  PlayStation icons. Physical aim, vertical orientation, tilt-shrine and reconnect tests gate motion support.
- USB mouse: relative deltas → mouse camera, buttons → bindings; no cursor capture concept needed;
- USB keyboard: scancodes → existing default bindings; reopen on hot-plug (poll open state each second);
- ImGui menus driven by pad navigation plus keyboard/mouse.

**D9. Audio.** SceAudioOut 48 kHz stereo S16 ring + worker (vkQuake `src/audio_ps5.cpp`), replacing the SDL streams
in `runtime/services/audio.c`. Movies keep FFmpeg from pacbrew.

**D10. Licence.** Savage Wind is MIT; MihawkGen `framegen/` and vkQuake are GPL-3.0-or-later. The PS5 build that links
MihawkGen is distributed under GPL-3.0-or-later with all notices (MihawkGen `THIRD_PARTY_NOTICES.txt`, Mesa, AMD
FidelityFX). Keep MihawkGen and other GPL code out of the desktop build so its licence is unchanged.

**D11. Drop-in NSP/XCI import: one-shot nstool payload** (requested 2026-10-27). The user uploads their packages
and their own `prod.keys` (optional `title.keys`) to `/data/savagewind/import/`. Supported input combinations:

| Inputs | Acceptance |
|---|---|
| Base XCI + 1.9.0 update NSP | Locate the base Program NCA in the XCI secure partition and merge the matching update. |
| Base NSP + 1.9.0 update NSP | Locate matching base/update Program NCAs and merge them, loading both embedded tickets. |
| One NSP labelled 1.9.0 | Accept only if its contents provide a complete usable 1.9.0 game, including all required base content. |

A lone update-only NSP is incomplete: display "Base game missing. Add the Breath of the Wild base NSP or XCI."
An XCI may also contain updates; inspect its internal content inventory rather than assuming every XCI is base-only.
The title starts an embedded `savagewind-import.elf` through local elfldr; the payload runs as root, exits after
the import, and never owns VideoOut. XCI is part of the planned importer, but requires its own extraction qualification.

- Tool: `../nstool2` (MIT, pinned at `7e0a49b61a`), cross-built into the payload. It reads NSP/PFS0, NCA and tickets,
  and applies an update to its base: `--basenca <base program NCA>` extracts the update Program NCA against the base,
  producing the 1.9.0 ExeFS and merged RomFS.
- Outputs: `/data/savagewind/game/romfs/` (+ `aoc*/` DLC directories) and a temporary
  `/data/savagewind/import/work/exefs/`. The PC helper fetches only ExeFS, generates and links the user's
  `eboot.bin` (D5), uploads it to the title folder, then removes the temporary ExeFS. No monolithic `romfs.bin` or
  second 15 GiB copy is made.
- Validate real contents, never package filenames. Parse CNMT type/title/version/content records and require base
  application `01007EF00011E000`, matching update `01007EF00011E800` v1114112, and a Patch application ID referring
  to that base. Resolve referenced NCA IDs, sizes and hashes; reject missing/ambiguous content. Test decryption for
  both base and update before a full merge, loading both tickets or matching user-supplied title-key mappings.
- Validate extracted output independently: `main` must be NSO and match the supported 1.9.0 build-ID prefix
  `cd57b23fa4bbad65` in `runtime/game_versions.h`; RomFS `System/Version.txt` must say `1.9.0`, with at least the
  existing 95%-of-41,062-file threshold. Metadata alone is not proof of complete content or bootability. Stage under
  `import/work/`; keep existing working game data unchanged until extraction and validation succeed.
- Keys: parsed in the payload's memory only, never logged, cached or shipped. `prod.keys` is deleted after a
  successful import by default; NSP deletion is a separate user choice. No key, title key or key-derivation constant
  is committed to this repository.
- Root payload does not imply unlimited disk speed. Existing payload measurements are ~13–35 MiB/s depending on
  write size; measure this importer with large sequential writes on .41. It avoids the title's per-launch write
  budget, but SSD/pwrite cost can still dominate.
- The launcher replaces NSP/XCI "unusable package" with Import. `.nsz` and `.xcz` remain unsupported. If local elfldr
  is absent, show "Import service unavailable. Load the console payload loader and retry." Never silently fall back.

**TV setup/progress screen.** The native title UI stays responsive while the root payload works. First show the
detected game, base/update sources, actual version and missing prerequisites; start extraction only after this
preflight passes. The payload publishes bounded, versioned progress/status records under
`/data/savagewind/import/` by temp-file + atomic rename. Include an import session ID so stale records cannot be
mistaken for the current run. The UI polls those records; no per-file log flood or second display owner.

Stages: **Checking packages → Checking keys → Extracting ExeFS → Merging/extracting RomFS → Verifying → Ready for
PC code build**. Show bytes processed/total and percentage when the total is known; use an indeterminate indicator
during scans rather than fake percentages. Show file counts for final verification. "Ready for PC code build" is
not "Ready to play": the PC helper still must generate/link and upload the user-specific eboot.

Failure messages name the file and corrective action: wrong game; base missing; update missing/wrong version;
missing key or ticket; decryption failure (not automatically blamed on keys); missing/corrupt NCA; unsupported
format; insufficient free space; payload failed/disconnected. Never show key values. Keep the error on screen until
acknowledged. Retry revalidates inputs; cancel is cooperative between bounded chunks, never kills a process halfway
through publishing the accepted game. Preserve the existing installation and remove only session-owned staging.
- **Policy change to decide:** `docs/SAVAGE-WIND.md` says Savage Wind never decrypts anything. This payload decrypts
  with the user's keys, changing that rule and the project's legal position. It remains behind a build option until
  the maintainers approve and rewrite that policy.

**D12. `/data/savagewind` access: direct reads, root payload for writes.** No Lapy dependency and no title credential
mutation. ShadowMountPlus is a prerequisite: on .41 it lets the title read `/data/savagewind`, and direct `pread`
measured 111–133 MiB/s sequential and 124–171 MiB/s for four concurrent random 64 KiB streams. Keep the hot RomFS
path direct. A resident `savagewind-storage-broker.elf`, started once per boot through local elfldr, handles:

- saves, settings, logs and shader/pipeline-cache writes, so the title never reaches its 20 → 2 MiB/s write budget;
- only paths beneath a pre-opened `/data/savagewind` directory fd, rejecting `..`, absolute paths, symlinks, hard
  links and unknown operations; atomic write = same-directory temp file + fsync + rename;
- a versioned loopback protocol on Savage-Wind-only port 9142, per-connection threads and a small connection pool.
  It cannot replace or stop WoW's unrelated broker on 9141. One Savage Wind broker owns its port; a newer embedded
  copy may ask an older version to exit.

The broker retains bounded `pread` for diagnostics, but it is not the runtime RomFS path: a request currently opens
and closes the file, unlike `romfs_image.c`'s persistent descriptor. If direct `/data` access is absent, the title
stops with one actionable message to load ShadowMountPlus; it does not route all gameplay reads through loopback.
The broker's same 3 GiB write benchmark on .41 must show no cumulative 2 MiB/s collapse before acceptance. Root alone
is not proof: direct root-payload measurements reached a steady 36 MiB/s, while Lapy-elevated title writes measured
143 MiB/s in MihawkOS.

## Frame budget at 4K / 120

Real frames at 60 fps, one generated frame each → 119.88 Hz. Per real frame:

| Item | Budget | Source / status |
|---|---|---|
| Real-frame interval | 16.68 ms | 59.94 Hz base |
| MihawkGen neural, 4K | ~2.2 ms GPU | MihawkGen README, base PS5 (unverified here) |
| Blit/present overhead | ~0.3 ms | estimate, to measure |
| **Game render + upscale + HUD** | **≤ ~14 ms GPU** | to measure on PS5 |
| Game CPU (recompiled code + HLE + NVN translation) | ≤ 16.68 ms per frame | desktop: steady 60 on Ryzen 5 5600X (`README.md`); PS5 Zen 2 unknown |

Consequences:

- Native 4K rendering is unlikely to fit next to FG on this GPU; plan on internal 1440p–1800p plus FSR 1 (exists) or
  FSR 3.1 (PR #195; its author reports 3.19 ms total GPU frame time vs 2.69 ms with FSR 1, 1440p→4K, RTX 3080 Ti;
  PS5 cost unmeasured). `ps5-fsr4` measured 10.86 ms for 1080p→4K on PS5 and does not fit.
- With VRR, a base frame rate between 48 and 60 still yields 96–120 Hz output; below 48 the TV leaves VRR range.
- FG adds latency (one real frame is held). Measure end-to-end input latency, not just fps.

## Milestones

Each milestone closes with vkQuake's four gates: host checks pass; the target build is from the Prospero toolchain;
it runs on the console with klog captured for the whole run; the capture is committed under `ps5/evidence/<step>/`
and replayable. No claim without an artifact.

### M0 — Feasibility spikes (blocking; no gameplay code)

| Spike | Question | Pass |
|---|---|---|
| S1 Guest memory | Can the title reserve `[0x7000000000, 0x7320000000)` at fixed addresses, commit/reset/protect pages there (`runtime/rt.h`, `runtime/platform.h`)? | Probe eboot maps, writes, protects and resets the whole range; logs the page granularity. If fixed mapping is refused: design a relocated guest base before anything else. |
| S2 Threads and faults | 256 MiB pthread stacks × BOTW's thread count (`runtime/kernel.c:380-412`), `sigaction` fault recovery for lazy commit / watchpoints. | N threads with 256 MiB stacks run; a guarded-page fault is caught and resumed. |
| S3 Game-code module | Can the 1.9.0 generated module (size measured from `origin/test`'s generator first; the only recorded figure, 198.8 MB, is 1.8.2) be built for Prospero and loaded at runtime? | One generated module + runtime loads via SDK rtld and calls in; else decide per-user eboot link. Measure size and load time. |
| S4 Vulkan feature set | RADV device creation with Savage Wind's exact requested features/extensions (`runtime/nvn/vulkan/device.c`). | Device created; list of missing optional features recorded. |
| S5 shaderc | glslang/shaderc cross-built and compiling translated shaders on the console. | Pinned-same-version compile; SPIR-V passes `spirv-val`, its disassembly matches the host compile, and the shader renders correctly in a draw test. |
| S6 CPU headroom | Time one hot generated function set on the console vs. desktop (Zen 2 @3.5 GHz, emulated TLS). | Ratio recorded; feeds the go/no-go for 60 fps base. |


Status 2026-10-27 (`ps5/FINDINGS.md`, console runs 3–11): **S1 go** — `mmap` refuses the window (flexible budget),
`sceKernelReserveVirtualRange` + fixed direct-memory commits work at the exact addresses, 16 KiB pages. **S2 go** —
256 MiB stacks as reserved addresses with fault-time 1 MiB direct-memory commits, 48 threads. **S3 go** — static link.
**S6 measured** on the console; desktop comparison with generated code pending. **S4, S5 not started.**
Exit: written go/no-go per spike in `ps5/FINDINGS.md`.

### M1 — Title shell
Title ID **PPSA90212** (`ps5/sce_sys/param.json`, chosen 2026-10-27). `param.json` with D4 metadata; eboot boots,
prints build identity, exits without
the SIGSYS-on-exit crash vkQuake recorded (`PS5_vkQuake/docs/FINDINGS.md`).
**Done when:** launch → identity in klog → clean exit, three runs.

### M2 — Platform layer
`runtime/platform/ps5.c`, paths (`/data/savagewind/` for game data, saves, cache, `savagewind.ini`), SDL shim,
`__cpuid_count`-based CPU checks (vkQuake finding: `__get_cpuid(7)` returns zeroes), PS5 build of the runtime
objects. Fix `runtime/gamelib.c`'s CPU check to a fixed `x86-64-v3` on PS5.
**Done when:** runtime starts, reserves guest memory, runs its linked game code, opens
`/data/savagewind/game/romfs/`, and reaches the game's first NVN call (no rendering yet).

### M3 — Rendering on VideoOut
RADV link, display-plane surface, swapchain at 3840×2160, FIFO, 119.88 Hz request; shaderc + caches (D6);
`sceAgcSuspendPoint` for rest mode (as `ps5-wow/ps5/title/main.c`).
**Done when:** title screen and Great Plateau render; klog shows the 4K surface, mode, and VRR lines from D4.

### M4 — Input
D8 complete. **Done when:** a scripted klog of each device: DualSense buttons/sticks/triggers/gyro aim/rumble; USB
mouse camera and clicks; USB keyboard movement and menus; unplug/replug each device mid-game.

### M5 — Audio and movies
D9. **Done when:** 10-minute run with zero AudioOut errors/underruns logged, intro movie plays; user listening check.

### M6 — Performance to a 60 fps base
Measure CPU/GPU per frame on the Great Plateau; tune internal resolution / FSR 1 defaults; shader precompile on
first start; fix hitches. **Done when:** ≥ 95 % of real frames within 16.68 ms over a 10-minute route, recorded.

### M7 — MihawkGen 2× and VRR at 120
D7 backend; frame pacing in `runtime/hle/framepace.c` for real + generated presents under VRR.
**Done when:** settled 119–120 Hz presented cadence in play, `gpu_ms` of the generator logged per window, VRR
confirmed on the TV, FG off in menus; A/B vs FG off for latency and artifacts (camera pan, Link running, HUD).

### M8 — Temporal upgrades (after PR #194/#195)
Reuse PR #194's motion vectors, jitter, depth copy and reactive mask and PR #195's FSR 3.1 path; do not re-derive
motion vectors. Open item: take them by upstream merge (preferred) or rebase/cherry-pick the pinned heads, resolving
the three `test` conflicts. Re-qualify on PS5 at 1.9.0: their evidence is 1.8.2, Linux, RTX 3080 Ti only, so CPU cost
(~0.7 ms/frame desktop), GPU cost, VRR/120 Hz pacing with FG, and image quality are all unmeasured here. Then A/B
MihawkGen neural vs `mvwarp`/`mvsplat`, and choose defaults by measured image quality and GPU time.

### M9 — Root storage broker (D12)
Path-restricted payload, versioned client, direct RomFS reads and brokered atomic writes. **Done when:** broker A/B on
.41 covers a cumulative 3 GiB write and save/cache-like atomic replacements without a 2 MiB/s collapse; direct reads
remain unchanged; unavailable/old broker and missing ShadowMountPlus each produce a clear error; path traversal,
intermediate/final symlinks, hard links and cross-root operations are rejected.

### M10 — NSP/XCI import payload and TV setup screen (D11)
nstool2 cross-built at its pin into a one-shot root payload, native TV UI progress/errors, PC helper handoff and
policy decision above. **Done when:** base XCI + update NSP, base NSP + update NSP, and a complete single-package
1.9.0 input each produce the same correct merged ExeFS/RomFS as manual extraction (file hashes). Update-only,
wrong-title, wrong-version, missing/corrupt-content, missing-key, insufficient-space and missing-elfldr cases show
specific actionable errors on the TV without modifying the existing game. Verify real progress, responsive UI,
stale-session rejection, cancel/retry, interruption recovery and the PC-built eboot handoff. No key material appears
in logs/status records; unsupported compressed containers are rejected explicitly.

### M11 — Release
Settings persistence, save/load, DLC, 1-hour soak, README for the setup/import flow, notices (D10), release ZIP.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Fixed guest address range refused or capped | Port blocked | S1 first; relocated guest base is a large `gen.py`/runtime change — decide early. |
| Zen 2 CPU too slow for 60 fps base | 120 Hz only with lower base / longer latency | S6, M6; VRR covers 48–60 base; FG 3× not planned (MihawkGen would rerun the network). |
| GPU: render + FG over budget at 4K | Missed 120 | Internal resolution + upscaler; measure MihawkGen in situ. |
| No shaderc port; on-device shader compile hitches | Stutter | S5; precompile + persistent caches. |
| Game-code module load size/time | Slow start / not loadable | S3; per-user eboot fallback. |
| Total memory (guest + Vulkan + caches + FG images) | OOM | Log memory per milestone; MihawkGen neural images per slot, mvsplat ~70 MB at 1080p. |
| PR #194/#195 diverge or are rejected | No motion vectors / FSR 3.1 | M7 depends only on colour-only MihawkGen and FSR 1. |
| GPL obligations | Licence mismatch | D10; PS5 build only. |

## Not planned

The exploit/kernel side, retail packaging and signing keys, shipping game data or generated code, any edits to
`../forever` beyond reusing its tools and patches (driver changes are requested from those projects by name).
