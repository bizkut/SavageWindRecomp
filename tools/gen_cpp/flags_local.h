// SPDX-License-Identifier: GPL-3.0-or-later
// Function-local NZCV dataflow, ported from tools/flags.py. That planner derives from
// TotK Recomp's recomp/flags.py, Copyright (c) 2026 Valiantsin Dzerakh, MIT License:
// third_party/totk-pc-recomp/LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace flags_local {

struct Effect {
    bool reads;
    std::string kind; // Empty means no definition.
};

inline const std::regex kind_def(R"(\b(subs|adds|logic)(32|64)\(c, )");
inline const std::regex carry_def(R"(\b(adcs|sbcs)(32|64)\(c, )");
inline const std::regex carry_use(R"(\b(adc|sbc)(32|64)\(c, )");
inline const std::regex cond(R"(COND_([A-Z]{2})\(c\))");
inline const std::regex nz_def(R"(c->nzcv\s*=(?!=))");
inline const std::regex nz_use(R"(c->nzcv(?!\s*=(?!=)))");
inline const std::regex void_def(R"(\(void\)(subs|adds)(32|64)\(c, )");

inline std::string kind_name(const std::string &op)
{
    if (op == "subs") return "sub";
    if (op == "adds") return "add";
    return "logic";
}

inline bool contains(const std::string &code, const char *text)
{
    return code.find(text) != std::string::npos;
}

inline bool has_kind(const std::string &code)
{
    return contains(code, "subs") || contains(code, "adds") || contains(code, "logic");
}

inline bool has_carry(const std::string &code)
{
    return contains(code, "adc") || contains(code, "sbc");
}

inline Effect effect(const std::string &code)
{
    bool carry = has_carry(code);
    bool nz = contains(code, "c->nzcv");
    bool carry_write = carry && std::regex_search(code, carry_def);
    bool reads = (contains(code, "COND_") && std::regex_search(code, cond)) ||
                 (carry && std::regex_search(code, carry_use)) || carry_write ||
                 (nz && std::regex_search(code, nz_use));
    if ((nz && std::regex_search(code, nz_def)) || carry_write) return {reads, "nz"};
    if (!has_kind(code)) return {reads, {}};
    std::sregex_iterator it(code.begin(), code.end(), kind_def), end;
    if (it == end) return {reads, {}};
    std::string kind = kind_name((*it)[1].str()) + (*it)[2].str();
    if (++it != end) throw std::runtime_error("several flag definitions in one instruction: " + code);
    return {reads, kind};
}

template <typename F>
inline std::string replace_matches(const std::string &code, const std::regex &pattern, F replacement)
{
    std::string out;
    size_t pos = 0;
    for (std::sregex_iterator it(code.begin(), code.end(), pattern), end; it != end; ++it) {
        size_t start = static_cast<size_t>(it->position());
        out.append(code, pos, start - pos);
        out += replacement(*it);
        pos = start + static_cast<size_t>(it->length());
    }
    out.append(code, pos, std::string::npos);
    return out;
}

struct Plan {
    std::vector<std::string> reader;
    std::vector<bool> pack;
    std::vector<std::string> exit;
    std::vector<bool> load;
    bool entry = false;
    bool entry_read = false;
    size_t after_call = 0;
    bool used = false;

    explicit Plan(size_t n) : reader(n), pack(n), exit(n), load(n) {}

    std::string decl() const { return entry ? "Flags F = {.nz = c->nzcv};" : "Flags F = {0};"; }
    std::string publish(size_t i) const { return exit[i].empty() ? "" : "c->nzcv = fnz_" + exit[i] + "(&F); "; }
    std::string reload(size_t i) const { return load[i] ? " F.nz = c->nzcv;" : ""; }

    std::string rewrite(size_t i, std::string code) const
    {
        const std::string &rk = reader[i];
        const std::string dk = effect(code).kind;
        if (!rk.empty()) {
            if (contains(code, "COND_")) code = replace_matches(code, cond, [&](const std::smatch &m) {
                return "fc_" + rk + "(&F, CC_" + m[1].str() + ")";
            });
            if (has_carry(code)) {
                code = replace_matches(code, carry_def, [&](const std::smatch &m) {
                    return m[1].str() + m[2].str() + "f(&F, fc_" + rk + "(&F, CC_CS), ";
                });
                code = replace_matches(code, carry_use, [&](const std::smatch &m) {
                    return m[1].str() + m[2].str() + "f(fc_" + rk + "(&F, CC_CS), ";
                });
            }
            if (contains(code, "c->nzcv"))
                code = replace_matches(code, nz_use, [&](const std::smatch &) { return "fnz_" + rk + "(&F)"; });
        }
        if (dk == "nz") {
            if (contains(code, "c->nzcv")) code = std::regex_replace(code, nz_def, "F.nz =");
            if (contains(code, "(void)")) code = replace_matches(code, void_def, [](const std::smatch &m) {
                return "F.nz = nz_" + kind_name(m[1].str()) + m[2].str() + "(";
            });
        } else if (!dk.empty()) {
            code = replace_matches(code, kind_def, [](const std::smatch &m) {
                return "f" + kind_name(m[1].str()) + m[2].str() + "(&F, ";
            });
            if (pack[i]) code += " F.nz = fnz_" + dk + "(&F);";
        }
        if (contains(code, "c->nzcv") || (contains(code, "COND_") && std::regex_search(code, cond)) ||
            (has_kind(code) && std::regex_search(code, kind_def)) ||
            (has_carry(code) && std::regex_search(code, carry_use)))
            throw std::runtime_error("flag access left after the rewrite: " + code);
        return code;
    }
};

inline Plan plan(const std::vector<Effect> &effects, const std::vector<std::vector<size_t>> &succ,
                 const std::vector<bool> &mems, const std::vector<bool> &exits)
{
    const size_t n = effects.size();
    Plan p(n);
    if (std::none_of(effects.begin(), effects.end(), [](const Effect &e) { return e.reads || !e.kind.empty(); }))
        return p;
    p.used = true;
    // The index n represents the entry definition.
    std::vector<std::set<size_t>> reach(n);
    reach[0].insert(n);
    std::vector<size_t> work{0};
    while (!work.empty()) {
        size_t i = work.back();
        work.pop_back();
        const std::set<size_t> out = !effects[i].kind.empty() || mems[i] ? std::set<size_t>{i} : reach[i];
        for (size_t s : succ[i]) {
            size_t before = reach[s].size();
            reach[s].insert(out.begin(), out.end());
            if (reach[s].size() != before) work.push_back(s);
        }
    }
    auto use = [&](const std::set<size_t> &r) {
        std::set<std::string> kinds;
        for (size_t d : r) {
            if (d == n) p.entry = true;
            else if (mems[d]) p.load[d] = true;
            kinds.insert(d == n || mems[d] ? "nz" : effects[d].kind);
        }
        if (kinds.size() == 1) return *kinds.begin();
        for (size_t d : r)
            if (d != n && !mems[d]) p.pack[d] = true;
        return std::string("nz");
    };
    for (size_t i = 0; i < n; ++i) {
        if (effects[i].reads) {
            p.after_call += std::any_of(reach[i].begin(), reach[i].end(),
                                        [&](size_t d) { return d != n && mems[d]; });
            p.reader[i] = use(reach[i]);
            p.entry_read = p.entry_read || reach[i].count(n) != 0;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (exits[i] && std::any_of(reach[i].begin(), reach[i].end(),
                                    [&](size_t d) { return d != n && !mems[d]; }))
            p.exit[i] = use(reach[i]);
    }
    return p;
}

} // namespace flags_local
