// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_mem.h"
namespace lift_scalar_fp {
using namespace lift_imm;
inline std::string n(unsigned x) {
    return std::to_string(x);
}
inline U expand(unsigned imm, unsigned size) {
    unsigned E = size == 32 ? 8 : 11, F = size - E - 1, b6 = (imm >> 6) & 1;
    U exp = U(b6 ^ 1) << (E - 1) | (b6 ? ones(E - 3) : 0) << 2 | ((imm >> 4) & 3);
    return U(imm >> 7) << (size - 1) | exp << F | U(imm & 15) << (F - 4);
}
inline std::string lift(uint32_t w) {
    unsigned op1 = bits(w, 23, 2), op2 = bits(w, 19, 4), op3 = bits(w, 10, 9), t = bits(w, 22, 2), d = bits(w, 0, 5),
             nn = bits(w, 5, 5), m = bits(w, 16, 5);
    if (bits(w, 29, 1) || (bits(w, 31, 1) && ((op1 & 2) || ((op2 & 4) && (op3 & 63))))) {
        throw std::runtime_error("fp M/S");
    }
    std::string F = t ? "d" : "s", SF = t ? "VSF64" : "VSF32", SU = t ? "VS64" : "VS32",
                SET = t ? "FP_SETD" : "FP_SETS";
    auto check = [&]() {
        if (t > 1) {
            throw std::runtime_error("fp type");
        }
    };
    if (op1 & 2) {
        check();
        unsigned o1 = bits(w, 21, 1), o0 = bits(w, 15, 1), a = bits(w, 10, 5);
        return "fp_fma_" + F + "(c, " + n(o1) + ", " + n(o1 ^ o0) + ", " + n(d) + ", " + n(nn) + ", " + n(m) + ", " +
               n(a) + ");";
    }
    if (!(op2 & 4)) {
        unsigned sf = bits(w, 31, 1), rm = bits(w, 19, 2), opc = bits(w, 16, 3), scale = bits(w, 10, 6),
                 ib = sf ? 64 : 32, fb = 64 - scale;
        if (t > 1) {
            throw std::runtime_error("fixed type");
        }
        if (!sf && scale < 32) {
            throw std::runtime_error("fixed scale");
        }
        if (rm == 0 && (opc == 2 || opc == 3)) {
            return SET + "(" + n(d) + ", fromint_" + F + "(c, " + reg(sf, nn) + ", " + n(ib) + ", " + n(opc & 1) +
                   ", " + n(fb) + "));";
        }
        if (rm == 3 && (opc == 0 || opc == 1)) {
            return set(sf, d,
                       "toint_" + F + "(c, " + SF + "(" + n(nn) + "), 3, " + n(fb) + ", " + n(ib) + ", " + n(opc) +
                           ")");
        }
        throw std::runtime_error("fixed opcode");
    }
    if (!(op3 & 63)) {
        unsigned sf = bits(w, 31, 1), rm = bits(w, 19, 2), opc = bits(w, 16, 3), ib = sf ? 64 : 32;
        if (opc == 6 || opc == 7) {
            if (t == 2 && sf && rm == 1) {
                return opc == 6 ? set(1, d, "VHI64(" + n(nn) + ")") : "VHI64(" + n(d) + ") = " + x(nn) + ";";
            }
            if (rm || t != sf || t > 1) {
                throw std::runtime_error("fmov general");
            }
            if (opc == 6) {
                return set(sf, d, std::string(sf ? "VS64(" : "VS32(") + n(nn) + ")");
            }
            return "c->v[" + n(d) + "] = " + reg(sf, nn) + ";";
        }
        if (t > 1) {
            throw std::runtime_error("fp int type");
        }
        if (opc == 2 || opc == 3) {
            if (rm) {
                throw std::runtime_error("cvtf rmode");
            }
            return SET + "(" + n(d) + ", fromint_" + F + "(c, " + reg(sf, nn) + ", " + n(ib) + ", " + n(opc & 1) +
                   ", 0));";
        }
        if (opc == 4 || opc == 5) {
            if (rm) {
                throw std::runtime_error("fcvta rmode");
            }
            rm = 4;
        }
        return set(sf, d,
                   "toint_" + F + "(c, " + SF + "(" + n(nn) + "), " + n(rm) + ", 0, " + n(ib) + ", " + n(opc & 1) +
                       ")");
    }
    if ((op3 & 31) == 16) {
        unsigned opc = bits(w, 15, 6);
        if (t == 3) {
            if (opc == 4) {
                return "FP_SETS(" + n(d) + ", simd_h2s(c, VS16(" + n(nn) + ")));";
            }
            if (opc == 5) {
                return "FP_SETD(" + n(d) + ", simd_h2d(c, VS16(" + n(nn) + ")));";
            }
            throw std::runtime_error("fp16 dp1");
        }
        check();
        std::string sign = t ? "0x8000000000000000ull" : "0x80000000u", src = SU + "(" + n(nn) + ")";
        if (opc == 0) {
            return "c->v[" + n(d) + "] = " + src + ";";
        }
        if (opc == 1) {
            return "c->v[" + n(d) + "] = " + src + " & ~" + sign + ";";
        }
        if (opc == 2) {
            return "c->v[" + n(d) + "] = " + src + " ^ " + sign + ";";
        }
        if (opc == 3) {
            return "fp_op_" + F + "(c, FOP_SQRT, " + n(d) + ", " + n(nn) + ", " + n(nn) + ");";
        }
        if (opc == 4 || opc == 5 || opc == 7) {
            unsigned to = opc == 4 ? 0 : opc == 5 ? 1 : 3;
            if (to == t) {
                throw std::runtime_error("fcvt same");
            }
            if (to == 3) {
                return "c->v[" + n(d) + "] = " + std::string(t ? "simd_dd2h" : "simd_s2h") + "(c, " + SF + "(" + n(nn) +
                       "));";
            }
            return to ? "FP_SETD(" + n(d) + ", simd_s2d(c, VSF32(" + n(nn) + ")));"
                      : "FP_SETS(" + n(d) + ", simd_d2s(c, VSF64(" + n(nn) + ")));";
        }
        int rm = opc >= 8 && opc <= 12 ? int(opc) - 8 : opc == 14 ? 5 : opc == 15 ? 6 : -1;
        if (rm < 0) {
            throw std::runtime_error("fp dp1 opcode");
        }
        return SET + "(" + n(d) + ", frint_" + F + "(c, " + SF + "(" + n(nn) + "), " + n(rm) + "));";
    }
    if ((op3 & 15) == 8) {
        check();
        if (bits(w, 14, 2) || bits(w, 0, 3)) {
            throw std::runtime_error("fcmp op");
        }
        auto b = bits(w, 3, 1) ? "0" : SF + "(" + n(m) + ")";
        return "c->nzcv = cmp_" + F + "(c, " + SF + "(" + n(nn) + "), " + b + ");";
    }
    if ((op3 & 7) == 4) {
        check();
        if (bits(w, 5, 5)) {
            throw std::runtime_error("fmov imm5");
        }
        return "c->v[" + n(d) + "] = " + hex(expand(bits(w, 13, 8), 32u << t)) + ";";
    }
    if ((op3 & 3) == 1) {
        check();
        unsigned c = bits(w, 12, 4), nz = bits(w, 0, 4);
        return "c->nzcv = " + lift_reg::cond(c) + " ? cmp_" + F + "(c, " + SF + "(" + n(nn) + "), " + SF + "(" + n(m) +
               ")) : " + hex(U(nz) << 28) + ";";
    }
    if ((op3 & 3) == 2) {
        check();
        unsigned opc = bits(w, 12, 4);
        const char *ops[] = {"FOP_MUL", "FOP_DIV",   "FOP_ADD",   "FOP_SUB", "FOP_MAX",
                             "FOP_MIN", "FOP_MAXNM", "FOP_MINNM", "FOP_NMUL"};
        if (opc > 8) {
            throw std::runtime_error("fp dp2");
        }
        return "fp_op_" + F + "(c, " + ops[opc] + ", " + n(d) + ", " + n(nn) + ", " + n(m) + ");";
    }
    if ((op3 & 3) == 3) {
        check();
        return "c->v[" + n(d) + "] = " + lift_reg::cond(bits(w, 12, 4)) + " ? " + SU + "(" + n(nn) + ") : " + SU + "(" +
               n(m) + ");";
    }
    throw std::runtime_error("fp op3");
}
inline bool handles(uint32_t w) {
    unsigned op0 = bits(w, 28, 4), op1 = bits(w, 23, 2), op2 = bits(w, 19, 4), op3 = bits(w, 10, 9);
    if (op0 == 5 && !(op1 & 2) && !(op2 & 4) && !(op3 & 35)) {
        return false;
    }
    if (op0 == 5 && !(op1 & 2) && (op2 & 7) == 5 && (op3 & 0x183) == 2) {
        return false;
    }
    if ((op0 & 13) == 5) {
        return false;
    }
    return (op0 & 5) == 1;
}
}
