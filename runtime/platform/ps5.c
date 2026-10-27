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
#define STACK_BYTES (256ull << 20)
#define MAX_STACKS 256
#define MAX_GUEST_CHUNKS (16 * 1024)

typedef struct {
    uint64_t lo;
    _Atomic unsigned char chunks[STACK_BYTES / CHUNK];
} StackRange;

static uint64_t guest_lo, guest_hi;
static _Atomic unsigned char guest_chunks[MAX_GUEST_CHUNKS];
static StackRange stacks[MAX_STACKS];
static _Atomic unsigned stack_count;
static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;

// Each state is 0 (unmapped), 1 (mapping), or 2 (mapped). This also prevents
// concurrent faults or explicit commits from replacing an existing mapping.
static bool map_chunk(uint64_t address, _Atomic unsigned char *state) {
    unsigned char expected = 0;
    if (!atomic_compare_exchange_strong(state, &expected, 1)) {
        while (atomic_load(state) == 1) {}
        return atomic_load(state) == 2;
    }
    int64_t physical = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), CHUNK,
                                           PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    if (!rc) {
        void *p = (void *)(uintptr_t)address;
        rc = sceKernelMapDirectMemory(&p, CHUNK, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, physical, 0);
        if (rc || p != (void *)(uintptr_t)address) {
            sceKernelReleaseDirectMemory(physical, CHUNK);
            rc = rc ? rc : -1;
        }
    }
    atomic_store(state, rc ? 0 : 2);
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
    uint64_t end = (addr + size + CHUNK - 1) & ~(CHUNK - 1);
    for (uint64_t p = addr & ~(CHUNK - 1); p < end; p += CHUNK) {
        if (!map_chunk(p, &guest_chunks[(p - guest_lo) / CHUNK])) {
            fprintf(stderr, "PS5: guest direct-memory allocation failed at %llx\n", (unsigned long long)p);
            abort();
        }
    }
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    // In-process reboot is not implemented in the M2 title.
    (void)lo; (void)hi;
    fprintf(stderr, "PS5: guest reset is unavailable\n");
    return false;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? PS5_KERNEL_PROT_CPU_READ :
            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    return sceKernelMprotect((void *)(uintptr_t)addr, size, p);
}

int plat_guest_thread_create(pthread_t *thread, void *(*entry)(void *), void *arg) {
    pthread_mutex_lock(&stack_lock);
    unsigned n = atomic_load(&stack_count);
    if (n == MAX_STACKS) { pthread_mutex_unlock(&stack_lock); return EAGAIN; }
    void *base = NULL;
    int rc = sceKernelReserveVirtualRange(&base, STACK_BYTES, 0, STACK_BYTES);
    if (rc) { pthread_mutex_unlock(&stack_lock); return EAGAIN; }
    StackRange *s = &stacks[n];
    s->lo = (uintptr_t)base;
    if (!map_chunk(s->lo + STACK_BYTES - CHUNK, &s->chunks[STACK_BYTES / CHUNK - 1])) {
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
        _Atomic unsigned char *state = &guest_chunks[(chunk - guest_lo) / CHUNK];
        if (atomic_load(state) != 2 && map_chunk(chunk, state)) return;
    }
    unsigned count = atomic_load(&stack_count);
    for (unsigned i = 0; i < count; i++) {
        StackRange *s = &stacks[i];
        if ((sig == SIGSEGV || sig == SIGBUS) && a >= s->lo && a < s->lo + STACK_BYTES) {
            _Atomic unsigned char *state = &s->chunks[(chunk - s->lo) / CHUNK];
            if (atomic_load(state) != 2 && map_chunk(chunk, state)) return;
            break;
        }
    }
    ucontext_t *uc = context;
    uint64_t pc = uc->uc_mcontext.mc_rip;
    g_fault_is_write = !!(uc->uc_mcontext.mc_err & 2);
    if (rt_fault_watch(a, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
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
void plat_thread_realtime(uint64_t period_ns) { (void)period_ns; }
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
