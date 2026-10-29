// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

struct PatchContext {
    const uint8_t *image;
    size_t size;
    uint64_t base;
    const std::map<std::string, uint64_t> &sites;
};

std::string apply_patch(const PatchContext &ctx, const std::string &name, uint64_t address);
std::map<uint64_t, std::string> make_patches(const PatchContext &ctx);
