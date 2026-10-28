// Frame time: per-frame CPU time of the game's threads and the render thread, the GPU's time per frame, and what each
// long frame went to (docs/PS5-PORT-PLAN.md M6). Implementation: runtime/frametime.c.
//
// Sources: the threads' own CPU times (plat_thread_cpu_ns: the render thread at each frame, a guest thread at each of
// its waits), the guest threads' waits (KTimes, runtime/kernel.c), the GPU's
// timestamps around each submission (vulkan/exec.c ft_gpu), and the counters below, which the renderer adds to where
// the work happens. ft_frame runs at each presented game frame on the thread that presents (the submission thread):
// a frame longer than 1.5 periods of the frame-rate limit (at least 25 ms) gets one "[hitch]" line with its cause,
// the 2 s "[vk]" line gets the window's summary (ft_window), and "[perf]" lines give the frame times over each minute
// and over the last 10 minutes (M6's measure: the share of frames on time).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FT_SHADER_NS, FT_SHADERS,         // shader translations and programs built (GLSL -> SPIR-V, modules): any thread
    FT_PIPELINE_NS, FT_PIPELINES,     // graphics and compute pipelines built where a draw or dispatch needed them
    FT_BUILD_INLINE_NS,               // the time of those builds the render thread ran itself (synchronous shaders)
    FT_BUILD_WAIT_NS,                 // the render thread waiting for a build on a worker (asynchronous shaders)
    FT_TEXTURE_NS, FT_TEXTURE_BYTES,  // texture checks (hashing guest memory) and uploads: render thread
    FT_UPLOAD_BYTES,                  // transient memory written for the GPU (uploads, guest copies): render thread
    FT_GPU_WAIT_NS,                   // the render thread waiting for the GPU
    FT_PRESENT_NS, FT_ACQUIRE_NS,     // the present (pass end, swapchain draw, submit, present) and its image acquire
    FT_COUNT
};
extern uint64_t g_ft[FT_COUNT];
static inline void ft_add(int counter, uint64_t v) { __atomic_fetch_add(&g_ft[counter], v, __ATOMIC_RELAXED); }

void ft_present_thread(void);              // nvnQueuePresentTexture: the calling guest thread presents the frames
void ft_gpu(uint64_t frame, uint64_t ns);  // GPU time of one submission recorded for that frame (in submission order)
void ft_frame(void);                       // a game frame was presented (the thread that presents, after it)
void ft_window(char *out, size_t n);       // this window's summary for the [vk] line; a new window starts

#ifdef __cplusplus
}
#endif
