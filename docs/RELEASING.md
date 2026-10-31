# Releasing the PS5 title

The public ZIP contains the console-built PPSA90212 title and storage broker, not a PC build kit or any game
files. Players supply their own Breath of the Wild 1.9.0 packages and keys on their console; see
[README-PS5.md](../README-PS5.md) for installation and the current requirements. Do not put private game inputs,
generated game code, textures or keys in the checkout, runner workspace or release.
The ZIP includes the bundled and standalone storage broker, console compiler headers, Open Sans and its
FreeType/libpng notices. Its only PNGs are the neutral title icon and the identical neutral restoration copy.

For a non-distributable local candidate, first run `PS5_GAMECODE=console bash ps5/tools/build-game.sh`, then
`bash ps5/tools/package-public-release.sh --candidate`. The candidate uses the existing build, permits a dirty
worktree and unresolved source provenance, and marks the ZIP accordingly; it is not a public release.

## Runner prerequisites

Register a self-hosted Linux x64 runner for `bizkut/SavageWindRecomp` on the build machine and label it
`savagewind-ps5`. The existing runner registered to `bizkut/ps5-forever` is not automatically available to the
fork. Install clang/LLVM 18, CMake, Ninja, Python 3 and its pip, the GitHub CLI, and the title build's other host
requirements. The workflow fetches pinned SDK, nstool, MihawkGen and zlib sources; it currently copies the pinned
RADV archive and its headers from `/home/bizkut/workspaces/SavageWindRecomp/.deps/ps5/radv`. The archive is checked
against `ps5/deps.lock`, as is the sorted list of header paths and content hashes. The runner must not have private game
files in its workspace. It retains a digest-checked LLVM cache; release runs are serialized. Dispatching on `ps5`
uploads a non-distributable candidate artifact; a PS5 release tag publishes a release.

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
