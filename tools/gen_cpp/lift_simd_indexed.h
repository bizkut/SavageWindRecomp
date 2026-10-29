// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_reserved.h"
namespace lift_simd_indexed {
using namespace lift_imm;
using namespace lift_simd_basic;
using lift_simd_diff::gen;
using lift_simd_diff::lop;
using lift_simd_same::vq;
inline int type(uint32_t w) {
    unsigned o = bits(w, 28, 4), p = bits(w, 23, 2), r = bits(w, 10, 9);
    if ((o & 13) == 5 && (p & 2) && !(r & 1)) {
        return 2;
    }
    if (!(o & 9) && (p & 2) && !(r & 1)) {
        return 1;
    }
    return 0;
}
inline std::string lift(uint32_t w, int type) {
    bool scalar = type == 2;
    unsigned Q = bits(w, 30, 1), U_ = bits(w, 29, 1), sz = bits(w, 22, 2), L = bits(w, 21, 1), M = bits(w, 20, 1),
             rm = bits(w, 16, 4), opc = bits(w, 12, 4), H = bits(w, 11, 1), nn = bits(w, 5, 5), d = bits(w, 0, 5);
    bool fp = (opc == 1 || opc == 5 || opc == 9) && !(U_ && opc != 9);
    if (fp) {
        if (sz < 2) {
            throw std::runtime_error("fp16 indexed");
        }
        unsigned fsz = sz & 1, idx;
        if (fsz) {
            if (L) {
                throw std::runtime_error("indexed L");
            }
            idx = H;
        } else {
            idx = (H << 1) | L;
        }
        unsigned m = (M << 4) | rm;
        if (!scalar && fsz && !Q) {
            throw std::runtime_error("indexed 2d Q");
        }
        std::string F = fsz ? "d" : "s", VT = fsz ? "vf64" : "vf32", SF = fsz ? "VSF64" : "VSF32",
                    SET = fsz ? "FP_SETD" : "FP_SETS";
        if (scalar) {
            auto el = vr(VT, m) + "[" + n(idx) + "]";
            if (opc == 9) {
                return SET + "(" + n(d) + ", op_" + F + "(c, " + (U_ ? "FOP_MULX" : "FOP_MUL") + ", " + SF + "(" +
                       n(nn) + "), " + el + ", 0));";
            }
            auto a = SF + "(" + n(nn) + ")";
            if (opc != 1) {
                a = "neg_" + F + "(" + a + ")";
            }
            return SET + "(" + n(d) + ", op_" + F + "(c, FOP_MADD, " + SF + "(" + n(d) + "), " + a + ", " + el + "));";
        }
        auto el = "vsplat_" + F + "(" + vr(VT, m) + ", " + n(idx) + ", " + n(Q) + ")", A = vq(VT, Q, nn);
        if (opc == 9) {
            return std::string("SV_FOP_") + (fsz ? "D" : "S") + "(" + (U_ ? "FOP_MULX" : "FOP_MUL") + ", " + n(Q) +
                   ", " + n(d) + ", " + A + ", " + el + ", (" + VT + "){});";
        }
        if (opc != 1) {
            A = "vneg_" + F + "(" + A + ")";
        }
        return std::string("SV_FOP_") + (fsz ? "D" : "S") + "(FOP_MADD, " + n(Q) + ", " + n(d) + ", " + vq(VT, Q, d) +
               ", " + A + ", " + el + ");";
    }
    unsigned idx, m;
    if (sz == 1) {
        idx = (H << 2) | (L << 1) | M;
        m = rm;
    } else if (sz == 2) {
        idx = (H << 1) | L;
        m = (M << 4) | rm;
    } else {
        throw std::runtime_error("indexed size");
    }
    if (scalar) {
        const char *name = nullptr;
        if (!U_) {
            if (opc == 3) {
                name = "SQDMLAL";
            } else if (opc == 7) {
                name = "SQDMLSL";
            } else if (opc == 11) {
                name = "SQDMULL";
            } else if (opc == 12) {
                name = "SQDMULH";
            } else if (opc == 13) {
                name = "SQRDMULH";
            }
        }
        if (!name) {
            throw std::runtime_error("scalar indexed");
        }
        return gen(lop(name), sz, 1, d, (opc == 12 || opc == 13) ? sz : sz + 1, 0, 0, nn, sz, 0, m, sz, 0, idx);
    }
    unsigned ne = (8u << Q) >> sz;
    auto T = vu(sz), el = shuf(vr(T, m), vr(T, m), std::vector<unsigned>(16u >> sz, idx));
    if (!U_ && opc == 8) {
        return vwr(T, Q, d, vr(T, nn) + " * " + el);
    }
    if (U_ && (opc == 0 || opc == 4)) {
        return vwr(T, Q, d, vr(T, d) + (opc == 0 ? " + " : " - ") + vr(T, nn) + " * " + el);
    }
    if (!U_ && (opc == 12 || opc == 13)) {
        return gen(lop(opc == 12 ? "SQDMULH" : "SQRDMULH"), sz, ne, d, sz, 0, 0, nn, sz, 0, m, sz, 0, idx);
    }
    const char *op = nullptr;
    switch (opc) {
    case 2:
        op = "MLAL";
        break;
    case 6:
        op = "MLSL";
        break;
    case 10:
        op = "MULL";
        break;
    case 3:
        op = "SQDMLAL";
        break;
    case 7:
        op = "SQDMLSL";
        break;
    case 11:
        op = "SQDMULL";
        break;
    }
    if (!op || (U_ && op[0] == 'S')) {
        throw std::runtime_error("indexed opcode");
    }
    ne = 8u >> sz;
    return gen(lop(op, op[0] == 'S' ? 0 : U_), sz, ne, d, sz + 1, 0, 0, nn, sz, Q * ne, m, sz, 0, idx);
}
} // namespace lift_simd_indexed
