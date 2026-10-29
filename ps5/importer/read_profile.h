// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <time.h>

struct ReadProfile {
    bool active = false;
    unsigned patch_depth = 0;
    uint64_t io_ns = 0, aes_ns = 0, sha_ns = 0, patch_ns = 0;
    uint64_t file_reads = 0, file_bytes = 0;
    uint64_t broker_reads = 0, broker_bytes = 0, broker_ns = 0;
};
extern thread_local ReadProfile savage_read_profile;

inline uint64_t read_profile_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// AES and BKTR include calls into lower layers; subtract their measured child time.
struct ReadProfileScope {
    uint64_t *counter;
    uint64_t start, io, aes;
    bool patch_entered;
    enum Kind { IO, AES, SHA, PATCH } kind;
    explicit ReadProfileScope(Kind k) : counter(nullptr), start(0), io(0), aes(0), patch_entered(false), kind(k) {
        auto &p = savage_read_profile;
        if (!p.active) return;
        if (k == PATCH) {
            patch_entered = true;
            if (p.patch_depth++ != 0) return;
        }
        counter = k == IO ? &p.io_ns : k == AES ? &p.aes_ns : k == SHA ? &p.sha_ns : &p.patch_ns;
        io = p.io_ns; aes = p.aes_ns;
        start = read_profile_now();
    }
    ~ReadProfileScope() {
        auto &p = savage_read_profile;
        if (patch_entered) --p.patch_depth;
        if (!counter) return;
        uint64_t elapsed = read_profile_now() - start;
        uint64_t children = kind == AES ? p.io_ns - io : kind == PATCH ? p.io_ns - io + p.aes_ns - aes : 0;
        *counter += elapsed > children ? elapsed - children : 0;
    }
};
