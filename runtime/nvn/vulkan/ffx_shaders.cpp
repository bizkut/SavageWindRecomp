// Shader permutations for the FidelityFX Vulkan backend shared by the temporal upscaler and desktop interpolation.
#include <cstring>
#include <cstdio>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>
#include "ffx_fsr3upscaler_vk.h"
#ifdef SW_FFX_INTERPOLATION
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include "ffx_shaders_vk.h"
#endif

enum { FU_PERM_FP16 = 1 << 7 };
extern "C" bool vk_shader_fp16(void);

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options,
                                                     FfxShaderBlob *out) {
#ifdef SW_FFX_INTERPOLATION
    if (effect == FFX_EFFECT_FRAMEINTERPOLATION && pass < FFX_FRAMEINTERPOLATION_PASS_COUNT) {
        memcpy(out, &ffx_fi_blobs[(options & (1 << 2)) ? 1 : 0][pass], sizeof *out);
        return FFX_OK;
    }
    if (effect == FFX_EFFECT_OPTICALFLOW && pass < FFX_OPTICALFLOW_PASS_COUNT) {
        memcpy(out, &ffx_of_blobs[pass], sizeof *out);
        return FFX_OK;
    }
#endif
    if (effect == FFX_EFFECT_FSR3UPSCALER && pass < FFX_FSR3UPSCALER_PASS_COUNT && ffx_fu_blobs[0][pass].data) {
        int half = (options & FU_PERM_FP16) && vk_shader_fp16();
        memcpy(out, &ffx_fu_blobs[half][pass], sizeof *out);
        return FFX_OK;
    }
    fprintf(stderr, "[ffx] ffxGetPermutationBlobByIndex: no shader for effect %d pass %d options 0x%x\n",
            (int)effect, (int)pass, options);
    return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;
    return FFX_OK;
}

// The backend's interpolation swapchain is not built by either platform.
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }
