// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_imm.h"
#include <string>
#include <stdexcept>
namespace lift_branch {
using namespace lift_imm;
inline std::string numberword(uint32_t w) {
    auto s = hex(w);
    s = s.substr(2, s.size() - 5);
    return std::string(8 - s.size(), '0') + s;
}
inline std::string tuple(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e) {
    return "(" + std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c) + ", " + std::to_string(d) +
           ", " + std::to_string(e) + ")";
}
inline std::string lift(uint32_t w, U pc) {
    if (bits(w, 26, 5) == 5) {
        U t = pc + U(sext(bits(w, 0, 26), 26) * 4);
        return std::string(bits(w, 31, 1) ? "bl\t" : "b\t") + std::to_string(t);
    }
    if (bits(w, 25, 7) == 42 && !bits(w, 4, 1)) {
        U t = pc + U(sext(bits(w, 5, 19), 19) * 4);
        unsigned c = bits(w, 0, 4);
        if (c >= 14) {
            return "b\t" + std::to_string(t);
        }
        const char *names[] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"};
        return "bcond\t" + std::to_string(t) + "\tCOND_" + names[c] + "(c)";
    }
    if (bits(w, 25, 6) == 26) {
        unsigned sf = bits(w, 31, 1), nz = bits(w, 24, 1), rt = bits(w, 0, 5);
        U t = pc + U(sext(bits(w, 5, 19), 19) * 4);
        return "bcond\t" + std::to_string(t) + "\t" + reg(sf, rt) + (nz ? " != 0" : " == 0");
    }
    if (bits(w, 25, 6) == 27) {
        unsigned b = (bits(w, 31, 1) << 5) | bits(w, 19, 5), nz = bits(w, 24, 1), rt = bits(w, 0, 5);
        U t = pc + U(sext(bits(w, 5, 14), 14) * 4);
        return "bcond\t" + std::to_string(t) + "\t((" + x(rt) + " >> " + std::to_string(b) + ") & 1)" +
               (nz ? " != 0" : " == 0");
    }
    if (bits(w, 25, 7) == 107) {
        unsigned opc = bits(w, 21, 4), rn = bits(w, 5, 5);
        if (bits(w, 16, 5) != 31 || bits(w, 10, 6) || bits(w, 0, 5)) {
            throw std::runtime_error("branch reg variant");
        }
        if (opc == 0) {
            return "br\t" + x(rn);
        }
        if (opc == 1) {
            return "blr\t" + x(rn);
        }
        if (opc == 2) {
            return "ret\t";
        }
        throw std::runtime_error("branch reg opc");
    }
    if (bits(w, 24, 8) == 0xd4) {
        unsigned opc = bits(w, 21, 3), ll = bits(w, 0, 5), imm = bits(w, 5, 16);
        if (opc == 0 && ll == 1) {
            return "op\trt_svc(c, " + std::to_string(imm) + ", " + hex(pc) + ");";
        }
        if (opc == 1 && ll == 0) {
            return "trap\trt_brk(c, " + std::to_string(imm) + ", " + hex(pc) + ");";
        }
        throw std::runtime_error("exception");
    }
    if (bits(w, 22, 10) == 0x354) {
        unsigned L = bits(w, 21, 1), op0 = bits(w, 19, 2), op1 = bits(w, 16, 3), crn = bits(w, 12, 4),
                 crm = bits(w, 8, 4), op2 = bits(w, 5, 3), rt = bits(w, 0, 5);
        if (op0 == 0 && !L) {
            if (crn == 2 && op1 == 3 && rt == 31) {
                return "op\t/* hint */";
            }
            if (crn == 3 && op1 == 3 && rt == 31) {
                return op2 == 2 ? "op\tc->excl_addr = ~0ull;" : "op\t__atomic_thread_fence(__ATOMIC_SEQ_CST);";
            }
            throw std::runtime_error("pstate/system " + numberword(w));
        }
        if (op0 == 1) {
            if (!L && op1 == 3 && crn == 7 && crm == 4 && op2 == 1) {
                return "op\tmemset((void*)(uintptr_t)(" + x(rt) + " & ~63ull), 0, 64);";
            }
            return "op\t/* cache maintenance */";
        }
        std::string v;
        if (op0 == 3 && op1 == 3) {
            if (L) {
                if (crn == 13 && crm == 0 && op2 == 3) {
                    v = "c->tpidrro";
                } else if (crn == 13 && crm == 0 && op2 == 2) {
                    v = "c->tpidr";
                } else if (crn == 14 && crm == 0 && op2 == 0) {
                    v = "19200000ull";
                } else if (crn == 14 && crm == 0 && (op2 == 1 || op2 == 2)) {
                    v = "rt_ticks()";
                } else if (crn == 4 && crm == 2 && op2 == 0) {
                    v = "c->nzcv";
                } else if (crn == 4 && crm == 4 && op2 == 0) {
                    v = "c->fpcr";
                } else if (crn == 4 && crm == 4 && op2 == 1) {
                    v = "c->fpsr";
                } else if (crn == 0 && crm == 0 && op2 == 7) {
                    v = "4ull";
                } else if (crn == 0 && crm == 0 && op2 == 1) {
                    v = "0x8444c004ull";
                }
                if (!v.empty()) {
                    return "op\t" + set(1, rt, v);
                }
            } else {
                if (crn == 13 && crm == 0 && op2 == 2) {
                    v = "c->tpidr = " + x(rt) + ";";
                } else if (crn == 4 && crm == 2 && op2 == 0) {
                    v = "c->nzcv = " + x(rt) + " & 0xf0000000ull;";
                } else if (crn == 4 && crm == 4 && op2 == 0) {
                    v = "rt_fpcr_write(c, " + x(rt) + ");";
                } else if (crn == 4 && crm == 4 && op2 == 1) {
                    v = "c->fpsr = " + x(rt) + " & 0xf800009full;";
                }
                if (!v.empty()) {
                    return "op\t" + v;
                }
            }
        }
        throw std::runtime_error(std::string(L ? "mrs " : "msr ") + tuple(op0, op1, crn, crm, op2));
    }
    throw std::runtime_error("branch_sys " + numberword(w));
}
} // namespace lift_branch
