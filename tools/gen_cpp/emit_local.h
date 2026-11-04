// SPDX-License-Identifier: GPL-3.0-or-later
// Included in Emitter: local-register and flag emission matching gen.py.
std::vector<std::string> local_body(U entry, U end, const Scan &scan, std::set<U> &refs,
                                    const std::string &mode)
{
    const size_t n = (end - entry) / 4;
    std::vector<Instruction> ins;
    std::vector<std::vector<size_t>> succ(n + 1);
    std::vector<bool> mems(n + 1), exits(n + 1), clean(n + 1);
    std::vector<flags_local::Effect> effects;
    auto index = [&](U t, std::vector<size_t> &to) {
        if (t >= entry && t < end) to.push_back((t - entry) / 4);
    };
    exits[n] = end >= d.hi || entry_flags(end, true);
    for (size_t i = 0; i < n; ++i) {
        U a = entry + 4 * i;
        ins.push_back(instruction(d.word(a), a));
        const auto &x = ins.back();
        auto patch = patches.find(a);
        if (patch != patches.end()) {
            std::string text = apply_patch(patch_context, patch->second, a);
            succ[i].push_back(i + 1);
            static const std::regex gotos("goto L_([0-9a-f]+);");
            for (std::sregex_iterator it(text.begin(), text.end(), gotos), last; it != last; ++it)
                index(std::stoull((*it)[1].str(), nullptr, 16), succ[i]);
            mems[i] = exits[i] = true;
            effects.push_back(flags_local::effect(""));
            continue;
        }
        if (x.kind == "op" || x.kind == "bl" || x.kind == "blr") succ[i].push_back(i + 1);
        else if (x.kind == "b") index(x.target, succ[i]);
        else if (x.kind == "bcond") { index(x.target, succ[i]); succ[i].push_back(i + 1); }
        else if (x.kind == "br") {
            auto t = scan.tables.find(a);
            if (t != scan.tables.end()) for (U target : t->second) index(target, succ[i]);
        }
        if (x.kind == "bl" || x.kind == "blr") {
            mems[i] = true;
            if (x.kind == "bl" && entry_flags(x.target, false)) exits[i] = true;
        } else if ((x.kind == "b" || x.kind == "bcond") && (x.target < entry || x.target >= end)) {
            if (!d.in_text(x.target) || entry_flags(x.target, true)) exits[i] = true;
        } else if (x.kind == "br") exits[i] = true;
        effects.push_back(flags_local::effect(x.kind == "bcond" ? x.cond : x.code));
    }
    effects.push_back(flags_local::effect(""));
    auto fp = flags_local::plan(effects, succ, mems, exits);
    auto fx = [&](size_t i, const std::string &s) { return fp.used ? fp.rewrite(i, s) : s; };
    auto publish = [&](size_t i) { return fp.used ? fp.publish(i) : std::string(); };
    auto fload = [&](size_t i) { return fp.used ? fp.reload(i) : std::string(); };
    using namespace local_regs;
    std::vector<std::pair<size_t, std::string>> lines;
    const size_t label_index = n + 1;
    auto jump = [&](U t, size_t i) {
        if (t >= entry && t < end) {
            if (t <= entry + 4 * i)
                return std::string("__asm__ volatile(\"\" ::: \"memory\"); goto ") + label(t) + ";";
            return std::string("goto ") + label(t) + ";";
        }
        if (!d.in_text(t))
            return publish(i) + spill + "c->pc = " + hx(t) + "; __attribute__((musttail)) return rt_dispatch(c);";
        refs.insert(t);
        return publish(i) + spill + "__attribute__((musttail)) return " + fn(t) + "(c);";
    };
    std::string last;
    for (size_t i = 0; i < n; ++i) {
        U a = entry + 4 * i;
        const auto &x = ins[i];
        if (scan.labels.count(a)) lines.emplace_back(label_index, label(a) + ":;");
        auto patch = patches.find(a);
        if (patch != patches.end()) {
            std::string text = apply_patch(patch_context, patch->second, a), transformed;
            static const std::regex gotos("goto (L_[0-9a-f]+);");
            size_t pos = 0;
            for (std::sregex_iterator it(text.begin(), text.end(), gotos), finish; it != finish; ++it) {
                auto &m = *it;
                transformed += text.substr(pos, m.position() - pos) + "{" + reload + fload(i) + " goto " +
                               m[1].str() + "; }";
                pos = m.position() + m.length();
            }
            transformed += text.substr(pos);
            lines.emplace_back(i, "    " + spill + publish(i) + opaque + transformed +
                                  local_regs::end + reload + fload(i));
            clean[i] = true; last = "op"; continue;
        }
        std::string line;
        if (x.kind == "op") {
            auto code = fx(i, x.code);
            bool escape = code.find("rt_svc(c") != std::string::npos || code.find("rt_brk(c") != std::string::npos ||
                          code.find("rt_udf(c") != std::string::npos;
            line = escape ? spill + code + reload : code;
            clean[i] = escape;
        } else if (x.kind == "trap") line = spill + x.code;
        else if (x.kind == "b") line = jump(x.target, i);
        else if (x.kind == "bcond") line = "if (" + fx(i, x.cond) + ") { " + jump(x.target, i) + " }";
        else if (x.kind == "bl") {
            U t = x.target; clean[i] = true;
            if (setjmps.count(t)) {
                refs.insert(t);
                line = spill + opaque + "{ jmp_buf *hb_ = rt_setjmp_slot(c, c->x[0]); "
                       "if (!setjmp(*hb_)) { c->x[30] = " + hx(a + 4) + "; " + fn(t) + "(c); } }" +
                       local_regs::end + reload + fload(i);
            } else if (d.in_text(t)) {
                refs.insert(t);
                line = "c->x[30] = " + hx(a + 4) + "; " + publish(i) + spill + fn(t) + "(c);" + reload_call + fload(i);
            } else {
                line = "c->x[30] = " + hx(a + 4) + "; " + spill + "c->pc = " + hx(t) +
                       "; rt_dispatch(c);" + reload_call + fload(i);
            }
        } else if (x.kind == "blr") {
            clean[i] = true;
            line = "c->pc = " + x.code + "; c->x[30] = " + hx(a + 4) + "; " + spill +
                   "rt_dispatch(c);" + reload_call + fload(i);
        } else if (x.kind == "br") {
            auto t = scan.tables.find(a);
            if (t != scan.tables.end() && !t->second.empty()) {
                line = "switch (" + x.code + ") { ";
                for (U target : t->second) line += "case " + hx(target) + ": " + jump(target, i) + " ";
                line += "default: break; }";
                lines.emplace_back(i, "    " + line);
                line = publish(i) + spill + "c->pc = " + x.code + "; " +
                       (mode == "fast" ? "__attribute__((musttail)) return " + fn(entry) + "_r(c);"
                                       : "goto L_dispatch;");
                lines.emplace_back(i, "    " + line);
                continue;
            }
            line = publish(i) + spill + "c->pc = " + x.code + "; __attribute__((musttail)) return rt_dispatch(c);";
        } else if (x.kind == "ret") line = spill + "return;";
        lines.emplace_back(i, "    " + line);
        last = x.kind;
    }
    if (last != "b" && last != "br" && last != "ret" && last != "trap")
        lines.emplace_back(n, "    " + (end < d.hi ? jump(end, n) : spill + "rt_udf(c, " + hx(end) + ");"));
    std::vector<std::string> texts(n + 1);
    for (const auto &line : lines) if (line.first <= n) texts[line.first] += line.second;
    auto rp = local_regs::plan(texts, succ, clean, reg_locals, trust);
    std::vector<std::string> out{"void " + fn(entry) + "(Cpu *c) {", "    FN_ENTER(" + hx(entry) + ");"};
    if (fp.used) out.push_back("    " + fp.decl());
    if (!rp.regs.empty()) out.push_back("    " + rp.decl());
    for (auto &line : lines) out.push_back(line.first <= n ? rp.resolve(line.first, line.second) : line.second);
    out.push_back("}");
    return out;
}
