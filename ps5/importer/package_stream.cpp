// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_core.h"
#include "package_exefs.h"
#include <mutex>

static void (*diagnostic_callback)(const char *);
extern "C" void savagewind_package_set_diagnostic(void (*callback)(const char *)) {
    diagnostic_callback = callback;
}
static void report_stage(const char *stage) {
    diagnostic("package-runtime", stage, 0, "begin", "");
    if (diagnostic_callback) diagnostic_callback(stage);
}

struct PackageStream {
    PackageCore importer;
    Stream stream;
    std::mutex mutex;
    PackageStream() : importer(".") { importer.st.session = "package-runtime"; }
};

extern "C" void *savagewind_package_open(const char *keys_path, const char *const *paths,
                                         size_t count, unsigned addon, char *why, size_t why_size) {
    try {
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
        } else {
            report_stage("nca-open-update");
            proc.setInputFile(vopen(importer.update.pkg->vfs, importer.update.pkg->prefix + "/" + importer.update.name));
            report_stage("nca-open-base");
            proc.setBaseNcaStream(vopen(importer.base.pkg->vfs, importer.base.pkg->prefix + "/" + importer.base.name));
        }
        report_stage("nca-process");
        proc.process();
        report_stage("mount-romfs");
        result->stream = proc.getPartitionReader(addon ? 0 : 1);
        if (!result->stream || result->stream->length() < 0)
            fail("content_corrupt", "Cannot open RomFS partition.");
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
    if (!handle) return UINT64_MAX;
    try {
        return uint64_t(static_cast<PackageStream *>(handle)->stream->length());
    } catch (const std::exception &e) {
        diagnostic("package-runtime", "size", errno, "exception", e.what());
    } catch (...) {
        diagnostic("package-runtime", "size", errno, "exception", "non-standard exception");
    }
    return UINT64_MAX;
}

extern "C" ssize_t savagewind_package_pread(void *handle, void *dst, size_t size, uint64_t offset) {
    if (!handle || offset > INT64_MAX) return -1;
    auto *package = static_cast<PackageStream *>(handle);
    try {
        std::lock_guard<std::mutex> guard(package->mutex);
        package->stream->seek(int64_t(offset), tc::io::SeekOrigin::Begin);
        return ssize_t(package->stream->read(static_cast<byte_t *>(dst), size));
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
