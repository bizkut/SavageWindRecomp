# Savage Wind on PS5: port plan

Status 2026-10-29, all on console .41 (`ps5/FINDINGS.md`): M0 S1–S5 go; M2 boot from the user's packages; M3 4K
rendering on VideoOut with VRR; M4 DualSense/USB keyboard+mouse code (hands-on pad/USB checks deferred by the user);
M5 audio (movie playback untested); M6 benchmark 98–100 % on time (10-minute played route deferred by the user);
M7 MihawkGen 2× at 119.3 Hz; M8 FSR 3.1/TAA integrated (FSR 4 measured too slow at 4K); M9 broker; M10 package reads;
sharp text/UI and the user-generated UI HD pack. Open: M11 release. Other numbers in this file are explicit targets.

The PS5 package carries `pic0.dds` and `pic1.dds`, both 3840×2160 single-mip BC7_UNORM, composed from the original
`docs/icon/icon.svg` on a dark background. `background-source.png` is the same composition used by the runtime
startup screen; `/data/savagewind/splash.jpg`, `.jpeg`, or `.png` overrides it there. `startup_splash = 0` disables
that runtime screen, not system artwork. One-shot `[startup]` timestamps measure each startup milestone. The runtime
screen joins its presentation worker before the first game presenter takes the GPU lock. System display of `pic1.dds`
and the runtime screen require console visual qualification; asset validation alone does not establish that behavior.
The source PNG SHA-256 is `d835977bb4d6a2c1e0371e4ec3218008a8a0230cbb008330a2cd204160f4c6bc`;
both DDS files hash to `94f5d2464cfbdd0e11b88f7263dcad0115ed2eb98d522b4bda08dfa9313cf86e`.

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

### Upstream PRs and issues (triage 2026-10-27)

Open upstream PRs/issues on `Revivedx/SavageWindRecomp` that affect this port. Read from the PR descriptions and issue
threads; diffs were not applied or measured here. Our `ps5` branch contains `origin/test` `6572c35483` (latest).
Policy (D1): take a PR by upstream merge into `test` when possible; cherry-pick only at the milestone named, and only
with a PS5 A/B receipt. No PR is assumed beneficial on the PS5 until measured there.

| PR | Change | Notes for PS5 | When |
|---|---|---|---|
| #154 | Render-target view-format lists, keeps DCC on AMD | Measured on RADV iGPUs; enabled by default only on iGPUs, so the PS5 (discrete-like RDNA2) needs an explicit enable and A/B. | M6 |
| #155 | Image barriers between passes instead of a full barrier | Avoids a per-pass L2 flush on RADV. Overlaps #189 in `runtime/nvn/vulkan/exec.c`. | M6 |
| #153 | NaN-aware min/max/saturate, ~18 % fewer shader instructions | Measured on Radeon 660M/RADV; compatible with #154/#155. Changes shader translation, so shader caches (D6) are invalidated. | M6 |
| #189 | Recorder thread for driver calls, barrier before the window pass | Changes submission/present order: re-check VideoOut/VRR pacing (D4, M7). Overlaps #155, #128, #111, #83. | M6, after #155 |
| #214 | One-off readback waits only for its own submission | Hitch fix in `exec.c`; tested on Windows/RTX only. | M6 |
| #83 | Retire command records when the game overwrites their lists (#74) | Command-memory lifetime; matters for the PS5 memory budget. | M2/M3 |
| #190 | Quick selectors without black entry frames at high FPS limits (#173) | We run a 60 fps base with FG to 120; check both FG on/off. | M7 |
| #128 | Descriptor sets kept across submissions | Draft; no measurable gain on RADV/RTX per its author. Only if M6 shows descriptor CPU cost. | M6 (conditional) |
| #194, #195 | Motion vectors/TAA, FSR 3.1 | See table above and M8; #195 conflicts with `test` in `runtime/hle/framepace.c`. | M8 |

Not for the PS5: #215 and #200 (Windows stack commit / CPU affinity), #197 (Intel iGPU sparse heap; its sparse path
crashes on AMD), #111 (NVIDIA DLSS), #212 (Mali/aarch64), #213 (Windows generator logging), #33/#40 (CI/layout).

| Issue | Status upstream | PS5 action |
|---|---|---|
| #144 arrows fall faster above 30 fps | Fixed; fix `85597d6` is in our branch. | Verify at the 60 fps base (M6). |
| #164 gyro flips past vertical | Fixed by `85597d6`, in our branch. | Covered by D8 gyro tests (M4). |
| #98 ultrawide loading screens | Fixed by `85597d6`, in our branch. | None (16:9 TV). |
| #161 cucco jump above ~90 fps | Open, cause unknown. | Base stays 60 fps; recheck if a higher base is ever offered. |
| #173 black flicker on weapon selector | Open; fix is PR #190. | M7. |
| #129 pixelated particle halos without FSR | Thread says fixed on `test` by PR #138; that commit was not found in our history. | Verify before M3 sign-off. |
| #95 dim HDR | Thread says fixed on `test` (HDR10 presentation); commit not found in our history. | HDR is out of scope until M3 runs SDR; recheck then. |
| #59, #182, #219 AMD texture/geometry artifacts | Open, no root cause (#182: disabling anisotropic filtering helps one symptom). | Watch on RADV in M3; capture before/after AF if seen. |
| #146 missing rumble | Partly fixed (PR #178, damage rumble); weapon-hit/bomb rumble open. | D8 rumble test lists which events rumble. |
| #136, #220 gyro devices | Controller-backend specific (Steam Input, GameSir). | None; DualSense goes through scePad (D8). |
| #204 `sr failed: -2` at `exec.c:310` | Open, validation-layer report on Vega 8. | Watch in M3 with validation. |
| #188 white screen after intro | Thread: oversized DLC map files exceed size-table limits; the RasterizerDiscard log is harmless. | Relevant to imported DLC (D11): test DLC on/off in M11. |

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

Other displays (unmeasured): S4 on .41's 4K TV saw one display, one plane and two modes, both 3840×2160
(119.88/59.94 Hz). A 1080p TV has not been tested. The window code enumerates the modes the display reports and
picks the largest extent, preferring 119.88 Hz; the swapchain and internal resolution follow that extent. On a 60 Hz
display, frame generation is off.

**D5. Game code is built from the user's files, never shipped.** Today on the user's PC; M12 moves it onto the console
(requested 2026-10-29) so the public release needs no PC. The project's rule is that no build contains game code,
generated C included (`docs/SAVAGE-WIND.md` "Reglas legales"). The PS5 has no compiler, and `runtime/gamebuild.c`
spawns one. Flow:

Here, **game code** means the user's Switch ExeFS executables (`main`, `sdk`, `subsdk0`, etc.), not RomFS assets.
Savage Wind's existing generator disassembles their ARM64 instructions and writes equivalent C; the PS5 PC helper
cross-compiles that C for the PS5's x86-64 Zen 2 CPU and links it with the open-source runtime into `eboot.bin`.
This is a one-time operation per game build id. Generated C and PS5 objects are cached on the user's PC; a Savage
Wind update with the same supported game version only relinks the eboot instead of regenerating/recompiling it.

1. The user keeps their packages (base NSP or XCI + 1.9.0 update NSP, optional DLC) and `prod.keys` in
   **`/data/savagewind/import/`** on the console, and has the same packages on the PC.
2. The PC helper extracts only ExeFS from the packages (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`; tens of MB),
   checks `main`'s build id, generates C (`tools/gen.py all`), cross-compiles it for the PS5
   (`ps5/tools/build-gamecode.sh`) and caches the generated C/objects.
3. It links those objects with the shipped runtime into the user's signed `eboot.bin` and uploads it to
   `/data/homebrew/<TITLE_ID>/`.
4. **No RomFS extraction** (decided 2026-10-27 after console run 3 measured on-console extraction at 15 MiB/s,
   ~17 min for 14.1 GiB): the runtime reads the merged RomFS from the packages directly. A third
   `runtime/romfs_image.c` backend decrypts and applies the update's patch per read; DLC images likewise
   (`aoc<N>`). Setup on the console is therefore a package scan, not a copy, and needs no second 15 GiB.
5. At startup the title checks its linked game-code build id against `runtime/game_versions.h` and validates the
   packages' CNMT title/version and the RomFS version/file count.

Consequence: the title, not a one-shot payload, now decrypts at every launch with the user's keys, and
`prod.keys` stays on the console. The policy was rewritten accordingly (D11, decided 2026-10-29).

**Data outside the title folder.** Everything that is the user's, or costly to rebuild, lives under `/data/savagewind/`
(decided 2026-10-27), so installing a new title build — which replaces `/data/homebrew/<TITLE_ID>/` — never erases it:

```
/data/savagewind/
  import/    the user's NSP/XCI packages and prod.keys (read in place by the game)
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

**D11. Game data from the user's NSP/XCI packages, read in place** (requested 2026-10-27; revised the same day). The
user uploads their packages and their own `prod.keys` (optional `title.keys`) to `/data/savagewind/import/`.
Supported input combinations:

| Inputs | Acceptance |
|---|---|
| Base XCI + 1.9.0 update NSP | Locate the base Program NCA in the XCI secure partition and apply the matching update. |
| Base NSP + 1.9.0 update NSP | Locate matching base/update Program NCAs and apply the update, loading both embedded tickets. |
| One NSP labelled 1.9.0 | Accept only if its contents provide a complete usable 1.9.0 game, including all required base content. |

A lone update-only NSP is incomplete: display "Base game missing. Add the Breath of the Wild base NSP or XCI."
An XCI may also contain updates and DLC; inspect its internal content inventory rather than assuming it is base-only.

- Library: `../nstool2` (MIT, pinned at `7e0a49b61a`) plus a patch in this repo (`ps5/importer/nstool-streams.patch`):
  base NCA streamed from inside its container (never copied), tickets from streams, XCI filesystem, and two upstream
  fixes (`NcaProcess::getFileSystem` returned null; `ContentInfo` copy dropped `id_offset`).
- **PC:** extract ExeFS only and build the eboot (D5). **Console:** the runtime opens the packages, selects the
  contents by CNMT and serves `romfs_image_pread` from the decrypted, update-patched RomFS stream; DLC as `aoc<N>`
  (index = AOC data id − (program id & ~0xFFF) − 0x1000). Nothing is written under `game/`.
- Validate real contents, never package filenames. Parse CNMT type/title/version/content records and require base
  application `01007EF00011E000`, matching update `01007EF00011E800` v1114112, and a Patch application ID referring
  to that base. Resolve referenced NCA IDs and sizes; reject missing/ambiguous content; load both tickets or matching
  user-supplied title-key mappings. At open, the RomFS `System/Version.txt` must say `1.9.0` with at least 95% of
  41,062 files. Whole-NCA hashing is a one-time check, not a per-launch cost.
- Keys: parsed in memory only, never logged, cached or shipped; no key, title key or key-derivation constant is
  committed. `prod.keys` must stay on the console because the title decrypts at every launch.
- `.nsz` and `.xcz` remain unsupported and are rejected by name and content.

**Retired: one-shot extraction payload.** `ps5/importer` (root elfldr payload extracting merged RomFS to
`game/romfs/`) and its TV progress title `ps5/title/import_ui.c` work end to end (host: 41,067/41,067 files
byte-identical to stock nstool; console run 3 on .41: preflight, confirm, ExeFS, RomFS, cancel), but extraction ran at
15 MiB/s (~17 min). It is kept only until the direct-read backend has qualified on the console, then removed; its
package-selection code moves into the shared package library.

**TV screen.** The title shows the detected game, base/update/DLC sources, actual version and missing prerequisites
before starting, and keeps a specific error on screen until acknowledged: wrong game; base missing; update
missing/wrong version; missing key or ticket; decryption failure (not automatically blamed on keys); missing/corrupt
NCA; unsupported format. Never show key values.
- **Policy (decided by the fork's maintainer, 2026-10-29):** the "never decrypts" rule is rewritten (README): on a PC
  Savage Wind reads extracted files; the PS5 port reads the player's own NSP/XCI with the player's own keys on the
  player's console, decrypting in memory per read; nothing decrypted is written out, uploaded or shipped, and the
  project still provides no game files, keys, tools or instructions. Package reading is no longer gated on approval.

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
it runs on the console with klog captured for the whole run; the klog and text results are committed under
`ps5/evidence/<step>/`. No claim without an artifact. Console captures that show the game (screenshots, PNG crops,
frame dumps) are never committed: they stay in `~/savagewind-private-evidence/` (`.gitignore` blocks
`ps5/evidence/**/*.png`); FINDINGS cites their measurements.

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
**S6 measured** on the console (micro-loops); with the game on the title screen the render thread takes 1.1 ms and
the GPU 1.8 ms of a 16.68 ms frame (M6 measure1); the guest threads' share and in-game numbers await the M6
benchmark. Desktop comparison with generated code pending. **S4, S5 not started.**
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
**Done when:** runtime starts, reserves guest memory, runs its linked game code, opens the game's ExeFS and RomFS
from the packages in `/data/savagewind/import/` (D11), and reaches the game's first NVN call (no rendering yet).

The M2 link uses desktop `hle/vi.c` for guest-side VI objects, but does not create a PS5 display surface. Its
`nvnBootstrapLoader` handler logs the requested entry point and exits before the desktop NVN implementation runs.
Relative to desktop `RT_SRC`, the PS5 entry, paths and static-game-library files replace `main.c`, `paths.c`,
`gamelib.c` and the desktop platform; `gamebuild.c` is PC-only. `imageload.c` and `window_size.c` belong to the
desktop launcher/window; `perfmon.c` and `sampler.c` are optional diagnostics. `hle/input.c`, `motion.c` and
`bindings.c` await M4; `hle/movie.c` awaits M5; `hle/swkbd.c` needs the later UI. `hle/framepace.c` and the
`runtime/nvn/` renderer, command, format, shader and translation files are post-bootstrap M3 work. These exclusions
are a link boundary, not proof that every pre-bootstrap guest path avoids them. Explicit generated-code hooks for
unavailable later subsystems log and fail if reached; missing HLE imports retain the desktop's fallback behavior.

### M3 — Rendering on VideoOut
RADV link, display-plane surface, swapchain at the chosen mode's extent (D4), FIFO, 119.88 Hz request; shaderc + caches
(D6); `sceAgcSuspendPoint` for rest mode (as `ps5-wow/ps5/title/main.c`).
**Done when:** title screen and Great Plateau render; klog shows the 4K surface, mode, and VRR lines from D4.

The M3 link adds the desktop Vulkan backend unchanged in behaviour (`runtime/nvn/*`, `runtime/nvn/vulkan/*` without
`framegen.cpp` and `imgui_glue.cpp`, `hle/nvn_boot.c`, `hle/framepace.c`) on the pinned RADV archive and shaderc.
`runtime/ps5/window.c` answers device.c's SDL window, surface, timer and event calls with the VideoOut display plane
(D4 mode choice; VSync pinned to On/FIFO) and the PS5 half of `window_size.h`. A title has no Vulkan loader, so
`ps5/tools/gen-vk-globals.py` generates a trampoline per command of the RADV header, resolved through
`vk_icdGetInstanceProcAddr`. RADV's zlib (linked whole) serves the runtime too. RADV's PS5 winsys already calls
`sceAgcSuspendPoint` after each submission. The game's controller boundary is linked as on desktop: `hle/npad.c`
(player 1 a connected FullKey controller; the SDK's own Npad calls read hid shared memory that no input service fills,
and run 3 aborted in its controller applet path) and `hle/motion.c` (six-axis states; with no sensors the controller
lies flat and still). Still at a boundary (`runtime/ps5/unavailable.c`, each answering as the desktop does with no
such device or feature): the input devices (player 1 reads nothing pressed) and bindings, button-glyph replacement,
the overlay menu and Savage Wind's Options pages (all M4), movies (M5), frame generation (M7), the performance overlay.

### M4 — Input
D8 complete. **Done when:** a scripted klog of each device: DualSense buttons/sticks/triggers/gyro aim/rumble; USB
mouse camera and clicks; USB keyboard movement and menus; unplug/replug each device mid-game.

USB keyboards and mice (`runtime/ps5/input.c`): the Keyboard (0x0106) and Mouse (0x00a9) sysmodules load once; indexes
0 and 1 of each open for the signed-in user (an index that does not open is tried again each second) and stay open
across unplugging, their records' `connected` flag giving connect / disconnect. Every record is processed oldest-first:
keys (HID usages, modifier bits as Left Control..Right GUI) are SDL scancode events; mouse buttons are SDL buttons;
a poll's motion and wheel, summed over every record of every mouse, are one SDL relative motion and one wheel event.
The klog has one line per device connect / disconnect, a rate-limited line per key or button change, and the mouse's
summed motion once a second while it moves. The Quit question's texts are the platform's (`opts::k_quit_texts`):
on the PS5 both answers close the title, so it asks about closing the game, not "Desktop / Start screen".

**PS5 Video page** in Savage Wind's existing Options pages (`runtime/ui/game_options.mm`, pause > System > Options
and the title screen), enabled with input:
- **Resolution: Auto / 1080p / 4K** replaces Window size and Fullscreen. Auto (default) is the D4 mode chosen at
  start; 1080p and 4K are offered only if the display reports them. Applying it rebuilds the display-plane swapchain
  (M3's surface code); if a live rebuild proves unreliable, the row says it applies at the next start.
- **Frame rate: 30 / 60 / Off**: the existing `frame_rate` setting, limiting real frames.
- **Upscaler** and its mode stay. VSync and HDR rows are removed: FIFO is fixed (D4) and HDR is not planned yet.
- Choices persist in `savagewind.ini` through the broker.
**Done when:** on .41, each row's choice is applied, logged, and restored after a restart; Resolution offers only the
modes the display reports (4K TV now, a 1080p display before M6 closes).

### M5 — Audio and movies
D9. **Done when:** 10-minute run with zero AudioOut errors/underruns logged, intro movie plays; user listening check.

`runtime/ps5/audio.c` answers the SDL audio-stream calls of `runtime/services/audio.c` with one SceAudioOut
main port (48 kHz stereo S16, 256-frame periods) and one worker thread: each stream (the SDK renderer's F32 stereo, an
IAudioOut's S16 1/2/6 channels per movie) is a ring of stereo frames, mixed with its gain every period; streams are told
1024-frame device periods (SDL3's default at 48 kHz) for their queue targets, as on desktop. Other rates are refused
(the service's no-device path), not resampled. The klog has one line per device open and stream, and every 10 s the
window's underruns (frames short), AudioOut errors (last result), dropped frames, deepest queue, and the level at each
stage (what the streams were given, what the mix read from their rings, the resumed streams' lowest gain, the output);
the SDK renderer's own 10 s line (`[audren-sdk]`, always on in the title) adds the voices' data-source commands per
frame and the sink's peak before the gain, so a silent output tells "the game plays no voice" from a lost signal; host
check `make test-ps5-audio`. Movies are `runtime/hle/movie.c` on pacbrew's installed FFmpeg 7.0.1 (`ps5/tools/setup-ffmpeg.sh`
checks the pinned pacbrew-repo revision, the packages' recipe versions and the archives' digest in `ps5/deps.lock`);
with no host RomFS folder, a movie is read from the RomFS image the game opened (`fs_romfs_file` / `fs_romfs_pread`,
an AVIOContext). `runtime/ps5/libc_aliases.S` maps the FreeBSD names those archives use (`_setjmp`, `_longjmp`,
`___mb_cur_max`) onto exported ones.

### M6 — Performance to a 60 fps base
Measure CPU/GPU per frame on the Great Plateau; tune internal resolution / FSR 1 defaults; shader precompile on
first start; fix hitches. Defaults per output resolution: at 1080p, native or supersampled internal resolution
without FSR; at 4K, the resolution/FSR mode the measurement supports. **Done when:** ≥ 95 % of real frames within
16.68 ms over a 10-minute route, recorded, at 4K and on a 1080p display.

**Built-in benchmark (new work, requested 2026-10-28).** Desktop lists it as roadmap only; nothing exists in the
repo. A fixed camera route through the Great Plateau that runs without player input, records per-frame CPU/GPU times
and hitch causes, and writes a summary through the broker, so console runs need no player. Shared code, so desktop
gains it too. **Done when:** two consecutive runs on .41 of the same route agree within 2 % on mean and 95th-percentile
frame time.

Status 2026-10-28 (host-built, eboot `4b70bf98…`, not yet run on the console): `runtime/benchmark.c` runs when
`benchmark.txt` is in the data root: it copies `save/` to `benchmark/save/` (the game continues that copy), warps Link
outside the Shrine of Resurrection (`test_warp.c` "plateau"), presses A on the title screen until Continue loads,
lets the world settle 20 s, then measures 60 s of camera turning (5 s still, 20 s right, 3 s up, 3 s down, 20 s left,
9 s still) and closes the game. Summary: `benchmark/last.txt` (mean/p50/p95/p99 frame time, on-time share, hitches by
cause, render/guest CPU and GPU per frame, and the difference from the previous run's mean and p95 with "agree
within 2 %" or not); one line per run in `benchmark/history.txt`. Measurement: `runtime/frametime.c` (`[vk]`,
`[hitch]` and `[perf]` lines, FINDINGS 2026-10-28 "M6 measure1"). The PS5 title precompiles every shader before the
game runs at its first start and after a translator/driver change (D6; `precompile_shaders = 0` turns it off).

### M7 — MihawkGen 2× and VRR at 120
D7 backend; frame pacing in `runtime/hle/framepace.c` for real + generated presents under VRR.
**Done when:** settled 119–120 Hz presented cadence in play, `gpu_ms` of the generator logged per window, VRR
confirmed on the TV, FG off in menus; A/B vs FG off for latency and artifacts (camera pan, Link running, HUD).
The Video page's **Frame generation: On / Off** row drives MihawkGen instead of desktop FSR 3, offered only when the
display runs at 119.88 Hz; with it on, the TV shows twice the frame-rate limit (60 → 120). Persisted like the rest.

**First-launch defaults (decided 2026-10-28).** Applied only for keys absent from `savagewind.ini`, then written to
it, so an explicit choice in the Video page always wins, also across updates:
- Resolution: Auto (the display's largest mode).
- Frame rate: 60 real frames (no 120 real-frame choice; 120 comes from frame generation).
- Frame generation: On when the display runs at 119.88 Hz (any resolution), off at 60 Hz (M7 met on the benchmark;
  the user confirmed the HUD and VRR on the TV).
- Upscaler at 4K: **FSR 3.1** (user decision 2026-10-29, `2a6aaf6`): Performance (1920×1080 → 3840×2160) with FG,
  Quality (2560×1440) without; at 1080p, native without upscaler. FSR 1 stays selectable. Measured with FG on
  (default settings, after the motion-vector optimizations `7afd98a`): 97.6–98.0 % on time, 59.6 / 119.3 fps,
  render thread 11.1 ms, GPU 12.4 ms; FSR 1 at 1080p internal: 99.9 %, 9.0 / 10.0 ms. Earlier internal-resolution
  measurements (FSR 1, `ps5/evidence/m7/res-*.klog`): 1080p 88.4 %, 1440p 79.4 % (before the recorder thread and
  async klog). The remaining FSR 3.1 cost is per-draw motion-vector work (~2.2 ms in busy scenes).

The PS5 applies missing video keys after choosing the actual VideoOut mode, then saves them; explicit keys are kept.
The default-selection and key-presence cases have a host test in `tools/test_ps5_video_defaults.c`; first launch on
.41 picked FG on, FSR 3.1 Performance, HDR from the system output, sharp UI and HD-UI generation at 4K.

Status 2026-10-28 (eboot `22a6cf72…`, host-built; console runs of earlier builds in FINDINGS): the backend choice is
the build's (`runtime/nvn/vulkan/framegen.h` `g_fg_backend`: desktop links `framegen.cpp`, FSR 3; the PS5 links
`mihawk.cpp`, MihawkGen, so no GPL code reaches the desktop build). MihawkGen is pinned in `ps5/deps.lock`
(`c57232a9…`, weights `network.fgw` by SHA-256), exported with `git archive` into `.deps/ps5/mihawkgen` and compiled
into the title; the title folder ships `network.fgw` and MihawkGen's licence and notices. With the row on, at
119.88 Hz and in play, `exec.c fg_mihawk` records the neural method at t = 0.5 between the HUD-less images of the last
two game frames (the scene composite before the HUD, as FSR 3 uses), scaled down to the 3D scene's internal size
(2560×1440 at 4K with FSR 1 Quality), 3 slots = frames in flight, all barriers its own; the generated frame goes back
up to the final image's 4K with FSR 1 EASU + RCAS as the scene does (or bilinearly: `fg_upscale = bilinear` in
savagewind.ini), and the newest frame's HUD is put over it (`fg_out.frag`, RCAS fused into that pass, drawn straight
into the window when the image is 1:1 there; `fg_passes = separate` keeps the passes apart, A/B; per-pass GPU time in
the `[fg]` line). `device.c` presents it as FSR 3's frame (crop, anti-aliasing) and the game's frame half a period
later, on a grid of the game's period (late frames move it, early ones bring it back 1 ms a pair), from a present
thread (on the one queue the PS5 has, under a queue lock; up to three queued presents; a generated frame with no free
window image is dropped rather than stalling the game). This replaces D7's "4K output" (the 4K generator measured
2.75 ms, FINDINGS). The frame-rate limit counts the game's frames (`framepace.c`: 60 gives ~120 presents; held 0.5 %
under half the refresh rate, 59.64 fps and 119.28 presents at 119.88 Hz, so FIFO can drain; not VSync's VRR margin);
camera cuts and the first frame after FG was off show the game's frame twice. The generator's GPU time (its passes'
timestamps, `fg_context_set_profile`) is in the 2 s `[fg]` line, each wait and the render thread's CPU in
`[fgpipe]`; the benchmark summary adds presented fps, the present-interval distribution and FG GPU ms, and 10 s of
unmeasured fast motion with generated/real captures follow it, with the present's and frame generation's render-thread
CPU; `internal = 1080p` / `1440p` in benchmark.txt picks a run's internal resolution (named in the summary);
`fg-capture.txt` captures pairs in play. Host check: `make test-framepace-fg`.
For a native-pixel crop of the real output, write `x y width height [route_seconds]` (output pixels, top-left origin)
to `/data/savagewind/shot.txt`; the request is saved as `screenshots/shot-<time>.png`, and re-arms on each start. The rectangle must
fit the output. The optional route time (0–60, exclusive) arms one capture at that measured benchmark time; omit it for
the next frame. Alternatively append `title N` or `loading N` to capture N seconds after that screen appears (N from
0 to less than 60). The selected screen must still be visible when the delay expires. The log includes elapsed route
and screen times and measured-frame count. Captures use the SDR intermediate before HDR encoding and wait for GPU readback.
Identified button sheets and HUD icon masks use 2× R8 replacements at 1440p output and 4× at 4K, with the same
normalized cell layout; allocation failure or multi-level textures retain the original-size upload path. The game's
unchanged bitmap letters and icons that are not redrawn remain limited by their original artwork.

### UI HD pack, user-generated

Generate the pack on the player's PC from their own base and 1.9.0 update packages; derived artwork is not distributed
with the port. The intended pack covers non-font BNTX textures in every `Layout/*.sblarc`: BC4/BC5 retain their
channels, while colour textures use BC7, all with mipmaps. A manifest records original content hashes. The runtime
uses it when `hd_ui` is enabled, retaining the original or Sharp UI fallback when a replacement cannot be used;
the GPU allocation budget is 512 MiB.
Storage-capable textures can use an HD replacement when sampled. When the guest actually uses a texture for storage,
copy, clear or rendering, the runtime restores the guest image and keeps that texture body on the guest path.

**Done when:** the generator produces the complete pack from the user's packages, host tests cover lookup and BC7
loading, desktop and PS5 builds succeed, and an in-game console comparison confirms the replacements and budget.

On Linux, build the host importer target `savagewind-hd-ui-extract`, then run
`savagewind-hd-ui-extract <prod.keys> <private-temporary-dir> <base.nsp> <update.nsp>` followed by
`python3 tools/hd_ui_pack.py <private-temporary-dir> <output-hd-ui-dir>`. The temporary directory contains derived game
data and must not be distributed. The Python tool needs Pillow, a C++ compiler and Vulkan; it fetches the pinned
upscaler and BC7 encoder into its cache. Copy the resulting output directory to `/data/savagewind/hd-ui/` on PS5 or
`hd-ui/` in the desktop data directory. `make test-hd-ui` exercises the host DDS lookup.
The default selection skips textures with a side under 16 pixels, sources larger than 1024 pixels on either side,
and flat, pure-gradient or low-edge masks; `--max-size` changes the size ceiling. Remaining textures at most 256
pixels on both sides use 4×, larger textures 2×. The common C policy in `runtime/nvn/hd_ui_policy.c` defines the
selection and layout enumeration for PC and console. The PC tool prioritizes HUD entries, writes the manifest after
each result, and resumes verified entries on restart. It prints the Vulkan device and periodic progress/ETA.
The `0x0901` source format is treated as two RG8 channels and encoded as BC5, matching its NVN upload. Generator
version 2 invalidates packs made with the earlier RGBA4/BC7 interpretation.


**On-console generation (requested 2026-10-28; qualified on .41 2026-10-29).** The PS5 title can generate the
same DDS/manifest format from the player's merged packages. `hd_ui_generate` defaults on at 4K output and off at
lower output resolutions; `hd_ui` independently controls whether the resulting textures are loaded. The generator
waits for shader archive reads before scanning layouts, then overlaps inference with shader compilation using the
pinned ncnn Real-ESRGAN animevideov3 network on low-priority Vulkan queues; ncnn CPU work is capped at one thread.
BC4/BC5/BC7 encoding and mips run after the shader workers finish. A bounded queue connects those stages, and the
precompile screen shows the HD interface count. Once precompilation ends, the game starts without waiting for the
pack; generation continues on low-priority producer and encoder threads, pausing between work items while a loading
screen is active. Replacements not discovered during the current session are available on the next launch. Files and
atomic manifest updates go through the storage broker to `/data/savagewind/hd-ui/`; a verified partial manifest
resumes after interruption. A generator or game version change triggers regeneration. To force regeneration for
testing, back up and remove `hd-ui/manifest.json`; matching DDS files will be replaced. The PC tool remains available.

Host evidence: the corrected extraction code matched the PC generator for 881 selected entries in the merged Common
and Title packs. On lavapipe, two real texture inputs at both scales matched the pinned release binary at infinite,
infinite, 65.03 and 65.49 dB PSNR; BC4/BC5/BC7 top-level blocks matched the PC encoders on the test inputs.
On .41, the earlier `e4845749…` build completed 866 textures in 204.6 s; shader state reached done at about 127 s,
but the game waited 208.6 s for the pack, versus 119.8 s without generation (ps5/evidence/hdgen/fresh-start.klog).
The nonblocking version (`30e899d5…`) on .41 from a fresh install: precompile done in 119.0 s (119.8 s without
generation), the game starts, and the pack completes in the background (866 textures, 199.1 s after its start; one
267 ms title-screen hitch). Coverage equals the PC pack (866 / 866, 51–67 dB on a 12-texture sample).

### After M7 (optional) — HDR10 output
Requested 2026-10-28. Desktop has HDR10 presentation on `test` and lists colour grading/HDR as roadmap. On PS5 the
only evidence is that the pinned RADV build carries Forever patch 0001 (HDR10/immediate); no HDR swapchain has run on
this port. **Done when:** an HDR10 surface is created on .41, the TV reports HDR, a Video row turns it on/off, and SDR
output is unchanged with it off.

Implementation awaiting console qualification: `hdr = off` is the PS5 default; the Video page offers Off/On when
RADV advertises HDR10. Patch 0001's format advertisement checks the title's HDR attribute, not the TV, so display
HDR capability is unknown and the user must choose a compatible display. The desktop HDR pass converts the SDR
intermediate to BT.2020 PQ after FSR 1 and MihawkGen; captures read that SDR intermediate before encoding and remain
SDR-viewable. `hdr_paper_white` optionally sets SDR white in nits (default 200). The title logs the selected format
and colour space. HDR switching, AvControl's status, SDR capture identity and benchmark cost still require .41 runs.

### After M6 (optional) — Draw distance and detail
Requested 2026-10-28; desktop roadmap item, not yet built there. On PS5 it trades distance for fps within the
headroom M6 measures. Take desktop's implementation when it lands; otherwise build it as shared code. **Done when:**
each setting's CPU/GPU cost is measured with the benchmark and the default keeps the M6 frame target.

### After M7 (optional) — Dynamic resolution (PS5 only)
Requested 2026-10-28; **PS5 only** (user decision): desktop behaviour is unchanged and gets no new setting. It
helps only when the GPU is the limit; the M6 benchmark measured GPU 7.7 ms and render-thread CPU 10–14 ms.
The controller (target GPU time from measured timestamps, per-frame internal size) lives in a PS5 file per D2;
the shared Vulkan backend gains a size hook that desktop leaves at the allocation size.
Allocate scaled targets at the maximum size and set each pass's viewport/scissor
to the used part. Every later pass
that samples those targets (post-effects, bloom, screen-space steps, the HUD composite, FSR 1's input rectangle, the
HUD-less capture for MihawkGen) must read only the used part. **Done when:** on the benchmark the internal size
varies with load, the GPU frame time stays under its target, there are no edge or stretch artifacts in the frame
captures, no new hitches, and the desktop build is unchanged in behaviour.
Behaviour (user's example, 2026-10-28): the internal resolution steps down from the default (e.g. 1080p) through
900p to a floor of 720p under load, and back up when there is room, always upscaled by FSR 1 to the output size. The
trigger is the **measured GPU time per game frame**, not the frame rate: when the render thread (CPU) causes a drop,
lowering the resolution would only blur the picture. Step down after a few frames over the GPU budget, step up only
after a longer stretch well under it (hysteresis, so the size doesn't oscillate). MihawkGen runs at the game image's
size (M7), so it must accept the current used part of a max-size image too.

The PS5 Video page exposes `dynamic_resolution` (default `0` in `savagewind.ini`); TAA forces it off.
FSR 3.1 also requires the experimental `drs_fsr3 = 1` ini switch (default `0`). With FSR 3.1 the three
levels are the configured height, the midpoint, and a floor of 67% of configured height or 720p, whichever
is higher (never above the configured height). Other upscalers retain their configured, 900p and 720p levels.
The GPU-timestamp controller uses three-frame downshift and two-second recovery hysteresis. Benchmark diagnostics
accept `gpu_load = N` in `benchmark.txt` for the first 20 seconds of the measured route and report the
internal-size distribution. Visual, timing, and hitch criteria above still apply.
When DRS is enabled at startup, shader precompilation uses a separate completion marker and includes DRS variants
of observed shader keys and their pipeline libraries. Enabling DRS during play does not rerun that precompilation;
restart with DRS enabled to run it. Pipeline-state and hitch coverage still require a console measurement.
The `[drs] step` log reports the internal-size change and shader and pipeline builds completed in the presented-frame
interval; asynchronous work may have started earlier, so the counts alone do not establish causation.

**DRS with FSR 3.1 (experimental; requested 2026-10-29).** With `drs_fsr3 = 1`, FSR 3.1 receives a per-frame
render size and the DRS remap renders into the used part of max-size targets. The same GPU-timestamp controller
and hysteresis apply. This lets a higher target (e.g. Quality 1440p) run by default and drop only in GPU-heavy
scenes; it does not help render-thread-bound frames. **Done when:** with `gpu_load` the size steps down and back on
the benchmark, no smearing/flicker at the steps in same-frame captures, on-time not worse than fixed size, and DRS
off is unchanged.

### After M7 — Render-thread CPU (done 2026-10-28/29)
The render thread (NVN → Vulkan translation) was the frame-time limit. Kept, each measured on .41: PR #189's recorder
thread (default on PS5; FG on 88 % → 99 % on time), asynchronous klog (the "command translation" hitches were our own
synchronous logging), fused FG passes. Measured and not kept: game code at `-O2` (within noise). PRs #128/#155
remain unported (no profile evidence that descriptor sets or barriers dominate). With FSR 3.1 the per-draw motion
vectors add ~2.2 ms in busy scenes after SIMD matching, radix sort and lighter bookkeeping (`7afd98a`); that is the
remaining CPU item (target: FSR 3.1 + FG ≥ 99 % on time, as FSR 1). Compact radix sorting (`b120ca3`) brought it to
98.3–98.7 %. The bounded match scan (`0513106`, equivalent on .41) showed no gain in the FG-on A/B at 120 Hz
(render 11.13 vs 11.17 ms, within noise; ps5/FINDINGS.md) and was dropped, not merged.

### M8 — Temporal upgrades (after PR #194/#195)
PR #194 (`35efe256e4`) and PR #195 (`5039b30407`) are integrated from their pinned heads, including per-object
motion vectors, jitter, depth copy, reactive mask, TAA and the FidelityFX FSR 3.1 upscaler. FSR 3.1 works on PS5
after a FidelityFX wide-string fix (`80f8327`) and is the 4K default since 2026-10-29 (`upscaler = fsr3`, quality by
FG state); `antialias = taa` stays opt-in. Either enables motion vectors and jitter. The title links the upscaler
and its Vulkan backend against RADV, separately from MihawkGen's frame-generation backend.

Re-qualify on PS5 at 1.9.0: upstream evidence is 1.8.2, Linux, RTX 3080 Ti only, so CPU cost, GPU cost, VRR/120 Hz
pacing with FG, and image quality are unmeasured here. The vectors are jitter-free NDC differences in an RGBA16F
scene-sized image, not MihawkGen's RG16F backward UV vectors; a conversion, matching depth/frame lifetime and explicit
barriers are needed before `mvwarp`/`mvsplat` can use them. Then A/B those methods against neural, and choose defaults
by measured image quality and GPU time.

**FSR 4 candidate (`../ps5-fsr4`, FSR 4.1.1 INT8; measured 2026-10-28).** Upscaler only (its plan lists frame
generation out of scope). Runs on RADV (probe `ps5/probe/fsr4_radv.c`): GPU 3.8 ms for 720p→1080p, 14.3 ms for
1080p→4K, 14.8 ms for 1440p→4K, so it can't hold 60 fps at 4K next to the game's ~10 ms (RADV has no accelerated
INT8 dot product on this GPU). Not integrated; stays a candidate for 1080p output only. GPL-3.0: PS5 build only (D10).

### M9 — Root storage broker (D12)
Path-restricted payload, versioned client, direct RomFS reads and brokered atomic writes. **Done when:** broker A/B on
.41 covers a cumulative 3 GiB write and save/cache-like atomic replacements without a 2 MiB/s collapse; direct reads
remain unchanged; unavailable/old broker and missing ShadowMountPlus each produce a clear error; path traversal,
intermediate/final symlinks, hard links and cross-root operations are rejected.

### M10 — Game data read from the user's packages (D11)
Package library (shared with the PC ExeFS tool), direct-read `romfs_image` backend with DLC, TV preflight/error
screen, policy decision above. **Done when:** on the host, every RomFS file read through the backend from base NSP +
update NSP matches manual extraction (all 41,062 RomFS SHA-256s), and a base XCI + update NSP is identified as the
same game (same 1.9.0 ExeFS; XCI used on the PC only, never copied to the console); on .41 the backend opens
the packages and a sampled set of files (including the largest packs) matches the host hashes, with open time and
read throughput recorded against the extracted-folder figures; and the game boots to the title screen from the
packages (needs M2/M3). Update-only, wrong-title, wrong-version, missing/corrupt-content and missing-key cases show
specific errors on the TV. No key material appears in logs; unsupported compressed containers are rejected.

### M11 — Release
Settings persistence, save/load, DLC, 1-hour soak, README for the setup/import flow, notices (D10), release ZIP.
Done: settings persist through the broker; soak mode (25 min run clean, memory flat; stopped by the user, who waived
the hour); private release ZIP builder with a no-game-content check (`ps5/tools/package-release.sh`). Open: save/load
trigger (below), DLC untested (user's DLC Pack 1 `01007EF00011F001` and Pack 2 `01007EF00011F002` NSPs, both
v196608, available since 2026-10-29: test PC ExeFS/AOC selection, console AOC reads and both packs in game), public repos, README with NSP/XCI install steps, GitHub
Actions on the self-hosted runner, catalog entry PPSA90212 (reserved, catalog PR #894), GPL source distribution, and
deleting the local `ps5-backup-before-rewrite` branch (old history with game captures) before any push.

User decisions (2026-10-29): (1) answered: the fork rewrites the "never decrypts" rule for the PS5 port (player's own
NSP/XCI and keys, decrypted in memory on the player's console, nothing shipped; README and D11), so D11 no longer
blocks a public release. (2) Release form, answered: the public release is the PS5 title alone, building the game
code on the console (M12); the PC builder is an interim/fallback.

For an unattended soak, put `soak = 60` in `/data/savagewind/benchmark.txt`. The benchmark continues a copied save,
settles for 20 seconds, and repeats the 60-second camera route for the requested measured minutes. Cumulative frame
and hitch counts and PS5 memory observations are appended every five minutes and at completion to
`benchmark/soak.txt`; the usual detailed `benchmark/last.txt` describes only the first route minute. The heap is
mapped title heap bytes, direct memory is the largest available block (not total free), and GPU bytes cover VMA
allocations, not every GPU allocation.

`save_check = 1` reports unavailable after the route: the runtime exposes save filesystem operations and the
benchmark's copied save directory, but no game-level save initiation, save completion, or reload trigger. It does
not claim that filesystem I/O alone proves a save/load round trip.

### M12 — On-console game-code build (PC-less public release; requested 2026-10-29)
Goal: the release ZIP holds only the title (runtime, translator, compiler parts, licences). On first start the console
generates and compiles the game code from the user's own ExeFS inside the packages, caches it under
`/data/savagewind/cache/gamecode/` (keyed by `main` build id + generator version), and loads the cache on later
starts. Nothing generated leaves the console; the legal position is the PC builder's, done on the user's console.

What already supports it: the title reads ExeFS from the packages (M2/M10); a title can run code it writes into
direct memory (`ps5platform/exec.h`, measured in `PS5_Vulkan` docs/PROBE.md), placed `PS5_EXEC_NEAR` the runtime
for rel32 calls. Changing protection to RX is firmware- and jailbreak-dependent (ProsperoEden `docs/FW740.md`:
`mprotect(RX)` fails for a non-jailbroken app on 7.40, while separate RW and RX views of the same memory work), so
the code cache uses `PS5_EXEC_DUAL_VIEW` (write through the RW view, run from the RX view, no protection changes).
ProsperoEden runs a dynarmic ARM64→x86-64 JIT on PS5 this way. What
doesn't: the title cannot load modules (`sceKernelLoadStartModule`/`dlopen` fail, S3), there is no compiler on the
console, and the generator is ~4,200 lines of Python (`tools/gen.py`, `lifter.py`, `simd_c.py`, `patches.py`,
`game_sites.py`, `layout.py`, `nso.py`, `analyze.py`; capstone, unicorn, lz4).

Paths, decided by the spikes below:
- **A. Same pipeline in the title (preferred):** a C++ port of the generator (byte-identical C to the Python on the
  host), compiled in-process by an embedded clang/LLVM (Apache-2.0 with LLVM exception) built with the Prospero
  toolchain, and an in-title static linker/cache (design below), not an eboot relink: the title can't load modules.
  Keeps desktop parity, the HLE patch sites and current performance; costs LLVM's size and a long first build.
- **B. Lifter to LLVM IR directly:** skips C parsing (faster compile); a second generator back end to keep in step.
- **C. Runtime translation (dynarmic, ProsperoEden precedent):** no compile step, but loses static recompilation's
  optimisations and the generated-function patch sites; a runtime redesign. Fallback only.

**Loader design (recommended A; not yet implemented).** The current `build-game.sh` puts `libgamecode.a` in a
`--start-group` link with `--gc-sections`; `gamelib_ps5.c` directly imports `layout.c`'s `game_modules`,
`game_module_count`, `game_version`, `game_sites` and `game_site_count`. Each module points to generated
`*_fn_addrs`, `*_fn_ptrs` and `*_fn_count`. In the other direction the generated objects call game functions and
runtime/HLE functions and access runtime data. An `nm` census of the current `-O1` archive finds 40 external
names after subtracting archive definitions, including `rt_dispatch`, `rt_svc`, `rt_setjmp_slot`, `hle_longjmp`,
`setjmp`, `memset`, `g_fnprof` and the `rt_*` patch hooks; derive the actual set from every build, not this list.
The title map confirms the layout and function tables and `rt_setjmp_slot` in the linked image.

- **Format/link:** clang 18 emits ELF64 x86-64 relocatable objects in memory for the 997 generated C files
  (including layout and tables). A small deterministic in-title static linker consumes them in stable input order,
  resolves local/global/weak definitions and lays out aligned allocatable text, rodata, data and zero-filled bss.
  Use S7's near dual-view allocation: write via RW, execute via RX; code pointers and PC-relative calculations
  use RX addresses, mutable data uses its RW address. Account for both view addresses, alignment, GOT, thunks,
  metadata and memory budget rather than assuming 256 MiB suffices. Keep the image alive until all guest threads
  stop. Compared with embedding LLVM ORC/JITLink, this adds a bounded ELF linker to maintain but avoids another
  large dependency and its integration/target-memory-manager complexity; ORC shares LLVM's Apache-2.0 with LLVM
  exception, but its footprint, determinism and PS5 support are unmeasured. Keep ORC as a fallback, not a claim
  that the custom linker is already smaller or correct.
- **Symbols and relocations:** maintain a checked-in, game-content-free manifest of runtime/HLE and libc/SDK ABI
  names available to generated code, seeded by the archive census and the generator's possible emitted calls.
  Generate a retained name-to-address table from that manifest at title build time, without player objects; use
  real address references so `--gc-sections` cannot discard exports. At console link time subtract object
  definitions from undefined symbols and reject required names absent from the table. Version the table by a
  digest of sorted names, ABI declarations, runtime build identity and compilation ABI; a changed runtime
  invalidates the linked cache. Reject duplicate strong definitions and
  unresolved non-weak symbols. The current archive's `readelf -rW` census has `R_X86_64_PC32`, `PLT32`,
  `GOTPCRELX`, `REX_GOTPCRELX` and `64`; implement their ELF RELA addends, signed-range checks and the
  PC-relative `S + A - P` / absolute `S + A` rules. Provide deterministic local GOT slots for GOT relocations
  without instruction relaxation, and direct PLT32 calls or nearby absolute-jump thunks for out-of-range calls;
  do not depend on a dynamic GOT/PLT. Check every PC32 data reference and fail clearly if placement cannot reach;
  never truncate. Census every newly compiled object and reject unsupported relocations or allocatable sections.
  The wrapper uses `-femulated-tls`; the current archive census has no TLS relocations, COMDAT groups, common
  symbols or `.init_array`. Reject these until their semantics are explicitly implemented and tested, including
  emulated TLS descriptors/`__emutls_get_address` if generated inputs ever require them.
- **Unwinding:** 992 current archive objects contain `.eh_frame`. Generated code is C; guest setjmp uses a host
  `setjmp` and `rt_setjmp_slot`, and reboot/guest longjmp crosses generated frames without C++ unwinding.
  PS5 `plat_print_backtrace` currently reports unavailable, but runtime C++ exceptions exist: audit all HLE
  boundaries for exceptions crossing generated C. Preserve and relocate `.eh_frame`, and register/deregister it
  with the actual PS5 unwinder if such crossings are supported; prove that registration works on console before
  relying on it. Otherwise contain exceptions at the C boundary and explicitly test that contract. Longjmp,
  reboot, and any supported stack walking must be tested independently.
- **Cache:** under `/data/savagewind/cache/gamecode/`, persist a versioned, checksummed manifest and linked image:
  ELF/architecture and target flags, generator and LLVM identities, runtime/export ABI digest, all ExeFS module
  build IDs and content digests, ordered generated-source/object digests, section sizes/alignment, symbol offsets
  and a bounded fixup stream. Store a canonical unrelocated image plus fixups referencing image offsets or export
  names, not process addresses; on every load allocate fresh views, resolve exports and reapply fixups at the new
  bases. Validate lengths, bounds, digests, relocation types and identity before publishing the image; atomically
  replace completed files through the storage broker. A corrupt, incomplete or mismatched cache is rebuilt, never
  executed. Cache individual compiled objects by source digest plus compiler, flags, headers and ABI identity,
  with atomic completion, so an interrupted build resumes without trusting partial files.
- **Cutover and startup:** build a release variant without `libgamecode.a` or static references to its symbols.
  Resolve the five layout symbols from the finished image and construct `GameLibTables` in the runtime; keep
  `gamelib_load`'s version/site checks, `gamelib_tables`' lifetime and all existing dispatch consumers unchanged.
  Validate every module's build ID against the user's ExeFS, not only `main`. On first start enumerate/validate
  inputs, generate C, reuse or compile each object, link/fix up, validate tables, then start the guest. Show
  stage and completed/total progress beside shader precompile; only overlap where measured memory and CPU budgets
  permit. Subsequent starts validate and relocate the cache before guest execution.
- **Qualification and order:** (1) census/ABI and linker contract, including unwind and memory budget (2–4
  engineer-days); (2) deterministic ELF linker, exports and relocation/negative tests (8–15 days); (3) cache,
  restart and runtime cutover (4–7 days); (4) generator/compiler orchestration and progress UI (4–7 days);
  (5) host and console qualification (5–10 days). Estimates are engineering ranges, not measured schedules.
  On Linux link the same PS5-target objects with the same linker logic and compare tables, selected generated
  functions, HLE calls, setjmp/longjmp and results with a conventionally linked reference; exercise relocation
  types, rebasing, weak/duplicate/missing symbols, corrupt cache and interrupted compile. On PS5 compare the
  loaded build to the eboot-linked build using identical input, settings and benchmark route: cold build and warm
  reload, per-stage time, peak memory, repeated reboot, relocation/range and cache invalidation/error paths,
  exception/unwind contract and gameplay. Record benchmark equivalence within 2% and preserve receipts before
  claiming qualification.

Spikes (console .41): **S7** a 256+ MiB dual-view executable image near the runtime, alongside the guest window,
GPU and FG memory budgets: load, relocate against runtime symbols, call through, reload from the cache file; measure
load time; check the console's firmware and jailbreak state, since both affect executable memory. **S8** clang/LLVM static libraries for Prospero in a
probe title: compile one real generated module (from the user's ExeFS on the console) to an object in memory;
time and peak memory per module, extrapolated to all 996 objects (PC: 324 s on 24 threads at `-O1`). **S9** the
generator port on the host: identical C output for `main`, `sdk`, `subsdk0` versus `tools/gen.py`; then its
runtime on the console. UI: the first-start screen shows game-code build progress next to the shader precompile
(both can overlap; the compile is CPU-bound, so measure).

S7 diagnostic: create `/data/savagewind/s7.txt` (empty for the default 256 MiB, or `mib = N`) before launching.
After the window and startup RomFS are ready it tries the SDK's near dual-view, toggled RX and RWX executable
regions independently, writes and reloads `cache/s7-image.bin` through the storage broker, then retains the first
successful mode while the game runs. `[s7]` lines include firmware, uid/euid, allocation, call/relocation results,
throughput, memory snapshots and summaries; `direct_free_after` is the largest free direct-memory block, not the
sum of all free blocks. UID is not proof of jailbreak state. Remove `s7.txt` to disable the diagnostic. The
current linked game archive contributes 235,425,008 bytes of text, rodata and data (224.52 MiB), so the minimum
256 MiB is the default. This is a synthetic image, not a game-code loader or a console qualification.
**Done when:** a console with only the release title, the user's packages and keys builds the game code on first
start with a progress screen, plays, and on later starts loads the cached code in seconds; generated C equals the PC
generator's; the benchmark is within 2 % of the PC-built eboot; first-start time and memory are recorded; the
release ZIP's no-game-content check still passes.

### After M11 (optional) — Community texture packs
High-resolution texture packs dropped into the existing overlay folder (`runtime/services/fs.c`); never shipped by
the project. **Done when:** one pack loads, with GPU/flexible memory and load-time cost logged against the M6
budget, and the frame target still holds.

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
