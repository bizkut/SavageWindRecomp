/*
 * Savage Wind PS5 - M0 spike S4: RADV on the console against what Savage Wind's renderer asks for
 * (runtime/nvn/vulkan/device.c), then the presentation path the port will use: a VK_KHR_display surface on
 * VideoOut, a 3840x2160 FIFO swapchain, and the measured present cadence (119.88 Hz with the title's VRR metadata).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Output: stdout and /app0/s4_vulkan.txt. "S4 PASS/FAIL" lines are the verdicts.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef __PROSPERO__
#define LOG_PATH "/app0/s4_vulkan.txt"
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
/* RADV's archive exports the ICD entry point; the probe calls Vulkan through it (PS5_vkQuake src/radv_icd_ps5.c) */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
#define GIPA vk_icdGetInstanceProcAddr
#else
#define LOG_PATH "s4_vulkan.txt"
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name);
#define GIPA vkGetInstanceProcAddr
#endif

static FILE *g_log;
static int g_fail;

static void say(const char *fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("s4: %s\n", line);
    fflush(stdout);
    if (g_log) fprintf(g_log, "%s\n", line), fflush(g_log);
}

static void verdict(int ok, const char *what) {
    say("S4 %s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#define FN(name) static PFN_##name name
FN(vkCreateInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties);
FN(vkGetPhysicalDeviceFeatures2); FN(vkEnumerateDeviceExtensionProperties); FN(vkGetPhysicalDeviceMemoryProperties);
FN(vkGetPhysicalDeviceQueueFamilyProperties); FN(vkCreateDevice); FN(vkGetDeviceQueue);
FN(vkGetPhysicalDeviceDisplayPropertiesKHR); FN(vkGetDisplayModePropertiesKHR);
FN(vkGetPhysicalDeviceDisplayPlanePropertiesKHR); FN(vkGetDisplayPlaneSupportedDisplaysKHR);
FN(vkCreateDisplayPlaneSurfaceKHR); FN(vkGetPhysicalDeviceSurfaceSupportKHR);
FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
FN(vkGetPhysicalDeviceSurfacePresentModesKHR); FN(vkCreateSwapchainKHR); FN(vkGetSwapchainImagesKHR);
FN(vkAcquireNextImageKHR); FN(vkQueuePresentKHR); FN(vkCreateCommandPool); FN(vkAllocateCommandBuffers);
FN(vkBeginCommandBuffer); FN(vkEndCommandBuffer); FN(vkCmdPipelineBarrier2); FN(vkCmdClearColorImage);
FN(vkQueueSubmit); FN(vkCreateSemaphore); FN(vkCreateFence); FN(vkWaitForFences); FN(vkResetFences);
FN(vkResetCommandBuffer); FN(vkDeviceWaitIdle); FN(vkGetPhysicalDeviceFormatProperties);
#undef FN

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int main(void) {
    g_log = fopen(LOG_PATH, "w");
    say("Savage Wind PS5 S4 Vulkan probe");
#ifdef __PROSPERO__
    freopen("/app0/s4_stderr.txt", "w", stderr);  /* RADV's WSI says why it picked a refresh rate on stderr */
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
#define L(name) name = (PFN_##name)GIPA(inst, #name)
    VkInstance inst = VK_NULL_HANDLE;
    L(vkCreateInstance);
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Savage Wind",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
    VkResult r = vkCreateInstance ? vkCreateInstance(&ici, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
    say("vkCreateInstance(1.3, surface + display) = %d", r);
    verdict(r == VK_SUCCESS, "instance");
    if (r) goto out;
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures2);
    L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkCreateDevice); L(vkGetDeviceQueue);
    L(vkGetPhysicalDeviceDisplayPropertiesKHR); L(vkGetDisplayModePropertiesKHR);
    L(vkGetPhysicalDeviceDisplayPlanePropertiesKHR); L(vkGetDisplayPlaneSupportedDisplaysKHR);
    L(vkCreateDisplayPlaneSurfaceKHR); L(vkGetPhysicalDeviceSurfaceSupportKHR);
    L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); L(vkGetPhysicalDeviceSurfaceFormatsKHR);
    L(vkGetPhysicalDeviceSurfacePresentModesKHR); L(vkGetPhysicalDeviceFormatProperties);

    uint32_t npd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &npd, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    say("gpu: %s api %u.%u.%u driver %#x maxImageDimension2D %u maxColorAttachments %u", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.limits.maxImageDimension2D,
        props.limits.maxColorAttachments);
    verdict(props.apiVersion >= VK_API_VERSION_1_3, "device API >= 1.3 (device.c skips older)");

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        say("heap %u: %llu MiB flags %#x", i, (unsigned long long)(mem.memoryHeaps[i].size >> 20),
            mem.memoryHeaps[i].flags);

    /* Extensions device.c uses when present */
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    static const char *const opt[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
        VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, "VK_KHR_unified_image_layouts",
        VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
        VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME};
    say("device extensions: %u", ne);
    for (size_t i = 0; i < sizeof opt / sizeof *opt; i++) say("  %-44s %s", opt[i], has_ext(ep, ne, opt[i]) ? "yes" : "no");
    verdict(has_ext(ep, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME), "VK_KHR_swapchain");

    /* The features device.c requests: required (WANT) and copied core ones */
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    const VkPhysicalDeviceFeatures *h = &f2.features;
    struct { const char *name; VkBool32 have; int required; } feats[] = {
        {"1.3 dynamicRendering", f13.dynamicRendering, 1}, {"1.3 synchronization2", f13.synchronization2, 1},
        {"1.3 shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation, 1},
        {"1.3 shaderTerminateInvocation", f13.shaderTerminateInvocation, 1}, {"1.3 maintenance4", f13.maintenance4, 1},
        {"1.2 timelineSemaphore", f12.timelineSemaphore, 1}, {"1.2 shaderInt8", f12.shaderInt8, 1},
        {"1.2 samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge, 1},
        {"1.2 uniformAndStorageBuffer8BitAccess", f12.uniformAndStorageBuffer8BitAccess, 1},
        {"1.2 storageBuffer8BitAccess", f12.storageBuffer8BitAccess, 1}, {"1.2 scalarBlockLayout", f12.scalarBlockLayout, 1},
        {"1.1 storageBuffer16BitAccess", f11.storageBuffer16BitAccess, 1},
        {"independentBlend", h->independentBlend, 0}, {"dualSrcBlend", h->dualSrcBlend, 0},
        {"fillModeNonSolid", h->fillModeNonSolid, 0}, {"samplerAnisotropy", h->samplerAnisotropy, 0},
        {"textureCompressionBC", h->textureCompressionBC, 0}, {"shaderInt64", h->shaderInt64, 0},
        {"depthBiasClamp", h->depthBiasClamp, 0}, {"depthClamp", h->depthClamp, 0},
        {"occlusionQueryPrecise", h->occlusionQueryPrecise, 0}, {"largePoints", h->largePoints, 0},
        {"wideLines", h->wideLines, 0}, {"imageCubeArray", h->imageCubeArray, 0},
        {"sampleRateShading", h->sampleRateShading, 0}, {"fragmentStoresAndAtomics", h->fragmentStoresAndAtomics, 0},
        {"vertexPipelineStoresAndAtomics", h->vertexPipelineStoresAndAtomics, 0},
        {"robustBufferAccess", h->robustBufferAccess, 0}, {"shaderClipDistance", h->shaderClipDistance, 0},
        {"shaderStorageImageExtendedFormats (PR #194 TAA)", h->shaderStorageImageExtendedFormats, 0},
    };
    int req_ok = 1;
    for (size_t i = 0; i < sizeof feats / sizeof *feats; i++) {
        say("  %-48s %s%s", feats[i].name, feats[i].have ? "yes" : "no", feats[i].required ? " (required)" : "");
        if (feats[i].required && !feats[i].have) req_ok = 0;
    }
    verdict(req_ok, "every feature device.c requires");

    static const struct { VkFormat f; const char *n; } fmts[] = {
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"},
        {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"}, {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4x4"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"}, {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F"}};
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, fmts[i].f, &fp);
        say("  format %-10s optimal %#x", fmts[i].n, fp.optimalTilingFeatures);
    }

    /* Device: one graphics+compute queue, the required feature set */
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[8];
    if (nq > 8) nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfi = 0;
    while (qfi < nq && (qf[qfi].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                           (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        qfi++;
    say("queue families %u, graphics+compute family %u", nq, qfi);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = f13.dynamicRendering, .synchronization2 = f13.synchronization2,
        .shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation,
        .shaderTerminateInvocation = f13.shaderTerminateInvocation, .maintenance4 = f13.maintenance4};
    VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13,
        .timelineSemaphore = f12.timelineSemaphore, .shaderInt8 = f12.shaderInt8,
        .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge,
        .uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess,
        .storageBuffer8BitAccess = f12.storageBuffer8BitAccess, .scalarBlockLayout = f12.scalarBlockLayout};
    VkPhysicalDeviceVulkan11Features e11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12,
        .storageBuffer16BitAccess = f11.storageBuffer16BitAccess};
    VkPhysicalDeviceFeatures2 ef = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11, .features = *h};
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &ef, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext};
    VkDevice dev = VK_NULL_HANDLE;
    uint64_t t0 = now_ns();
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    say("vkCreateDevice(all supported core features + 1.1/1.2/1.3 set) = %d in %.1f ms", r, (now_ns() - t0) / 1e6);
    verdict(r == VK_SUCCESS, "device with Savage Wind's feature set");
    if (r) goto out;
#define D(name) name = (PFN_##name)GIPA(inst, #name)
    D(vkGetDeviceQueue); D(vkCreateSwapchainKHR); D(vkGetSwapchainImagesKHR); D(vkAcquireNextImageKHR);
    D(vkQueuePresentKHR); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkCmdPipelineBarrier2); D(vkCmdClearColorImage); D(vkQueueSubmit); D(vkCreateSemaphore);
    D(vkCreateFence); D(vkWaitForFences); D(vkResetFences); D(vkResetCommandBuffer); D(vkDeviceWaitIdle);
    VkQueue q;
    vkGetDeviceQueue(dev, qfi, 0, &q);

    /* Display: VideoOut through VK_KHR_display */
    uint32_t nd = 1;
    VkDisplayPropertiesKHR dp;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, &dp);
    say("displays: %u (%d) %s %ux%u", nd, r, nd ? dp.displayName : "-", nd ? dp.physicalResolution.width : 0,
        nd ? dp.physicalResolution.height : 0);
    if (!nd) {
        verdict(0, "a display");
        goto out;
    }
    uint32_t nm = 0;
    vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, NULL);
    VkDisplayModePropertiesKHR modes[16];
    if (nm > 16) nm = 16;
    vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, modes);
    uint32_t best = 0;
    for (uint32_t i = 0; i < nm; i++) {
        say("  mode %u: %ux%u @ %u mHz", i, modes[i].parameters.visibleRegion.width,
            modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate);
        if (modes[i].parameters.refreshRate > modes[best].parameters.refreshRate) best = i;
    }
    verdict(modes[best].parameters.visibleRegion.width == 3840 && modes[best].parameters.visibleRegion.height == 2160,
            "a 3840x2160 display mode");
    verdict(modes[best].parameters.refreshRate >= 119000, "a 119.88 Hz mode offered");
    uint32_t nplane = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &nplane, NULL);
    VkDisplaySurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = modes[best].displayMode, .planeIndex = 0, .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR, .imageExtent = modes[best].parameters.visibleRegion};
    VkSurfaceKHR surf;
    r = vkCreateDisplayPlaneSurfaceKHR(inst, &sci, NULL, &surf);
    say("planes %u; vkCreateDisplayPlaneSurfaceKHR(mode %u) = %d", nplane, best, r);
    verdict(r == VK_SUCCESS, "display plane surface");
    if (r) goto out;
    VkBool32 sup = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, qfi, surf, &sup);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    uint32_t nf = 8, npm = 8;
    VkSurfaceFormatKHR sf[8];
    VkPresentModeKHR pm[8];
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pm);
    say("surface: support %u, images %u-%u, extent %ux%u, %u formats (first %d/%d), %u present modes", sup,
        caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height, nf,
        sf[0].format, sf[0].colorSpace, npm);
    for (uint32_t i = 0; i < nf; i++) say("  format %d colorspace %d", sf[i].format, sf[i].colorSpace);
    for (uint32_t i = 0; i < npm; i++) say("  present mode %d", pm[i]);

    uint32_t nimg = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
        .minImageCount = nimg, .imageFormat = sf[0].format, .imageColorSpace = sf[0].colorSpace,
        .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkSwapchainKHR sc;
    r = vkCreateSwapchainKHR(dev, &swci, NULL, &sc);
    say("vkCreateSwapchainKHR(%ux%u, %u images, FIFO) = %d", swci.imageExtent.width, swci.imageExtent.height, nimg, r);
    verdict(r == VK_SUCCESS, "4K FIFO swapchain");
    if (r) goto out;
    uint32_t nsi = 8;
    VkImage si[8];
    vkGetSwapchainImagesKHR(dev, sc, &nsi, si);

    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfi};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpi, NULL, &pool);
    enum { FRAMES = 2 };
    VkCommandBuffer cb[FRAMES];
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .commandBufferCount = FRAMES};
    vkAllocateCommandBuffers(dev, &cai, cb);
    VkSemaphore acq[FRAMES], done[8];
    VkFence fence[FRAMES];
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < FRAMES; i++) vkCreateSemaphore(dev, &semi, NULL, &acq[i]), vkCreateFence(dev, &fci, NULL, &fence[i]);
    for (uint32_t i = 0; i < nsi; i++) vkCreateSemaphore(dev, &semi, NULL, &done[i]);

    /* 6 s of presents; intervals after the first second (mode switch settles) */
    enum { N = 720 };
    static uint64_t at[N];
    int presented = 0;
    for (int frame = 0; frame < N; frame++) {
        int f = frame % FRAMES;
        vkWaitForFences(dev, 1, &fence[f], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence[f]);
        uint32_t idx;
        r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[f], VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            say("acquire frame %d = %d", frame, r);
            break;
        }
        vkResetCommandBuffer(cb[f], 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb[f], &bi);
        VkImageMemoryBarrier2 b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = si[idx],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b1};
        vkCmdPipelineBarrier2(cb[f], &di);
        float v = (float)(frame % 120) / 119.0f;  /* a ramp: a visible 1 s cycle at 120 Hz */
        VkClearColorValue c = {.float32 = {v * 0.2f, 0.25f + 0.5f * v, 0.35f, 1}};
        vkCmdClearColorImage(cb[f], si[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b1.subresourceRange);
        VkImageMemoryBarrier2 b2 = b1;
        b2.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b2.dstAccessMask = 0;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        di.pImageMemoryBarriers = &b2;
        vkCmdPipelineBarrier2(cb[f], &di);
        vkEndCommandBuffer(cb[f]);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si1 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq[f],
                            .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb[f],
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &done[idx]};
        vkQueueSubmit(q, 1, &si1, fence[f]);
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &done[idx], .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx};
        r = vkQueuePresentKHR(q, &pi);
        at[frame] = now_ns();
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            say("present frame %d = %d", frame, r);
            break;
        }
        presented++;
    }
    vkDeviceWaitIdle(dev);
    if (presented > 240) {
        uint64_t span = at[presented - 1] - at[120];
        double hz = (presented - 1 - 120) / (span / 1e9);
        uint64_t worst = 0;
        for (int i = 121; i < presented; i++)
            if (at[i] - at[i - 1] > worst) worst = at[i] - at[i - 1];
        say("presented %d frames; settled rate %.2f Hz (%.3f ms/frame), worst interval %.2f ms", presented, hz,
            1000.0 / hz, worst / 1e6);
        verdict(hz > 115 && hz < 125, "FIFO presents at ~119.88 Hz");
    } else {
        verdict(0, "presents");
    }
out:
    say("done: %d failed check(s)", g_fail);
    if (g_log) fclose(g_log);
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) usleep(100000);
#endif
    return g_fail != 0;
}
