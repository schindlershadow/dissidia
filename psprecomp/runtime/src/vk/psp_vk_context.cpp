#include "psp_vk_context.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include "../../third_party/vk_mem_alloc.h"

namespace pspvk {

static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    std::fprintf(stderr, "[VK%s] %s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? " ERROR" : "", d->pMessage);
    return VK_FALSE;
}

static bool has_ext(const std::vector<VkExtensionProperties>& v, const char* n) {
    for (auto& e : v) if (!std::strcmp(e.extensionName, n)) return true;
    return false;
}

bool Context::init(const ContextOptions& opt, std::string* err) {
    auto fail = [&](const char* m, VkResult r = VK_SUCCESS) { if (err) { *err = m; if (r) *err += " (VkResult " + std::to_string(r) + ")"; } return false; };
    window_ = opt.window;

    std::vector<const char*> iext;
    if (window_) {
        unsigned n = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(window_, &n, nullptr)) return fail("SDL_Vulkan_GetInstanceExtensions failed");
        iext.resize(n);
        SDL_Vulkan_GetInstanceExtensions(window_, &n, iext.data());
    }
    std::vector<const char*> layers;
    uint32_t nl = 0; vkEnumerateInstanceLayerProperties(&nl, nullptr);
    std::vector<VkLayerProperties> lp(nl); vkEnumerateInstanceLayerProperties(&nl, lp.data());
    bool dbg = false;
    if (opt.validation) {
        for (auto& l : lp) if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
        uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ep(ne); vkEnumerateInstanceExtensionProperties(nullptr, &ne, ep.data());
        if (has_ext(ep, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); dbg = true; }
        if (layers.empty()) std::fprintf(stderr, "[VK] validation requested but VK_LAYER_KHRONOS_validation is not installed\n");
    }

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "Dissidia Trededecim"; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = (uint32_t)iext.size(); ici.ppEnabledExtensionNames = iext.data();
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed", r);
    if (dbg) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = dbg_cb;
        if (fn) fn(instance_, &mi, nullptr, &messenger_);
    }
    if (window_ && !SDL_Vulkan_CreateSurface(window_, instance_, &surface_)) return fail("SDL_Vulkan_CreateSurface failed");

    // Pick a device: has a graphics queue (+ present support when windowed), then name/index, then discrete > integrated > other.
    uint32_t nd = 0; vkEnumeratePhysicalDevices(instance_, &nd, nullptr);
    std::vector<VkPhysicalDevice> devs(nd); vkEnumeratePhysicalDevices(instance_, &nd, devs.data());
    int best = -1, best_score = -1; uint32_t best_q = 0;
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qp(nq); vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qp.data());
        int q = -1;
        for (uint32_t k = 0; k < nq; k++) {
            if (!(qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 ok = VK_TRUE;
            if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], k, surface_, &ok);
            if (ok) { q = (int)k; break; }
        }
        if (q < 0) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (!opt.device_name.empty()) score = std::string(p.deviceName).find(opt.device_name) != std::string::npos ? 100 : -1;
        else if (opt.device_index >= 0) score = (int)i == opt.device_index ? 100 : -1;
        if (score > best_score) { best_score = score; best = (int)i; best_q = (uint32_t)q; }
    }
    if (best < 0) return fail("no suitable Vulkan 1.1 device");
    phys_ = devs[best]; qfam_ = best_q;
    vkGetPhysicalDeviceProperties(phys_, &props_);
    name_ = props_.deviceName;

    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(phys_, nullptr, &nde, nullptr);
    std::vector<VkExtensionProperties> dep(nde); vkEnumerateDeviceExtensionProperties(phys_, nullptr, &nde, dep.data());
    std::vector<const char*> dext;
    if (surface_) dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (opt.want_ray_query && has_ext(dep, "VK_KHR_ray_query") && has_ext(dep, "VK_KHR_acceleration_structure")
        && has_ext(dep, "VK_KHR_deferred_host_operations") && has_ext(dep, "VK_KHR_buffer_device_address")) {
        // Feature chain is filled in when the RT milestone lands; recorded here so callers can query the capability.
        ray_query_ = false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feat{};
    VkPhysicalDeviceFeatures avail; vkGetPhysicalDeviceFeatures(phys_, &avail);
    feat.samplerAnisotropy = avail.samplerAnisotropy;
    feat.dualSrcBlend = avail.dualSrcBlend;      // GE blend modes that GL expresses with dual-source
    feat.independentBlend = avail.independentBlend;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)dext.size(); di.ppEnabledExtensionNames = dext.data();
    di.pEnabledFeatures = &feat;
    r = vkCreateDevice(phys_, &di, nullptr, &dev_);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed", r);
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = qfam_;
    r = vkCreateCommandPool(dev_, &pi, nullptr, &pool_);
    if (r != VK_SUCCESS) return fail("vkCreateCommandPool failed", r);

    VmaAllocatorCreateInfo vi{};
    vi.physicalDevice = phys_; vi.device = dev_; vi.instance = instance_; vi.vulkanApiVersion = VK_API_VERSION_1_1;
    VmaAllocator a = nullptr;
    r = vmaCreateAllocator(&vi, &a);
    if (r != VK_SUCCESS) return fail("vmaCreateAllocator failed", r);
    vma_ = a;
    std::fprintf(stderr, "[VK] device: %s (Vulkan %u.%u.%u)\n", name_.c_str(), VK_VERSION_MAJOR(props_.apiVersion),
                 VK_VERSION_MINOR(props_.apiVersion), VK_VERSION_PATCH(props_.apiVersion));
    return true;
}

bool Context::create_swapchain(int w, int h, bool vsync, std::string* err) {
    if (!surface_) { if (err) *err = "no surface"; return false; }
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fm(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, fm.data());
    VkSurfaceFormatKHR sf = fm.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fm[0];
    for (auto& f : fm) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }  // UNORM: the GE is not sRGB
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = std::clamp<uint32_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width);
                                    ext.height = std::clamp<uint32_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height); }
    uint32_t np = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &np, nullptr);
    std::vector<VkPresentModeKHR> pm(np); vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &np, pm.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) for (auto m : pm) { if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; break; } if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m; }
    uint32_t cnt = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount && cnt > caps.maxImageCount) cnt = caps.maxImageCount;
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = surface_; si.minImageCount = cnt; si.imageFormat = sf.format; si.imageColorSpace = sf.colorSpace;
    si.imageExtent = ext; si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // present = blit/copy from the scaled target
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; si.preTransform = caps.currentTransform;
    si.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    si.presentMode = mode; si.clipped = VK_TRUE; si.oldSwapchain = sc_.handle;
    VkSwapchainKHR nsc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(dev_, &si, nullptr, &nsc);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateSwapchainKHR failed (" + std::to_string(r) + ")"; return false; }
    if (sc_.handle) vkDestroySwapchainKHR(dev_, sc_.handle, nullptr);
    sc_.handle = nsc; sc_.format = sf.format; sc_.extent = ext;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(dev_, nsc, &ni, nullptr);
    sc_.images.resize(ni); vkGetSwapchainImagesKHR(dev_, nsc, &ni, sc_.images.data());
    return true;
}

void Context::destroy_swapchain() {
    if (sc_.handle) vkDestroySwapchainKHR(dev_, sc_.handle, nullptr);
    sc_ = Swapchain{};
}

VkCommandBuffer Context::begin_once() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev_, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

bool Context::end_once(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkFence f; VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(dev_, &fi, nullptr, &f);
    bool ok = vkQueueSubmit(queue_, 1, &si, f) == VK_SUCCESS && vkWaitForFences(dev_, 1, &f, VK_TRUE, 10ull * 1000 * 1000 * 1000) == VK_SUCCESS;
    vkDestroyFence(dev_, f, nullptr);
    vkFreeCommandBuffers(dev_, pool_, 1, &cb);
    return ok;
}

void Context::destroy() {
    if (dev_) vkDeviceWaitIdle(dev_);
    destroy_swapchain();
    if (vma_) { vmaDestroyAllocator((VmaAllocator)vma_); vma_ = nullptr; }
    if (pool_) { vkDestroyCommandPool(dev_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
    if (dev_) { vkDestroyDevice(dev_, nullptr); dev_ = VK_NULL_HANDLE; }
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (messenger_) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
}

bool vulkan_available(std::string* why) {
    Context c; ContextOptions o;
    return c.init(o, why);
}

}  // namespace pspvk
