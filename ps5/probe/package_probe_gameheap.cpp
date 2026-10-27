// Package probe with the M2 title's guest reservation and platform allocator.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>
#include "../../runtime/rt.h"
#define PACKAGE_PROBE_TEMP "/app0/package-probe-gameheap.txt"
#define PACKAGE_PROBE_RESULT "/app0/package-probe-gameheap.txt"
#define main package_probe_main
#include "../importer/package_probe.cpp"
#undef main
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int main() {
    sceKernelDebugOutText(0, "savagewind-package-probe: game heap variant reserving guest window\n");
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) package_probe_main();
    else sceKernelDebugOutText(0, "savagewind-package-probe: done: guest reserve failed\n");
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sleep(1);
}
