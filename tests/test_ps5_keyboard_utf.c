// Host test: cc -std=c11 -Wall -Wextra -o /tmp/test_ps5_keyboard_utf tests/test_ps5_keyboard_utf.c
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/keyboard_utf.h"

int main(void) {
    static const char text[] = "A\xc3\xa9\xe9\xa6\xac\xf0\x9f\x90\x8e";
    static const uint16_t expected[] = {'A', 0xe9, 0x99ac, 0xd83d, 0xdc0e, 0};
    uint16_t wide[16];
    char utf8[32];
    assert(keyboard_utf8_to_utf16(text, wide, 16, 16) == 5);
    assert(memcmp(wide, expected, sizeof expected) == 0);
    assert(keyboard_utf16_to_utf8(wide, 16, utf8, sizeof utf8, 16) == strlen(text));
    assert(strcmp(utf8, text) == 0);
    assert(keyboard_utf8_to_utf16(text, wide, 5, 16) == 3);
    assert(wide[3] == 0);
    assert(keyboard_utf8_to_utf16(text, wide, 16, 3) == 3);
    assert(wide[3] == 0);
    // A supplementary code point must never be split at a UTF-16 limit.
    assert(keyboard_utf8_to_utf16("\xf0\x9f\x90\x8e", wide, 2, 1) == 0 && wide[0] == 0);
    assert(keyboard_utf8_to_utf16("\xf0\x9f\x90\x8e", wide, 3, 1) == 2);
    assert(keyboard_utf16_to_utf8(expected, 6, utf8, 9, 16) == 6);
    assert(strcmp(utf8, "A\xc3\xa9\xe9\xa6\xac") == 0);
    assert(keyboard_utf16_to_utf8(expected, 6, utf8, sizeof utf8, 2) == 3);
    assert(strcmp(utf8, "A\xc3\xa9") == 0);
    assert(keyboard_utf8_to_utf16("\xf0\x9f", wide, 16, 16) == 2);
    assert(wide[0] == 0xfffd && wide[1] == 0xfffd && wide[2] == 0);
    const uint16_t malformed[] = {0xd800, 'x', 0xdc00, 0};
    assert(keyboard_utf16_to_utf8(malformed, 4, utf8, sizeof utf8, 16) == 7);
    assert(strcmp(utf8, "\xef\xbf\xbdx\xef\xbf\xbd") == 0);
    assert(keyboard_utf16_to_utf8(expected, 1, utf8, sizeof utf8, 16) == 1);
    assert(strcmp(utf8, "A") == 0);
    assert(keyboard_utf8_to_utf16("x", wide, 1, 1) == 0 && wide[0] == 0);
    assert(keyboard_utf16_to_utf8(expected, 6, utf8, 1, 1) == 0 && utf8[0] == 0);
    assert(keyboard_utf8_to_utf16("x", wide, 0, 1) == 0);
    assert(keyboard_utf16_to_utf8(expected, 6, utf8, 0, 1) == 0);
    return 0;
}
