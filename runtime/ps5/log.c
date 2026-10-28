// PS5 klog: bounded MPSC queue so diagnostic output does not stall the renderer.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../platform.h"
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sceKernelDebugOutText(int channel, const char *text);
void ps5_logf(const char *format, ...);

enum { LOG_SLOTS = 512, LOG_LINE = 2048 };
typedef struct { _Atomic uint64_t seq; char text[LOG_LINE]; } LogSlot;
static LogSlot slots[LOG_SLOTS];
static _Atomic uint64_t write_pos, dropped;
static uint64_t read_pos;
static _Atomic bool active, stopping;
static __thread bool fatal;
static pthread_t logger;

void ps5_log_fatal_mode(void) { fatal = true; }

static void *log_worker(void *unused) {
    (void)unused;
    // The lowest scheduling priority the platform permits for normal threads.
    struct sched_param sp = { .sched_priority = sched_get_priority_min(SCHED_OTHER) };
    int prio_rc = sp.sched_priority < 0 ? -1 : pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    if (prio_rc) sceKernelDebugOutText(0, "savagewind: [log] could not lower logger priority\n");
    for (;;) {
        LogSlot *s = &slots[read_pos % LOG_SLOTS];
        if (atomic_load_explicit(&s->seq, memory_order_acquire) == read_pos + 1) {
            sceKernelDebugOutText(0, s->text);
            atomic_store_explicit(&s->seq, read_pos + LOG_SLOTS, memory_order_release);
            read_pos++;
            continue;
        }
        uint64_t lost = atomic_exchange_explicit(&dropped, 0, memory_order_relaxed);
        if (lost) {
            char msg[128];
            snprintf(msg, sizeof msg, "savagewind: [log] %llu lines dropped (queue full)\n", (unsigned long long)lost);
            sceKernelDebugOutText(0, msg);
        }
        if (atomic_load_explicit(&stopping, memory_order_acquire)) break;
        struct timespec delay = { .tv_nsec = 1000000 };
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static void log_shutdown(void) {
    atomic_store_explicit(&active, false, memory_order_release);
    atomic_store_explicit(&stopping, true, memory_order_release);
    pthread_join(logger, NULL);
}

void ps5_log_init(bool sync) {
    const char *env = getenv("SAVAGEWIND_LOG_SYNC");
    if (env && *env) sync = atoi(env) != 0;
    if (sync) {
        sceKernelDebugOutText(0, "savagewind: [log] synchronous (log_sync / SAVAGEWIND_LOG_SYNC)\n");
        return;
    }
    for (uint64_t i = 0; i < LOG_SLOTS; i++) atomic_init(&slots[i].seq, i);
    if (pthread_create(&logger, NULL, log_worker, NULL)) {
        sceKernelDebugOutText(0, "savagewind: [log] logger thread failed; synchronous logging\n");
        return;
    }
    atexit(log_shutdown);
    atomic_store_explicit(&active, true, memory_order_release);
    ps5_logf("[log] asynchronous klog (queue %d lines)\n", LOG_SLOTS);
}

void ps5_logf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (fatal || !atomic_load_explicit(&active, memory_order_acquire)) {
        char line[LOG_LINE] = "savagewind: ";
        vsnprintf(line + 12, sizeof line - 12, format, args);
        sceKernelDebugOutText(0, line);
    } else {
        uint64_t pos = atomic_load_explicit(&write_pos, memory_order_relaxed);
        LogSlot *s;
        for (;;) {
            s = &slots[pos % LOG_SLOTS];
            uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
            intptr_t diff = (intptr_t)(seq - pos);
            if (diff < 0) { atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed); break; }
            if (diff == 0 && atomic_compare_exchange_weak_explicit(&write_pos, &pos, pos + 1,
                                                                   memory_order_relaxed, memory_order_relaxed)) {
                memcpy(s->text, "savagewind: ", 12);
                vsnprintf(s->text + 12, sizeof s->text - 12, format, args);
                atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
                break;
            }
            pos = atomic_load_explicit(&write_pos, memory_order_relaxed);
        }
    }
    va_end(args);
}
