// Vulkan backend foundation: instance, device, queue, allocator, optional SDL swapchain.
// Design: docs/VULKAN_PORT.md. The Vulkan loader is opened at run time (psp_vk_loader.h), never linked.
#pragma once
// VMA targets Vulkan 1.1 here: the 1.3 entry points it would otherwise call (vkGetDeviceBufferMemoryRequirements...) are not
// exported by libvulkan.so at Android API 29.
#ifndef VMA_VULKAN_VERSION
#define VMA_VULKAN_VERSION 1001000
#endif
#include "psp_vk_loader.h"   // <vulkan/vulkan.h> with function pointers resolved at run time
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct VmaAllocator_T;

namespace pspvk {

struct ContextOptions {
    bool validation = false;          // PSPRECOMP_VK_VALIDATION=1: VK_LAYER_KHRONOS_validation if installed
    int  device_index = -1;           // PSPRECOMP_VK_DEVICE=<index|name substring>; -1 = best (discrete first)
    std::string device_name;          // substring match, wins over device_index when non-empty
    SDL_Window* window = nullptr;     // null = headless (no surface, no swapchain)
    bool want_ray_query = false;      // enable VK_KHR_ray_query + acceleration structure when the device has them
};

struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    int hdr = 0;                      // 0 SDR, 1 scRGB (R16G16B16A16_SFLOAT, extended sRGB linear), 2 HDR10 (A2B10G10R10, ST2084 PQ)
};

class Context {
public:
    ~Context() { destroy(); }
    bool init(const ContextOptions& opt, std::string* err);
    void destroy();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return phys_; }
    VkDevice device() const { return dev_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return qfam_; }
    VkCommandPool command_pool() const { return pool_; }
    VmaAllocator_T* allocator() const { return vma_; }
    VkSurfaceKHR surface() const { return surface_; }
    const VkPhysicalDeviceProperties& props() const { return props_; }
    bool has_ray_query() const { return ray_query_; }
    bool has_multi_draw() const { return multi_draw_; }
    const std::string& device_name() const { return name_; }

    bool create_surface(std::string* err);
    bool recreate_surface(std::string* err);   // after VK_ERROR_SURFACE_LOST_KHR (Android window recreated)
    bool create_swapchain(int w, int h, bool vsync, std::string* err);
    void destroy_swapchain();
    const Swapchain& swapchain() const { return sc_; }
    // HDR output: the swapchain takes an HDR format when the user wants it and the surface offers one (needs
    // VK_EXT_swapchain_colorspace, enabled at instance creation when present). Takes effect at the next create_swapchain.
    void set_hdr_wanted(bool on) { hdr_wanted_ = on; }
    bool hdr_wanted() const { return hdr_wanted_; }
    bool hdr_supported() const { return hdr_supported_; }   // as of the last create_swapchain
    const std::string& hdr_reason() const { return hdr_reason_; }

    // One-shot command buffer helpers (init-time and tests; the frame path uses its own ring).
    VkCommandBuffer begin_once();
    bool end_once(VkCommandBuffer cb);   // submit + wait

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qfam_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VmaAllocator_T* vma_ = nullptr;
    VkPhysicalDeviceProperties props_{};
    std::string name_;
    bool ray_query_ = false;
    bool multi_draw_ = false;           // VK_EXT_multi_draw enabled (one command for a batch of strip segments)
    Swapchain sc_;
    bool hdr_wanted_ = false, hdr_supported_ = false, colorspace_ext_ = false;
    std::string hdr_reason_ = "The Vulkan swapchain has not been created yet.";
    SDL_Window* window_ = nullptr;
};

/// Quick probe used for backend selection: true if a Vulkan 1.1 device with a graphics queue exists.
bool vulkan_available(std::string* why);

}  // namespace pspvk
