// Real-ESRGAN animevideov3 inference for user-owned layout pixels.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct HdUiNcnn HdUiNcnn;
// Paths name the pinned x2/x4 ncnn graphs and their shared weight file.
HdUiNcnn *hd_ui_ncnn_create(const char *param2, const char *param4, const char *weights);
void hd_ui_ncnn_destroy(HdUiNcnn *upscaler);
// RGB input and output are tightly packed, output is width*scale by height*scale.
// Caller owns output; only scales 2 and 4 are supported. Returns 0 on success.
int hd_ui_ncnn_upscale(HdUiNcnn *upscaler, const uint8_t *rgb, uint32_t width, uint32_t height,
                       unsigned scale, uint8_t *output);
#ifdef __cplusplus
}
#endif
