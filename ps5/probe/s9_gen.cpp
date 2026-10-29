// M12 on-console generator probe; generated files are hashed in memory, never written.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
void s9_output_sink(const std::string &filename, const std::string &text);
#define S9_OUTPUT_SINK
#define main s9_generator_main
#include "../../tools/gen_cpp/emit.cpp"
#undef main
#include "../tooling/native/hash.hpp"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#ifdef __PROSPERO__
#include <ps5platform/heap.h>
#include <unistd.h>
extern "C" int sceKernelDebugOutText(int, const char *);
extern "C" int sceSystemServiceLoadExec(const char *, const char *const *);
#endif

static void say(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __PROSPERO__
    sceKernelDebugOutText(0, buf);
#else
    std::fputs(buf, stdout);
    std::fflush(stdout);
#endif
}

static std::string current_module;
static size_t module_files, module_bytes;
static unsigned long long sampled_peak;

static void sample_heap() {
#ifdef __PROSPERO__
    struct ps5_heap_stats heap{};
    ps5_heap_stats(&heap);
    sampled_peak = std::max(sampled_peak, static_cast<unsigned long long>(heap.peak_bytes));
#endif
}

void s9_output_sink(const std::string &filename, const std::string &text) {
    sample_heap();
    auto sha = ps5::crypto::sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    std::string digest = hexbytes(sha.data(), sha.size());
    say("[s9] file %s bytes %zu sha256 %s\n", filename.c_str(), text.size(), digest.c_str());
    ++module_files;
    module_bytes += text.size();
}

static uint64_t now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
static int finish(int code) {
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sleep(1);
#endif
    return code;
}
int main(int argc, char **argv) {
#ifdef __PROSPERO__
    (void)argc;
    (void)argv;
    const std::string root = "/data/savagewind/s9";
#else
    const std::string root = argc > 1 ? argv[1] : "build/ps5/s9-input";
#endif
    try {
        for (const auto &name : {"main", "subsdk0", "sdk", "layout.txt", "hle_symbols.txt"}) {
            std::string path = root + "/" + name;
            struct stat st{};
            if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) {
                say("[s9] failed input %s\n", path.c_str());
                return finish(1);
            }
        }
        auto modules = layout(root);
        require(modules.size() == 3 && modules[0].name == "main" && modules[1].name == "subsdk0" &&
                modules[2].name == "sdk", "unexpected module layout");
        std::ifstream expected(root + "/layout.txt");
        require(bool(expected), "layout unavailable");
        std::ostringstream layout_text;
        layout_text << expected.rdbuf();
        std::string actual = "# module layout (tools/layout.py), load order: name, guest base, image size, NSO build id\n";
        for (const auto &module : modules)
            actual += module.name + " " + hex_plain(module.base) + " " + hex_plain(module.size) + " " + module.id + "\n";
        require(actual == layout_text.str(), "layout mismatch");
        auto sites = [&]() {
            Nso main_nso(file(root + "/main"));
            return game_sites::for_module(main_nso, modules[0].base, false);
        }();
        write_layout(modules, ".", sites.version, sites.sites);
        Options opts;
        opts.hlepath = root + "/hle_symbols.txt";
        for (const auto &module : modules) {
            current_module = module.name;
            module_files = 0;
            module_bytes = 0;
            sampled_peak = 0;
            sample_heap();
            uint64_t start = now_ns();
            Options module_opts = opts;
            if (module.name == "main")
                module_opts.hlepath.clear();
            generate(module.name, fs::path(root) / module.name, module.base, fs::path(".") / module.name,
                     module_opts, &sites.sites);
            uint64_t elapsed = now_ns() - start;
            sample_heap();
            require(module_files != 0, "generator produced no files");
            say("[s9] module %s files %zu bytes %zu ms %.3f sampled_peak_heap %llu\n",
                current_module.c_str(), module_files, module_bytes, elapsed / 1e6, sampled_peak);
        }
        say("[s9] done\n");
        return finish(0);
    } catch (const std::exception &e) {
        say("[s9] failed: %s\n", e.what());
        return finish(1);
    }
}
