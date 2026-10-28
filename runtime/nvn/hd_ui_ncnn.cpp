// Real-ESRGAN animevideov3: release-compatible tiling and reflected prepadding.
#include "hd_ui_ncnn.h"
#include "net.h"
#include "gpu.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <cstdio>

struct HdUiNcnn { ncnn::Net net[2]; };

HdUiNcnn *hd_ui_ncnn_create(const char *param2, const char *param4, const char *weights) {
    if (!param2 || !param4 || !weights || ncnn::get_gpu_count() <= 0) return nullptr;
    HdUiNcnn *u = new (std::nothrow) HdUiNcnn;
    if (!u) return nullptr;
    std::fprintf(stderr, "[hd-ui-gen] GPU device: %s\n", ncnn::get_gpu_info(ncnn::get_default_gpu_index()).device_name());
    const char *params[] = {param2, param4};
    for (int i = 0; i < 2; ++i) {
        ncnn::Net &n = u->net[i];
        n.opt.num_threads = 1;
        n.opt.use_vulkan_compute = true;
        n.opt.use_fp16_packed = true;
        n.opt.use_fp16_storage = true;
        n.opt.use_fp16_arithmetic = false;
        n.opt.use_int8_storage = false;
        n.opt.use_int8_arithmetic = false;
        n.set_vulkan_device(ncnn::get_default_gpu_index());
        if (n.load_param(params[i]) != 0 || n.load_model(weights) != 0) {
            delete u;
            return nullptr;
        }
    }
    return u;
}

void hd_ui_ncnn_destroy(HdUiNcnn *u) { delete u; }

static int reflect(int x, int size) {
    x = std::abs(x);
    return size - 1 - std::abs(x - (size - 1));
}

int hd_ui_ncnn_upscale(HdUiNcnn *u, const uint8_t *rgb, uint32_t width, uint32_t height,
                       unsigned scale, uint8_t *output) {
    if (!u || !rgb || !output || width < 16 || height < 16 || width > 1024 || height > 1024 ||
        (scale != 2 && scale != 4)) return -1;
    constexpr int tile = 200, pad = 10;
    ncnn::Net &net = u->net[scale == 2 ? 0 : 1];
    for (int y = 0; y < (int)height; y += tile) for (int x = 0; x < (int)width; x += tile) {
        int tw = std::min(tile, (int)width - x), th = std::min(tile, (int)height - y);
        ncnn::Mat in(tw + 2 * pad, th + 2 * pad, 3);
        if (in.empty()) return -1;
        for (int c = 0; c < 3; ++c) {
            float *dst = in.channel(c);
            for (int yy = 0; yy < th + 2 * pad; ++yy) {
                int sy = reflect(y + yy - pad, (int)height);
                for (int xx = 0; xx < tw + 2 * pad; ++xx) {
                    int sx = reflect(x + xx - pad, (int)width);
                    dst[(size_t)yy * (tw + 2 * pad) + xx] = rgb[((size_t)sy * width + sx) * 3 + c] * (1.f / 255.f);
                }
            }
        }
        ncnn::Extractor ex = net.create_extractor();
        ex.set_num_threads(1);
        ncnn::Mat out;
        if (ex.input("data", in) != 0 || ex.extract("output", out) != 0 || out.empty() ||
            out.w != (tw + 2 * pad) * (int)scale || out.h != (th + 2 * pad) * (int)scale || out.c != 3)
            return -1;
        for (int c = 0; c < 3; ++c) {
            const float *src = out.channel(c);
            for (int yy = 0; yy < th * (int)scale; ++yy) for (int xx = 0; xx < tw * (int)scale; ++xx) {
                float f = src[(size_t)(yy + pad * scale) * out.w + xx + pad * scale];
                int v = std::isfinite(f) ? (int)std::floor(f * 255.f + 0.5f) : 0;
                output[((size_t)(y * scale + yy) * width * scale + x * scale + xx) * 3 + c] =
                    (uint8_t)std::max(0, std::min(255, v));
            }
        }
    }
    return 0;
}
