// PS5 first-start generation of the user's UI texture pack.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool hd_ui_generate_needed(void);
bool hd_ui_generate_start(void);
// Release package scanning after the shader archive read phase.
void hd_ui_generate_shader_reads_done(void);
void hd_ui_generate_shaders_done(void);
// Loading-screen changes are advisory; in-flight texture work may finish.
void hd_ui_generate_set_loading(bool loading);
// True while generation is active. done/total count unique selected textures.
bool hd_ui_generate_progress(int *done, int *total);
void hd_ui_generate_join(void);
#ifdef __cplusplus
}
#endif
