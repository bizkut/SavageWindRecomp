// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace lift_imm {
using U = uint64_t;
inline unsigned bits(uint32_t w, unsigned lo, unsigned n) {
    return (w >> lo) & ((1u << n) - 1);
}
inline U ones(unsigned n) {
    return n >= 64 ? ~U(0) : (U(1) << n) - 1;
}
inline std::string hex(U x) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), d[x & 15]);
        x >>= 4;
    } while (x);
    return "0x" + s + "ull";
}
inline int64_t sext(U v, unsigned n) {
    return v & (U(1) << (n - 1)) ? int64_t(v) - int64_t(U(1) << n) : int64_t(v);
}
inline std::string x(unsigned r) {
    return r == 31 ? "0ull" : "c->x[" + std::to_string(r) + "]";
}
inline std::string reg(unsigned sf, unsigned r, bool sp = false) {
    if (sf) {
        return r == 31 && sp ? "c->sp" : x(r);
    }
    if (r == 31) {
        return sp ? "(uint32_t)c->sp" : "0u";
    }
    return "(uint32_t)c->x[" + std::to_string(r) + "]";
}
inline std::string set(unsigned sf, unsigned r, const std::string &e, bool sp = false) {
    if (r == 31 && !sp) {
        return "(void)(" + e + ");";
    }
    std::string d = r == 31 ? "c->sp" : "c->x[" + std::to_string(r) + "]";
    return d + " = (uint" + (sf ? "64" : "32") + "_t)(" + e + ");";
}
inline std::pair<U, U> masks(unsigned N, unsigned imms, unsigned immr, unsigned size, bool immediate) {
    unsigned v = (N << 6) | (~imms & 63), length = 0;
    if (!v) {
        throw std::runtime_error("reserved bitmask");
    }
    for (unsigned t = v; t >>= 1;) {
        ++length;
    }
    if (length < 1) {
        throw std::runtime_error("reserved bitmask");
    }
    unsigned levels = unsigned(ones(length));
    if (immediate && (imms & levels) == levels) {
        throw std::runtime_error("reserved bitmask");
    }
    unsigned S = imms & levels, R = immr & levels, diff = (S - R) & 63, esize = 1u << length, d = diff & levels;
    U welem = ones(S + 1), telem = ones(d + 1), w = 0, t = 0;
    R %= esize;
    if (R) {
        welem = ((welem >> R) | (welem << (esize - R))) & ones(esize);
    }
    for (unsigned i = 0; i < size; i += esize) {
        w |= welem << i;
        t |= telem << i;
    }
    return {w, t};
}
// Equivalent to Lifter.dp_imm. The caller dispatches only op0 8/9 here.
inline std::string lift(uint32_t w, U pc) {
    unsigned op = bits(w, 23, 3), sf = bits(w, 31, 1), rd = bits(w, 0, 5), rn = bits(w, 5, 5);
    if (op <= 1) {
        int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
        U val = sf ? (pc & ~U(0xfff)) + U(imm * 4096) : pc + U(imm);
        return set(1, rd, hex(val));
    }
    if (op == 2) {
        unsigned opc = bits(w, 30, 1), S = bits(w, 29, 1), sh = bits(w, 22, 1);
        U imm = U(bits(w, 10, 12)) << (sh ? 12 : 0);
        auto a = reg(sf, rn, true);
        if (S) {
            return set(sf, rd,
                       std::string(opc ? "subs" : "adds") + (sf ? "64" : "32") + "(c, " + a + ", " + hex(imm) + ")");
        }
        return set(sf, rd, a + (opc ? " - " : " + ") + hex(imm), true);
    }
    if (op == 4) {
        unsigned opc = bits(w, 29, 2), N = bits(w, 22, 1), size = sf ? 64 : 32;
        if (!sf && N) {
            throw std::runtime_error("logical imm N=1 32-bit");
        }
        U imm = masks(N, bits(w, 10, 6), bits(w, 16, 6), size, true).first;
        auto a = reg(sf, rn);
        if (opc < 3) {
            return set(sf, rd, a + (opc == 0 ? " & " : opc == 1 ? " | " : " ^ ") + hex(imm), true);
        }
        return set(sf, rd, "logic" + std::to_string(size) + "(c, " + a + " & " + hex(imm) + ")");
    }
    if (op == 5) {
        unsigned opc = bits(w, 29, 2), hw = bits(w, 21, 2);
        if (!sf && hw > 1) {
            throw std::runtime_error("movw hw");
        }
        U imm = U(bits(w, 5, 16)) << (hw * 16);
        if (opc == 0) {
            return set(sf, rd, hex(~imm & ones(sf ? 64 : 32)));
        }
        if (opc == 2) {
            return set(sf, rd, hex(imm));
        }
        if (opc == 3) {
            return set(sf, rd, "(" + x(rd) + " & " + hex(~(U(0xffff) << (hw * 16))) + ") | " + hex(imm));
        }
        throw std::runtime_error("movw opc");
    }
    if (op == 6) {
        unsigned opc = bits(w, 29, 2), N = bits(w, 22, 1), immr = bits(w, 16, 6), imms = bits(w, 10, 6),
                 size = sf ? 64 : 32;
        if (N != sf) {
            throw std::runtime_error("bitfield N");
        }
        auto [wmask, tmask] = masks(N, imms, immr, size, false);
        auto src = reg(sf, rn);
        auto rot = immr ? "ROR" + std::to_string(size) + "(" + src + ", " + std::to_string(immr) + ")" : src;
        if (opc == 2) {
            return set(sf, rd, rot + " & " + hex(wmask & tmask));
        }
        if (opc == 0) {
            auto top = "(uint" + std::to_string(size) + "_t)(-(int" + std::to_string(size) + "_t)((" + src + " >> " +
                       std::to_string(imms) + ") & 1))";
            return set(sf, rd,
                       "(" + top + " & " + hex(~tmask & ones(size)) + ") | (" + rot + " & " + hex(wmask & tmask) + ")");
        }
        if (opc == 1) {
            auto dst = reg(sf, rd),
                 bot = "((" + dst + " & " + hex(~wmask & ones(size)) + ") | (" + rot + " & " + hex(wmask) + "))";
            return set(sf, rd, "(" + dst + " & " + hex(~tmask & ones(size)) + ") | (" + bot + " & " + hex(tmask) + ")");
        }
        throw std::runtime_error("bitfield opc");
    }
    if (op == 7) {
        unsigned rm = bits(w, 16, 5), lsb = bits(w, 10, 6), size = sf ? 64 : 32;
        if (!lsb) {
            return set(sf, rd, reg(sf, rm));
        }
        return set(sf, rd,
                   "(" + reg(sf, rm) + " >> " + std::to_string(lsb) + ") | ((uint" + std::to_string(size) + "_t)" +
                       reg(sf, rn) + " << " + std::to_string(int(size) - int(lsb)) + ")");
    }
    static const char d[] = "0123456789abcdef";
    std::string word(8, '0');
    for (int i = 7; i >= 0; --i) {
        word[i] = d[w & 15];
        w >>= 4;
    }
    throw std::runtime_error("dp_imm " + word);
}
} // namespace lift_imm
