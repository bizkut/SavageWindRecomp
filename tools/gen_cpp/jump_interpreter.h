// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <capstone/capstone.h>
#include <array>
#include <unordered_map>
#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>
// Interpreter only for the straight-line windows used by jump-table discovery.
// Unknown instructions fail the whole discovery rather than silently changing a table.
class JumpInterpreter {
    using U = uint64_t;
    struct State {
        std::array<U, 31> x{};
        U sp = 0;
        unsigned nzcv = 0;
        std::array<__uint128_t, 32> v{};
    } state;
    const std::vector<uint8_t> &image;
    U base;
    csh cs = 0;
    std::unordered_map<U, std::array<uint8_t, 4096>> pages;
    std::unordered_map<U, cs_insn *> decoded;
    struct R {
        int index = -1;
        unsigned width = 64;
        bool sp = false, zero = false, vec = false;
    };
    R reg(unsigned id) const {
        if (!id) {
            return R{-1, 64, false, true, false};
        }
        const char *p = cs_reg_name(cs, id);
        if (!p) {
            throw std::runtime_error("unknown register");
        }
        std::string s = p;
        if (s == "sp" || s == "wsp") {
            return R{-1, s == "sp" ? 64u : 32u, true, false, false};
        }
        if (s == "xzr" || s == "wzr") {
            return R{-1, s == "xzr" ? 64u : 32u, false, true, false};
        }
        if (s == "fp") {
            return R{29, 64, false, false, false};
        }
        if (s == "lr") {
            return R{30, 64, false, false, false};
        }
        if (s.size() < 2 || s[1] < '0' || s[1] > '9') {
            throw std::runtime_error("unsupported register " + s);
        }
        int i = std::stoi(s.substr(1));
        bool vec = s[0] == 'b' || s[0] == 'h' || s[0] == 's' || s[0] == 'd' || s[0] == 'q' || s[0] == 'v';
        unsigned w = s[0] == 'w'                  ? 32
                     : s[0] == 'b'                ? 8
                     : s[0] == 'h'                ? 16
                     : s[0] == 's'                ? 32
                     : s[0] == 'q' || s[0] == 'v' ? 128
                                                  : 64;
        if (i < 0 || i >= (vec ? 32 : 31)) {
            throw std::runtime_error("invalid register " + s);
        }
        return R{i, w, false, false, vec};
    }
    static U mask(unsigned width) {
        return width >= 64 ? ~U(0) : (U(1) << width) - 1;
    }
    U get(R r) const {
        if (r.zero) {
            return 0;
        }
        U v = r.sp ? state.sp : r.vec ? U(state.v[r.index]) : state.x[r.index];
        return v & mask(r.width);
    }
    void put(R r, U v) {
        if (r.zero) {
            return;
        }
        v &= mask(r.width);
        if (r.sp) {
            state.sp = v;
        } else if (r.vec) {
            state.v[r.index] = v;
        } else {
            state.x[r.index] = v;
        }
    }
    uint8_t byte(U addr) const {
        auto it = pages.find(addr & ~U(4095));
        if (it != pages.end()) {
            return it->second[addr & 4095];
        }
        if (addr >= base && addr - base < image.size()) {
            return image[addr - base];
        }
        return 0;
    }
    U load(U addr, unsigned bytes) const {
        U v = 0;
        for (unsigned i = 0; i < bytes; i++) {
            v |= U(byte(addr + i)) << (8 * i);
        }
        return v;
    }
    void store(U addr, U val, unsigned bytes) {
        for (unsigned i = 0; i < bytes; i++) {
            U a = addr + i, page = a & ~U(4095);
            auto [it, inserted] = pages.try_emplace(page);
            if (inserted) {
                for (unsigned j = 0; j < 4096; j++) {
                    U p = page + j;
                    it->second[j] = p >= base && p - base < image.size() ? image[p - base] : 0;
                }
            }
            it->second[a & 4095] = val >> (8 * i);
        }
    }
    U operand(const cs_arm64_op &o, unsigned width = 64) const {
        U v = 0;
        if (o.type == ARM64_OP_REG) {
            v = get(reg(o.reg));
        } else if (o.type == ARM64_OP_IMM) {
            v = U(o.imm);
        } else {
            throw std::runtime_error("unsupported operand");
        }
        switch (o.ext) {
        case ARM64_EXT_UXTB:
            v &= 255;
            break;
        case ARM64_EXT_UXTH:
            v &= 65535;
            break;
        case ARM64_EXT_UXTW:
            v &= 0xffffffff;
            break;
        case ARM64_EXT_SXTB:
            v = U(int64_t(int8_t(v)));
            break;
        case ARM64_EXT_SXTH:
            v = U(int64_t(int16_t(v)));
            break;
        case ARM64_EXT_SXTW:
            v = U(int64_t(int32_t(v)));
            break;
        case ARM64_EXT_INVALID:
        case ARM64_EXT_UXTX:
        case ARM64_EXT_SXTX:
            break;
        default:
            throw std::runtime_error("unsupported register extension");
        }
        unsigned sh = o.shift.value;
        switch (o.shift.type) {
        case ARM64_SFT_INVALID:
            break;
        case ARM64_SFT_LSL:
            v <<= sh;
            break;
        case ARM64_SFT_LSR:
            v = (v & mask(width)) >> sh;
            break;
        case ARM64_SFT_ASR:
            v = width == 32 ? U(int64_t(int32_t(v) >> sh)) : U(int64_t(v) >> sh);
            break;
        case ARM64_SFT_ROR: {
            sh %= width;
            U x = v & mask(width);
            v = sh ? (x >> sh) | (x << (width - sh)) : x;
            break;
        }
        default:
            throw std::runtime_error("unsupported operand shift");
        }
        return v & mask(width);
    }
    void flags(U a, U b, U result, unsigned width, bool sub) {
        U m = mask(width), sign = U(1) << (width - 1);
        a &= m;
        b &= m;
        result &= m;
        bool C = sub ? a >= b : (width == 32 ? (a + b) > m : result < a);
        bool V = sub ? ((a ^ b) & (a ^ result) & sign) != 0 : ((~(a ^ b)) & (a ^ result) & sign) != 0;
        state.nzcv = ((result & sign) ? 8 : 0) | (!result ? 4 : 0) | (C ? 2 : 0) | (V ? 1 : 0);
    }
    bool condition(arm64_cc cc) const {
        bool N = state.nzcv & 8, Z = state.nzcv & 4, C = state.nzcv & 2, V = state.nzcv & 1;
        switch (cc) {
        case ARM64_CC_EQ:
            return Z;
        case ARM64_CC_NE:
            return !Z;
        case ARM64_CC_HS:
            return C;
        case ARM64_CC_LO:
            return !C;
        case ARM64_CC_MI:
            return N;
        case ARM64_CC_PL:
            return !N;
        case ARM64_CC_VS:
            return V;
        case ARM64_CC_VC:
            return !V;
        case ARM64_CC_HI:
            return C && !Z;
        case ARM64_CC_LS:
            return !C || Z;
        case ARM64_CC_GE:
            return N == V;
        case ARM64_CC_LT:
            return N != V;
        case ARM64_CC_GT:
            return !Z && N == V;
        case ARM64_CC_LE:
            return Z || N != V;
        default:
            return true;
        }
    }
    [[noreturn]] static void unsupported(U pc, uint32_t w, const cs_insn *i, const char *why) {
        std::ostringstream s;
        s << "unsupported jump interpreter instruction pc=0x" << std::hex << pc << " word=0x" << w << " "
          << (i ? i->mnemonic : "invalid") << " " << (i ? i->op_str : "") << " (" << why << ")";
        throw std::runtime_error(s.str());
    }

public:
    using Context = State;
    JumpInterpreter(const std::vector<uint8_t> &img, U b) : image(img), base(b) {
        if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs) != CS_ERR_OK ||
            cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
            throw std::runtime_error("cannot initialize interpreter decoder");
        }
    }
    ~JumpInterpreter() {
        for (auto &p : decoded) {
            cs_free(p.second, 1);
        }
        if (cs) {
            cs_close(&cs);
        }
    }
    void prepare() {
        for (int i = 0; i < 29; i++) {
            state.x[i] = 0x7300080000ULL;
        }
        state.sp = 0x7300080000ULL;
    }
    Context context() const {
        return state;
    }
    void restore(const Context &s) {
        state = s;
    }
    void set_reg(unsigned r, U v) {
        if (r < 31) {
            state.x[r] = v;
        }
    }
    U get_reg(unsigned r) const {
        return r < 31 ? state.x[r] : 0;
    }
    bool step(U pc) {
        U off = pc - base;
        if (pc < base || off + 4 > image.size()) {
            return false;
        }
        uint32_t w = U(byte(pc)) | U(byte(pc + 1)) << 8 | U(byte(pc + 2)) << 16 | U(byte(pc + 3)) << 24;
        auto it = decoded.find(pc);
        cs_insn *ins;
        if (it != decoded.end()) {
            const uint8_t *b = it->second->bytes;
            uint32_t old = U(b[0]) | U(b[1]) << 8 | U(b[2]) << 16 | U(b[3]) << 24;
            if (old != w) {
                cs_free(it->second, 1);
                decoded.erase(it);
                it = decoded.end();
            }
        }
        if (it == decoded.end()) {
            uint8_t bytes[] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)};
            ins = nullptr;
            if (!cs_disasm(cs, bytes, 4, pc, 1, &ins)) {
                unsupported(pc, w, nullptr, "undecodable");
            }
            decoded.emplace(pc, ins);
        } else {
            ins = it->second;
        }
        auto &a = ins->detail->arm64;
        auto &o = a.operands;
        auto id = ins->id;
        auto getop = [&](int i, unsigned width = 64) {
            if (i >= a.op_count) {
                unsupported(pc, w, ins, "missing operand");
            }
            return operand(o[i], width);
        };
        auto dest = [&](int i = 0) {
            if (i >= a.op_count || o[i].type != ARM64_OP_REG) {
                unsupported(pc, w, ins, "bad destination");
            }
            return reg(o[i].reg);
        };
        if (id == ARM64_INS_NOP || id == ARM64_INS_PRFM) {
            return true;
        }
        if (id == ARM64_INS_ADR || id == ARM64_INS_ADRP) {
            put(dest(), getop(1));
            return true;
        }
        if (id == ARM64_INS_MOVI || id == ARM64_INS_MVNI) {
            R r = dest();
            unsigned op = (w >> 29) & 1, cmode = (w >> 12) & 15, imm = ((w >> 16) & 7) << 5 | ((w >> 5) & 31);
            U v = lift_simd_shift::expanded(op, cmode, imm);
            if (op && cmode != 14 && cmode != 15) {
                v = ~v;
            }
            state.v[r.index] = __uint128_t(v) | (((w >> 30) & 1) ? __uint128_t(v) << 64 : 0);
            return true;
        }
        if (id == ARM64_INS_DUP) {
            unsigned imm = (w >> 16) & 31, sz = 0;
            while (sz < 4 && !(imm & (1u << sz))) {
                ++sz;
            }
            if (sz > 3) {
                unsupported(pc, w, ins, "dup size");
            }
            unsigned width = 8u << sz, idx = imm >> (sz + 1), src = (w >> 5) & 31, d = w & 31, Q = (w >> 30) & 1;
            U val;
            if (((w >> 11) & 15) == 1) {
                val = src == 31 ? 0 : state.x[src];
            } else {
                val = U(state.v[src] >> (idx * width));
            }
            val &= mask(width);
            __uint128_t v = 0;
            for (unsigned i = 0; i < (8u << Q) / (width / 8); i++) {
                v |= __uint128_t(val) << (i * width);
            }
            state.v[d] = v;
            return true;
        }
        if (id == ARM64_INS_ST1 || id == ARM64_INS_LD1R) {
            unsigned Q = (w >> 30) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rt = w & 31,
                     sz = (w >> 10) & 3;
            U addr = rn == 31 ? state.sp : state.x[rn];
            unsigned total = 0;
            if (!((w >> 24) & 1)) {
                unsigned opc = (w >> 12) & 15, rpt = opc == 7 ? 1 : opc == 10 ? 2 : opc == 6 ? 3 : opc == 2 ? 4 : 0;
                if (!rpt) {
                    unsupported(pc, w, ins, "unsupported interleaved store");
                }
                unsigned bytes = 8u << Q;
                total = bytes * rpt;
                for (unsigned i = 0; i < rpt; i++) {
                    __uint128_t v = state.v[(rt + i) % 32];
                    store(addr + i * bytes, U(v), 8);
                    if (Q) {
                        store(addr + i * bytes + 8, U(v >> 64), 8);
                    }
                }
            } else {
                unsigned opc = (w >> 13) & 7, S = (w >> 12) & 1, scale = opc >> 1, idx = 0;
                if ((opc & 1) || ((w >> 21) & 1)) {
                    unsupported(pc, w, ins, "multi-register single structure");
                }
                if ((scale == 3) != (id == ARM64_INS_LD1R)) {
                    unsupported(pc, w, ins, "unsupported single structure");
                }
                if (scale == 3) {
                    unsigned bytes = 1u << sz;
                    total = bytes;
                    U v = load(addr, bytes);
                    __uint128_t vec = 0;
                    for (unsigned i = 0; i < (8u << Q) / bytes; i++) {
                        vec |= __uint128_t(v) << (i * bytes * 8);
                    }
                    state.v[rt] = vec;
                } else {
                    if (scale == 0) {
                        idx = (Q << 3) | (S << 2) | sz;
                    } else if (scale == 1) {
                        idx = (Q << 2) | (S << 1) | (sz >> 1);
                    } else if (sz & 1) {
                        idx = Q;
                        scale = 3;
                    } else {
                        idx = (Q << 1) | S;
                    }
                    total = 1u << scale;
                    store(addr, U(state.v[rt] >> (idx * 8 * total)), total);
                }
            }
            if (post) {
                U value = addr + (rm == 31 ? total : state.x[rm]);
                if (rn == 31) {
                    state.sp = value;
                } else {
                    state.x[rn] = value;
                }
            }
            return true;
        }
        if (o[0].type == ARM64_OP_REG && reg(o[0].reg).vec && reg(o[0].reg).width == 128) {
            unsigned d = w & 31, nn = (w >> 5) & 31, m = (w >> 16) & 31, Q = (w >> 30) & 1, bytes = 8u << Q;
            __uint128_t maskv = Q ? ~__uint128_t(0) : __uint128_t(~U(0)), av = state.v[nn], bv = state.v[m], v = 0;
            if (id == ARM64_INS_MOV) {
                if (o[0].vector_index >= 0) {
                    unsigned imm = (w >> 16) & 31, sz = 0;
                    while (sz < 4 && !(imm & (1u << sz))) {
                        ++sz;
                    }
                    unsigned width = 8u << sz, idx = imm >> (sz + 1);
                    __uint128_t mk = __uint128_t(mask(width)) << (idx * width);
                    state.v[d] = (state.v[d] & ~mk) | (__uint128_t(getop(1) & mask(width)) << (idx * width));
                } else {
                    state.v[d] = state.v[reg(o[1].reg).index] & maskv;
                }
                return true;
            }
            if (id == ARM64_INS_REV16 || id == ARM64_INS_REV32 || id == ARM64_INS_REV64) {
                unsigned group = id == ARM64_INS_REV16 ? 2 : id == ARM64_INS_REV32 ? 4 : 8, es = 1u << ((w >> 22) & 3);
                if (es >= group) {
                    unsupported(pc, w, ins, "invalid vector reverse size");
                }
                for (unsigned i = 0; i < bytes; i++) {
                    unsigned g = i / group * group, within = i % group,
                             j = g + (group / es - 1 - within / es) * es + within % es;
                    v |= __uint128_t(U(av >> (j * 8)) & 255) << (i * 8);
                }
                state.v[d] = v;
                return true;
            }
            if (id == ARM64_INS_AND || id == ARM64_INS_ORR || id == ARM64_INS_EOR || id == ARM64_INS_BIC ||
                id == ARM64_INS_BSL) {
                if (id == ARM64_INS_AND) {
                    v = av & bv;
                } else if (id == ARM64_INS_ORR) {
                    v = av | bv;
                } else if (id == ARM64_INS_EOR) {
                    v = av ^ bv;
                } else if (id == ARM64_INS_BIC) {
                    v = av & ~bv;
                } else {
                    v = (state.v[d] & av) | (~state.v[d] & bv);
                }
                state.v[d] = v & maskv;
                return true;
            }
            if (id == ARM64_INS_EXT) {
                unsigned off = (w >> 11) & 15;
                for (unsigned i = 0; i < bytes; i++) {
                    unsigned k = off + i;
                    U b = k < bytes ? U(av >> (k * 8)) & 255 : U(bv >> ((k - bytes) * 8)) & 255;
                    v |= __uint128_t(b) << (i * 8);
                }
                state.v[d] = v;
                return true;
            }
            if (id == ARM64_INS_ADDP || id == ARM64_INS_CMEQ) {
                unsigned width = 8u << ((w >> 22) & 3), count = bytes * 8 / width;
                U lm = mask(width);
                for (unsigned i = 0; i < count; i++) {
                    U a0, b0;
                    if (id == ARM64_INS_ADDP) {
                        unsigned k = 2 * i;
                        auto src = k < count ? av : bv;
                        k %= count;
                        a0 = U(src >> (k * width)) & lm;
                        b0 = U(src >> ((k + 1) * width)) & lm;
                    } else {
                        a0 = U(av >> (i * width)) & lm;
                        b0 = U(bv >> (i * width)) & lm;
                    }
                    U r = id == ARM64_INS_ADDP ? (a0 + b0) & lm : (a0 == b0 ? lm : 0);
                    v |= __uint128_t(r) << (i * width);
                }
                state.v[d] = v;
                return true;
            }
            if (id == ARM64_INS_FADD || id == ARM64_INS_FSUB || id == ARM64_INS_FMUL || id == ARM64_INS_FCMGE ||
                id == ARM64_INS_FCMLE || id == ARM64_INS_FCMLT || id == ARM64_INS_UCVTF || id == ARM64_INS_SCVTF) {
                unsigned width = 32u << ((w >> 22) & 1), count = bytes * 8 / width;
                U lm = mask(width);
                for (unsigned i = 0; i < count; i++) {
                    U ab = U(av >> (i * width)) & lm, bb = U(bv >> (i * width)) & lm;
                    if (a.op_count > 2 && o[2].type == ARM64_OP_REG && o[2].vector_index >= 0) {
                        bb = U(state.v[reg(o[2].reg).index] >> (unsigned(o[2].vector_index) * width)) & lm;
                    }
                    if ((id == ARM64_INS_FCMGE || id == ARM64_INS_FCMLE || id == ARM64_INS_FCMLT) &&
                        (a.op_count < 3 || o[2].type != ARM64_OP_REG)) {
                        bb = 0;
                    }
                    U out = 0;
                    if (width == 32) {
                        uint32_t ai = ab, bi = bb;
                        float x, y, z = 0;
                        std::memcpy(&x, &ai, 4);
                        std::memcpy(&y, &bi, 4);
                        if (id == ARM64_INS_FADD) {
                            z = x + y;
                        } else if (id == ARM64_INS_FSUB) {
                            z = x - y;
                        } else if (id == ARM64_INS_FMUL) {
                            z = x * y;
                        } else if (id == ARM64_INS_UCVTF) {
                            z = float(ai);
                        } else if (id == ARM64_INS_SCVTF) {
                            z = float(int32_t(ai));
                        }
                        if (id == ARM64_INS_FCMGE || id == ARM64_INS_FCMLE || id == ARM64_INS_FCMLT) {
                            out = (id == ARM64_INS_FCMGE ? x >= y : id == ARM64_INS_FCMLE ? x <= y : x < y) ? lm : 0;
                        } else {
                            uint32_t b;
                            std::memcpy(&b, &z, 4);
                            out = b;
                        }
                    } else {
                        double x, y, z = 0;
                        std::memcpy(&x, &ab, 8);
                        std::memcpy(&y, &bb, 8);
                        if (id == ARM64_INS_FADD) {
                            z = x + y;
                        } else if (id == ARM64_INS_FSUB) {
                            z = x - y;
                        } else if (id == ARM64_INS_FMUL) {
                            z = x * y;
                        } else if (id == ARM64_INS_UCVTF) {
                            z = double(ab);
                        } else if (id == ARM64_INS_SCVTF) {
                            z = double(int64_t(ab));
                        }
                        if (id == ARM64_INS_FCMGE || id == ARM64_INS_FCMLE || id == ARM64_INS_FCMLT) {
                            out = (id == ARM64_INS_FCMGE ? x >= y : id == ARM64_INS_FCMLE ? x <= y : x < y) ? lm : 0;
                        } else {
                            std::memcpy(&out, &z, 8);
                        }
                    }
                    v |= __uint128_t(out) << (i * width);
                }
                state.v[d] = v;
                return true;
            }
            if (id != ARM64_INS_LDR && id != ARM64_INS_LDUR && id != ARM64_INS_LDP && id != ARM64_INS_STR &&
                id != ARM64_INS_STUR && id != ARM64_INS_STP) {
                unsupported(pc, w, ins, "unimplemented vector operation");
            }
        }
        if (id == ARM64_INS_FMOV) {
            R r = dest();
            U v;
            if (o[1].type == ARM64_OP_FP) {
                if (r.width == 32) {
                    float f = float(o[1].fp);
                    uint32_t bits;
                    std::memcpy(&bits, &f, 4);
                    v = bits;
                } else {
                    double f = o[1].fp;
                    std::memcpy(&v, &f, 8);
                }
            } else {
                v = getop(1);
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_FADD || id == ARM64_INS_FSUB || id == ARM64_INS_FMUL || id == ARM64_INS_FDIV ||
            id == ARM64_INS_FMAXNM || id == ARM64_INS_FNEG || id == ARM64_INS_FSQRT || id == ARM64_INS_FMADD ||
            id == ARM64_INS_FCMP || id == ARM64_INS_FCSEL || id == ARM64_INS_SCVTF || id == ARM64_INS_UCVTF ||
            id == ARM64_INS_FCVTZS) {
            auto real = [&](int i) -> double {
                if (o[i].type == ARM64_OP_FP) {
                    return o[i].fp;
                }
                R rr = reg(o[i].reg);
                U raw = get(rr);
                if (rr.width == 32) {
                    uint32_t b = uint32_t(raw);
                    float v;
                    std::memcpy(&v, &b, 4);
                    return v;
                }
                double v;
                std::memcpy(&v, &raw, 8);
                return v;
            };
            auto save_real = [&](R rr, double v) {
                U raw = 0;
                if (rr.width == 32) {
                    float f = float(v);
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    raw = b;
                } else {
                    std::memcpy(&raw, &v, 8);
                }
                put(rr, raw);
            };
            if (id == ARM64_INS_FCMP) {
                double l = real(0), r = real(1);
                state.nzcv = std::isnan(l) || std::isnan(r) ? 3 : l < r ? 8 : l == r ? 6 : 2;
                return true;
            }
            R r = dest();
            if (id == ARM64_INS_FCSEL) {
                put(r, get(reg(o[condition(a.cc) ? 1 : 2].reg)));
                return true;
            }
            if (id == ARM64_INS_SCVTF || id == ARM64_INS_UCVTF) {
                R src = reg(o[1].reg);
                U v = get(src);
                double d =
                    id == ARM64_INS_SCVTF ? (src.width == 32 ? double(int32_t(v)) : double(int64_t(v))) : double(v);
                save_real(r, d);
                return true;
            }
            if (id == ARM64_INS_FCVTZS) {
                double d = real(1);
                U v;
                if (std::isnan(d)) {
                    v = 0;
                } else if (r.width == 32) {
                    v = d >= double(INT32_MAX)   ? uint32_t(INT32_MAX)
                        : d <= double(INT32_MIN) ? uint32_t(INT32_MIN)
                                                 : uint32_t(int32_t(d));
                } else {
                    v = d >= double(INT64_MAX) ? U(INT64_MAX) : d <= double(INT64_MIN) ? U(INT64_MIN) : U(int64_t(d));
                }
                put(r, v);
                return true;
            }
            double l = real(1), v;
            if (id == ARM64_INS_FNEG) {
                v = -l;
            } else if (id == ARM64_INS_FSQRT) {
                v = std::sqrt(l);
            } else {
                double b = real(2);
                if (id == ARM64_INS_FADD) {
                    v = l + b;
                } else if (id == ARM64_INS_FSUB) {
                    v = l - b;
                } else if (id == ARM64_INS_FMUL) {
                    v = l * b;
                } else if (id == ARM64_INS_FDIV) {
                    v = l / b;
                } else if (id == ARM64_INS_FMAXNM) {
                    v = std::fmax(l, b);
                } else {
                    v = r.width == 32 ? double(std::fma(float(l), float(b), float(real(3)))) : std::fma(l, b, real(3));
                }
            }
            save_real(r, v);
            return true;
        }
        if (id == ARM64_INS_MOV || id == ARM64_INS_MOVZ || id == ARM64_INS_MOVN || id == ARM64_INS_MOVK) {
            R r = dest();
            U v = getop(1, r.width);
            if (id == ARM64_INS_MOVN) {
                v = ~v;
            }
            if (id == ARM64_INS_MOVK) {
                unsigned sh = o[1].shift.value;
                v = (get(r) & ~(U(0xffff) << sh)) | v;
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_ADD || id == ARM64_INS_ADDS || id == ARM64_INS_SUB || id == ARM64_INS_SUBS ||
            id == ARM64_INS_CMP || id == ARM64_INS_CMN || id == ARM64_INS_NEG || id == ARM64_INS_NEGS) {
            bool cmp = id == ARM64_INS_CMP || id == ARM64_INS_CMN, neg = id == ARM64_INS_NEG || id == ARM64_INS_NEGS;
            R r = dest();
            unsigned width = r.width;
            U a0 = neg ? 0 : getop(cmp ? 0 : 1, width), b = getop(cmp ? 1 : neg ? 1 : 2, width);
            bool sub = id == ARM64_INS_SUB || id == ARM64_INS_SUBS || id == ARM64_INS_CMP || neg;
            U v = sub ? a0 - b : a0 + b;
            if (a.update_flags || cmp || id == ARM64_INS_ADDS || id == ARM64_INS_SUBS || id == ARM64_INS_NEGS) {
                flags(a0, b, v, width, sub);
            }
            if (!cmp) {
                put(r, v);
            }
            return true;
        }
        if (id == ARM64_INS_AND || id == ARM64_INS_ANDS || id == ARM64_INS_ORR || id == ARM64_INS_EOR ||
            id == ARM64_INS_BIC || id == ARM64_INS_TST) {
            bool tst = id == ARM64_INS_TST;
            R r = dest();
            unsigned width = r.width;
            U l = getop(tst ? 0 : 1, width), v = getop(tst ? 1 : 2, width);
            v = id == ARM64_INS_ORR   ? (l | v)
                : id == ARM64_INS_EOR ? (l ^ v)
                : id == ARM64_INS_BIC ? (l & ~v)
                                      : (l & v);
            v &= mask(width);
            if (tst || id == ARM64_INS_ANDS) {
                state.nzcv = ((v >> (width - 1)) ? 8 : 0) | (!v ? 4 : 0);
            }
            if (!tst) {
                put(r, v);
            }
            return true;
        }
        if (id == ARM64_INS_LSL || id == ARM64_INS_LSR || id == ARM64_INS_ASR || id == ARM64_INS_ROR) {
            R r = dest();
            U v = getop(1, r.width), sh = getop(2) & (r.width - 1);
            if (id == ARM64_INS_LSL) {
                v <<= sh;
            } else if (id == ARM64_INS_LSR) {
                v >>= sh;
            } else if (id == ARM64_INS_ASR) {
                v = r.width == 32 ? U(int64_t(int32_t(v) >> sh)) : U(int64_t(v) >> sh);
            } else if (sh) {
                v = (v >> sh) | (v << (r.width - sh));
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_EXTR) {
            R r = dest();
            U high = getop(1, r.width), low = getop(2, r.width), shift = getop(3);
            put(r, shift ? (low >> shift) | (high << (r.width - shift)) : low);
            return true;
        }
        if (id == ARM64_INS_CLZ) {
            R r = dest();
            U v = getop(1, r.width);
            unsigned count = v ? unsigned(__builtin_clzll(v)) - (64 - r.width) : r.width;
            put(r, count);
            return true;
        }
        if (id == ARM64_INS_REV || id == ARM64_INS_REV16 || id == ARM64_INS_REV32) {
            R r = dest();
            U v = getop(1, r.width), out = 0;
            unsigned bytes = r.width / 8, group = id == ARM64_INS_REV ? bytes : id == ARM64_INS_REV16 ? 2 : 4;
            for (unsigned i = 0; i < bytes; i++) {
                out |= ((v >> (i * 8)) & 255) << ((i / group * group + group - 1 - i % group) * 8);
            }
            put(r, out);
            return true;
        }
        if (id == ARM64_INS_UBFX || id == ARM64_INS_SBFX || id == ARM64_INS_UBFIZ || id == ARM64_INS_SBFIZ ||
            id == ARM64_INS_BFI || id == ARM64_INS_BFXIL) {
            R r = dest();
            U v = getop(1), lsb = getop(2), width = getop(3), field = mask(width);
            if (id == ARM64_INS_UBFX || id == ARM64_INS_SBFX || id == ARM64_INS_BFXIL) {
                v = (v >> lsb) & field;
                if (id == ARM64_INS_SBFX && width && (v & (U(1) << (width - 1)))) {
                    v |= ~field;
                }
                if (id == ARM64_INS_BFXIL) {
                    v = (get(r) & ~field) | v;
                }
            } else {
                v = (v & field) << lsb;
                if (id == ARM64_INS_BFI) {
                    v = (get(r) & ~(field << lsb)) | v;
                } else if (id == ARM64_INS_SBFIZ && width && (v & (U(1) << (lsb + width - 1)))) {
                    v |= ~(mask(width) << lsb);
                }
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_SXTB || id == ARM64_INS_SXTH || id == ARM64_INS_SXTW || id == ARM64_INS_UXTB ||
            id == ARM64_INS_UXTH) {
            R r = dest();
            U v = getop(1);
            if (id == ARM64_INS_SXTB) {
                v = U(int64_t(int8_t(v)));
            } else if (id == ARM64_INS_SXTH) {
                v = U(int64_t(int16_t(v)));
            } else if (id == ARM64_INS_SXTW) {
                v = U(int64_t(int32_t(v)));
            } else {
                v &= id == ARM64_INS_UXTB ? 255 : 65535;
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_MUL || id == ARM64_INS_MADD || id == ARM64_INS_MSUB || id == ARM64_INS_MNEG ||
            id == ARM64_INS_SMADDL || id == ARM64_INS_UMADDL || id == ARM64_INS_UMULL || id == ARM64_INS_SMULL) {
            R r = dest();
            U v = getop(1) * getop(2);
            if (id == ARM64_INS_SMADDL || id == ARM64_INS_SMULL) {
                v = U(int64_t(int32_t(getop(1))) * int64_t(int32_t(getop(2))));
            }
            if (id == ARM64_INS_MADD || id == ARM64_INS_SMADDL || id == ARM64_INS_UMADDL) {
                v += getop(3);
            } else if (id == ARM64_INS_MSUB) {
                v = getop(3) - v;
            } else if (id == ARM64_INS_MNEG) {
                v = 0 - v;
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_SMULH) {
            __int128 v = __int128(int64_t(getop(1))) * __int128(int64_t(getop(2)));
            put(dest(), U(v >> 64));
            return true;
        }
        if (id == ARM64_INS_UDIV || id == ARM64_INS_SDIV) {
            R r = dest();
            U x = getop(1, r.width), y = getop(2, r.width), v = 0;
            if (y) {
                if (id == ARM64_INS_UDIV) {
                    v = x / y;
                } else if (r.width == 32) {
                    int32_t sx = x, sy = y;
                    v = sy == -1 && sx == INT32_MIN ? U(uint32_t(sx)) : U(int64_t(sx / sy));
                } else {
                    int64_t sx = x, sy = y;
                    v = sy == -1 && sx == INT64_MIN ? U(sx) : U(sx / sy);
                }
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_CSEL || id == ARM64_INS_CSINC || id == ARM64_INS_CSINV || id == ARM64_INS_CSNEG ||
            id == ARM64_INS_CSET || id == ARM64_INS_CINC || id == ARM64_INS_CNEG || id == ARM64_INS_CINV) {
            R r = dest();
            bool c = condition(a.cc);
            U v;
            if (id == ARM64_INS_CSET) {
                v = c ? 1 : 0;
            } else if (id == ARM64_INS_CINC || id == ARM64_INS_CNEG || id == ARM64_INS_CINV) {
                v = getop(1, r.width);
                if (c) {
                    v = id == ARM64_INS_CINC ? v + 1 : id == ARM64_INS_CNEG ? 0 - v : ~v;
                }
            } else {
                v = getop(c ? 1 : 2, r.width);
                if (!c) {
                    if (id == ARM64_INS_CSINC) {
                        ++v;
                    } else if (id == ARM64_INS_CSINV) {
                        v = ~v;
                    } else if (id == ARM64_INS_CSNEG) {
                        v = 0 - v;
                    }
                }
            }
            put(r, v);
            return true;
        }
        if (id == ARM64_INS_CCMP || id == ARM64_INS_CCMN) {
            R r = reg(o[0].reg);
            if (condition(a.cc)) {
                U l = getop(0, r.width), b = getop(1, r.width);
                flags(l, b, id == ARM64_INS_CCMP ? l - b : l + b, r.width, id == ARM64_INS_CCMP);
            } else {
                state.nzcv = getop(2) & 15;
            }
            return true;
        }
        if (id == ARM64_INS_LDR || id == ARM64_INS_LDUR || id == ARM64_INS_LDRB || id == ARM64_INS_LDURB ||
            id == ARM64_INS_LDRH || id == ARM64_INS_LDURH || id == ARM64_INS_LDRSB || id == ARM64_INS_LDRSH ||
            id == ARM64_INS_LDRSW || id == ARM64_INS_LDURSW || id == ARM64_INS_LDAR || id == ARM64_INS_STR ||
            id == ARM64_INS_STUR || id == ARM64_INS_STRB || id == ARM64_INS_STURB || id == ARM64_INS_STRH ||
            id == ARM64_INS_STURH || id == ARM64_INS_LDP || id == ARM64_INS_LDPSW || id == ARM64_INS_STP) {
            bool pair = id == ARM64_INS_LDP || id == ARM64_INS_LDPSW || id == ARM64_INS_STP;
            bool st = id == ARM64_INS_STR || id == ARM64_INS_STUR || id == ARM64_INS_STRB || id == ARM64_INS_STURB ||
                      id == ARM64_INS_STRH || id == ARM64_INS_STURH || id == ARM64_INS_STP;
            R r = dest(), r2 = pair ? dest(1) : r;
            unsigned mi = pair ? 2 : 1;
            U addr;
            if (o[mi].type == ARM64_OP_IMM) {
                addr = U(o[mi].imm);
            } else if (o[mi].type == ARM64_OP_MEM) {
                auto &mo = o[mi];
                addr = get(reg(mo.mem.base)) + U(int64_t(mo.mem.disp));
                if (mo.mem.index) {
                    auto index = mo;
                    index.type = ARM64_OP_REG;
                    index.reg = mo.mem.index;
                    addr += operand(index);
                }
            } else {
                unsupported(pc, w, ins, "bad memory address");
            }
            unsigned bytes = r.width / 8;
            if (id == ARM64_INS_LDRB || id == ARM64_INS_LDURB || id == ARM64_INS_LDRSB || id == ARM64_INS_STRB ||
                id == ARM64_INS_STURB) {
                bytes = 1;
            } else if (id == ARM64_INS_LDRH || id == ARM64_INS_LDURH || id == ARM64_INS_LDRSH || id == ARM64_INS_STRH ||
                       id == ARM64_INS_STURH) {
                bytes = 2;
            } else if (id == ARM64_INS_LDRSW || id == ARM64_INS_LDURSW || id == ARM64_INS_LDPSW) {
                bytes = 4;
            }
            auto read = [&](U pos) -> __uint128_t {
                U v = load(pos, std::min(bytes, 8u));
                if (bytes == 16) {
                    return __uint128_t(v) | (__uint128_t(load(pos + 8, 8)) << 64);
                }
                if (id == ARM64_INS_LDRSB) {
                    v = U(int64_t(int8_t(v)));
                } else if (id == ARM64_INS_LDRSH) {
                    v = U(int64_t(int16_t(v)));
                } else if (id == ARM64_INS_LDRSW || id == ARM64_INS_LDURSW || id == ARM64_INS_LDPSW) {
                    v = U(int64_t(int32_t(v)));
                }
                return v;
            };
            auto save = [&](U pos, R r0) {
                __uint128_t v = r0.vec ? state.v[r0.index] : get(r0);
                store(pos, U(v), std::min(bytes, 8u));
                if (bytes == 16) {
                    store(pos + 8, U(v >> 64), 8);
                }
            };
            __uint128_t v = 0, v2 = 0;
            if (st) {
                save(addr, r);
                if (pair) {
                    save(addr + bytes, r2);
                }
            } else {
                v = read(addr);
                if (pair) {
                    v2 = read(addr + bytes);
                }
            }
            if (a.writeback && o[mi].type == ARM64_OP_MEM) {
                auto br = reg(o[mi].mem.base);
                U wb = addr;
                if (a.post_index) {
                    wb = get(br);
                    if (mi + 1 < a.op_count) {
                        wb += getop(mi + 1);
                    }
                }
                put(br, wb);
            }
            if (!st) {
                if (bytes == 16) {
                    state.v[r.index] = v;
                } else {
                    put(r, U(v));
                }
                if (pair) {
                    if (bytes == 16) {
                        state.v[r2.index] = v2;
                    } else {
                        put(r2, U(v2));
                    }
                }
            }
            return true;
        }
        unsupported(pc, w, ins, "unimplemented operation");
    }
};
