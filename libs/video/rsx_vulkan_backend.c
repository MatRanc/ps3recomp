/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#ifndef _WIN32

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "ps3_live_ctl.h"
#include "ps3emu/ps3_trace.h"   /* PS3RECOMP_TIMELINE: present and fence events */
#include "rsx_tex_miss.h"
#include "vk_dump_queue.h"
#include "ps3emu/thread_name.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"
#include "vulkan/vtx_unpack_comp.spv.h"

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* Pipeline creation timing: creation uses s_pipe_cache, which is
 * VK_NULL_HANDLE unless PS3RECOMP_VK_PIPELINE_CACHE is set. These helpers
 * feed the walker profile's `pipecrt`/`pipems` and, under
 * PS3RECOMP_PIPE_TIMING=1, log each creation with its wall time so a
 * scene-change stutter can be attributed. */
extern unsigned long long ps3_ms_now(void);
static void vk_pipe_cache_tick(void);
static void vk_pipe_create_done(const char* what, unsigned long long t0)
{
    const unsigned long long ns = rsx_wprof_now() - t0;
    rsx_wprof_note_pipeline_create(ns);
    {
        uint32_t kind = 4;
        if (what && strcmp(what, "fixed") == 0) kind = 0;
        else if (what && strcmp(what, "guest") == 0) kind = 1;
        else if (what && strcmp(what, "engine") == 0) kind = 2;
        else if (what && strcmp(what, "compute") == 0) kind = 3;
        ps3_trace_instant(PS3_TR_PIPE_CREATE, kind,
                          (uint32_t)(ns / 1000ull > 0xFFFFFFFFull ? 0xFFFFFFFFull : ns / 1000ull));
    }
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_PIPE_TIMING");
        on = e && *e && strcmp(e, "0") != 0;
    }
    if (on)
        fprintf(stderr, "[pipe-create] %s %.1f us at %llu ms\n",
                what, (double)ns / 1000.0, ps3_ms_now());
    vk_pipe_cache_tick();
}

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdDispatch) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBounds) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */
/* Window presents queued to the presentation thread. The live slot count is
 * one less than the swapchain image count (see vk_create_swapchain), so one
 * image is always available to acquire even when every slot is queued. */
#define VK_PRESENT_SLOTS 8
/* Batch sets for VK_FRAME_PIPELINE. Three, not two: with two the walker swaps
 * back into a set the GPU may still be rendering; with three it waits on a set
 * submitted two frames ago, which is normally complete. */
#define VK_EB_SETS 3

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    VkQueue          present_queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    /* Macro Target 2: GPU Vertex Unpack Compute Pipeline */
    VkShaderModule        vtx_unpack_module;
    VkDescriptorSetLayout vtx_unpack_dsl;
    VkPipelineLayout      vtx_unpack_layout;
    VkPipeline            vtx_unpack_pipe;
    VkDescriptorPool      vtx_unpack_pool;
    VkDescriptorSet       vtx_unpack_ds[VK_EB_SETS];

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* A present records and submits on its own pool, command buffer and
     * fences, so its second half can run on a host thread of its own while
     * the FIFO walker records the next frame (rsx_vulkan_backend.h,
     * present_begin/_finish). The queue is shared, and Vulkan wants every
     * vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle on it externally
     * synchronised: queue_mu. present_mu keeps the present's own objects to
     * one present at a time, which matters when a flip in the command stream
     * presents on the walker's thread while a host thread finishes the last. */
    VkCommandPool    present_pool;
    VkCommandBuffer  present_cmd;
    VkFence          present_fence;   /* the copy out of the presented surface */
    int              present_inflight;/* copy submitted, not yet finished      */
    int              present_split;   /* inside rsx_vulkan_backend_present_begin */
    int              present_order;   /* the walker's next command buffer waits */
    pthread_mutex_t  queue_mu;
    pthread_mutex_t  present_mu;

    /* Macro Target 1: a dedicated thread owns vkQueuePresentKHR. The walker
     * records and submits the swapchain blit, hands the image index and its
     * render-finished semaphore to this queue, and returns at once. One slot
     * per in-flight present, so each has its own command buffer and fence. */
    VkCommandBuffer  window_cmd[VK_PRESENT_SLOTS];
    VkFence          window_fence[VK_PRESENT_SLOTS];
    int              window_slot_busy[VK_PRESENT_SLOTS];
    struct {
        u32          slot;
        u32          idx;
        u32          frame;          /* guest flip seq, for the per-flip log */
        unsigned long long due_us;  /* PS3RECOMP_VK_PACE: earliest present */
        unsigned long long submit_us; /* when the blit was submitted (timeline) */
        VkSemaphore  done;
    }                present_q[VK_PRESENT_SLOTS];
    u32              present_q_head, present_q_tail, present_q_count;
    u32              present_slots;
    pthread_mutex_t  present_q_mu;
    pthread_cond_t   present_q_cv;    /* a job is ready for the thread */
    pthread_cond_t   present_slot_cv; /* the thread freed a slot       */
    pthread_t        present_thread;
    int              present_thread_on;
    int              present_quit;
    int              present_lost;    /* vkQueuePresentKHR said recreate */

    /* PS3RECOMP_VK_PACE: the present thread holds each queued present until
     * its scheduled slot, derived from the cadence of NEW guest frames (not
     * from our own presents, which would feed back). The blit is already
     * recorded and submitted when the frame ends; only the
     * vkQueuePresentKHR call is delayed, by at most one frame interval. */
    unsigned long long pace_last_arrival_us, pace_interval_us, pace_due_us;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              depth_bounds_ok; /* depthBounds enabled                */
    int              indep_blend_ok;  /* independentBlend: per-target MRT state */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire[VK_PRESENT_SLOTS]; /* per slot, reused only once
                                       * its queued present has completed */
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
} vk_state;

static vk_state s_vk;

/* Optional persistent VkPipelineCache (PS3RECOMP_VK_PIPELINE_CACHE=1 or a
 * path). Off by default: every creation passes VK_NULL_HANDLE, exactly as
 * before. Keyed by the physical device's pipelineCacheUUID and driver
 * version, so a driver update starts a new file instead of feeding stale
 * blobs to the compiler. */
static VkPipelineCache s_pipe_cache = VK_NULL_HANDLE;
static char s_pipe_cache_path[1024];
static unsigned long long s_pipe_cache_saved;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---- persistent pipeline cache (NHL #128 analogue) --------------------- */
static int vk_pipe_cache_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_VK_PIPELINE_CACHE");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

static void vk_pipe_cache_save(void)
{
    if (!s_pipe_cache || !s_pipe_cache_path[0] || !pvkGetPipelineCacheData) return;
    size_t sz = 0;
    if (pvkGetPipelineCacheData(s_vk.device, s_pipe_cache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void* data = malloc(sz);
    if (!data) return;
    if (pvkGetPipelineCacheData(s_vk.device, s_pipe_cache, &sz, data) != VK_SUCCESS) {
        free(data); return;
    }
    char tmp[sizeof s_pipe_cache_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_pipe_cache_path);
    FILE* f = fopen(tmp, "wb");
    if (f) {
        const size_t w = fwrite(data, 1, sz, f);
        fclose(f);
        if (w == sz) rename(tmp, s_pipe_cache_path);
        else remove(tmp);
    }
    free(data);
}

/* Called after a pipeline was compiled; writing the cache takes a few ms, so
 * rate-limit it. This also covers a run that is killed instead of shut down. */
static void vk_pipe_cache_tick(void)
{
    if (!s_pipe_cache) return;
    const unsigned long long now = ps3_trace_now();
    if (!s_pipe_cache_saved || now - s_pipe_cache_saved > 2000000000ull) {
        s_pipe_cache_saved = now;
        vk_pipe_cache_save();
    }
}

static void vk_pipe_cache_init(void)
{
    if (!vk_pipe_cache_on() || !s_vk.device || !pvkCreatePipelineCache) return;
    const char* e = getenv("PS3RECOMP_VK_PIPELINE_CACHE");
    if (e && strchr(e, '/')) {
        snprintf(s_pipe_cache_path, sizeof s_pipe_cache_path, "%s", e);
    } else {
        const char* dir = getenv("PS3RECOMP_VK_PIPELINE_CACHE_DIR");
        if (!dir || !*dir) dir = getenv("PS3_GAMEDATA_ROOT");
        if (!dir || !*dir) dir = ".";
        VkPhysicalDeviceProperties p;
        memset(&p, 0, sizeof p);
        pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
        char dev[48];
        unsigned d = 0;
        for (const char* s = p.deviceName; *s && d + 1 < sizeof dev; s++)
            dev[d++] = (char)(((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                               (*s >= '0' && *s <= '9')) ? *s : '_');
        dev[d] = 0;
        unsigned long long h = 1469598103934665603ull;
        for (unsigned i = 0; i < VK_UUID_SIZE; i++) {
            h ^= p.pipelineCacheUUID[i];
            h *= 1099511628211ull;
        }
        snprintf(s_pipe_cache_path, sizeof s_pipe_cache_path,
                 "%s/vk_pipe_cache_%s_%08x_%016llx.bin", dir, dev,
                 (unsigned)p.driverVersion, h);
    }
    void* data = NULL;
    size_t sz = 0;
    FILE* f = fopen(s_pipe_cache_path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            const long n = ftell(f);
            if (n > 0 && (unsigned long)n < (256u << 20)) {
                sz = (size_t)n;
                data = malloc(sz);
                if (data) {
                    rewind(f);
                    if (fread(data, 1, sz, f) != sz) { free(data); data = NULL; sz = 0; }
                }
            }
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = sz, .pInitialData = data };
    VkResult r = pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_pipe_cache);
    free(data);
    if (r != VK_SUCCESS) {
        s_pipe_cache = VK_NULL_HANDLE;
        VK_LOG("pipeline cache %s could not be created (%d); creating without one\n",
               s_pipe_cache_path, (int)r);
    } else {
        VK_LOG("pipeline cache %s: %zu bytes loaded\n", s_pipe_cache_path, sz);
    }
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D >= 4096u ? p.limits.maxImageDimension2D : 4096u;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_present_wait_gpu(void);
/* The recording context of the calling thread, or NULL on the state thread. */
static __thread void* vk_t_user;
/* Non-NULL while this thread is recording a worker chunk: command helpers
 * that would otherwise open a one-shot buffer record into the chunk instead.
 * A worker's clear (first depth bind) and scratch-depth setup belong at their
 * packet's position in the chunk, not in a separate submission that could
 * overtake earlier chunks. */
static __thread VkCommandBuffer vk_t_chunk;
static VkCommandBuffer vk_rec_cmd(void)
{
    return vk_t_chunk ? vk_t_chunk : s_vk.cmd;
}
/* Wait for every record worker's submitted chunk (PS3RECOMP_WALKER_MT). */
static void vk_mt_wait_all(void);
static int  vk_mt_oneshot_begin(void* user);
static int  vk_mt_oneshot_submit(void* user);

/* VK_DRAW_BATCH (vk_eng_draw): draws recorded into s_vk.cmd and not yet
 * submitted. Anything that records other work, destroys an object or reads
 * results flushes them first, so everything else keeps the invariant that
 * all earlier work has completed. */
static u32 s_eb_pending;
/* Bumped each time the open batch is submitted: an async readback ticket
 * recorded into batch N is still unsubmitted while this equals N. */
static unsigned long long s_eb_batch_id;
/* VK_FRAME_PIPELINE=1 (with VK_DRAW_BATCH=1): VK_EB_SETS sets of what a batch
 * holds. A present submits the frame's batch without waiting and the next frame
 * records into the next set, so the walker records frame N+1 while the GPU
 * renders N. A set is waited on only before it is recorded into again; every
 * other flush still waits for all the other sets, so the rest of the backend
 * keeps its invariant that all earlier work has completed. */
typedef struct {
    VkCommandBuffer  cmd;
    VkFence          fence;
    vk_buffer        vb, ib, ub, sb;
    vk_buffer        raw_vb, remap_buf;
    VkDescriptorPool pool;
    u32              sets;
    int              inflight;
} vk_eb_set;
static vk_eb_set s_ebs[VK_EB_SETS];
static int s_ebs_cur, s_ebs_on;
static void vk_ebs_wait_other(void);
static void vk_eng_flush_async(void);
/* VK_EB_STATS=1 (diagnostic): why batches end. Set before a flush; vk_eng_flush counts and resets it. */
static u32 s_eb_why;                /* 0 other, 1 ring/pool full, 2 unbatched draw, 3 max draws,
                                     * 4 engine submit, 5 destroy, 6 present, 7 vk_begin */
static void vk_eng_flush(void);
static int vk_eb_active(void);       /* batching decided on and on */
static u32 vk_eb_max(void);
static void vk_eb_recorded(void);   /* one more command in the open batch */
static void vk_eb_release(void);    /* engine shutdown: rings and pool */
static int vk_eb_upload(VkImage img, VkBufferImageCopy* region, const void* src, size_t tight,
                        u32 rows, u32 row_bytes);   /* -2: not batched, do it the old way */

static void vk_destroy_image(vk_image* im)
{
    if (!vk_t_user) {
        if (s_eb_pending) { s_eb_why = 5; vk_eng_flush(); }
        vk_ebs_wait_other();   /* a pipelined frame may still use it */
        /* Never under a present's copy that may still read it. */
        if (im->img) vk_present_wait_gpu();
    } else {
        /* A worker's own scratch image: no global batch/present references it. */
    }
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

/* A buffer only the GPU touches (copies through it stay in VRAM). */
static int vk_create_device_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) { VK_LOG("no memory for a device buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(device buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    b->ptr = NULL; b->coherent = 0; b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    if (!vk_t_user) {
        if (s_eb_pending) vk_eng_flush();
        vk_ebs_wait_other();
    }
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 *
 * The queue is shared with the present (see present_mu in vk_state), so every
 * operation on it goes through these.
 * -------------------------------------------------------------------------*/
static VkResult vk_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static VkResult vk_queue_present(const VkPresentInfoKHR* pi)
{
    if (s_vk.present_queue && s_vk.present_queue != s_vk.queue) {
        return pvkQueuePresentKHR(s_vk.present_queue, pi);
    }
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static void vk_device_idle(void)
{
    if (!s_vk.device || !pvkDeviceWaitIdle) return;
    pthread_mutex_lock(&s_vk.queue_mu);
    pvkDeviceWaitIdle(s_vk.device);
    pthread_mutex_unlock(&s_vk.queue_mu);
}

/* A full execution and memory dependency on everything submitted before
 * this command buffer, the present's copy included. */
static void vk_cmd_after_everything(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static int vk_begin(void)
{
    if (vk_t_user) return vk_mt_oneshot_begin(vk_t_user);
    if (s_eb_pending) { if (!s_eb_why) s_eb_why = 7; vk_eng_flush(); }
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    /* A present's copy may still be reading the surface this frame is about
     * to draw over: order everything here after it. Set only by a present,
     * which never runs alongside the walker's recording. */
    if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
    return 0;
}

static unsigned long long s_eb_wait_us[10];  /* VK_EB_STATS: walker time blocked on the GPU: [why] for a batch
                                             * flush, 8 a submit of its own (readback, create, snapshot), 9 present */
static u32 s_wait_tag = 8;
static unsigned long long vk_now_us(void)
{
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}
static int vk_submit_and_wait(void)
{
    if (vk_t_user) return vk_mt_oneshot_submit(vk_t_user);
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(vk_queue_submit(&si, s_vk.fence), "vkQueueSubmit");
    {   const unsigned long long w0 = vk_now_us();
        VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vk_ebs_wait_other();
        s_eb_wait_us[s_wait_tag] += vk_now_us() - w0; }
    return 0;
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image_on(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    vk_barrier_image_on(vk_rec_cmd(), img, aspect, old_layout);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Census hook for the vtable path: the unit is about to sample the dummy. */
static void vk_tex_miss_vtable(const rsx_texture_state* t, unsigned reason,
                               u32 w, u32 h, u32 pitch, u32 levels)
{
    rsx_tex_miss_note(reason, 0, t->offset, (t->format >> 8) & 0xFFu,
                      w, h, pitch, levels, 0, 0, 0);
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) {
        vk_tex_miss_vtable(t, RSX_TEX_MISS_SIZE, w, h, row_pitch, levels);
        vk_tex_off(unit); return;
    }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) {
        vk_tex_miss_vtable(t, RSX_TEX_MISS_ADDRESS, w, h, row_pitch, levels);
        vk_tex_off(unit); return;
    }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) {                                /* no BC path yet */
        vk_tex_miss_vtable(t, RSX_TEX_MISS_FORMAT, w, h, row_pitch, levels);
        vk_tex_off(unit); return;
    }

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_miss_vtable(t, RSX_TEX_MISS_OTHER, w, h, row_pitch, levels);
                vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            vk_tex_miss_vtable(t, RSX_TEX_MISS_OTHER, w, h, row_pitch, levels);
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    if (!s_vk.tex_ready[unit])
        vk_tex_miss_vtable(t, RSX_TEX_MISS_OTHER, w, h, row_pitch, levels);
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    static const u8 black[4] = { 0, 0, 0, 0 };   /* VK_DUMMY_BLACK=1 (experiment): what a disabled unit reads */
    if (vk_upload_texture(&s_vk.dummy, (getenv("VK_DUMMY_BLACK") && *getenv("VK_DUMMY_BLACK") != '0') ? black : magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const unsigned long long _pt = rsx_wprof_now();
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_pipe_cache, 1, &gci, NULL, &p);
    vk_pipe_create_done("fixed", _pt);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const unsigned long long _pt = rsx_wprof_now();
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_pipe_cache, 1, &gci, NULL, &p);
    vk_pipe_create_done("guest", _pt);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        vk_device_idle();
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
            if (s_vk.sem_acquire[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire[i], NULL);
                s_vk.sem_acquire[i] = VK_NULL_HANDLE;
            }
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.width, (int)s_vk.height, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) { VK_LOG("window has no drawable area\n"); return -1; }

    u32 count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    u32 npm = 0;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (pvkGetPhysicalDeviceSurfacePresentModesKHR &&
        pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, NULL) == VK_SUCCESS && npm > 0) {
        if (npm > 16) npm = 16;
        VkPresentModeKHR pmodes[16];
        if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, pmodes) == VK_SUCCESS) {
            for (u32 i = 0; i < npm; i++) {
                if (pmodes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                    present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                    break;
                }
            }
        }
    }
    const char* pm_env = getenv("PS3RECOMP_VK_PRESENT_MODE");
    if (pm_env) {
        if (!strcmp(pm_env, "fifo")) present_mode = VK_PRESENT_MODE_FIFO_KHR;
        else if (!strcmp(pm_env, "mailbox")) present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = present_mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    /* Leave one image unacquired: the presentation engine can be holding the
     * others, and exhausting the set makes some drivers return VK_NOT_READY
     * from an infinite-timeout acquire instead of waiting. */
    s_vk.present_slots = n > 1 ? n - 1 : 1;
    if (s_vk.present_slots > VK_PRESENT_SLOTS) s_vk.present_slots = VK_PRESENT_SLOTS;
    VK_LOG("window swapchain %ux%u, %u images, format %d, mode %s\n",
           ext.width, ext.height, n, (int)fmt.format,
           present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");
    return 0;
}

static void vk_swapchain_lost(void)
{
    vk_device_idle();
    if (vk_create_swapchain()) vk_window_drop("the swapchain could not be recreated");
}

/* ---------------------------------------------------------------------------
 * Window diagnostics (PS3RECOMP_VK_PRESENT_STATS=1) and the acquire timeout
 * (PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS=<n>, default 0 = the old non-blocking try).
 *
 * vk_present_window drops a frame without a word when no present slot is free
 * or the acquire comes back VK_NOT_READY / VK_TIMEOUT, while s_vk.presented
 * still counts it. The window can then stand still with every counter moving.
 * These say which of the three it is: the pump not running (max_gap), the
 * present thread stuck in vkQueuePresentKHR (noslot, q_max), or the WSI
 * holding every image (notready). Read and written with relaxed atomics from
 * the frame-clock and present threads; one line a second from the pump.
 * -------------------------------------------------------------------------*/
static struct {
    unsigned long long last_pump_us, last_print_us, max_gap_us, q_max_us;
    unsigned pumps, enq, shown, noslot, notready;
} s_vkw;

static int vk_window_stats_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_PRESENT_STATS"); on = e && *e == '1'; }
    return on;
}

#define VKW_ADD(f) do { if (vk_window_stats_on()) __atomic_fetch_add(&s_vkw.f, 1u, __ATOMIC_RELAXED); } while (0)

static void vk_window_stats_max(unsigned long long* slot, unsigned long long v)
{
    unsigned long long cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(slot, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* Pump thread only. */
static void vk_window_stats_pump(void)
{
    if (!vk_window_stats_on()) return;
    const unsigned long long now = vk_now_us();
    if (s_vkw.last_pump_us) vk_window_stats_max(&s_vkw.max_gap_us, now - s_vkw.last_pump_us);
    s_vkw.last_pump_us = now;
    s_vkw.pumps++;
    if (!s_vkw.last_print_us) { s_vkw.last_print_us = now; return; }
    if (now - s_vkw.last_print_us < 1000000ull) return;
    VK_LOG("window: %u pumps, max pump gap %llu ms | blits %u, shown %u, "
           "dropped %u no-slot + %u not-ready, longest vkQueuePresentKHR %llu ms\n",
           s_vkw.pumps, s_vkw.max_gap_us / 1000ull,
           __atomic_exchange_n(&s_vkw.enq, 0u, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_vkw.shown, 0u, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_vkw.noslot, 0u, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_vkw.notready, 0u, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_vkw.q_max_us, 0ull, __ATOMIC_RELAXED) / 1000ull);
    s_vkw.pumps = 0;
    s_vkw.max_gap_us = 0;
    s_vkw.last_print_us = now;
}

static uint64_t vk_acquire_timeout_ns(void)
{
    static long long ms = -1;
    if (ms < 0) {
        const char* e = getenv("PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS");
        ms = e && atoll(e) > 0 ? atoll(e) : 0;
        if (ms > 1000) ms = 1000;
        if (ms) VK_LOG("window: vkAcquireNextImageKHR waits up to %lld ms\n", ms);
    }
    return (uint64_t)ms * 1000000ull;
}

/* ---------------------------------------------------------------------------
 * Presentation thread (Macro Target 1)
 *
 * The walker still records and submits the swapchain blit, because that blit
 * reads the offscreen target the next frame is about to reuse; submitting it
 * before returning keeps the GPU order right. What the walker no longer does
 * is wait for the blit fence and call vkQueuePresentKHR, which can sit in FIFO
 * for ~9 ms. It enqueues the acquired image index and its render-finished
 * semaphore and goes on to the next command buffer.
 * -------------------------------------------------------------------------*/
/* PS3RECOMP_VK_PACE=1 (live key PS3RECOMP_CTL): even out the window present
 * cadence in the present thread. Default off, seeded from the environment. */
static int vk_pace_on(void)
{
#if defined(__GNUC__) && !defined(_WIN32)
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_PRESENT_PACE);
#endif
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_VK_PACE");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

/* When a new guest frame reaches the window, work out the earliest instant
 * this present may run. The target spacing is an EWMA of the *arrival*
 * intervals of new frames, so the pacer never measures its own delay (that
 * would ratchet: a held present makes the next interval look longer). The
 * schedule advances one interval per frame but never runs ahead of the frame
 * by more than one interval, and is abandoned for a frame when presents are
 * already queued behind it. A frame that arrives late is due at once. */
static unsigned long long vk_pace_frame_due(void)
{
    if (!vk_pace_on()) { s_vk.pace_due_us = 0; return 0; }
    const unsigned long long now = vk_now_us();
    if (!s_vk.pace_last_arrival_us || now < s_vk.pace_last_arrival_us ||
        now - s_vk.pace_last_arrival_us > 250000ull) {
        /* First frame, a clock jump, or a pause longer than any frame: start
         * a fresh schedule rather than carrying a stale due time. */
        s_vk.pace_interval_us = 16666;
        s_vk.pace_due_us = now;
    } else {
        const unsigned long long d = now - s_vk.pace_last_arrival_us;
        if (d > 2000 && d < 250000) {
            long long iv = (long long)s_vk.pace_interval_us;
            /* Down faster than up: a cadence that gets faster converges in
             * a handful of frames; a slow patch does not inflate the target. */
            const long long alpha = ((long long)d < iv) ? 4 : 16;
            iv += ((long long)d - iv) / alpha;
            if (iv < 2000) iv = 2000;
            if (iv > 100000) iv = 100000;
            s_vk.pace_interval_us = (unsigned long long)iv;
        }
    }
    s_vk.pace_last_arrival_us = now;

    unsigned long long due = now;
    if (s_vk.pace_due_us > due) due = s_vk.pace_due_us;
    const unsigned long long horizon = now + s_vk.pace_interval_us;
    if (due > horizon) due = horizon;      /* at most one interval of wait */
    if (s_vk.present_q_count >= 2) due = now;   /* behind: do not compound */
    s_vk.pace_due_us = due + s_vk.pace_interval_us;
    return due;
}

static void* vk_present_thread_main(void* arg)
{
    (void)arg;
    ps3_thread_name("present");
    for (;;) {
        u32 slot, idx, frame;
        unsigned long long due, submit_us;
        VkSemaphore done;

        pthread_mutex_lock(&s_vk.present_q_mu);
        while (s_vk.present_q_count == 0 && !s_vk.present_quit)
            pthread_cond_wait(&s_vk.present_q_cv, &s_vk.present_q_mu);
        if (s_vk.present_q_count == 0 && s_vk.present_quit) {
            pthread_mutex_unlock(&s_vk.present_q_mu);
            break;
        }
        slot = s_vk.present_q[s_vk.present_q_head].slot;
        idx  = s_vk.present_q[s_vk.present_q_head].idx;
        frame = s_vk.present_q[s_vk.present_q_head].frame;
        due = s_vk.present_q[s_vk.present_q_head].due_us;
        submit_us = s_vk.present_q[s_vk.present_q_head].submit_us;
        done = s_vk.present_q[s_vk.present_q_head].done;
        s_vk.present_q_head = (s_vk.present_q_head + 1) % VK_PRESENT_SLOTS;
        s_vk.present_q_count--;
        pthread_mutex_unlock(&s_vk.present_q_mu);

        {
            const uint64_t tl_t0 = ps3_trace_begin();
            pvkWaitForFences(s_vk.device, 1, &s_vk.window_fence[slot], VK_TRUE, UINT64_MAX);
            if (tl_t0) {
                const uint64_t t1 = ps3_trace_now();
                ps3_trace_span(PS3_TR_PRESENT_FENCE_WAIT, tl_t0, t1, frame, 0);
                /* The causal event is the window blit submission; the lag
                 * includes the GPU's blit time, which is exactly what this
                 * wait is for. */
                if (submit_us) {
                    const unsigned long long lag = vk_now_us() - submit_us;
                    ps3_trace_instant(PS3_TR_WAIT_WAKE_LAG,
                                      (uint32_t)(lag > 0xFFFFFFFFull ? 0xFFFFFFFFull : lag),
                                      (uint32_t)PS3_TR_NOTE_PRESENT_SUBMIT |
                                      ((uint32_t)PS3_TR_WAIT_PRESENT_FENCE << 8));
                }
            }
        }

        /* PS3RECOMP_VK_PACE: hold the display present, not the frame's GPU
         * work (the swapchain blit was submitted when the frame ended). The
         * due time is the arrival-cadence schedule computed in
         * vk_pace_frame_due; a frame that arrived late is due at once. */
        if (due && vk_pace_on()) {
            while (!s_vk.present_quit) {
                const unsigned long long now = vk_now_us();
                if (now >= due) break;
                unsigned long long us = due - now;
                if (us > 2000) us = 2000;
                struct timespec ts = { 0, (long)us * 1000L };
                nanosleep(&ts, NULL);
            }
        }

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &done,
            .swapchainCount = 1, .pSwapchains = &s_vk.swapchain,
            .pImageIndices = &idx,
        };
        const unsigned long long q0 = vk_window_stats_on() ? vk_now_us() : 0;
        const uint64_t tp0 = ps3_trace_begin();
        VkResult r = vk_queue_present(&pi);
        ps3_trace_end(tp0, PS3_TR_VK_PRESENT, frame, 0);
        if (q0) { vk_window_stats_max(&s_vkw.q_max_us, vk_now_us() - q0); VKW_ADD(shown); }
        cellGcm_flip_log_shown(frame);
        ps3_trace_instant(PS3_TR_IMAGE_SHOWN, frame, 0);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            __atomic_store_n(&s_vk.present_lost, 1, __ATOMIC_RELEASE);
        else if (r != VK_SUCCESS)
            VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);

        pthread_mutex_lock(&s_vk.present_q_mu);
        s_vk.window_slot_busy[slot] = 0;
        pthread_cond_broadcast(&s_vk.present_slot_cv);
        pthread_mutex_unlock(&s_vk.present_q_mu);
    }
    return NULL;
}

static int vk_present_thread_start(void)
{
    if (!s_vk.windowed || s_vk.present_thread_on) return 0;
    if (s_vk.present_slots == 0) return -1;
    s_vk.present_quit = 0;
    if (pthread_create(&s_vk.present_thread, NULL, vk_present_thread_main, NULL) != 0)
        return -1;
    s_vk.present_thread_on = 1;
    return 0;
}

/* Drain every queued present, then let the thread exit. */
static void vk_present_thread_stop(void)
{
    if (!s_vk.present_thread_on) return;
    pthread_mutex_lock(&s_vk.present_q_mu);
    s_vk.present_quit = 1;
    pthread_cond_broadcast(&s_vk.present_q_cv);
    pthread_mutex_unlock(&s_vk.present_q_mu);
    pthread_join(s_vk.present_thread, NULL);
    s_vk.present_thread_on = 0;
}

/* Wait until the thread owns no present and no slot. Needed before the
 * swapchain, its semaphores or its images are destroyed. */
static void vk_present_thread_barrier(void)
{
    if (!s_vk.present_thread_on) return;
    pthread_mutex_lock(&s_vk.present_q_mu);
    while (s_vk.present_q_count != 0)
        pthread_cond_wait(&s_vk.present_slot_cv, &s_vk.present_q_mu);
    for (;;) {
        int busy = 0;
        for (u32 i = 0; i < s_vk.present_slots; i++)
            if (s_vk.window_slot_busy[i]) { busy = 1; break; }
        if (!busy) break;
        pthread_cond_wait(&s_vk.present_slot_cv, &s_vk.present_q_mu);
    }
    pthread_mutex_unlock(&s_vk.present_q_mu);
}

/* Reserve a slot in a non-blocking manner. Returns -1 if every slot is busy. */
static int vk_present_slot_acquire(void)
{
    int slot = -1;
    pthread_mutex_lock(&s_vk.present_q_mu);
    for (u32 i = 0; i < s_vk.present_slots; i++) {
        if (!s_vk.window_slot_busy[i]) {
            slot = (int)i;
            s_vk.window_slot_busy[slot] = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_vk.present_q_mu);
    return slot;
}

static void vk_present_slot_release(int slot)
{
    pthread_mutex_lock(&s_vk.present_q_mu);
    s_vk.window_slot_busy[slot] = 0;
    pthread_cond_broadcast(&s_vk.present_slot_cv);
    pthread_mutex_unlock(&s_vk.present_q_mu);
}

static void vk_present_enqueue(u32 slot, u32 idx, u32 frame,
                               unsigned long long due, unsigned long long submit_us,
                               VkSemaphore done)
{
    pthread_mutex_lock(&s_vk.present_q_mu);
    s_vk.present_q[s_vk.present_q_tail].slot = slot;
    s_vk.present_q[s_vk.present_q_tail].idx  = idx;
    s_vk.present_q[s_vk.present_q_tail].frame = frame;
    s_vk.present_q[s_vk.present_q_tail].due_us = due;
    s_vk.present_q[s_vk.present_q_tail].submit_us = submit_us;
    s_vk.present_q[s_vk.present_q_tail].done = done;
    s_vk.present_q_tail = (s_vk.present_q_tail + 1) % VK_PRESENT_SLOTS;
    s_vk.present_q_count++;
    pthread_cond_signal(&s_vk.present_q_cv);
    pthread_mutex_unlock(&s_vk.present_q_mu);
}

/* PS3RECOMP_VK_PRESENT_NEW_ONLY=1: present the window only when the title
 * requested a new flip since the last window present. The frame clock's outer
 * pass also presents when s_flip_request_count changes, and SetPrepareFlip
 * bumps that count without presenting a frame, so the window was re-presenting
 * the frame already on screen; MangoHud's row rate (one per vkQueuePresentKHR)
 * then read above the [flip] rate. Boot presents (no flip yet) stay free so
 * the window is not black while the title starts. Default off. */
static int s_present_new_only = -1;
static u32 s_last_present_frame;
/* PS3RECOMP_PRESENT_DUMP_DIR: the readback of the copy a present is about to
 * show. vk_present_window dumps it only after the NEW_ONLY gate, so the files
 * are exactly the frames the window shows, in display order. */
static void vk_present_dump_small(const unsigned char* rgba);
static const unsigned char* s_present_dump_px;
static int s_present_dump_px_valid;

static int vk_present_new_only_on(void)
{
    if (s_present_new_only < 0) {
        const char* e = getenv("PS3RECOMP_VK_PRESENT_NEW_ONLY");
        s_present_new_only = e && *e && strcmp(e, "0") != 0;
    }
    return s_present_new_only;
}

/* Blit the offscreen target into the next swapchain image and hand the
 * present to the presentation thread. */
static int vk_stall_trace_on(void);

static void vk_present_window(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return;
    const unsigned long long st_win0 = vk_stall_trace_on() ? vk_now_us() : 0;

    const u32 present_frame = cellGcm_flip_frame_count();
    if (vk_present_new_only_on()) {
        if (present_frame && present_frame == s_last_present_frame) return;
        s_last_present_frame = present_frame;
    }
    if (s_present_dump_px_valid) {
        vk_present_dump_small(s_present_dump_px);
        s_present_dump_px_valid = 0;
    }
    const unsigned long long pace_due = vk_pace_frame_due();

    /* The thread saw the swapchain go stale; drain it before replacing the
     * swapchain out from under queued presents. */
    if (__atomic_load_n(&s_vk.present_lost, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_vk.present_lost, 0, __ATOMIC_RELEASE);
        vk_present_thread_barrier();
        vk_swapchain_lost();
        if (!s_vk.windowed || !s_vk.swapchain) return;
    }

    const int slot = vk_present_slot_acquire();
    if (slot < 0) { VKW_ADD(noslot); return; }

    u32 idx = 0;
    const unsigned long long st_aq0 = vk_stall_trace_on() ? vk_now_us() : 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, vk_acquire_timeout_ns(),
                                       s_vk.sem_acquire[slot], VK_NULL_HANDLE, &idx);
    if (st_aq0) {
        const unsigned long long dt = vk_now_us() - st_aq0;
        if (dt >= 200000ull) VK_LOG("stalltrace acquire=%.1fms\n", dt / 1000.0);
    }
    if (r == VK_NOT_READY || r == VK_TIMEOUT) {
        VKW_ADD(notready);
        vk_present_slot_release(slot);
        return;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_present_slot_release(slot);
        vk_present_thread_barrier();
        vk_swapchain_lost();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r);
        vk_present_slot_release(slot);
        return;
    }

    VkCommandBuffer cb = s_vk.window_cmd[slot];
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) {
        VK_LOG("vkResetCommandBuffer (window) failed\n");
        vk_present_slot_release(slot);
        return;
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOG("vkBeginCommandBuffer (window) failed\n");
        vk_present_slot_release(slot);
        return;
    }
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)s_vk.sc_extent.width, (int32_t)s_vk.sc_extent.height, 1 } },
    };
    pvkCmdBlitImage(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOG("vkEndCommandBuffer failed\n");
        vk_present_slot_release(slot);
        return;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire[slot],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.window_fence[slot]);
    if (vk_queue_submit(&si, s_vk.window_fence[slot]) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n");
        vk_present_slot_release(slot);
        return;
    }

    VKW_ADD(enq);
    cellGcm_flip_log_present(present_frame);
    const unsigned long long submit_us = vk_now_us();
    if (ps3_trace_on()) {
        ps3_trace_instant(PS3_TR_PRESENT_QUEUED, present_frame, (uint32_t)slot);
        ps3_trace_instant(PS3_TR_PRESENT_SUBMIT, present_frame, (uint32_t)slot);
        ps3_trace_note_at(PS3_TR_NOTE_PRESENT_SUBMIT,
                          (uint64_t)submit_us * 1000ull, present_frame);
    }
    if (s_vk.present_thread_on) {
        vk_present_enqueue((u32)slot, idx, present_frame, pace_due, submit_us,
                           s_vk.sc_done[idx]);
        if (st_win0) {
            const unsigned long long dt = vk_now_us() - st_win0;
            if (dt >= 200000ull) VK_LOG("stalltrace present_window=%.1fms\n", dt / 1000.0);
        }
        return;
    }

    /* Fallback when the thread could not start. */
    pvkWaitForFences(s_vk.device, 1, &s_vk.window_fence[slot], VK_TRUE, UINT64_MAX);
    if (pace_due && vk_pace_on()) {
        for (;;) {
            const unsigned long long now = vk_now_us();
            if (now >= pace_due) break;
            unsigned long long us = pace_due - now;
            if (us > 2000) us = 2000;
            struct timespec ts = { 0, (long)us * 1000L };
            nanosleep(&ts, NULL);
        }
    }
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    const unsigned long long q0 = vk_window_stats_on() ? vk_now_us() : 0;
    r = vk_queue_present(&pi);
    if (q0) { vk_window_stats_max(&s_vkw.q_max_us, vk_now_us() - q0); VKW_ADD(shown); }
    cellGcm_flip_log_shown(present_frame);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
    vk_present_slot_release(slot);
}

static int vk_stall_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_STALL_TRACE"); on = e && *e == '1'; }
    return on;
}

static int vk_dump_async_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_DUMP_ASYNC"); on = e && *e == '1'; }
    return on;
}

/* One write of a packed copy: a 3-byte fwrite per pixel cost the walker's
 * thread ~20% of its time at one dump every 10 frames. Written beside the
 * target and renamed over it, so a reader never sees half a frame. */
static void vk_dump_ppm(const unsigned char* rgba)
{
    const unsigned long long st_t0 = vk_stall_trace_on() ? vk_now_us() : 0;
    const size_t n = (size_t)s_vk.width * s_vk.height;
    unsigned char* rgb = (unsigned char*)malloc(n * 3);
    if (!rgb) return;
    for (size_t i = 0; i < n; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
    if (vk_dump_async_on() &&
        vk_dump_queue_init(s_vk.dump_path) == 0 &&
        vk_dump_queue_submit(rgb, n * 3, s_vk.width, s_vk.height) == 0) {
        if (st_t0) {
            const unsigned long long dt = vk_now_us() - st_t0;
            if (dt >= 20000ull)
                VK_LOG("stalltrace dump_handoff=%.1fms\n", dt / 1000.0);
        }
        return;
    }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_vk.dump_path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", tmp); free(rgb); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    fwrite(rgb, 1, n * 3, f);
    fclose(f);
    free(rgb);
#ifdef _WIN32
    remove(s_vk.dump_path);   /* rename does not replace on Windows */
#endif
    rename(tmp, s_vk.dump_path);
    if (st_t0) {
        const unsigned long long dt = vk_now_us() - st_t0;
        if (dt >= 20000ull)
            VK_LOG("stalltrace dump=%.1fms\n", dt / 1000.0);
    }
}

/* ---------------------------------------------------------------------------
 * Present
 *
 * In two halves. The first records, on the present's own command buffer,
 * whatever puts the frame into the offscreen target (the engine's blit of
 * the flipped surface) and the copy of that target into `readback`, and
 * submits it without waiting. The second waits for it, reads the centre
 * pixel and the frame dump back, and blits the target into the window.
 *
 * The first half reads the frame, so it must not run while the walker is
 * recording; the second touches only the target, `readback` and the
 * present's own objects, none of which the engine path draws into, so a host
 * can run it on a thread of its own beside the walker
 * (rsx_vulkan_backend_present_begin / _finish). On the fixed-function path
 * the walker draws into the target itself, so there the second half always
 * follows the first at once.
 * -------------------------------------------------------------------------*/

/* Wait until the GPU is done with a present's copy. Safe from any thread:
 * present_fence is reset only when a present starts, which never runs
 * alongside the walker. */
static void vk_present_wait_gpu(void)
{
    if (__atomic_load_n(&s_vk.present_inflight, __ATOMIC_ACQUIRE) && s_vk.present_fence)
        pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
}

/* PS3RECOMP_PRESENT_DUMP_DIR=<dir>: write a small grayscale PGM of every
 * presented frame (default 160x90, PS3RECOMP_PRESENT_DUMP_MAX frames) so the
 * displayed sequence can be checked offline for backward steps. Downsampled
 * on the CPU from the readback the present already made; default off. */
static void vk_present_dump_small(const unsigned char* rgba)
{
    static int on = -1;
    static const char* dir;
    static u32 maxn, n, dw, dh;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_PRESENT_DUMP_DIR");
        dir = (e && *e) ? e : NULL;
        const char* m = getenv("PS3RECOMP_PRESENT_DUMP_MAX");
        maxn = m && atoi(m) > 0 ? (u32)atoi(m) : 240u;
        const char* w = getenv("PS3RECOMP_PRESENT_DUMP_W");
        const char* h = getenv("PS3RECOMP_PRESENT_DUMP_H");
        dw = w && atoi(w) > 0 ? (u32)atoi(w) : 160u;
        dh = h && atoi(h) > 0 ? (u32)atoi(h) : 90u;
        on = dir != NULL;
        if (on) VK_LOG("present dump: %ux%u PGM into %s, max %u frames\n", dw, dh, dir, maxn);
    }
    if (!on || !dir || n >= maxn) return;
    unsigned char* img = (unsigned char*)malloc((size_t)dw * dh);
    if (!img) { on = 0; return; }
    const u32 sw = s_vk.width, sh = s_vk.height;
    for (u32 y = 0; y < dh; y++) {
        const u32 y0 = (u32)((unsigned long long)y * sh / dh);
        u32 y1 = (u32)((unsigned long long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (u32 x = 0; x < dw; x++) {
            const u32 x0 = (u32)((unsigned long long)x * sw / dw);
            u32 x1 = (u32)((unsigned long long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long long sum = 0, cnt = 0;
            for (u32 yy = y0; yy < y1 && yy < sh; yy++)
                for (u32 xx = x0; xx < x1 && xx < sw; xx++) {
                    const unsigned char* p = rgba + ((size_t)yy * sw + xx) * 4;
                    sum += (unsigned)p[0] * 77u + (unsigned)p[1] * 150u + (unsigned)p[2] * 29u;
                    cnt++;
                }
            img[(size_t)y * dw + x] = cnt ? (unsigned char)(sum / (cnt * 256u)) : 0;
        }
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/present_%05u.pgm", dir, n);
    FILE* f = fopen(path, "wb");
    if (!f) { on = 0; free(img); return; }
    fprintf(f, "P5\n%u %u\n255\n", dw, dh);
    fwrite(img, 1, (size_t)dw * dh, f);
    fclose(f);
    u64 hsh = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)dw * dh; i++) hsh = (hsh ^ img[i]) * 1099511628211ull;
    fprintf(stderr, "[pdump] n=%u hash=%016llx\n", n, (unsigned long long)hsh);
    free(img);
    n++;
}

/* present_mu held. */
static void vk_present_complete_locked(void)
{
    if (!s_vk.present_inflight) return;
    {   const unsigned long long w0 = vk_now_us();
        pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
        const unsigned long long dt = vk_now_us() - w0;
        s_eb_wait_us[9] += dt;
        if (dt >= 200000ull) VK_LOG("stalltrace present_fence=%.1fms\n", dt / 1000.0); }
    __atomic_store_n(&s_vk.present_inflight, 0, __ATOMIC_RELEASE);

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    s_present_dump_px = px;
    s_present_dump_px_valid = 1;
    vk_present_window();
}

/* present_mu held: a previous present finished, the present's command
 * buffer begun and ordered after everything submitted so far. */
static VkCommandBuffer vk_present_record_begin(void)
{
    vk_present_complete_locked();
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_cmd_after_everything(cb);
    return cb;
}

/* present_mu held: copy the target out, submit, and finish at once unless a
 * host asked for the halves separately on the engine path. */
static void vk_present_record_end(VkCommandBuffer cb)
{
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (present) failed\n"); return;
    }
    __atomic_store_n(&s_vk.present_inflight, 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    if (!(s_vk.present_split && s_vk.eng_active)) vk_present_complete_locked();
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) vk_present_record_end(cb);
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 8192          /* engine texture cache max 4096 + targets/views */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: six layers, a CUBE view    */
    u8       snapshot;               /* COLOR: a feedback copy, never a target */
    u32      mips;                   /* TEXTURE: levels the image was created with */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];
/* Highest handle handed out since init: the object-table scans stop here
 * instead of at the (now 8192-entry) array end. */
static u32 s_eobj_hi;

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng2_shutdown(void* user);

/* Bound targets, per recording context (PS3RECOMP_WALKER_MT). The state
 * thread uses the global one; each record worker gets its own so two workers
 * may bind different targets at the same time. */
typedef struct vk_eng_bound_state {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} vk_eng_bound_state;
static vk_eng_bound_state s_bound_global;
static __thread vk_eng_bound_state* s_bound_tls;
static vk_eng_bound_state* vk_bound_cur(void)
{
    return s_bound_tls ? s_bound_tls : &s_bound_global;
}
#define s_eng_bound (*vk_bound_cur())
static vk_eng_bound_state* vk_bound_for_user(void* user);

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets. Both are required
 * colour attachment formats with blending in Vulkan 1.0. */
#define VK_ENG_RT_FORMATS 2
static int vk_eng_rt_index(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    default:                            return -1;
    }
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) {
            if (i > s_eobj_hi) s_eobj_hi = i;
            return i;
        }
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Host staging kept across readbacks and uploads. One buffer allocated,
 * mapped and freed per call is three driver round trips, and on Windows
 * those took from tens of milliseconds to seconds each under load. Each
 * grows to the largest request and is reused after its submission
 * completes; every user waits for that before returning. */
static vk_buffer s_rb_stage, s_up_stage;
static vk_buffer* vk_stage_get(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= size) return b;
    if (b->buf) vk_destroy_buffer(b);
    VkDeviceSize cap = (VkDeviceSize)1 << 20;
    while (cap < size) cap *= 2u;
    if (vk_create_host_buffer(b, cap, usage)) { vk_destroy_buffer(b); return NULL; }
    return b;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of an image that is in GENERAL. */
static int vk_eng_upload_region(VkImage img, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    {   /* VK_DRAW_BATCH: staged in a ring and recorded into the open batch. */
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        const int b = vk_eb_upload(img, &region, src, tight, rows, row_bytes);
        if (b != -2) return b;
    }
    /* Unbatched: #552's reused upload staging rather than a buffer created and
     * destroyed per region. */
    vk_buffer* const st = vk_stage_get(&s_up_stage, (VkDeviceSize)tight * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!st || !st->coherent) return -1;
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st->ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st->buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    return rc;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(img, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    s_eobj_hi = 0;
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? 0 : -1;
}

static void vk_eng_snapshot_recycle(vk_eng_obj* o);

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    if (!vk_t_user) vk_mt_wait_all();   /* no recorded chunk may still name it */
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = 1; i <= s_eobj_hi; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].snapshot) {
        /* Never an attachment, so no framebuffer names it; the image goes
         * back to the pool for the next feedback draw. */
        vk_eng_snapshot_recycle(&s_eobj[handle]);
        memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
        return;
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers();
    vk_destroy_image(&s_eobj[handle].im);      /* idle: submissions are waited on */
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_snapshot_pool_drain(void);
static void vk_eng_readback_tickets_drain(void);

static void vk_eng_shutdown(void* user)
{
    vk_eng2_shutdown(user);
    for (u32 i = 1; i <= s_eobj_hi; i++) vk_eng_release(user, i);
    s_eobj_hi = 0;
    vk_eng_snapshot_pool_drain();
    vk_eb_release();
    vk_eng_readback_tickets_drain();
    vk_destroy_buffer(&s_rb_stage);
    vk_destroy_buffer(&s_up_stage);
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;      /* every other submission has already completed */
    if (s_eb_pending) { s_eb_why = 4; vk_eng_flush(); }
    vk_ebs_wait_other();
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, vf, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes)
        vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim()) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        s_vk.depth_aspect)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    vk_eng_bound_state* b = vk_bound_for_user(user);
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        b->rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    b->nrt = count;
    b->depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vk_t_chunk) {
        vk_barrier_image_on(vk_t_chunk, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(vk_t_chunk, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        return;
    }
    const int batched = vk_eb_active();
    if (batched ? (!s_eb_pending && vk_begin()) : vk_begin()) return;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    if (batched) vk_eb_recorded();
    else vk_submit_and_wait();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    if (vk_t_chunk) {
        vk_barrier_image_on(vk_t_chunk, o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(vk_t_chunk, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
        return;
    }
    const int batched = vk_eb_active();
    if (batched ? (!s_eb_pending && vk_begin()) : vk_begin()) return;
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    if (batched) vk_eb_recorded();
    else vk_submit_and_wait();
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    if (!s_vk.device) return;
    if (s_eb_pending) { s_eb_why = 6; vk_eng_flush_async(); }   /* the present records on its own buffer */
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) {
        if (o) {
            vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
            };
            pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                            (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST
                                                                        : VK_FILTER_LINEAR);
        }
        vk_present_record_end(cb);
    }
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || o->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;   /* rows are R,G,B,A */
    if (!rows || !w || !h || x + w > o->w || y + h > o->h) return;
    vk_eng_upload_region(o->im.img, 0, 0, x, y, w, h, (size_t)w * o->bpp, h, rows, row_bytes);
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w || y + h > o->h) return;
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer* const st = vk_stage_get(&s_rb_stage, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!st) return;
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait()) {
            vk_invalidate(st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st->ptr + (size_t)row * tight, tight);
        }
    }
}

/* ---- asynchronous readback (on-demand WCB, docs/wcb-async.md) -------------
 * A copy of a colour target into a host buffer of its own, recorded into the
 * open draw batch when batching is on, so queueing it costs no GPU wait: it
 * runs when the batch is submitted, ordered after every draw before it by the
 * full barrier in front of it and before every draw after it by the one
 * behind. Fetching waits for that batch alone if it is still open or in
 * flight. With batching off the copy is one synchronous submission, as every
 * other operation here is, and the ticket only carries the bytes. Buffers are
 * kept per ticket slot and grow; a released slot is reused, and the barrier
 * in front of the next copy orders it after any copy still in flight there. */
#define VK_RB_TICKETS 64
typedef struct {
    int                used, done;
    vk_buffer          buf;
    u32                w, h, bpp;
    unsigned long long batch;
} vk_rb_ticket;
static vk_rb_ticket s_rbt[VK_RB_TICKETS];

static u32 vk_eng_readback_async(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !w || !h || x + w > o->w || y + h > o->h) return 0;
    u32 i = 0;
    while (i < VK_RB_TICKETS && s_rbt[i].used) i++;
    if (i == VK_RB_TICKETS) return 0;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * o->bpp;
    if (s_rbt[i].buf.buf && s_rbt[i].buf.size < bytes) vk_destroy_buffer(&s_rbt[i].buf);
    if (!s_rbt[i].buf.buf) {
        VkDeviceSize cap = (VkDeviceSize)1 << 20;
        while (cap < bytes) cap *= 2u;
        if (vk_create_host_buffer(&s_rbt[i].buf, cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_rbt[i].buf);
            return 0;
        }
    }
    const int batched = vk_eb_active();
    if (batched ? (!s_eb_pending && vk_begin()) : vk_begin()) return 0;
    vk_cmd_after_everything(s_vk.cmd);
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
    };
    pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, s_rbt[i].buf.buf, 1, &region);
    VkBufferMemoryBarrier hb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_rbt[i].buf.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    /* Later draws into the target wait for the copy to have read it. */
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    s_rbt[i].used = 1;
    s_rbt[i].w = w; s_rbt[i].h = h; s_rbt[i].bpp = o->bpp;
    s_rbt[i].batch = s_eb_batch_id;
    if (batched) {
        s_rbt[i].done = 0;
        vk_eb_recorded();            /* may submit the batch: the ticket's batch id is already set */
    } else {
        s_rbt[i].done = vk_submit_and_wait() == 0;
        if (!s_rbt[i].done) { s_rbt[i].used = 0; return 0; }
    }
    return i + 1u;
}

static int vk_eng_readback_fetch(void* user, u32 ticket, void* out, u32 out_pitch)
{
    (void)user;
    if (!ticket || ticket > VK_RB_TICKETS || !s_rbt[ticket - 1u].used) return -1;
    vk_rb_ticket* t = &s_rbt[ticket - 1u];
    if (!t->done) {
        const unsigned long long w0 = vk_now_us();
        if (t->batch == s_eb_batch_id && s_eb_pending) { s_eb_why = 4; vk_eng_flush(); }
        vk_ebs_wait_other();         /* a pipelined batch set may still hold it */
        s_eb_wait_us[8] += vk_now_us() - w0;
        t->done = 1;
    }
    int rc = -1;
    if (out && out_pitch) {
        vk_invalidate(&t->buf);
        const size_t tight = (size_t)t->w * t->bpp;
        for (u32 row = 0; row < t->h; row++)
            memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)t->buf.ptr + (size_t)row * tight, tight);
        rc = 0;
    }
    t->used = 0;
    return rc;
}

static void vk_eng_readback_release(void* user, u32 ticket)
{
    (void)user;
    if (ticket && ticket <= VK_RB_TICKETS) s_rbt[ticket - 1u].used = 0;
}

/* PS3RECOMP_RSX_WCB_PREFETCH: submit the open batch and keep recording in the
 * other set. No-op without frame pipelining, where a flush would wait -- the
 * sync point must not turn into a GPU stall. */
static void vk_eng_submit_async(void* user)
{
    (void)user;
    if (!s_ebs_on || !s_eb_pending) return;
    vk_eng_flush_async();
}

static void vk_eng_readback_tickets_drain(void)
{
    for (u32 i = 0; i < VK_RB_TICKETS; i++) {
        if (s_rbt[i].buf.buf) vk_destroy_buffer(&s_rbt[i].buf);
        memset(&s_rbt[i], 0, sizeof s_rbt[i]);
    }
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_split_layout). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube; o->mips = mips;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_and_wait()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u) || mip >= o->mips) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
/* PS3RECOMP_VK_VIEW_MEMO: the (surface, crossbar, format) -> view answer in a
 * direct-mapped table. The old path scanned all 2048 object slots on every
 * target-as-texture bind; a crowded frame does that thousands of times. The
 * cached answer is validated against the object table (kind, surface, remap,
 * format), so a released or reused handle simply misses. */
#define VK_VIEW_MEMO 512u
static struct { u32 surface, remap, rsx_fmt, handle; } s_view_memo[VK_VIEW_MEMO];
static int s_view_memo_on = -1;

static int vk_view_memo_on(void)
{
    if (ps3_live_ctl_get) return ps3_live_ctl_get(PS3_LIVE_VK_VIEW_MEMO);
    if (s_view_memo_on < 0) {
        const char* e = getenv("PS3RECOMP_VK_VIEW_MEMO");
        s_view_memo_on = e && *e && strcmp(e, "0") != 0 ? 1 : 0;
    }
    return s_view_memo_on;
}

static u32 vk_view_memo_slot(u32 surface, u32 remap, u32 rsx_format)
{
    u32 h = surface * 0x9E3779B1u ^ remap * 0x85EBCA77u ^ rsx_format * 0xC2B2AE3Du;
    h ^= h >> 15;
    return h & (VK_VIEW_MEMO - 1u);
}

static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    const int memo = vk_view_memo_on();
    const u32 ms = vk_view_memo_slot(surface, remap, rsx_format);
    if (memo) {
        const u32 mh = s_view_memo[ms].handle;
        if (mh && mh < VK_ENG_MAX_OBJ && s_eobj[mh].kind == VK_ENG_VIEW &&
            s_eobj[mh].surface == surface && s_eobj[mh].remap == remap &&
            s_eobj[mh].rsx_fmt == rsx_format)
            return mh;
    }
    for (u32 i = 1; i <= s_eobj_hi; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format) {
            if (memo) s_view_memo[ms] = (typeof(s_view_memo[0])){ surface, remap, rsx_format, i };
            return i;
        }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    if (memo) s_view_memo[ms] = (typeof(s_view_memo[0])){ surface, remap, rsx_format, hd };
    return hd;
}
/* ---- cube render targets (PS3RECOMP_RSX_CUBE_RT) -------------------------
 * The game renders an 8x8 cube environment probe into ordinary colour
 * targets; a TextureCube unit cannot sample a 2-D target. Assemble one
 * six-layer cube image on the GPU, copying each face from its target once per
 * face-content change, and hand back a cube-view handle. The cache is keyed
 * by the six face windows, their colour serials and the crossbar, so every
 * draw in a frame reuses the same image and no face is copied twice. */
#define VK_CUBE_RT_CACHE 32
typedef struct {
    int      used;
    rsx_be_cube_face face[6];
    u64      serials[6];
    rsx_be_format fmt;
    u32      location, offset;     /* guest address the cube belongs to */
    u32      w, h, remap, rsx_fmt, handle;
    unsigned long long tick;
} vk_cube_rt;

static vk_cube_rt s_cube_rt[VK_CUBE_RT_CACHE];
static unsigned long long s_cube_rt_tick;

/* Copy each face window into one layer of the cube image, in command order,
 * and wait: the sources may include writes recorded earlier in the current
 * batch, and the draw that samples the cube follows this call. */
static int vk_cube_rt_copy(u32 handle, const rsx_be_cube_face faces[6], u32 w, u32 h)
{
    vk_eng_obj* dst = vk_eng_get(handle, VK_ENG_TEXTURE);
    if (!dst || !dst->cube) return -1;
    for (u32 f = 0; f < 6; f++)
        if (!vk_eng_get(faces[f].handle, VK_ENG_COLOR)) return -1;
    if (vk_begin()) return -1;
    for (u32 f = 0; f < 6; f++)
        vk_barrier_image(s_eobj[faces[f].handle].im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(dst->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    for (u32 f = 0; f < 6; f++) {
        const vk_eng_obj* src = &s_eobj[faces[f].handle];
        VkImageCopy c = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffset = { (int32_t)faces[f].x, (int32_t)faces[f].y, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (u32)f, 1 },
            .extent = { w, h, 1 },
        };
        pvkCmdCopyImage(s_vk.cmd, src->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        dst->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    vk_barrier_image(dst->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    return vk_submit_and_wait();
}

static u32 vk_eng_cube_target_view(void* user, u32 location, u32 offset,
                                   const rsx_be_cube_face faces[6],
                                   const u64 serials[6],
                                   rsx_be_format fmt, u32 w, u32 h,
                                   u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!w || w != h) return 0;
    vk_cube_rt* e = NULL;
    int spare = -1;
    for (u32 i = 0; i < VK_CUBE_RT_CACHE; i++) {
        vk_cube_rt* c = &s_cube_rt[i];
        if (!c->used) { if (spare < 0) spare = (int)i; continue; }
        if (c->w == w && c->h == h && c->fmt == fmt && c->remap == remap &&
            c->rsx_fmt == rsx_fmt && c->location == location &&
            c->offset == offset && !memcmp(c->face, faces, sizeof c->face)) {
            e = c; break;
        }
    }
    if (!e) {
        if (spare < 0) {
            /* Evict the least recently used; a pending batch may still name
             * its image, so let that batch finish first. */
            unsigned long long oldest = (unsigned long long)-1;
            for (u32 i = 0; i < VK_CUBE_RT_CACHE; i++)
                if (s_cube_rt[i].tick < oldest) { oldest = s_cube_rt[i].tick; spare = (int)i; }
            if (s_eb_pending) vk_eng_flush();
            vk_eng_release(NULL, s_cube_rt[spare].handle);
            memset(&s_cube_rt[spare], 0, sizeof s_cube_rt[spare]);
        }
        e = &s_cube_rt[spare];
        memcpy(e->face, faces, sizeof e->face);
        memcpy(e->serials, serials, sizeof e->serials);
        e->fmt = fmt; e->w = w; e->h = h; e->remap = remap; e->rsx_fmt = rsx_fmt;
        e->location = location; e->offset = offset;
        e->handle = vk_eng_texture_create(NULL, fmt, w, h, 1, 6, remap, rsx_fmt);
        if (!e->handle) { memset(e, 0, sizeof *e); return 0; }
        e->used = 1;
        if (vk_cube_rt_copy(e->handle, faces, w, h)) {
            vk_eng_release(NULL, e->handle);
            memset(e, 0, sizeof *e);
            return 0;
        }
    } else if (memcmp(e->serials, serials, sizeof e->serials)) {
        if (vk_cube_rt_copy(e->handle, faces, w, h)) return 0;
        memcpy(e->serials, serials, sizeof e->serials);
    }
    e->tick = ++s_cube_rt_tick;
    return e->handle;
}

/* The last complete cube image at a guest address. An incomplete probe (a
 * face target was just recreated, so its colour serial is 0, or one face has
 * not been declared yet) binds this instead of the dummy, which is what the
 * pregame cinematics need: the probe is refreshed in bursts, and flashing
 * black between refreshes is the bright/dark alternation the user saw. The
 * handle stays owned by the cache; returning it only bumps its LRU tick so a
 * live fallback is not evicted while a draw still names it. */
static u32 vk_eng_cube_target_last(void* user, u32 location, u32 offset,
                                   rsx_be_format fmt, u32 w, u32 h,
                                   u32 remap, u32 rsx_fmt)
{
    (void)user;
    vk_cube_rt* best = NULL;
    for (u32 i = 0; i < VK_CUBE_RT_CACHE; i++) {
        vk_cube_rt* c = &s_cube_rt[i];
        if (!c->used || !c->handle) continue;
        if (c->location != location || c->offset != offset) continue;
        if (c->fmt != fmt || c->w != w || c->h != h) continue;
        if (c->remap != remap || c->rsx_fmt != rsx_fmt) continue;
        if (!best || c->tick > best->tick) best = c;
    }
    if (!best) return 0;
    best->tick = ++s_cube_rt_tick;
    return best->handle;
}

/* ---- colour feedback snapshots ---------------------------------------------
 * A draw that samples a colour target it is also writing (in-place deferred
 * lighting: read the albedo, write the lit colour over it) gets a copy of
 * that target taken just before it. The engine asks for one per such draw
 * (rsx_draw_backend.color_snapshot) and releases the previous one; without
 * this hook it falls back to the guest-memory bytes at the target's address,
 * which hold whatever was last stored there -- for Twisted Metal the previous
 * frame's UI, lit and composited into the next frame.
 *
 * Every operation here is one synchronous submission, so the copy is already
 * in command order: after the draws before it, before the draw that samples
 * it. The copy is a COLOR object marked `snapshot` so the engine can take a
 * crossbar view of it (vk_eng_surface_view); it is never an attachment.
 * Released copies keep their images in a small pool, since a lit frame asks
 * for one per light. */
#define VK_SNAP_POOL 8
static struct { vk_image im; VkFormat fmt; u32 w, h; } s_snap_pool[VK_SNAP_POOL];
static u32 s_snap_pool_n;
static vk_buffer s_snap_st;         /* depth snapshot bounce buffer (vk_eng_depth_snapshot) */

static void vk_eng_snapshot_recycle(vk_eng_obj* o)
{
    if (s_snap_pool_n < VK_SNAP_POOL && o->im.img) {
        s_snap_pool[s_snap_pool_n].im = o->im;
        s_snap_pool[s_snap_pool_n].fmt = o->fmt;
        s_snap_pool[s_snap_pool_n].w = o->w;
        s_snap_pool[s_snap_pool_n].h = o->h;
        s_snap_pool_n++;
    } else {
        vk_destroy_image(&o->im);
    }
}

static void vk_eng_snapshot_pool_drain(void)
{
    for (u32 i = 0; i < s_snap_pool_n; i++) vk_destroy_image(&s_snap_pool[i].im);
    s_snap_pool_n = 0;
    vk_destroy_buffer(&s_snap_st);
}

static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    const vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t || t->snapshot) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++)
        if (s_snap_pool[i].fmt == t->fmt && s_snap_pool[i].w == t->w && s_snap_pool[i].h == t->h) {
            o->im = s_snap_pool[i].im;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    if (!reused && vk_create_image(&o->im, t->fmt, t->w, t->h,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im);
        memset(o, 0, sizeof *o);
        return 0;
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = t->fmt; o->bpp = t->bpp; o->w = t->w; o->h = t->h;
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                     reused ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { t->w, t->h, 1 },
    };
    pvkCmdCopyImage(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    /* The copy, made visible to whatever samples it next. */
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_submit_and_wait()) return 0;
    }

    /* The bounce buffer stays (device-local when the device has it, grown as
     * needed): a fresh host buffer per snapshot cost ~4.8 ms each, 375 times
     * in 20 s of God of War: Ascension's first level. With draw batching the
     * copies join the open batch instead of a submission of their own; the
     * draws that sample the snapshot come after them in the same buffer, and
     * each batched draw starts with a full memory barrier. */
    const VkDeviceSize need = (VkDeviceSize)w * h * 4u;
    if (s_snap_st.size < need) {
        vk_destroy_buffer(&s_snap_st);                   /* flushes a pending batch first */
        if (vk_create_device_buffer(&s_snap_st, need,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_snap_st); return 0;
        }
    }
    vk_buffer st = s_snap_st;
    const int batch = vk_eb_active();
    int rc = (batch && s_eb_pending) ? 0 : vk_begin();
    if (!rc && batch) vk_cmd_after_everything(s_vk.cmd);
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        if (batch) {
            vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            if (++s_eb_pending >= vk_eb_max()) vk_eng_flush();
        } else
            rc = vk_submit_and_wait();
    }
    return rc ? 0 : snap;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. */
static int vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h ||
        out_pitch < w * 4u || (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
                                  s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
        return -1;
    vk_buffer* const st = vk_stage_get(&s_rb_stage, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!st) return -1;
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st->buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st->buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_submit_and_wait();
        if (!rc) {
            vk_invalidate(st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch,
                       (const u8*)st->ptr + (size_t)row * w * 4u, (size_t)w * 4u);
        }
    }
    return rc ? -1 : 0;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES in rsx_draw_engine.c). God of
 * War: Ascension selects ~740 distinct fragment programs in its first level on
 * top of the menu's; with 512 slots every later pipeline failed silently and the
 * level drew nothing but its clears. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units: the split set layout */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

#define VK_ENG_SAMP_CACHE 64

/* Every field of the E2 per-draw state is per recording context: the state
 * thread uses s_e2_global, each MT record worker its own copy. The object
 * caches inside (render passes, framebuffers, split layout, scratch depth)
 * are per context too; only the pipeline table above is shared, and creating
 * a pipeline variant is serialised on s_pipe_create_mu. */

typedef struct vk_e2_state {
    VkRenderPass rp[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1]; /* by format, colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    int          vs_ranged;           /* only vs_lo..+vs_n rows and the tail are live */
    u32          vs_lo, vs_n, vs_tail_off, vs_tail_bytes;
    u8*          ucpu;                /* the constants, in the shared UBO's layout */
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    float        depth_bounds[2];     /* min, max for the next draw */
    vk_buffer    indices;
    u32          warned;
    /* The split set layout (vk_eng_split_layout) and its one set, made the
     * first time a pipeline samples a cube map; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout split_layout;
    VkDescriptorPool      split_pool;
    VkDescriptorSet       split_set;
    VkPipelineLayout      split_pipe_layout;
    u32                   dummy_cube;
    /* Per-context sampler cache; sampler objects are immutable, so a context
     * may make its own rather than share a locked table. */
    struct { u64 key; VkSampler s; } esamp[VK_ENG_SAMP_CACHE];
    u32                   esamp_count;
} vk_e2_state;

static vk_e2_state s_e2_global;
static __thread vk_e2_state* s_e2_tls;
static vk_e2_state* vk_e2_cur(void)
{
    return s_e2_tls ? s_e2_tls : &s_e2_global;
}
#define s_e2 (*vk_e2_cur())
#define s_esamp (s_e2.esamp)
#define s_esamp_count (s_e2.esamp_count)

/* One record worker's Vulkan context: its own command pool/buffer/fence,
 * descriptor pool, ring slices and per-draw bind state (bound targets and the
 * whole E2 state). Command buffers are primaries, one per contiguous chunk,
 * and are submitted in chunk order by whichever worker owns the chunk's turn;
 * queue_mu makes the submits serialised. Secondary command buffers are not
 * used because every draw already begins and ends its own render pass (§3.5). */
#define VK_MT_MAX_WORKERS 4
typedef struct vk_mt_ctx {
    int              id;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;             /* the chunk being recorded */
    VkFence          fence;           /* chunk submission */
    int              inflight;
    VkCommandBuffer  os_cmd;          /* one-shot work before a chunk begins */
    VkFence          os_fence;
    VkDescriptorPool dpool;
    VkDescriptorPool vtx_pool;        /* owns vtx_set; never reset with dpool */
    VkDescriptorSet  vtx_set;         /* GPU vertex unpack, names this ctx's rings */
    vk_buffer        vb, raw_vb, remap_buf, ib, ub;
    VkDeviceSize     vb_head, raw_head, remap_head, ib_head, ub_head;
    vk_e2_state      e2;
    vk_eng_bound_state bound;
    unsigned long long chunks, draws;
} vk_mt_ctx;

static vk_mt_ctx s_mtctx[VK_MT_MAX_WORKERS];
static int s_mtctx_on;
/* vkCreateGraphicsPipelines may not run concurrently with itself while the
 * shared VkPipelineCache is in use; variant creation (first draw of a
 * topology) is serialised here and the result re-checked. */
static pthread_mutex_t s_pipe_create_mu = PTHREAD_MUTEX_INITIALIZER;

static vk_eng_bound_state* vk_bound_for_user(void* user)
{
    if (!user) return &s_bound_global;
    return &((vk_mt_ctx*)user)->bound;
}

static void vk_bind_user(void* user)
{
    vk_t_user = user;
    if (!user) {
        s_e2_tls = &s_e2_global;
        s_bound_tls = &s_bound_global;
    } else {
        vk_mt_ctx* c = (vk_mt_ctx*)user;
        s_e2_tls = &c->e2;
        s_bound_tls = &c->bound;
    }
}

static void vk_eng_drop_framebuffers(void)
{
    if (!vk_t_user) {
        if (s_eb_pending) vk_eng_flush();
        vk_ebs_wait_other();
    }
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb) pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    s_e2.fb_count = 0;
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. An MRT set shares target A's format (engine contract). */
static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n)
{
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[fi][n]) return s_e2.rp[fi][n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[fi][n]) != VK_SUCCESS)
        s_e2.rp[fi][n] = VK_NULL_HANDLE;
    return s_e2.rp[fi][n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

/* The engine's identity for the pipeline_create in progress. It is the key
 * of the guest-keyed shader disk cache (PS3RECOMP_SHADER_DISK_CACHE); the
 * walker is the only caller, so one static is enough. */
static u64 s_eng_pipe_key;

static void vk_eng_set_pipeline_key(void* user, u64 key)
{
    (void)user;
    s_eng_pipe_key = key;
}

static int vk_eng_translate(u64 guest_key, const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv_keyed(guest_key, hlsl, stage, s_g.spv, VK_SPV_WORDS,
                                &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* A pixel program with a cube unit declares all sixteen units one by one
 * (TextureCube or Texture2D rsx_texN : register(tN)) instead of the
 * Texture2D array, and register(tN) is binding RSX_SPIRV_TEXTURE_BINDING + N
 * (rsx_shader_spirv.h): sixteen single bindings, where the shared layout has
 * one binding of sixteen. Those pipelines use this layout, made on first use
 * so a title without cube maps never builds it. */
static VkPipelineLayout vk_eng_split_layout(void)
{
    if (s_e2.split_pipe_layout) return s_e2.split_pipe_layout;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        b[3 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3 + RSX_MAX_TEXTURES, .pBindings = b };
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.split_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_e2.split_pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e2.split_pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_e2.split_layout };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e2.split_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_e2.split_set) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.split_pipe_layout) != VK_SUCCESS) {
        s_e2.split_pipe_layout = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return s_e2.split_pipe_layout;
}

/* Which units a pixel program declares as TextureCube. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) {
            VK_LOG("engine: pipeline table full (%u), draws will drop\n", (unsigned)VK_ENG_MAX_PIPE);
            s_e2.warned |= 128u;
        }
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    {
        const uint64_t _t = ps3_trace_begin();
        const int _rc = vk_eng_translate(s_eng_pipe_key, vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs);
        if (_t) ps3_trace_end(_t, PS3_TR_SHADER_COMPILE, 0,
                              (uint32_t)strlen(vs_hlsl));
        if (_rc) return 0;
    }
    {
        const uint64_t _t = ps3_trace_begin();
        const int _rc = vk_eng_translate(s_eng_pipe_key, ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs);
        if (_t) ps3_trace_end(_t, PS3_TR_SHADER_COMPILE, 1,
                              (uint32_t)strlen(ps_hlsl));
        if (_rc) {
            pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P); return 0;
        }
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = vk_eng_cube_units(ps_hlsl);
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!vk_t_user) vk_mt_wait_all();
    if (s_eb_pending) vk_eng_flush();
    vk_ebs_wait_other();
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if (P->variant[t]) pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant_create(vk_eng_pipe* P, u32 topo_idx)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (P->variant[topo_idx]) return P->variant[topo_idx];
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, P->rt_count);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const VkPipelineLayout layout = P->cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
    if (!layout) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back,
        /* The bounds themselves are dynamic (vk_eng_set_depth_bounds). */
        .depthBoundsTestEnable = (rs->depth_bounds_test && s_vk.depth_bounds_ok) ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f };
    if (rs->depth_bounds_test && !s_vk.depth_bounds_ok && !(s_e2.warned & 64u)) {
        VK_LOG("engine: the device has no depthBounds; the depth bounds test is ignored\n");
        s_e2.warned |= 64u;
    }
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Target A takes COLOR_MASK and BLEND_ENABLE; targets B..D their own
     * nibble of COLOR_MASK_MRT (A,R,G,B at bits 0..3) and bit of
     * BLEND_ENABLE_MRT, the hardware rule the Metal backend follows. With A's
     * state on every target, a light pass that adds into A with B and C
     * masked off added every light into the G-buffer, and a material pass
     * that writes B and C's alpha with A's alpha masked left them stale.
     * PS3RECOMP_VK_MRT_MASK takes the same values as PS3RECOMP_METAL_MRT_MASK:
     * 1 (default) the hardware rule; 0 target A's state everywhere, the old
     * behaviour; 2 only a target masked off entirely is off, with per-target
     * blend. A device without independentBlend keeps A's state everywhere. */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    static int mrt_mode = -1;
    if (mrt_mode < 0) { const char* e = getenv("PS3RECOMP_VK_MRT_MASK"); mrt_mode = e ? atoi(e) : 1; }
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++) {
        VkColorComponentFlags twm = wm;
        int tblend = rs->blend_enable != 0;
        if (r > 0 && mrt_mode != 0 && s_vk.indep_blend_ok) {
            const u32 nib = (rs->color_mask_mrt >> (r * 4u)) & 0xFu;
            if (mrt_mode == 1 || nib == 0) {
                twm = 0;
                if (nib & 1u) twm |= VK_COLOR_COMPONENT_A_BIT;
                if (nib & 2u) twm |= VK_COLOR_COMPONENT_R_BIT;
                if (nib & 4u) twm |= VK_COLOR_COMPONENT_G_BIT;
                if (nib & 8u) twm |= VK_COLOR_COMPONENT_B_BIT;
            }
            tblend = (rs->blend_mrt >> r) & 1u;
        }
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = tblend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = twm };
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = s_vk.depth_bounds_ok ? 4u : 3u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = layout, .renderPass = rp, .subpass = 0 };
    const unsigned long long _pt = rsx_wprof_now();
    const VkResult _pr = pvkCreateGraphicsPipelines(s_vk.device, s_pipe_cache, 1, &gci, NULL,
                                                    &P->variant[topo_idx]);
    vk_pipe_create_done("engine", _pt);
    if (_pr != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        P->variant[topo_idx] = VK_NULL_HANDLE;
    }
    return P->variant[topo_idx];
}

/* First use of a (pipeline, topology) builds the VkPipeline. Two record
 * workers may reach the same pair together; the shared VkPipelineCache is
 * externally synchronised, so serialise creation and re-check. */
static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx)
{
    if (P->variant[topo_idx]) return P->variant[topo_idx];
    pthread_mutex_lock(&s_pipe_create_mu);
    VkPipeline pipe = P->variant[topo_idx];
    if (!pipe) pipe = vk_eng_variant_create(P, topo_idx);
    pthread_mutex_unlock(&s_pipe_create_mu);
    return pipe;
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { vk_bind_user(user); s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[f][n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    if (s_e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_e2.split_pipe_layout, NULL);
    if (s_e2.split_pool)        pvkDestroyDescriptorPool(s_vk.device, s_e2.split_pool, NULL);
    if (s_e2.split_layout)      pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.split_layout, NULL);
    free(s_e2.ucpu);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants are kept in ordinary memory laid out like the shared UBO (vertex
 * at 0, pixel at ps_off) and copied to the GPU per draw: into the draw's ring
 * slice when batched, into the shared UBO otherwise (every such draw is waited
 * on, so overwriting it is safe). Writing them straight into the UBO meant a
 * batched draw read them back out of it, and that memory is host-coherent,
 * i.e. write-combined and uncached on most drivers. */
static u8* vk_eng_ucpu(void)
{
    if (!s_e2.ucpu && s_g.ubo.size) s_e2.ucpu = (u8*)calloc(1, (size_t)s_g.ubo.size);
    return s_e2.ucpu;
}
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    vk_bind_user(user);
    s_e2.vs_ranged = 0;
    if (!s_g.on || !data || !vk_eng_ucpu()) { s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    memcpy(s_e2.ucpu, data, bytes);
    s_e2.vs_bytes = bytes;
}
/* The rows the vertex program reads and the viewport tail, copied without
 * comparing; the rest of the block is never read. Ported from the inFAMOUS 2
 * port (github.com/ThiagoFraga761/infamous2-pc, MIT). */
static void vk_eng_bind_vs_constants_range(void* user, const void* rows, u32 first, u32 count,
                                           const void* tail, u32 tail_bytes, u32 total_bytes)
{
    vk_bind_user(user);
    const u32 cap = (u32)s_g.ps_off;
    s_e2.vs_ranged = 0;
    if (!s_g.on || !rows || !vk_eng_ucpu()) { s_e2.vs_bytes = 0; return; }
    /* The engine's block always fits the VPConst slot (VK_VPCONST_BYTES). */
    if (total_bytes > cap || tail_bytes > total_bytes ||
        (first + count) * 16u > total_bytes - tail_bytes) {
        if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constant range does not fit\n"); s_e2.warned |= 2u; }
        s_e2.vs_bytes = 0;
        return;
    }
    const u32 tail_off = total_bytes - tail_bytes;
    memcpy(s_e2.ucpu + first * 16u, (const u8*)rows + first * 16u, count * 16u);
    memcpy(s_e2.ucpu + tail_off, tail, tail_bytes);
    s_e2.vs_bytes = total_bytes;
    s_e2.vs_ranged = 1;
    s_e2.vs_lo = first; s_e2.vs_n = count;
    s_e2.vs_tail_off = tail_off; s_e2.vs_tail_bytes = tail_bytes;
}
/* The VS block into the UBO slice the draw reads. */
static void vk_eng_put_vs_constants(u8* dst, u32 vsb)
{
    if (!s_e2.vs_ranged) { memcpy(dst, s_e2.ucpu, vsb); return; }
    memcpy(dst + s_e2.vs_lo * 16u, s_e2.ucpu + s_e2.vs_lo * 16u, s_e2.vs_n * 16u);
    memcpy(dst + s_e2.vs_tail_off, s_e2.ucpu + s_e2.vs_tail_off, s_e2.vs_tail_bytes);
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    vk_bind_user(user);
    if (!s_g.on || !data || !vk_eng_ucpu()) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    memcpy(s_e2.ucpu + s_g.ps_off, data, bytes);
    s_e2.ps_bytes = bytes;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    vk_bind_user(user);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    vk_bind_user(user); (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    vk_bind_user(user);
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    vk_bind_user(user);
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { vk_bind_user(user); s_e2.stencil_ref = ref; }

/* Vulkan wants the bounds inside [0, 1] without VK_EXT_depth_range_unrestricted;
 * a NaN clamps to the open end so a garbage register cannot reject a draw. */
static float vk_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void vk_eng_set_depth_bounds(void* user, float min, float max)
{
    vk_bind_user(user);
    s_e2.depth_bounds[0] = vk_depth_bound(min, 0.0f);
    s_e2.depth_bounds[1] = vk_depth_bound(max, 1.0f);
}

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.depth_aspect))
        return VK_NULL_HANDLE;
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    if (vk_t_chunk) {
        /* A worker's chunk: the image is private to this context and the
         * clear must execute at this draw's position, not in a submission
         * that could run before the chunks recorded earlier. */
        vk_barrier_image_on(vk_t_chunk, s_e2.scratch_depth.img, s_vk.depth_aspect,
                            VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdClearDepthStencilImage(vk_t_chunk, s_e2.scratch_depth.img,
                                     VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    } else {
        if (vk_begin()) return VK_NULL_HANDLE;
        vk_barrier_image(s_e2.scratch_depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img,
                                     VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
        if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    }
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32);
    for (u32 i = 0; i < s_esamp_count; i++) if (s_esamp[i].key == key) return s_esamp[i].s;
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    return ((int)o->cube == want_cube) ? o->im.view : VK_NULL_HANDLE;
}

/* What a cube unit with nothing bound samples: a 1x1 cube, magenta on every
 * face like the 2D dummy, made on first use. */
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        static const u8 black[4] = { 0, 0, 0, 0 };
        const u8* fill = vk_env_on("VK_DUMMY_BLACK") ? black : magenta;
        s_e2.dummy_cube = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6,
                                                0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_texture_upload(NULL, s_e2.dummy_cube, f, 0, 1, 1, fill, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}

/* ---------------------------------------------------------------------------
 * Draw batching (VK_DRAW_BATCH=1)
 *
 * Every vk_eng_draw used to submit its own command buffer and wait on a
 * fence, because the vertex, index and uniform buffers and the descriptor set
 * are single objects rewritten per draw. In God of War: Ascension's first level
 * that was 54,759 draws in 20 s at ~168 us each: the FIFO walker spent 46% of
 * its time blocked on the GPU, the RSX ring drained one draw at a time and the
 * SPU geometry jobs waited for ring space. Batched, a draw takes slices of
 * three ring buffers and a descriptor set from a pool, records into the open
 * command buffer, and only s_eb_pending grows. vk_eng_flush() submits, waits
 * once, and resets the rings and the pool; vk_begin, every object destroy, the
 * present and the engine's own submit point call it first, so all other work
 * keeps seeing everything before it complete. Between batched draws a full
 * memory barrier keeps render-to-texture ordering without tracking.
 * VK_DRAW_BATCH_MAX (default 512) caps the draws per submission.
 * -------------------------------------------------------------------------*/
#define VK_EB_VB_BYTES       (64u << 20)
#define VK_EB_RAW_VB_BYTES   (32u << 20)
#define VK_EB_REMAP_BYTES    (4u << 20)
#define VK_EB_IB_BYTES       (16u << 20)
#define VK_EB_UB_BYTES       (32u << 20)
#define VK_EB_SB_BYTES       (64u << 20)   /* texture upload staging */
#define VK_EB_MAX_SETS       1024u

static struct {
    int              on;            /* -1 undecided, 0 off, 1 on */
    u32              max_draws;
    vk_buffer        vb, raw_vb, remap_buf, ib, ub, sb;
    VkDeviceSize     vb_head, raw_vb_head, remap_head, ib_head, ub_head, sb_head, ub_align;
    VkDescriptorPool pool;
    u32              sets;
    unsigned long long draws, flushes, unbatched, why[8];
} s_eb = { .on = -1 };

/* Macro Target 2: Vulkan compute pipeline for GPU vertex unpacking */
static int vk_vtx_unpack_init(void)
{
    const char* off = getenv("PS3RECOMP_VK_VTX_UNPACK");
    if (off && *off == '0') {
        VK_LOG("gpu vertex unpack: disabled by env\n");
        return 0;
    }
    if (s_vk.vtx_unpack_pipe) return 0;

    if (vk_create_shader(vtx_unpack_comp_spv, sizeof vtx_unpack_comp_spv, &s_vk.vtx_unpack_module)) {
        VK_LOG("gpu vertex unpack: failed to create compute shader module\n");
        return -1;
    }

    VkDescriptorSetLayoutBinding binds[3] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = binds
    };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &dlci, NULL, &s_vk.vtx_unpack_dsl) != VK_SUCCESS)
        return -1;

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 104
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &s_vk.vtx_unpack_dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr
    };
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.vtx_unpack_layout) != VK_SUCCESS)
        return -1;

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = s_vk.vtx_unpack_module,
            .pName = "main"
        },
        .layout = s_vk.vtx_unpack_layout
    };
    const unsigned long long _pt = rsx_wprof_now();
    const VkResult _pr = pvkCreateComputePipelines(s_vk.device, s_pipe_cache, 1, &cpci, NULL,
                                                   &s_vk.vtx_unpack_pipe);
    vk_pipe_create_done("compute", _pt);
    if (_pr != VK_SUCCESS) {
        VK_LOG("gpu vertex unpack: failed to create compute pipeline\n");
        return -1;
    }

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * VK_EB_SETS };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = VK_EB_SETS,
        .poolSizeCount = 1,
        .pPoolSizes = &ps
    };
    if (pvkCreateDescriptorPool(s_vk.device, &dpci, NULL, &s_vk.vtx_unpack_pool) != VK_SUCCESS)
        return -1;

    VkDescriptorSetLayout lays[VK_EB_SETS];
    for (int i = 0; i < VK_EB_SETS; i++) lays[i] = s_vk.vtx_unpack_dsl;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s_vk.vtx_unpack_pool,
        .descriptorSetCount = VK_EB_SETS,
        .pSetLayouts = lays
    };
    if (pvkAllocateDescriptorSets(s_vk.device, &dsai, s_vk.vtx_unpack_ds) != VK_SUCCESS)
        return -1;

    /* One descriptor set per batch set: it names that set's raw, vertex and
     * remap buffers. Set 0 is s_eb; the rest exist only with frame pipelining. */
    for (int s = 0; s < VK_EB_SETS; s++) {
        if (s > 0 && !s_ebs_on) break;
        const vk_buffer* raw = s ? &s_ebs[s].raw_vb : &s_eb.raw_vb;
        const vk_buffer* vb  = s ? &s_ebs[s].vb     : &s_eb.vb;
        const vk_buffer* rm  = s ? &s_ebs[s].remap_buf : &s_eb.remap_buf;
        VkDescriptorBufferInfo bi[3] = {
            { raw->buf, 0, VK_WHOLE_SIZE },
            { vb->buf,  0, VK_WHOLE_SIZE },
            { rm->buf,  0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet w[3];
        for (int i = 0; i < 3; i++) {
            w[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = s_vk.vtx_unpack_ds[s],
                .dstBinding = (uint32_t)i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bi[i]
            };
        }
        pvkUpdateDescriptorSets(s_vk.device, 3, w, 0, NULL);
    }

    VK_LOG("gpu vertex unpack: compute pipeline ready\n");
    return 0;
}

static void vk_vtx_unpack_release(void)
{
    if (s_vk.vtx_unpack_pipe) {
        pvkDestroyPipeline(s_vk.device, s_vk.vtx_unpack_pipe, NULL);
        s_vk.vtx_unpack_pipe = VK_NULL_HANDLE;
    }
    if (s_vk.vtx_unpack_layout) {
        pvkDestroyPipelineLayout(s_vk.device, s_vk.vtx_unpack_layout, NULL);
        s_vk.vtx_unpack_layout = VK_NULL_HANDLE;
    }
    if (s_vk.vtx_unpack_pool) {
        pvkDestroyDescriptorPool(s_vk.device, s_vk.vtx_unpack_pool, NULL);
        s_vk.vtx_unpack_pool = VK_NULL_HANDLE;
    }
    if (s_vk.vtx_unpack_dsl) {
        pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.vtx_unpack_dsl, NULL);
        s_vk.vtx_unpack_dsl = VK_NULL_HANDLE;
    }
    if (s_vk.vtx_unpack_module) {
        pvkDestroyShaderModule(s_vk.device, s_vk.vtx_unpack_module, NULL);
        s_vk.vtx_unpack_module = VK_NULL_HANDLE;
    }
}

static int vk_eb_on(void)
{
    if (s_eb.on >= 0) return s_eb.on;
    const char* e = getenv("VK_DRAW_BATCH");
    s_eb.on = 0;
    if (!e || *e != '1' || !s_g.on) return 0;
    const char* m = getenv("VK_DRAW_BATCH_MAX");
    s_eb.max_draws = m && atoi(m) > 0 ? (u32)atoi(m) : 512u;
    if (s_eb.max_draws > VK_EB_MAX_SETS) s_eb.max_draws = VK_EB_MAX_SETS;
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_eb.ub_align = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256;
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * VK_EB_MAX_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES * VK_EB_MAX_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES * VK_EB_MAX_SETS },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = VK_EB_MAX_SETS, .poolSizeCount = 3, .pPoolSizes = ps };
    if (vk_create_host_buffer(&s_eb.vb, VK_EB_VB_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !s_eb.vb.coherent ||
        vk_create_host_buffer(&s_eb.raw_vb, VK_EB_RAW_VB_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !s_eb.raw_vb.coherent ||
        vk_create_host_buffer(&s_eb.remap_buf, VK_EB_REMAP_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !s_eb.remap_buf.coherent ||
        vk_create_host_buffer(&s_eb.ib, VK_EB_IB_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !s_eb.ib.coherent ||
        vk_create_host_buffer(&s_eb.ub, VK_EB_UB_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_eb.ub.coherent ||
        vk_create_host_buffer(&s_eb.sb, VK_EB_SB_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !s_eb.sb.coherent ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_eb.pool) != VK_SUCCESS) {
        VK_LOG("draw batching: no host-coherent ring memory, staying off\n");
        return 0;
    }
    VK_LOG("draw batching: on (up to %u draws per submission)\n", s_eb.max_draws);
    s_eb.on = 1;
    const char* fp = getenv("VK_FRAME_PIPELINE");
    if (fp && *fp == '1') {
        int made = 1;
        for (int i = 1; i < VK_EB_SETS && made; i++) {
            vk_eb_set* o = &s_ebs[i];
            VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = s_vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1 };
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (pvkAllocateCommandBuffers(s_vk.device, &cai, &o->cmd) != VK_SUCCESS ||
                pvkCreateFence(s_vk.device, &fci, NULL, &o->fence) != VK_SUCCESS ||
                vk_create_host_buffer(&o->vb, VK_EB_VB_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !o->vb.coherent ||
                vk_create_host_buffer(&o->raw_vb, VK_EB_RAW_VB_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !o->raw_vb.coherent ||
                vk_create_host_buffer(&o->remap_buf, VK_EB_REMAP_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !o->remap_buf.coherent ||
                vk_create_host_buffer(&o->ib, VK_EB_IB_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !o->ib.coherent ||
                vk_create_host_buffer(&o->ub, VK_EB_UB_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !o->ub.coherent ||
                vk_create_host_buffer(&o->sb, VK_EB_SB_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !o->sb.coherent ||
                pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &o->pool) != VK_SUCCESS)
                made = 0;
        }
        if (made) {
            s_ebs[0].vb = s_eb.vb;
            s_ebs[0].raw_vb = s_eb.raw_vb;
            s_ebs[0].remap_buf = s_eb.remap_buf;
            s_ebs[0].ib = s_eb.ib;
            s_ebs[0].ub = s_eb.ub;
            s_ebs[0].sb = s_eb.sb;
            s_ebs[0].pool = s_eb.pool;
            s_ebs_cur = 0; s_ebs_on = 1;
            VK_LOG("frame pipelining: on (%d batch sets)\n", VK_EB_SETS);
        } else {
            for (int i = 1; i < VK_EB_SETS; i++) {
                vk_eb_set* o = &s_ebs[i];
                if (o->vb.buf) vk_destroy_buffer(&o->vb);
                if (o->raw_vb.buf) vk_destroy_buffer(&o->raw_vb);
                if (o->remap_buf.buf) vk_destroy_buffer(&o->remap_buf);
                if (o->ib.buf) vk_destroy_buffer(&o->ib);
                if (o->ub.buf) vk_destroy_buffer(&o->ub);
                if (o->sb.buf) vk_destroy_buffer(&o->sb);
                if (o->pool) pvkDestroyDescriptorPool(s_vk.device, o->pool, NULL);
                if (o->fence) pvkDestroyFence(s_vk.device, o->fence, NULL);
                memset(o, 0, sizeof *o);
            }
            VK_LOG("frame pipelining: no memory for extra batch sets, staying off\n");
        }
    }
    vk_vtx_unpack_init();
    return 1;
}

/* The set the walker records into goes to the GPU without a wait; the other
 * becomes current once the GPU is done with it. */
static void vk_ebs_swap(void)
{
    vk_eb_set* c = &s_ebs[s_ebs_cur];
    c->cmd = s_vk.cmd; c->fence = s_vk.fence;
    c->vb = s_eb.vb; c->ib = s_eb.ib; c->ub = s_eb.ub; c->sb = s_eb.sb;
    c->raw_vb = s_eb.raw_vb; c->remap_buf = s_eb.remap_buf;
    c->pool = s_eb.pool; c->sets = s_eb.sets; c->inflight = 1;
    s_ebs_cur = (s_ebs_cur + 1) % VK_EB_SETS;
    vk_eb_set* n = &s_ebs[s_ebs_cur];
    if (n->inflight) {
        const unsigned long long w0 = vk_now_us();
        pvkWaitForFences(s_vk.device, 1, &n->fence, VK_TRUE, UINT64_MAX);
        s_eb_wait_us[6] += vk_now_us() - w0;
        n->inflight = 0;
    }
    s_vk.cmd = n->cmd; s_vk.fence = n->fence;
    s_eb.vb = n->vb; s_eb.ib = n->ib; s_eb.ub = n->ub; s_eb.sb = n->sb;
    s_eb.raw_vb = n->raw_vb; s_eb.remap_buf = n->remap_buf;
    s_eb.pool = n->pool;
    s_eb.vb_head = s_eb.ib_head = s_eb.ub_head = s_eb.sb_head = 0;
    s_eb.raw_vb_head = s_eb.remap_head = 0;
    s_eb.raw_vb_head = s_eb.remap_head = 0;
    if (n->sets) pvkResetDescriptorPool(s_vk.device, n->pool, 0);
    s_eb.sets = 0;
}

static void vk_ebs_wait_other(void)
{
    if (!s_ebs_on) return;
    for (int i = 0; i < VK_EB_SETS; i++) {
        vk_eb_set* o = &s_ebs[i];
        if (i == s_ebs_cur || !o->inflight) continue;
        pvkWaitForFences(s_vk.device, 1, &o->fence, VK_TRUE, UINT64_MAX);
        o->inflight = 0;
    }
}

/* At a present with pipelining on: submit the open batch and switch sets. */
static void vk_eng_flush_async(void)
{
    if (!s_ebs_on) { vk_eng_flush(); return; }
    if (!s_eb_pending) return;
    s_eb_pending = 0;
    s_eb_why = 0;
    if (pvkEndCommandBuffer(s_vk.cmd) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (pipelined) failed\n"); return; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    pvkResetFences(s_vk.device, 1, &s_vk.fence);
    if (vk_queue_submit(&si, s_vk.fence) != VK_SUCCESS) { VK_LOG("vkQueueSubmit (pipelined) failed\n"); return; }
    s_eb_batch_id++;
    s_eb.flushes++;
    ps3_trace_instant(PS3_TR_DRAW_SUBMIT, s_eb_batch_id, (uint32_t)s_eb.flushes);
    vk_ebs_swap();
}

/* The rings and the pool live until the engine shuts down; a later vk_eb_on
 * decides again. Also frees what a failed vk_eb_on had already created. */
static void vk_eb_release(void)
{
    if (s_eb_pending) vk_eng_flush();
    vk_ebs_wait_other();
    if (s_ebs_on) {   /* every set but the current one; the current is s_eb / s_vk below */
        for (int i = 0; i < VK_EB_SETS; i++) {
            if (i == s_ebs_cur) continue;
            vk_eb_set* o = &s_ebs[i];
            vk_destroy_buffer(&o->vb); vk_destroy_buffer(&o->ib);
            vk_destroy_buffer(&o->ub); vk_destroy_buffer(&o->sb);
            vk_destroy_buffer(&o->raw_vb); vk_destroy_buffer(&o->remap_buf);
            if (o->pool) pvkDestroyDescriptorPool(s_vk.device, o->pool, NULL);
            if (o->fence) pvkDestroyFence(s_vk.device, o->fence, NULL);
        }
        memset(s_ebs, 0, sizeof s_ebs); s_ebs_on = 0; s_ebs_cur = 0;
    }
    vk_vtx_unpack_release();
    vk_destroy_buffer(&s_eb.vb);
    vk_destroy_buffer(&s_eb.ib);
    vk_destroy_buffer(&s_eb.ub);
    vk_destroy_buffer(&s_eb.sb);
    vk_destroy_buffer(&s_eb.raw_vb);
    vk_destroy_buffer(&s_eb.remap_buf);
    if (s_eb.pool) pvkDestroyDescriptorPool(s_vk.device, s_eb.pool, NULL);
    memset(&s_eb, 0, sizeof s_eb);
    s_eb.on = -1;
}

static int vk_eb_active(void) { return s_eb.on == 1; }
static u32 vk_eb_max(void) { return s_eb.max_draws; }
static void vk_eb_recorded(void) { if (++s_eb_pending >= s_eb.max_draws) vk_eng_flush_async(); }

static VkDeviceSize vk_eb_take(VkDeviceSize* head, VkDeviceSize cap, VkDeviceSize bytes, VkDeviceSize align);

/* A texture upload as one more batched operation: rows staged in the ring, the
 * copy recorded behind a full memory barrier (earlier draws of the batch may
 * still sample the old texels) and followed by one on the image (later draws
 * sample the new ones). Each used to allocate a host buffer and wait on its own
 * submission, ~3000 times in 20 s of God of War: Ascension's first level. */
static int vk_eb_upload(VkImage img, VkBufferImageCopy* region, const void* src, size_t tight,
                        u32 rows, u32 row_bytes)
{
    if (!vk_eb_active()) return -2;
    const VkDeviceSize bytes = (VkDeviceSize)tight * rows;
    if (!bytes || bytes > VK_EB_SB_BYTES) return -2;
    VkDeviceSize off = vk_eb_take(&s_eb.sb_head, VK_EB_SB_BYTES, bytes, 16u);
    if (off == (VkDeviceSize)-1) {
        vk_eng_flush();
        off = vk_eb_take(&s_eb.sb_head, VK_EB_SB_BYTES, bytes, 16u);
        if (off == (VkDeviceSize)-1) return -2;
    }
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)s_eb.sb.ptr + off + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    if (!s_eb_pending && vk_begin()) return -1;
    vk_cmd_after_everything(s_vk.cmd);
    region->bufferOffset = off;
    pvkCmdCopyBufferToImage(s_vk.cmd, s_eb.sb.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, region);
    vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    if (++s_eb_pending >= s_eb.max_draws) vk_eng_flush_async();
    return 0;
}

static void vk_eng_flush(void)
{
    if (!s_eb_pending) return;
    {   static int st = -1; static unsigned long long t0;
        if (st < 0) st = getenv("VK_EB_STATS") ? 1 : 0;
        if (st) {
            s_eb.why[s_eb_why & 7]++;
            struct timespec ts; timespec_get(&ts, TIME_UTC);
            unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
            if (!t0) t0 = now;
            if (now - t0 >= 2000) {
                unsigned long long* w = s_eb.why;
                /* The walker's own CPU time per draw: what CPU-side draw work costs, whatever the scene. */
                static unsigned long long cpu0;
                unsigned long long cpu = 0;
#ifndef _WIN32
                { struct timespec ct; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ct);
                  cpu = (unsigned long long)ct.tv_sec * 1000000ull + (unsigned long long)ct.tv_nsec / 1000ull; }
#endif
                const unsigned long long nd = s_eb.draws + s_eb.unbatched;
                VK_LOG("batch stats 2s: %llu batched draws, %llu unbatched, %llu flushes; walker %.1f us cpu/draw;"
                       " why: other %llu ring %llu unbatched %llu max %llu engine %llu destroy %llu present %llu begin %llu\n",
                       s_eb.draws, s_eb.unbatched, s_eb.flushes, (cpu0 && nd) ? (double)(cpu - cpu0) / nd : 0.0,
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                unsigned long long* gw = s_eb_wait_us;
                VK_LOG("gpu wait ms/2s: flush other %.0f ring %.0f unbatched %.0f max %.0f engine %.0f destroy %.0f"
                       " present %.0f begin %.0f; own submits %.0f; present completion %.0f\n",
                       gw[0] / 1e3, gw[1] / 1e3, gw[2] / 1e3, gw[3] / 1e3, gw[4] / 1e3, gw[5] / 1e3, gw[6] / 1e3,
                       gw[7] / 1e3, gw[8] / 1e3, gw[9] / 1e3);
                memset(s_eb_wait_us, 0, sizeof s_eb_wait_us);
                cpu0 = cpu;
                memset(w, 0, sizeof s_eb.why); s_eb.draws = s_eb.unbatched = s_eb.flushes = 0; t0 = now;
            }
        }
        s_wait_tag = s_eb_why & 7;
        s_eb_why = 0; }
    s_eb_pending = 0;               /* first: the submit below must not re-enter */
    vk_submit_and_wait();
    s_eb_batch_id++;
    ps3_trace_instant(PS3_TR_DRAW_SUBMIT, s_eb_batch_id, (uint32_t)s_eb.flushes);
    s_wait_tag = 8;
    s_eb.vb_head = s_eb.ib_head = s_eb.ub_head = s_eb.sb_head = 0;
    s_eb.raw_vb_head = s_eb.remap_head = 0;
    if (s_eb.sets) { pvkResetDescriptorPool(s_vk.device, s_eb.pool, 0); s_eb.sets = 0; }
    if ((++s_eb.flushes & 0x3FFFu) == 0)
        VK_LOG("draw batching: %llu draws in %llu submissions\n", s_eb.draws, s_eb.flushes);
}

/* Submit the open state-thread batch in stream order. Worker chunks are
 * submitted by their own threads at the ordered turn; anything the state
 * thread recorded (a clear, an inline packet draw) must be on the queue
 * before a later worker chunk, or it would execute at the next present,
 * after the draws it is supposed to precede. Asynchronous with frame
 * pipelining, a wait without it. */
static void vk_eng_submit_batch(void* user)
{
    (void)user;
    if (!s_eb_pending) return;
    if (s_ebs_on) vk_eng_flush_async();
    else { s_eb_why = 4; vk_eng_flush(); }
}

static VkDeviceSize vk_eb_take(VkDeviceSize* head, VkDeviceSize cap, VkDeviceSize bytes, VkDeviceSize align)
{
    const VkDeviceSize at = (*head + align - 1u) / align * align;
    if (at + bytes > cap) return (VkDeviceSize)-1;
    *head = at + bytes;
    return at;
}

static VkDescriptorSet vk_eng_write_descriptors_to(const vk_eng_pipe* P, VkDescriptorSet set,
                                                   VkBuffer ubo, VkDeviceSize vs_off, VkDeviceSize ps_off);

/* Fill the set the bound pipeline uses: the shared one (sixteen textures in
 * one binding), or for a pipeline with cube units the split one (one binding
 * per unit). Returns the set, or VK_NULL_HANDLE. */
static VkDescriptorSet vk_eng_write_descriptors(const vk_eng_pipe* P)
{
    const VkDescriptorSet set = P->cube_mask ? s_e2.split_set : s_g.set;
    return vk_eng_write_descriptors_to(P, set, s_g.ubo.buf, 0, s_g.ps_off);
}

static VkDescriptorSet vk_eng_write_descriptors_to(const vk_eng_pipe* P, VkDescriptorSet set,
                                                   VkBuffer ubo, VkDeviceSize vs_off, VkDeviceSize ps_off)
{
    const int split = P->cube_mask != 0;
    if (!set) return VK_NULL_HANDLE;
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { ubo, vs_off, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { ubo, ps_off, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const int cube = (P->cube_mask >> u) & 1u;
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                     : VK_NULL_HANDLE;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        /* A bound handle whose kind/cube does not match the unit samples the
         * dummy. Handles that failed to resolve were counted by the engine. */
        if (!v && s_e2.tex[u] && s_e2.tex[u] < VK_ENG_MAX_OBJ) {
            const vk_eng_obj* o = &s_eobj[s_e2.tex[u]];
            /* A surface_view is already counted by the engine with the
             * texture's guest address; counting it here too would double the
             * census. Other kinds (or a cube mismatch on a texture) are only
             * visible from this side. kind in bits 8-15, VkFormat in 0-7. */
            if (o->kind != VK_ENG_VIEW) {
                const u32 tag = ((u32)o->kind << 8) | ((u32)o->fmt & 0xFFu);
                rsx_tex_miss_note(RSX_TEX_MISS_OTHER, 0, s_e2.tex[u], tag,
                                  o->w, o->h, 0, o->mips, o->cube, 0, 0);
            }
        }
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         v ? v : (cube ? dummy_cube : s_vk.dummy.view),
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[3 + RSX_MAX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    return set;
}

/* vk_eng_draw and vk_eng_draw_fill: the vertices come from `vertices`, or are
 * written by `fill` into the slice reserved for them, exactly where the copy
 * would have gone, so the rings, the flushes and the frame pipelining are the
 * same either way. Returns 0 only for a fill draw that found no vertex memory
 * and recorded nothing; every other outcome, drawn or dropped, is 1. */
/* VK_DRAWPROF=1: where the walker's per-draw CPU time goes, as us/draw over
 * each 2 s window. Diagnostic only. */
static int s_drawprof = -1;
static unsigned long long s_dp_ns[8], s_dp_n[9];
static unsigned long long s_dp_t0;
static inline unsigned long long vk_dp_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
static void vk_dp_log(void)
{
    const unsigned long long now = vk_dp_ns();
    if (s_dp_t0 && now - s_dp_t0 < 2000000000ull) return;
    static const char* names[8] = { "desc", "copy", "vtxgpu", "barrier", "rp_begin",
                                    "state", "bind_draw", "rp_end" };
    const unsigned long long nd = s_dp_n[8] ? s_dp_n[8] : 1;
    unsigned long long tot = 0;
    for (int i = 0; i < 8; i++) tot += s_dp_ns[i];
    VK_LOG("drawprof 2s: %llu draws, %.1f us/draw measured", s_dp_n[8], (double)tot / nd / 1000.0);
    for (int i = 0; i < 8; i++)
        VK_LOG("  %-9s %6.1f us/draw (n=%llu)", names[i], (double)s_dp_ns[i] / nd / 1000.0, s_dp_n[i]);
    memset(s_dp_ns, 0, sizeof s_dp_ns);
    memset(s_dp_n, 0, sizeof s_dp_n);
    s_dp_t0 = now;
}
#define DP_T0() do { dp1 = s_drawprof ? vk_dp_ns() : 0; } while (0)
#define DP_T1(i) do { if (s_drawprof) { s_dp_ns[i] += vk_dp_ns() - dp1; s_dp_n[i]++; } } while (0)

static int vk_eng_draw_from(rsx_topology topology, const void* vertices,
                            int (*fill)(void* ctx, void* dst), void* fill_ctx,
                            u32 vertex_count, u32 stride, const u32* indices, u32 index_count)
{
    if (s_drawprof < 0) { const char* e = getenv("VK_DRAWPROF"); s_drawprof = e && *e && strcmp(e, "0") != 0; }
    unsigned long long dp1 = 0;
    if (s_drawprof) { s_dp_n[8]++; DP_T0(); }
    if (!s_e2.pipeline || (!vertices && !fill) || !vertex_count || !stride) return 1;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return 1;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return 1;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return 1;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return 1;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return 1;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return 1;
    }
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return 1;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, n);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return 1;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    const rsx_vtx_gpu_desc* gpu = (const rsx_vtx_gpu_desc*)fill_ctx;
    int use_gpu_vtx = fill && gpu && gpu->gpu_eligible && s_vk.vtx_unpack_pipe;
    const VkDeviceSize raw_bytes = (use_gpu_vtx && gpu) ? (VkDeviceSize)gpu->raw_bytes : 0;
    const VkDeviceSize remap_bytes = (use_gpu_vtx && gpu && (gpu->flags & 1)) ? (VkDeviceSize)vertex_count * 4u : 0;
    VkBuffer vbuf = s_vk.vertices.buf, ibuf = s_e2.indices.buf;
    VkDeviceSize voff = 0, ioff = 0, raw_off = 0, remap_off = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const u32 vsb = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, psb = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
    int batched = vk_eb_on() && vbytes <= VK_EB_VB_BYTES && ibytes <= VK_EB_IB_BYTES &&
                  raw_bytes <= VK_EB_RAW_VB_BYTES && remap_bytes <= VK_EB_REMAP_BYTES;
    if (batched && P->cube_mask && !vk_eng_dummy_cube_view()) return 1;   /* may flush: before any slice */
    if (batched) {
        /* Slices of the rings and a set from the pool; a full one flushes the
         * batch (which empties them all) and the draw takes its slices again. */
        DP_T0();
        for (int tries = 0; tries < 2; tries++) {
            VkDeviceSize vh = s_eb.vb_head, ih = s_eb.ib_head, uh = s_eb.ub_head;
            VkDeviceSize rh = s_eb.raw_vb_head, mh = s_eb.remap_head;
            voff = vk_eb_take(&vh, VK_EB_VB_BYTES, vbytes, 16u);
            ioff = ibytes ? vk_eb_take(&ih, VK_EB_IB_BYTES, ibytes, 16u) : 0;
            raw_off = use_gpu_vtx ? vk_eb_take(&rh, VK_EB_RAW_VB_BYTES, raw_bytes, 16u) : 0;
            remap_off = (use_gpu_vtx && remap_bytes) ? vk_eb_take(&mh, VK_EB_REMAP_BYTES, remap_bytes, 16u) : 0;
            const VkDeviceSize uvs = vk_eb_take(&uh, VK_EB_UB_BYTES, vsb, s_eb.ub_align);
            const VkDeviceSize ups = uvs == (VkDeviceSize)-1 ? uvs : vk_eb_take(&uh, VK_EB_UB_BYTES, psb, s_eb.ub_align);
            if (voff != (VkDeviceSize)-1 && ioff != (VkDeviceSize)-1 && ups != (VkDeviceSize)-1 &&
                raw_off != (VkDeviceSize)-1 && remap_off != (VkDeviceSize)-1 && s_eb.sets < VK_EB_MAX_SETS) {
                VkDescriptorSetLayout lay = P->cube_mask ? s_e2.split_layout : s_g.set_layout;
                VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = s_eb.pool, .descriptorSetCount = 1,
                                                    .pSetLayouts = &lay };
                if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) == VK_SUCCESS) {
                    s_eb.sets++;
                    s_eb.vb_head = vh; s_eb.ib_head = ih; s_eb.ub_head = uh;
                    if (use_gpu_vtx) {
                        s_eb.raw_vb_head = rh;
                        if (remap_bytes) s_eb.remap_head = mh;
                    }
                    vk_eng_put_vs_constants((u8*)s_eb.ub.ptr + uvs, vsb);
                    memcpy((u8*)s_eb.ub.ptr + ups, vk_eng_ucpu() + s_g.ps_off, psb);
                    if (!vk_eng_write_descriptors_to(P, set, s_eb.ub.buf, uvs, ups)) return 1;
                    break;
                }
                set = VK_NULL_HANDLE;
            }
            if (tries || !s_eb_pending) { batched = 0; break; }
            s_eb_why = 1; vk_eng_flush();
        }
        DP_T1(0);
        if (batched && !set) batched = 0;
    }
    if (batched) {
        DP_T0();
        if (use_gpu_vtx) {
            memcpy((u8*)s_eb.raw_vb.ptr + raw_off, gpu->raw_data, (size_t)gpu->raw_bytes);
            if (gpu->flags & 1) {
                memcpy((u8*)s_eb.remap_buf.ptr + remap_off, gpu->remap_indices, (size_t)remap_bytes);
            }
        } else if (fill) {
            if (!fill(fill_ctx, (u8*)s_eb.vb.ptr + voff)) return 1;
        } else {
            memcpy((u8*)s_eb.vb.ptr + voff, vertices, (size_t)vbytes);
        }
        if (ibytes) memcpy((u8*)s_eb.ib.ptr + ioff, indices, (size_t)ibytes);
        DP_T1(1);
        vbuf = s_eb.vb.buf; ibuf = s_eb.ib.buf;
        DP_T0();
        if (!s_eb_pending && vk_begin()) return 1;
        /* Earlier draws of this batch may have rendered what this one samples
         * or blends over: order it after all of them. */
        vk_cmd_after_everything(s_vk.cmd);
        if (use_gpu_vtx) {
            pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.vtx_unpack_pipe);
            pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     s_vk.vtx_unpack_layout, 0, 1,
                                     &s_vk.vtx_unpack_ds[s_ebs_cur], 0, NULL);
            struct {
                uint32_t src_byte_offset;
                uint32_t dst_float_offset;
                uint32_t vertex_count;
                uint32_t src_stride;
                uint32_t dst_stride;
                uint32_t layout_mode;
                uint32_t flags;
                uint32_t remap_offset;
                uint32_t num_attrs;
                uint32_t pad0;
                rsx_vtx_attr_desc attrs[8];
            } pc = {
                .src_byte_offset = (uint32_t)raw_off,
                .dst_float_offset = (uint32_t)(voff / 4u),
                .vertex_count = vertex_count,
                .src_stride = gpu->src_stride,
                .dst_stride = gpu->dst_stride,
                .layout_mode = gpu->layout_mode,
                .flags = gpu->flags,
                .remap_offset = (uint32_t)(remap_off / 4u),
                .num_attrs = gpu->num_attrs,
                .pad0 = 0
            };
            memcpy(pc.attrs, gpu->attrs, sizeof(pc.attrs));
            pvkCmdPushConstants(s_vk.cmd, s_vk.vtx_unpack_layout,
                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            pvkCmdDispatch(s_vk.cmd, (vertex_count + 63u) / 64u, 1, 1);

            VkBufferMemoryBarrier vbarrier = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = s_eb.vb.buf,
                .offset = voff,
                .size = vbytes
            };
            static int s_gpu_vtx_logged = 0;
            if (!s_gpu_vtx_logged) {
                s_gpu_vtx_logged = 1;
                VK_LOG("gpu vertex unpack: offloading draws to Vulkan compute (mode %u)\n", gpu->layout_mode);
            }
            pvkCmdPipelineBarrier(s_vk.cmd,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                  0, 0, NULL, 1, &vbarrier, 0, NULL);
        }
        DP_T1(2);
    } else {
        if (s_eb_pending) { s_eb_why = 2; vk_eng_flush(); }
        if (vk_ensure_vertex_buffer(vbytes)) return fill ? 0 : 1;
        if (fill) { if (!fill(fill_ctx, s_vk.vertices.ptr)) return 1; }
        else memcpy(s_vk.vertices.ptr, vertices, (size_t)vbytes);
        if (ibytes) {
            if (vk_eng_ensure_indices(ibytes)) return 1;
            memcpy(s_e2.indices.ptr, indices, (size_t)ibytes);
        }
        vbuf = s_vk.vertices.buf; ibuf = s_e2.indices.buf; voff = ioff = 0;
        if (!vk_eng_ucpu()) return 1;
        vk_eng_put_vs_constants((u8*)s_g.ubo.ptr, vsb);
        memcpy((u8*)s_g.ubo.ptr + s_g.ps_off, s_e2.ucpu + s_g.ps_off, psb);
        set = vk_eng_write_descriptors(P);
        if (!set) return 1;
        if (vk_begin()) return 1;
    }
    /* Everything submitted before is complete (each submission is waited
     * on); this orders memory for the surfaces this draw touches. */
    DP_T0();
    for (u32 i = 0; i < n; i++)
        vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    DP_T1(3);
    DP_T0();
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    DP_T1(4);
    DP_T0();
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0] + s_e2.sc[2], y1 = s_e2.sc[1] + s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    if (s_vk.depth_bounds_ok)
        pvkCmdSetDepthBounds(s_vk.cmd, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
    DP_T1(5);
    DP_T0();
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             P->cube_mask ? s_e2.split_pipe_layout : s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &voff);
    if (ibytes) {
        pvkCmdBindIndexBuffer(s_vk.cmd, ibuf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    DP_T1(6);
    DP_T0();
    pvkCmdEndRenderPass(s_vk.cmd);
    DP_T1(7);
    if (s_drawprof) vk_dp_log();
    if (!batched) { s_eb.unbatched++; vk_submit_and_wait(); return 1; }
    s_eb.draws++;
    if (++s_eb_pending >= s_eb.max_draws) { s_eb_why = 3; vk_eng_flush_async(); }   /* full: no CPU wait needed */
    return 1;
}


/* ---------------------------------------------------------------------------
 * Per-worker recording for PS3RECOMP_WALKER_MT
 *
 * Each record worker records a whole contiguous chunk into its own primary
 * command buffer, using its own rings, descriptor pool and bind state. No two
 * workers touch the same Vulkan objects while recording. The engine takes the
 * chunk's global turn and then calls mt_chunk_submit, so command buffers reach
 * vkQueueSubmit in guest order; queue_mu serialises the calls themselves.
 * -------------------------------------------------------------------------*/
#define VK_MT_VB_BYTES    (16u << 20)
#define VK_MT_RAW_BYTES   (8u << 20)
#define VK_MT_REMAP_BYTES (1u << 20)
#define VK_MT_IB_BYTES    (4u << 20)
#define VK_MT_UB_BYTES    (4u << 20)

static VkDeviceSize vk_mt_ub_align(void)
{
    return s_eb.ub_align ? s_eb.ub_align : 256u;
}

static void vk_mt_wait_ctx(vk_mt_ctx* c)
{
    if (!c->inflight) return;
    pvkWaitForFences(s_vk.device, 1, &c->fence, VK_TRUE, UINT64_MAX);
    c->inflight = 0;
}

static void vk_mt_wait_all(void)
{
    if (!s_mtctx_on) return;
    for (int i = 0; i < VK_MT_MAX_WORKERS; i++) vk_mt_wait_ctx(&s_mtctx[i]);
}

static int vk_mt_oneshot_begin(void* user)
{
    vk_mt_ctx* c = (vk_mt_ctx*)user;
    if (!c || !c->os_cmd) return -1;
    pvkWaitForFences(s_vk.device, 1, &c->os_fence, VK_TRUE, UINT64_MAX);
    VK_CHECK(pvkResetCommandBuffer(c->os_cmd, 0), "vkResetCommandBuffer(mt oneshot)");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(c->os_cmd, &bi), "vkBeginCommandBuffer(mt oneshot)");
    return 0;
}

static int vk_mt_oneshot_submit(void* user)
{
    vk_mt_ctx* c = (vk_mt_ctx*)user;
    if (!c || !c->os_cmd) return -1;
    VK_CHECK(pvkEndCommandBuffer(c->os_cmd), "vkEndCommandBuffer(mt oneshot)");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &c->os_cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &c->os_fence), "vkResetFences(mt oneshot)");
    VK_CHECK(vk_queue_submit(&si, c->os_fence), "vkQueueSubmit(mt oneshot)");
    VK_CHECK(pvkWaitForFences(s_vk.device, 1, &c->os_fence, VK_TRUE, UINT64_MAX),
             "vkWaitForFences(mt oneshot)");
    return 0;
}

static void vk_mt_ctx_free(vk_mt_ctx* c)
{
    vk_bind_user(c);
    vk_mt_wait_ctx(c);
    if (c->os_fence) pvkWaitForFences(s_vk.device, 1, &c->os_fence, VK_TRUE, UINT64_MAX);
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (c->e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, c->e2.rp[f][n], NULL);
    for (u32 i = 0; i < c->e2.fb_count; i++)
        if (c->e2.fb[i].fb) pvkDestroyFramebuffer(s_vk.device, c->e2.fb[i].fb, NULL);
    c->e2.fb_count = 0;
    vk_destroy_image(&c->e2.scratch_depth);
    vk_destroy_buffer(&c->e2.indices);
    free(c->e2.ucpu); c->e2.ucpu = NULL;
    if (c->e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, c->e2.split_pipe_layout, NULL);
    if (c->e2.split_pool) pvkDestroyDescriptorPool(s_vk.device, c->e2.split_pool, NULL);
    if (c->e2.split_layout) pvkDestroyDescriptorSetLayout(s_vk.device, c->e2.split_layout, NULL);
    for (u32 i = 0; i < c->e2.esamp_count; i++) pvkDestroySampler(s_vk.device, c->e2.esamp[i].s, NULL);
    c->e2.esamp_count = 0;
    vk_destroy_buffer(&c->vb); vk_destroy_buffer(&c->raw_vb);
    vk_destroy_buffer(&c->remap_buf); vk_destroy_buffer(&c->ib);
    vk_destroy_buffer(&c->ub);
    if (c->dpool) pvkDestroyDescriptorPool(s_vk.device, c->dpool, NULL);
    if (c->vtx_pool) pvkDestroyDescriptorPool(s_vk.device, c->vtx_pool, NULL);
    if (c->os_fence) pvkDestroyFence(s_vk.device, c->os_fence, NULL);
    if (c->fence) pvkDestroyFence(s_vk.device, c->fence, NULL);
    if (c->pool) pvkDestroyCommandPool(s_vk.device, c->pool, NULL);
    memset(c, 0, sizeof *c);
}

static int vk_mt_init(void* user, int workers)
{
    (void)user;
    if (s_mtctx_on) return 1;
    if (workers <= 0) return 1;
    if (workers > VK_MT_MAX_WORKERS) workers = VK_MT_MAX_WORKERS;
    /* The worker path records into ring slices; without draw batching there
     * are none, so leave the serialised turn path in charge. */
    if (!vk_eb_on()) {
        VK_LOG("walker-mt: draw batching is off; per-worker recording disabled\n");
        return 0;
    }
    s_mtctx_on = 0;
    int made = 0;
    for (int i = 0; i < workers; i++) {
        vk_mt_ctx* c = &s_mtctx[i];
        memset(c, 0, sizeof *c);
        c->id = i;
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                        .queueFamilyIndex = s_vk.queue_family };
        VkCommandBuffer cbs[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                  .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * 64u },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES * 64u },
            { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES * 64u },
        };
        VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 64, .poolSizeCount = 3, .pPoolSizes = ps };
        VkDescriptorPoolSize vps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
        VkDescriptorPoolCreateInfo vpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &vps };
        if (pvkCreateCommandPool(s_vk.device, &pci, NULL, &c->pool) != VK_SUCCESS) break;
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = c->pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 2 };
        if (pvkAllocateCommandBuffers(s_vk.device, &cai, cbs) != VK_SUCCESS) break;
        c->cmd = cbs[0]; c->os_cmd = cbs[1];
        if (pvkCreateFence(s_vk.device, &fci, NULL, &c->fence) != VK_SUCCESS ||
            pvkCreateFence(s_vk.device, &fci, NULL, &c->os_fence) != VK_SUCCESS) break;
        if (vk_create_host_buffer(&c->vb, VK_MT_VB_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !c->vb.coherent ||
            vk_create_host_buffer(&c->raw_vb, VK_MT_RAW_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !c->raw_vb.coherent ||
            vk_create_host_buffer(&c->remap_buf, VK_MT_REMAP_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !c->remap_buf.coherent ||
            vk_create_host_buffer(&c->ib, VK_MT_IB_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !c->ib.coherent ||
            vk_create_host_buffer(&c->ub, VK_MT_UB_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !c->ub.coherent) break;
        if (pvkCreateDescriptorPool(s_vk.device, &dpci, NULL, &c->dpool) != VK_SUCCESS) break;
        if (s_vk.vtx_unpack_pipe) {
            if (pvkCreateDescriptorPool(s_vk.device, &vpci, NULL, &c->vtx_pool) != VK_SUCCESS) break;
            VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = c->vtx_pool, .descriptorSetCount = 1,
                                                 .pSetLayouts = &s_vk.vtx_unpack_dsl };
            if (pvkAllocateDescriptorSets(s_vk.device, &dsai, &c->vtx_set) != VK_SUCCESS) break;
            VkDescriptorBufferInfo bi[3] = {
                { c->raw_vb.buf, 0, VK_WHOLE_SIZE },
                { c->vb.buf,     0, VK_WHOLE_SIZE },
                { c->remap_buf.buf, 0, VK_WHOLE_SIZE },
            };
            VkWriteDescriptorSet w[3];
            for (int k = 0; k < 3; k++)
                w[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = c->vtx_set, .dstBinding = (uint32_t)k, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[k] };
            pvkUpdateDescriptorSets(s_vk.device, 3, w, 0, NULL);
        }
        made++;
    }
    if (made != workers) {
        for (int i = 0; i < made; i++) vk_mt_ctx_free(&s_mtctx[i]);
        VK_LOG("walker-mt: per-worker Vulkan contexts failed; serialised turn path\n");
        return 0;
    }
    /* The dummy cube is a shared handle; make it once on the state thread so
     * workers never allocate a backend object. */
    vk_bind_user(NULL);
    if (s_g.on) {
        vk_eng_dummy_cube_view();               /* creates s_e2_global.dummy_cube */
        const u32 cube = s_e2_global.dummy_cube;
        for (int i = 0; i < workers; i++) s_mtctx[i].e2.dummy_cube = cube;
    }
    s_mtctx_on = workers;
    VK_LOG("walker-mt: %d per-worker Vulkan recording contexts (VB %u MB, IB %u MB)\n",
           workers, VK_MT_VB_BYTES >> 20, VK_MT_IB_BYTES >> 20);
    return 1;
}

static void* vk_mt_ctx_get(void* user, int worker)
{
    (void)user;
    if (!s_mtctx_on || worker < 0 || worker >= s_mtctx_on) return NULL;
    return &s_mtctx[worker];
}

static void vk_mt_shutdown(void* user)
{
    (void)user;
    if (!s_mtctx_on) return;
    for (int i = 0; i < s_mtctx_on; i++) vk_mt_ctx_free(&s_mtctx[i]);
    s_mtctx_on = 0;
}

static int vk_mt_chunk_begin(void* user, void* ctx)
{
    vk_mt_ctx* c = (vk_mt_ctx*)ctx;
    if (!c) return -1;
    vk_bind_user(c);
    vk_mt_wait_ctx(c);
    if (c->dpool) pvkResetDescriptorPool(s_vk.device, c->dpool, 0);
    c->vb_head = c->raw_head = c->remap_head = c->ib_head = c->ub_head = 0;
    VK_CHECK(pvkResetCommandBuffer(c->cmd, 0), "vkResetCommandBuffer(mt)");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(c->cmd, &bi), "vkBeginCommandBuffer(mt)");
    vk_t_chunk = c->cmd;
    return 0;
}

static int vk_mt_chunk_submit(void* user, void* ctx)
{
    (void)user;
    vk_mt_ctx* c = (vk_mt_ctx*)ctx;
    if (!c) return -1;
    VK_CHECK(pvkEndCommandBuffer(c->cmd), "vkEndCommandBuffer(mt)");
    vk_t_chunk = NULL;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &c->cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &c->fence), "vkResetFences(mt)");
    VK_CHECK(vk_queue_submit(&si, c->fence), "vkQueueSubmit(mt)");
    c->inflight = 1;
    c->chunks++;
    return 0;
}

/* The worker-recorded body of vk_eng_draw_from: the same bindings and draw,
 * but into the context's rings and command buffer, and no submit. */
static int vk_mt_draw(vk_mt_ctx* c, rsx_topology topology, const void* vertices,
                      int (*fill)(void* ctx, void* dst), void* fill_ctx,
                      u32 vertex_count, u32 stride, const u32* indices, u32 index_count)
{
    vk_bind_user(c);
    if (!s_e2.pipeline || (!vertices && !fill) || !vertex_count || !stride) return 1;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return 1;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return 1;

    const u32 n = c->bound.nrt;
    if (n < 1 || n != P->rt_count) return 1;
    vk_eng_obj* rt0 = vk_eng_get(c->bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return 1;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(c->bound.rt[i], VK_ENG_COLOR)) return 1;
    if (rt0->fmt != P->rt_fmt) return 1;
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(c->bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return 1;
    const u32 dkey = (zo && dview == zo->im.view) ? c->bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, n);
    VkFramebuffer fb = vk_eng_framebuffer(rp, c->bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return 1;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    const rsx_vtx_gpu_desc* gpu = (const rsx_vtx_gpu_desc*)fill_ctx;
    int use_gpu_vtx = fill && gpu && gpu->gpu_eligible && s_vk.vtx_unpack_pipe;
    const VkDeviceSize raw_bytes = (use_gpu_vtx && gpu) ? (VkDeviceSize)gpu->raw_bytes : 0;
    const VkDeviceSize remap_bytes = (use_gpu_vtx && gpu && (gpu->flags & 1)) ? (VkDeviceSize)vertex_count * 4u : 0;
    if (vbytes > VK_MT_VB_BYTES || ibytes > VK_MT_IB_BYTES ||
        raw_bytes > VK_MT_RAW_BYTES || remap_bytes > VK_MT_REMAP_BYTES)
        return 1;

    VkDescriptorSetLayout lay;
    if (P->cube_mask) {
        if (!vk_eng_split_layout()) return 1;
        lay = c->e2.split_layout;
    } else {
        lay = s_g.set_layout;
    }
    const u32 vsb = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, psb = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
    VkDeviceSize voff, ioff = 0, raw_off = 0, remap_off = 0;
    voff = vk_eb_take(&c->vb_head, VK_MT_VB_BYTES, vbytes, 16u);
    if (ioff == 0) ioff = ibytes ? vk_eb_take(&c->ib_head, VK_MT_IB_BYTES, ibytes, 16u) : 0;
    raw_off = use_gpu_vtx ? vk_eb_take(&c->raw_head, VK_MT_RAW_BYTES, raw_bytes, 16u) : 0;
    remap_off = (use_gpu_vtx && remap_bytes) ? vk_eb_take(&c->remap_head, VK_MT_REMAP_BYTES, remap_bytes, 16u) : 0;
    const VkDeviceSize uvs = vk_eb_take(&c->ub_head, VK_MT_UB_BYTES, vsb, vk_mt_ub_align());
    const VkDeviceSize ups = (uvs == (VkDeviceSize)-1) ? uvs
        : vk_eb_take(&c->ub_head, VK_MT_UB_BYTES, psb, vk_mt_ub_align());
    if (voff == (VkDeviceSize)-1 || ioff == (VkDeviceSize)-1 || ups == (VkDeviceSize)-1 ||
        raw_off == (VkDeviceSize)-1 || remap_off == (VkDeviceSize)-1) {
        /* A single chunk's draw does not fit the worker rings. Drop it rather
         * than submit out of order; the fallback counter is in the engine. */
        if (!(c->e2.warned & 256u)) {
            VK_LOG("walker-mt: worker %d ring full; a draw was dropped\n", c->id);
            c->e2.warned |= 256u;
        }
        return 1;
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = c->dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &lay };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) return 1;
    vk_eng_put_vs_constants((u8*)c->ub.ptr + uvs, vsb);
    memcpy((u8*)c->ub.ptr + ups, vk_eng_ucpu() + s_g.ps_off, psb);
    if (!vk_eng_write_descriptors_to(P, set, c->ub.buf, uvs, ups)) return 1;

    if (use_gpu_vtx) {
        memcpy((u8*)c->raw_vb.ptr + raw_off, gpu->raw_data, (size_t)gpu->raw_bytes);
        if (gpu->flags & 1)
            memcpy((u8*)c->remap_buf.ptr + remap_off, gpu->remap_indices, (size_t)remap_bytes);
    } else if (fill) {
        if (!fill(fill_ctx, (u8*)c->vb.ptr + voff)) return 1;
    } else {
        memcpy((u8*)c->vb.ptr + voff, vertices, (size_t)vbytes);
    }
    if (ibytes) memcpy((u8*)c->ib.ptr + ioff, indices, (size_t)ibytes);

    vk_cmd_after_everything(c->cmd);
    if (use_gpu_vtx) {
        pvkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.vtx_unpack_pipe);
        pvkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 s_vk.vtx_unpack_layout, 0, 1, &c->vtx_set, 0, NULL);
        struct {
            uint32_t src_byte_offset;
            uint32_t dst_float_offset;
            uint32_t vertex_count;
            uint32_t src_stride;
            uint32_t dst_stride;
            uint32_t layout_mode;
            uint32_t flags;
            uint32_t remap_offset;
            uint32_t num_attrs;
            uint32_t pad0;
            rsx_vtx_attr_desc attrs[8];
        } pc = {
            .src_byte_offset = (uint32_t)raw_off,
            .dst_float_offset = (uint32_t)(voff / 4u),
            .vertex_count = vertex_count,
            .src_stride = gpu->src_stride,
            .dst_stride = gpu->dst_stride,
            .layout_mode = gpu->layout_mode,
            .flags = gpu->flags,
            .remap_offset = (uint32_t)(remap_off / 4u),
            .num_attrs = gpu->num_attrs,
        };
        memcpy(pc.attrs, gpu->attrs, sizeof(pc.attrs));
        pvkCmdPushConstants(c->cmd, s_vk.vtx_unpack_layout,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        pvkCmdDispatch(c->cmd, (vertex_count + 63u) / 64u, 1, 1);
        VkBufferMemoryBarrier vbarrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = c->vb.buf, .offset = voff, .size = vbytes };
        pvkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &vbarrier, 0, NULL);
    }

    for (u32 i = 0; i < n; i++)
        vk_barrier_image_on(c->cmd, s_eobj[c->bound.rt[i]].im.img,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(c->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0] + s_e2.sc[2], y1 = s_e2.sc[1] + s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(c->cmd, 0, 1, &vp);
    pvkCmdSetScissor(c->cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(c->cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    if (s_vk.depth_bounds_ok)
        pvkCmdSetDepthBounds(c->cmd, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
    pvkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             P->cube_mask ? c->e2.split_pipe_layout : s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(c->cmd, 0, 1, &c->vb.buf, &voff);
    if (ibytes) {
        pvkCmdBindIndexBuffer(c->cmd, c->ib.buf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(c->cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(c->cmd, vertex_count, 1, 0, 0);
    }
    pvkCmdEndRenderPass(c->cmd);
    c->draws++;
    return 1;
}

static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    if (user) {   /* a record worker's context */
        vk_mt_draw((vk_mt_ctx*)user, topology, vertices, NULL, NULL,
                   vertex_count, stride, indices, index_count);
        return;
    }
    if (!vertices) return;
    vk_eng_draw_from(topology, vertices, NULL, NULL, vertex_count, stride, indices, index_count);
}

/* PS3RECOMP_ENG_VTX_DIRECT (the engine's switch) decides whether this is used. */
static int vk_eng_draw_fill(void* user, rsx_topology topology, int (*fill)(void* ctx, void* dst),
                            void* ctx, u32 vertex_count, u32 stride, const u32* indices,
                            u32 index_count)
{
    if (!fill) return 0;
    if (user)
        return vk_mt_draw((vk_mt_ctx*)user, topology, NULL, fill, ctx,
                          vertex_count, stride, indices, index_count);
    return vk_eng_draw_from(topology, NULL, fill, ctx, vertex_count, stride, indices, index_count);
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .cube_target_view     = vk_eng_cube_target_view,
    .cube_target_last     = vk_eng_cube_target_last,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .color_snapshot       = vk_eng_color_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .set_pipeline_key     = vk_eng_set_pipeline_key,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_vs_constants_range = vk_eng_bind_vs_constants_range,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bounds     = vk_eng_set_depth_bounds,
    .draw                 = vk_eng_draw,
    .draw_fill            = vk_eng_draw_fill,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
    .readback_async       = vk_eng_readback_async,
    .readback_async_fetch = vk_eng_readback_fetch,
    .readback_async_release = vk_eng_readback_release,
    .submit_async         = vk_eng_submit_async,
    .submit_batch         = vk_eng_submit_batch,
    .mt_init              = vk_mt_init,
    .mt_shutdown          = vk_mt_shutdown,
    .mt_chunk_begin       = vk_mt_chunk_begin,
    .mt_chunk_submit      = vk_mt_chunk_submit,
    .mt_ctx               = vk_mt_ctx_get,
    .depth_readback       = vk_eng_depth_readback,
    .color_target_write   = vk_eng_color_target_write,
    /* Every draw and every upload submits and waits, so an upload runs after
     * each earlier draw that sampled the old bytes has completed. */
    .texture_upload_ordered = 1,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    pthread_mutex_init(&s_vk.present_q_mu, NULL);
    pthread_cond_init(&s_vk.present_q_cv, NULL);
    pthread_cond_init(&s_vk.present_slot_cv, NULL);
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    u32 n_props = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &n_props, NULL);
    u32 queue_count = 1;
    if (n_props > 0 && n_props <= 64) {
        VkQueueFamilyProperties props[64];
        pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &n_props, props);
        if (s_vk.queue_family < n_props && props[s_vk.queue_family].queueCount >= 2) {
            queue_count = 2;
        }
    }

    float priorities[2] = { 1.0f, 0.5f };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = queue_count,
                                    .pQueuePriorities = priorities };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    /* User clip planes (rsx_vp_apply_user_clip) write ClipDistance. */
    want.shaderClipDistance = have.shaderClipDistance;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    /* The depth bounds test: a deferred renderer bounds its light pass to
     * the geometry and its sky to the far plane with it. Optional in
     * Vulkan; without it the test stays off, as before. */
    want.depthBounds = have.depthBounds;
    s_vk.depth_bounds_ok = have.depthBounds ? 1 : 0;
    /* Per-target colour mask and blend enable (an MRT set's targets B..D
     * have their own); without it every target takes target A's. */
    want.independentBlend = have.independentBlend;
    s_vk.indep_blend_ok = have.independentBlend ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    if (queue_count >= 2) {
        pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 1, &s_vk.present_queue);
        VK_LOG("requested 2 queues: queue 0 for walker, queue 1 for presentation\n");
    } else {
        s_vk.present_queue = s_vk.queue;
    }
    vk_pipe_cache_init();

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");
    /* The present's own (see vk_state). */
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.present_pool), "vkCreateCommandPool");
    cai.commandPool = s_vk.present_pool;
    cai.commandBufferCount = 1;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.present_cmd), "vkAllocateCommandBuffers");
    cai.commandBufferCount = VK_PRESENT_SLOTS;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_vk.window_cmd), "vkAllocateCommandBuffers");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.present_fence), "vkCreateFence");
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.window_fence[i]), "vkCreateFence");

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        int ok = 1;
        for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
            if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire[i]) != VK_SUCCESS)
                ok = 0;
        if (!ok || vk_create_swapchain()) {
            vk_window_drop("the swapchain could not be created");
        } else {
            const char* pt = getenv("PS3RECOMP_VK_PRESENT_THREAD");
            if (!(pt && pt[0] == '0') && vk_present_thread_start())
                VK_LOG("presentation thread could not start; presenting synchronously\n");
        }
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    rsx_vulkan_backend_present_finish();   /* a host that began one and quit */
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
    vk_present_thread_stop();        /* drains queued presents, then joins */
    vk_dump_queue_shutdown();
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_device_idle();
        if (s_pipe_cache) {
            vk_pipe_cache_save();
            pvkDestroyPipelineCache(s_vk.device, s_pipe_cache, NULL);
            s_pipe_cache = VK_NULL_HANDLE;
        }
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        if (s_vk.present_fence) pvkDestroyFence(s_vk.device, s_vk.present_fence, NULL);
        for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
            if (s_vk.window_fence[i]) pvkDestroyFence(s_vk.device, s_vk.window_fence[i], NULL);
        if (s_vk.present_pool)  pvkDestroyCommandPool(s_vk.device, s_vk.present_pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    pthread_cond_destroy(&s_vk.present_slot_cv);
    pthread_cond_destroy(&s_vk.present_q_cv);
    pthread_mutex_destroy(&s_vk.present_q_mu);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    vk_window_stats_pump();
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
    }
    return quit ? -1 : 0;
}

void rsx_vulkan_backend_present(void)
{
    rsx_vulkan_backend_present_begin();
    /* VK_PRESENT_DEFER=1: leave the wait for this frame's GPU work, its
     * readback and the window blit to the next present (whose begin finishes
     * it first), so the walker records the next frame while the GPU draws this
     * one. The window shows each frame one present later. */
    static int defer = -1;
    if (defer < 0) { const char* e = getenv("VK_PRESENT_DEFER"); defer = e && *e == '1'; }
    if (!(defer || s_ebs_on) || !s_vk.eng_active) rsx_vulkan_backend_present_finish();
}

void rsx_vulkan_backend_present_begin(void)
{
    if (!s_vk.device) return;
    s_vk.present_split = 1;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    s_vk.present_split = 0;
}

void rsx_vulkan_backend_present_finish(void)
{
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    vk_present_complete_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
}

u32 rsx_vulkan_backend_presented_center(void)
{
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

int rsx_vulkan_backend_depth_bounds(void)
{
    return s_vk.eng_active && s_vk.depth_bounds_ok;
}

#endif /* !_WIN32 */
