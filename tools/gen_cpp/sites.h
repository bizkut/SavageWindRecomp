// SPDX-License-Identifier: GPL-3.0-or-later
// Include after the generator's Nso definition.
#pragma once
#include <functional>
#include <optional>

namespace game_sites {
struct Version {
    const char *name, *id;
    bool supported;
};
struct Pattern {
    const char *text;
    unsigned at;
};
struct Site {
    const char *name, *kind;
    std::vector<Pattern> patterns;
};
#include "sites_data.h"

struct Result {
    std::string version;
    std::map<std::string, U> sites;
    std::map<std::string, bool> fallback;
    std::vector<bool> groups;
};

using Log = std::function<void(const std::string &)>;

static std::string hex(U value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

static int64_t sext(U v, unsigned bits)
{
    return int64_t(v ^ (U(1) << (bits - 1))) - int64_t(U(1) << (bits - 1));
}

static bool is_adrp(uint32_t w)
{
    return (w & 0x9f000000) == 0x90000000;
}

static U adrp_page(uint32_t w, U pc)
{
    return (pc & ~U(0xfff)) + sext(((w >> 5) & 0x7ffff) * 4 | ((w >> 29) & 3), 21) * 4096;
}

static std::optional<std::pair<U, unsigned>> lo12(uint32_t w)
{
    if ((w & 0xffc00000) == 0x91000000)
        return std::make_pair(U((w >> 10) & 0xfff), (w >> 5) & 31);
    if ((w & 0x3b000000) == 0x39000000) {
        unsigned scale = (w >> 30) & 3;
        if ((w & 0x04000000) && ((w >> 23) & 1))
            scale = 4;
        return std::make_pair(U((w >> 10) & 0xfff) << scale, (w >> 5) & 31);
    }
    return std::nullopt;
}

static std::optional<U> imm_value(uint32_t w)
{
    if ((w & 0x3b000000) == 0x39000000)
        return lo12(w)->first;
    if ((w & 0x1f000000) == 0x11000000)
        return U((w >> 10) & 0xfff) << (((w >> 22) & 1) ? 12 : 0);
    if ((w & 0x7f800000) == 0x52800000)
        return U((w >> 5) & 0xffff) << (16 * ((w >> 21) & 3));
    return std::nullopt;
}

struct Main {
    const Nso &nso;
    U base;

    uint32_t word(U address) const
    {
        return uint32_t(nso.u32(address - base));
    }

    U target(const std::string &kind, U address) const
    {
        uint32_t w = word(address);
        if (kind == "code")
            return address;
        if (kind == "call") {
            if ((w & 0x7c000000) != 0x14000000)
                throw std::runtime_error(hex(address) + " is not a branch (" + hex(w) + ")");
            return address + sext(w & 0x3ffffff, 26) * 4;
        }
        if (kind == "imm") {
            auto value = imm_value(w);
            if (!value)
                throw std::runtime_error(hex(address) + " has no immediate (" + hex(w) + ")");
            return *value;
        }
        U value;
        if (is_adrp(w)) {
            value = adrp_page(w, address);
        } else {
            auto lo = lo12(w);
            if (!lo)
                throw std::runtime_error(hex(address) + " does not complete an address (" + hex(w) + ")");
            bool found = false;
            for (unsigned i = 1; i <= 32; ++i) {
                U p = address - 4 * i;
                uint32_t pw = word(p);
                if (is_adrp(pw) && (pw & 31) == lo->second) {
                    value = adrp_page(pw, p) + lo->first;
                    found = true;
                    break;
                }
            }
            if (!found)
                throw std::runtime_error(hex(address) + ": no ADRP for x" + std::to_string(lo->second) + " above it");
        }
        return kind == "got" ? base + nso.u64(value - base) : value;
    }

    std::pair<std::optional<U>, unsigned> locate(const Site &site) const
    {
        std::string kind(site.kind);
        if (kind == "data")
            return {base + nso.h.off[2], 1};
        if (kind == "end")
            return {base + nso.image.size(), 1};
        unsigned hits = 0;
        for (const auto &pattern : site.patterns) {
            std::vector<std::pair<uint32_t, uint32_t>> words;
            std::istringstream input(pattern.text);
            std::string token;
            while (input >> token) {
                auto slash = token.find('/');
                uint32_t value = uint32_t(std::stoul(token.substr(0, slash), nullptr, 16));
                uint32_t mask = slash == std::string::npos ? 0xffffffff :
                    uint32_t(std::stoul(token.substr(slash + 1), nullptr, 16));
                words.emplace_back(value & mask, mask);
            }
            hits = 0;
            U match = 0;
            U size = nso.h.size[0], off = nso.h.off[0];
            for (U p = 0; p + words.size() * 4 <= size; p += 4) {
                size_t i = 0;
                for (; i < words.size(); ++i) {
                    uint32_t w = uint32_t(nso.u32(off + p + i * 4));
                    if ((w & words[i].second) != words[i].first)
                        break;
                }
                if (i == words.size()) {
                    match = base + off + p;
                    if (++hits == 2)
                        break;
                }
            }
            if (hits == 1)
                return {target(kind, match + 4 * pattern.at), 1};
        }
        return {std::nullopt, hits};
    }
};

static Result for_module(const Nso &nso, U base, bool allow_unsupported = false, Log log = {})
{
    const Version *version = nullptr;
    for (const auto &v : versions) {
        if (*v.id && nso.h.id.substr(0, 16) == v.id) {
            version = &v;
            break;
        }
    }
    if (!version || !version->supported) {
        std::string what = version ? "version " + std::string(version->name) + ", not supported yet" :
            "not a version Savage Wind knows";
        std::string message = "main module build " + nso.h.id.substr(0, 16) + ": " + what;
        if (!allow_unsupported)
            throw std::runtime_error(message);
        if (log)
            log("game_sites: " + message + "; sites by pattern only");
    }
    Result result;
    result.version = version ? version->name : nso.h.id.substr(0, 16);
    const std::map<std::string, U> *table = nullptr;
    auto table_it = known.find(result.version);
    if (version && version->supported && table_it != known.end())
        table = &table_it->second;
    std::set<std::string> optional;
    for (const auto &group : optional_groups)
        optional.insert(group.begin(), group.end());
    std::vector<std::string> problems;
    Main main{nso, base};
    for (const auto &site : definitions) {
        std::optional<U> found;
        unsigned hits;
        try {
            auto located = main.locate(site);
            found = located.first;
            hits = located.second;
        } catch (const std::exception &e) {
            problems.push_back(std::string(site.name) + ": " + e.what());
            continue;
        }
        std::optional<U> expected;
        if (table) {
            auto it = table->find(site.name);
            if (it != table->end())
                expected = it->second;
        }
        if (found && expected && *found != *expected)
            throw std::runtime_error(std::string(site.name) + ": the pattern finds " + hex(*found) +
                                     ", the table says " + hex(*expected));
        if (!found && !expected) {
            if (!optional.count(site.name))
                problems.push_back(std::string(site.name) + ": the pattern matches " + std::to_string(hits) +
                                   " places" + (hits > 1 ? " or more" : ""));
            continue;
        }
        if (!found && log)
            log("game_sites: " + std::string(site.name) + ": the pattern matches " + std::to_string(hits) +
                " places; " + hex(*expected) + " from the table");
        result.sites[site.name] = found ? *found : *expected;
        result.fallback[site.name] = !found;
    }
    if (!problems.empty()) {
        std::string message;
        for (const auto &problem : problems) {
            if (!message.empty())
                message += "; ";
            message += problem;
        }
        throw std::runtime_error(message);
    }
    for (const auto &group : optional_groups) {
        std::vector<std::string> missing;
        for (const auto &name : group) {
            if (!result.sites.count(name))
                missing.push_back(name);
        }
        result.groups.push_back(missing.empty());
        if (!missing.empty()) {
            if (log) {
                auto join = [](const std::vector<std::string> &names) {
                    std::string s;
                    for (const auto &name : names) {
                        if (!s.empty())
                            s += ", ";
                        s += name;
                    }
                    return s;
                };
                log("game_sites: " + join(missing) + " not found: " + join(group) + " left out");
            }
            for (const auto &name : group) {
                result.sites.erase(name);
                result.fallback.erase(name);
            }
        }
    }
    return result;
}
} // namespace game_sites
