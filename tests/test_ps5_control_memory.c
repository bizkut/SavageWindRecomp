// Host test for PS5 control memory bounds, protection and cross-unit access.
#include "../runtime/ps5/control_memory.h"
#include "../runtime/rt.h"
#include <assert.h>
#include <pthread.h>
#include <ps5platform/kernel.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define UNIT PS5_KERNEL_DIRECT_ALIGNMENT
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool committed[2] = {true, true};
static uint32_t protection[2] = {PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                 PS5_KERNEL_PROT_CPU_READ};
void plat_guest_access_lock(void) { pthread_mutex_lock(&lock); }
void plat_guest_access_unlock(void) { pthread_mutex_unlock(&lock); }
bool plat_guest_is_committed(uint64_t addr, size_t size) {
    if (addr < GUEST_LO || addr >= GUEST_LO + 2 * UNIT || size > GUEST_LO + 2 * UNIT - addr) return false;
    for (uint64_t a = addr; a < addr + size;) {
        if (!committed[(a - GUEST_LO) / UNIT]) return false;
        a = ((a - GUEST_LO) / UNIT + 1) * UNIT + GUEST_LO;
    }
    return true;
}
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *prot) {
    uint64_t a = (uint64_t)(uintptr_t)address;
    if (a < GUEST_LO || a >= GUEST_LO + 2 * UNIT) return -1;
    size_t i = (a - GUEST_LO) / UNIT;
    *start = (void *)(uintptr_t)(GUEST_LO + i * UNIT);
    *end = (void *)(uintptr_t)(GUEST_LO + (i + 1) * UNIT);
    *prot = protection[i];
    return 0;
}
int main(void) {
    void *p = mmap((void *)(uintptr_t)GUEST_LO, 2 * UNIT, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(p == (void *)(uintptr_t)GUEST_LO);
    unsigned char value[4] = {1, 2, 3, 4}, out[4] = {0};
    assert(ps5_control_copy(GUEST_LO, value, sizeof value, true));
    assert(ps5_control_copy(GUEST_LO, out, sizeof out, false));
    assert(!memcmp(value, out, sizeof out));
    assert(!ps5_control_copy(GUEST_LO - 1, out, 1, false));
    assert(!ps5_control_copy(GUEST_HI - 1, out, 2, false));
    assert(!ps5_control_copy(UINT64_MAX, out, sizeof out, false));
    assert(ps5_control_copy(GUEST_HI, out, 0, false));
    assert(ps5_control_copy(GUEST_LO + UNIT - 2, out, 4, false));
    assert(!ps5_control_copy(GUEST_LO + UNIT - 2, value, 4, true));
    committed[1] = false;
    assert(!ps5_control_copy(GUEST_LO + UNIT - 2, out, 4, false));
    uint64_t end;
    bool readable, writable;
    assert(ps5_control_region(GUEST_LO + UNIT, &end, &readable, &writable));
    assert(end == GUEST_LO + 2 * UNIT && !readable && !writable);
    assert(!munmap(p, 2 * UNIT));
    return 0;
}
