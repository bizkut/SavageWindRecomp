// SPDX-License-Identifier: GPL-3.0-or-later
#define S9_DISCOVERY_ONLY
#include "discover.cpp"
#include <regex>

static std::string hx(U a)
{
    return lift_imm::hex(a);
}

static std::string fn(U a)
{
    auto s = hx(a);
    return "f_" + s.substr(2, s.size() - 5);
}

static std::string label(U a)
{
    return "L_" + fn(a).substr(2);
}

static std::string sanitize(std::string s)
{
    for (char &c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            c = '_';
    return "hle_" + s;
}

struct Instruction {
    std::string kind, code, cond;
    U target = 0;
};

static Instruction instruction(uint32_t w, U pc)
{
    unsigned op = (w >> 25) & 15;
    std::string s;
    Instruction i;
    try {
        if ((op & 14) == 8)
            s = lift_imm::lift(w, pc);
        else if ((op & 14) == 10) {
            s = lift_branch::lift(w, pc);
            auto p = s.find('\t');
            i.kind = s.substr(0, p);
            s = s.substr(p + 1);
            if (i.kind == "b" || i.kind == "bl" || i.kind == "bcond") {
                auto q = s.find('\t');
                i.target = std::stoull(s.substr(0, q));
                if (q != std::string::npos)
                    i.cond = s.substr(q + 1);
            } else {
                i.code = s;
            }
            return i;
        } else if ((op & 5) == 4 && !((w >> 26) & 1))
            s = lift_mem::lift(w, pc);
        else if ((op & 5) == 4)
            s = lift_simd_mem::lift(w, pc);
        else if ((op & 7) == 5)
            s = lift_reg::lift(w);
        else if ((op & 7) == 7) {
            if (auto t = lift_simd_basic::type(w))
                s = lift_simd_basic::lift(w, t);
            else if (auto t = lift_simd_diff::type(w))
                s = lift_simd_diff::lift(w, t);
            else if (auto t = lift_simd_same::type(w))
                s = lift_simd_same::lift(w, t);
            else if (auto t = lift_simd_indexed::type(w))
                s = lift_simd_indexed::lift(w, t);
            else if (auto t = lift_simd_shift::type(w))
                s = lift_simd_shift::lift(w, t);
            else if (auto t = lift_simd_misc::type(w))
                s = lift_simd_misc::lift(w, t);
            else if (lift_scalar_fp::handles(w))
                s = lift_scalar_fp::lift(w);
            else
                throw std::runtime_error(lift_simd_reserved::error(w));
        } else {
            return {"trap", "rt_udf(c, " + hx(pc) + ");", {}, 0};
        }
        return {"op", s, {}, 0};
    } catch (const std::runtime_error &e) {
        return {"trap", "rt_udf(c, " + hx(pc) + "); /* " + e.what() + " */", {}, 0};
    }
}
struct Emitter {
    Discovery &d;
    std::set<U> setjmps;
    std::map<U, std::string> hle;

    explicit Emitter(Discovery &dis, const std::string &hle_path) : d(dis)
    {
        std::set<std::string> want;
        std::ifstream f(hle_path);
        std::string s;
        while (std::getline(f, s)) {
            if (!s.empty() && s[0] == '#')
                continue;
            auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            if (a != std::string::npos)
                want.insert(s.substr(a, b - a + 1));
        }
        auto is_setjmp = [](const std::string &x) {
            return x == "setjmp" || x == "_setjmp" || x == "__setjmp";
        };
        for (auto &sym : d.nso.syms) {
            if (sym.shndx && want.count(sym.name))
                hle[d.base + sym.value] = sym.name;
            if (sym.shndx && is_setjmp(sym.name))
                setjmps.insert(d.base + sym.value);
        }
        std::set<U> slots;
        for (auto &r : d.nso.relocs)
            if (r.type == 1026 && r.sym < d.nso.syms.size() && is_setjmp(d.nso.syms[r.sym].name))
                slots.insert(d.base + r.off);
        if (!slots.empty()) {
            for (U a = d.lo; a + 12 < d.hi; a += 4) {
                U w = d.word(a), ld = d.word(a + 4);
                if ((w & 0x9f00001f) != 0x90000010 || d.word(a + 12) != 0xd61f0220 ||
                    (ld & 0xffc003ff) != 0xf9400211)
                    continue;
                U imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2);
                U slot = (a & ~U(4095)) + U(lift_imm::sext(imm, 21) * 4096) + ((ld >> 10) & 4095) * 8;
                if (slots.count(slot))
                    setjmps.insert(a);
            }
        }
    }
    std::vector<std::string> body(U entry, U end, const Scan &scan, std::set<U> &refs, const std::string &mode)
    {
        std::vector<std::string> out;
        bool all = mode == "resume" || (mode == "plain" && (!scan.tables.empty() || !scan.all.empty()));
        if (mode == "resume") {
            out.push_back("__attribute__((cold, noinline, optnone)) static void " + fn(entry) + "_r(Cpu *c) {");
            out.push_back("    goto L_dispatch;");
        } else {
            out.push_back("void " + fn(entry) + "(Cpu *c) {");
            out.push_back("    FN_ENTER(" + hx(entry) + ");");
        }
        auto jump = [&](U t) {
            if (t >= entry && t < end)
                return "goto " + label(t) + ";";
            if (!d.in_text(t))
                return "c->pc = " + hx(t) + "; __attribute__((musttail)) return rt_dispatch(c);";
            refs.insert(t);
            return "__attribute__((musttail)) return " + fn(t) + "(c);";
        };
        std::string last;
        for (U a = entry; a < end; a += 4) {
            auto i = instruction(d.word(a), a);
            if (all || scan.labels.count(a))
                out.push_back(label(a) + ":;");
            if (i.kind == "op" || i.kind == "trap")
                out.push_back("    " + i.code);
            else if (i.kind == "b")
                out.push_back("    " + jump(i.target));
            else if (i.kind == "bcond")
                out.push_back("    if (" + i.cond + ") { " + jump(i.target) + " }");
            else if (i.kind == "bl") {
                U t = i.target;
                if (setjmps.count(t)) {
                    refs.insert(t);
                    out.push_back("    { jmp_buf *hb_ = rt_setjmp_slot(c, c->x[0]); "
                                  "if (!setjmp(*hb_)) { c->x[30] = " + hx(a + 4) + "; " + fn(t) + "(c); } }");
                } else if (d.in_text(t)) {
                    refs.insert(t);
                    out.push_back("    c->x[30] = " + hx(a + 4) + "; " + fn(t) + "(c);");
                } else {
                    out.push_back("    c->x[30] = " + hx(a + 4) + "; c->pc = " + hx(t) + "; rt_dispatch(c);");
                }
            } else if (i.kind == "blr") {
                out.push_back("    c->pc = " + i.code + "; c->x[30] = " + hx(a + 4) + "; rt_dispatch(c);");
            } else if (i.kind == "br") {
                if (scan.all.count(a)) {
                    out.push_back("    c->pc = " + i.code + "; goto L_dispatch;");
                    continue;
                }
                auto it = scan.tables.find(a);
                if (it != scan.tables.end() && !it->second.empty()) {
                    std::string line = "    switch (" + i.code + ") { ";
                    for (U t : it->second)
                        line += "case " + hx(t) + ": " + jump(t) + " ";
                    line += "default: break; }";
                    out.push_back(line);
                    out.push_back("    c->pc = " + i.code + "; " +
                                  (mode == "fast" ? "__attribute__((musttail)) return " + fn(entry) + "_r(c);"
                                                  : "goto L_dispatch;"));
                    continue;
                }
                out.push_back("    c->pc = " + i.code + "; __attribute__((musttail)) return rt_dispatch(c);");
            } else if (i.kind == "ret") {
                out.push_back("    return;");
            }
            last = i.kind;
        }
        if (last != "b" && last != "br" && last != "ret" && last != "trap")
            out.push_back(end < d.hi ? "    " + jump(end) : "    rt_udf(c, " + hx(end) + ");");
        if (all) {
            out.push_back("L_dispatch:");
            std::string line = "    switch (c->pc) { ";
            for (U t = entry; t < end; t += 4)
                line += "case " + hx(t) + ": goto " + label(t) + "; ";
            out.push_back(line + "default: break; }");
            out.push_back("    __attribute__((musttail)) return rt_dispatch(c);");
        }
        out.push_back("}");
        return out;
    }
    std::vector<std::string> function(U a, U end, const Scan &scan, std::set<U> &refs)
    {
        if (hle.count(a))
            return {"void " + fn(a) + "(Cpu *c) {", "    " + sanitize(hle.at(a)) + "(c);", "}"};
        if (scan.tables.empty() || !scan.all.empty())
            return body(a, end, scan, refs, "plain");
        std::vector<std::string> r{"static void " + fn(a) + "_r(Cpu *c);"};
        auto fast = body(a, end, scan, refs, "fast"), resume = body(a, end, scan, refs, "resume");
        r.insert(r.end(), fast.begin(), fast.end());
        r.insert(r.end(), resume.begin(), resume.end());
        return r;
    }
};
static void save(const fs::path &path, const std::string &text)
{
    std::ifstream input(path, std::ios::binary);
    if (input) {
        std::string old((std::istreambuf_iterator<char>(input)), {});
        if (old == text)
            return;
    }
    std::ofstream output(path, std::ios::binary);
    require(bool(output), "cannot write output");
    output << text;
    require(bool(output), "output write failed");
}

int main(int argc, char **argv) try {
    require(argc >= 5, "usage: gen MODULE NSO LAYOUT OUTDIR [--hle FILE] [--simd c] "
                       "[--chunk N] [--chunk-bytes N] [--big-fn N] [--limit N]");
    std::string name = argv[1], hlepath;
    require(name != "main" && name != "all" && name != "layout", "main and layout generation require slice 5");
    size_t chunk = 1500, bytes = 1500000, big = 100000, limit = 0;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        require(i + 1 < argc, "missing option value");
        std::string val = argv[++i];
        if (arg == "--hle")
            hlepath = val;
        else if (arg == "--simd")
            require(val == "c", "only portable C SIMD is supported");
        else if (arg == "--chunk")
            chunk = std::stoull(val);
        else if (arg == "--chunk-bytes")
            bytes = std::stoull(val);
        else if (arg == "--big-fn")
            big = std::stoull(val);
        else if (arg == "--limit")
            limit = std::stoull(val);
        else
            throw std::runtime_error("unknown option: " + arg);
    }
    require(chunk && bytes, "chunk limits must be positive");
    Nso n(file(argv[2]));
    std::ifstream layoutfile(argv[3]);
    require(bool(layoutfile), "layout unavailable");
    std::string line;
    U base = 0;
    bool found = false;
    while (std::getline(layoutfile, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        std::string nm, b, sz, id;
        require(bool(s >> nm >> b >> sz >> id), "bad layout");
        if (nm == name) {
            require(id == n.h.id, "module build ID mismatch");
            base = std::stoull(b, nullptr, 16);
            found = true;
        }
    }
    require(found, "module absent from layout");
    Discovery d(n, base);
    Emitter e(d, hlepath);
    auto order = d.run(false);
    auto full_order = order;
    if (limit && limit < order.size())
        order.resize(limit);
    fs::path outdir = argv[4];
    fs::create_directories(outdir);
    struct Pending {
        U addr;
        std::vector<std::string> lines;
        std::set<U> refs;
    };
    std::optional<Pending> pending;
    size_t ci = 0, nchunks = 0;
    while (ci < order.size() || pending) {
        std::vector<std::string> body;
        std::set<U> refs;
        std::vector<U> fns;
        size_t size = 0;
        while ((pending || ci < order.size()) && fns.size() < chunk && size < bytes) {
            Pending p;
            if (pending) {
                p = std::move(*pending);
                pending.reset();
            } else {
                p.addr = order[ci++];
                U end = ci < full_order.size() ? full_order[ci] : d.hi;
                p.lines = e.function(p.addr, end, d.scanned.at({p.addr, end}), p.refs);
            }
            size_t nbytes = 0;
            for (auto &l : p.lines)
                nbytes += l.size() + 1;
            if (nbytes > big && !fns.empty()) {
                pending = std::move(p);
                break;
            }
            size += nbytes;
            fns.push_back(p.addr);
            refs.insert(p.refs.begin(), p.refs.end());
            body.insert(body.end(), p.lines.begin(), p.lines.end());
            if (nbytes > big)
                break;
        }
        std::string text = "#include \"cpu.h\"\n#include \"simd.h\"\n";
        for (auto &l : body) {
            if (l.find("rt_setjmp_slot(") != std::string::npos) {
                text += "#include <setjmp.h>\njmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf);\n";
                break;
            }
        }
        for (U a : refs)
            text += "void " + fn(a) + "(Cpu *c);\n";
        for (U a : fns)
            if (e.hle.count(a))
                text += "void " + sanitize(e.hle.at(a)) + "(Cpu *c);\n";
        for (size_t i = 0; i < body.size(); i++) {
            if (i)
                text += '\n';
            text += body[i];
        }
        text += '\n';
        std::ostringstream filename;
        filename << name << '_' << std::setw(3) << std::setfill('0') << nchunks++ << ".c";
        save(outdir / filename.str(), text);
    }
    std::regex old(name + "_([0-9]{3})\\.c");
    for (auto &ent : fs::directory_iterator(outdir)) {
        std::smatch m;
        auto s = ent.path().filename().string();
        if (std::regex_match(s, m, old) && std::stoul(m[1]) >= nchunks)
            fs::remove(ent.path());
    }
    std::string table = "#include \"cpu.h\"\n";
    for (U a : order)
        table += "void " + fn(a) + "(Cpu *c);\n";
    table += "const uint64_t " + name + "_fn_addrs[] = {\n";
    if (order.empty())
        table += "0";
    for (size_t i = 0; i < order.size(); i++) {
        if (i)
            table += ",\n";
        table += hx(order[i]);
    }
    table += "};\nconst GuestFn " + name + "_fn_ptrs[] = {\n";
    if (order.empty())
        table += "0";
    for (size_t i = 0; i < order.size(); i++) {
        if (i)
            table += ",\n";
        table += fn(order[i]);
    }
    table += "};\nconst uint64_t " + name + "_fn_count = " + std::to_string(order.size()) + ";\n";
    save(outdir / (name + "_table.c"), table);
    std::cerr << name << ": " << order.size() << " functions, " << nchunks << " chunks\n";
} catch (const std::exception &e) {
    std::cerr << "gen: " << e.what() << '\n';
    return 1;
}
