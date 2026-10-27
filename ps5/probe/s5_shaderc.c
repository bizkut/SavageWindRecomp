/*
 * Savage Wind PS5 - M0 spike S5: shaderc on the console, with the options runtime/nvn/vulkan/spirv.c uses (Vulkan 1.3
 * target, optimization zero), compiling GLSL of the shape the Maxwell translator emits. Each SPIR-V is written to
 * /app0/s5_<name>.spv for comparison with the same compile on the PC (ps5/tools/compare-spirv.sh: spirv-val +
 * disassembly), and the vertex/fragment pair is drawn by s4-style Vulkan in a later step.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <shaderc/shaderc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#define DIR "/app0/"
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
#else
#define DIR ""
#endif

static FILE *g_log;
static int g_fail;

static void say(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("s5: %s\n", line);
    fflush(stdout);
    if (g_log) fprintf(g_log, "%s\n", line), fflush(g_log);
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Shaped like the translator's output: cbuf uniform blocks, scalar layout, 8/16-bit storage, demote */
static const char vs[] =
    "#version 460\n"
    "#extension GL_EXT_scalar_block_layout : require\n"
    "layout(std430, set = 0, binding = 3) uniform c3 { vec4 d[256]; } cb3;\n"
    "layout(location = 0) in vec4 a0;\n"
    "layout(location = 1) in vec2 a1;\n"
    "layout(location = 0) out vec2 v0;\n"
    "void main() {\n"
    "  vec4 p = vec4(dot(a0, cb3.d[0]), dot(a0, cb3.d[1]), dot(a0, cb3.d[2]), dot(a0, cb3.d[3]));\n"
    "  gl_Position = p; v0 = a1 * cb3.d[4].xy + cb3.d[4].zw;\n"
    "}\n";
static const char fs[] =
    "#version 460\n"
    "#extension GL_EXT_demote_to_helper_invocation : require\n"
    "#extension GL_EXT_shader_8bit_storage : require\n"
    "#extension GL_EXT_shader_16bit_storage : require\n"
    "layout(set = 0, binding = 8) uniform sampler2D t0;\n"
    "layout(std430, set = 0, binding = 20) readonly buffer g0 { uint8_t b[]; } gb;\n"
    "layout(location = 0) in vec2 v0;\n"
    "layout(location = 0) out vec4 o0;\n"
    "void main() {\n"
    "  vec4 c = texture(t0, v0);\n"
    "  if (c.a < float(uint(gb.b[0])) / 255.0) demote;\n"
    "  o0 = vec4(c.rgb * 2.0 - 1.0, 1.0);\n"
    "}\n";
static const char cs[] =
    "#version 460\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, set = 0, binding = 0) buffer b0 { uint v[]; } buf;\n"
    "shared uint s[64];\n"
    "void main() {\n"
    "  uint i = gl_LocalInvocationIndex; s[i] = buf.v[gl_GlobalInvocationID.x];\n"
    "  barrier(); buf.v[gl_GlobalInvocationID.x] = s[63 - i] ^ 0x5au;\n"
    "}\n";
static const char bad[] = "#version 460\nvoid main() { undeclared = 1; }\n";

/* The last successful compute SPIR-V, for the GPU run below */
static uint32_t g_cs[4096];
static size_t g_cs_bytes;

static int compile(shaderc_compiler_t c, const char *name, const char *src, shaderc_shader_kind kind, int expect_ok) {
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_zero);
    uint64_t t0 = now_ns();
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, src, strlen(src), kind, name, "main", o);
    double ms = (now_ns() - t0) / 1e6;
    shaderc_compile_options_release(o);
    int ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    size_t n = shaderc_result_get_length(r);
    const uint32_t *w = (const uint32_t *)shaderc_result_get_bytes(r);
    say("%s: status %d, %zu bytes, magic %#x, %.2f ms%s%s", name, shaderc_result_get_compilation_status(r), n,
        ok && n >= 4 ? w[0] : 0, ms, ok ? "" : ", error: ", ok ? "" : shaderc_result_get_error_message(r));
    if (ok) {
        char path[256];
        snprintf(path, sizeof path, DIR "s5_%s.spv", name);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(w, 1, n, f), fclose(f);
    }
    if (ok && kind == shaderc_compute_shader && n <= sizeof g_cs) memcpy(g_cs, w, n), g_cs_bytes = n;
    int pass = expect_ok ? ok && n >= 20 && w[0] == 0x07230203 : !ok;  /* before release: w is the result's memory */
    shaderc_result_release(r);
    say("S5 %s: %s %s", pass ? "PASS" : "FAIL", name, expect_ok ? "compiles to SPIR-V" : "is rejected with a message");
    if (!pass) g_fail++;
    return pass;
}

#ifdef __PROSPERO__
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

/* Runs the console-compiled compute shader on RADV over 256 uints and checks every result against the CPU */
static void gpu_run(void) {
#define G(name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(inst, #name)
    VkInstance inst = VK_NULL_HANDLE;
    G(vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ici, NULL, &inst)) { say("S5 FAIL: gpu instance"); g_fail++; return; }
    G(vkEnumeratePhysicalDevices); G(vkCreateDevice); G(vkGetDeviceQueue); G(vkCreateBuffer);
    G(vkGetBufferMemoryRequirements); G(vkGetPhysicalDeviceMemoryProperties); G(vkAllocateMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCreateShaderModule); G(vkCreateDescriptorSetLayout);
    G(vkCreatePipelineLayout); G(vkCreateComputePipelines); G(vkCreateDescriptorPool); G(vkAllocateDescriptorSets);
    G(vkUpdateDescriptorSets); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdDispatch); G(vkEndCommandBuffer); G(vkQueueSubmit);
    G(vkQueueWaitIdle);
    uint32_t one = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &one, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    VkDevice dev;
    vkCreateDevice(pd, &dci, NULL, &dev);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    enum { N = 256 };
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ty = 0;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (ty < mp.memoryTypeCount &&
           !((mr.memoryTypeBits >> ty & 1) && (mp.memoryTypes[ty].propertyFlags & want) == want))
        ty++;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = ty};
    VkDeviceMemory mem;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    uint32_t *data;
    vkMapMemory(dev, mem, 0, N * 4, 0, (void **)&data);
    for (uint32_t i = 0; i < N; i++) data[i] = i * 2654435761u;
    VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_cs_bytes,
                                    .pCode = g_cs};
    VkShaderModule sm;
    VkResult r = vkCreateShaderModule(dev, &smi, NULL, &sm);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &b};
    VkDescriptorSetLayout dl;
    vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl);
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dl};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = sm, .pName = "main"}};
    VkPipeline pipe;
    uint64_t t0 = now_ns();
    VkResult rp = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
    say("gpu: module %d, pipeline %d (ACO compile %.1f ms)", r, rp, (now_ns() - t0) / 1e6);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dl};
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(dev, &dsa, &ds);
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    VkCommandPoolCreateInfo cpool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpool, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) { /* the shader: v[g] = old v[group base + 63 - local] ^ 0x5a */
        uint32_t src = (i & ~63u) + 63 - (i & 63);
        if (data[i] != ((src * 2654435761u) ^ 0x5au)) bad++;
    }
    say("S5 %s: console-compiled compute shader ran on RADV, %d/%d results wrong", bad || r || rp ? "FAIL" : "PASS",
        bad, N);
    if (bad || r || rp) g_fail++;
#undef G
}
#endif

int main(void) {
    g_log = fopen(DIR "s5_shaderc.txt", "w");
    say("Savage Wind PS5 S5 shaderc probe");
    uint64_t t0 = now_ns();
    shaderc_compiler_t c = shaderc_compiler_initialize();
    say("shaderc_compiler_initialize: %p in %.1f ms", (void *)c, (now_ns() - t0) / 1e6);
    if (!c) {
        g_fail++;
        goto out;
    }
    compile(c, "vs", vs, shaderc_vertex_shader, 1);
    compile(c, "fs", fs, shaderc_fragment_shader, 1);
    compile(c, "cs", cs, shaderc_compute_shader, 1);
    compile(c, "bad", bad, shaderc_fragment_shader, 0);
    /* Repeat: steady-state cost per shader (spirv.c compiles on worker threads, one compiler each) */
    t0 = now_ns();
    for (int i = 0; i < 50; i++) {
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compilation_result_t r =
            shaderc_compile_into_spv(c, fs, strlen(fs), shaderc_fragment_shader, "fs", "main", o);
        shaderc_result_release(r);
        shaderc_compile_options_release(o);
    }
    say("50 fragment compiles: %.2f ms each", (now_ns() - t0) / 50e6);
    shaderc_compiler_release(c);
#ifdef __PROSPERO__
    gpu_run();
#endif
out:
    say("done: %d failed check(s)", g_fail);
    if (g_log) fclose(g_log);
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) usleep(100000);
#endif
    return g_fail != 0;
}
