/* FSR4 RADV feasibility probe. SPDX-License-Identifier: GPL-3.0-or-later */
#include <vulkan/vulkan.h>
#include <ps5fsr4/ps5_fsr4.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int sceSystemServiceLoadExec(const char *, const char *const *);
static FILE *logfile;
void ps5_logf(const char *fmt, ...) {
    char line[512]; va_list a; va_start(a, fmt); vsnprintf(line, sizeof(line), fmt, a); va_end(a);
    fputs(line, stdout); fflush(stdout); if (logfile) { fputs(line, logfile); fflush(logfile); }
}
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { ps5_logf("FSR4_ERROR %s result=%d\n", #x, r_); return 1; } } while (0)
#define RW 1280u
#define RH 720u
#define OW 1920u
#define OH 1080u
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mem;
static uint32_t type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i=0; i<mem.memoryTypeCount; ++i) if ((bits & (1u<<i)) && (mem.memoryTypes[i].propertyFlags & flags)==flags) return i;
    return UINT32_MAX;
}
struct image { VkImage image; VkImageView view; VkDeviceMemory memory; };
static int image(struct image *im, VkFormat fmt, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=fmt,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateImage(dev,&ci,NULL,&im->image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(dev,im->image,&req);
    uint32_t t=type(req.memoryTypeBits,0); if (t==UINT32_MAX) return 1;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=t};
    CHECK(vkAllocateMemory(dev,&ai,NULL,&im->memory)); CHECK(vkBindImageMemory(dev,im->image,im->memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=im->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=fmt,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(dev,&vi,NULL,&im->view)); return 0;
}
static void barrier(VkCommandBuffer cmd,VkImage im,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags s,VkPipelineStageFlags d) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=src,.dstAccessMask=dst,.oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=im,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,s,d,0,0,NULL,0,NULL,1,&b);
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipelines) {
    static uint32_t index; uint32_t first=index; index+=count;
    ps5_logf("FSR4_PIPELINE_BEGIN index=%u count=%u\n",first,count);
    double start=now_ms(); VkResult result=__real_vkCreateComputePipelines(device,cache,count,infos,alloc,pipelines);
    ps5_logf("FSR4_PIPELINE index=%u count=%u result=%d create_ms=%.3f\n",first,count,result,now_ms()-start);
    return result;
}
static int run(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="FSR4 RADV probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app}; VkInstance inst; CHECK(vkCreateInstance(&ici,NULL,&inst));
    uint32_t n=1; VkPhysicalDevice pd; CHECK(vkEnumeratePhysicalDevices(inst,&n,&pd)); if (!n) { ps5_logf("FSR4_ERROR no physical device\n"); return 1; }
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13};
    VkPhysicalDeviceVulkan11Features f11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&f12};
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f11}; vkGetPhysicalDeviceFeatures2(pd,&f2);
    VkPhysicalDeviceShaderIntegerDotProductProperties dot={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sc={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,.pNext=&dot};
    VkPhysicalDeviceSubgroupProperties sg={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,.pNext=&sc};
    VkPhysicalDeviceProperties2 prop={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&sg}; vkGetPhysicalDeviceProperties2(pd,&prop);
    ps5_logf("FSR4_DEVICE %s api=%u.%u int16=%u int8=%u float16=%u dot=%u storage8=%u storage16=%u subgroup_ops=%#x subgroup_size=%u size_control=%u range=%u-%u stages=%#x timestampPeriod=%g\n",prop.properties.deviceName,VK_API_VERSION_MAJOR(prop.properties.apiVersion),VK_API_VERSION_MINOR(prop.properties.apiVersion),f2.features.shaderInt16,f12.shaderInt8,f12.shaderFloat16,f13.shaderIntegerDotProduct,f12.storageBuffer8BitAccess,f11.storageBuffer16BitAccess,sg.supportedOperations,sg.subgroupSize,f13.subgroupSizeControl,sc.minSubgroupSize,sc.maxSubgroupSize,sc.requiredSubgroupSizeStages,prop.properties.limits.timestampPeriod);
    ps5_logf("FSR4_DOT packed_signed_accelerated=%u packed_unsigned_accelerated=%u\n",dot.integerDotProduct4x8BitPackedSignedAccelerated,dot.integerDotProduct4x8BitPackedUnsignedAccelerated);
    const VkFormat formats[]={VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT};
    for (unsigned i=0;i<3;i++) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(pd,formats[i],&p); ps5_logf("FSR4_FORMAT format=%d optimal=%#x\n",formats[i],p.optimalTilingFeatures); }
    if (!f2.features.shaderInt16 || !f12.shaderInt8 || !(sg.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT)) { ps5_logf("FSR4_ERROR missing required shader feature\n"); return 1; }
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,NULL); VkQueueFamilyProperties *q=calloc(nq,sizeof(*q)); if (!q) return 1; vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,q);
    uint32_t qi=0; while(qi<nq && (!(q[qi].queueFlags&VK_QUEUE_COMPUTE_BIT) || !q[qi].timestampValidBits)) qi++; if(qi==nq) { free(q); ps5_logf("FSR4_ERROR no compute timestamp queue\n"); return 1; } uint32_t valid=q[qi].timestampValidBits; free(q);
    float prio=1; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&prio};
    VkPhysicalDeviceVulkan13Features e13=f13; e13.pNext=NULL;
    VkPhysicalDeviceVulkan12Features e12=f12; e12.pNext=&e13;
    VkPhysicalDeviceVulkan11Features e11=f11; e11.pNext=&e12;
    VkPhysicalDeviceFeatures enabled=f2.features;
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&e11,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.pEnabledFeatures=&enabled}; CHECK(vkCreateDevice(pd,&di,NULL,&dev)); VkQueue queue; vkGetDeviceQueue(dev,qi,0,&queue); vkGetPhysicalDeviceMemoryProperties(pd,&mem);
    struct image im[4]; if(image(&im[0],formats[0],RW,RH)||image(&im[1],formats[1],RW,RH)||image(&im[2],formats[2],RW,RH)||image(&im[3],formats[2],OW,OH)) return 1;
    VkPipelineCacheCreateInfo pci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; VkPipelineCache cache; CHECK(vkCreatePipelineCache(dev,&pci,NULL,&cache));
    int wave=f13.subgroupSizeControl && sc.minSubgroupSize<=64 && sc.maxSubgroupSize>=64 && (sc.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT);
    ps5fsr4_context_desc cd={sizeof(cd),pd,dev,RW,RH,OW,OH,PS5FSR4_FLAG_HIGH_DYNAMIC_RANGE|PS5FSR4_FLAG_AUTO_EXPOSURE|(wave?PS5FSR4_FLAG_SUBGROUP_SIZE_CONTROL:0),NULL,cache};
    ps5fsr4_context *ctx=NULL; double start=now_ms(); ps5_logf("FSR4_CONTEXT_BEGIN wave64=%d\n",wave); ps5fsr4_result fr=ps5fsr4_context_create(&cd,&ctx); ps5_logf("FSR4_CONTEXT result=%d pipeline_create_ms=%.3f\n",fr,now_ms()-start); if(fr) return 1;
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=qi}; VkCommandPool pool; CHECK(vkCreateCommandPool(dev,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev,&cai,&cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(dev,&fi,NULL,&fence));
    VkQueryPoolCreateInfo qp={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2}; VkQueryPool queries; CHECK(vkCreateQueryPool(dev,&qp,NULL,&queries));
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)OW*OH*16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT}; VkBuffer out; CHECK(vkCreateBuffer(dev,&bi,NULL,&out)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,out,&mr); uint32_t mt=type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); if(mt==UINT32_MAX) { ps5_logf("FSR4_ERROR no coherent readback\n"); return 1; } VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt}; VkDeviceMemory om; CHECK(vkAllocateMemory(dev,&ma,NULL,&om)); CHECK(vkBindBufferMemory(dev,out,om,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
    const VkClearColorValue clear[3]={{{0.4f,0.3f,0.2f,1.0f}},{{0.5f,0,0,0}},{{0,0,0,0}}}; VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for(unsigned i=0;i<4;i++) { barrier(cmd,im[i].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); if(i<3) vkCmdClearColorImage(cmd,im[i].image,VK_IMAGE_LAYOUT_GENERAL,&clear[i],1,&range); barrier(cmd,im[i].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); }
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd}; CHECK(vkQueueSubmit(queue,1,&si,fence)); CHECK(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX)); CHECK(vkResetFences(dev,1,&fence)); CHECK(vkResetCommandBuffer(cmd,0));
    uint32_t phases=ps5fsr4_jitter_phase_count(RW,OW);
    for(uint32_t frame=0;frame<8;frame++) {
        float x,y; ps5fsr4_jitter_offset(frame,phases,&x,&y); ps5fsr4_dispatch_desc dd={.struct_size=sizeof(dd),.command_buffer=cmd,.color=im[0].view,.depth=im[1].view,.motion_vectors=im[2].view,.output=im[3].view,.color_layout=VK_IMAGE_LAYOUT_GENERAL,.depth_layout=VK_IMAGE_LAYOUT_GENERAL,.motion_vectors_layout=VK_IMAGE_LAYOUT_GENERAL,.render_width=RW,.render_height=RH,.jitter_x=x,.jitter_y=y,.motion_vector_scale_x=RW,.motion_vector_scale_y=RH,.pre_exposure=1,.frame_time_delta_ms=16.666667f,.camera_near=0.1f,.camera_far=200,.camera_fov_vertical=1.0471976f,.reset=frame==0};
        CHECK(vkBeginCommandBuffer(cmd,&begin)); vkCmdResetQueryPool(cmd,queries,0,2); vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0); fr=ps5fsr4_dispatch(ctx,&dd); if(fr) { ps5_logf("FSR4_ERROR dispatch=%d frame=%u\n",fr,frame); return 1; } vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
        barrier(cmd,im[3].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={OW,OH,1}}; vkCmdCopyImageToBuffer(cmd,im[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,out,1,&copy); barrier(cmd,im[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT}; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
        CHECK(vkEndCommandBuffer(cmd)); CHECK(vkQueueSubmit(queue,1,&si,fence)); CHECK(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX)); uint64_t ts[2]; CHECK(vkGetQueryPoolResults(dev,queries,0,2,sizeof(ts),ts,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)); uint64_t delta=ts[1]-ts[0]; if(valid<64) delta &= (UINT64_C(1)<<valid)-1;
        unsigned char *p; CHECK(vkMapMemory(dev,om,0,VK_WHOLE_SIZE,0,(void**)&p)); uint64_t hash=UINT64_C(14695981039346656037); for(size_t j=0;j<(size_t)OW*OH*16;j++) { hash^=p[j]; hash*=UINT64_C(1099511628211); } vkUnmapMemory(dev,om); ps5_logf("FSR4_FRAME frame=%u gpu_ms=%.3f checksum_fnv1a64=%016llx reference=unavailable\n",frame,delta*prop.properties.limits.timestampPeriod/1e6,(unsigned long long)hash); CHECK(vkResetFences(dev,1,&fence)); CHECK(vkResetCommandBuffer(cmd,0));
    }
    ps5fsr4_context_destroy(ctx); ps5_logf("FSR4_COMPLETE\n"); return 0;
}
int main(void) { logfile=fopen("/app0/fsr4_radv.txt","w"); ps5_logf("FSR4_BEGIN\n"); int r=run(); ps5_logf("FSR4_END result=%s\n",r?"FAIL":"PASS"); if(logfile) fclose(logfile); sceSystemServiceLoadExec("exit",NULL); for(;;) usleep(100000); }
