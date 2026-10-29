// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_branch.h"
namespace lift_reg {
using namespace lift_imm;
inline std::string shift(unsigned sf, const std::string &v, unsigned typ, unsigned amt) {
    auto n = std::to_string(amt);
    if (!amt && typ != 3) {
        return v;
    }
    if (typ == 0) {
        return "(" + v + " << " + n + ")";
    }
    if (typ == 1) {
        return "(" + v + " >> " + n + ")";
    }
    if (typ == 2) {
        return std::string(sf ? "(uint64_t)((int64_t)" : "(uint32_t)((int32_t)") + v + " >> " + n + ")";
    }
    return std::string(sf ? "ROR64(" : "ROR32(") + v + ", " + n + ")";
}
inline std::string extend(unsigned r, unsigned opt, unsigned amt) {
    auto base = x(r);
    std::string v;
    if (opt == 3 || opt == 7) {
        v = base;
    } else if (opt < 3) {
        v = "(uint64_t)(uint" + std::to_string(8u << opt) + "_t)" + base;
    } else {
        v = "(uint64_t)(int64_t)(int" + std::to_string(8u << (opt - 4)) + "_t)" + base;
    }
    return amt ? "(" + v + " << " + std::to_string(amt) + ")" : v;
}
inline std::string cond(unsigned c) {
    static const char *names[] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"};
    return c < 14 ? std::string("COND_") + names[c] + "(c)" : "1";
}
inline std::string lift(uint32_t w) {
    unsigned sf = bits(w, 31, 1), rd = bits(w, 0, 5), rn = bits(w, 5, 5), rm = bits(w, 16, 5), size = sf ? 64 : 32,
             op1 = bits(w, 28, 1);
    auto sz = std::to_string(size);
    if (!op1) {
        if (bits(w, 24, 4) == 10) {
            unsigned opc = bits(w, 29, 2), sh = bits(w, 22, 2), N = bits(w, 21, 1), amt = bits(w, 10, 6);
            if (!sf && amt >= 32) {
                throw std::runtime_error("logical shift amount");
            }
            auto b = shift(sf, reg(sf, rm), sh, amt);
            if (N) {
                b = "(uint" + sz + "_t)~" + b;
            }
            auto a = reg(sf, rn), expr = a + (opc == 1 ? " | " : opc == 2 ? " ^ " : " & ") + b;
            if (opc == 3) {
                return set(sf, rd, "logic" + sz + "(c, (uint" + sz + "_t)(" + expr + "))");
            }
            return set(sf, rd, expr);
        }
        if (bits(w, 24, 4) == 11) {
            unsigned op = bits(w, 30, 1), S = bits(w, 29, 1);
            std::string a, b;
            bool sp = false;
            if (!bits(w, 21, 1)) {
                unsigned sh = bits(w, 22, 2), amt = bits(w, 10, 6);
                if (sh == 3) {
                    throw std::runtime_error("add shift ror");
                }
                a = reg(sf, rn);
                b = shift(sf, reg(sf, rm), sh, amt);
            } else {
                if (bits(w, 22, 2)) {
                    throw std::runtime_error("add ext opt");
                }
                a = reg(sf, rn, true);
                b = extend(rm, bits(w, 13, 3), bits(w, 10, 3));
                if (!sf) {
                    b = "(uint32_t)(" + b + ")";
                }
                sp = !S;
            }
            if (S) {
                return set(sf, rd,
                           std::string(op ? "subs" : "adds") + sz + "(c, " + a + ", (uint" + sz + "_t)(" + b + "))");
            }
            return set(sf, rd, a + (op ? " - " : " + ") + b, sp);
        }
        throw std::runtime_error("dp_reg " + lift_branch::numberword(w));
    }
    unsigned op2 = bits(w, 21, 4);
    if (bits(w, 24, 4) == 11) {
        unsigned op54 = bits(w, 29, 2), op31 = bits(w, 21, 3), o0 = bits(w, 15, 1), ra = bits(w, 10, 5);
        if (op54) {
            throw std::runtime_error("dp3 op54");
        }
        std::string sign = o0 ? " - " : " + ";
        if (op31 == 0) {
            return set(sf, rd, reg(sf, ra) + sign + reg(sf, rn) + " * " + reg(sf, rm));
        }
        if (op31 == 1) {
            return set(1, rd,
                       x(ra) + sign + "(uint64_t)((int64_t)(int32_t)" + reg(0, rn) + " * (int64_t)(int32_t)" +
                           reg(0, rm) + ")");
        }
        if (op31 == 5) {
            return set(1, rd, x(ra) + sign + "(uint64_t)" + reg(0, rn) + " * (uint64_t)" + reg(0, rm));
        }
        if (op31 == 2) {
            return set(1, rd,
                       "(uint64_t)(((__int128)(int64_t)" + x(rn) + " * (__int128)(int64_t)" + x(rm) + ") >> 64)");
        }
        if (op31 == 6) {
            return set(1, rd, "(uint64_t)(((unsigned __int128)" + x(rn) + " * " + x(rm) + ") >> 64)");
        }
        throw std::runtime_error("dp3");
    }
    if (op2 == 0) {
        unsigned op = bits(w, 30, 1), S = bits(w, 29, 1);
        return set(sf, rd,
                   std::string(op ? "sbc" : "adc") + (S ? "s" : "") + sz + "(c, " + reg(sf, rn) + ", " + reg(sf, rm) +
                       ")");
    }
    if (op2 == 2) {
        unsigned op = bits(w, 30, 1), imm = bits(w, 11, 1), c = bits(w, 12, 4), nzcv = bits(w, 0, 4);
        return "if (" + cond(c) + ") (void)" + (op ? "subs" : "adds") + sz + "(c, " + reg(sf, rn) + ", " +
               (imm ? hex(rm) : reg(sf, rm)) + "); else c->nzcv = " + hex(U(nzcv) << 28) + ";";
    }
    if (op2 == 4) {
        unsigned op = bits(w, 30, 1), o2 = bits(w, 10, 2), c = bits(w, 12, 4);
        if (o2 > 1) {
            throw std::runtime_error("csel variant");
        }
        auto b = reg(sf, rm);
        if (op) {
            b = (o2 ? "-" : "~") + b;
        } else if (o2) {
            b += " + 1";
        }
        return set(sf, rd, "(" + cond(c) + ") ? " + reg(sf, rn) + " : (uint" + sz + "_t)(" + b + ")");
    }
    if (op2 == 6) {
        unsigned opcode = bits(w, 10, 6);
        auto a = reg(sf, rn), b = reg(sf, rm);
        if (bits(w, 30, 1)) {
            if (rm) {
                throw std::runtime_error("dp1 opcode2");
            }
            std::string expr;
            if (opcode == 0) {
                expr = "rbit" + sz + "(" + a + ")";
            } else if (opcode == 1) {
                expr = "rev16_" + sz + "(" + a + ")";
            } else if (opcode == 2) {
                expr = std::string(sf ? "rev32_64(" : "__builtin_bswap32(") + a + ")";
            } else if (opcode == 3 && sf) {
                expr = "__builtin_bswap64(" + a + ")";
            } else if (opcode == 4) {
                expr = "clz" + sz + "(" + a + ")";
            } else if (opcode == 5) {
                expr = "cls" + sz + "(" + a + ")";
            }
            if (expr.empty()) {
                throw std::runtime_error("dp1");
            }
            return set(sf, rd, expr);
        }
        if (opcode >= 16 && opcode <= 23) {
            unsigned C = bits(w, 12, 1), z = bits(w, 10, 2);
            if ((z == 3) != bool(sf)) {
                throw std::runtime_error("crc size");
            }
            return set(0, rd,
                       std::string(C ? "CRC32C(" : "CRC32(") + reg(0, rn) + ", " + (z == 3 ? x(rm) : reg(0, rm)) +
                           ", " + std::to_string(1u << z) + ")");
        }
        std::string e, mask = std::to_string(size - 1);
        if (opcode == 2 || opcode == 3) {
            e = std::string(opcode == 2 ? "udiv" : "sdiv") + sz + "(" + a + ", " + b + ")";
        } else if (opcode == 8 || opcode == 9) {
            e = a + (opcode == 8 ? " << (" : " >> (") + b + " & " + mask + ")";
        } else if (opcode == 10) {
            e = "(uint" + sz + "_t)((int" + sz + "_t)" + a + " >> (" + b + " & " + mask + "))";
        } else if (opcode == 11) {
            e = "ROR" + sz + "(" + a + ", " + b + " & " + mask + ")";
        }
        if (e.empty()) {
            throw std::runtime_error("dp2");
        }
        return set(sf, rd, e);
    }
    throw std::runtime_error("dp_reg " + lift_branch::numberword(w));
}
} // namespace lift_reg
