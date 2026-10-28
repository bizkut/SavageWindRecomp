// Savage Wind's pages of the game's Options screen (runtime/ui/game_options.mm): their row model, and the Video page's
// rows every platform has. Each platform composes its own Video page from them and its own rows (D2: the desktop's
// window, VSync, HDR and frame generation in runtime/ui/video_page.mm; the PS5's display resolution in
// runtime/ps5/video_page.cpp).
#pragma once
#include <string>
#include <vector>

namespace opts {

enum Page { P_MAIN, P_GAMEPLAY, P_VIDEO, P_AUDIO, P_CONTROLS, P_KEYS, P_STICKS, P_PAD, P_LANGUAGE, P_COUNT };
enum Action { A_NONE, A_PAGE, A_BIND_KEY, A_BIND_PAD, A_RESET_KEYS, A_RESET_PAD, A_RESTART, A_QUIT, A_SIZE_LIST };
enum { KIND_TOGGLE = 0, KIND_CHOICE = 1, KIND_BUTTON = 2 };

struct Item {
    int page;
    int kind;
    const char *title, *desc;
    const char *button;                          // KIND_BUTTON: its text (NULL: label(item, -1))
    int (*count)(const Item &);                  // KIND_CHOICE: number of values
    const char *(*label)(const Item &, int v);   // KIND_CHOICE: value v's text
    int (*get)(const Item &);
    void (*set)(const Item &, int v);
    int def;                                     // the default value ("reset to defaults")
    int action, arg;                             // KIND_BUTTON
    const char *(*desc_fn)(const Item &);        // a description made when shown (NULL: desc)
    void (*reset)(const Item &);                 // "reset to defaults" when it isn't set(def)
    bool (*value_off)(const Item &, int v);      // KIND_CHOICE: value v can't be picked yet (shown greyed)
    bool wrap;                                   // KIND_CHOICE: a step past either end goes to the other one
    const int *values;                           // KIND_CHOICE: the setting's value per choice (rows that have them)
    int nvalues;
};

Item toggle(int page, const char *title, const char *desc, int (*get)(const Item &), void (*set)(const Item &, int),
            int def);
Item choice(int page, const char *title, const char *desc, int (*count)(const Item &),
            const char *(*label)(const Item &, int), int (*get)(const Item &), void (*set)(const Item &, int), int def);
Item button(int page, const char *title, const char *desc, const char *text, int action, int arg);
int nearest(const int *v, int n, float x);  // the index of the value nearest x
std::string pct(int v);                     // "25%"
// A value that can't be picked yet: its name and why ("(coming soon)"), translated
const char *soon(const char *name, const char *why = "(coming soon)");

// ---- the Video page's rows every platform has
Item video_wide_view();
Item video_field_of_view();
Item video_frame_rate(const int *rates, int n);  // frames per second per choice (0: Off, no limit)
Item video_upscaler(const int *upscalers, int n);  // UPSCALE_* per choice
Item video_upscaler_mode();
Item video_antialiasing();
Item video_anisotropy();
Item video_sharpening();
Item video_async_shaders();
Item video_shadows();

// The platform's Video page, in order
void video_page(std::vector<Item> &v);

}  // namespace opts
