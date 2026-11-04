// SPDX-License-Identifier: GPL-3.0-or-later
#define S9_DISCOVERY_ONLY
#include "discover.cpp"
#include "sites.h"
#include "patches.h"
#include <regex>
#include "regs_local.h"
#include "flags_local.h"

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
    PatchContext patch_context;
    std::map<U, std::string> patches;
    bool locals = false, trust = true;
    size_t reg_locals = 6;
    std::map<std::tuple<U, bool, int>, bool> entry_flags_cache;

    explicit Emitter(Discovery &dis, const std::string &hle_path, const std::map<std::string, U> &sites,
                     const std::string *hle_text = nullptr)
        : d(dis), patch_context{dis.nso.image.data(), dis.nso.image.size(), dis.base, sites},
          patches(make_patches(patch_context))
    {
        std::set<std::string> want;
        std::ifstream file_stream;
        std::istringstream memory_stream;
        if (hle_text)
            memory_stream.str(*hle_text);
        else
            file_stream.open(hle_path);
        std::istream &f = hle_text ? static_cast<std::istream &>(memory_stream) : file_stream;
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
    bool entry_flags(U a, bool unknown, int depth = 4)
    {
        auto key = std::make_tuple(a, unknown, depth);
        auto found = entry_flags_cache.find(key);
        if (found != entry_flags_cache.end()) return found->second;
        auto result = [&]() {
            for (int n = 0; n < 32; ++n) {
                if (!d.in_text(a) || patches.count(a)) return unknown;
                auto ins = instruction(d.word(a), a);
                const auto &text = ins.kind == "bcond" ? ins.cond : ins.kind == "op" ? ins.code : std::string();
                static const std::regex flag_read(
                    R"(\b(?:COND_|FLAG_)|\b(?:adc|sbc|adcs|sbcs)\d+\(|c->nzcv(?! =))");
                if (std::regex_search(text, flag_read)) return true;
                if (ins.kind == "op" && sets_flags(ins.code)) return false;
                if (ins.kind == "bl" || ins.kind == "blr" || ins.kind == "ret" || ins.kind == "trap") return false;
                if (ins.kind == "br") return unknown;
                if (ins.kind == "b") { a = ins.target; continue; }
                if (ins.kind == "bcond" && (!depth || entry_flags(ins.target, unknown, depth - 1)))
                    return depth ? true : unknown;
                a += 4;
            }
            return unknown;
        }();
        entry_flags_cache[key] = result;
        return result;
    }
#include "emit_local.h"
    std::vector<std::string> body(U entry, U end, const Scan &scan, std::set<U> &refs, const std::string &mode)
    {
        std::vector<std::string> out;
        bool all = mode == "resume" || (mode == "plain" && (!scan.tables.empty() || !scan.all.empty()));
        if (locals && !all) return local_body(entry, end, scan, refs, mode);
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
            auto patch = patches.find(a);
            if (patch != patches.end()) {
                out.push_back("    " + apply_patch(patch_context, patch->second, a));
                last = "op";
                continue;
            }
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
#ifdef S9_OUTPUT_SINK
extern void s9_output_sink(const std::string &filename, const std::string &text);
#endif

static void save(const fs::path &path, const std::string &text)
{
#ifdef S9_OUTPUT_SINK
    s9_output_sink(path.string(), text);
#else
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
#endif
}

struct Options {
    std::string hlepath;
    size_t chunk = 1500;
    size_t bytes = 1500000;
    size_t big = 100000;
    size_t limit = 0;
    bool any_version = false;
    bool no_sites = false;
    bool locals = true, trust = true;
    size_t reg_locals = 6;
};

static void generate(const std::string &name, const fs::path &input, U base, const fs::path &outdir,
                     const Options &opts, const std::map<std::string, U> *resolved_sites = nullptr,
                     const Bytes *input_bytes = nullptr, const std::string *hle_text = nullptr)
{
    Nso n(input_bytes ? *input_bytes : file(input));
    std::map<std::string, U> sites;
    if (name == "main" && !opts.no_sites) {
        if (resolved_sites)
            sites = *resolved_sites;
        else
            sites = game_sites::for_module(n, base, opts.any_version).sites;
    }
    Discovery d(n, base);
    Emitter e(d, opts.hlepath, sites, hle_text);
    e.locals = opts.locals;
    e.trust = opts.trust;
    e.reg_locals = opts.reg_locals;
    auto order = d.run(false);
    auto full_order = order;
    if (opts.limit && opts.limit < order.size())
        order.resize(opts.limit);
#ifndef S9_OUTPUT_SINK
    fs::create_directories(outdir);
#endif
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
        while ((pending || ci < order.size()) && fns.size() < opts.chunk && size < opts.bytes) {
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
            if (nbytes > opts.big && !fns.empty()) {
                pending = std::move(p);
                break;
            }
            size += nbytes;
            fns.push_back(p.addr);
            refs.insert(p.refs.begin(), p.refs.end());
            body.insert(body.end(), p.lines.begin(), p.lines.end());
            if (nbytes > opts.big)
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
#ifndef S9_OUTPUT_SINK
    std::regex old(name + "_([0-9]{3})\\.c");
    for (auto &ent : fs::directory_iterator(outdir)) {
        std::smatch m;
        auto s = ent.path().filename().string();
        if (std::regex_match(s, m, old) && std::stoul(m[1]) >= nchunks)
            fs::remove(ent.path());
    }
#endif
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
}

static std::string hex_plain(U value)
{
    auto text = hx(value);
    return text.substr(0, text.size() - 3);
}

static void write_layout(const std::vector<Layout> &modules, const fs::path &outdir,
                         const std::string &version, const std::map<std::string, U> &sites)
{
#ifndef S9_OUTPUT_SINK
    fs::create_directories(outdir);
#endif
    std::string text = "# module layout (tools/layout.py), load order: name, guest base, image size, NSO build id\n";
    std::string source = "// Module layout of this game code (tools/layout.py). Generated.\n"
                         "#include \"gamelib.h\"\n\n";
    for (const auto &module : modules) {
        text += module.name + " " + hex_plain(module.base) + " " + hex_plain(module.size) + " " + module.id + "\n";
        source += "extern const uint64_t " + module.name + "_fn_addrs[], " + module.name + "_fn_count;\n"
                  "extern const GuestFn " + module.name + "_fn_ptrs[];\n";
    }
    source += "\nconst GameLibModule game_modules[] = {\n";
    for (const auto &module : modules) {
        source += "    {\"" + module.name + "\", " + hx(module.base) + ", " + hx(module.size) + ", \"" +
                  module.id + "\", " + module.name + "_fn_addrs, " + module.name + "_fn_ptrs, &" +
                  module.name + "_fn_count},\n";
    }
    source += "};\nconst uint32_t game_module_count = " + std::to_string(modules.size()) + ";\n";
    source += "\nconst char game_version[] = \"" + version + "\";\nconst GameLibSite game_sites[] = {\n";
    if (sites.empty())
        source += "    {0, 0},\n";
    for (const auto &[name, address] : sites)
        source += "    {\"" + name + "\", " + hx(address) + "},\n";
    source += "};\nconst uint32_t game_site_count = " + std::to_string(sites.size()) + ";\n";
    save(outdir / "layout.txt", text);
    save(outdir / "layout.c", source);
}

int main(int argc, char **argv) try {
    require(argc >= 4, "usage: gen NAME INPUT [LAYOUT] OUTDIR [options]");
    std::string name = argv[1];
    fs::path input = argv[2];
    std::vector<std::string> paths;
    Options opts;
    opts.locals = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--locals") {
            opts.locals = true;
        } else if (arg == "--no-callee-saved-trust") {
            opts.trust = false;
        } else if (arg == "--no-sites") {
            opts.no_sites = true;
        } else if (arg == "--any-version") {
            opts.any_version = true;
        } else if (arg.rfind("--", 0) == 0) {
            require(i + 1 < argc, "missing option value");
            std::string val = argv[++i];
            if (arg == "--hle")
                opts.hlepath = val;
            else if (arg == "--simd")
                require(val == "c", "only portable C SIMD is supported");
            else if (arg == "--chunk")
                opts.chunk = std::stoull(val);
            else if (arg == "--chunk-bytes")
                opts.bytes = std::stoull(val);
            else if (arg == "--big-fn")
                opts.big = std::stoull(val);
            else if (arg == "--limit")
                opts.limit = std::stoull(val);
            else if (arg == "--reg-locals")
                opts.reg_locals = std::stoull(val);
            else
                throw std::runtime_error("unknown option: " + arg);
        } else {
            paths.push_back(arg);
        }
    }
    require(opts.chunk && opts.bytes, "chunk limits must be positive");
    bool all = name == "all";
    bool layout_only = name == "layout";
    require(paths.size() == (all || layout_only ? 1u : 2u), "wrong number of arguments");
    if (all || layout_only) {
        auto modules = layout(input);
        auto main_module = std::find_if(modules.begin(), modules.end(),
                                        [](const Layout &m) { return m.name == "main"; });
        require(main_module != modules.end(), "main module absent");
        std::string version;
        std::map<std::string, U> sites;
        if (!opts.no_sites) {
            Nso main_nso(file(input / "main"));
            auto result = game_sites::for_module(main_nso, main_module->base, opts.any_version);
            version = std::move(result.version);
            sites = std::move(result.sites);
        }
        fs::path outdir = paths[0];
        write_layout(modules, outdir, version, sites);
        if (all) {
#ifndef S9_OUTPUT_SINK
            std::set<std::string> present;
            for (const auto &module : modules)
                present.insert(module.name);
            std::vector<std::string> names{"main"};
            for (int i = 0; i < 10; ++i)
                names.push_back("subsdk" + std::to_string(i));
            names.push_back("sdk");
            for (const auto &module : names)
                if (!present.count(module) && fs::is_directory(outdir / module))
                    fs::remove_all(outdir / module);
#endif
            for (const auto &module : modules) {
                Options module_opts = opts;
                if (module.name == "main")
                    module_opts.hlepath.clear();
                generate(module.name, input / module.name, module.base, outdir / module.name, module_opts, &sites);
            }
        }
        return 0;
    }
    std::ifstream layoutfile(paths[0]);
    require(bool(layoutfile), "layout unavailable");
    std::string line;
    U base = 0;
    bool found = false;
    Header header(file(input));
    while (std::getline(layoutfile, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        std::string nm, b, sz, id;
        require(bool(s >> nm >> b >> sz >> id), "bad layout");
        if (nm == name) {
            require(id == header.id, "module build ID mismatch");
            base = std::stoull(b, nullptr, 16);
            found = true;
        }
    }
    require(found, "module absent from layout");
    generate(name, input, base, paths[1], opts);
    return 0;
} catch (const std::exception &e) {
    std::cerr << "gen: " << e.what() << '\n';
    return 1;
}
