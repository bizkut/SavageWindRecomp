// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_core.h"
#include "../../runtime/game_versions.h"

struct GameVersion {
    const char *version;
    const char *build_id;
    bool supported;
};
#define VERSION_ROW(version, build, supported, files) {version, build, supported},
static const GameVersion versions[] = {GAME_VERSIONS(VERSION_ROW)};
#undef VERSION_ROW

int main(int argc, char **argv) {
    if (argc < 6 || std::strcmp(argv[1], "--keys") || std::strcmp(argv[3], "--out") || !*argv[4]) {
        std::fputs("usage: savagewind-exefs --keys <prod.keys> --out <dir> <package>...\n", stderr);
        return 2;
    }
    try {
        PackageCore importer(".");
        importer.st.session = "host";
        importer.open_inputs(argv[2], const_cast<const char *const *>(argv + 5), size_t(argc - 5));
        nstool::NcaProcess proc;
        proc.setKeyCfg(*importer.keys);
        proc.setInputFile(vopen(importer.update.pkg->vfs, importer.update.pkg->prefix + "/" + importer.update.name));
        proc.setBaseNcaStream(vopen(importer.base.pkg->vfs, importer.base.pkg->prefix + "/" + importer.base.name));
        proc.process();
        auto vfs = proc.getFileSystem();
        auto files = listing(vfs, "/0");
        for (const char *required : {"main", "main.npdm", "rtld", "sdk"}) {
            if (std::find(files.file_list.begin(), files.file_list.end(), required) == files.file_list.end())
                fail("content_missing", "ExeFS is missing a required module.");
        }
        auto main = vopen(vfs, "/0/main");
        unsigned char header[0x60];
        const char *expected = nullptr;
        for (const auto &version : versions)
            if (version.supported && std::strcmp(version.version, "1.9.0") == 0) expected = version.build_id;
        if (main->read(header, sizeof(header)) != sizeof(header) ||
            std::memcmp(header, "NSO0", 4) || !expected || hex(header + 0x40, 8) != expected)
            fail("wrong_version", "main build ID does not match version 1.9.0.");
        fs::create_directories(argv[4]);
        std::vector<byte_t> buffer(1024 * 1024);
        for (const auto &name : files.file_list) {
            if (name != "main" && name != "main.npdm" && name != "rtld" && name != "sdk" && name.rfind("subsdk", 0) != 0)
                continue;
            if (!safe_component(name))
                fail("content_corrupt", "Invalid ExeFS filename.");
            auto input = vopen(vfs, "/0/" + name);
            input->seek(0, tc::io::SeekOrigin::Begin);
            std::ofstream output(fs::path(argv[4]) / name, std::ios::binary | std::ios::trunc);
            if (!output)
                fail("io_error", "Cannot create ExeFS file.");
            auto left = input->length();
            if (left < 0)
                fail("content_corrupt", "Invalid ExeFS file size.");
            while (left) {
                size_t n = size_t(std::min<int64_t>(left, buffer.size()));
                if (input->read(buffer.data(), n) != n)
                    fail("content_corrupt", "Truncated ExeFS file.");
                output.write(reinterpret_cast<const char *>(buffer.data()), n);
                if (!output)
                    fail("io_error", "Cannot write ExeFS file.");
                left -= n;
            }
        }
        return 0;
    } catch (const Failure &e) {
        std::fprintf(stderr, "savagewind-exefs: %s: %s\n", e.code.c_str(), e.message.c_str());
    } catch (...) {
        std::fputs("savagewind-exefs: unable to read packages or keys\n", stderr);
    }
    return 1;
}
