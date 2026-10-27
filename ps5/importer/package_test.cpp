// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/romfs_image.h"
#include <tc/crypto/Sha2256Generator.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
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
int main(int argc, char **argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: package-test <keys> <manifest> <folder> <package>...\n"); return 2; }
    try {
        char why[256] = {};
        auto start = Clock::now();
        RomfsImage *image = romfs_image_open_package(argv[1], const_cast<const char *const *>(argv + 4), size_t(argc - 4), 0, why, sizeof why);
        double open_time = seconds(start);
        if (!image) throw std::runtime_error(why);
        Tables tables;
        if (!tables.load(image)) throw std::runtime_error("invalid RomFS tables");
        std::ifstream manifest(argv[2]);
        if (!manifest) throw std::runtime_error("cannot open manifest");
        std::vector<unsigned char> buf(1024 * 1024);
        std::string line;
        size_t matched = 0, total = 0;
        while (std::getline(manifest, line)) {
            if (line.size() < 74 || line.compare(66, 6, "romfs/") != 0) continue;
            ++total;
            uint64_t offset, size;
            std::string name = line.substr(72);
            if (!tables.file(name, offset, size)) throw std::runtime_error("missing " + name);
            tc::crypto::Sha2256Generator hash;
            hash.initialize();
            for (uint64_t pos = 0; pos < size;) {
                size_t n = size_t(std::min<uint64_t>(buf.size(), size - pos));
                if (!read_at(image, buf.data(), n, offset + pos)) throw std::runtime_error("read failed " + name);
                hash.update(buf.data(), n);
                pos += n;
            }
            std::array<unsigned char, 32> digest{};
            hash.getHash(digest.data());
            if (hex(digest.data(), digest.size()) != line.substr(0, 64)) throw std::runtime_error("hash mismatch " + name);
            ++matched;
        }
        std::printf("manifest: %zu/%zu matched\n", matched, total);
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
