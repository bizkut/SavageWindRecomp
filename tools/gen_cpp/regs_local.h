// SPDX-License-Identifier: GPL-3.0-or-later
// Register-local dataflow for the console generator; mirrors tools/regs.py.
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace local_regs {
constexpr char marker = '\1';
const std::string spill = "\1S", reload = "\1R", reload_call = "\1C", opaque = "\1O", end = "\1E";

inline std::string name(int r) { return r == 31 ? "sp_" : "x" + std::to_string(r); }
inline std::string cpu(int r) { return r == 31 ? "c->sp" : "c->x[" + std::to_string(r) + "]"; }
inline void replace_all(std::string &s, const std::string &a, const std::string &b)
{
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}
struct Ref { size_t pos, len; int reg; };
inline std::vector<Ref> references(const std::string &s)
{
    std::vector<Ref> refs;
    bool hidden = false;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, opaque) == 0) { hidden = true; i += 2; continue; }
        if (s.compare(i, 2, end) == 0) { hidden = false; i += 2; continue; }
        if (!hidden && s.compare(i, 5, "c->x[") == 0) {
            size_t j = i + 5;
            int n = 0;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) n = n * 10 + s[j++] - '0';
            if (j > i + 5 && j < s.size() && s[j] == ']') {
                if (n < 32) refs.push_back({i, j + 1 - i, n});
                i = j + 1; continue;
            }
        }
        if (!hidden && s.compare(i, 5, "c->sp") == 0 &&
            (i + 5 == s.size() || !(std::isalnum(static_cast<unsigned char>(s[i + 5])) || s[i + 5] == '_'))) {
            refs.push_back({i, 5, 31}); i += 5; continue;
        }
        ++i;
    }
    return refs;
}
inline uint32_t writes(const std::string &s, const std::vector<Ref> &refs)
{
    uint32_t result = 0;
    for (const auto &r : refs) {
        size_t p = r.pos + r.len;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        bool w = s.compare(p, 2, "++") == 0 || s.compare(p, 2, "--") == 0;
        if (s.compare(p, 2, "<<") == 0 || s.compare(p, 2, ">>") == 0) p += 2;
        else if (p < s.size() && std::string("-+*/%&|^").find(s[p]) != std::string::npos) ++p;
        w |= p < s.size() && s[p] == '=' && (p + 1 == s.size() || s[p + 1] != '=');
        size_t q = r.pos;
        while (q && std::isspace(static_cast<unsigned char>(s[q - 1]))) --q;
        w |= q >= 2 && (s.compare(q - 2, 2, "++") == 0 || s.compare(q - 2, 2, "--") == 0);
        if (w) result |= uint32_t(1) << r.reg;
    }
    return result;
}
struct Plan {
    std::vector<int> regs;
    std::vector<uint32_t> spills;
    uint32_t kept = 0;
    std::string decl() const {
        if (regs.empty()) return {};
        std::string s = "uint64_t ";
        for (size_t i = 0; i < regs.size(); ++i) {
            if (i) s += ", ";
            s += name(regs[i]) + " = " + cpu(regs[i]);
        }
        return s + ";";
    }
    std::string resolve(size_t i, std::string text) const {
        uint32_t mask = 0;
        for (int r : regs) mask |= uint32_t(1) << r;
        auto refs = references(text);
        for (auto it = refs.rbegin(); it != refs.rend(); ++it)
            if (mask & (uint32_t(1) << it->reg)) text.replace(it->pos, it->len, name(it->reg));
        replace_all(text, opaque, ""); replace_all(text, end, "");
        std::string sp, re, call;
        for (int r : regs) {
            if (spills[i] & (uint32_t(1) << r)) sp += cpu(r) + " = " + name(r) + "; ";
            re += " " + name(r) + " = " + cpu(r) + ";";
            if (!(kept & (uint32_t(1) << r))) call += " " + name(r) + " = " + cpu(r) + ";";
        }
        replace_all(text, spill, sp); replace_all(text, reload, re); replace_all(text, reload_call, call);
        return text;
    }
};
inline Plan plan(const std::vector<std::string> &texts, const std::vector<std::vector<size_t>> &succ,
                 const std::vector<bool> &clean, size_t count = 6, bool trust = true)
{
    Plan p;
    std::array<size_t, 32> uses{};
    std::vector<uint32_t> written(texts.size()), dirty(texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        auto refs = references(texts[i]);
        for (auto r : refs) ++uses[r.reg];
        written[i] = writes(texts[i], refs);
    }
    for (int r = 0; r < 32; ++r) if (uses[r]) p.regs.push_back(r);
    std::sort(p.regs.begin(), p.regs.end(),
              [&](int a, int b) { return uses[a] != uses[b] ? uses[a] > uses[b] : a < b; });
    if (p.regs.size() > count) p.regs.resize(count);
    std::sort(p.regs.begin(), p.regs.end());
    uint32_t mask = 0, every = 0;
    for (int r : p.regs) mask |= uint32_t(1) << r;
    for (auto &w : written) { w &= mask; every |= w; }
    if (trust) { for (int r = 19; r < 30; ++r) p.kept |= uint32_t(1) << r; p.kept |= uint32_t(1) << 31; }
    std::vector<bool> seen(texts.size());
    std::vector<size_t> work;
    if (!texts.empty()) { seen[0] = true; work.push_back(0); }
    while (!work.empty()) {
        size_t i = work.back(); work.pop_back();
        uint32_t out = clean[i] ? 0 : dirty[i] | written[i];
        for (size_t s : succ[i]) if (!seen[s] || (out & ~dirty[s])) {
            seen[s] = true; dirty[s] |= out; work.push_back(s);
        }
    }
    p.spills.resize(texts.size());
    for (size_t i = 0; i < texts.size(); ++i)
        if (texts[i].find(spill) != std::string::npos) p.spills[i] = (seen[i] ? dirty[i] : every) | written[i];
    return p;
}
}
