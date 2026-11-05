/*
 * Offline microbenchmark: CPU staging cost of the c690 0x2A Vulkan arenas.
 *
 * The live prepare copies every job's source arrays, offset list and seed into
 * three host-visible, host-coherent buffers (32 MiB / 4 MiB / 32 MiB). On RADV
 * the first coherent type is usually GTT write-combined, which can make those
 * copies several times slower than cached memory. This program allocates the
 * same three buffers with the backend's selection rule, then times the
 * per-frame copy volume measured live (232,588 jobs over 1,104 frames).
 *
 * It also reports every host-visible memory type so a cached alternative is
 * visible. Build/run:
 *   cc -O2 -o /tmp/bench_c690_arena tools/bench_c690_arena.c -ldl
 *   /tmp/bench_c690_arena
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define L(n) static PFN_##n p##n
L(vkCreateInstance); L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
L(vkCreateBuffer); L(vkGetBufferMemoryRequirements);
L(vkGetPhysicalDeviceMemoryProperties); L(vkAllocateMemory); L(vkBindBufferMemory);
L(vkMapMemory); L(vkUnmapMemory); L(vkDestroyBuffer); L(vkFreeMemory);
L(vkDestroyDevice); L(vkDestroyInstance);

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int find_type(VkPhysicalDeviceMemoryProperties* mp, uint32_t bits,
                     VkMemoryPropertyFlags flags, uint32_t* out)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & flags) == flags) {
            *out = i; return 0;
        }
    return -1;
}

int main(void)
{
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts("no libvulkan"); return 1; }
    pvkCreateInstance = (PFN_vkCreateInstance)dlsym(lib, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (pvkCreateInstance(&ici, NULL, &inst)) { puts("instance failed"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GETG(n) p##n = (PFN_##n)gipa(inst, #n)
    GETG(vkEnumeratePhysicalDevices); GETG(vkGetPhysicalDeviceProperties);
    GETG(vkGetPhysicalDeviceQueueFamilyProperties); GETG(vkCreateDevice);
    GETG(vkGetDeviceProcAddr); GETG(vkGetPhysicalDeviceMemoryProperties);
    uint32_t nd = 0; pvkEnumeratePhysicalDevices(inst, &nd, NULL);
    VkPhysicalDevice pd[8]; if (nd > 8) nd = 8;
    pvkEnumeratePhysicalDevices(inst, &nd, pd);
    VkPhysicalDevice chosen = VK_NULL_HANDLE; uint32_t fam = 0;
    for (uint32_t d = 0; d < nd && chosen == VK_NULL_HANDLE; d++) {
        uint32_t nq = 0;
        pvkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, NULL);
        VkQueueFamilyProperties q[64]; if (nq > 64) nq = 64;
        pvkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, q);
        for (uint32_t i = 0; i < nq; i++)
            if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { chosen = pd[d]; fam = i; break; }
    }
    if (chosen == VK_NULL_HANDLE) { puts("no compute device"); return 2; }
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = fam; qci.queueCount = 1;
    float prio = 1.0f; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (pvkCreateDevice(chosen, &dci, NULL, &dev)) { puts("device failed"); return 1; }
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define GETD(n) p##n = (PFN_##n)gdpa(dev, #n)
    GETD(vkCreateBuffer); GETD(vkGetBufferMemoryRequirements);
    GETD(vkAllocateMemory); GETD(vkBindBufferMemory); GETD(vkMapMemory);
    GETD(vkDestroyDevice);
    VkPhysicalDeviceMemoryProperties mp;
    pvkGetPhysicalDeviceMemoryProperties(chosen, &mp);
    printf("memory types:\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        printf("  %u: %s%s%s%s%s heap=%u\n", i,
               (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
               (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
               (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
               (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
               (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZY " : "",
               mp.memoryTypes[i].heapIndex);
    }

    const VkDeviceSize sizes[3] = { 32u << 20, 4u << 20, 32u << 20 };
    VkBuffer buf[3]; VkDeviceMemory mem[3]; void* ptr[3];
    VkMemoryPropertyFlags used[3];
    for (int b = 0; b < 3; b++) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = sizes[b]; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (pvkCreateBuffer(dev, &bci, NULL, &buf[b])) { puts("buffer failed"); return 1; }
        VkMemoryRequirements req; pvkGetBufferMemoryRequirements(dev, buf[b], &req);
        const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0; used[b] = coh;
        if (find_type(&mp, req.memoryTypeBits, coh, &type) != 0) {
            used[b] = vis;
            if (find_type(&mp, req.memoryTypeBits, vis, &type) != 0) { puts("no host mem"); return 1; }
        }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = type;
        if (pvkAllocateMemory(dev, &mai, NULL, &mem[b]) ||
            pvkBindBufferMemory(dev, buf[b], mem[b], 0) ||
            pvkMapMemory(dev, mem[b], 0, VK_WHOLE_SIZE, 0, &ptr[b])) {
            puts("mem bind/map failed"); return 1;
        }
        printf("buffer %d: type %u flags %s%s\n", b, type,
               (used[b] & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "coherent " : "",
               (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                   ? "cached" : "uncached/wc");
    }

    /* Live volume: 232,588 jobs over 1,104 frames = 210.7 jobs/frame;
     * 51.4 jobs/batch, 4.1 batches/frame. Per job: extent*4 (~25.5 KiB) in,
     * count*2 (~0.8 KiB) offsets, count*32 (~12.7 KiB) seed. */
    enum { FRAMES = 200, JOBS_PER_FRAME = 211, EXTENT_BYTES = 26112,
           OFF_BYTES = 816, SEED_BYTES = 12736 };
    uint8_t* src = (uint8_t*)malloc(EXTENT_BYTES);
    memset(src, 0x5A, EXTENT_BYTES);
    uint64_t t0 = now_ns();
    uint64_t total = 0;
    for (int f = 0; f < FRAMES; f++) {
        uint32_t in_off = 0, off_off = 0, out_off = 0;
        for (int j = 0; j < JOBS_PER_FRAME; j++) {
            const uint32_t in_at = (in_off + 255u) & ~255u;
            const uint32_t of_at = (off_off + 255u) & ~255u;
            const uint32_t out_at = (out_off + 255u) & ~255u;
            if (in_at + EXTENT_BYTES > (32u << 20) ||
                of_at + OFF_BYTES > (4u << 20) ||
                out_at + SEED_BYTES > (32u << 20)) { in_off = off_off = out_off = 0; }
            memcpy((uint8_t*)ptr[0] + in_at, src, EXTENT_BYTES);
            memcpy((uint8_t*)ptr[1] + of_at, src, OFF_BYTES);
            memcpy((uint8_t*)ptr[2] + out_at, src, SEED_BYTES);
            in_off = in_at + EXTENT_BYTES;
            off_off = of_at + OFF_BYTES;
            out_off = out_at + SEED_BYTES;
            total += EXTENT_BYTES + OFF_BYTES + SEED_BYTES;
        }
    }
    const uint64_t t1 = now_ns();
    printf("staging: %.2f MB/frame, %.0f us/frame, %.2f GB/s (coherent-first rule)\n",
           (double)total / FRAMES / (1024.0 * 1024.0),
           (double)(t1 - t0) / 1000.0 / FRAMES,
           (double)total / (double)(t1 - t0));

    /* Same volume into a cached host-visible type when one exists. */
    VkMemoryRequirements req; pvkGetBufferMemoryRequirements(dev, buf[0], &req);
    uint32_t ct = 0;
    if (find_type(&mp, req.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  &ct) == 0) {
        VkBuffer cb; VkDeviceMemory cm; void* cp;
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = 32u << 20; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        pvkCreateBuffer(dev, &bci, NULL, &cb);
        pvkGetBufferMemoryRequirements(dev, cb, &req);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = ct;
        if (pvkAllocateMemory(dev, &mai, NULL, &cm) == VK_SUCCESS &&
            pvkBindBufferMemory(dev, cb, cm, 0) == VK_SUCCESS &&
            pvkMapMemory(dev, cm, 0, VK_WHOLE_SIZE, 0, &cp) == VK_SUCCESS) {
            uint64_t u0 = now_ns(), u = 0;
            for (int f = 0; f < FRAMES; f++) {
                uint32_t at = 0;
                for (int j = 0; j < JOBS_PER_FRAME; j++) {
                    at = (at + 255u) & ~255u;
                    if (at + EXTENT_BYTES > (32u << 20)) at = 0;
                    memcpy((uint8_t*)cp + at, src, EXTENT_BYTES);
                    at += EXTENT_BYTES; u += EXTENT_BYTES;
                }
            }
            uint64_t u1 = now_ns();
            printf("cached type %u: %.2f MB/frame, %.0f us/frame, %.2f GB/s\n", ct,
                   (double)u / FRAMES / (1024.0 * 1024.0),
                   (double)(u1 - u0) / 1000.0 / FRAMES,
                   (double)u / (double)(u1 - u0));
        }
    } else {
        printf("no cached host-visible type\n");
    }

    free(src);
    pvkDestroyDevice(dev, NULL);
    return 0;
}
