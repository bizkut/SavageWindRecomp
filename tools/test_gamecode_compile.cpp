// SPDX-License-Identifier: GPL-3.0-or-later
#include "../runtime/gamecode_compile.h"
#include <elf.h>
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Cache {
    fs::path directory;
    size_t hits = 0;
    size_t misses = 0;
    size_t stores = 0;
    Clock::time_point start = Clock::now();
    Clock::time_point gen_start;
    Clock::time_point gen_end;
};
static std::vector<char> read_file(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    auto n = in.tellg();
    if (n < 0)
        throw std::runtime_error("cannot size " + path.string());
    std::vector<char> data(static_cast<size_t>(n));
    in.seekg(0);
    if (!in.read(data.data(), data.size()))
        throw std::runtime_error("cannot read " + path.string());
    return data;
}
static fs::path key_path(Cache &cache, const uint8_t key[32])
{
    static const char hex[] = "0123456789abcdef";
    std::string name;
    for (int i = 0; i < 32; ++i) {
        name += hex[key[i] >> 4];
        name += hex[key[i] & 15];
    }
    return cache.directory / name;
}
static int lookup(void *context, const uint8_t key[32], const void **data, size_t *size)
{
    Cache &cache = *static_cast<Cache *>(context);
    fs::path path = key_path(cache, key);
    if (!fs::exists(path)) {
        ++cache.misses;
        return 0;
    }
    try {
        auto bytes = read_file(path);
        void *copy = std::malloc(bytes.size());
        if (!copy)
            return -1;
        std::memcpy(copy, bytes.data(), bytes.size());
        *data = copy;
        *size = bytes.size();
        ++cache.hits;
        return 1;
    } catch (...) {
        return -1;
    }
}
static void release(void *, const void *data, size_t)
{
    std::free(const_cast<void *>(data));
}
static int store(void *context, const uint8_t key[32], const void *data, size_t size)
{
    Cache &cache = *static_cast<Cache *>(context);
    std::ofstream out(key_path(cache, key), std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(data), size);
    out.close();
    if (!out)
        return -1;
    ++cache.stores;
    return 0;
}
static void progress(void *context, const char *stage, size_t done, size_t)
{
    Cache &cache = *static_cast<Cache *>(context);
    if (!std::strcmp(stage, "generate") && done == 0)
        cache.gen_start = Clock::now();
    if (!std::strcmp(stage, "generate") && done == 3)
        cache.gen_end = Clock::now();
}
static double seconds(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}
static bool equal_except_comment(const void *left, size_t left_size, const void *right, size_t right_size)
{
    const auto *a = static_cast<const unsigned char *>(left);
    const auto *b = static_cast<const unsigned char *>(right);
    if (left_size < sizeof(Elf64_Ehdr) || right_size < sizeof(Elf64_Ehdr))
        return false;
    Elf64_Ehdr x{}, y{};
    std::memcpy(&x, a, sizeof x);
    std::memcpy(&y, b, sizeof y);
    if (std::memcmp(x.e_ident, y.e_ident, EI_NIDENT) || std::memcmp(x.e_ident, ELFMAG, SELFMAG) ||
        x.e_ident[EI_CLASS] != ELFCLASS64 || x.e_ident[EI_DATA] != ELFDATA2LSB ||
        x.e_type != y.e_type || x.e_machine != y.e_machine || x.e_version != y.e_version ||
        x.e_flags != y.e_flags || x.e_shentsize != sizeof(Elf64_Shdr) || y.e_shentsize != sizeof(Elf64_Shdr) ||
        x.e_shnum != y.e_shnum || x.e_shstrndx != y.e_shstrndx || x.e_shstrndx >= x.e_shnum ||
        x.e_shoff > left_size || x.e_shnum > (left_size - x.e_shoff) / sizeof(Elf64_Shdr) ||
        y.e_shoff > right_size || y.e_shnum > (right_size - y.e_shoff) / sizeof(Elf64_Shdr))
        return false;
    auto section = [](const unsigned char *data, const Elf64_Ehdr &header, size_t index) {
        Elf64_Shdr result{};
        std::memcpy(&result, data + header.e_shoff + index * sizeof result, sizeof result);
        return result;
    };
    Elf64_Shdr names = section(a, x, x.e_shstrndx);
    if (names.sh_offset > left_size || names.sh_size > left_size - names.sh_offset)
        return false;
    for (size_t i = 0; i < x.e_shnum; ++i) {
        Elf64_Shdr p = section(a, x, i), q = section(b, y, i);
        if (p.sh_name != q.sh_name || p.sh_type != q.sh_type || p.sh_flags != q.sh_flags ||
            p.sh_addr != q.sh_addr || p.sh_link != q.sh_link || p.sh_info != q.sh_info ||
            p.sh_addralign != q.sh_addralign || p.sh_entsize != q.sh_entsize ||
            p.sh_name >= names.sh_size)
            return false;
        const char *name = reinterpret_cast<const char *>(a + names.sh_offset + p.sh_name);
        if (!std::memchr(name, 0, names.sh_size - p.sh_name))
            return false;
        if (!std::strcmp(name, ".comment"))
            continue;
        if (p.sh_size != q.sh_size)
            return false;
        if (p.sh_type == SHT_NOBITS)
            continue;
        if (p.sh_offset > left_size || p.sh_size > left_size - p.sh_offset ||
            q.sh_offset > right_size || q.sh_size > right_size - q.sh_offset ||
            std::memcmp(a + p.sh_offset, b + q.sh_offset, p.sh_size))
            return false;
    }
    return true;
}
int main(int argc, char **argv) try {
    if (argc != 6)
        throw std::runtime_error("usage: test INPUT INCLUDE_ROOT CACHE REFERENCE_DIR ARCHIVE_LIST");
    fs::path input = argv[1];
    auto main_nso = read_file(input / "main");
    auto sub = read_file(input / "subsdk0");
    auto sdk = read_file(input / "sdk");
    auto hle = read_file("runtime/hle/hle_symbols.txt");
    auto layout = read_file("build/s9-cpp/layout.txt");
    GameCodeCompileInputs inputs{};
    inputs.modules[0] = {main_nso.data(), main_nso.size()};
    inputs.modules[1] = {sub.data(), sub.size()};
    inputs.modules[2] = {sdk.data(), sdk.size()};
    inputs.hle = {hle.data(), hle.size()};
    inputs.layout = {layout.data(), layout.size()};
    Cache cache;
    cache.directory = argv[3];
    fs::create_directories(cache.directory);
    GameCodeObjectCache callbacks{&cache, lookup, release, store};
    GameCodeObjects result{};
    char error[4096];
    cache.start = Clock::now();
    if (gamecode_build_objects(&inputs, argv[2], 8, &callbacks, progress, &cache, &result, error, sizeof error))
        throw std::runtime_error(error);
    auto end = Clock::now();
    std::ifstream listing(argv[5]);
    if (!listing)
        throw std::runtime_error("cannot open archive list");
    std::string name;
    size_t index = 0, different = 0, comment_only = 0;
    while (std::getline(listing, name)) {
        if (index >= result.count || name != result.objects[index].name)
            throw std::runtime_error("archive order differs at index " + std::to_string(index));
        auto expected = read_file(fs::path(argv[4]) / name);
        const auto &actual = result.objects[index];
        if (expected.size() != actual.size || std::memcmp(expected.data(), actual.data, actual.size)) {
            bool only_comment = equal_except_comment(expected.data(), expected.size(), actual.data, actual.size);
            std::cout << "DIFFER " << name << (only_comment ? " comment-only" : " other-sections") << "\n";
            if (only_comment)
                ++comment_only;
            ++different;
        }
        ++index;
    }
    if (index != result.count || result.count != 996)
        throw std::runtime_error("unexpected object count " + std::to_string(result.count));
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "RESULT objects=" << result.count << " differing=" << different << " comment_only=" << comment_only
              << " hits=" << cache.hits << " misses=" << cache.misses << " stores=" << cache.stores
              << " generation_seconds=" << seconds(cache.gen_end - cache.gen_start)
              << " total_seconds=" << seconds(end - cache.start) << " peak_rss_kib=" << usage.ru_maxrss << "\n";
    gamecode_free_objects(&result);
    if (cache.hits + cache.misses != 996 || cache.stores != cache.misses)
        throw std::runtime_error("cache callback counts differ");
    if (different != comment_only)
        throw std::runtime_error("object sections differ beyond compiler comment");
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
}
