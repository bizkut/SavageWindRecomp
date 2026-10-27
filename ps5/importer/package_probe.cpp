// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/romfs_image.h"
#include <ps5platform/klog.h>
#include <tc/crypto/Sha2256Generator.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <dirent.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct Sample { const char *path; const char *hash; uint64_t size; };
#include "package_probe_samples.h"
#ifndef PACKAGE_PROBE_TEMP
#define PACKAGE_PROBE_TEMP "/data/savagewind/logs/package-probe.txt.tmp"
#define PACKAGE_PROBE_RESULT "/data/savagewind/logs/package-probe.txt"
#endif
using Clock = std::chrono::steady_clock;
static FILE *log_file;
static void report(const char *format, ...) {
    char message[1024], line[1100];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    std::snprintf(line, sizeof line, "savagewind-package-probe: %s\n", message);
    sceKernelDebugOutText(0, line);
    if (log_file) { std::fputs(line, log_file); std::fflush(log_file); }
}

// Exception messages and paths may contain key material; redact long hex runs.
static void report_safe(const char *step, int error, const char *detail) {
    char safe[512];
    size_t out = 0;
    const char *p = detail ? detail : "";
    while (*p && out + 1 < sizeof safe) {
        if (std::isxdigit(static_cast<unsigned char>(*p))) {
            const char *end = p;
            while (std::isxdigit(static_cast<unsigned char>(*end))) ++end;
            if (end - p >= 16) {
                const char *redacted = "[redacted]";
                while (*redacted && out + 1 < sizeof safe) safe[out++] = *redacted++;
                p = end;
                continue;
            }
        }
        unsigned char c = static_cast<unsigned char>(*p++);
        safe[out++] = c >= 32 && c < 127 ? char(c) : ' ';
    }
    safe[out] = 0;
    report("%s errno=%d detail=%s", step, error, safe);
}
static void probe_access(const char *path) {
    struct stat st = {};
    errno = 0;
    int stat_rc = stat(path, &st), stat_errno = errno;
    errno = 0;
    int fd = open(path, O_RDONLY), open_errno = errno;
    if (fd >= 0) close(fd);
    char detail[768];
    std::snprintf(detail, sizeof detail, "%s stat=%d stat_errno=%d mode=%o size=%lld open=%d",
                  path, stat_rc, stat_errno, stat_rc ? 0 : unsigned(st.st_mode),
                  stat_rc ? 0ll : (long long)st.st_size, fd);
    report_safe("access", open_errno, detail);
}
static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
static uint32_t u32(const unsigned char *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t u64(const unsigned char *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
static bool read_at(RomfsImage *image, void *p, size_t n, uint64_t off) {
    return romfs_image_pread(image, p, n, off) == ssize_t(n);
}
struct Tables {
    uint64_t h[10];
    std::vector<unsigned char> dh, dm, fh, fm;
    bool load(RomfsImage *image) {
        if (!read_at(image, h, sizeof h, 0) || h[0] != 0x50) return false;
        dh.resize(h[2]); dm.resize(h[4]); fh.resize(h[6]); fm.resize(h[8]);
        return read_at(image, dh.data(), dh.size(), h[1]) && read_at(image, dm.data(), dm.size(), h[3]) &&
               read_at(image, fh.data(), fh.size(), h[5]) && read_at(image, fm.data(), fm.size(), h[7]);
    }
    uint32_t lookup(bool dir, uint32_t parent, const std::string &name) const {
        const auto &hash = dir ? dh : fh;
        const auto &meta = dir ? dm : fm;
        uint32_t value = parent ^ 123456789u;
        for (unsigned char c : name) value = ((value >> 5) | (value << 27)) ^ c;
        if (hash.size() < 4) return UINT32_MAX;
        uint32_t e = u32(hash.data() + 4 * (value % (hash.size() / 4)));
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
    bool file(const char *path, uint64_t &offset, uint64_t &size) const {
        std::string full(path);
        uint32_t parent = 0;
        size_t start = 0;
        for (;;) {
            size_t slash = full.find('/', start);
            std::string name = full.substr(start, slash == std::string::npos ? slash : slash - start);
            uint32_t e = lookup(slash != std::string::npos, parent, name);
            if (e == UINT32_MAX) return false;
            if (slash == std::string::npos) {
                offset = h[9] + u64(fm.data() + e + 8);
                size = u64(fm.data() + e + 16);
                return true;
            }
            parent = e;
            start = slash + 1;
        }
    }
};
static std::string hex(const unsigned char *bytes, size_t n) {
    const char *digits = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < n; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
    return result;
}
static bool benchmark(RomfsImage *image, uint64_t offset, uint64_t size) {
    if (size <= 65536) { report("benchmark file is too small"); return false; }
    std::vector<unsigned char> data(1024 * 1024);
    auto start = Clock::now();
    for (uint64_t pos = 0; pos < size;) {
        size_t n = size_t(std::min<uint64_t>(data.size(), size - pos));
        if (!read_at(image, data.data(), n, offset + pos)) { report("sequential read failed"); return false; }
        pos += n;
    }
    double sec = elapsed(start);
    report("sequential bytes=%llu seconds=%.3f MiB_s=%.2f", (unsigned long long)size, sec, size / 1048576.0 / sec);
    for (int threads : {1, 4}) {
        std::vector<std::thread> workers;
        std::vector<int> failed(threads);
        std::vector<double> latency(threads);
        start = Clock::now();
        for (int i = 0; i < threads; ++i) workers.emplace_back([&, i] {
            std::vector<unsigned char> buffer(65536);
            uint64_t seed = 0x53575035ULL + i;
            for (int j = 0; j < 2000; ++j) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                uint64_t off = offset + seed % (size - buffer.size() + 1);
                auto begin = Clock::now();
                if (!read_at(image, buffer.data(), buffer.size(), off)) { failed[i] = 1; return; }
                latency[i] += elapsed(begin);
            }
        });
        for (auto &worker : workers) worker.join();
        if (std::find(failed.begin(), failed.end(), 1) != failed.end()) { report("random read failed threads=%d", threads); return false; }
        sec = elapsed(start);
        double total_latency = 0;
        for (double n : latency) total_latency += n;
        double reads = 2000.0 * threads;
        report("random threads=%d reads=%u mean_ms=%.3f MiB_s=%.2f", threads, threads * 2000,
               total_latency * 1000.0 / reads, reads * 65536.0 / 1048576.0 / sec);
    }
    return true;
}
int main() {
    mkdir("/data/savagewind/logs", 0777);
    log_file = std::fopen(PACKAGE_PROBE_TEMP, "w");
    unsigned passed = 0, failed = 0;
    if (!log_file) {
        report("unable to create result file");
        report("done: pass=0 fail=1");
        return 1;
    }
    const char *step = "import directory";
    probe_access("/data/savagewind/import");
    probe_access("/data/savagewind/import/prod.keys");
    try {
        DIR *dir = opendir("/data/savagewind/import");
        if (!dir) throw std::runtime_error("cannot open import directory");
        std::vector<std::string> names;
        while (auto *entry = readdir(dir)) {
            const char *ext = std::strrchr(entry->d_name, '.');
            if (ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci")))
                names.emplace_back(std::string("/data/savagewind/import/") + entry->d_name);
        }
        closedir(dir);
        std::sort(names.begin(), names.end());
        for (const auto &name : names) probe_access(name.c_str());
        report("package scan count=%zu", names.size());
        std::vector<const char *> paths;
        for (auto &name : names) paths.push_back(name.c_str());
        char why[256] = {};
        step = "package keys/CNMT/NCA/RomFS open";
        report("opening package keys/CNMT/NCA/RomFS");
        auto start = Clock::now();
        RomfsImage *image = romfs_image_open_package("/data/savagewind/import/prod.keys", paths.data(), paths.size(), 0, why, sizeof why);
        double open_time = elapsed(start);
        if (!image) throw std::runtime_error(why[0] ? why : "package open failed");
        report("open seconds=%.3f image_bytes=%llu", open_time, (unsigned long long)romfs_image_size(image));
        step = "RomFS tables";
        Tables tables;
        if (!tables.load(image)) { romfs_image_close(image); throw std::runtime_error("RomFS tables failed"); }
        uint64_t largest_offset = 0, largest_size = 0;
        std::vector<unsigned char> buffer(1024 * 1024);
        for (size_t i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
            uint64_t offset = 0, size = 0;
            bool ok = tables.file(samples[i].path, offset, size) && size == samples[i].size;
            if (i == 0 && ok) { largest_offset = offset; largest_size = size; }
            tc::crypto::Sha2256Generator hash;
            hash.initialize();
            for (uint64_t pos = 0; ok && pos < size;) {
                size_t n = size_t(std::min<uint64_t>(buffer.size(), size - pos));
                ok = read_at(image, buffer.data(), n, offset + pos);
                if (ok) hash.update(buffer.data(), n);
                pos += n;
            }
            std::array<unsigned char, 32> digest{};
            hash.getHash(digest.data());
            ok = ok && hex(digest.data(), digest.size()) == samples[i].hash;
            if (ok) ++passed;
            else { ++failed; report("sample failed: %s", samples[i].path); }
        }
        report("samples passed=%u failed=%u", passed, failed);
        if (largest_size && benchmark(image, largest_offset, largest_size)) ++passed;
        else ++failed;
        romfs_image_close(image);
    } catch (const std::exception &e) {
        ++failed;
        report_safe(step, errno, e.what());
    } catch (...) {
        ++failed;
        report_safe(step, errno, "non-standard exception");
    }
    report("done: pass=%u fail=%u", passed, failed);
    bool ok = std::fclose(log_file) == 0;
    log_file = nullptr;
    if (!ok || (std::strcmp(PACKAGE_PROBE_TEMP, PACKAGE_PROBE_RESULT) &&
                std::rename(PACKAGE_PROBE_TEMP, PACKAGE_PROBE_RESULT) != 0)) {
        report("unable to publish result file");
        report("done: pass=%u fail=%u", passed, failed + 1);
        return 1;
    }
    return failed ? 1 : 0;
}
