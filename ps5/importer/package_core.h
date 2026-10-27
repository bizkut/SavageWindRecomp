// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GameCardProcess.h"
#include "KeyBag.h"
#include "NcaProcess.h"
#include "PfsProcess.h"
#include <pietendo/hac/ContentMeta.h>
#include <tc/crypto/Sha2256Generator.h>
#include <tc/io.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef SAVAGEWIND_PS5_PAYLOAD
#include <ps5platform/klog.h>
#endif

namespace fs = std::filesystem;
using Stream = std::shared_ptr<tc::io::IStream>;
using Vfs = std::shared_ptr<tc::io::IFileSystem>;
using namespace std::chrono_literals;
static constexpr uint64_t base_id = 0x01007ef00011e000ULL, update_id = 0x01007ef00011e800ULL;
static constexpr size_t chunk_size = 8 * 1024 * 1024;
// Exception messages can contain paths or key-file values. Redact hex strings
// long enough to be key material before sending diagnostics to klog.
static void diagnostic(const char *session, const char *step, int error, const char *kind,
                       const char *detail) noexcept {
    char safe[256];
    size_t out = 0;
    const char *p = detail ? detail : "";
    while (*p && out + 1 < sizeof(safe)) {
        if (std::isxdigit(static_cast<unsigned char>(*p))) {
            const char *end = p;
            while (std::isxdigit(static_cast<unsigned char>(*end)))
                ++end;
            if (end - p >= 16) {
                const char *redacted = "[redacted]";
                while (*redacted && out + 1 < sizeof(safe))
                    safe[out++] = *redacted++;
                p = end;
                continue;
            }
        }
        unsigned char c = static_cast<unsigned char>(*p++);
        safe[out++] = c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
    }
    safe[out] = '\0';
    char line[512];
    std::snprintf(
        line, sizeof(line), "savagewind-import: session=%s step=%s errno=%d type=%s what=%s\n",
        session && *session ? session : "-", step ? step : "-", error, kind ? kind : "-", safe);
#ifdef SAVAGEWIND_PS5_PAYLOAD
    sceKernelDebugOutText(0, line);
#else
    std::fputs(line, stderr);
#endif
}
struct Failure {
    std::string code, message, file;
    int error;
};
[[noreturn]] static void fail(const char *code, const char *message, const std::string &file = {}) {
    int saved_errno = errno;
    throw Failure{code, message, file, saved_errno};
}
static std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower((unsigned char)c));
    return s;
}
static bool ends(const std::string &s, const char *tail) {
    size_t n = strlen(tail);
    return s.size() >= n && lower(s.substr(s.size() - n)) == tail;
}
static bool safe_component(const std::string &s) {
    return !s.empty() && s != "." && s != ".." && s.find_first_of("/\\\r\n") == s.npos;
}
static std::string text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void atomic_text(const fs::path &p, const std::string &s) {
    fs::path tmp = p;
    tmp += ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        fail("io_error", "Unable to write import status.");
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n <= 0) {
            int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            fail("io_error", "Unable to write import status.");
        }
        off += size_t(n);
    }
    if (::close(fd) || ::rename(tmp.c_str(), p.c_str()))
        fail("io_error", "Unable to publish import status.");
}
static std::string field(const std::string &s, const std::string &key) {
    std::string prefix = key + "=";
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find('\n', pos);
        if (end == s.npos)
            break;
        if (s.compare(pos, prefix.size(), prefix) == 0)
            return s.substr(pos + prefix.size(), end - pos - prefix.size());
        pos = end + 1;
    }
    return {};
}
static bool session_valid(const std::string &s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
struct Status {
    fs::path dir;
    std::string session, stage, current, base, update, dlc, error, error_file, message;
    uint64_t seq = 0, done = 0, total = 0, files = 0, file_total = 0;
    std::chrono::steady_clock::time_point last{};
    void publish(bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last < 250ms)
            return;
        last = now;
        auto clean = [](std::string s) {
            for (char &c : s)
                if (c == '\n' || c == '\r' || (unsigned char)c < 32)
                    c = ' ';
            return s;
        };
        std::string out = "version=1\nsession=" + session + "\nseq=" + std::to_string(++seq) +
                          "\nstage=" + stage + "\nbytes_done=" + std::to_string(done) +
                          "\nbytes_total=" + std::to_string(total) +
                          "\nfiles_done=" + std::to_string(files) +
                          "\nfiles_total=" + std::to_string(file_total) + "\n";
        auto add = [&](const char *k, const std::string &v) {
            if (!v.empty())
                out += std::string(k) + "=" + clean(v) + "\n";
        };
        add("current", current);
        add("base_source", base);
        add("update_source", update);
        if (!base.empty() && !update.empty())
            add("game_version", "1.9.0");
        add("dlc", dlc);
        add("error", error);
        add("error_file", error_file);
        add("message", message);
        if (out.size() > 4096)
            fail("internal", "Import status is too large.");
        atomic_text(dir / "status", out);
    }
    void set(const char *s) {
        stage = s;
        current.clear();
        diagnostic(session.c_str(), s, 0, "stage", "");
        publish(true);
    }
    bool cancelled() const {
        return session_valid(session) && field(text(dir / "cancel"), "session") == session;
    }
    void check_cancel() const {
        if (cancelled())
            fail("cancelled", "Import cancelled.");
    }
};
static Stream vopen(const Vfs &v, const std::string &p) {
    Stream s;
    v->openFile(tc::io::Path(p), tc::io::FileMode::Open, tc::io::FileAccess::Read, s);
    return s;
}
static tc::io::sDirectoryListing listing(const Vfs &v, const std::string &p) {
    tc::io::sDirectoryListing d;
    v->getDirectoryListing(tc::io::Path(p), d);
    return d;
}
struct Package {
    std::string name, prefix;
    Vfs vfs;
};
static std::string hex(const byte_t *p, size_t n) {
    static const char x[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += x[p[i] >> 4];
        s += x[p[i] & 15];
    }
    return s;
}
struct Selection {
    Package *pkg = nullptr;
    std::string name;
    int64_t size = 0;
    std::array<byte_t, 32> hash{};
};
struct Candidate {
    Package *pkg;
    pie::hac::ContentMeta meta;
    std::string id;
};

struct PackageCore {
    fs::path dir;
    Status st;
    std::vector<Package> packages;
    std::vector<Candidate> bases, updates;
    std::array<std::vector<Candidate>, 8> aoc_candidates;
    struct Addon {
        Selection content;
        Vfs vfs;
    };
    std::array<Addon, 8> addons;
    Selection base, update;
    std::shared_ptr<nstool::KeyBagInitializer> keys;
    Vfs merged;
    explicit PackageCore(fs::path path) : dir(std::move(path)) { st.dir = dir; }
    void open_package(const fs::path &path);
    void scan_packages();
    void load_keys(const fs::path &prod, const fs::path &title);
    void load_keys();
    void find_meta();
    void open_inputs(const char *keys_path, const char *const *paths, size_t count);
};
