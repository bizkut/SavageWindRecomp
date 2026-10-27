/*
 * Savage Wind PS5 - M0 feasibility probe (docs/PS5-PORT-PLAN.md, spikes S1, S2, S3 and S6).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One run answers, with numbers, what the runtime's platform layer needs from the console:
 *   S1  the guest window [0x7000000000, 0x7320000000) (runtime/rt.h) reserved at its fixed address, pages committed,
 *       protected and reset inside it (runtime/platform.h's plat_reserve/commit/protect/reset), page granularity;
 *   S2  guest threads on 256 MiB host stacks (runtime/kernel.c), and a write fault on a protected page caught by a
 *       SIGSEGV handler that unprotects it and resumes (lazy commit / watchpoints, runtime/dispatch.c);
 *   S3  an image with a large .text (m0_bigtext.S: PS5_BIGTEXT_MB) loads and its code runs; the eboot's load time;
 *   S6  CPU timings to set against the same binary built for the PC (ps5/tools/build-probe.sh host).
 *
 * Every line goes to stdout and to <title folder>/m0_probe.txt (PC: ./m0_probe.txt). A step that fails says so
 * and the probe goes on, so one run reports every spike.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define GUEST_LO 0x7000000000ull
#define GUEST_HI 0x7320000000ull
#define GIB (1024ull * 1024 * 1024)
#define MIB (1024ull * 1024)

#ifdef __PROSPERO__
#define LOG_PATH "/app0/m0_probe.txt"
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
#else
#define LOG_PATH "m0_probe.txt"
#endif

static FILE *g_log;
static int g_fail;

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("m0: %s\n", line);
    fflush(stdout);
    if (g_log) fprintf(g_log, "%s\n", line), fflush(g_log);
}

static void verdict(const char *spike, int ok, const char *what) {
    say("%s %s: %s", spike, ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- S1: the fixed guest window ------------------------------------------------------------------------------ */

/* Where the window could go instead: which fixed bases accept a 16 MiB mapping, and the largest region the kernel
 * places itself (a relocated guest base, docs/PS5-PORT-PLAN.md S1). */
static void s1_survey(void) {
    for (uint64_t at = 0x0100000000ull; at <= 0x7f00000000ull; at += 0x0800000000ull) {
        void *p = mmap((void *)at, 16 * MIB, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        int e = errno;
        say("S1 survey fixed 16 MiB at %#llx -> %s errno=%d", (unsigned long long)at,
            p == (void *)at ? "ok" : "refused", p == (void *)at ? 0 : e);
        if (p == (void *)at) munmap(p, 16 * MIB);
    }
    for (uint64_t gib = 16; gib >= 1; gib--) {
        void *p = mmap(NULL, gib * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            say("S1 survey largest kernel-placed PROT_NONE region: %llu GiB at %p", (unsigned long long)gib, p);
            int rc = mprotect(p, 64 * MIB, PROT_READ | PROT_WRITE);
            if (rc == 0) ((volatile uint8_t *)p)[0] = 1;
            say("S1 survey commit 64 MiB inside it: rc=%d errno=%d", rc, rc ? errno : 0);
            munmap(p, gib * GIB);
            return;
        }
    }
    say("S1 survey: no PROT_NONE region of 1 GiB or more, errno=%d", errno);
}

#ifdef __PROSPERO__
#include <ps5platform/kernel.h>

/* The console's way to own a large fixed address range: reserve the addresses only (sceKernelReserveVirtualRange),
 * then back the parts in use with direct memory mapped at fixed addresses inside it. Anonymous mmap draws on the
 * small flexible budget, which the survey above exhausts below 1 GiB. */
static void s1_direct_window(void) {
    size_t flex = 0, flex_cfg = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    sceKernelConfiguredFlexibleMemorySize(&flex_cfg);
    int64_t dstart = 0;
    size_t davail = 0;
    int64_t dtotal = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, dtotal, PS5_KERNEL_DIRECT_ALIGNMENT, &dstart, &davail);
    say("S1 memory: flexible available %zu MiB of %zu MiB configured; direct %lld MiB, largest free %zu MiB",
        flex / MIB, flex_cfg / MIB, (long long)(dtotal / (int64_t)MIB), davail / MIB);

    void *at = (void *)GUEST_LO;
    int rc = sceKernelReserveVirtualRange(&at, GUEST_HI - GUEST_LO, PS5_KERNEL_MAP_FIXED, 0);
    say("S1 reserve virtual range %#llx +%llu GiB fixed -> rc=%#x at %p", GUEST_LO,
        (unsigned long long)((GUEST_HI - GUEST_LO) / GIB), (unsigned)rc, at);
    verdict("S1", rc == 0 && at == (void *)GUEST_LO, "guest window reserved (addresses only) at its fixed address");
    if (rc || at != (void *)GUEST_LO) return;

    static const struct { uint64_t at, size; const char *what; } regions[] = {
        {0x7100000000ull, 256 * MIB, "module image"},
        {0x7092000000ull, 64 * MIB, "HLE heap"},
        {0x7240000000ull, 3 * GIB, "guest heap"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof regions / sizeof *regions; i++) {
        uint64_t t0 = now_ns();
        int64_t phys = 0;
        int a = sceKernelAllocateDirectMemory(0, dtotal, regions[i].size, PS5_KERNEL_DIRECT_ALIGNMENT,
                                              PS5_KERNEL_DIRECT_TYPE_CPU, &phys);
        void *va = (void *)regions[i].at;
        int m = a ? -1
                  : sceKernelMapDirectMemory(&va, regions[i].size, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                             PS5_KERNEL_MAP_FIXED, phys, 0);
        uint64_t touched = 0;
        if (!a && !m && va == (void *)regions[i].at)
            for (uint64_t off = 0; off < regions[i].size; off += 16 * 1024, touched++)
                ((volatile uint8_t *)va)[off] = (uint8_t)off;
        say("S1 direct commit %s %llu MiB at %#llx: alloc=%#x map=%#x at %p touched=%llu %.1f ms", regions[i].what,
            (unsigned long long)(regions[i].size / MIB), (unsigned long long)regions[i].at, (unsigned)a,
            (unsigned)m, va, (unsigned long long)touched, (now_ns() - t0) / 1e6);
        ok &= !a && !m && va == (void *)regions[i].at;
    }
    verdict("S1", ok, "module image, HLE heap and 3 GiB guest heap backed by direct memory at their addresses");
    if (!ok) return;

    uint8_t *p = (uint8_t *)0x7100000000ull;
    int r16 = sceKernelMprotect(p + 16384, 16384, PS5_KERNEL_PROT_CPU_READ);
    int r4 = sceKernelMprotect(p + 65536 + 4096, 4096, PS5_KERNEL_PROT_CPU_READ);
    say("S1 sceKernelMprotect 16 KiB rc=%#x, 4 KiB rc=%#x", (unsigned)r16, (unsigned)r4);
    verdict("S1", r16 == 0, "16 KiB protection inside the window");
    sceKernelMprotect(p, 256 * 1024, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);

    /* Reset: the runtime zeroes the guest on an in-process reboot; time a 256 MiB clear */
    uint64_t t0 = now_ns();
    memset(p, 0, 256 * MIB);
    say("S1 reset: zeroing 256 MiB took %.1f ms", (now_ns() - t0) / 1e6);
}
#endif

#ifdef __PROSPERO__
#define S2_THREADS 48
static void *s2_thread(void *arg);
/* S2 on the console: guest stacks the runtime owns. Each is 256 MiB of reserved addresses (no memory); only the top
 * 1 MiB is backed at start, and a fault below it maps another 1 MiB of direct memory there and resumes, on an
 * alternate signal stack (the faulting stack has no room). Physical cost is what each thread actually used. */
#define LAZY_STACK (256 * MIB)
#define LAZY_STEP (1 * MIB)
static uint8_t *g_lazy_lo;
static uint64_t g_lazy_span;
static volatile int g_lazy_maps, g_lazy_fail;

static void lazy_on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig, (void)ctx;
    uint8_t *a = info->si_addr;
    if (a >= g_lazy_lo && a < g_lazy_lo + g_lazy_span) {
        uint8_t *chunk = (uint8_t *)((uintptr_t)a & ~(uintptr_t)(LAZY_STEP - 1));
        int64_t phys = 0;
        void *va = chunk;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), LAZY_STEP, PS5_KERNEL_DIRECT_ALIGNMENT,
                                          PS5_KERNEL_DIRECT_TYPE_CPU, &phys) == 0 &&
            sceKernelMapDirectMemory(&va, LAZY_STEP, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                     PS5_KERNEL_MAP_FIXED, phys, 0) == 0 && va == chunk) {
            __atomic_add_fetch(&g_lazy_maps, 1, __ATOMIC_RELAXED);
            return;
        }
        g_lazy_fail = 1;
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

static volatile int g_lazy_on_own_stack;

static void *lazy_thread(void *arg) {
    static __thread uint8_t alt[64 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    uint8_t here;
    if (&here >= g_lazy_lo && &here < g_lazy_lo + g_lazy_span)
        __atomic_add_fetch(&g_lazy_on_own_stack, 1, __ATOMIC_RELAXED);
    /* Walk 8 MiB down this thread's own stack, below the backed top 1 MiB: each new MiB must fault and be mapped */
    size_t flex_before = 0, flex_after = 0;
    if ((uintptr_t)arg == 1) sceKernelAvailableFlexibleMemorySize(&flex_before);
    uintptr_t sp = (uintptr_t)&here;
    __asm__ volatile("" : "+r"(sp));  /* an opaque address: no object the compiler can reason about */
    for (int k = 1; k <= 8; k++) *(volatile uint8_t *)(sp - (uintptr_t)k * MIB) = (uint8_t)k;
    if ((uintptr_t)arg == 1) {
        sceKernelAvailableFlexibleMemorySize(&flex_after);
        void *qs = NULL, *qe = NULL;
        uint32_t qp = 0;
        int q = sceKernelQueryMemoryProtection(&here - 8 * MIB, &qs, &qe, &qp);
        say("S2 lazy thread 1: 8 MiB walked; flexible %zu -> %zu KiB; stack at -8 MiB: rc=%#x %p-%p prot=%#x",
            flex_before / 1024, flex_after / 1024, (unsigned)q, qs, qe, qp);
    }
    return (void *)(uintptr_t)(2 * ((uintptr_t)arg & 0xff));
}

static void s2_lazy_stacks(void) {
    const int n = S2_THREADS;
    void *at = NULL;
    int rc = sceKernelReserveVirtualRange(&at, (size_t)n * LAZY_STACK, 0, LAZY_STACK);
    say("S2 lazy: reserve %d x 256 MiB stack addresses -> rc=%#x at %p", n, (unsigned)rc, at);
    if (rc) {
        verdict("S2", 0, "lazily committed 256 MiB stacks");
        return;
    }
    g_lazy_lo = at;
    g_lazy_span = (uint64_t)n * LAZY_STACK;
    struct sigaction sa = {0};
    sa.sa_sigaction = lazy_on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    pthread_t th[S2_THREADS];
    int made = 0, joined = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *lo = (uint8_t *)at + (size_t)i * LAZY_STACK;
        void *top = lo + LAZY_STACK - LAZY_STEP;
        int64_t phys = 0;
        int a = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), LAZY_STEP, PS5_KERNEL_DIRECT_ALIGNMENT,
                                              PS5_KERNEL_DIRECT_TYPE_CPU, &phys);
        int m = a ? -1 : sceKernelMapDirectMemory(&top, LAZY_STEP, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                                  PS5_KERNEL_MAP_FIXED, phys, 0);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        int r = (a || m) ? -1 : pthread_attr_setstack(&attr, lo, LAZY_STACK);
        if (r == 0) r = pthread_create(&th[made], &attr, lazy_thread, (void *)(uintptr_t)(i + 1));
        pthread_attr_destroy(&attr);
        if (r) {
            say("S2 lazy thread %d: alloc=%#x map=%#x create=%d", i, (unsigned)a, (unsigned)m, r);
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        void *r;
        if (pthread_join(th[i], &r) == 0 && (uintptr_t)r == (uintptr_t)(2 * ((i + 1) & 0xff))) joined++;
    }
    say("S2 lazy: %d/%d threads joined, %d ran on the given stack, %d fault-time 1 MiB maps, failure=%d", joined, n,
        g_lazy_on_own_stack, g_lazy_maps, g_lazy_fail);
    verdict("S2", joined == n && g_lazy_on_own_stack == n && g_lazy_maps >= n && !g_lazy_fail,
            "guest threads on lazily committed 256 MiB stacks");
    /* Does an access to reserved addresses with nothing mapped fault at all? */
    void *qs = NULL, *qe = NULL;
    uint32_t qp = 0;
    int q = sceKernelQueryMemoryProtection(g_lazy_lo, &qs, &qe, &qp);
    say("S2 lazy: query bottom of stack 0: rc=%#x %p-%p prot=%#x", (unsigned)q, qs, qe, qp);
    uint8_t *mid = g_lazy_lo + LAZY_STACK / 2;
    int before = g_lazy_maps;
    say("S2 lazy: read reserved, unmapped byte -> %d (maps %d -> ", ((volatile uint8_t *)mid)[0], before);
    say("S2 lazy: ... %d)", g_lazy_maps);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}
#endif

static void s1_guest_window(void) {
    const size_t size = GUEST_HI - GUEST_LO;
    say("S1 page size: getpagesize=%d sysconf=%ld", getpagesize(), sysconf(_SC_PAGESIZE));

    /* The Linux backend's reservation: the whole window read/write, nothing committed (runtime/platform/linux.c) */
    void *rw = mmap((void *)GUEST_LO, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
                    -1, 0);
    say("S1 mmap fixed rw %zu GiB at %#llx -> %p errno=%d", (size_t)(size / GIB), GUEST_LO, rw,
        rw == MAP_FAILED ? errno : 0);
    int rw_ok = rw == (void *)GUEST_LO;
    if (rw_ok) munmap(rw, size);

    /* The Windows-style reservation: the window inaccessible, committed region by region */
    void *base = mmap((void *)GUEST_LO, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    say("S1 mmap fixed none -> %p errno=%d", base, base == MAP_FAILED ? errno : 0);
    verdict("S1", rw_ok || base == (void *)GUEST_LO, "guest window reserved at its fixed address");
    if (base != (void *)GUEST_LO) {
        s1_survey();
#ifdef __PROSPERO__
        s1_direct_window();
#endif
        return;
    }

    /* Commit regions the runtime uses: module image (0x7100000000), HLE heap (0x7092000000), guest heap (0x7240000000) */
    static const struct { uint64_t at, size; const char *what; } regions[] = {
        {0x7100000000ull, 256 * MIB, "module image"},
        {0x7092000000ull, 64 * MIB, "HLE heap"},
        {0x7240000000ull, 3 * GIB, "guest heap"},
    };
    int commit_ok = 1;
    for (size_t i = 0; i < sizeof regions / sizeof *regions; i++) {
        uint64_t t0 = now_ns();
        int rc = mprotect((void *)regions[i].at, regions[i].size, PROT_READ | PROT_WRITE);
        int e = errno;
        uint64_t touched = 0;
        if (rc == 0) {
            /* Touch every 16 KiB: the guest heap touched whole is a real commit of 3 GiB */
            for (uint64_t off = 0; off < regions[i].size; off += 16 * 1024, touched++)
                ((volatile uint8_t *)regions[i].at)[off] = (uint8_t)off;
        }
        say("S1 commit %s %llu MiB at %#llx: rc=%d errno=%d touched=%llu pages %.1f ms", regions[i].what,
            (unsigned long long)(regions[i].size / MIB), (unsigned long long)regions[i].at, rc, rc ? e : 0,
            (unsigned long long)touched, (now_ns() - t0) / 1e6);
        commit_ok &= rc == 0;
    }
    verdict("S1", commit_ok, "module image, HLE heap and 3 GiB guest heap committed and written");

    /* Protection granularity: the guest maps 4 KiB pages (runtime/platform/macos.c comment) */
    uint8_t *p = (uint8_t *)0x7100000000ull;
    int r4k = mprotect(p + 4096, 4096, PROT_READ);
    int e4k = errno;
    int r16k = mprotect(p + 16384, 16384, PROT_READ);
    int e16k = errno;
    say("S1 mprotect 4 KiB rc=%d errno=%d, 16 KiB rc=%d errno=%d", r4k, r4k ? e4k : 0, r16k, r16k ? e16k : 0);
    verdict("S1", r16k == 0, "16 KiB protection inside the window");
    mprotect(p, 64 * 1024, PROT_READ | PROT_WRITE);

    /* Reset (in-process reboot, runtime/main.c): map the region over again, it must read zero */
    p[0] = 0x5a;
    void *again = mmap(p, 1 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    say("S1 reset remap -> %p, first byte %#x", again, again == p ? p[0] : 0xff);
    verdict("S1", again == p && p[0] == 0, "reset by fixed remap reads zero");
}

/* ---- S2: big stacks and fault recovery ---------------------------------------------------------------------- */

#define S2_THREADS 48

static void *s2_thread(void *arg) {
    /* 4 MiB of stack, as a deep guest call chain would */
    volatile uint8_t deep[4 * MIB];
    deep[0] = deep[sizeof deep - 1] = (uint8_t)(uintptr_t)arg;
    return (void *)(uintptr_t)(deep[0] + deep[sizeof deep - 1]);
}

static uint8_t *volatile g_fault_page;
static volatile int g_faults;

static void s2_on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig, (void)ctx;
    uint8_t *page = g_fault_page;
    if (page && (uint8_t *)info->si_addr >= page && (uint8_t *)info->si_addr < page + 16384) {
        g_faults++;
        mprotect(page, 16384, PROT_READ | PROT_WRITE);
        return;  /* the faulting store runs again */
    }
    signal(SIGSEGV, SIG_DFL);
}

static void *s2_light(void *arg) { return arg; }

/* The largest stack pthread_create accepts by size, and whether a stack the title maps itself (pthread_attr_setstack)
 * of 256 MiB works: the runtime could own its guest stacks that way. */
static void s2_stack_survey(void) {
    for (uint64_t mib = 128; mib >= 1; mib /= 2) {
        pthread_attr_t a;
        pthread_t t;
        pthread_attr_init(&a);
        int rc = pthread_attr_setstacksize(&a, mib * MIB);
        if (rc == 0) rc = pthread_create(&t, &a, s2_light, NULL);
        pthread_attr_destroy(&a);
        say("S2 survey stacksize %llu MiB: rc=%d", (unsigned long long)mib, rc);
        if (rc == 0) {
            pthread_join(t, NULL);
            break;
        }
    }
    void *stack = mmap(NULL, 256 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    say("S2 survey own 256 MiB stack mmap -> %p errno=%d", stack, stack == MAP_FAILED ? errno : 0);
    if (stack == MAP_FAILED) return;
    int made = 0, joined = 0;
    void *stacks[S2_THREADS] = {stack};
    pthread_t th[S2_THREADS];
    for (int i = 0; i < S2_THREADS; i++) {
        if (!stacks[i]) stacks[i] = mmap(NULL, 256 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stacks[i] == MAP_FAILED) {
            say("S2 survey own stack %d: mmap errno=%d", i, errno);
            stacks[i] = NULL;
            break;
        }
        pthread_attr_t a;
        pthread_attr_init(&a);
        int rc = pthread_attr_setstack(&a, stacks[i], 256 * MIB);
        if (rc == 0) rc = pthread_create(&th[made], &a, s2_thread, (void *)(uintptr_t)(i + 1));
        pthread_attr_destroy(&a);
        if (rc) {
            say("S2 survey own stack %d: rc=%d", i, rc);
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        void *r;
        if (pthread_join(th[i], &r) == 0 && (uintptr_t)r == (uintptr_t)(2 * ((i + 1) & 0xff))) joined++;
    }
    say("S2 survey %d/%d threads on title-mapped 256 MiB stacks ran and joined", joined, S2_THREADS);
    for (int i = 0; i < S2_THREADS && stacks[i]; i++) munmap(stacks[i], 256 * MIB);
}

static void s2_threads_and_faults(void) {
    pthread_t th[S2_THREADS];
    int made = 0, joined = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < S2_THREADS; i++) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        int rc = pthread_attr_setstacksize(&a, 256 * MIB);
        if (rc == 0) rc = pthread_create(&th[made], &a, s2_thread, (void *)(uintptr_t)(i + 1));
        pthread_attr_destroy(&a);
        if (rc) {
            say("S2 thread %d: rc=%d", i, rc);
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        void *r;
        if (pthread_join(th[i], &r) == 0 && (uintptr_t)r == (uintptr_t)(2 * ((i + 1) & 0xff))) joined++;
    }
    say("S2 %d/%d threads on 256 MiB stacks ran and joined in %.1f ms", joined, S2_THREADS, (now_ns() - t0) / 1e6);
    verdict("S2", joined == S2_THREADS, "guest threads on 256 MiB stacks");
    if (joined != S2_THREADS) s2_stack_survey();

    struct sigaction sa = {0};
    sa.sa_sigaction = s2_on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int rc = sigaction(SIGSEGV, &sa, NULL);
    struct sigaction bus = sa;
    sigaction(SIGBUS, &bus, NULL);
    uint8_t *page = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    say("S2 sigaction rc=%d, guard page %p", rc, (void *)page);
    if (rc || page == MAP_FAILED) {
        verdict("S2", 0, "write fault recovered");
        return;
    }
    g_fault_page = page;
    page[100] = 42;  /* faults once; the handler unprotects; the store is retried */
    say("S2 faults=%d value=%d", g_faults, page[100]);
    verdict("S2", g_faults == 1 && page[100] == 42, "write fault caught and resumed");
    g_fault_page = NULL;
}

/* ---- S3: a large .text ------------------------------------------------------------------------------------- */

int m0_bigtext_entry(int x);       /* m0_bigtext.S: at the start of the block */
int m0_bigtext_tail(int x);        /* ... and after it */
extern const char m0_bigtext_end[];

static void s3_bigtext(void) {
    long mb = (long)(((uintptr_t)m0_bigtext_end - (uintptr_t)m0_bigtext_entry) / MIB);
    int a = m0_bigtext_entry(20), b = m0_bigtext_tail(20);
    say("S3 .text block %ld MiB: entry(20)=%d tail(20)=%d (load time: launch to this line in the run's klog)", mb, a,
        b);
    verdict("S3", a == 21 && b == 22, "code at both ends of the large .text runs");
}

/* ---- S6: CPU ---------------------------------------------------------------------------------------------- */

static __thread uint64_t g_tls_counter;
static uint64_t g_counter;

static void s6_cpu(void) {
    enum { N = 200000000 };
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < N; i++) g_counter += i ^ (g_counter >> 3);
    uint64_t t1 = now_ns();
    for (uint32_t i = 0; i < N; i++) g_tls_counter += i ^ (g_tls_counter >> 3);
    uint64_t t2 = now_ns();
    say("S6 dependent int loop: global %.3f ns/iter, __thread %.3f ns/iter (emulated TLS on PS5) [%llu %llu]",
        (double)(t1 - t0) / N, (double)(t2 - t1) / N, (unsigned long long)g_counter,
        (unsigned long long)g_tls_counter);

    enum { FN = 1 << 20 };
    float *v = malloc(FN * sizeof *v);
    for (int i = 0; i < FN; i++) v[i] = (float)i * 0.5f;
    uint64_t t3 = now_ns();
    float acc = 0;
    for (int r = 0; r < 200; r++)
        for (int i = 0; i < FN; i++) acc = acc * 0.999f + v[i];
    uint64_t t4 = now_ns();
    say("S6 dependent float loop: %.3f ns/iter [%g]", (double)(t4 - t3) / (200.0 * FN), (double)acc);

    enum { MB = 256 };
    uint8_t *src = malloc(MB * MIB), *dst = malloc(MB * MIB);
    memset(src, 1, MB * MIB);
    memset(dst, 0, MB * MIB);
    uint64_t t5 = now_ns();
    for (int r = 0; r < 4; r++) {
        src[r] = (uint8_t)r;
        memcpy(dst, src, MB * MIB);
        __asm__ volatile("" : : "r"(dst) : "memory");
    }
    uint64_t t6 = now_ns();
    say("S6 memcpy %d MiB x4: %.2f GB/s [%d]", MB, 4.0 * MB * MIB / ((t6 - t5) / 1e9) / 1e9, dst[3] + dst[MB * MIB - 1]);
    free(v), free(src), free(dst);
}

int main(void) {
    g_log = fopen(LOG_PATH, "w");
    say("Savage Wind PS5 M0 probe, %s", LOG_PATH);
    s3_bigtext();
    s1_guest_window();
    s2_threads_and_faults();
#ifdef __PROSPERO__
    s2_lazy_stacks();
#endif
    s6_cpu();
    say("done: %d failed check(s)", g_fail);
    if (g_log) fclose(g_log);
#ifdef __PROSPERO__
    /* exit() faults for titles (PS5_vkQuake docs/FINDINGS.md); the shell closes asynchronously */
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) usleep(100000);
#endif
    return g_fail != 0;
}
