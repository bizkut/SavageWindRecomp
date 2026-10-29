// Armed S7 direct-memory executable image and cache experiment.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../platform.h"
#include "../storage_ps5.h"
#include <ps5platform/exec.h>
#include <ps5platform/kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB ((size_t)1024 * 1024)
#define CACHE "cache/s7-image.bin"
// Linked libgamecode text+rodata+data measured from title.map: 235425008 bytes.
#define DEFAULT_MIB 256
#define MAGIC 0x51472319u

static struct ps5_exec_region held;
static int runtime_export(void) { return MAGIC; }

static size_t memory(const char *stage) {
    int64_t total = sceKernelGetDirectMemorySize(), start = 0;
    size_t largest = 0, flex = 0, configured = 0;
    int dr = sceKernelAvailableDirectMemorySize(0, total, PS5_KERNEL_DIRECT_ALIGNMENT, &start, &largest);
    int fr = sceKernelAvailableFlexibleMemorySize(&flex);
    int cr = sceKernelConfiguredFlexibleMemorySize(&configured);
    fprintf(stderr, "[s7] memory %s direct_total=%lld direct_largest=%llu direct_start=%lld direct_rc=%#x flex_free=%llu flex_total=%llu flex_rc=%#x config_rc=%#x\n", stage, (long long)total, (unsigned long long)largest, (long long)start, (unsigned)dr, (unsigned long long)flex, (unsigned long long)configured, (unsigned)fr, (unsigned)cr);
    return largest;
}
static double ms(uint64_t start) { return (plat_now_ns() - start) / 1e6; }
static double rate(size_t bytes, double elapsed_ms) { return elapsed_ms > 0 ? bytes / (double)MIB * 1000 / elapsed_ms : 0; }
static void relocate(unsigned char *p) {
    // sub rsp,8; movabs rax, runtime_export; call rax; add rsp,8; ret
    static const unsigned char code[] = {0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    uintptr_t target = (uintptr_t)&runtime_export;
    memcpy(p, code, sizeof code);
    memcpy(p + 6, &target, sizeof target);
}
static void fill(struct ps5_exec_region *r, size_t mib) {
    unsigned char page[4096];
    for (size_t i = 0; i < sizeof page; i++) page[i] = (unsigned char)(i * 37u + (i >> 4));
    unsigned char *p = r->write_view;
    for (size_t off = 0; off < r->bytes; off += sizeof page) memcpy(p + off, page, sizeof page);
    for (size_t i = 0; i < mib; i++) {
        unsigned char *stub = p + i * MIB;
        uint32_t value = 0x10000000u + (uint32_t)i;
        memset(stub, 0x90, 4096);
        stub[0] = 0xb8; memcpy(stub + 1, &value, sizeof value); stub[5] = 0xc3;
    }
    relocate(p + 4096);
}
static int execute(struct ps5_exec_region *r, size_t mib, const char *phase) {
    uint64_t start = plat_now_ns();
    uint32_t first = ((uint32_t (*)(void))r->base)();
    double first_ms = ms(start);
    size_t errors = first != 0x10000000u;
    for (size_t i = 1; i < mib; i++) {
        uint32_t got = ((uint32_t (*)(void))((char *)r->base + i * MIB))();
        if (got != 0x10000000u + (uint32_t)i) errors++;
    }
    uint32_t relocated = ((uint32_t (*)(void))((char *)r->base + 4096))();
    if (relocated != MAGIC) errors++;
    fprintf(stderr, "[s7] %s first_ms=%.3f first=%#x relocation=%#x stubs=%llu errors=%llu\n", phase, first_ms, first, relocated, (unsigned long long)mib, (unsigned long long)errors);
    return errors == 0;
}
static int make_exec(struct ps5_exec_region *r) {
    if (!(r->flags & PS5_EXEC_TOGGLED)) return 0;
    int rc = ps5_exec_protect(r, 0, r->bytes, false);
    fprintf(stderr, "[s7] protect RX rc=%#x\n", (unsigned)rc);
    return rc;
}
struct result { int ok; double alloc_ms, fill_mbs, reload_mbs; };
static struct result run(const char *name, unsigned flags, size_t mib, int keep) {
    struct result result = {0};
    struct ps5_exec_region r = {0};
    struct ps5_exec_request req = { .bytes = mib * MIB, .anchor = (uintptr_t)&runtime_export, .flags = PS5_EXEC_NEAR | flags };
    uint64_t t = plat_now_ns();
    int rc = ps5_exec_alloc(&req, &r);
    result.alloc_ms = ms(t);
    fprintf(stderr, "[s7] mode=%s alloc rc=%#x base=%p write=%p bytes=%llu alloc_ms=%.3f\n", name, (unsigned)rc, r.base, r.write_view, (unsigned long long)r.bytes, result.alloc_ms);
    if (rc) goto done;
    t = plat_now_ns(); fill(&r, mib); double elapsed = ms(t);
    result.fill_mbs = rate(r.bytes, elapsed);
    fprintf(stderr, "[s7] mode=%s fill_ms=%.3f fill_mbs=%.2f\n", name, elapsed, result.fill_mbs);
    if (make_exec(&r) || !execute(&r, mib, "initial")) goto done;
    t = plat_now_ns();
    rc = rt_storage_mkdirs("cache");
    if (!rc) rc = rt_storage_replace(CACHE, r.write_view, r.bytes);
    fprintf(stderr, "[s7] mode=%s cache_write rc=%d ms=%.3f\n", name, rc, ms(t));
    if (rc) goto done;
    ps5_exec_free(&r);
    t = plat_now_ns(); rc = ps5_exec_alloc(&req, &r);
    fprintf(stderr, "[s7] mode=%s reload_alloc rc=%#x base=%p write=%p alloc_ms=%.3f\n", name, (unsigned)rc, r.base, r.write_view, ms(t));
    if (rc) goto done;
    t = plat_now_ns();
    ssize_t n = rt_storage_read(CACHE, r.write_view, r.bytes, 0);
    elapsed = ms(t); result.reload_mbs = rate(n > 0 ? (size_t)n : 0, elapsed);
    fprintf(stderr, "[s7] mode=%s reload bytes=%lld ms=%.3f reload_mbs=%.2f\n", name, (long long)n, elapsed, result.reload_mbs);
    if (n != (ssize_t)r.bytes) goto done;
    relocate((unsigned char *)r.write_view + 4096);
    if (make_exec(&r) || !execute(&r, mib, "reload")) goto done;
    result.ok = 1;
    if (keep) { held = r; memset(&r, 0, sizeof r); fprintf(stderr, "[s7] holding %llu MiB (%s)\n", (unsigned long long)mib, name); }
done:
    ps5_exec_free(&r);
    return result;
}
void ps5_s7_run(void) {
    FILE *f = fopen("/data/savagewind/s7.txt", "r");
    if (!f) return;
    char line[128]; size_t mib = DEFAULT_MIB;
    while (fgets(line, sizeof line, f)) {
        unsigned long long value; char extra;
        if (sscanf(line, " mib = %llu %c", &value, &extra) == 1 && value && value <= SIZE_MAX / MIB && value <= UINT32_MAX - 0x10000000u) mib = (size_t)value;
    }
    fclose(f);
    struct ps5_kernel_sw_version version = { .size = sizeof version };
    int vr = sceKernelGetSystemSwVersion(&version); version.text[sizeof version.text - 1] = 0;
    fprintf(stderr, "[s7] firmware %s rc=%#x version=%#x uid=%u euid=%u (identity only; jailbreak state unknown)\n", vr ? "unknown" : version.text, (unsigned)vr, version.version, (unsigned)getuid(), (unsigned)geteuid());
    memory("before");
    static const struct { const char *name; unsigned flags; } modes[] = {{"dual", PS5_EXEC_DUAL_VIEW}, {"rx", PS5_EXEC_TOGGLED}, {"rwx", 0}};
    int winner = -1;
    for (int i = 0; i < 3; i++) {
        struct result r = run(modes[i].name, modes[i].flags, mib, 0);
        size_t free_bytes = memory("after_mode");
        fprintf(stderr, "[s7] summary mode=%s ok=%d alloc_ms=%.3f fill_mbs=%.2f reload_mbs=%.2f direct_free_after=%llu\n", modes[i].name, r.ok, r.alloc_ms, r.fill_mbs, r.reload_mbs, (unsigned long long)free_bytes);
        if (r.ok && winner < 0) winner = i;
    }
    if (winner >= 0) {
        struct result r = run(modes[winner].name, modes[winner].flags, mib, 1);
        size_t free_bytes = memory("after_hold");
        fprintf(stderr, "[s7] summary mode=%s ok=%d alloc_ms=%.3f fill_mbs=%.2f reload_mbs=%.2f direct_free_after=%llu\n", modes[winner].name, r.ok, r.alloc_ms, r.fill_mbs, r.reload_mbs, (unsigned long long)free_bytes);
    }
}
