// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_core.h"
#include "read_profile.h"
#include "../storagebroker/client.h"
#include <atomic>

static std::atomic<bool> use_package_broker{false};
extern "C" void savagewind_package_set_broker(int enabled) {
    use_package_broker.store(enabled != 0, std::memory_order_relaxed);
}

// Each package stream owns its connection and broker-side file descriptor.
class PackageFileStream : public tc::io::IStream {
    std::shared_ptr<tc::io::FileStream> direct;
    std::string relative;
    sb_client broker{-1, 0};
    bool attempted = false;
public:
    explicit PackageFileStream(const fs::path &path)
        : direct(std::make_shared<tc::io::FileStream>(
              tc::io::Path(path.string()), tc::io::FileMode::Open, tc::io::FileAccess::Read)) {
        const std::string full = path.string(), root = SB_ROOT "/";
        if (full.compare(0, root.size(), root) == 0)
            relative = full.substr(root.size());
    }
    ~PackageFileStream() override { sb_close(&broker); }
    bool canRead() const override { return direct->canRead(); }
    bool canWrite() const override { return direct->canWrite(); }
    bool canSeek() const override { return direct->canSeek(); }
    int64_t length() override { return direct->length(); }
    int64_t position() override { return direct->position(); }
    size_t read(byte_t *ptr, size_t count) override {
        if (savage_read_profile.active && use_package_broker.load(std::memory_order_relaxed) &&
            count >= 256 * 1024 && count <= SB_MAX_TRANSFER && relative.compare(0, 7, "import/") == 0) {
            if (!attempted) {
                attempted = true;
                if (sb_connect(&broker) || broker.server_version != SB_VERSION ||
                    sb_package_open(&broker, relative.c_str())) sb_close(&broker);
            }
            if (broker.fd >= 0) {
                int64_t pos = direct->position();
                uint64_t start = read_profile_now();
                int result = pos < 0 ? -1 : sb_package_read(&broker, ptr, uint32_t(count), uint64_t(pos));
                uint64_t elapsed = read_profile_now() - start;
                auto &profile = savage_read_profile;
                profile.io_ns += elapsed;
                profile.broker_ns += elapsed;
                if (result >= 0) {
                    ++profile.broker_reads;
                    profile.broker_bytes += uint64_t(result);
                    direct->seek(pos + result, tc::io::SeekOrigin::Begin);
                    return size_t(result);
                }
                sb_close(&broker);
            }
        }
        return direct->read(ptr, count);
    }
    size_t write(const byte_t *p, size_t n) override { return direct->write(p, n); }
    int64_t seek(int64_t o, tc::io::SeekOrigin s) override { return direct->seek(o, s); }
    void setLength(int64_t n) override { direct->setLength(n); }
    void flush() override { direct->flush(); }
    void dispose() override { sb_close(&broker); direct->dispose(); }
};

Vfs package_filesystem(const fs::path &path) {
    std::string name = path.filename().string();
    auto stream = std::make_shared<PackageFileStream>(path);
    if (ends(name, ".nsp")) {
        nstool::PfsProcess proc;
        proc.setInputFile(stream);
        proc.process();
        return proc.getFileSystem();
    }
    if (ends(name, ".xci")) {
        nstool::GameCardProcess proc;
        proc.setInputFile(stream);
        proc.process();
        return proc.getFileSystem();
    }
    fail("unsupported_container", "Expected an NSP or XCI.", name);
}
void PackageCore::open_package(const fs::path &path) {
        std::string name = path.filename().string();
        try {
            Vfs vfs = package_filesystem(path);
            packages.push_back({name, ends(name, ".xci") ? "/secure" : "", vfs, path});
        } catch (const tc::Exception &e) {
            diagnostic(st.session.c_str(), "open_package", errno, "tc::Exception", e.what());
            fail("content_corrupt", "Cannot read package. Replace the damaged file.", name);
        }
    }
void PackageCore::scan_packages() {
        std::vector<fs::path> inputs;
        for (auto &e : fs::directory_iterator(dir)) {
            std::string n = e.path().filename().string();
            if (ends(n, ".nsz") || ends(n, ".xcz"))
                fail("unsupported_container", "Compressed NSZ/XCZ is not supported.", n);
            if (ends(n, ".nsp") || ends(n, ".xci"))
                inputs.push_back(e.path());
        }
        std::sort(inputs.begin(), inputs.end());
        for (auto &p : inputs) {
            st.check_cancel();
            open_package(p);
        }
    }
void PackageCore::load_keys(const fs::path &prod, const fs::path &title) {
        // Non-throwing checks: the PS5 title libc answers stat("") with EINVAL (not ENOENT), which the throwing
        // fs::exists overload turns into an exception; an empty path means "not supplied".
        std::error_code ec;
        if (prod.empty()) fail("keys_missing", "Add prod.keys to the import directory.");
        bool have_prod = fs::exists(prod, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            errno = ec.value();
            fail("io_error", "Unable to inspect prod.keys.");
        }
        if (!have_prod) fail("keys_missing", "Add prod.keys to the import directory.");
        try {
            tc::Optional<tc::io::Path> kp{tc::io::Path(prod.string())}, tp;
            if (!title.empty()) {
                ec.clear();
                bool have_title = fs::exists(title, ec);
                if (ec && ec != std::errc::no_such_file_or_directory) {
                    errno = ec.value();
                    fail("io_error", "Unable to inspect title.keys.");
                }
                if (have_title) tp = tc::io::Path(title.string());
            }
            keys = std::make_shared<nstool::KeyBagInitializer>(
                false, kp, tp, std::vector<tc::io::Path>{}, tc::Optional<tc::io::Path>());
        } catch (const tc::Exception &) {
            diagnostic(st.session.c_str(), "read_keys", errno, "tc::Exception",
                       "Key parser exception details withheld");
            fail("keys_invalid", "Cannot read prod.keys. Supply valid keys.");
        }
        if (keys->nca_header_key.isNull())
            fail("keys_invalid", "prod.keys lacks the NCA header key.");
        for (auto &p : packages) {
            auto d = listing(p.vfs, p.prefix.empty() ? "/" : p.prefix);
            for (auto &n : d.file_list)
                if (ends(n, ".tik")) {
                    try {
                        keys->importTicketStream(vopen(p.vfs, p.prefix + "/" + n));
                    } catch (const tc::Exception &) {
                        diagnostic(st.session.c_str(), "read_ticket", errno, "tc::Exception",
                                   "Ticket parser exception details withheld");
                        fail("content_corrupt", "Cannot read a package ticket.", p.name);
                    }
                }
        }
    }
void PackageCore::load_keys() { load_keys(dir / "prod.keys", dir / "title.keys"); }
void PackageCore::find_meta() {
        bool saw_meta = false;
        for (auto &p : packages) {
            auto d = listing(p.vfs, p.prefix.empty() ? "/" : p.prefix);
            for (auto &n : d.file_list)
                if (ends(n, ".cnmt.nca")) {
                    st.check_cancel();
                    try {
                        nstool::NcaProcess proc;
                        proc.setInputFile(vopen(p.vfs, p.prefix + "/" + n));
                        proc.setKeyCfg(*keys);
                        proc.process();
                        auto v = proc.getFileSystem();
                        auto files = listing(v, "/0");
                        for (auto &f : files.file_list)
                            if (ends(f, ".cnmt")) {
                                auto s = vopen(v, "/0/" + f);
                                auto size = s->length();
                                if (size < 0 || size > 20 * 1024 * 1024)
                                    fail("content_corrupt", "Invalid package metadata.", p.name);
                                std::vector<byte_t> buf(size_t(size), 0);
                                if (s->read(buf.data(), buf.size()) != buf.size())
                                    fail("content_corrupt", "Truncated package metadata.", p.name);
                                pie::hac::ContentMeta m;
                                m.fromBytes(buf.data(), buf.size());
                                saw_meta = true;
                                if (m.getContentMetaType() ==
                                        pie::hac::cnmt::ContentMetaType_AddOnContent &&
                                    m.getAddOnContentMetaExtendedHeader().getApplicationId() ==
                                        base_id) {
                                    uint64_t aoc_base = (base_id & ~uint64_t(0xfff)) + 0x1000,
                                             id = m.getTitleId();
                                    if (id <= aoc_base || id > aoc_base + 8)
                                        fail("unsupported_container", "Unsupported add-on index.",
                                             p.name);
                                    aoc_candidates[size_t(id - aoc_base - 1)].push_back({&p, m, n});
                                }
                                if (m.getTitleId() == base_id &&
                                    m.getContentMetaType() ==
                                        pie::hac::cnmt::ContentMetaType_Application)
                                    bases.push_back({&p, m, n});
                                if (m.getTitleId() == update_id &&
                                    m.getContentMetaType() == pie::hac::cnmt::ContentMetaType_Patch)
                                    updates.push_back({&p, m, n});
                            }
                    } catch (const Failure &) {
                        throw;
                    } catch (const tc::Exception &e) {
                        diagnostic(st.session.c_str(), "read_metadata", errno, "tc::Exception",
                                   e.what());
                        fail("content_corrupt",
                             "Unable to decrypt package metadata. Check keys and package.", p.name);
                    }
                }
        }
        if (bases.empty() && updates.empty() && saw_meta)
            fail("wrong_title", "No Breath of the Wild game content was found.");
        if (bases.empty())
            fail("base_missing", "Base game missing. Add the Breath of the Wild base NSP or XCI.");
        if (updates.empty())
            fail("update_missing", "Update missing. Add the 1.9.0 update NSP.");
        updates.erase(
            std::remove_if(updates.begin(), updates.end(),
                           [](const Candidate &c) {
                               return c.meta.getTitleVersion() != 1114112 ||
                                      c.meta.getPatchMetaExtendedHeader().getApplicationId() !=
                                          base_id;
                           }),
            updates.end());
        if (updates.empty())
            fail("wrong_version", "Add the Breath of the Wild 1.9.0 update.");
        auto choose = [&](std::vector<Candidate> &c,
                          pie::hac::cnmt::ContentType type) -> Selection {
            Selection result;
            for (auto &candidate : c)
                for (auto &entry : candidate.meta.getContentInfo())
                    if (entry.getContentType() == type && entry.getIdOffset() == 0) {
                        auto id =
                            hex(entry.getContentId().data(), entry.getContentId().size()) + ".nca";
                        if (!std::equal(entry.getContentId().begin(), entry.getContentId().end(),
                                        entry.getContentHash().begin()))
                            fail("content_corrupt",
                                 "Content identifier does not match its checksum.",
                                 candidate.pkg->name);
                        if (result.pkg &&
                            (result.name != id || result.size != entry.getContentSize() ||
                             result.hash != entry.getContentHash()))
                            fail("ambiguous_content",
                                 "Multiple different game contents were found.");
                        if (!result.pkg)
                            result = {candidate.pkg, id, entry.getContentSize(),
                                      entry.getContentHash()};
                    }
            if (!result.pkg)
                fail("content_missing", "Package is missing required NCA content.");
            try {
                auto s = vopen(result.pkg->vfs, result.pkg->prefix + "/" + result.name);
                if (s->length() != result.size)
                    fail("content_missing", "Package contains a truncated NCA.", result.pkg->name);
            } catch (const tc::Exception &e) {
                diagnostic(st.session.c_str(), "open_content", errno, "tc::Exception", e.what());
                fail("content_missing", "Package is missing required NCA content.",
                     result.pkg->name);
            }
            return result;
        };
        base = choose(bases, pie::hac::cnmt::ContentType_Program);
        update = choose(updates, pie::hac::cnmt::ContentType_Program);
        st.base = base.pkg->name;
        st.update = update.pkg->name;
        for (size_t i = 0; i < addons.size(); ++i)
            if (!aoc_candidates[i].empty()) {
                addons[i].content = choose(aoc_candidates[i], pie::hac::cnmt::ContentType_Data);
                if (!st.dlc.empty())
                    st.dlc += ",";
                st.dlc += std::to_string(i + 1);
            }
    }
void PackageCore::open_inputs(const char *keys_path, const char *const *paths, size_t count) {
    if (!keys_path || !*keys_path) fail("keys_missing", "Add prod.keys to the import directory.");
    if (count && !paths) fail("content_missing", "No package paths supplied.");
    for (size_t i = 0; i < count; ++i)
        if (!paths[i] || !*paths[i]) fail("content_missing", "Empty package path.");
    auto run_step = [&](const char *step, const char *detail, auto &&action) {
        diagnostic(st.session.c_str(), step, 0, "begin", detail);
        try {
            action();
        } catch (const Failure &e) {
            diagnostic(st.session.c_str(), step, e.error, e.code.c_str(), e.message.c_str());
            throw;
        } catch (const std::exception &e) {
            diagnostic(st.session.c_str(), step, errno, "exception", e.what());
            throw;
        } catch (...) {
            diagnostic(st.session.c_str(), step, errno, "exception", "non-standard exception");
            throw;
        }
    };
    packages.reserve(count);
    for (size_t i = 0; i < count; ++i)
        run_step("open-package", paths[i], [&] { open_package(fs::path(paths[i])); });
    run_step("parse-keys", "", [&] {
        fs::path prod(keys_path);
        load_keys(prod, prod.parent_path() / "title.keys");
    });
    run_step("scan-cnmt", "", [&] { find_meta(); });
}
