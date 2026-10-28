// Menu languages' fonts (i18n.h ui_merge_fallback_font): the ImGui half of runtime/ui/i18n.mm, for the UI contexts
// that draw the start screen and the overlay menu.
#include "i18n.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "imgui.h"
extern "C" {
#include "../paths.h"
}

// Per script: system fonts that have it (the first one found)
static const char *const cyrillic_fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                             "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc", nullptr};
static const char *const ja_fonts[] = {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",
                                       "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                       "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                       "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc", nullptr};
static const char *const ko_fonts[] = {"C:/Windows/Fonts/malgun.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                       "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                       "/System/Library/Fonts/AppleSDGothicNeo.ttc", nullptr};
static const char *const zh_hans_fonts[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc",
                                            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc", nullptr};
static const char *const zh_hant_fonts[] = {"C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/mingliu.ttc",
                                            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc", nullptr};

// Thai: Noto Sans Thai, shipped (third_party/fonts/NotoSansThai, OFL: neither Fira Sans nor the CJK fonts have Thai), then
// the system's
static const char *const th_fonts[] = {"C:/Windows/Fonts/LeelawUI.ttf", "C:/Windows/Fonts/tahoma.ttf",
                                       "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
                                       "/usr/share/fonts/noto/NotoSansThai-Regular.ttf",
                                       "/System/Library/Fonts/Supplemental/Ayuthaya.ttf", nullptr};

// (Noto Sans CJK .ttc: face 0 is Japanese, 1 Korean, 2 Simplified, 3 Traditional Chinese)
static int ttc_face(const char *path, const char *lang) {
    if (!strstr(path, "NotoSansCJK")) return 0;
    return !strcmp(lang, "ko") ? 1 : !strcmp(lang, "zh-Hans") ? 2 : !strcmp(lang, "zh-Hant") ? 3 : 0;
}

bool ui_merge_fallback_font(ImFontAtlas *atlas, float size, const ImFontConfig *base) {
    const char *lang = ui_lang();
    const char *const *fonts = !strcmp(lang, "ru") ? cyrillic_fonts : !strcmp(lang, "ja") ? ja_fonts
                             : !strcmp(lang, "ko") ? ko_fonts : !strcmp(lang, "zh-Hans") ? zh_hans_fonts
                             : !strcmp(lang, "zh-Hant") ? zh_hant_fonts : !strcmp(lang, "th") ? th_fonts : nullptr;
    if (!fonts) return false;
    static char shipped[1100];
    if (fonts == th_fonts && !rt_resource("NotoSansThai-Medium.ttf", "third_party/fonts/NotoSansThai/NotoSansThai-Medium.ttf",
                                          shipped, sizeof shipped))
        shipped[0] = 0;
    // only the glyphs the language's texts use (a CJK font's full range would not fit an atlas texture), in both cases:
    // option labels are drawn upper-cased (tr_upper), with capitals the texts themselves may never use
    static ImVector<ImWchar> ranges;
    static int ranges_gen = -1;
    if (ranges_gen != ui_lang_generation()) {
        ranges_gen = ui_lang_generation();
        ImFontGlyphRangesBuilder b;
        ui_lang_texts([](const char *text, void *ctx) {
            auto *rb = (ImFontGlyphRangesBuilder *)ctx;
            rb->AddText(text), rb->AddText(tr_upper(text).c_str());
        }, &b);
        b.AddText("0123456789%()/:.,-+ ");
        ranges.clear();
        b.BuildRanges(&ranges);
    }
    for (int i = -1; i < 0 || fonts[i]; i++) {
        const char *path = i < 0 ? (fonts == th_fonts ? shipped : "") : fonts[i];  // (-1: a shipped font)
        FILE *f = *path ? fopen(path, "rb") : nullptr;
        if (!f) continue;
        fclose(f);
        ImFontConfig cfg = base ? *base : ImFontConfig();
        cfg.MergeMode = true;
        cfg.FontNo = ttc_face(path, lang);
        cfg.FontDataOwnedByAtlas = true;
        atlas->AddFontFromFileTTF(path, size, &cfg, ranges.Data);
        return true;
    }
    return false;
}
