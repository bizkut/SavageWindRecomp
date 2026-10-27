#include "package_core.h"
struct Importer : PackageCore {
    fs::path root, work, staging;
    int lock = -1;
    explicit Importer(fs::path r) : PackageCore(r / "import"), root(std::move(r)), work(dir / "work") {}
    ~Importer() {
        if (lock >= 0)
            ::close(lock);
    }
    void recover() {
        fs::create_directories(work);
        fs::create_directories(root / "game");
        auto restore = [&](const fs::path &parent, const std::string &name) {
            std::vector<fs::path> previous;
            std::string prefix = name + ".prev-";
            for (auto &e : fs::directory_iterator(parent)) {
                auto n = e.path().filename().string();
                if (n.rfind(prefix, 0) == 0 && session_valid(n.substr(prefix.size())))
                    previous.push_back(e.path());
            }
            auto live = parent / name;
            if (!fs::exists(live) && previous.size() == 1)
                fs::rename(previous[0], live);
            if (!fs::exists(live) && previous.size() > 1)
                fail("internal", "Multiple recovery copies need attention.");
            for (const auto &p : previous)
                if (fs::exists(p))
                    fs::remove_all(p);
        };
        restore(root / "game", "romfs");
        for (size_t i = 1; i <= 8; ++i)
            restore(root / "game", "aoc" + std::to_string(i));
        restore(work, "exefs");
        for (auto &e : fs::directory_iterator(work)) {
            auto n = e.path().filename().string();
            if (session_valid(n) && n != st.session)
                fs::remove_all(e.path());
        }
    }
    void verify_content_hash(const Selection &content) {
        auto s = vopen(content.pkg->vfs, content.pkg->prefix + "/" + content.name);
        tc::crypto::Sha2256Generator hash;
        hash.initialize();
        std::vector<byte_t> buffer(chunk_size);
        uint64_t left = uint64_t(content.size);
        while (left) {
            st.check_cancel();
            size_t n = size_t(std::min<uint64_t>(left, buffer.size()));
            if (s->read(buffer.data(), n) != n)
                fail("content_corrupt", "NCA is truncated.", content.pkg->name);
            hash.update(buffer.data(), n);
            left -= n;
        }
        std::array<byte_t, 32> digest{};
        hash.getHash(digest.data());
        if (digest != content.hash)
            fail("content_corrupt", "NCA checksum failed.", content.pkg->name);
    }
    struct Entry {
        Vfs source;
        std::string virtual_path, relative, directory;
        uint64_t size;
    };
    std::vector<Entry> entries;
    uint64_t rom_count = 0;
    void walk(const Vfs &source, const std::string &vpath, const std::string &relative,
              const std::string &directory) {
        st.check_cancel();
        auto d = listing(source, vpath);
        for (auto &n : d.file_list) {
            if (!safe_component(n))
                fail("content_corrupt", "Invalid path in game content.");
            auto s = vopen(source, vpath + "/" + n);
            if (s->length() < 0)
                fail("content_corrupt", "Invalid file size.");
            uint64_t size = uint64_t(s->length());
            if (UINT64_MAX - st.total < size)
                fail("content_corrupt", "Invalid content size.");
            st.total += size;
            ++st.file_total;
            if (directory == "romfs")
                ++rom_count;
            entries.push_back({source, vpath + "/" + n, relative.empty() ? n : relative + "/" + n,
                               directory, size});
        }
        for (auto &n : d.dir_list) {
            if (!safe_component(n))
                fail("content_corrupt", "Invalid directory in game content.");
            walk(source, vpath + "/" + n, relative.empty() ? n : relative + "/" + n, directory);
        }
    }
    void mount() {
        try {
            nstool::NcaProcess base_probe;
            base_probe.setKeyCfg(*keys);
            base_probe.setInputFile(vopen(base.pkg->vfs, base.pkg->prefix + "/" + base.name));
            base_probe.process();
            (void)listing(base_probe.getFileSystem(), "/0");
            (void)listing(base_probe.getFileSystem(), "/1");
            nstool::NcaProcess update_probe;
            update_probe.setKeyCfg(*keys);
            update_probe.setInputFile(
                vopen(update.pkg->vfs, update.pkg->prefix + "/" + update.name));
            update_probe.process();
            (void)listing(update_probe.getFileSystem(), "/0");
            nstool::NcaProcess p;
            p.setKeyCfg(*keys);
            p.setInputFile(vopen(update.pkg->vfs, update.pkg->prefix + "/" + update.name));
            p.setBaseNcaStream(vopen(base.pkg->vfs, base.pkg->prefix + "/" + base.name));
            p.process();
            merged = p.getFileSystem();
            if (!merged)
                fail("content_corrupt", "Unable to mount game content.");
            walk(merged, "/0", "", "exefs");
            walk(merged, "/1", "", "romfs");
            for (size_t i = 0; i < addons.size(); ++i)
                if (addons[i].content.pkg) {
                    const auto &a = addons[i].content;
                    try {
                        nstool::NcaProcess addon;
                        addon.setKeyCfg(*keys);
                        addon.setInputFile(vopen(a.pkg->vfs, a.pkg->prefix + "/" + a.name));
                        addon.process();
                        addons[i].vfs = addon.getFileSystem();
                        walk(addons[i].vfs, "/0", "", "aoc" + std::to_string(i + 1));
                    } catch (const tc::Exception &e) {
                        diagnostic(st.session.c_str(), "mount_addon", errno, "tc::Exception",
                                   e.what());
                        fail("ticket_missing",
                             "Cannot decrypt add-on content. Supply its ticket and valid keys.",
                             a.pkg->name);
                    }
                }
        } catch (const Failure &) {
            throw;
        } catch (const tc::Exception &e) {
            diagnostic(st.session.c_str(), "mount_content", errno, "tc::Exception", e.what());
            fail("ticket_missing", "Cannot decrypt game content. Supply valid keys and tickets.");
        }
        if (rom_count < 39009)
            fail("content_missing", "Game content is incomplete.");
        struct statvfs sb;
        if (statvfs(dir.c_str(), &sb))
            fail("io_error", "Cannot check free space.");
        if ((uint64_t)sb.f_bavail * (uint64_t)sb.f_frsize < st.total + 64 * 1024 * 1024ULL)
            fail("insufficient_space", "Free disk space before importing.");
    }
    void wait_confirm() {
        st.set("awaiting_confirm");
        auto until = std::chrono::steady_clock::now() + 15min;
        while (std::chrono::steady_clock::now() < until) {
            st.check_cancel();
            if (field(text(dir / "confirm"), "session") == st.session)
                return;
            std::this_thread::sleep_for(100ms);
        }
        fail("stale_request", "Import confirmation timed out. Retry the import.");
    }
    void extract() {
        staging = work / st.session;
        fs::remove_all(staging);
        fs::create_directories(staging / "exefs");
        fs::create_directories(staging / "romfs");
        for (size_t i = 0; i < addons.size(); ++i)
            if (addons[i].content.pkg)
                fs::create_directories(staging / ("aoc" + std::to_string(i + 1)));
        std::vector<byte_t> buffer(chunk_size);
        bool in_rom = false;
        st.set("extracting_exefs");
        for (auto &e : entries) {
            st.check_cancel();
            if (e.directory != "exefs" && !in_rom) {
                in_rom = true;
                st.set("extracting_romfs");
            }
            st.current = e.directory + "/" + e.relative;
            auto output = staging / st.current;
            fs::create_directories(output.parent_path());
            FILE *out = fopen(output.c_str(), "wb");
            if (!out)
                fail("io_error", "Cannot create output file.");
            try {
                auto in = vopen(e.source, e.virtual_path);
                uint64_t left = e.size;
                while (left) {
                    st.check_cancel();
                    size_t want = size_t(std::min<uint64_t>(left, buffer.size()));
                    size_t got = in->read(buffer.data(), want);
                    if (got != want)
                        fail("content_corrupt", "Unexpected end of game content.");
                    if (fwrite(buffer.data(), 1, got, out) != got)
                        fail("io_error", "Cannot write game content.");
                    left -= got;
                    st.done += got;
                    st.publish();
                }
                if (fclose(out)) {
                    out = nullptr;
                    fail("io_error", "Cannot finish output file.");
                }
                out = nullptr;
            } catch (...) {
                if (out)
                    fclose(out);
                throw;
            }
            ++st.files;
            st.publish();
        }
    }
    void verify() {
        st.set("verifying");
        std::ifstream f(staging / "exefs/main", std::ios::binary);
        std::array<unsigned char, 0x48> hdr{};
        f.read((char *)hdr.data(), hdr.size());
        static const unsigned char id[] = {0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65};
        if (f.gcount() != hdr.size() || memcmp(hdr.data(), "NSO0", 4) ||
            memcmp(hdr.data() + 0x40, id, 8))
            fail("wrong_version", "The extracted game executable is not version 1.9.0.");
        auto version = text(staging / "romfs/System/Version.txt");
        while (!version.empty() &&
               (version.back() == '\r' || version.back() == '\n' || version.back() == ' '))
            version.pop_back();
        if (version != "1.9.0")
            fail("wrong_version", "The extracted game data is not version 1.9.0.");
    }
    void publish() {
        st.set("publishing");
        struct Move {
            fs::path live, staged, previous;
            bool has_new, moved = false, installed = false;
        };
        std::vector<Move> moves;
        auto add = [&](const fs::path &parent, const std::string &name, bool has_new) {
            moves.push_back(
                {parent / name, staging / name, parent / (name + ".prev-" + st.session), has_new});
        };
        add(work, "exefs", true);
        add(root / "game", "romfs", true);
        for (size_t i = 0; i < addons.size(); ++i)
            add(root / "game", "aoc" + std::to_string(i + 1), addons[i].content.pkg != nullptr);
        try {
            for (auto &m : moves) {
                if (fs::exists(m.live)) {
                    fs::rename(m.live, m.previous);
                    m.moved = true;
                }
                if (m.has_new) {
                    fs::rename(m.staged, m.live);
                    m.installed = true;
                }
            }
            if (!fs::remove(dir / "prod.keys"))
                fail("io_error", "Unable to remove prod.keys after import.");
            if (fs::exists(dir / "title.keys") && !fs::remove(dir / "title.keys"))
                fail("io_error", "Unable to remove title.keys after import.");
        } catch (...) {
            for (auto i = moves.rbegin(); i != moves.rend(); ++i) {
                if (i->installed)
                    fs::remove_all(i->live);
                if (i->moved)
                    fs::rename(i->previous, i->live);
            }
            throw;
        }
        std::error_code ec;
        for (const auto &m : moves)
            if (m.moved)
                fs::remove_all(m.previous, ec);
        fs::remove_all(staging, ec);
        st.set("ready");
    }
    int run() {
        const char *step = "create_import_directory";
        bool locked = false;
        auto report_failure = [&](const char *code, const char *message,
                                  const std::string &file) noexcept {
            try {
                if (!staging.empty()) {
                    std::error_code ec;
                    fs::remove_all(staging, ec);
                    if (ec)
                        diagnostic(st.session.c_str(), "cleanup_staging", ec.value(),
                                   "filesystem_error", ec.message().c_str());
                }
                if (locked && session_valid(st.session)) {
                    st.error = code;
                    st.error_file = file;
                    st.message = message;
                    st.set(std::strcmp(code, "cancelled") == 0 ? "cancelled" : "failed");
                }
            } catch (const Failure &e) {
                diagnostic(st.session.c_str(), "publish_failure_status", e.error, e.code.c_str(),
                           e.message.c_str());
            } catch (const tc::Exception &e) {
                diagnostic(st.session.c_str(), "publish_failure_status", errno, "tc::Exception",
                           e.what());
            } catch (const std::exception &e) {
                diagnostic(st.session.c_str(), "publish_failure_status", errno, "std::exception",
                           e.what());
            } catch (...) {
                diagnostic(st.session.c_str(), "publish_failure_status", errno, "unknown", "");
            }
        };
        try {
            errno = 0;
            fs::create_directories(dir);
            step = "open_lock";
            errno = 0;
            lock = ::open((dir / ".import-lock").c_str(), O_CREAT | O_RDWR, 0644);
            if (lock < 0) {
                diagnostic(nullptr, step, errno, "system_error", "Unable to open import lock");
                return 1;
            }
            step = "acquire_lock";
            errno = 0;
            if (flock(lock, LOCK_EX | LOCK_NB)) {
                diagnostic(nullptr, step, errno, "system_error", "Unable to acquire import lock");
                return 1;
            }
            locked = true;
            step = "read_request";
            errno = 0;
            std::string req = text(dir / "request");
            int request_errno = errno;
            st.session = field(req, "session");
            if (!session_valid(st.session)) {
                diagnostic(nullptr, step, request_errno, "invalid_request",
                           req.empty() ? "Request empty or unreadable"
                                       : "Missing or invalid session");
                return 1;
            }
            if (field(req, "version") != "1") {
                diagnostic(st.session.c_str(), step, request_errno, "invalid_request",
                           "Missing or unsupported request version");
                report_failure("internal", "Invalid import request. Retry the import.", {});
                return 1;
            }
            step = "recovery";
            errno = 0;
            recover();
            step = "checking_packages";
            st.set("checking_packages");
            scan_packages();
            step = "checking_keys";
            st.set("checking_keys");
            load_keys();
            step = "reading_metadata";
            find_meta();
            step = "hashing_content";
            verify_content_hash(update);
            for (const auto &a : addons)
                if (a.content.pkg)
                    verify_content_hash(a.content);
            step = "mounting_content";
            mount();
            step = "awaiting_confirm";
            wait_confirm();
            step = "extracting";
            extract();
            step = "verifying";
            verify();
            step = "publishing";
            publish();
            return 0;
        } catch (const Failure &e) {
            diagnostic(session_valid(st.session) ? st.session.c_str() : nullptr, step, e.error,
                       e.code.c_str(), e.message.c_str());
            report_failure(e.code.c_str(), e.message.c_str(), e.file);
        } catch (const tc::Exception &e) {
            diagnostic(session_valid(st.session) ? st.session.c_str() : nullptr, step, errno,
                       "tc::Exception",
                       std::strcmp(step, "checking_keys") == 0
                           ? "Key parser exception details withheld"
                           : e.what());
            report_failure("internal", "Import failed. Retry the import.", {});
        } catch (const std::exception &e) {
            diagnostic(session_valid(st.session) ? st.session.c_str() : nullptr, step, errno,
                       "std::exception",
                       std::strcmp(step, "checking_keys") == 0
                           ? "Key parser exception details withheld"
                           : e.what());
            report_failure("internal", "Import failed. Retry the import.", {});
        } catch (...) {
            diagnostic(session_valid(st.session) ? st.session.c_str() : nullptr, step, errno,
                       "unknown_exception", "");
            report_failure("internal", "Import failed. Retry the import.", {});
        }
        return 1;
    }
};
int savagewind_import(const char *root) {
    try {
        return Importer(root).run();
    } catch (const tc::Exception &e) {
        diagnostic(nullptr, "construct_importer", errno, "tc::Exception", e.what());
        return 1;
    } catch (const std::exception &e) {
        diagnostic(nullptr, "construct_importer", errno, "std::exception", e.what());
        return 1;
    } catch (...) {
        diagnostic(nullptr, "construct_importer", errno, "unknown_exception", "");
        return 1;
    }
}
