#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/settings.h"
#include "../runtime/nvn/output_scale.h"
#include "../runtime/ps5/upscale_label.h"

Settings g_settings;
bool fsr3_available(void) { return true; }
bool framepace_title_ui(void) { return false; }
bool framepace_loading_ui(void) { return false; }
bool framepace_menu_ui(void) { return false; }
bool framepace_in_event(void) { return false; }
uint64_t plat_now_ns(void) { return 0; }

int main(void) {
    const char *names[] = {"Ultra Quality", "Quality", "Balanced", "Performance"};
    const char *expected[2][4] = {
        {"Ultra Quality (2954×1662)", "Quality (2560×1440)", "Balanced (2259×1271)", "Performance (1920×1080)"},
        {"Ultra Quality (1477×831)", "Quality (1280×720)", "Balanced (1129×635)", "Performance (960×540)"}
    };
    for (int s = 0; s < 2; ++s) {
        int w = s ? 1920 : 3840, h = s ? 1080 : 2160;
        for (int u = UPSCALE_FSR1; u <= UPSCALE_FSR3; ++u) {
            if (u != UPSCALE_FSR1 && u != UPSCALE_FSR3) continue;
            g_settings.upscaler = u;
            for (int q = 0; q < UPQ_COUNT; ++q) {
                char label[128], expected_dynamic[128];
                double factor = upscale_mode_factor(q);
                ps5_upscale_label(label, sizeof label, names[q], w, h, factor, 0);
                assert(!strcmp(label, expected[s][q]));
                g_settings.upscale_quality = q;
                OutScale os;
                output_scale(w, h, false, &os);
                char renderer[128];
                snprintf(renderer, sizeof renderer, "%s (%u×%u)", names[q],
                         texture_scaled_extent(1600, (float)os.scene_sx), texture_scaled_extent(900, (float)os.scene_sy));
                assert(!strcmp(label, renderer));
                ps5_upscale_label(label, sizeof label, names[q], w, h, factor, 1);
                snprintf(expected_dynamic, sizeof expected_dynamic, "%s (≤%u×%u)", names[q],
                         texture_scaled_extent(1600, (float)os.scene_sx), texture_scaled_extent(900, (float)os.scene_sy));
                assert(!strcmp(label, expected_dynamic));
            }
        }
        char label[128], native[128];
        ps5_upscale_label(label, sizeof label, "Quality", w, h, 1.0, 0);
        snprintf(native, sizeof native, "Quality (%d×%d)", w, h);
        assert(!strcmp(label, native));
    }
    return 0;
}
