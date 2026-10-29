// SPDX-License-Identifier: GPL-3.0-or-later
#include "patches.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
        return 2;
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(input)), {});
    std::map<std::string, uint64_t> sites;
    size_t count;
    std::cin >> count;
    for (size_t i = 0; i < count; ++i) {
        std::string name;
        uint64_t value;
        std::cin >> name >> std::hex >> value;
        sites[name] = value;
    }
    constexpr uint64_t base = 0x7000000000;
    PatchContext ctx{image.data(), image.size(), base, sites};
    std::string name;
    uint64_t address, offset;
    unsigned mask;
    while (std::cin >> name >> std::hex >> address >> offset >> mask) {
        if (offset >= image.size() || mask > 255)
            return 2;
        image[offset] ^= uint8_t(mask);
        try {
            std::string result = apply_patch(ctx, name, address);
            std::cout << "ok\t" << result << '\n';
        } catch (const std::exception &) {
            std::cout << "invalid\n";
        }
        image[offset] ^= uint8_t(mask);
    }
}
