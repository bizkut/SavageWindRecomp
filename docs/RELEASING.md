# Releasing the PS5 title

The public ZIP contains the console-built PPSA90212 title and storage broker, not a PC build kit or any game
files. Players supply their own Breath of the Wild 1.9.0 packages and keys on their console; see
[README-PS5.md](../README-PS5.md) for installation and the current requirements. Do not put private game inputs,
generated game code, textures or keys in the checkout, runner workspace or release.
The ZIP includes the bundled and standalone storage broker, console compiler headers, Open Sans and its
FreeType/libpng notices. Its only PNGs are the neutral title icon and the identical neutral restoration copy.

For a non-distributable local candidate, first run `PS5_GAMECODE=console bash ps5/tools/build-game.sh`, then
`bash ps5/tools/package-public-release.sh --candidate`. The candidate uses the existing build, permits a dirty
worktree and unresolved source provenance, and marks dirty candidates in the filename; it is not a public release.

## Published release notes

The tag workflow publishes the text between the matching tag's markers below. For each future release, add a
new uniquely marked section before tagging; a missing, duplicated or empty section fails publication.

<!-- release-notes:ps5-v0.2.1:start -->
## Breath of the Wild on PS5 — ps5-v0.2.1

This update includes the upstream merge and PS5 improvements since ps5-v0.1.0. ps5-v0.2.0 was withdrawn;
upgraders can install this release directly over ps5-v0.1.0.

- Fixes the observed ground and rainbow foliage corruption by disabling Vulkan view format lists on PS5.
- Game-requested naming opens the inline PS5 keyboard; Enter confirms and closing it cancels without changing the
  existing text. There is no manual L3+R3 shortcut. Confirm and cancel were exercised in a guest-thread probe
  without a freeze; actual horse registration has not yet been verified.
- Includes upstream game-code and runtime changes, gameplay fixes, and PS5 settings for draw distance, haze,
  colour grading, HUD size and accessibility assists. The storage helper also recovers from a replaced or removed
  data directory and reports startup errors.

**Updating requires a one-time on-console game-code rebuild**, approximately 9 minutes plus shader preparation;
actual time varies. Leave the console on while the progress screen is displayed.

**Performance:** in a limited PS5 benchmark at native 1080p with frame generation off, the merged build reached
99.3% on-time frames versus 98.0–98.2% for ps5-v0.1.0. Average render time regressed from 8.61–8.63 ms to
9.28 ms. This measurement does not establish a specific CPU cause or predict every scene and setting.

**Requirements:** supply your own base NSP or XCI, matching 1.9.0 update NSP and `prod.keys`; optional DLC NSPs
and, when needed, `title.keys` go in `/data/savagewind/import/` too. Keep your inputs there for subsequent starts.
No game content or keys are included. See the
[PS5 installation guide](https://github.com/bizkut/SavageWindRecomp/blob/ps5-v0.2.1/README-PS5.md).

**Limits:** horse registration and rumble output remain unverified; videos have not been tested and gyro axes
are not qualified. The keyboard probe is not end-to-end horse registration, and these fixes do not establish that
all gameplay issues are resolved.
<!-- release-notes:ps5-v0.2.1:end -->

<!-- release-notes:ps5-v0.2.0:start -->
## Breath of the Wild on PS5 — ps5-v0.2.0

This release merges upstream changes and fixes the storage helper, including recovery when
`/data/savagewind` is deleted or replaced, visible startup errors, and replacement of an older running helper.
The PS5 home-screen name is **Breath of the Wild**. There was no published ps5-v0.1.1.

**Highlights**

- Generated game code keeps frequently used guest registers and flags in locals. Memory freed on reload returns to
  the system, and finished threads, timers and closed files are released.
- Upstream fixes address high-frame-rate moving platforms and shield-surf jumps, Master Kohga's entrance at high
  frame rates, and DLC-merged game data.
- PS5 settings include draw distance and haze, Colour grading controls, HUD size and Accessibility gameplay assists.

**Your own game files are required.** Put your base NSP or XCI, matching 1.9.0 update NSP and `prod.keys` in
`/data/savagewind/import/`; optional DLC NSPs and, when needed, `title.keys` go there too. Keep them there for
subsequent starts. The release does not include game files or keys. See the
[PS5 installation guide](https://github.com/bizkut/SavageWindRecomp/blob/ps5-v0.2.0/README-PS5.md) for console
requirements and setup.

**Updating requires a one-time game-code rebuild** on the console, approximately 9 minutes plus shader
preparation; actual time varies. Leave the console on while the progress screen is displayed.

**Stutter measurement:** in a limited PS5 benchmark at native 1080p with frame generation off, two merged-build
runs had 99.3% on-time frames, 17.086/17.336 ms p99 frame time and 0/1 hitches. Three ps5-v0.1.0 runs had
98.0/98.2/98.2% on-time, 19.315/19.220/18.758 ms p99 and 4/1/1 hitches. Average render time increased from
8.61–8.63 ms to 9.28 ms. These results do not establish a specific CPU cause or predict every scene and setting.

**Limits:** Console IME, gyro axes and authenticated developer control have not been tested. An exit abort
predates this release.
<!-- release-notes:ps5-v0.2.0:end -->

## Runner prerequisites

Register a self-hosted Linux x64 runner for `bizkut/SavageWindRecomp` on the build machine and label it
`savagewind-ps5`. The existing runner registered to `bizkut/ps5-forever` is not automatically available to the
fork. Install clang/LLVM 18, CMake, Ninja, Python 3 and its pip, the GitHub CLI, and the title build's other host
requirements, including Meson, Ninja, rsync and LLVM 19, SPIR-V translator and libclc development tools for Mesa.
The workflow fetches pinned SDK, nstool, MihawkGen, zlib, Mesa and public RADV build recipe sources. It builds RADV
with `ps5/tools/meson-radv.sh` to normalize build and SDK debug paths, then verifies the archive and headers against
`ps5/deps.lock`. The runner must not have private game files in its workspace. It retains a digest-checked LLVM cache;
release runs are serialized. Dispatching on `ps5` uploads a
non-distributable candidate artifact from a clean worktree; a PS5 release tag publishes a release.

## Cut a release

1. Review and commit the release changes, including the title content version, notices and installation instructions.
   Ensure the fork has the required corresponding source and that the tag is unique. Create an annotated tag, for
   example `git tag -a ps5-v1.0.0 -m 'PS5 release ps5-v1.0.0'`.
2. Push only the intended branch and tag to a remote named `fork` pointing at
   `https://github.com/bizkut/SavageWindRecomp`. Before **every** push, inspect a dry run of the exact same refspecs
   and refuse the push if it includes `refs/heads/ps5-backup-before-rewrite`. Never use `--mirror` or `--all`.
   For example:

   ```sh
   refs=(HEAD:refs/heads/ps5 refs/tags/ps5-v1.0.0:refs/tags/ps5-v1.0.0)
   preview=$(git push --dry-run --porcelain fork "${refs[@]}") || exit 1
   printf '%s\n' "$preview"
   if printf '%s\n' "$preview" | grep -Fq 'refs/heads/ps5-backup-before-rewrite'; then
       echo 'Refusing to push the backup branch' >&2; exit 1
   fi
   git push fork "${refs[@]}"
   ```

3. The tag starts `.github/workflows/ps5-release.yml`. It checks for private inputs, runs the host tests, builds the
   console variant and broker, packages them and publishes `SavageWind-PS5-<tag>.zip` and `SHA256SUMS`. Check the
   run and independently download and verify the published ZIP against `SHA256SUMS`. Host tests are not console
   qualification; qualify the actual published ZIP on a PS5 before claiming it works.
4. In the catalog's `apps/PPSA90212.json`, replace the placeholder version, tag in the artifact and icon URLs, and
   all-zero SHA-256 with the actual release values. The source license metadata is MIT; the console title includes
   GPL components and carries their notices. Check and verify the catalog entry and scan the published ZIP following
   the catalog's submission instructions before committing and proposing the catalog update.
