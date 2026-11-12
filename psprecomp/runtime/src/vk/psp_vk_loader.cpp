#include "psp_vk_loader.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <mutex>

#define PSP_VK_DEFINE(name) PFN_##name name = nullptr;
PSP_VK_DEFINE(vkGetInstanceProcAddr)
PSP_VK_DEFINE(vkCreateInstance)
PSP_VK_DEFINE(vkEnumerateInstanceExtensionProperties)
PSP_VK_DEFINE(vkEnumerateInstanceLayerProperties)
PSP_VK_FUNCS(PSP_VK_DEFINE)

namespace pspvk {

bool load_global(std::string* err) {
    static std::mutex mu;
    static int state = 0;           // 0 = untried, 1 = loaded, -1 = failed
    static std::string why;
    std::lock_guard<std::mutex> lk(mu);
    if (state == 0) {
        state = -1;
        if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);   // SDL loads the Vulkan driver through its video layer
        if (SDL_Vulkan_LoadLibrary(nullptr) == 0) {
            vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
        } else {
            // SDL's video driver has no Vulkan (headless tools, the offscreen driver): open the loader library directly.
            why = std::string("no Vulkan driver found (") + SDL_GetError() + ")";
            for (const char* name : {"libvulkan.so.1", "vulkan-1.dll", "libvulkan.1.dylib"})
                if (void* h = SDL_LoadObject(name)) {
                    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(h, "vkGetInstanceProcAddr");
                    if (vkGetInstanceProcAddr) break;
                }
        }
        {
            if (!vkGetInstanceProcAddr) {
                if (why.empty()) why = "the Vulkan loader has no vkGetInstanceProcAddr";
            } else {
#define G(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n)
                G(vkCreateInstance); G(vkEnumerateInstanceExtensionProperties); G(vkEnumerateInstanceLayerProperties);
#undef G
                if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties || !vkEnumerateInstanceLayerProperties)
                    why = "the Vulkan loader is missing instance-level entry points";
                else
                    state = 1;
            }
        }
    }
    if (state != 1 && err) *err = why;
    return state == 1;
}

void load_instance(VkInstance instance) {
#define L(n) n = (PFN_##n)vkGetInstanceProcAddr(instance, #n);
    PSP_VK_FUNCS(L)
#undef L
}

void load_device(VkDevice device) {
    if (!vkGetDeviceProcAddr) return;
#define L(n) if (auto p = (PFN_##n)vkGetDeviceProcAddr(device, #n)) n = p;
    PSP_VK_FUNCS(L)
#undef L
}

}  // namespace pspvk
