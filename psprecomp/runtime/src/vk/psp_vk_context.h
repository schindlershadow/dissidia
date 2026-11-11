// Vulkan backend foundation: instance, device, queue, allocator, optional SDL swapchain.
// Design: docs/VULKAN_PORT.md. Desktop links libvulkan directly; Android will dlopen it (same entry points).
#pragma once
#include <vulkan/vulkan.h>
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
    const std::string& device_name() const { return name_; }

    bool create_swapchain(int w, int h, bool vsync, std::string* err);
    void destroy_swapchain();
    const Swapchain& swapchain() const { return sc_; }

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
    Swapchain sc_;
    SDL_Window* window_ = nullptr;
};

/// Quick probe used for backend selection: true if a Vulkan 1.1 device with a graphics queue exists.
bool vulkan_available(std::string* why);

}  // namespace pspvk
