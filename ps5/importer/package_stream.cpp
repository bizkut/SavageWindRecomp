// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_core.h"
#include <mutex>

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
        nstool::NcaProcess proc;
        proc.setKeyCfg(*importer.keys);
        if (addon) {
            if (addon > importer.addons.size() || !importer.addons[addon - 1].content.pkg)
                fail("content_missing", "Add-on content not found.");
            const auto &selection = importer.addons[addon - 1].content;
            proc.setInputFile(vopen(selection.pkg->vfs, selection.pkg->prefix + "/" + selection.name));
        } else {
            proc.setInputFile(vopen(importer.update.pkg->vfs, importer.update.pkg->prefix + "/" + importer.update.name));
            proc.setBaseNcaStream(vopen(importer.base.pkg->vfs, importer.base.pkg->prefix + "/" + importer.base.name));
        }
        proc.process();
        result->stream = proc.getPartitionReader(addon ? 0 : 1);
        if (!result->stream || result->stream->length() < 0)
            fail("content_corrupt", "Cannot open RomFS partition.");
        return result.release();
    } catch (const Failure &e) {
        if (why && why_size)
            std::snprintf(why, why_size, "%s: %s", e.code.c_str(), e.message.c_str());
    } catch (...) {
        if (why && why_size)
            std::snprintf(why, why_size, "Unable to open package content.");
    }
    return nullptr;
}

extern "C" uint64_t savagewind_package_size(void *handle) {
    return uint64_t(static_cast<PackageStream *>(handle)->stream->length());
}

extern "C" ssize_t savagewind_package_pread(void *handle, void *dst, size_t size, uint64_t offset) {
    auto *package = static_cast<PackageStream *>(handle);
    try {
        std::lock_guard<std::mutex> guard(package->mutex);
        package->stream->seek(int64_t(offset), tc::io::SeekOrigin::Begin);
        return ssize_t(package->stream->read(static_cast<byte_t *>(dst), size));
    } catch (...) {
        return -1;
    }
}

extern "C" void savagewind_package_close(void *handle) {
    delete static_cast<PackageStream *>(handle);
}
