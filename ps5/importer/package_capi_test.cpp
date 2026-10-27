// Host checks for package C ABI failure paths.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_exefs.h"
#include <cstdint>
#include <cstring>

extern "C" void *savagewind_package_open(const char *, const char *const *, size_t, unsigned, char *, size_t);
extern "C" uint64_t savagewind_package_size(void *);
extern "C" ssize_t savagewind_package_pread(void *, void *, size_t, uint64_t);
extern "C" void savagewind_package_close(void *);

int main() {
    char why[256] = {};
    if (savagewind_package_open(nullptr, nullptr, 0, 0, why, sizeof why) ||
        !std::strstr(why, "keys_missing")) return 1;
    if (savagewind_package_size(nullptr) != UINT64_MAX ||
        savagewind_package_pread(nullptr, nullptr, 0, 0) != -1) return 1;
    savagewind_package_close(nullptr);
    why[0] = 0;
    if (savagewind_exefs_open(nullptr, nullptr, 0, why, sizeof why) ||
        !std::strstr(why, "keys_missing")) return 1;
    if (savagewind_exefs_file_open(nullptr, "main") || savagewind_exefs_file_size(nullptr) != -1 ||
        savagewind_exefs_file_pread(nullptr, nullptr, 0, 0) != -1) return 1;
    savagewind_exefs_file_close(nullptr);
    savagewind_exefs_close(nullptr);
    return 0;
}
