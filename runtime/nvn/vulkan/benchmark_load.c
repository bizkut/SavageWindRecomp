// Optional benchmark-only GPU load. Each sample has its own query pair, so recording never waits for the GPU.
#ifdef __PROSPERO__
#include <stdlib.h>
#include "../../benchmark.h"
#include "record.h"
#include "shaders.h"
#include "vk.h"

#define LOAD_SAMPLES 8192
static VkPipeline pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkQueryPool queries;
static unsigned issued, collected;
static double iterations = 256;
static uint64_t sample_seq[LOAD_SAMPLES];
static unsigned sample_iterations[LOAD_SAMPLES];
static int failed;

static bool load_init(void) {
    if (pipe) return true;
    if (failed) return false;
    failed = 1;
    if (!g_vk.props.limits.timestampComputeAndGraphics) {
        LOG("[bench] gpu_load unavailable: no compute timestamps\n");
        return false;
    }
    static const char source[] =
        "#version 450\n"
        "layout(local_size_x=64) in;\n"
        "layout(set=0,binding=0,std430) buffer Output { uint v[]; } o;\n"
        "layout(push_constant) uniform Params { uint count; } p;\n"
        "void main() { uint i=gl_GlobalInvocationID.x; uint x=i+1u;"
        " for(uint n=0u;n<p.count;n++) { x=(x^(x>>13u))*1664525u+1013904223u; } o.v[i]=x; }\n";
    size_t nwords = 0;
    uint32_t *spv = vk_glsl_to_spirv(source, STAGE_COMPUTE, "benchmark_load", &nwords);
    if (!spv) { LOG("[bench] gpu_load shader compilation failed\n"); return false; }
    VkShaderModule module = vk_module(spv, nwords);
    free(spv);
    if (!module) return false;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = LOAD_SAMPLES * 2};
    VkResult r = vkCreateDescriptorSetLayout(g_vk.dev, &si, NULL, &set_layout);
    if (r == VK_SUCCESS) r = vkCreatePipelineLayout(g_vk.dev, &li, NULL, &layout);
    if (r == VK_SUCCESS) r = vkCreateQueryPool(g_vk.dev, &qi, NULL, &queries);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
    if (r == VK_SUCCESS) r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (r != VK_SUCCESS) { LOG("[bench] gpu_load initialization failed: %d\n", r); return false; }
    return true;
}

// Call once per game frame, outside a render pass, before its final timestamp.
void benchmark_load_record(VkCommandBuffer cb) {
    static bool active;
    int ms = benchmark_gpu_load();
    if (!ms) {
        if (active) { LOG("[bench] gpu_load = 0 ms (first 20 measured seconds elapsed)\n"); active = false; }
        return;
    }
    if (!load_init()) return;
    if (!active) { LOG("[bench] gpu_load active: %d ms per game frame (timestamp calibrated)\n", ms); active = true; }
    uint64_t completed = vk_completed_seq();
    while (collected < issued && sample_seq[collected] <= completed) {
        uint64_t t[2];
        if (vkGetQueryPoolResults(g_vk.dev, queries, collected * 2, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        if (t[1] > t[0]) {
            double measured = (t[1] - t[0]) * g_vk.props.limits.timestampPeriod / 1e6;
            if (measured > 0.001) {
                double target = sample_iterations[collected] * (double)ms / measured;
                if (target < 1) target = 1;
                if (target > 1000000) target = 1000000;
                iterations = iterations * 0.75 + target * 0.25;
            }
        }
        collected++;
    }
    if (issued == LOAD_SAMPLES) { static bool warned; if (!warned) { warned = true; LOG("[bench] gpu_load: timestamp sample capacity exhausted\n"); } return; }
    uint32_t count = (uint32_t)iterations;
    sample_iterations[issued] = count;
    sample_seq[issued] = vk_pending_seq();
    VkSpan out = vk_transient(64 * 256 * sizeof(uint32_t), g_vk.props.limits.minStorageBufferOffsetAlignment);
    VkDescriptorSet ds = vk_alloc_set(set_layout);
    VkDescriptorBufferInfo bi = {out.buf, out.off, 64 * 256 * sizeof(uint32_t)};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    rec_UpdateDescriptorSets(1, &wr);
    rec_CmdResetQueryPool(cb, queries, issued * 2, 2);
    vk_full_barrier(cb);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, issued * 2);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    rec_CmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof count, &count);
    rec_CmdDispatch(cb, 256, 1, 1);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, issued * 2 + 1);
    vk_full_barrier(cb);
    issued++;
}
#endif
