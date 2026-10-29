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

struct FileResult { std::string name; size_t bytes; std::array<uint8_t, 32> sha; };
static std::vector<FileResult> results;
static size_t total_bytes;
void s9_output_sink(const std::string &filename, const std::string &text) {
    auto sha = ps5::crypto::sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    results.push_back({filename.substr(filename.find_last_of("/\\") + 1), text.size(), sha});
    total_bytes += text.size();
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
    (void)argc; (void)argv;
    const std::string root = "/data/savagewind/s9";
#else
    const std::string root = argc > 1 ? argv[1] : "build/ps5/s9-input";
#endif
    std::string nso = root + "/subsdk0", layout = root + "/layout.txt", hle = root + "/hle_symbols.txt";
    for (const auto &p : {nso, layout, hle}) {
        struct stat st{};
        if (stat(p.c_str(), &st) || !S_ISREG(st.st_mode)) {
            say("[s9] failed input %s\n", p.c_str());
            return finish(1);
        }
    }
    std::vector<std::string> args = {"gen", "subsdk0", nso, layout, ".", "--hle", hle, "--simd", "c"};
    std::vector<char *> raw;
    for (auto &arg : args) raw.push_back(arg.data());
    uint64_t start = now_ns();
    int rc = s9_generator_main(int(raw.size()), raw.data());
    uint64_t elapsed = now_ns() - start;
    if (rc || results.empty()) {
        say("[s9] failed generator rc=%d files=%zu\n", rc, results.size());
        return finish(1);
    }
#ifdef __PROSPERO__
    struct ps5_heap_stats heap{};
    ps5_heap_stats(&heap);
    unsigned long long peak = heap.peak_bytes;
#else
    // PS5 heap allocator statistics are not available on the host.
    unsigned long long peak = 0;
#endif
    say("[s9] module subsdk0 files %zu bytes %zu ms %.3f peak_heap %llu\n",
        results.size(), total_bytes, elapsed / 1e6, peak);
    for (const auto &f : results) {
        std::string digest = hexbytes(f.sha.data(), f.sha.size());
        say("[s9] file %s bytes %zu sha256 %s\n", f.name.c_str(), f.bytes, digest.c_str());
    }
    say("[s9] done\n");
    return finish(0);
}
