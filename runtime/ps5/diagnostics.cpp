// M2 title diagnostics: report allocation pressure and abnormal termination.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../ps5/importer/package_exefs.h"
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>
#include <pthread.h>
#include <pthread_np.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <unistd.h>

static std::atomic<const char *> current_stage{"startup"};

static void memory_report(const char *stage, bool inspect_stack) {
    struct ps5_heap_stats heap{};
    ps5_heap_stats(&heap);
    size_t flex_free = 0, flex_total = 0, direct_free = 0;
    int flex_rc = sceKernelAvailableFlexibleMemorySize(&flex_free);
    int config_rc = sceKernelConfiguredFlexibleMemorySize(&flex_total);
    int64_t direct_total = sceKernelGetDirectMemorySize(), direct_start = 0;
    int direct_rc = sceKernelAvailableDirectMemorySize(0, direct_total, PS5_KERNEL_DIRECT_ALIGNMENT,
                                                       &direct_start, &direct_free);
    char line[512];
    std::snprintf(line, sizeof line,
                  "savagewind: memory stage=%s heap_base=%#llx mapped=%llu peak=%llu segments=%u arenas=%u "
                  "libc_fallbacks=%llu flex_free=%llu flex_total=%llu flex_rc=%d config_rc=%d "
                  "direct_largest=%llu direct_total=%lld direct_rc=%d\n",
                  stage, (unsigned long long)heap.range_base, (unsigned long long)heap.mapped_bytes,
                  (unsigned long long)heap.peak_bytes, heap.segments, heap.arenas, heap.libc_fallbacks,
                  (unsigned long long)flex_free, (unsigned long long)flex_total, flex_rc, config_rc,
                  (unsigned long long)direct_free, (long long)direct_total, direct_rc);
    sceKernelDebugOutText(0, line);
    if (!inspect_stack) return;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    void *base = nullptr;
    size_t size = 0;
    if (!rc) {
        rc = pthread_attr_get_np(pthread_self(), &attr);
        if (!rc) rc = pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
    }
    uintptr_t here = reinterpret_cast<uintptr_t>(&attr), lo = reinterpret_cast<uintptr_t>(base);
    size_t used = !rc && here >= lo && here - lo < size ? size - (here - lo) : 0;
    std::snprintf(line, sizeof line, "savagewind: stack stage=%s rc=%d base=%p size=%llu used=%llu\n", stage,
                  rc, base, (unsigned long long)size, (unsigned long long)used);
    sceKernelDebugOutText(0, line);
}

static void package_stage(const char *stage) {
    current_stage.store(stage, std::memory_order_relaxed);
    memory_report(stage, true);
}
static void out_of_memory() {
    sceKernelDebugOutText(0, "savagewind: C++ new_handler: allocation failed\n");
    memory_report(current_stage.load(std::memory_order_relaxed), false);
    std::fflush(stderr);
    throw std::bad_alloc();
}
static void terminated() noexcept {
    sceKernelDebugOutText(0, "savagewind: std::terminate invoked\n");
    memory_report(current_stage.load(std::memory_order_relaxed), false);
    std::fflush(stderr);
    std::abort();
}
static void at_exit() {
    sceKernelDebugOutText(0, "savagewind: atexit invoked\n");
    memory_report(current_stage.load(std::memory_order_relaxed), false);
    std::fflush(stderr);
}
__attribute__((constructor(101))) static void install_diagnostics() {
    std::set_terminate(terminated);
    std::set_new_handler(out_of_memory);
    if (std::atexit(at_exit)) sceKernelDebugOutText(0, "savagewind: atexit registration failed\n");
    savagewind_package_set_diagnostic(package_stage);
    sceKernelDebugOutText(0, "savagewind: termination and allocation diagnostics installed\n");
    memory_report("startup", true);
}

extern "C" {
[[noreturn]] void __real_exit(int);
[[noreturn]] void __real__exit(int);
[[noreturn]] void __real__Exit(int);
[[noreturn]] void __real_quick_exit(int);
[[noreturn]] void __real_abort(void);
int __real_sceSystemServiceLoadExec(const char *, const char *const *);

[[noreturn]] void __wrap_exit(int status) {
    char line[96]; std::snprintf(line, sizeof line, "savagewind: exit(%d)\n", status);
    sceKernelDebugOutText(0, line); std::fflush(stderr); __real_exit(status);
}
[[noreturn]] void __wrap__exit(int status) {
    char line[96]; std::snprintf(line, sizeof line, "savagewind: _exit(%d)\n", status);
    sceKernelDebugOutText(0, line); std::fflush(stderr); __real__exit(status);
}
[[noreturn]] void __wrap__Exit(int status) {
    char line[96]; std::snprintf(line, sizeof line, "savagewind: _Exit(%d)\n", status);
    sceKernelDebugOutText(0, line); std::fflush(stderr); __real__Exit(status);
}
[[noreturn]] void __wrap_quick_exit(int status) {
    char line[96]; std::snprintf(line, sizeof line, "savagewind: quick_exit(%d)\n", status);
    sceKernelDebugOutText(0, line); std::fflush(stderr); __real_quick_exit(status);
}
[[noreturn]] void __wrap_abort(void) {
    sceKernelDebugOutText(0, "savagewind: abort invoked\n");
    memory_report(current_stage.load(std::memory_order_relaxed), false);
    std::fflush(stderr); __real_abort();
}
int __wrap_sceSystemServiceLoadExec(const char *path, const char *const *argv) {
    sceKernelDebugOutText(0, "savagewind: sceSystemServiceLoadExec invoked\n");
    std::fflush(stderr);
    int rc = __real_sceSystemServiceLoadExec(path, argv);
    char line[96]; std::snprintf(line, sizeof line, "savagewind: sceSystemServiceLoadExec returned %#x\n", unsigned(rc));
    sceKernelDebugOutText(0, line);
    return rc;
}
}
