// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_core.h"
#include "package_exefs.h"
#include <condition_variable>
#include <mutex>

static void (*diagnostic_callback)(const char *);
extern "C" void savagewind_package_set_diagnostic(void (*callback)(const char *)) {
    diagnostic_callback = callback;
}
static void report_stage(const char *stage) {
    diagnostic("package-runtime", stage, 0, "begin", "");
    if (diagnostic_callback) diagnostic_callback(stage);
}
// nstool's process classes append to a process-global report; every use in this library holds this lock.
static std::mutex nstool_process_mutex;

// Every layer of a RomFS reader (package file, NCA partition, decryption, update patch, hash check) keeps a
// position, so one reader serves one read at a time. Concurrent reads get further readers, made on first need from
// the first one: they share its keys, patch tables and verified last hash layer (the large parts, read and checked
// once at open) and add two package file descriptors plus small per-reader objects. At most kMaxReaders exist; more
// concurrent reads wait for a free one.
static constexpr size_t kMaxReaders = 4;

struct PackageStream {
    PackageCore importer;
    nstool::NcaProcess::PartitionReaderFactory factory;
    const Package *file_package = nullptr, *base_package = nullptr;
    std::string file_name, base_name;
    uint64_t size = 0;
    std::mutex mutex;
    std::condition_variable available;
    std::vector<Stream> idle;
    size_t readers = 0, reader_limit = kMaxReaders;
    PackageStream() : importer(".") { importer.st.session = "package-runtime"; }

    Stream make_reader() {
        Stream file, base;
        {
            std::lock_guard<std::mutex> guard(nstool_process_mutex);
            file = vopen(package_filesystem(file_package->path), file_package->prefix + "/" + file_name);
            if (base_package)
                base = vopen(package_filesystem(base_package->path), base_package->prefix + "/" + base_name);
        }
        Stream reader = factory(file, base);
        if (!reader || reader->length() != int64_t(size))
            fail("content_corrupt", "Cannot open RomFS partition.");
        return reader;
    }
    Stream acquire() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (!idle.empty()) {
                Stream reader = std::move(idle.back());
                idle.pop_back();
                return reader;
            }
            if (readers < reader_limit) break;
            available.wait(lock);
        }
        size_t count = ++readers;
        lock.unlock();
        try {
            Stream reader = make_reader();
            char detail[32];
            std::snprintf(detail, sizeof detail, "readers=%zu", count);
            diagnostic("package-runtime", "reader-open", 0, "info", detail);
            return reader;
        } catch (const Failure &e) {
            diagnostic("package-runtime", "reader-open", e.error, e.code.c_str(), e.message.c_str());
        } catch (const std::exception &e) {
            diagnostic("package-runtime", "reader-open", errno, "exception", e.what());
        } catch (...) {
            diagnostic("package-runtime", "reader-open", errno, "exception", "non-standard exception");
        }
        // Keep the readers that exist (the first always does) and wait for one of them.
        lock.lock();
        reader_limit = --readers;
        while (idle.empty())
            available.wait(lock);
        Stream reader = std::move(idle.back());
        idle.pop_back();
        return reader;
    }
    void release(Stream reader) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            idle.push_back(std::move(reader));
        }
        available.notify_one();
    }
};

extern "C" void *savagewind_package_open(const char *keys_path, const char *const *paths,
                                         size_t count, unsigned addon, char *why, size_t why_size) {
    try {
        std::lock_guard<std::mutex> guard(nstool_process_mutex);
        auto result = std::make_unique<PackageStream>();
        auto &importer = result->importer;
        importer.open_inputs(keys_path, paths, count);
        report_stage("open-program-nca");
        report_stage("nca-construct");
        nstool::NcaProcess proc;
        report_stage("nca-set-keys");
        proc.setKeyCfg(*importer.keys);
        report_stage("nca-keys-ready");
        if (addon) {
            if (addon > importer.addons.size() || !importer.addons[addon - 1].content.pkg)
                fail("content_missing", "Add-on content not found.");
            const auto &selection = importer.addons[addon - 1].content;
            report_stage("nca-open-addon");
            proc.setInputFile(vopen(selection.pkg->vfs, selection.pkg->prefix + "/" + selection.name));
            result->file_package = selection.pkg;
            result->file_name = selection.name;
        } else {
            report_stage("nca-open-update");
            proc.setInputFile(vopen(importer.update.pkg->vfs, importer.update.pkg->prefix + "/" + importer.update.name));
            report_stage("nca-open-base");
            proc.setBaseNcaStream(vopen(importer.base.pkg->vfs, importer.base.pkg->prefix + "/" + importer.base.name));
            result->file_package = importer.update.pkg;
            result->file_name = importer.update.name;
            result->base_package = importer.base.pkg;
            result->base_name = importer.base.name;
        }
        report_stage("nca-process");
        proc.process();
        report_stage("mount-romfs");
        size_t partition = addon ? 0 : 1;
        Stream stream = proc.getPartitionReader(partition);
        if (!stream || stream->length() < 0)
            fail("content_corrupt", "Cannot open RomFS partition.");
        result->size = uint64_t(stream->length());
        result->factory = proc.getPartitionReaderFactory(partition);
        result->idle.reserve(kMaxReaders);
        result->idle.push_back(std::move(stream));
        result->readers = 1;
        report_stage("romfs-ready");
        return result.release();
    } catch (const Failure &e) {
        diagnostic("package-runtime", "open", e.error, e.code.c_str(), e.message.c_str());
        if (why && why_size)
            std::snprintf(why, why_size, "%s: %s", e.code.c_str(), e.message.c_str());
    } catch (const std::exception &e) {
        diagnostic("package-runtime", "open", errno, "exception", e.what());
        if (why && why_size)
            std::snprintf(why, why_size, "Unable to open package content; see diagnostic log.");
    } catch (...) {
        diagnostic("package-runtime", "open", errno, "exception", "non-standard exception");
        if (why && why_size)
            std::snprintf(why, why_size, "Unable to open package content.");
    }
    return nullptr;
}

extern "C" uint64_t savagewind_package_size(void *handle) {
    return handle ? static_cast<PackageStream *>(handle)->size : UINT64_MAX;
}

extern "C" ssize_t savagewind_package_pread(void *handle, void *dst, size_t size, uint64_t offset) {
    if (!handle || offset > INT64_MAX) return -1;
    auto *package = static_cast<PackageStream *>(handle);
    struct Lease {
        PackageStream *package;
        Stream reader;
        ~Lease() { if (reader) package->release(std::move(reader)); }
    } lease{package, nullptr};
    try {
        lease.reader = package->acquire();
        lease.reader->seek(int64_t(offset), tc::io::SeekOrigin::Begin);
        return ssize_t(lease.reader->read(static_cast<byte_t *>(dst), size));
    } catch (const std::exception &e) {
        diagnostic("package-runtime", "pread", errno, "exception", e.what());
        return -1;
    } catch (...) {
        diagnostic("package-runtime", "pread", errno, "exception", "non-standard exception");
        return -1;
    }
}

extern "C" void savagewind_package_close(void *handle) {
    try { delete static_cast<PackageStream *>(handle); }
    catch (...) { diagnostic("package-runtime", "close", errno, "exception", "non-standard exception"); }
}

// ExeFS uses the same selected base and update NCAs as the RomFS reader.
struct PackageExefs {
    PackageCore importer;
    Vfs vfs;
    PackageExefs() : importer(".") { importer.st.session = "package-exefs"; }
};

extern "C" void *savagewind_exefs_open(const char *keys_path, const char *const *paths,
                                       size_t count, char *why, size_t why_size) {
    try {
        std::lock_guard<std::mutex> guard(nstool_process_mutex);
        auto result = std::make_unique<PackageExefs>();
        auto &importer = result->importer;
        importer.open_inputs(keys_path, paths, count);
        report_stage("exefs-nca-construct");
        nstool::NcaProcess proc;
        report_stage("exefs-nca-set-keys");
        proc.setKeyCfg(*importer.keys);
        report_stage("exefs-nca-open-update");
        proc.setInputFile(vopen(importer.update.pkg->vfs, importer.update.pkg->prefix + "/" + importer.update.name));
        report_stage("exefs-nca-open-base");
        proc.setBaseNcaStream(vopen(importer.base.pkg->vfs, importer.base.pkg->prefix + "/" + importer.base.name));
        report_stage("exefs-nca-process");
        proc.process();
        report_stage("exefs-mount");
        result->vfs = proc.getFileSystem();
        if (!result->vfs) fail("content_corrupt", "Cannot open ExeFS partition.");
        return result.release();
    } catch (const Failure &e) {
        diagnostic("package-exefs", "open", e.error, e.code.c_str(), e.message.c_str());
        if (why && why_size) std::snprintf(why, why_size, "%s: %s", e.code.c_str(), e.message.c_str());
    } catch (const std::exception &e) {
        diagnostic("package-exefs", "open", errno, "exception", e.what());
        if (why && why_size) std::snprintf(why, why_size, "Unable to open package ExeFS; see diagnostic log.");
    } catch (...) {
        if (why && why_size) std::snprintf(why, why_size, "Unable to open package ExeFS.");
    }
    return nullptr;
}

extern "C" void *savagewind_exefs_file_open(void *handle, const char *name) {
    if (!handle || !name) return nullptr;
    try {
        if (!safe_component(name)) return nullptr;
        return new Stream(vopen(static_cast<PackageExefs *>(handle)->vfs, std::string("/0/") + name));
    } catch (...) {
        return nullptr;
    }
}

extern "C" int64_t savagewind_exefs_file_size(void *file) {
    if (!file) return -1;
    try { return (*static_cast<Stream *>(file))->length(); } catch (...) { return -1; }
}

extern "C" ssize_t savagewind_exefs_file_pread(void *file, void *dst, size_t size, uint64_t offset) {
    if (!file || offset > INT64_MAX) return -1;
    try {
        Stream &stream = *static_cast<Stream *>(file);
        stream->seek(int64_t(offset), tc::io::SeekOrigin::Begin);
        return ssize_t(stream->read(static_cast<byte_t *>(dst), size));
    } catch (...) {
        return -1;
    }
}

extern "C" void savagewind_exefs_file_close(void *file) {
    try { delete static_cast<Stream *>(file); }
    catch (...) { diagnostic("package-exefs", "file-close", errno, "exception", "non-standard exception"); }
}
extern "C" void savagewind_exefs_close(void *handle) {
    try { delete static_cast<PackageExefs *>(handle); }
    catch (...) { diagnostic("package-exefs", "close", errno, "exception", "non-standard exception"); }
}
