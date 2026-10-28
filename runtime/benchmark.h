// The built-in benchmark (docs/PS5-PORT-PLAN.md M6; runtime/benchmark.c): with a file benchmark.txt in the data
// root, the game runs a fixed camera route on the Great Plateau with no player, measures it and closes. It
// continues the player's latest save from a copy (benchmark/save: the player's saves are never written), outside
// the Shrine of Resurrection, and writes its summary to benchmark/last.txt (the previous run's is compared with it)
// and one line per run to benchmark/history.txt.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool benchmark_on(void);  // benchmark.txt is there (checked once)
// runtime/hle/input.c, once per presented frame: the route's controller (true while the benchmark drives it)
bool benchmark_input(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
const char *benchmark_save_dir(void);  // runtime/services/fs.c: the copy of the saves the game uses; NULL: off
// runtime/frametime.c: each presented frame (its number, length, render thread and guest CPU; the hitch cause or
// NULL), and each frame's GPU time once known
void benchmark_frame(uint64_t frame, uint64_t dt_ns, uint64_t render_ns, uint64_t guest_ns, const char *hitch);
void benchmark_gpu(uint64_t frame, uint64_t ns);
// vulkan/device.c: each present (any thread; generated: the first of frame generation's pair, the generated frame or
// the game's shown early on a cut), and the frame generator's GPU time for each generated frame (MihawkGen)
void benchmark_present(bool generated);
void benchmark_fg_gpu(uint64_t ns);
// vulkan/device.c, each presented frame: true once, when the fast-motion segment after the route wants frame
// generation's captures (*count pairs, one every *every game frames)
bool benchmark_fg_capture(int *count, int *every);
// vulkan/device.c, each game frame's present: the render thread's CPU time in it and, of that, in frame generation's
// recording (vk_fg_generate); fg: the frame was presented with a generated one
void benchmark_render_cpu(uint64_t present_ns, uint64_t fg_ns, bool fg);

#ifdef __cplusplus
}
#endif
