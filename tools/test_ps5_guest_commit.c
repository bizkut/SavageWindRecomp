// Exercise PS5 guest allocation, partial decommit and VA reservation with host-side kernel substitutes.
#define _GNU_SOURCE
#include <ucontext.h>
#define __PROSPERO__ 1
#define PS5PLATFORM_CONTEXT_H
#define mc_rip gregs[REG_RIP]
#define mc_err gregs[REG_ERR]
#include "../runtime/platform/ps5.c"
#include <sys/mman.h>

static unsigned allocations, releases, reservations;
int sceKernelDebugOutText(int channel, const char *text) { (void)channel; (void)text; return 0; }
static int64_t last_released;
int64_t sceKernelGetDirectMemorySize(void) { return 1ll << 30; }
int32_t sceKernelAllocateDirectMemory(int64_t lo, int64_t hi, size_t len, size_t align, int type, int64_t *physical) {
    (void)lo; (void)hi; (void)len; (void)align; (void)type;
    *physical = (int64_t)++allocations * GUEST_UNIT;
    return 0;
}
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical,
                                 size_t alignment) {
    (void)protection; (void)flags; (void)physical; (void)alignment;
    void *p = mmap(*address, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return -1;
    memset(p, 0xa5, length);
    *address = p;
    return 0;
}
int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t length) {
    if (length != GUEST_UNIT) return -1;
    last_released = physical;
    releases++;
    return 0;
}
int32_t sceKernelMunmap(void *address, size_t length) { return munmap(address, length); }
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int flags, size_t align) {
    (void)flags; (void)align;
    void *p = mmap(*address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return -1;
    *address = p;
    reservations++;
    return 0;
}
int32_t sceKernelMprotect(const void *address, size_t length, int protection) {
    (void)address; (void)length; (void)protection;
    return 0;
}
static _Atomic int worker_started, worker_done;
static void *decommit_worker(void *arg) {
    atomic_store(&worker_started, 1);
    plat_decommit((uintptr_t)arg, CHUNK);
    atomic_store(&worker_done, 1);
    return NULL;
}
int main(void) {
    void *raw = mmap(NULL, CHUNK * 5, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return 1;
    uint64_t base = ((uintptr_t)raw + CHUNK - 1) & ~(CHUNK - 1);
    if (!plat_reserve_guest(base, base + CHUNK * 4)) return 2;
    plat_commit(base, GUEST_UNIT * 3);
    unsigned char *p = (void *)(uintptr_t)base;
    for (size_t i = 0; i < GUEST_UNIT * 3; i++) if (p[i]) return 3;
    memset(p, 0x5a, GUEST_UNIT * 3);
    plat_commit(base, GUEST_UNIT * 3);
    if (allocations != 3 || p[123] != 0x5a) return 4;
    // Round inward to 4 KiB, preserving neighbours while releasing the middle physical unit.
    plat_decommit(base + 0x800, GUEST_UNIT * 2 + 0x1000);
    if (releases != 1 || last_released != (int64_t)GUEST_UNIT * 2 || reservations != 2) return 5;
    if (p[0] != 0x5a || p[0xfff] != 0x5a || p[0x1000] || p[GUEST_UNIT * 2] ||
        p[GUEST_UNIT * 2 + 0x1000] != 0x5a) return 6;
    if (plat_guest_is_committed(base, GUEST_UNIT * 3) || !plat_guest_is_committed(base, GUEST_UNIT) ||
        !plat_guest_is_committed(base + CHUNK * 4, 0) || plat_guest_is_committed(base - 1, 0) ||
        plat_guest_is_committed(UINT64_MAX, 2)) return 7;
    plat_commit(base + GUEST_UNIT, GUEST_UNIT);
    if (allocations != 4 || !plat_guest_is_committed(base, GUEST_UNIT * 3)) return 8;
    for (size_t i = GUEST_UNIT; i < GUEST_UNIT * 2; i++) if (p[i]) return 9;
    // A later decommit completes the first chunk and releases it, despite split calls.
    plat_decommit(base, 0x1000);
    if (releases != 2 || last_released != (int64_t)GUEST_UNIT) return 10;
    plat_commit(base, GUEST_UNIT);
    if (allocations != 5) return 11;
    plat_decommit(base, GUEST_UNIT * 3);
    if (releases != 5 || plat_guest_is_committed(base, 1)) return 12;
    plat_commit(base + CHUNK * 3, CHUNK);
    plat_decommit(base + CHUNK * 3, CHUNK / 2);
    plat_commit(base + CHUNK * 3 + 4096, 4096);
    plat_decommit(base + CHUNK * 3 + CHUNK / 2, CHUNK / 2);
    if (releases != 5 || !plat_guest_is_committed(base + CHUNK * 3, 1)) return 13;
    plat_decommit(base + CHUNK * 3 + 4096, 4096);
    if (releases != 6 || plat_guest_is_committed(base + CHUNK * 3, 1)) return 14;
    // A checked copy holds this same lock while another thread tries to decommit its backing.
    plat_commit(base, CHUNK);
    plat_guest_access_lock();
    if (plat_protect(base, 4096, PLAT_PROT_RW)) return 15;  // Synchronous fault-watch reentry.
    pthread_t worker;
    if (pthread_create(&worker, NULL, decommit_worker, (void *)(uintptr_t)base)) return 16;
    while (!atomic_load(&worker_started)) {}
    if (atomic_load(&worker_done) || !plat_guest_is_committed(base, CHUNK)) return 17;
    plat_guest_access_unlock();
    if (pthread_join(worker, NULL) || !atomic_load(&worker_done) || plat_guest_is_committed(base, 1)) return 18;
    munmap(raw, CHUNK * 5);
    puts("PS5 guest decommit and recommit passed");
    return 0;
}
