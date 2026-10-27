# Where ps5/tooling comes from

Copied unchanged; GPL-3.0-or-later (see each file's header). Revisions are in `ps5/deps.lock`.

| Files | Source |
|---|---|
| `native/` (except `ps5-pie-unwind.ld`), `ps5-stubs/`, `prospero-clang18`, `../runtime/libc.prx.sha256` | `../PS5_vkQuake/tooling` (ps5-native-app-boilerplate, BlackBearReloaded) at `3de45e5099` |
| `native/ps5-pie-unwind.ld` | `../forever/PS5_Vulkan/tooling/psbc/ps5-pie-unwind.ld` (Mihawk-99) at `2cf7597741`; adds libunwind's boundary symbols and includes `ps5-pie.ld` for the layout, so link with `-L ps5/tooling/native` |
| `../tools/link-recipe.sh` | ported from `../forever/PS5_Vulkan/tools/radv-link.sh` at `2cf7597741`, writing only under this repository's `build/` |
