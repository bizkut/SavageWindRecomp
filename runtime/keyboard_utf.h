// UTF-8 / UTF-16 conversion for software keyboard text.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RUNTIME_KEYBOARD_UTF_H
#define RUNTIME_KEYBOARD_UTF_H

#include <stddef.h>
#include <stdint.h>

// Capacities include the terminator; malformed input becomes U+FFFD. Never split a character.
static inline size_t keyboard_utf8_to_utf16(const char *src, uint16_t *dst, size_t cap, size_t max_chars) {
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    size_t used = 0, chars = 0;
    if (!cap) return 0;
    while (*p && chars < max_chars) {
        uint32_t c;
        size_t n;
        if (*p < 0x80) { c = *p; n = 1; }
        else if (*p >= 0xc2 && *p <= 0xdf) { c = *p & 0x1f; n = 2; }
        else if (*p >= 0xe0 && *p <= 0xef) { c = *p & 0x0f; n = 3; }
        else if (*p >= 0xf0 && *p <= 0xf4) { c = *p & 7; n = 4; }
        else { c = 0xfffd; n = 1; }
        if (n > 1) {
            size_t i = 1;
            while (i < n && p[i] && (p[i] & 0xc0) == 0x80) {
                c = (c << 6) | (p[i] & 0x3f);
                i++;
            }
            if (i != n || (n == 2 && c < 0x80) || (n == 3 && c < 0x800) ||
                (n == 4 && c < 0x10000) || (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) {
                c = 0xfffd;
                n = 1;
            }
        }
        size_t units = c >= 0x10000 ? 2 : 1;
        if (units > cap - 1 - used) break;
        if (units == 2) {
            c -= 0x10000;
            dst[used++] = (uint16_t)(0xd800 + (c >> 10));
            dst[used++] = (uint16_t)(0xdc00 + (c & 0x3ff));
        } else {
            dst[used++] = (uint16_t)c;
        }
        p += n;
        chars++;
    }
    dst[used] = 0;
    return used;
}

static inline size_t keyboard_utf16_to_utf8(const uint16_t *src, size_t src_cap, char *dst, size_t cap,
                                            size_t max_chars) {
    size_t pos = 0, used = 0, chars = 0;
    if (!cap) return 0;
    while (pos < src_cap && src[pos] && chars < max_chars) {
        uint32_t c = src[pos++];
        if (c >= 0xd800 && c <= 0xdbff && pos < src_cap && src[pos] >= 0xdc00 && src[pos] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (src[pos++] - 0xdc00);
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n > cap - 1 - used) break;
        if (n == 1) dst[used++] = (char)c;
        else {
            if (n == 2) dst[used++] = (char)(0xc0 | (c >> 6));
            else if (n == 3) dst[used++] = (char)(0xe0 | (c >> 12));
            else dst[used++] = (char)(0xf0 | (c >> 18));
            if (n == 4) dst[used++] = (char)(0x80 | ((c >> 12) & 0x3f));
            if (n >= 3) dst[used++] = (char)(0x80 | ((c >> 6) & 0x3f));
            dst[used++] = (char)(0x80 | (c & 0x3f));
        }
        chars++;
    }
    dst[used] = 0;
    return used;
}
#endif
