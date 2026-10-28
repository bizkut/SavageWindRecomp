// Title heap arenas of their own for the threads that allocate the most at once (plat_thread_heap_own), and the time
// spent in the heap, sampled (rt_heap_timing).
//
// The payload SDK's title heap (PS5_PayloadSDK platform/src/heap.c) is eight dlmalloc mspaces, each behind its own
// lock, handed out round robin by thread: sixteen compiling threads share them two by two. On .41 the shader
// precompilation's translations ran 2.1x faster on 16 workers than on one (ps5/evidence/m6/precompile-first3.klog;
// 6.6x on 7 host threads), with every worker on its own core. Here a thread that asks gets an mspace of its own from
// the same reserved range (create_mspace maps through ps5p_heap_map), so its allocations take no lock another thread
// holds; frees and reallocs go to the block's own arena wherever they come from (the SDK's dlmalloc keeps FOOTERS).
// An arena goes back to the pool when its thread ends, for the next thread that asks.
//
// Linked with --wrap=ps5_heap_malloc/calloc/memalign/free (ps5/tools/build-game.sh): the SDK's malloc wrappers
// (heap_wrap.c) call these.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform.h"

typedef void *mspace;
mspace create_mspace(size_t capacity, int locked);
void *mspace_malloc(mspace space, size_t bytes);
void *mspace_calloc(mspace space, size_t count, size_t bytes);
void *mspace_memalign(mspace space, size_t alignment, size_t bytes);
void *__real_ps5_heap_malloc(size_t bytes);
void *__real_ps5_heap_calloc(size_t count, size_t bytes);
void *__real_ps5_heap_memalign(size_t alignment, size_t bytes);
void __real_ps5_heap_free(void *pointer);

#define OWN_ARENAS 32
static mspace g_pool[OWN_ARENAS];
static int g_free[OWN_ARENAS], g_nfree, g_made;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static _Atomic unsigned g_own_now;  // threads on arenas of their own now
static _Atomic uint64_t g_calls, g_ns;
// (no __thread here: the PS5 build's TLS is emulated, and its first access in a thread allocates)

static void give_back(void *slot) {  // (the thread ends: its arena waits for the next thread that asks)
    pthread_mutex_lock(&g_pool_lock);
    g_free[g_nfree++] = (int)(uintptr_t)slot - 1;
    pthread_mutex_unlock(&g_pool_lock);
    atomic_fetch_sub(&g_own_now, 1);
}

static void make_key(void) { pthread_key_create(&g_key, give_back); }

static mspace own(void) {
    if (!atomic_load_explicit(&g_own_now, memory_order_relaxed)) return NULL;  // (none handed out: no lookup)
    uintptr_t slot = (uintptr_t)pthread_getspecific(g_key);
    return slot ? g_pool[slot - 1] : NULL;
}

void plat_thread_heap_own(void) {
    pthread_once(&g_key_once, make_key);
    if (pthread_getspecific(g_key)) return;
    __real_ps5_heap_free(__real_ps5_heap_malloc(1));  // (the heap's range reserved first: the arenas map from it)
    pthread_mutex_lock(&g_pool_lock);
    int slot = -1;
    if (g_nfree) slot = g_free[--g_nfree];
    else if (g_made < OWN_ARENAS && (g_pool[g_made] = create_mspace(0, 1))) slot = g_made++;
    pthread_mutex_unlock(&g_pool_lock);
    if (slot < 0) return;  // (all taken: the shared arenas)
    atomic_fetch_add(&g_own_now, 1);
    pthread_setspecific(g_key, (void *)(uintptr_t)(slot + 1));
}

// About one call in 64 is timed (x64), picked by the time stamp counter's low bits: the heap's time without a clock
// read per allocation
#define SAMPLED(call)                                                                                         \
    if ((__builtin_ia32_rdtsc() & 63) != 0) return call;                                                      \
    uint64_t t0 = plat_now_ns();                                                                              \
    __typeof__(call) r_ = call;                                                                               \
    atomic_fetch_add_explicit(&g_ns, (plat_now_ns() - t0) * 64, memory_order_relaxed);                        \
    atomic_fetch_add_explicit(&g_calls, 64, memory_order_relaxed);                                            \
    return r_;

void *__wrap_ps5_heap_malloc(size_t bytes) {
    mspace m = own();
    SAMPLED(m ? mspace_malloc(m, bytes) : __real_ps5_heap_malloc(bytes))
}
void *__wrap_ps5_heap_calloc(size_t count, size_t bytes) {
    mspace m = own();
    SAMPLED(m ? mspace_calloc(m, count, bytes) : __real_ps5_heap_calloc(count, bytes))
}
void *__wrap_ps5_heap_memalign(size_t alignment, size_t bytes) {
    mspace m = own();
    SAMPLED(m ? mspace_memalign(m, alignment, bytes) : __real_ps5_heap_memalign(alignment, bytes))
}
void __wrap_ps5_heap_free(void *pointer) {
    if ((__builtin_ia32_rdtsc() & 63) != 0) { __real_ps5_heap_free(pointer); return; }
    uint64_t t0 = plat_now_ns();
    __real_ps5_heap_free(pointer);
    atomic_fetch_add_explicit(&g_ns, (plat_now_ns() - t0) * 64, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_calls, 64, memory_order_relaxed);
}

void rt_heap_timing(uint64_t *calls, uint64_t *ns, unsigned *own_arenas) {
    *calls = atomic_load(&g_calls);
    *ns = atomic_load(&g_ns);
    *own_arenas = atomic_load(&g_own_now);
}
