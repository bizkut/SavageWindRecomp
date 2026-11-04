// Synchronized guest memory access for the PS5 control tools.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_memory.h"
#include "../platform.h"
#include "../rt.h"
#include <ps5platform/kernel.h>
#include <string.h>

#define CONTROL_UNIT ((uint64_t)PS5_KERNEL_DIRECT_ALIGNMENT)

// Returns the next protection span inside one tracked direct-memory unit, including inaccessible spans.
static void region_locked(uint64_t addr, uint64_t *end, bool *readable, bool *writable) {
    uint64_t unit_end = (addr & ~(CONTROL_UNIT - 1)) + CONTROL_UNIT;
    if (unit_end > GUEST_HI) unit_end = GUEST_HI;
    *end = unit_end;
    *readable = false;
    *writable = false;
    if (!plat_guest_is_committed(addr, 1)) return;
    void *start = NULL, *limit = NULL;
    uint32_t prot = 0;
    if (sceKernelQueryMemoryProtection((void *)(uintptr_t)addr, &start, &limit, &prot) ||
        (uint64_t)(uintptr_t)start > addr || (uint64_t)(uintptr_t)limit <= addr) return;
    if ((uint64_t)(uintptr_t)limit < *end) *end = (uint64_t)(uintptr_t)limit;
    *readable = (prot & PS5_KERNEL_PROT_CPU_READ) != 0;
    *writable = (prot & PS5_KERNEL_PROT_CPU_WRITE) != 0;
}

bool ps5_control_region(uint64_t addr, uint64_t *end, bool *readable, bool *writable) {
    if (addr < GUEST_LO || addr >= GUEST_HI) return false;
    plat_guest_access_lock();
    region_locked(addr, end, readable, writable);
    plat_guest_access_unlock();
    return true;
}

bool ps5_control_copy(uint64_t addr, void *buffer, size_t size, bool write) {
    if (addr < GUEST_LO || addr > GUEST_HI || size > GUEST_HI - addr) return false;
    if (!size) return true;
    if (!buffer) return false;
    plat_guest_access_lock();
    bool ok = true;
    uint64_t limit = addr + size;
    for (uint64_t p = addr; p < limit;) {
        uint64_t end;
        bool readable, writable;
        region_locked(p, &end, &readable, &writable);
        if (!readable || (write && !writable)) {
            ok = false;
            break;
        }
        p = end;
    }
    if (ok) {
        if (write) memcpy((void *)(uintptr_t)addr, buffer, size);
        else memcpy(buffer, (const void *)(uintptr_t)addr, size);
    }
    plat_guest_access_unlock();
    return ok;
}
