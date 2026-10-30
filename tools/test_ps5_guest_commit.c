// Verify that newly mapped PS5 guest chunks do not expose recycled direct-memory contents.
#define _GNU_SOURCE
#include <ucontext.h>
#define __PROSPERO__ 1
#define PS5PLATFORM_CONTEXT_H
#define mc_rip gregs[REG_RIP]
#define mc_err gregs[REG_ERR]
#include "../runtime/platform/ps5.c"
#include <sys/mman.h>

static unsigned allocations;
int64_t sceKernelGetDirectMemorySize(void) { return 1ll << 30; }
int32_t sceKernelAllocateDirectMemory(int64_t lo, int64_t hi, size_t len, size_t align, int type, int64_t *physical) {
    (void)lo; (void)hi; (void)len; (void)align; (void)type;
    *physical = 0;
    allocations++;
    return 0;
}
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical,
                                 size_t alignment) {
    (void)protection; (void)flags; (void)physical; (void)alignment;
    void *p = mmap(*address, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return -1;
    memset(p, 0xa5, length);  // Simulate direct memory released by another allocation.
    *address = p;
    return 0;
}
int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t length) {
    (void)physical; (void)length;
    return 0;
}
int main(void) {
    void *p = mmap(NULL, CHUNK * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    _Atomic unsigned char state = 0;
    if (!map_chunk((uintptr_t)p, &state)) return 2;
    const unsigned char *bytes = p;
    for (size_t i = 0; i < CHUNK; i++) if (bytes[i]) return 3;
    ((unsigned char *)p)[123] = 0x5a;
    if (!map_chunk((uintptr_t)p, &state) || allocations != 1 || bytes[123] != 0x5a) return 4;
    munmap(p, CHUNK * 2);
    puts("PS5 recycled guest chunk zeroing passed");
    return 0;
}
