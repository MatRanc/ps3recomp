/*
 * Offline Vulkan validation arm for the async c690 hand-off.
 *
 * Mirrors the object/submit pattern in libs/video/rsx_vulkan_backend.c's
 * vk_c690_* path: a compute-only queue family, a timeline semaphore the
 * compute signals, a compute->host/transfer pipeline barrier, and 200
 * submit/poll/wait rounds. It is not part of the default build (it needs a
 * Vulkan device); build and run it with:
 *
 *   cc -O1 -Ilibs/video/vulkan -o /tmp/vk_c690_timeline_validate \
 *       tools/c690_vk_timeline_validate.c -ldl
 *   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
 *       /tmp/vk_c690_timeline_validate
 *
 * The validation layer found VUID-vkCmdPipelineBarrier-dstStageMask-06462 in
 * the first version (VERTEX_SHADER in a compute-queue barrier); the backend
 * now uses compute/transfer/host stages only.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "c690_2a_32b_comp.spv.h"

#define L(n) static PFN_##n p##n
L(vkCreateInstance); L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceFeatures2);
L(vkEnumerateDeviceExtensionProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
L(vkGetDeviceQueue); L(vkCreateBuffer); L(vkGetBufferMemoryRequirements);
L(vkGetPhysicalDeviceMemoryProperties); L(vkAllocateMemory); L(vkBindBufferMemory);
L(vkMapMemory); L(vkCreateShaderModule); L(vkCreateDescriptorSetLayout);
L(vkCreatePipelineLayout); L(vkCreateComputePipelines); L(vkCreateDescriptorPool);
L(vkAllocateDescriptorSets); L(vkUpdateDescriptorSets); L(vkCreateCommandPool);
L(vkAllocateCommandBuffers); L(vkBeginCommandBuffer); L(vkEndCommandBuffer); L(vkResetCommandBuffer);
L(vkCmdBindPipeline); L(vkCmdBindDescriptorSets); L(vkCmdPushConstants);
L(vkCmdDispatch); L(vkCmdPipelineBarrier); L(vkQueueSubmit); L(vkWaitSemaphores);
L(vkGetSemaphoreCounterValue); L(vkCreateSemaphore); L(vkDestroySemaphore);
L(vkDeviceWaitIdle); L(vkDestroyDevice); L(vkDestroyInstance);

static void* lib;
static VkInstance g_inst;
static void* gipa(const char* n) { return ((PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr"))(g_inst,n); }
#define GETG(n) p##n = (PFN_##n)gipa(#n)
static void* gdpa(void* d, const char* n) { return ((PFN_vkGetDeviceProcAddr)pvkGetDeviceProcAddr)(d,n); }
#define GETD(n) p##n = (PFN_##n)gdpa(dev,#n)

int main(void)
{
    lib = dlopen("libvulkan.so.1", RTLD_NOW|RTLD_LOCAL);
    if (!lib) { puts("no libvulkan"); return 1; }
    GETG(vkCreateInstance); GETG(vkEnumeratePhysicalDevices);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (pvkCreateInstance(&ici, NULL, &inst)) { puts("instance failed"); return 1; }
    g_inst = inst;
    GETG(vkEnumeratePhysicalDevices); GETG(vkGetPhysicalDeviceProperties);
    GETG(vkGetPhysicalDeviceQueueFamilyProperties); GETG(vkGetPhysicalDeviceFeatures2);
    GETG(vkEnumerateDeviceExtensionProperties); GETG(vkCreateDevice);
    GETG(vkGetDeviceProcAddr); GETG(vkGetPhysicalDeviceMemoryProperties);
    uint32_t nd = 0; pvkEnumeratePhysicalDevices(inst,&nd,NULL);
    VkPhysicalDevice pd[8]; if (nd>8) nd=8; pvkEnumeratePhysicalDevices(inst,&nd,pd);
    int chosen=-1, gfam=-1, cfam=-1;
    for (uint32_t d=0; d<nd && chosen<0; d++) {
        VkPhysicalDeviceProperties p; pvkGetPhysicalDeviceProperties(pd[d],&p);
        uint32_t nq=0; pvkGetPhysicalDeviceQueueFamilyProperties(pd[d],&nq,NULL);
        VkQueueFamilyProperties q[64]; if (nq>64)nq=64; pvkGetPhysicalDeviceQueueFamilyProperties(pd[d],&nq,q);
        gfam=-1; cfam=-1;
        for (uint32_t i=0;i<nq;i++) {
            if ((q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && gfam<0) gfam=(int)i;
            if (!(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) &&
                (q[i].queueFlags&VK_QUEUE_COMPUTE_BIT) && cfam<0) cfam=(int)i;
        }
        if (gfam>=0 && cfam>=0) { chosen=(int)d; printf("device %s: graphics family %d, compute-only family %d\n", p.deviceName, gfam, cfam); }
    }
    if (chosen<0) { puts("no device with a compute-only family"); return 2; }
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext=&tl; pvkGetPhysicalDeviceFeatures2(pd[chosen],&f2);
    printf("timelineSemaphore=%u\n", tl.timelineSemaphore);
    if (!tl.timelineSemaphore) return 2;

    float prio[2]={1.0f,0.5f}, cprio[1]={1.0f};
    VkDeviceQueueCreateInfo qci[2] = {
      { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)gfam, 2, prio },
      { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)cfam, 1, cprio } };
    const char* ext[1] = { "VK_KHR_timeline_semaphore" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext=&tl; dci.queueCreateInfoCount=2; dci.pQueueCreateInfos=qci;
    dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=ext;
    VkDevice dev;
    if (pvkCreateDevice(pd[chosen],&dci,NULL,&dev)) { puts("device failed"); return 1; }
    GETD(vkGetDeviceQueue); GETD(vkCreateBuffer); GETD(vkGetBufferMemoryRequirements);
    GETD(vkAllocateMemory); GETD(vkBindBufferMemory);
    GETD(vkMapMemory); GETD(vkCreateShaderModule); GETD(vkCreateDescriptorSetLayout);
    GETD(vkCreatePipelineLayout); GETD(vkCreateComputePipelines); GETD(vkCreateDescriptorPool);
    GETD(vkAllocateDescriptorSets); GETD(vkUpdateDescriptorSets); GETD(vkCreateCommandPool);
    GETD(vkAllocateCommandBuffers); GETD(vkBeginCommandBuffer); GETD(vkEndCommandBuffer); GETD(vkResetCommandBuffer);
    GETD(vkCmdBindPipeline); GETD(vkCmdBindDescriptorSets); GETD(vkCmdPushConstants);
    GETD(vkCmdDispatch); GETD(vkCmdPipelineBarrier); GETD(vkQueueSubmit); GETD(vkWaitSemaphores);
    GETD(vkGetSemaphoreCounterValue); GETD(vkCreateSemaphore); GETD(vkDestroySemaphore);
    GETD(vkDeviceWaitIdle); GETD(vkDestroyDevice);
    VkQueue gq, cq; pvkGetDeviceQueue(dev,(uint32_t)gfam,0,&gq); pvkGetDeviceQueue(dev,(uint32_t)cfam,0,&cq);

    VkPhysicalDeviceMemoryProperties mp; pvkGetPhysicalDeviceMemoryProperties(pd[chosen],&mp);
    VkBuffer bufs[3]; VkDeviceMemory mems[3]; void* ptrs[3];
    for (int b=0;b<3;b++) {
        VkBufferCreateInfo bci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=1u<<20; bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (pvkCreateBuffer(dev,&bci,NULL,&bufs[b])) { puts("buffer failed"); return 1; }
        VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev,bufs[b],&mr);
        uint32_t mt=0; for (uint32_t i=0;i<mp.memoryTypeCount;i++)
            if ((mr.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt=i; break; }
        VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize=mr.size; mai.memoryTypeIndex=mt;
        pvkAllocateMemory(dev,&mai,NULL,&mems[b]);
        pvkBindBufferMemory(dev,bufs[b],mems[b],0);
        pvkMapMemory(dev,mems[b],0,VK_WHOLE_SIZE,0,&ptrs[b]);
        memset(ptrs[b],0,1u<<20);
    }
    VkDescriptorSetLayoutBinding bnd[3];
    for (int i=0;i<3;i++) { bnd[i]=(VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}; }
    VkDescriptorSetLayoutCreateInfo dlci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount=3; dlci.pBindings=bnd; VkDescriptorSetLayout dsl;
    pvkCreateDescriptorSetLayout(dev,&dlci,NULL,&dsl);
    VkPushConstantRange pcr={VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount=1; plci.pSetLayouts=&dsl; plci.pushConstantRangeCount=1; plci.pPushConstantRanges=&pcr;
    VkPipelineLayout play; pvkCreatePipelineLayout(dev,&plci,NULL,&play);
    VkShaderModule sm; VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize=sizeof(c690_2a_32b_comp_spv); smci.pCode=c690_2a_32b_comp_spv;
    if (pvkCreateShaderModule(dev,&smci,NULL,&sm)) { puts("shader failed"); return 1; }
    VkComputePipelineCreateInfo cpci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module=sm; cpci.stage.pName="main";
    cpci.layout=play; VkPipeline pipe;
    if (pvkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpci,NULL,&pipe)) { puts("pipeline failed"); return 1; }
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,3};
    VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&ps; VkDescriptorPool pool;
    pvkCreateDescriptorPool(dev,&dpci,NULL,&pool);
    VkDescriptorSetAllocateInfo dsai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool=pool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&dsl; VkDescriptorSet set;
    pvkAllocateDescriptorSets(dev,&dsai,&set);
    VkDescriptorBufferInfo bi[3]; VkWriteDescriptorSet w[3];
    for (int i=0;i<3;i++) { bi[i]=(VkDescriptorBufferInfo){bufs[i],0,VK_WHOLE_SIZE};
        w[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,(uint32_t)i,0,1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,NULL,&bi[i],NULL}; }
    pvkUpdateDescriptorSets(dev,3,w,0,NULL);
    VkCommandPoolCreateInfo cpci2={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci2.queueFamilyIndex=(uint32_t)cfam;
    VkCommandPool cpool; pvkCreateCommandPool(dev,&cpci2,NULL,&cpool);
    VkCommandBufferAllocateInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool=cpool; cbi.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount=1;
    VkCommandBuffer cb; pvkAllocateCommandBuffers(dev,&cbi,&cb);
    VkSemaphoreTypeCreateInfo tci={VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE; tci.initialValue=0;
    VkSemaphoreCreateInfo sci={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext=&tci;
    VkSemaphore sem; if (pvkCreateSemaphore(dev,&sci,NULL,&sem)) { puts("sem failed"); return 1; }

    int rc=0;
    for (int it=0; it<200; it++) {
        pvkResetCommandBuffer(cb,0);
        VkCommandBufferBeginInfo cbbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pvkBeginCommandBuffer(cb,&cbbi);
        pvkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        const uint32_t dyn[3]={0,0,0};
        pvkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,play,0,1,&set,3,dyn);
        struct { uint32_t pos,nrm,tan,uv,outb,count,p0,p1; } pc={0,0,0,0,0,64,0,0};
        pvkCmdPushConstants(cb,play,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof pc,&pc);
        pvkCmdDispatch(cb,1,1,1);
        VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_HOST_READ_BIT;
        pvkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&mb,0,NULL,0,NULL);
        pvkEndCommandBuffer(cb);
        uint64_t value=(uint64_t)it+1;
        VkTimelineSemaphoreSubmitInfo tsi={VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tsi.signalSemaphoreValueCount=1; tsi.pSignalSemaphoreValues=&value;
        VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext=&tsi;
        si.commandBufferCount=1; si.pCommandBuffers=&cb;
        si.signalSemaphoreCount=1; si.pSignalSemaphores=&sem;
        if (pvkQueueSubmit(cq,1,&si,VK_NULL_HANDLE)) { printf("submit %d failed\n",it); rc=1; break; }
        uint64_t have=0;
        pvkGetSemaphoreCounterValue(dev,sem,&have);
        VkSemaphoreWaitInfo wi={VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount=1; wi.pSemaphores=&sem; wi.pValues=&value;
        pvkWaitSemaphores(dev,&wi,UINT64_MAX);
    }
    pvkDeviceWaitIdle(dev);
    printf("timeline harness: %s (%d iterations)\n", rc?"FAIL":"PASS", 200);
    /* Process exit reclaims the rest; destroying with child objects still
     * live would only add harness-cleanup VUIDs. */
    (void)inst;
    return rc;
}
