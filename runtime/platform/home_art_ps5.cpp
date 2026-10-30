// SPDX-License-Identifier: GPL-3.0-or-later
// Home-screen images are derived on the console from the player's own game packages.
#include <cstddef>
#include "home_art_ps5.h"
#include "../imageload.h"
#include "../paths.h"
#include "../nvn/hd_ui_dds.h"
#include "../nvn/hd_ui_extract.h"
#include "../nvn/hd_ui_ncnn.h"
#include "../nvn/hd_ui_pack_source.h"
extern "C" {
#include "../storage_ps5.h"
}
#include "../../ps5/storagebroker/client.h"
#include <mbedtls/sha256.h>
#include <bc7decomp.h>
#include <stb_image.h>
#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" struct RomfsImage *fs_game_romfs(void);

namespace {
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> pixels;
    uint64_t hash = 0;
};

struct Filter {
    int first, count;
    double weight[8];
};

static double lanczos(double x) {
    constexpr double pi = 3.14159265358979323846;
    x = std::abs(x);
    if (x < 1e-9) return 1;
    if (x >= 3) return 0;
    return std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x / 3);
}

static std::vector<Filter> filters(uint32_t from, uint32_t to) {
    std::vector<Filter> result(to);
    double scale = double(from) / to;
    for (uint32_t i = 0; i < to; ++i) {
        double center = (i + 0.5) * scale - 0.5;
        int first = std::max(0, int(std::ceil(center - 3)));
        int last = std::min(int(from) - 1, int(std::floor(center + 3)));
        Filter &f = result[i];
        f.first = first;
        f.count = last - first + 1;
        double sum = 0;
        for (int j = 0; j < f.count; ++j) sum += f.weight[j] = lanczos(first + j - center);
        for (int j = 0; j < f.count; ++j) f.weight[j] /= sum;
    }
    return result;
}

// Upsampling only; filter coefficients are evaluated once per output coordinate.
static Image resize(const Image &input, uint32_t width, uint32_t height) {
    if (!input.width || !input.height || width < input.width || height < input.height)
        return {};
    auto x = filters(input.width, width);
    auto y = filters(input.height, height);
    std::vector<float> rows(size_t(width) * input.height * 4);
    Image output{width, height, std::vector<unsigned char>(size_t(width) * height * 4)};
    for (uint32_t sy = 0; sy < input.height; ++sy) {
        for (uint32_t dx = 0; dx < width; ++dx) {
            for (unsigned c = 0; c < 4; ++c) {
                double value = 0;
                for (int j = 0; j < x[dx].count; ++j)
                    value += input.pixels[(size_t(sy) * input.width + x[dx].first + j) * 4 + c] * x[dx].weight[j];
                rows[(size_t(sy) * width + dx) * 4 + c] = float(value);
            }
        }
    }
    for (uint32_t dy = 0; dy < height; ++dy) {
        for (uint32_t dx = 0; dx < width; ++dx) {
            for (unsigned c = 0; c < 4; ++c) {
                double value = 0;
                for (int j = 0; j < y[dy].count; ++j)
                    value += rows[(size_t(y[dy].first + j) * width + dx) * 4 + c] * y[dy].weight[j];
                output.pixels[(size_t(dy) * width + dx) * 4 + c] =
                    static_cast<unsigned char>(std::clamp(int(std::floor(value + 0.5)), 0, 255));
            }
        }
    }
    return output;
}

static int texture(void *context, const HdUiTexture *t) {
    Image *image = static_cast<Image *>(context);
    if (image->width || t->channels != 4) return -1;
    image->width = t->width;
    image->height = t->height;
    image->hash = t->hash;
    image->pixels.assign(t->pixels, t->pixels + size_t(t->width) * t->height * 4);
    return 0;
}

struct Wanted { const char *name; Image *image; };
static int layout(void *context, const char *, const char *, size_t, const uint8_t *archive, size_t size) {
    Wanted *wanted = static_cast<Wanted *>(context);
    return hd_ui_extract_layout_named(archive, size, wanted->name, texture, wanted->image);
}

static bool read_file(const char *path, std::vector<unsigned char> &out) {
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    bool ok = !std::fseek(file, 0, SEEK_END);
    long size = ok ? std::ftell(file) : -1;
    ok = size >= 0 && size <= 16 * 1024 * 1024 && !std::fseek(file, 0, SEEK_SET);
    if (ok) {
        out.resize(size_t(size));
        ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok;
}

static uint32_t get32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// HD UI files are indexed by the exact original texture hash; reject unrelated or malformed files.
static void prefer_hd(Image &image, uint32_t max_width, uint32_t max_height) {
    char relative[64], path[1024];
    std::snprintf(relative, sizeof relative, "hd-ui/%016llx.dds", (unsigned long long)image.hash);
    if (!rt_data_path(relative, path, sizeof path)) return;
    std::vector<unsigned char> bytes;
    if (!read_file(path, bytes) || bytes.size() < 148 || std::memcmp(bytes.data(), "DDS ", 4) ||
        get32(bytes.data() + 4) != 124 || std::memcmp(bytes.data() + 84, "DX10", 4)) return;
    uint32_t width = get32(bytes.data() + 16), height = get32(bytes.data() + 12);
    uint32_t format = get32(bytes.data() + 128);
    if (width != image.width * 2 || height != image.height * 2 ||
        width > max_width || height > max_height || (format != 98 && format != 99) ||
        get32(bytes.data() + 132) != 3 || get32(bytes.data() + 140) != 1) return;
    size_t blocks = size_t((width + 3) / 4) * ((height + 3) / 4);
    if (blocks > (bytes.size() - 148) / 16) return;
    Image hd{width, height, std::vector<unsigned char>(size_t(width) * height * 4)};
    for (uint32_t by = 0; by < (height + 3) / 4; ++by) {
        for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
            bc7decomp::color_rgba pixels[16];
            size_t index = size_t(by) * ((width + 3) / 4) + bx;
            if (!bc7decomp::unpack_bc7(bytes.data() + 148 + index * 16, pixels)) return;
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    if (bx * 4 + x >= width || by * 4 + y >= height) continue;
                    std::memcpy(&hd.pixels[(size_t(by * 4 + y) * width + bx * 4 + x) * 4],
                                pixels[y * 4 + x].m_comps, 4);
                }
            }
        }
    }
    image = std::move(hd);
    std::fprintf(stderr, "[home-art] using HD UI texture %s\n", relative);
}

static bool icon(Image &out) {
    static const char *const languages[] = {
        "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish",
        "Spanish", "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean",
        "Taiwanese", "Chinese", "BrazilianPortuguese"
    };
    std::vector<unsigned char> jpeg;
    char why[256] = {};
    std::vector<unsigned char> nacp(0x4000);
    ssize_t nacp_size = fs_control_read("control.nacp", nacp.data(), nacp.size(), why, sizeof why);
    if (nacp_size < 0x3000 || size_t(nacp_size) > nacp.size()) return false;
    size_t language_index = 0;
    for (const char *language : languages) {
        bool present = std::any_of(nacp.begin() + language_index * 0x300,
                                   nacp.begin() + language_index * 0x300 + 0x200,
                                   [](unsigned char c) { return c != 0; });
        ++language_index;
        if (!present) continue;
        std::string name = std::string("icon_") + language + ".dat";
        ssize_t size = fs_control_read(name.c_str(), nullptr, 0, why, sizeof why);
        if (size <= 0 || size > 1024 * 1024) continue;
        jpeg.resize(size_t(size));
        if (fs_control_read(name.c_str(), jpeg.data(), jpeg.size(), why, sizeof why) != size) return false;
        break;
    }
    if (jpeg.empty()) {
        std::fprintf(stderr, "[home-art] no Control NCA icon: %s\n", why);
        return false;
    }
    int width, height, channels;
    unsigned char *pixels = stbi_load_from_memory(jpeg.data(), int(jpeg.size()), &width, &height, &channels, 4);
    if (!pixels || width != 256 || height != 256) {
        stbi_image_free(pixels);
        return false;
    }
    Image source{256, 256, std::vector<unsigned char>(pixels, pixels + 256 * 256 * 4)};
    stbi_image_free(pixels);
    char param2[1024], param4[1024], weights[1024];
    if (rt_resource("realesr-animevideov3-x2.param", nullptr, param2, sizeof param2) &&
        rt_resource("realesr-animevideov3-x4.param", nullptr, param4, sizeof param4) &&
        rt_resource("realesr-animevideov3-x4.bin", nullptr, weights, sizeof weights)) {
        HdUiNcnn *upscaler = hd_ui_ncnn_create(param2, param4, weights);
        if (upscaler) {
            std::vector<unsigned char> rgb(256 * 256 * 3), enlarged(512 * 512 * 3);
            for (size_t i = 0; i < 256 * 256; ++i)
                std::memcpy(rgb.data() + i * 3, source.pixels.data() + i * 4, 3);
            if (!hd_ui_ncnn_upscale(upscaler, rgb.data(), 256, 256, 2, enlarged.data())) {
                out = {512, 512, std::vector<unsigned char>(512 * 512 * 4)};
                for (size_t i = 0; i < 512 * 512; ++i) {
                    std::memcpy(out.pixels.data() + i * 4, enlarged.data() + i * 3, 3);
                    out.pixels[i * 4 + 3] = 255;
                }
            }
            hd_ui_ncnn_destroy(upscaler);
        }
    }
    if (out.pixels.empty()) out = resize(source, 512, 512);
    return !out.pixels.empty();
}

static bool background(Image &out) {
    Image base, logo;
    Wanted background_wanted{"TitleBGMainA_00^o", &base};
    Wanted logo_wanted{"Logo_W_00^l", &logo};
    if (fs_hd_ui_layouts_in_pack("Pack/Title.pack", layout, &background_wanted) ||
        fs_hd_ui_layouts_in_pack("Pack/Title.pack", layout, &logo_wanted) ||
        !base.width || !logo.width || base.width > 3840 || base.height > 2160 ||
        logo.width > 768 || logo.height > 603) return false;
    prefer_hd(base, 3840, 2160);
    prefer_hd(logo, 768, 603);
    out = resize(base, 3840, 2160);
    // Title_00.bflyt: Logo_W_00 is at (466, 230), scale .67 on a 1280x720 canvas.
    Image scaled_logo = resize(logo, 768, 603);
    if (out.pixels.empty() || scaled_logo.pixels.empty()) return false;
    for (uint32_t y = 0; y < scaled_logo.height; ++y) {
        for (uint32_t x = 0; x < scaled_logo.width; ++x) {
            const unsigned char *src = &scaled_logo.pixels[(size_t(y) * scaled_logo.width + x) * 4];
            unsigned char *dst = &out.pixels[(size_t(y + 88) * out.width + x + 2934) * 4];
            unsigned a = src[3];
            for (unsigned c = 0; c < 3; ++c) dst[c] = (src[c] * a + dst[c] * (255 - a) + 127) / 255;
            dst[3] = 255;
        }
    }
    return true;
}

static bool install(sb_client &client, sb_home_art_kind kind, const char *name,
                    const std::vector<unsigned char> &bytes, unsigned char digest[32]) {
    int rc = mbedtls_sha256_ret(bytes.data(), bytes.size(), digest, 0);
    if (!rc) rc = sb_install_home_art(&client, kind, bytes.data(), uint32_t(bytes.size()));
    if (rc) std::fprintf(stderr, "[home-art] %s install failed: %d\n", name, rc);
    return rc == 0;
}
}

extern "C" int home_art_generate(int enabled, unsigned char digests[3][32]) {
    if (!digests) return -1;
    std::memset(digests, 0, 3 * 32);
    const char *stage = "initialization";
    try {
        std::vector<unsigned char> png, pic0;
        if (enabled) {
            stage = "package and texture extraction";
            Image source_icon, picture;
            if (!fs_game_romfs() || !icon(source_icon) || !background(picture)) throw 1;
            stage = "PNG and DDS encoding";
            size_t png_size = 0, dds_size = 0;
            unsigned char *png_data = rt_png_encode(source_icon.pixels.data(), 512, 512, &png_size);
            unsigned char *dds_data = nullptr;
            if (!png_data || hd_ui_dds_encode_single(picture.pixels.data(), 3840, 2160, &dds_data, &dds_size)) {
                std::free(png_data);
                std::free(dds_data);
                throw 1;
            }
            png.assign(png_data, png_data + png_size);
            pic0.assign(dds_data, dds_data + dds_size);
            std::free(png_data);
            std::free(dds_data);
            stage = "splash PNG encoding";
            size_t splash_size = 0;
            unsigned char *splash = rt_png_encode(picture.pixels.data(), 3840, 2160, &splash_size);
            if (!splash) throw 1;
            stage = "splash cache write";
            int rc = rt_storage_mkdirs("cache/home-art");
            if (!rc) rc = rt_storage_replace("cache/home-art/splash.png", splash, splash_size);
            std::free(splash);
            if (rc < 0) throw 1;
        } else {
            stage = "shipped art read";
            if (!read_file("/app0/home-art-original-icon0.png", png)) throw 1;
        }
        stage = "broker connection";
        sb_client client{-1, 0};
        if (sb_connect(&client)) throw 1;
        stage = "broker art installation";
        bool ok = install(client, SB_HOME_ART_ICON0, "icon0.png", png, digests[0]);
        if (ok && enabled) {
            ok = install(client, SB_HOME_ART_PIC0, "pic0.dds", pic0, digests[1]) &&
                 install(client, SB_HOME_ART_PIC1, "pic1.dds", pic0, digests[2]);
        } else if (ok) {
            int rc = sb_remove_home_backgrounds(&client);
            if (rc) std::fprintf(stderr, "[home-art] background removal failed: %d\n", rc);
            ok = rc == 0;
        }
        sb_close(&client);
        if (!ok) throw 1;
        if (!enabled) {
            stage = "splash cache removal";
            int rc = rt_storage_unlink("cache/home-art/splash.png");
            if (rc < 0 && rc != -ENOENT) throw 1;
        }
        std::fprintf(stderr, "[home-art] %s installed\n", enabled ? "generated art" : "shipped art");
        return 0;
    } catch (...) {
        std::fprintf(stderr, "[home-art] %s failed\n", stage);
        return -1;
    }
}
