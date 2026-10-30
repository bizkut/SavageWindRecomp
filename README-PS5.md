# Savage Wind PS5 — private setup kit

This kit is unpublished. **Public release requires the maintainers' decision on D11**, because the title decrypts the user's packages on the console and the current `docs/SAVAGE-WIND.md` policy says Savage Wind does not decrypt. No approval is implied by this kit.

The kit contains no game executable, game-generated C, keys, game assets, or built title eboot. As in the upstream public generator and Windows release, its generator contains short game-specific signatures and site identifiers used to locate patch sites; these are not generated game code. Keep your own inputs and generated files private.

## Build on a Linux x86-64 PC

Install Python 3 with venv, clang-18, LLVM 18, LLD 18 (including compiler-rt builtins), bash, coreutils, xargs and the shared libraries needed by the bundled Linux `savagewind-exefs`. In this directory, run `bash ps5/tools/setup-release.sh`, then activate the environment (`source .venv/bin/activate`). The provided PS5 SDK is the pinned build recorded in `ps5/deps.lock`; the other bundled libraries and model are from the pinned inputs. The linked PS5 title is GPL-3.0-or-later. This private kit is not a complete corresponding-source distribution; fulfil the applicable source and licence obligations before distributing it.

Export your own base NSP or XCI, update NSP for 1.9.0, and `prod.keys` from your own console. Do not send these to anyone. With the same packages you will use on the PS5, run:

```sh
bash ps5/tools/link-release.sh /private/prod.keys /private/base.xci /private/update.nsp
```

This extracts the selected ExeFS locally, generates and compiles C from it, then links and signs `title/PPSA90212/eboot.bin`. It can take considerable time and disk space. `work/`, `generated-ps5/`, `build/ps5/gamecode/`, and the resulting eboot contain your game-derived data: do not distribute them. A fresh kit avoids stale compiled game objects. The helper does not upload anything.

## Install and first start

Install the PS5 homebrew prerequisites, including ShadowMountPlus. Once per console boot, load the supplied `bin/storagebroker.elf` with your root ELF loader; it serves only Savage Wind storage on loopback port 9142. Copy the completed `title/PPSA90212` folder to `/data/homebrew/PPSA90212/`. Copy your base and update packages, optional DLC and your `prod.keys` (and `title.keys` if needed) to `/data/savagewind/import/`. Package reads require these inputs on every launch; they are not included in the kit. Start the title: the first launch precompiles shaders and can take time. The on-console UI pack is generated from the user's own UI assets; it is not included in the kit. Saves, settings, caches and generated UI data live under `/data/savagewind/`.

**Home-screen art.** The kit ships neutral, original home-screen art. On the first start (as one step of the
first-start work, once) the title makes the PS5 home-screen icon and backgrounds from your own game files and
installs them through the storage broker; it records what it made in `/data/savagewind/cache/home-art/` and does
nothing on later starts unless your game files change. The start-up splash uses, in this order: your own
`/data/savagewind/splash.jpg` (or `.jpeg`/`.png`), the background generated from your game files
(`cache/home-art/splash.png`), then the shipped neutral picture. Nothing is uploaded or shared. Put `home_art = 0` in
`/data/savagewind/savagewind.ini` to restore the shipped art and remove the generated splash (also done once). The
home screen may show the new art only after it refreshes the title's tile (e.g. after a console restart).

## Unattended benchmark

With an existing save, write `soak = 60` to `/data/savagewind/benchmark.txt` and launch. The benchmark copies the saves to `benchmark/save`, loads the copy, settles for 20 seconds, then repeats the Great Plateau camera route for 60 minutes. Every five minutes and at the end it appends cumulative frames, on-time frames, hitches, mapped heap bytes, flexible-memory availability, direct-memory largest available block, and VMA allocation/block bytes to `/data/savagewind/benchmark/soak.txt`; the normal detailed summary is in `benchmark/last.txt` for the first minute. Memory figures are not interchangeable: direct largest block is not total free memory and VMA does not account for all GPU allocations. Remove `benchmark.txt` to disable the benchmark.

`save_check = 1` records **unavailable**, not success: the runtime has no game-level save initiation, completion or reload trigger, although filesystem operations and the isolated copied save exist. No save/load round trip is performed.
