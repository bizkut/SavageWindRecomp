// PS5 first-start home-screen art generated from the player's own packages.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Four CNMT content hashes: base/update program, then base/update control.
int ps5_exefs_home_art_identity(unsigned char out[128]);
// Runs on an 8 MiB worker stack. Digests correspond to icon0, pic0 and pic1.
int home_art_generate(int enabled, unsigned char digests[3][32]);
#ifdef __cplusplus
}
#endif
