// PS5 first-start generation of the user's UI texture pack.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool hd_ui_generate_needed(void);
bool hd_ui_generate_start(void);
void hd_ui_generate_shaders_done(void);
// True while generation is active. done/total count unique selected textures.
bool hd_ui_generate_progress(int *done, int *total);
void hd_ui_generate_join(void);
#ifdef __cplusplus
}
#endif
