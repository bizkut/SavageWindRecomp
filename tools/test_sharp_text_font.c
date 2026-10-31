#include "../runtime/settings.h"
#include "../runtime/language.h"
#include <assert.h>
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
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(setenv("SAVAGEWIND_SETTINGS", argv[1], 1) == 0);
    FILE *f = fopen(argv[1], "w");
    assert(f);
    fputs("sharp_text_font = 1\n", f);
    assert(fclose(f) == 0);
    settings_load();
    assert(g_settings.sharp_text_font);
    g_settings.sharp_text_font = false;
    settings_save();
    f = fopen(argv[1], "r");
    assert(f);
    char line[256];
    bool found = false;
    while (fgets(line, sizeof line, f))
        if (!strcmp(line, "sharp_text_font = 0\n")) found = true;
    assert(fclose(f) == 0);
    assert(found);
    return 0;
}
