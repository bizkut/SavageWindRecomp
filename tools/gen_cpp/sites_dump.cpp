// SPDX-License-Identifier: GPL-3.0-or-later
#define S9_PARSER_ONLY
#include "dump.cpp"
#include "sites.h"

int main(int argc, char **argv) try
{
    require(argc == 2, "usage: sites_dump MAIN");
    Nso nso(file(argv[1]));
    auto result = game_sites::for_module(nso, 0x7000000000ULL);
    std::map<std::string, std::string> kinds;
    for (const auto &site : game_sites::definitions)
        kinds[site.name] = site.kind;
    std::cout << "version " << result.version << '\n';
    for (const auto &entry : result.sites) {
        std::cout << "site " << entry.first << ' ' << kinds.at(entry.first) << ' ' << std::hex << entry.second
                  << std::dec << ' ' << result.fallback.at(entry.first) << '\n';
    }
    for (size_t i = 0; i < result.groups.size(); ++i)
        std::cout << "group " << i << ' ' << result.groups[i] << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
