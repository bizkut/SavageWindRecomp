# Savage Wind on PS5

The PS5 title reads your own Switch game packages and keys on your console, decrypting in memory. The release contains no game files or keys. Keep your packages, keys and generated files private. No PC build is required.

## What you need

- A jailbroken PS5 with homebrew enabled. Firmware 5.50 has been tested; other firmware is not qualified here.
- ShadowMountPlus and an ELF loader (elfldr).
- Your own base NSP or XCI, the 1.9.0 update NSP, and `prod.keys` dumped from your own Switch. Recommended: `prod.keys` dumped from a Switch on firmware 19.0.0. Optional DLC NSPs and `title.keys` may also be supplied. The game and keys are not included.
- Sufficient free console storage for the inputs, generated game code and caches.

## Install and start

1. Install the release's `PPSA90212` folder as `/data/homebrew/PPSA90212/` using your homebrew installation method.
2. Load `storagebroker.elf` with elfldr once after each console boot, before starting the title. It serves Savage Wind storage over loopback port 9142.
3. Put your own base package, 1.9.0 update, optional DLC, `prod.keys`, and, if needed, `title.keys` in `/data/savagewind/import/`. Keep these inputs there: the title reads them on subsequent starts too.
4. Start Savage Wind from the PS5 home screen. The first start builds game code on the console, prepares shaders and generates home art and UI data. A progress screen shows the work. Allow roughly 11 minutes; time varies. Later starts are roughly 20 seconds with a warm cache.

Output follows **Settings > Screen and Video > Video Output > Resolution** by default. Automatic and 2160p use 4K;
720p, 1080p and 1440p use their corresponding output sizes. Video > Output overrides this for the next start.
In `savagewind.ini`, `output` may be `auto`, `720p`, `1080p`, `1440p` or `4k`.

The home-screen icon and backgrounds are generated from your own game files and installed through the broker. The tile may not refresh until the console restarts. A custom `/data/savagewind/splash.jpg`, `.jpeg` or `.png` takes precedence over the generated splash. Set `home_art = 0` in `/data/savagewind/savagewind.ini` to restore the included neutral icon, remove the generated backgrounds and splash, and use the system background.

## Settings and storage

The game's Options menu has a PS5 Video page. Settings are stored in `/data/savagewind/savagewind.ini`; useful keys include `frame_rate` (e.g. `30`, `60`, or `off`), `vsync` (`on`, `fast`, `adaptive`, `off`), `hdr` (`auto`, `on`, `off`), `home_art` (0/1), `precompile_shaders` (0/1), `async_shaders` (0/1), and `gamecode_threads`. Changing settings can affect performance or start-up time.

Asynchronous shaders are on by default on PS5: the game doesn't pause while a new shader compiles, so there is no
stutter the first time an effect appears, but that effect is skipped until its shader is ready (it pops in late) and a
picture the game draws only once, such as a save file's screenshot, can come out incomplete. Set `async_shaders = 0` (or
Video > Asynchronous shaders) to wait for every shader instead.

Sharp game text uses the bundled Open Sans font (SIL Open Font License 1.1) for Latin glyphs, retaining the normal
reconstruction for glyphs the font does not contain. Set `sharp_text_font = 0` in `savagewind.ini` and restart to
use only the normal reconstruction; `sharp_text_font = 1` enables the bundled font.

On DualSense, Minus (the map) defaults to a touchpad click. Create is reserved by the PS5 system and cannot be
bound; an old Create binding falls back to its default. Changing Confirm in Controls updates the action prompts
immediately.

Your imports, saves, settings, generated game code, UI and caches live under `/data/savagewind/`. Generated files are derived from your own game and must not be shared. Back up your saves before removing data.

## Troubleshooting

- **Storage unavailable:** load `storagebroker.elf` after each boot, then restart the title. Ensure ShadowMountPlus is active.
- **Missing or unsupported update:** check that the base package and the matching 1.9.0 update NSP are both in `/data/savagewind/import/`.
- **Keys or package cannot be opened:** check that your own `prod.keys` is present and corresponds to the packages; keys dumped from a Switch on firmware 19.0.0 are recommended. Add your own `title.keys` when required. Do not share keys or packages when asking for help.
- **First start takes a long time:** compilation and shader preparation are expected on first start or after relevant inputs change. Leave the console on and consult the progress screen.

## Update or uninstall

To update, replace the installed title folder and `storagebroker.elf` with the new release and load the new broker after a reboot. Keep `/data/savagewind/` to retain imports, saves and caches. To uninstall, remove the homebrew title and broker ELF; remove `/data/savagewind/` only if you also want to delete your imports, saves, settings and generated data.

