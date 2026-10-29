// SPDX-License-Identifier: GPL-3.0-or-later
#include "patches.h"
#include <sstream>
#include <stdexcept>

namespace {
std::string hex(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

uint32_t word(const PatchContext &ctx, uint64_t address)
{
    if (address < ctx.base || address - ctx.base > ctx.size || ctx.size - (address - ctx.base) < 4)
        throw std::runtime_error("patch instruction outside image: " + hex(address));
    const uint8_t *p = ctx.image + (address - ctx.base);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void validate(bool ok, uint64_t address, uint32_t instruction)
{
    if (!ok)
        throw std::runtime_error("invalid patch instruction " + hex(instruction) + " at " + hex(address));
}

int64_t sext(uint64_t value, unsigned bits)
{
    return int64_t(value ^ (uint64_t(1) << (bits - 1))) - int64_t(uint64_t(1) << (bits - 1));
}

uint64_t adrp_page(uint32_t w, uint64_t a)
{
    uint32_t immediate = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2);
    return (a & ~uint64_t(0xfff)) + sext(immediate, 21) * 4096;
}

std::string patch_fn(const PatchContext &ctx, const std::string &name)
{
    return "f_" + hex(ctx.sites.at(name)).substr(2);
}

uint32_t previous_aim_word(const PatchContext &ctx, uint64_t a)
{
    for (unsigned i = 1; i <= 4; ++i) {
        uint32_t w = word(ctx, a - 4 * i);
        if ((w & 0xffc003ff) == 0xf9400268)
            return w;
    }
    throw std::runtime_error("camera pitch parameter load not found at " + hex(a));
}

#include "patches_generated.h"
}

std::string apply_patch(const PatchContext &ctx, const std::string &name, uint64_t address)
{
    return generated_patches::apply(ctx, name, address);
}

std::map<uint64_t, std::string> make_patches(const PatchContext &ctx)
{
    std::map<uint64_t, std::string> result;
    for (const char *name : generated_patches::names) {
        auto site = ctx.sites.find(name);
        if (site != ctx.sites.end())
            result[site->second] = name;
    }
    return result;
}
