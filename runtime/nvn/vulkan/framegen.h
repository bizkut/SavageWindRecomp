// Frame generation's backends behind exec.c vk_fg_generate and device.c's presents (docs/PS5-PORT-PLAN.md D7). Each
// build links one: desktop framegen.cpp (AMD FSR 3 frame interpolation: the FidelityFX SDK's frame interpolation and
// optical flow, from the game image, its HUD-less image, depth and camera motion vectors), the PS5 mihawk.cpp
// (PS5_MihawkGen's neural method, from the last two final window images only; GPL-3.0-or-later, D10: never in the
// desktop build). Plain C: both are C++.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t w, h;
} FgImage;

// What the FSR 3 contexts are made for (a change makes them again)
typedef struct {
    uint32_t display_w, display_h;  // the game's final image (the interpolated frame's size)
    uint32_t render_w, render_h;    // the 3D scene's (depth and motion vectors)
    VkFormat color_format, hudless_format;
    bool depth_inverted;
} FgConfig;

typedef struct {
    FgImage color;     // this frame as presented (with the HUD)
    FgImage hudless;   // ... without it (the 3D scene composited, before the HUD's first draw)
    FgImage depth;     // the 3D scene's depth (D32)
    FgImage motion;    // camera motion vectors (R16G16F, uv units: where each pixel was in the previous frame - here)
    FgImage out;       // the interpolated frame (storage image)
    float near_, far_, fovy;  // the world camera's (fovy in radians)
    float cam_pos[3], cam_up[3], cam_right[3], cam_fwd[3];
    float frame_ms;    // time since the previous real frame
    uint64_t frame_id; // +1 per real frame (a gap resets the interpolation)
    bool reset;        // the camera cut: no history
    bool debug_view;   // (SAVAGEWIND_FG_DEBUG_VIEW) the SDK's debug view instead of the interpolated frame
} FgFrame;

typedef struct {
    VkPhysicalDevice phys;
    VkDevice dev;
    void (*wait_idle)(void);  // the GPU idle (before the contexts are made again)
} FgVk;
extern FgVk g_fg_vk;  // (exec.c, set before a backend's first call)

enum { FG_BACKEND_FSR3_FI, FG_BACKEND_MIHAWK };

typedef struct {
    int kind;          // FG_BACKEND_*
    const char *name;  // for the log
    // FSR 3 (FG_BACKEND_FSR3_FI): records the interpolated frame between the previous call's frame and this one into
    // f->out. false: failed (logged).
    bool (*dispatch)(VkCommandBuffer cb, const FgConfig *c, const FgFrame *f);
    // MihawkGen (FG_BACKEND_MIHAWK), on the final window images (w x h):
    //   ready: the generator for that size with `slots` frames in flight (made at the first call and after a size
    //     change, with the GPU idle; the weights read then). false: unavailable, the reason logged once.
    //   record: the frame halfway between prev and next (sampled views, both in SHADER_READ_ONLY_OPTIMAL with their
    //     writes visible to compute) into out (a storage view of an R8G8B8A8_UNORM image of that size, in GENERAL).
    //     No barriers around them: the caller's, as is making out's compute writes visible to whatever reads it.
    //     A slot is reused only once the submission that last recorded it is done.
    //   gpu_ns: the slot's last recording's GPU time (its passes' timestamps), once that submission is done; false:
    //     none (no timestamps, or read already).
    bool (*ready)(uint32_t w, uint32_t h, uint32_t slots);
    void (*record)(VkCommandBuffer cb, uint32_t slot, VkImageView prev, VkImageView next, VkImageView out);
    bool (*gpu_ns)(uint32_t slot, uint64_t *ns);
} FgBackend;
extern const FgBackend g_fg_backend;  // the build's

#ifdef __cplusplus
}
#endif
