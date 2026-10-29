// SPDX-License-Identifier: GPL-3.0-or-later
#define S9_PARSER_ONLY
#include "dump.cpp"
#include "lift_simd_misc.h"
#include "jump_interpreter.h"
#include <capstone/capstone.h>
#include <optional>
#include <unordered_map>
#include <tuple>

struct Decoded {
    enum Kind { unknown, op, trap, b, bcond, bl, br, blr, ret } kind = unknown;
    bool flags = false, unsupported = false;
    U target = 0;
};
static bool sets_flags(const std::string &s) {
    if (s.find("nzcv") != std::string::npos) {
        return true;
    }
    for (const char *p : {"adds32(", "adds64(", "subs32(", "subs64(", "adcs32(", "adcs64(", "sbcs32(", "sbcs64(",
                          "logic32(", "logic64("}) {
        if (s.find(p) != std::string::npos) {
            return true;
        }
    }
    return false;
}
static Decoded decode(uint32_t w, U pc) {
    Decoded x;
    unsigned op = (w >> 25) & 15;
    std::string s;
    try {
        if ((op & 14) == 8) {
            s = lift_imm::lift(w, pc);
        } else if ((op & 14) == 10) {
            s = lift_branch::lift(w, pc);
            auto tab = s.find('\t');
            auto kind = s.substr(0, tab);
            if (kind == "b") {
                x.kind = Decoded::b;
            } else if (kind == "bcond") {
                x.kind = Decoded::bcond;
            } else if (kind == "bl") {
                x.kind = Decoded::bl;
            } else if (kind == "br") {
                x.kind = Decoded::br;
            } else if (kind == "blr") {
                x.kind = Decoded::blr;
            } else if (kind == "ret") {
                x.kind = Decoded::ret;
            } else if (kind == "trap") {
                x.kind = Decoded::trap;
            } else {
                x.kind = Decoded::op;
            }
            if (x.kind == Decoded::b || x.kind == Decoded::bcond || x.kind == Decoded::bl) {
                x.target = std::stoull(s.substr(tab + 1));
            }
            if (x.kind == Decoded::op) {
                x.flags = sets_flags(s.substr(tab + 1));
            }
            return x;
        } else if ((op & 5) == 4 && !((w >> 26) & 1)) {
            s = lift_mem::lift(w, pc);
        } else if ((op & 5) == 4) {
            s = lift_simd_mem::lift(w, pc);
        } else if ((op & 7) == 5) {
            s = lift_reg::lift(w);
        } else if ((op & 7) == 7) {
            if (auto t = lift_simd_basic::type(w)) {
                s = lift_simd_basic::lift(w, t);
            } else if (auto t = lift_simd_diff::type(w)) {
                s = lift_simd_diff::lift(w, t);
            } else if (auto t = lift_simd_same::type(w)) {
                s = lift_simd_same::lift(w, t);
            } else if (auto t = lift_simd_indexed::type(w)) {
                s = lift_simd_indexed::lift(w, t);
            } else if (auto t = lift_simd_shift::type(w)) {
                s = lift_simd_shift::lift(w, t);
            } else if (auto t = lift_simd_misc::type(w)) {
                s = lift_simd_misc::lift(w, t);
            } else if (lift_scalar_fp::handles(w)) {
                s = lift_scalar_fp::lift(w);
            } else {
                throw std::runtime_error(lift_simd_reserved::error(w));
            }
        } else {
            x.kind = Decoded::trap;
            return x;
        }
        x.kind = Decoded::op;
        x.flags = sets_flags(s);
        return x;
    } catch (const std::runtime_error &) {
        x.kind = Decoded::trap;
        x.flags = false;
        x.unsupported = true;
        return x;
    }
}
struct Scan {
    std::set<U> labels, ext;
    std::map<U, std::vector<U>> tables;
    std::set<U> all;
};
struct Discovery {
    const Nso &nso;
    U base, lo, hi;
    std::vector<Decoded> cache;
    std::vector<std::pair<U, U>> fdes;
    csh cs = 0;
    std::map<std::tuple<U, U, U>, std::optional<std::vector<U>>> jump_cache;
    std::map<std::pair<U, U>, Scan> scanned;
    JumpInterpreter interp;
    U unsupported = 0;
    explicit Discovery(const Nso &n, U b)
        : nso(n), base(b), lo(b + n.h.off[0]), hi(lo + n.h.size[0]), cache(n.h.size[0] / 4), interp(n.image, b) {
        for (auto [a, z] : n.extents(n.unwind_table())) {
            if (z > a) {
                fdes.emplace_back(base + a, base + z);
            }
        }
        std::sort(fdes.begin(), fdes.end());
        require(cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs) == CS_ERR_OK, "capstone open failed");
        require(cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK, "capstone detail failed");
    }
    ~Discovery() {
        if (cs) {
            cs_close(&cs);
        }
    }
    uint32_t word(U a) const {
        return nso.u32(nso.h.off[0] + a - lo);
    }
    bool in_text(U a) const {
        return a >= lo && a < hi && !(a & 3);
    }
    Decoded &lift(U a) {
        auto &d = cache[(a - lo) / 4];
        if (d.kind == Decoded::unknown) {
            d = decode(word(a), a);
            if (d.unsupported) {
                ++unsupported;
            }
        }
        return d;
    }
    std::optional<std::pair<U, U>> fde(U a) const {
        auto it = std::upper_bound(fdes.begin(), fdes.end(), std::pair<U, U>{a, ~U(0)});
        if (it == fdes.begin()) {
            return {};
        }
        --it;
        if (a < it->second) {
            return *it;
        }
        return {};
    }
    bool writes(U a, unsigned rn) {
        uint32_t w = word(a);
        uint8_t bytes[] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)};
        cs_insn *ins = nullptr;
        size_t count = cs_disasm(cs, bytes, 4, a, 1, &ins);
        if (!count) {
            return true;
        }
        cs_regs read, write;
        uint8_t nr = 0, nw = 0;
        bool result = true;
        if (cs_regs_access(cs, ins, read, &nr, write, &nw) == CS_ERR_OK) {
            result = false;
            auto x = "x" + std::to_string(rn), y = "w" + std::to_string(rn);
            for (unsigned i = 0; i < nw; i++) {
                auto name = cs_reg_name(cs, write[i]);
                if (name && (x == name || y == name)) {
                    result = true;
                    break;
                }
            }
        }
        cs_free(ins, count);
        return result;
    }
    bool computed(U pc, U entry) {
        unsigned rn = (word(pc) >> 5) & 31;
        U a = pc;
        while (a >= entry + 4 && pc - (a - 4) <= 64) {
            a -= 4;
            auto k = lift(a).kind;
            if (k != Decoded::op) {
                return false;
            }
            U w = word(a);
            if (writes(a, rn)) {
                return (w & 0x1f000000) == 0x0b000000 || (w & 0x9f000000) == 0x10000000;
            }
        }
        return false;
    }
    std::optional<U> guard_cmp(U guard, U lower) {
        U a = guard;
        while (a >= lower + 4 && guard - (a - 4) <= 48) {
            a -= 4;
            U w = word(a);
            if ((w & 0x7f80001f) == 0x7100001f) {
                return ((w >> 22) & 1) ? std::optional<U>{} : a;
            }
            auto d = lift(a);
            if ((d.kind != Decoded::op && d.kind != Decoded::bcond) || d.flags) {
                return {};
            }
        }
        return {};
    }
    std::vector<unsigned> index_regs(U start, U at, unsigned idx) {
        std::set<unsigned> regs{idx};
        bool change = true;
        while (change) {
            change = false;
            for (U a = start; a < at; a += 4) {
                uint32_t w = word(a);
                int src = -1;
                if ((w & 0x7f800000) == 0x12000000 || (w & 0x7f800000) == 0x53000000 ||
                    (w & 0x7f800000) == 0x13000000) {
                    src = (w >> 5) & 31;
                } else if ((w & 0x7fe0ffe0) == 0x2a0003e0) {
                    src = (w >> 16) & 31;
                }
                unsigned rd = w & 31;
                if (src < 0 || src >= 31 || rd >= 31) {
                    continue;
                }
                if (regs.count(rd) && regs.insert(src).second) {
                    change = true;
                }
                if (regs.count(src) && regs.insert(rd).second) {
                    change = true;
                }
            }
        }
        return {regs.begin(), regs.end()};
    }
    std::optional<std::vector<U>> emulate(U pc, U lower, U upper, U start, U at, const std::vector<unsigned> &regs,
                                          unsigned count) {
        unsigned rn = (word(pc) >> 5) & 31;
        std::vector<U> targets;
        interp.prepare();
        for (U a = lower; a < start; a += 4) {
            uint32_t w = word(a);
            if ((w & 0x1f000000) == 0x10000000 || (w & 0xff800000) == 0x91000000) {
                if (!interp.step(a)) {
                    return {};
                }
            }
        }
        auto saved = interp.context();
        for (unsigned i = 0; i < count; i++) {
            interp.restore(saved);
            for (U a = start; a < pc; a += 4) {
                if (a == at) {
                    for (unsigned r : regs) {
                        interp.set_reg(r < 29 ? r : 29, i);
                    }
                }
                auto kind = lift(a).kind;
                if (kind == Decoded::bcond) {
                    continue;
                }
                if (kind != Decoded::op) {
                    return {};
                }
                if (!interp.step(a)) {
                    return {};
                }
            }
            U t = interp.get_reg(rn);
            if (t < lower || t >= upper || (t & 3)) {
                return {};
            }
            targets.push_back(t);
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (count > 1 && targets.size() == 1) {
            return {};
        }
        return targets;
    }
    std::optional<std::vector<U>> fallthrough(U pc, U lower, U upper) {
        (void)upper;
        U a = pc, guard = 0;
        while (a >= lower + 4 && pc - (a - 4) <= 128) {
            a -= 4;
            uint32_t w = word(a);
            unsigned c = w & 15;
            if ((w & 0xff000010) == 0x54000000 && (c == 8 || c == 2)) {
                guard = a;
                break;
            }
            auto k = lift(a).kind;
            if (k != Decoded::op && k != Decoded::bcond) {
                return {};
            }
        }
        if (!guard) {
            return {};
        }
        auto cmp = guard_cmp(guard, lower);
        if (!cmp) {
            return {};
        }
        uint32_t w = word(*cmp);
        unsigned limit = (w >> 10) & 4095, count = limit + ((word(guard) & 15) == 8);
        if (!count || count > 4096) {
            return {};
        }
        U start = *cmp;
        while (start >= lower + 4 && *cmp - (start - 4) <= 48 && lift(start - 4).kind == Decoded::op) {
            start -= 4;
        }
        return emulate(pc, lower, upper, start, *cmp, index_regs(start, *cmp, (w >> 5) & 31), count);
    }
    std::optional<std::vector<U>> taken(U pc, U lower, U upper) {
        U start = pc;
        while (start >= lower + 4 && pc - (start - 4) <= 48 && lift(start - 4).kind == Decoded::op) {
            start -= 4;
        }
        for (U g = lower; g < upper; g += 4) {
            uint32_t w = word(g);
            unsigned c = w & 15;
            if ((w & 0xff000010) != 0x54000000 || (c != 9 && c != 3)) {
                continue;
            }
            U t = g + U(lift_imm::sext((w >> 5) & 0x7ffff, 19) * 4);
            if (t != start) {
                continue;
            }
            auto cmp = guard_cmp(g, lower);
            if (!cmp) {
                continue;
            }
            unsigned limit = (word(*cmp) >> 10) & 4095, count = limit + (c == 9);
            if (!count || count > 4096) {
                continue;
            }
            return emulate(pc, lower, upper, start, start, {unsigned((word(*cmp) >> 5) & 31)}, count);
        }
        return {};
    }
    std::optional<std::vector<U>> table(U pc, U entry, U end) {
        auto extent = fde(pc).value_or(std::pair<U, U>{entry, end});
        auto key = std::make_tuple(pc, extent.first, extent.second);
        auto it = jump_cache.find(key);
        if (it != jump_cache.end()) {
            return it->second;
        }
        auto r = fallthrough(pc, extent.first, extent.second);
        if (!r) {
            r = taken(pc, extent.first, extent.second);
        }
        jump_cache.emplace(key, r);
        return r;
    }
    Scan scan(U entry, U end) {
        Scan result;
        result.labels.insert(entry);
        bool terminated = false;
        for (U a = entry; a < end; a += 4) {
            auto ins = lift(a);
            auto k = ins.kind;
            if (k == Decoded::b || k == Decoded::bcond) {
                U t = ins.target;
                if (t >= entry && t < end) {
                    result.labels.insert(t);
                } else if (in_text(t)) {
                    result.ext.insert(t);
                }
            } else if (k == Decoded::bl) {
                if (in_text(ins.target)) {
                    result.ext.insert(ins.target);
                }
            } else if (k == Decoded::br) {
                auto jt = table(a, entry, end);
                if (jt && !jt->empty()) {
                    result.tables[a] = *jt;
                    for (U t : *jt) {
                        (t >= entry && t < end ? result.labels : result.ext).insert(t);
                    }
                } else if (computed(a, fde(a).value_or(std::pair<U, U>{entry, end}).first)) {
                    result.all.insert(a);
                }
            }
            terminated = k == Decoded::b || k == Decoded::br || k == Decoded::ret || k == Decoded::trap;
        }
        if (!terminated && end < hi) {
            result.ext.insert(end);
        }
        return result;
    }
    std::vector<U> run(bool report = true) {
        std::set<U> starts;
        auto add = [&](U a) {
            if (in_text(a)) {
                starts.insert(a);
            }
        };
        for (auto [a, _] : nso.unwind_table()) {
            add(base + a);
        }
        for (auto &s : nso.syms) {
            if (s.shndx && (s.info & 15) == 2) {
                add(base + s.value);
            }
        }
        for (auto &r : nso.relocs) {
            if (r.type == 1027) {
                add(base + r.add);
            }
        }
        for (U a = lo; a < hi; a += 4) {
            uint32_t w = word(a);
            if ((w & 0xfc000000) == 0x94000000) {
                add(a + U(lift_imm::sext(w & 0x3ffffff, 26) * 4));
            }
        }
        std::array<std::pair<U, U>, 32> pages{};
        std::array<bool, 32> present{};
        auto own = [&](U pc, U t) {
            auto f = fde(pc);
            return f && f->first < t && t < f->second;
        };
        for (U a = lo, i = 0; a < hi; a += 4, ++i) {
            uint32_t w = word(a);
            U imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3);
            if ((w & 0x9f000000) == 0x10000000) {
                U t = a + U(lift_imm::sext(imm, 21));
                if (in_text(t) && !own(a, t)) {
                    starts.insert(t);
                }
            } else if ((w & 0x9f000000) == 0x90000000) {
                unsigned rd = w & 31;
                pages[rd] = {(a & ~U(4095)) + U(lift_imm::sext(imm, 21) * 4096), i};
                present[rd] = true;
            } else if ((w & 0xffc00000) == 0x91000000) {
                unsigned rn = (w >> 5) & 31;
                if (present[rn] && i - pages[rn].second <= 8) {
                    U t = pages[rn].first + ((w >> 10) & 4095);
                    if (in_text(t) && !own(a, t)) {
                        starts.insert(t);
                    }
                }
            }
        }
        unsigned rounds = 0;
        std::vector<U> order;
        for (;;) {
            ++rounds;
            order.assign(starts.begin(), starts.end());
            std::set<U> added;
            for (size_t i = 0; i < order.size(); i++) {
                U a = order[i], end = i + 1 < order.size() ? order[i + 1] : hi;
                auto key = std::make_pair(a, end);
                auto it = scanned.find(key);
                if (it == scanned.end()) {
                    it = scanned.emplace(key, scan(a, end)).first;
                }
                for (U t : it->second.ext) {
                    if (!starts.count(t)) {
                        added.insert(t);
                    }
                }
            }
            if (added.empty()) {
                break;
            }
            starts.insert(added.begin(), added.end());
        }
        if (!report) {
            return order;
        }
        std::cout << std::hex << "rounds " << std::dec << rounds << '\n' << std::hex;
        U jt = 0, all = 0, dispatch = 0;
        for (size_t i = 0; i < order.size(); i++) {
            U a = order[i], end = i + 1 < order.size() ? order[i + 1] : hi;
            auto &s = scanned.at({a, end});
            std::cout << "function " << a << ' ' << end << ' ';
            bool first = true;
            for (U l : s.labels) {
                if (!first) {
                    std::cout << ',';
                }
                std::cout << l;
                first = false;
            }
            std::cout << '\n';
            std::set<U> pcs = s.all;
            for (auto &p : s.tables) {
                pcs.insert(p.first);
            }
            for (U pc : pcs) {
                std::cout << "table " << pc << ' ';
                if (s.all.count(pc)) {
                    std::cout << "all";
                    ++all;
                } else {
                    first = true;
                    for (U t : s.tables.at(pc)) {
                        if (!first) {
                            std::cout << ',';
                        }
                        std::cout << t;
                        first = false;
                    }
                    ++jt;
                }
                std::cout << '\n';
            }
            for (U pc = a; pc < end; pc += 4) {
                if (lift(pc).kind == Decoded::br && !pcs.count(pc)) {
                    ++dispatch;
                }
            }
        }
        std::cout << std::dec << "stat unsupported " << unsupported << "\nstat jumptables " << jt
                  << "\nstat br_all_labels " << all << "\nstat br_dispatch " << dispatch << '\n';
        return order;
    }
};
#ifndef S9_DISCOVERY_ONLY
int main(int argc, char **argv) try {
    require(argc == 4, "usage: discover EXEFS LAYOUT MODULE");
    fs::path dir = argv[1];
    auto lay = layout(dir);
    std::ifstream in(argv[2]);
    require(bool(in), "layout unavailable");
    std::string line;
    bool found = false;
    U base = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream s(line);
        std::string name, b, size, id;
        require(bool(s >> name >> b >> size >> id), "bad layout");
        if (name == argv[3]) {
            auto it = std::find_if(lay.begin(), lay.end(), [&](auto &x) { return x.name == name; });
            require(it != lay.end() && it->id == id && it->base == std::stoull(b, nullptr, 16),
                    "module layout mismatch");
            base = it->base;
            found = true;
        }
    }
    require(found, "module absent");
    Nso n(file(dir / argv[3]));
    Discovery d(n, base);
    d.run();
} catch (const std::exception &e) {
    std::cerr << "discover: " << e.what() << '\n';
    return 1;
}
#endif
