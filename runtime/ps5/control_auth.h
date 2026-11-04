// PS5 control socket first-line authentication, independent of transport.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONTROL_TOKEN_HEX 64
#define CONTROL_AUTH_TIMEOUT_NS 5000000000ull

typedef struct {
    uint64_t accepted_ns;
    bool authenticated;
} ControlAuth;

static inline bool control_auth_expired(const ControlAuth *auth, uint64_t now) {
    return !auth->authenticated && now - auth->accepted_ns >= CONTROL_AUTH_TIMEOUT_NS;
}

static inline bool control_auth_line(ControlAuth *auth, const char *line, size_t len,
                                     const char token[CONTROL_TOKEN_HEX], uint64_t now) {
    if (auth->authenticated || control_auth_expired(auth, now) || len != 5 + CONTROL_TOKEN_HEX ||
        memcmp(line, "auth ", 5)) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < CONTROL_TOKEN_HEX; i++)
        diff |= (unsigned char)line[5 + i] ^ (unsigned char)token[i];
    if (diff) return false;
    auth->authenticated = true;
    return true;
}
