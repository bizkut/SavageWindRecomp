// Run the package benchmark as an unelevated application title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>
#define PACKAGE_PROBE_TEMP "/app0/package-probe.txt"
#define PACKAGE_PROBE_RESULT "/app0/package-probe.txt"
#define main package_probe_main
#include "../importer/package_probe.cpp"
#undef main
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int main() {
    int result = package_probe_main();
    (void)result;
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sleep(1);
}
