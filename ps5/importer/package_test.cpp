// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/romfs_image.h"
#include "package_exefs.h"
#include <tc/crypto/Sha2256Generator.h>
#include <tc/crypto/Aes128CtrEncryptor.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
extern "C" void savagewind_package_set_coalesce(int);
static void test_coalesced_reads(RomfsImage *image) {
    uint64_t size = romfs_image_size(image);
    for (uint64_t offset : {uint64_t(0), uint64_t(1), uint64_t(16383), uint64_t(16384),
                            size > 32768 ? size - 32768 : uint64_t(0), size ? size - 1 : uint64_t(0), size}) {
        for (size_t count : {size_t(1), size_t(16384), size_t(65537), size_t(524288)}) {
            std::vector<unsigned char> direct(count), merged(count);
            savagewind_package_set_coalesce(0);
            ssize_t expected = romfs_image_pread(image, direct.data(), count, offset);
            savagewind_package_set_coalesce(1);
            ssize_t actual = romfs_image_pread(image, merged.data(), count, offset);
            if (actual != expected || (actual > 0 && std::memcmp(direct.data(), merged.data(), size_t(actual))))
                throw std::runtime_error("coalesced package read differs");
        }
    }
    savagewind_package_set_coalesce(0);
}
extern "C" int savage_sha256_ni(unsigned char *, const unsigned char *, size_t);
static void test_sha256_ni() {
    std::vector<unsigned char> input(16400);
    for (size_t i = 0; i < input.size(); ++i) input[i] = (unsigned char)(i * 73 + 11);
    for (size_t n : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64),
                     size_t(65), size_t(4096), size_t(16384)}) {
        unsigned char expected[32], actual[32];
        tc::crypto::Sha2256Generator h;
        h.initialize(); h.update(input.data() + 1, n); h.getHash(expected);
        int accelerated = savage_sha256_ni(actual, input.data() + 1, n);
        if (!accelerated) throw std::runtime_error("SHA-NI unavailable");
        if (std::memcmp(expected, actual, 32))
            throw std::runtime_error("SHA-NI digest mismatch");
    }
}
static uint32_t u32(const unsigned char *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t u64(const unsigned char *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
static std::string hex(const unsigned char *p, size_t n) {
    const char *digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}
static bool read_at(RomfsImage *image, void *p, size_t n, uint64_t off) {
    return romfs_image_pread(image, p, n, off) == ssize_t(n);
}
struct Tables {
    uint64_t h[10];
    std::vector<unsigned char> dh, dm, fh, fm;
    bool load(RomfsImage *image) {
        if (!read_at(image, h, sizeof(h), 0) || h[0] != 0x50) return false;
        dh.resize(h[2]); dm.resize(h[4]); fh.resize(h[6]); fm.resize(h[8]);
        return read_at(image, dh.data(), dh.size(), h[1]) && read_at(image, dm.data(), dm.size(), h[3]) &&
               read_at(image, fh.data(), fh.size(), h[5]) && read_at(image, fm.data(), fm.size(), h[7]);
    }
    uint32_t lookup(bool dir, uint32_t parent, const std::string &name) const {
        const auto &hash = dir ? dh : fh;
        const auto &meta = dir ? dm : fm;
        uint32_t hval = parent ^ 123456789u;
        for (unsigned char c : name) hval = ((hval >> 5) | (hval << 27)) ^ c;
        if (hash.size() < 4) return UINT32_MAX;
        uint32_t e = u32(hash.data() + 4 * (hval % (hash.size() / 4)));
        size_t header = dir ? 0x18 : 0x20, next = dir ? 0x10 : 0x18;
        for (size_t visited = 0; e != UINT32_MAX && visited < meta.size() / header; ++visited) {
            if (uint64_t(e) + header > meta.size()) return UINT32_MAX;
            const auto *p = meta.data() + e;
            uint32_t n = u32(p + header - 4);
            if (uint64_t(e) + header + n > meta.size()) return UINT32_MAX;
            if (u32(p) == parent && n == name.size() && !std::memcmp(p + header, name.data(), n)) return e;
            e = u32(p + next);
        }
        return UINT32_MAX;
    }
    bool file(const std::string &path, uint64_t &offset, uint64_t &size) const {
        uint32_t parent = 0;
        size_t start = 0;
        for (;;) {
            size_t slash = path.find('/', start);
            auto name = path.substr(start, slash == std::string::npos ? slash : slash - start);
            bool dir = slash != std::string::npos;
            uint32_t e = lookup(dir, parent, name);
            if (e == UINT32_MAX) return false;
            if (!dir) { offset = h[9] + u64(fm.data() + e + 8); size = u64(fm.data() + e + 16); return true; }
            parent = e; start = slash + 1;
        }
    }
    size_t file_count() const {
        size_t count = 0;
        for (size_t off = 0; off < fm.size(); ++count) {
            if (fm.size() - off < 0x20) throw std::runtime_error("truncated file table");
            uint64_t name_size = u32(fm.data() + off + 0x1c);
            uint64_t next = uint64_t(off) + 0x20 + ((name_size + 3) & ~uint64_t(3));
            if (next > fm.size()) throw std::runtime_error("invalid file table");
            off = size_t(next);
        }
        return count;
    }
};
static double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
static void bench(const char *label, RomfsImage *image, double open_time) {
    const uint64_t size = romfs_image_size(image), block = 65536;
    std::vector<unsigned char> buf(1024 * 1024);
    auto start = Clock::now();
    uint64_t n = std::min<uint64_t>(size, 256 * 1024 * 1024);
    for (uint64_t off = 0; off < n; off += buf.size()) {
        size_t take = size_t(std::min<uint64_t>(buf.size(), n - off));
        if (!read_at(image, buf.data(), take, off)) throw std::runtime_error("sequential read failed");
    }
    double seq = seconds(start);
    std::printf("%s open=%.3fs sequential=%.2f MiB/s", label, open_time, n / 1048576.0 / seq);
    for (int threads : {1, 4}) {
        start = Clock::now();
        std::vector<std::thread> workers;
        std::vector<int> failures(threads);
        std::vector<double> latency(threads);
        for (int i = 0; i < threads; ++i) workers.emplace_back([&, i] {
            std::vector<unsigned char> b(block);
            uint64_t seed = 0x123456789abcdefULL + i;
            for (int j = 0; j < 512; ++j) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                uint64_t off = seed % (size - block);
                auto before = Clock::now();
                if (!read_at(image, b.data(), b.size(), off)) { failures[i] = 1; break; }
                latency[i] += seconds(before);
            }
        });
        for (auto &thread : workers) thread.join();
        if (std::find(failures.begin(), failures.end(), 1) != failures.end()) throw std::runtime_error("random read failed");
        double elapsed = seconds(start), calls = threads * 512.0;
        double latency_sum = 0;
        for (double value : latency) latency_sum += value;
        std::printf(" random%d=%.3fms %.2fMiB/s", threads, latency_sum * 1000.0 / calls,
                    calls * block / 1048576.0 / elapsed);
    }
    std::puts("");
}

static void bench_sizes(RomfsImage *image) {
    const uint64_t size = romfs_image_size(image);
    for (size_t bytes : {size_t(4096), size_t(16384), size_t(65536), size_t(524288), size_t(1048576)}) {
        std::vector<unsigned char> buf(bytes);
        double total = 0, max = 0;
        uint64_t seed = 0x123456789abcdefULL;
        for (int i = 0; i < 512; ++i) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            uint64_t off = seed % (size - bytes);
            auto start = Clock::now();
            if (!read_at(image, buf.data(), bytes, off)) throw std::runtime_error("size benchmark read failed");
            double elapsed = seconds(start);
            total += elapsed;
            max = std::max(max, elapsed);
        }
        std::printf("size=%zu random mean=%.3fms max=%.3fms\n", bytes, total * 1000 / 512, max * 1000);
    }
    std::vector<unsigned char> block(1048576);
    unsigned char key[16] = {}, iv[16] = {}, digest[32];
    auto start = Clock::now();
    for (int i = 0; i < 256; ++i)
        tc::crypto::DecryptAes128Ctr(block.data(), block.data(), block.size(), 0, key, sizeof key, iv, sizeof iv);
    double aes = seconds(start);
    start = Clock::now();
    for (int i = 0; i < 256; ++i) {
        tc::crypto::Sha2256Generator hash;
        for (size_t off = 0; off < block.size(); off += 16384) {
            hash.initialize();
            hash.update(block.data() + off, 16384);
            hash.getHash(digest);
        }
    }
    std::printf("crypto 1MiB AES-CTR=%.3fms SHA256-16KiB-blocks=%.3fms\n", aes * 1000 / 256,
                seconds(start) * 1000 / 256);
}
int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--sha-test")) {
        try { test_sha256_ni(); std::puts("SHA-NI vectors matched"); return 0; }
        catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    }
    if (argc < 6) { std::fprintf(stderr, "usage: package-test <keys> <manifest> <folder> <package>...\n"); return 2; }
    if (!std::strcmp(argv[2], "--bench-only")) {
        char why[256] = {};
        RomfsImage *image = romfs_image_open_package(argv[1], const_cast<const char *const *>(argv + 4),
                                                      size_t(argc - 4), 0, why, sizeof why);
        if (!image) { std::fprintf(stderr, "package-test: %s\n", why); return 1; }
        try { bench_sizes(image); }
        catch (const std::exception &e) { std::fprintf(stderr, "package-test: %s\n", e.what()); romfs_image_close(image); return 1; }
        romfs_image_close(image);
        return 0;
    }
    try {
        char invalid_why[256] = {};
        if (romfs_image_open_package(nullptr, nullptr, 0, 0, invalid_why, sizeof invalid_why) ||
            !std::strstr(invalid_why, "keys_missing"))
            throw std::runtime_error("invalid package input did not return an error");
        invalid_why[0] = 0;
        if (savagewind_exefs_open(nullptr, nullptr, 0, invalid_why, sizeof invalid_why) ||
            !std::strstr(invalid_why, "keys_missing"))
            throw std::runtime_error("invalid ExeFS input did not return an error");
        char exefs_why[256] = {};
        void *exefs = savagewind_exefs_open(argv[1], const_cast<const char *const *>(argv + 4),
                                           size_t(argc - 4), exefs_why, sizeof exefs_why);
        if (!exefs) throw std::runtime_error(exefs_why);
        void *main_file = savagewind_exefs_file_open(exefs, "main");
        unsigned char nso[0x60], second[0x10];
        if (!main_file || savagewind_exefs_file_size(main_file) < 0x100 ||
            savagewind_exefs_file_pread(main_file, nso, sizeof nso, 0) != sizeof nso ||
            std::memcmp(nso, "NSO0", 4) || hex(nso + 0x40, 8) != "cd57b23fa4bbad65" ||
            savagewind_exefs_file_pread(main_file, second, sizeof second, 0x40) != sizeof second ||
            std::memcmp(second, nso + 0x40, sizeof second))
            throw std::runtime_error("package ExeFS main read failed");
        savagewind_exefs_file_close(main_file);
        void *npdm = savagewind_exefs_file_open(exefs, "main.npdm");
        unsigned char meta[4];
        if (!npdm || savagewind_exefs_file_pread(npdm, meta, sizeof meta, 0) != sizeof meta ||
            std::memcmp(meta, "META", 4)) throw std::runtime_error("package ExeFS NPDM read failed");
        savagewind_exefs_file_close(npdm);
        if (savagewind_exefs_file_open(exefs, "../main") ||
            savagewind_exefs_file_open(exefs, "nonexistent"))
            throw std::runtime_error("package ExeFS accepted invalid or missing file");
        savagewind_exefs_close(exefs);
        char why[256] = {};
        auto start = Clock::now();
        RomfsImage *image = romfs_image_open_package(argv[1], const_cast<const char *const *>(argv + 4), size_t(argc - 4), 0, why, sizeof why);
        double open_time = seconds(start);
        if (!image) throw std::runtime_error(why);
        test_coalesced_reads(image);
        Tables tables;
        if (!tables.load(image)) throw std::runtime_error("invalid RomFS tables");
        std::ifstream manifest(argv[2]);
        if (!manifest) throw std::runtime_error("cannot open manifest");
        std::vector<std::string> lines;
        for (std::string line; std::getline(manifest, line);)
            if (line.size() >= 74 && line.compare(66, 6, "romfs/") == 0) lines.push_back(line);
        // Four threads read concurrently, so the check covers every reader of the package stream.
        const size_t check_threads = 4, total = lines.size();
        std::atomic<size_t> matched{0};
        std::mutex error_mutex;
        std::string error;
        std::vector<std::thread> checkers;
        for (size_t t = 0; t < check_threads; ++t) checkers.emplace_back([&, t] {
            std::vector<unsigned char> buf(1024 * 1024);
            auto failed = [&](const std::string &what) {
                std::lock_guard<std::mutex> guard(error_mutex);
                if (error.empty()) error = what;
            };
            for (size_t i = t; i < total; i += check_threads) {
                const std::string &line = lines[i];
                uint64_t offset, size;
                std::string name = line.substr(72);
                if (!tables.file(name, offset, size)) return failed("missing " + name);
                tc::crypto::Sha2256Generator hash;
                hash.initialize();
                for (uint64_t pos = 0; pos < size;) {
                    size_t n = size_t(std::min<uint64_t>(buf.size(), size - pos));
                    if (!read_at(image, buf.data(), n, offset + pos)) return failed("read failed " + name);
                    hash.update(buf.data(), n);
                    pos += n;
                }
                std::array<unsigned char, 32> digest{};
                hash.getHash(digest.data());
                if (hex(digest.data(), digest.size()) != line.substr(0, 64)) return failed("hash mismatch " + name);
                ++matched;
            }
        });
        for (auto &thread : checkers) thread.join();
        if (!error.empty()) throw std::runtime_error(error);
        std::printf("manifest: %zu/%zu matched (%zu threads)\n", matched.load(), total, check_threads);
        if (total != 41062 || matched != total || tables.file_count() != total)
            throw std::runtime_error("unexpected RomFS or manifest count");
        bench("package", image, open_time);
        start = Clock::now();
        RomfsImage *folder = romfs_image_open(argv[3], why, sizeof why);
        double folder_time = seconds(start);
        if (!folder) throw std::runtime_error(why);
        bench("folder", folder, folder_time);
        romfs_image_close(folder);
        romfs_image_close(image);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "package-test: %s\n", e.what()); return 1; }
}
