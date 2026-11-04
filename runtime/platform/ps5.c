// PS5 host layer (runtime/platform.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/context.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include "../platform.h"


#define CHUNK (1ull << 20)
// Guest and stack backing retain the original 1 MiB direct-memory mapping granularity.
#define GUEST_UNIT CHUNK
#define STACK_BYTES (256ull << 20)
#define MAX_STACKS 256
// Guest stacks are reserved at fixed addresses in [STACK_AREA, STACK_AREA + MAX_STACKS * STACK_BYTES): 64 GiB below
// the platform heap (0x2000000000), above where the kernel places mappings given no address. Left to the kernel,
// 256 MiB reservations filled RADV's 4 GiB shader address window [0x200000000, 0x300000000), where every descriptor
// pool, push constant and shader buffer must go (PS5_Mesa radv_ps5_platform.h), and descriptor pools failed with
// VK_ERROR_OUT_OF_DEVICE_MEMORY (ps5/evidence/m3/run2.klog: a guest stack at 0x210000000).
#define STACK_AREA 0x1000000000ull
#define MAX_GUEST_CHUNKS (16 * 1024)

typedef struct {
    _Atomic unsigned char state;  // 0 reserved, 1 changing, 2 mapped
    int64_t physical;
} DirectUnit;

typedef struct {
    uint64_t lo;
    DirectUnit chunks[STACK_BYTES / CHUNK];
} StackRange;

static uint64_t guest_lo, guest_hi;
static DirectUnit guest_units[MAX_GUEST_CHUNKS];
// Bits set by decommit, cleared on explicit recommit; protected by guest_access_lock.
static uint64_t guest_freed[MAX_GUEST_CHUNKS][CHUNK / 4096 / 64];
static StackRange stacks[MAX_STACKS];
static _Atomic unsigned stack_count;
static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t guest_access_lock = PTHREAD_MUTEX_INITIALIZER;
void plat_guest_access_lock(void) { pthread_mutex_lock(&guest_access_lock); }
void plat_guest_access_unlock(void) { pthread_mutex_unlock(&guest_access_lock); }

// The state serializes a fault, explicit commit and decommit of the same allocation.
static bool map_chunk(uint64_t address, uint64_t size, DirectUnit *unit) {
    unsigned char expected = 0;
    while (!atomic_compare_exchange_weak(&unit->state, &expected, 1)) {
        if (expected == 2) return true;
        expected = 0;
    }
    int64_t physical = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                           PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    if (!rc) {
        void *p = (void *)(uintptr_t)address;
        rc = sceKernelMapDirectMemory(&p, size, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, physical, 0);
        if (rc || p != (void *)(uintptr_t)address) {
            sceKernelReleaseDirectMemory(physical, size);
            rc = rc ? rc : -1;
        }
        if (!rc) {
            memset(p, 0, size);  // Direct memory can contain data from a released allocation.
            unit->physical = physical;
        }
    }
    atomic_store(&unit->state, rc ? 0 : 2);
    return rc == 0;
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    if (lo >= hi || (lo & (CHUNK - 1)) || (hi & (CHUNK - 1)) ||
        (hi - lo) / CHUNK > MAX_GUEST_CHUNKS) return false;
    void *p = (void *)(uintptr_t)lo;
    int rc = sceKernelReserveVirtualRange(&p, hi - lo, PS5_KERNEL_MAP_FIXED, 0);
    if (rc || p != (void *)(uintptr_t)lo) return false;
    guest_lo = lo;
    guest_hi = hi;
    char line[160];
    snprintf(line, sizeof line, "savagewind: guest reserved %#llx-%#llx\n",
             (unsigned long long)lo, (unsigned long long)hi);
    sceKernelDebugOutText(0, line);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    if (addr < guest_lo || addr >= guest_hi || size > guest_hi - addr) {
        fprintf(stderr, "PS5: invalid guest commit\n");
        abort();
    }
    plat_guest_access_lock();
    uint64_t end = (addr + size + GUEST_UNIT - 1) & ~(GUEST_UNIT - 1);
    for (uint64_t p = addr & ~(GUEST_UNIT - 1); p < end; p += GUEST_UNIT) {
        if (!map_chunk(p, GUEST_UNIT, &guest_units[(p - guest_lo) / GUEST_UNIT])) {
            fprintf(stderr, "PS5: guest direct-memory allocation failed at %llx\n", (unsigned long long)p);
            abort();
        }
        uint64_t start = addr > p ? addr : p;
        uint64_t stop = addr + size < p + GUEST_UNIT ? addr + size : p + GUEST_UNIT;
        uint64_t *freed = guest_freed[(p - guest_lo) / GUEST_UNIT];
        unsigned first = (unsigned)((start - p) / 4096);
        unsigned last = (unsigned)((stop - p + 4095) / 4096);
        for (unsigned i = first; i < last; i++) freed[i / 64] &= ~(1ull << (i % 64));
    }
    plat_guest_access_unlock();
}

// The upstream contract rounds inward to 4 KiB pages. Keep a bitmap of decommitted pages in each 1 MiB mapping;
// release its direct memory once every page has been decommitted. Partial chunks are zeroed and stay backed.
void plat_decommit(uint64_t addr, uint64_t size) {
    if (!size) return;
    if (addr < guest_lo || addr >= guest_hi || size > guest_hi - addr) {
        fprintf(stderr, "PS5: invalid guest decommit\n");
        abort();
    }
    uint64_t a = (addr + 0xfff) & ~0xfffull, end = (addr + size) & ~0xfffull;
    plat_guest_access_lock();
    while (a < end) {
        uint64_t base = a & ~(GUEST_UNIT - 1);
        uint64_t stop = base + GUEST_UNIT < end ? base + GUEST_UNIT : end;
        DirectUnit *u = &guest_units[(base - guest_lo) / GUEST_UNIT];
        uint64_t *freed = guest_freed[(base - guest_lo) / GUEST_UNIT];
        unsigned char state;
        do {
            state = atomic_load(&u->state);
            if (state == 0) break;
        } while (state == 1 || !atomic_compare_exchange_weak(&u->state, &state, 1));
        if (state == 2) {
            unsigned first = (unsigned)((a - base) / 4096), last = (unsigned)((stop - base) / 4096);
            for (unsigned i = first; i < last; i++) freed[i / 64] |= 1ull << (i % 64);
            bool all_freed = true;
            for (unsigned i = 0; i < CHUNK / 4096 / 64; i++)
                if (freed[i] != UINT64_MAX) { all_freed = false; break; }
            if (all_freed) {
                void *p = (void *)(uintptr_t)base;
                int rc = sceKernelMunmap(p, GUEST_UNIT);
                if (!rc) rc = sceKernelReleaseDirectMemory(u->physical, GUEST_UNIT);
                if (!rc) rc = sceKernelReserveVirtualRange(&p, GUEST_UNIT, PS5_KERNEL_MAP_FIXED, 0);
                if (rc || p != (void *)(uintptr_t)base) {
                    fprintf(stderr, "PS5: guest decommit failed at %llx (%#x)\n", (unsigned long long)base, rc);
                    abort();
                }
                memset(freed, 0, sizeof guest_freed[0]);
                atomic_store(&u->state, 0);
            } else {
                memset((void *)(uintptr_t)a, 0, stop - a);
                atomic_store(&u->state, 2);
            }
        }
        a = stop;
    }
    plat_guest_access_unlock();
}

bool plat_guest_is_committed(uint64_t addr, size_t size) {
    if (addr < guest_lo || addr > guest_hi || size > guest_hi - addr) return false;
    if (!size) return true;
    uint64_t end = addr + size;
    for (uint64_t p = addr & ~(GUEST_UNIT - 1); p < end; p += GUEST_UNIT)
        if (atomic_load(&guest_units[(p - guest_lo) / GUEST_UNIT].state) != 2) return false;
    return true;
}

// Unsupported write watch disables the Vulkan heap copy, which is unnecessary on PS5 unified memory.
long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, size_t max) {
    (void)addr; (void)size; (void)pages; (void)max;
    return -1;
}
int plat_any_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; return -1; }

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    // In-process reboot is not implemented in the M2 title.
    (void)lo; (void)hi;
    fprintf(stderr, "PS5: guest reset is unavailable\n");
    return false;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? PS5_KERNEL_PROT_CPU_READ :
            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    plat_guest_access_lock();
    int rc = sceKernelMprotect((void *)(uintptr_t)addr, size, p);
    plat_guest_access_unlock();
    return rc;
}

int plat_guest_thread_create(pthread_t *thread, void *(*entry)(void *), void *arg) {
    pthread_mutex_lock(&stack_lock);
    unsigned n = atomic_load(&stack_count);
    if (n == MAX_STACKS) { pthread_mutex_unlock(&stack_lock); return EAGAIN; }
    void *base = (void *)(uintptr_t)(STACK_AREA + (uint64_t)n * STACK_BYTES);
    int rc = sceKernelReserveVirtualRange(&base, STACK_BYTES, PS5_KERNEL_MAP_FIXED, 0);
    if (rc || base != (void *)(uintptr_t)(STACK_AREA + (uint64_t)n * STACK_BYTES)) {
        ps5_logf("[ps5] guest stack %u: reserving %#llx failed (%d)\n", n,
                 (unsigned long long)(STACK_AREA + (uint64_t)n * STACK_BYTES), rc);
        pthread_mutex_unlock(&stack_lock);
        return EAGAIN;
    }
    StackRange *s = &stacks[n];
    s->lo = (uintptr_t)base;
    if (!map_chunk(s->lo + STACK_BYTES - CHUNK, CHUNK, &s->chunks[STACK_BYTES / CHUNK - 1])) {
        sceKernelMunmap(base, STACK_BYTES);
        s->lo = 0;
        pthread_mutex_unlock(&stack_lock);
        return ENOMEM;
    }
    atomic_store(&stack_count, n + 1);
    pthread_attr_t attr;
    rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstack(&attr, base, STACK_BYTES);
        if (!rc) rc = pthread_create(thread, &attr, entry, arg);
        pthread_attr_destroy(&attr);
    }
    // A published range is kept until process exit: a live signal handler may
    // be reading the registry. M2 has no in-process reboot or thread recycling.
    pthread_mutex_unlock(&stack_lock);
    return rc;
}

static void crash_handler(int sig, siginfo_t *si, void *context) {
    uint64_t a = (uintptr_t)si->si_addr;
    uint64_t chunk = a & ~(CHUNK - 1);
    if ((sig == SIGSEGV || sig == SIGBUS) && a >= guest_lo && a < guest_hi) {
        uint64_t base = a & ~(GUEST_UNIT - 1);
        DirectUnit *unit = &guest_units[(base - guest_lo) / GUEST_UNIT];
        if (atomic_load(&unit->state) != 2 && map_chunk(base, GUEST_UNIT, unit)) return;
    }
    unsigned count = atomic_load(&stack_count);
    for (unsigned i = 0; i < count; i++) {
        StackRange *s = &stacks[i];
        if ((sig == SIGSEGV || sig == SIGBUS) && a >= s->lo && a < s->lo + STACK_BYTES) {
            DirectUnit *unit = &s->chunks[(chunk - s->lo) / CHUNK];
            if (atomic_load(&unit->state) != 2 && map_chunk(chunk, CHUNK, unit)) return;
            break;
        }
    }
    ucontext_t *uc = context;
    uint64_t pc = uc->uc_mcontext.mc_rip;
    g_fault_is_write = !!(uc->uc_mcontext.mc_err & 2);
    if (rt_fault_watch(a, pc)) return;
    char what[64];  // (si_code: SEGV_MAPERR 1 / SEGV_ACCERR 2; err: the CPU's page-fault error code, bit 1 a write)
    snprintf(what, sizeof what, "signal %d (si_code %d, err %#llx)", sig, si->si_code,
             (unsigned long long)uc->uc_mcontext.mc_err);
    rt_fault_report(what, a, pc, 0);
}

void plat_crash_handler_install(void) {
    static __thread unsigned char alt_stack[64 * 1024];
    stack_t ss = {.ss_sp = alt_stack, .ss_size = sizeof alt_stack};
    if (sigaltstack(&ss, NULL)) abort();
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) || sigaction(SIGBUS, &sa, NULL) ||
        sigaction(SIGILL, &sa, NULL) || sigaction(SIGFPE, &sa, NULL)) abort();
}

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = t / 1000000000ull, .tv_nsec = t % 1000000000ull};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
void plat_wait_until_ns(uint64_t t) { plat_sleep_until_ns(t); }  // Absolute OS wait, no spin.

int plat_cpu_count(void) {
    uint64_t mask = 0;
    if (!scePthreadGetaffinity(pthread_self(), &mask)) {
        int n = __builtin_popcountll(mask);
        if (n) return n;
    }
    // Observed title affinity is 0x1fff (13 logical CPUs), but do not claim that if the query failed.
    return 1;
}
void plat_cpu_placement(void) {
    // Preserve the title's kernel-provided affinity; physical-core topology is not exposed here.
    const char *one = getenv("SAVAGEWIND_ONE_PER_CORE");
    if (one && atoi(one))
        fprintf(stderr, "[cpu] one-per-core placement unavailable on PS5; keeping the title affinity\n");
}
void plat_thread_realtime(uint64_t period_ns) { (void)period_ns; }
uint64_t plat_thread_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_init(void) {}
bool plat_exe_path(char *buf, size_t size) { (void)buf; (void)size; return false; }
void plat_exec(char *const argv[]) { (void)argv; fprintf(stderr, "PS5: exec is unavailable\n"); }
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv; (void)log_path; (void)append_log;
    fprintf(stderr, "PS5: child processes are unavailable\n");
    return 0;
}
int plat_proc_wait(PlatProc p, bool block) { (void)p; (void)block; return 255; }
void plat_proc_kill(PlatProc p) { (void)p; fprintf(stderr, "PS5: child processes are unavailable\n"); abort(); }
bool plat_remove_tree(const char *dir) { (void)dir; return false; }
const char *plat_symbol(uint64_t pc, uint64_t *offset) { (void)pc; (void)offset; return NULL; }
void plat_print_backtrace(void) { ps5_logf("PS5: backtrace unavailable\n"); }
void plat_fatal_notice(void) {}
