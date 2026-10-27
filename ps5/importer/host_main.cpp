// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

int savagewind_import(const char *root);

int main(int argc, char **argv) {
    if (argc != 3 || std::strcmp(argv[1], "--root") != 0) {
        std::fputs("savagewind-import: session=- step=arguments errno=0 type=invalid_request "
                   "what=Expected --root <path>\n",
                   stderr);
        return 2;
    }
    return savagewind_import(argv[2]);
}
