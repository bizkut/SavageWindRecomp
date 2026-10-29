// Render-thread profile (docs/PS5-PORT-PLAN.md "After M7 - Render-thread CPU"): where the thread that translates the
// game's command lists (runtime/nvn/objects.c queue_thread) spends each frame, by command kind, by step of a draw and,
// on the PS5, by Vulkan command (the driver's own time: ps5/tools/gen-vk-globals.py). On with `render_profile = 1` in
// savagewind.ini (or SAVAGEWIND_RENDER_PROFILE=1); one "[rprof]" line every 10 s, and with the built-in benchmark two
// more summary lines (camera turning / still parts of the route). Off, it costs one predicted branch per command.
// The times are the CPU's time stamp counter (one read per command, ~7 per draw), turned into ms at each report.
#pragma once
#include <stdint.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    // the command lists' commands, each its own time up to the next one (exec.c exec_list)
    RP_OP_DRAW, RP_OP_DISPATCH, RP_OP_CLEAR, RP_OP_COPY, RP_OP_UUB, RP_OP_TARGETS, RP_OP_STATE, RP_OP_SYNC, RP_OP_LIST,
    // backend_execute around the lists: readbacks and guest copies before, the submission after
    RP_EXEC_IN, RP_EXEC_SUBMIT,
    // the steps of a draw (exec.c draw_; the rest of RP_OP_DRAW is "other")
    RP_D_PRE, RP_D_PASS, RP_D_KEY, RP_D_DYN, RP_D_VSET, RP_D_TEX, RP_D_FSET, RP_D_VTX,
    // FSR 3's recorder synchronization and direct FidelityFX dispatch (subsets of draw)
    RP_FSR3_DRAIN, RP_FSR3_DISPATCH,
    RP_MVEC_RECORD, RP_MVEC_FRAME, RP_MVEC_MATCH, RP_MVEC_COPY, RP_MVEC_VARIANT, RP_SCENE_POST,
    RP_PRE_PROGRAM, RP_PRE_DEBUG, RP_PRE_FEEDBACK, RP_PRE_FG, RP_PRE_SNAPSHOT, RP_PRE_GW, RP_PRE_MOTION, RP_PRE_REACTIVE,
    RP_COUNT
};
extern int g_rprof;              // the profile is on (rprof_init)
extern uint64_t g_rp[RP_COUNT];  // time stamp counter ticks, render thread, never reset
extern uint64_t g_rp_n[RP_COUNT];  // commands / calls counted (the RP_OP_* and RP_D_PASS ones)

static inline uint64_t rp_now(void) {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

void rprof_init(void);         // reads the setting; the calling thread is the render thread
void rprof_frame(void);        // each presented game frame (vulkan/device.c, after ft_frame)
double rprof_tick_ns(void);    // ns per tick (calibrated against the monotonic clock)

#ifdef __cplusplus
}
#endif
