// SPDX-License-Identifier: GPL-3.0-or-later
// Copy selected pack archives from the merged, package-backed RomFS to a private output directory.
#include "package_exefs.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/types.h>
extern "C" void *savagewind_package_open(const char *, const char *const *, size_t, unsigned, char *, size_t);
extern "C" ssize_t savagewind_package_pread(void *, void *, size_t, uint64_t);
extern "C" void savagewind_package_close(void *);
// HD_UI_PACK optionally selects one archive for a bounded first run.
static uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v,p,4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; std::memcpy(&v,p,8); return v; }
int main(int argc, char **argv) {
    if (argc < 5) { std::fprintf(stderr,"usage: hd-ui-extract <keys> <output-dir> <base.nsp> <update.nsp> [packages...]\n"); return 2; }
    const char *control = std::getenv("HD_UI_CONTROL");
    if (control) {
        char error[256] = {};
        const char *const *packages = const_cast<const char *const *>(argv + 3);
        ssize_t size = savagewind_control_read(argv[1], packages, argc - 3, control, nullptr, 0,
                                               error, sizeof error);
        if (size < 0 || size > 16 * 1024 * 1024) {
            std::fprintf(stderr, "Control NCA: %s\n", error);
            return 1;
        }
        std::vector<uint8_t> bytes(size);
        if (savagewind_control_read(argv[1], packages, argc - 3, control, bytes.data(), bytes.size(),
                                    error, sizeof error) != size) {
            std::fprintf(stderr, "Control NCA: %s\n", error);
            return 1;
        }
        std::filesystem::create_directories(argv[2]);
        auto output = std::filesystem::path(argv[2]) / control;
        FILE *f = std::fopen(output.string().c_str(), "wb");
        if (!f) return 1;
        bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        ok = std::fclose(f) == 0 && ok;
        return ok ? 0 : 1;
    }
    char why[256] = {}; void *pkg=savagewind_package_open(argv[1],const_cast<const char *const *>(argv+3),argc-3,0,why,sizeof why);
    if (!pkg) { std::fprintf(stderr,"%s\n",why); return 1; }
    try {
        auto read=[&](uint64_t off,size_t n) { std::vector<uint8_t> v(n); if (savagewind_package_pread(pkg,v.data(),n,off)!=(ssize_t)n) throw std::runtime_error("RomFS read failed"); return v; };
        auto h=read(0,80); uint64_t ds=u64(h.data()+32), fs=u64(h.data()+64);
        if (ds>128*1024*1024 || fs>128*1024*1024) throw std::runtime_error("invalid RomFS metadata size");
        auto dirs=read(u64(h.data()+24),ds), files=read(u64(h.data()+56),fs);
        std::filesystem::create_directories(argv[2]);
        size_t count=0;
        for (size_t o=0; o+32<=fs;) {
            uint32_t parent=u32(files.data()+o), n=u32(files.data()+o+28);
            if (n>fs-o-32) throw std::runtime_error("invalid RomFS entry");
            std::string name((char *)files.data()+o+32,n), path=name;
            for (int depth=0; parent && depth<32; depth++) {
                if (parent>ds || ds-parent<24) throw std::runtime_error("invalid RomFS directory");
                uint32_t len=u32(dirs.data()+parent+20); if (len>ds-parent-24) throw std::runtime_error("invalid RomFS directory name");
                path=std::string((char *)dirs.data()+parent+24,len)+"/"+path; parent=u32(dirs.data()+parent);
            }
            const char *selected=std::getenv("HD_UI_PACK");
            if (path.rfind("Pack/",0)==0 && name.size()>5 && name.substr(name.size()-5)==".pack" &&
                name.find_first_of("/\\")==name.npos && (!selected || name==selected)) {
                uint64_t len=u64(files.data()+o+16); if (len>1024ull*1024*1024) throw std::runtime_error("pack too large");
                auto data=read(u64(h.data()+72)+u64(files.data()+o+8),size_t(len));
                auto output=std::filesystem::path(argv[2])/name; FILE *f=std::fopen(output.string().c_str(),"wb");
                if (!f) throw std::runtime_error("cannot write pack");
                bool ok=std::fwrite(data.data(),1,data.size(),f)==data.size(); ok=std::fclose(f)==0 && ok; if (!ok) throw std::runtime_error("cannot write pack");
                count++;
            }
            o+=32+((uint64_t(n)+3)&~3ull);
        }
        std::fprintf(stderr,"extracted %zu packs\n",count);
        savagewind_package_close(pkg); return count ? 0 : 1;
    } catch (const std::exception &e) { std::fprintf(stderr,"hd-ui-extract: %s\n",e.what()); savagewind_package_close(pkg); return 1; }
}
