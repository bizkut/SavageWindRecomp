// Behavioral checks for GAMECODE_CONSOLE settings and ordinary-build isolation.
#include "../runtime/settings.h"
#include "../runtime/language.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const TextLanguage g_text_languages[] = {{"Auto", "", NULL}};
const int g_ntext_languages = 1;
const VoiceLanguage g_voice_languages[] = {{"Text", NULL}};
const int g_nvoice_languages = 1;
const char *rt_data_root(void) { return ""; }
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *file) { (void)file; }
bool fsr3_upscaler_enabled(void) { return false; }

int main(int argc, char **argv) {
    assert(argc == 5);
    assert(setenv("SAVAGEWIND_SETTINGS", argv[1], 1) == 0);
#ifdef GAMECODE_CONSOLE
    assert(!g_settings.gamecode_overlap && g_settings.gamecode_threads == 8);
#endif
    settings_load();
#ifdef GAMECODE_CONSOLE
    assert(g_settings.gamecode_overlap == (atoi(argv[2]) != 0));
    assert(g_settings.gamecode_threads == atoi(argv[3]));
#endif
    if (atoi(argv[4])) {
        settings_save();
        FILE *f = fopen(argv[1], "r");
        assert(f);
        char line[512];
        int overlap = 0, threads = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "gamecode_overlap = ", 19)) overlap++;
            if (!strncmp(line, "gamecode_threads = ", 19)) threads++;
        }
        assert(fclose(f) == 0);
#ifdef GAMECODE_CONSOLE
        assert(overlap == (atoi(argv[2]) != 0));
        assert(threads == (atoi(argv[3]) != 8));
#else
        assert(overlap == 0 && threads == 0);
#endif
    }
    puts("gamecode settings test passed");
    return 0;
}
