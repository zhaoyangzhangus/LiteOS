#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <stdlib.h>
#include <stdio.h>

/* Own a minimal transfer/compute + H264 decode or encode device. FFmpeg's automatic
 * device enables unrelated encode/optical-flow engines and all their queues. */
struct video_device {
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance;
};
static const char *extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};
static const char *encode_extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME};
static int video_choose_queue_families(const VkQueueFamilyProperties2 *queues,
                                      const VkQueueFamilyVideoPropertiesKHR *video,
                                      uint32_t count, int encode,
                                      int *transfer, int *codec_queue) {
    VkQueueFlags required_queue = encode ? VK_QUEUE_VIDEO_ENCODE_BIT_KHR : VK_QUEUE_VIDEO_DECODE_BIT_KHR;
    VkVideoCodecOperationFlagsKHR required_codec = encode ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR : VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    *transfer = *codec_queue = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!queues[i].queueFamilyProperties.queueCount) continue;
        VkQueueFlags flags = queues[i].queueFamilyProperties.queueFlags;
        if (*transfer < 0 && (flags & VK_QUEUE_COMPUTE_BIT)) *transfer = (int)i;
        if (*codec_queue < 0 && (flags & required_queue) &&
            (video[i].videoCodecOperations & required_codec)) *codec_queue = (int)i;
    }
    return *transfer >= 0 && *codec_queue >= 0 && *transfer != *codec_queue;
}
static void release(AVHWDeviceContext *ctx) {
    AVVulkanDeviceContext *vk=ctx->hwctx;
    if(vk->act_dev){vkDeviceWaitIdle(vk->act_dev);vkDestroyDevice(vk->act_dev,NULL);}
    if(vk->inst)vkDestroyInstance(vk->inst,NULL);
    free(ctx->user_opaque);
}
#define CHECK(call) do {VkResult r=(call);if(r!=VK_SUCCESS){fprintf(stderr,"VIDEO_DEVICE_FAIL %s result=%d\n",#call,r);goto fail;}}while(0)
static int video_device_create_mode(AVBufferRef **output, int encode) {
    if (!output) return AVERROR(EINVAL);
    *output = NULL;
    AVBufferRef *hw=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if(!hw)return AVERROR(ENOMEM);
    AVHWDeviceContext *ctx=(AVHWDeviceContext*)hw->data;
    AVVulkanDeviceContext *vk=ctx->hwctx;
    struct video_device *own=calloc(1,sizeof(*own));
    if(!own){av_buffer_unref(&hw);return AVERROR(ENOMEM);}
    ctx->user_opaque=own;ctx->free=release;
    vk->get_proc_addr=vkGetInstanceProcAddr;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Vulkan Video",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ic,NULL,&vk->inst));
    uint32_t n=0;CHECK(vkEnumeratePhysicalDevices(vk->inst,&n,NULL));
    if(!n||n>32)goto fail;
    VkPhysicalDevice devices[32];CHECK(vkEnumeratePhysicalDevices(vk->inst,&n,devices));
    for(uint32_t i=0;i<n;++i) {
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        if(p.vendorID==0x10de&&p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){vk->phys_dev=devices[i];break;}
    }
    if(!vk->phys_dev)goto fail;
    own->f13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    if (encode) {
        own->video_maintenance.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
        own->f13.pNext=&own->video_maintenance;
    }
    own->f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    own->f12.pNext=&own->f13;
    own->f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    own->f11.pNext=&own->f12;
    VkPhysicalDeviceFeatures2 available={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&own->f11};
    vkGetPhysicalDeviceFeatures2(vk->phys_dev,&available);
    if(!own->f12.timelineSemaphore||!own->f13.synchronization2||
       (encode&&!own->video_maintenance.videoMaintenance1))goto fail;
    VkBool32 ycbcr=own->f11.samplerYcbcrConversion,bda=own->f12.bufferDeviceAddress,m4=own->f13.maintenance4;
    own->f11=(VkPhysicalDeviceVulkan11Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&own->f12,.samplerYcbcrConversion=ycbcr};
    own->f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&own->f13,.timelineSemaphore=1,.bufferDeviceAddress=bda};
    own->f13=(VkPhysicalDeviceVulkan13Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=1,.maintenance4=m4};
    if (encode) own->f13.pNext=&own->video_maintenance;
    vk->device_features=(VkPhysicalDeviceFeatures2){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&own->f11};
    vkGetPhysicalDeviceQueueFamilyProperties2(vk->phys_dev,&n,NULL);
    if(!n||n>32)goto fail;
    VkQueueFamilyProperties2 qp[32]={0};VkQueueFamilyVideoPropertiesKHR vp[32]={0};
    for(uint32_t i=0;i<n;++i){vp[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;qp[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;qp[i].pNext=&vp[i];}
    vkGetPhysicalDeviceQueueFamilyProperties2(vk->phys_dev,&n,qp);
    const VkVideoCodecOperationFlagsKHR codec = encode ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR : VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    int transfer=-1,decode=-1;
    if(!video_choose_queue_families(qp,vp,n,encode,&transfer,&decode))goto fail;
    float priority=1;
    VkDeviceQueueCreateInfo qi[2]={
        {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=(uint32_t)transfer,.queueCount=1,.pQueuePriorities=&priority},
        {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=(uint32_t)decode,.queueCount=1,.pQueuePriorities=&priority}};
    const char **selected_extensions = encode ? encode_extensions : extensions;
    uint32_t extension_count = encode ? 4U : 3U;
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&vk->device_features,
        .queueCreateInfoCount=2,.pQueueCreateInfos=qi,.enabledExtensionCount=extension_count,.ppEnabledExtensionNames=selected_extensions};
    CHECK(vkCreateDevice(vk->phys_dev,&dc,NULL,&vk->act_dev));
    vk->enabled_dev_extensions=selected_extensions;vk->nb_enabled_dev_extensions=extension_count;
    vk->qf[0]=(AVVulkanDeviceQueueFamily){.idx=transfer,.num=1,.flags=qp[transfer].queueFamilyProperties.queueFlags};
    vk->qf[1]=(AVVulkanDeviceQueueFamily){.idx=decode,.num=1,.flags=qp[decode].queueFamilyProperties.queueFlags,.video_caps=codec};
    vk->nb_qf=2;
    /* Deprecated fixed indices still form part of the FFmpeg 7 ABI. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    vk->queue_family_index=transfer;vk->nb_graphics_queues=1;
    vk->queue_family_tx_index=transfer;vk->nb_tx_queues=1;
    vk->queue_family_comp_index=transfer;vk->nb_comp_queues=1;
    vk->queue_family_decode_index=encode ? -1 : decode;vk->nb_decode_queues=encode ? 0 : 1;
    vk->queue_family_encode_index=encode ? decode : -1;vk->nb_encode_queues=encode ? 1 : 0;
#pragma GCC diagnostic pop
    int err=av_hwdevice_ctx_init(hw);
    if(err<0){av_buffer_unref(&hw);return err;}
    if (encode) printf("VIDEO_ENCODE_DEVICE_READY transfer_family=%d encode_family=%d queues=2\n",transfer,decode);
    else printf("VIDEO_DEVICE_READY transfer_family=%d decode_family=%d queues=2\n",transfer,decode);
    *output=hw;return 0;
fail:
    av_buffer_unref(&hw);return AVERROR_EXTERNAL;
}
int video_device_create(AVBufferRef **output) {
    return video_device_create_mode(output, 0);
}
int video_encode_device_create(AVBufferRef **output) {
    return video_device_create_mode(output, 1);
}
